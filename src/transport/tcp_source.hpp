#pragma once

#include "common.hpp"
#include "object_pool.hpp"
#include <arpa/inet.h>
#include <cstring>
#include <liburing.h>
#include <netinet/in.h>
#include <optional>
#include <source_location>
#include <span>
#include <sys/socket.h>
#include <utility>

class TcpSource {
private:
  struct io_uring_data_t {
    static constexpr uint32_t IO_URING_BUF_MAX_SZ = 4096;

    enum class DataType : uint8_t { SEND, RECV } data_type;
    std::byte buf[IO_URING_BUF_MAX_SZ];
    size_t sz;
    int rc;
  };

  // RAII for restore.
  class PoolEntry {
    ObjectPool<io_uring_data_t> *pool_;
    io_uring_data_t *data_;

  public:
    PoolEntry(ObjectPool<io_uring_data_t> *pool, io_uring_data_t *data) noexcept
        : pool_(pool), data_(data) {}
    ~PoolEntry() {
      if (data_) {
        pool_->restore(data_);
      }
    }

    PoolEntry(const PoolEntry &) = delete;
    PoolEntry &operator=(const PoolEntry &) = delete;
    PoolEntry(PoolEntry &&other) noexcept
        : pool_(other.pool_), data_(std::exchange(other.data_, nullptr)) {}
    PoolEntry &operator=(PoolEntry &&rhs) noexcept {
      std::swap(pool_, rhs.pool_);
      std::swap(data_, rhs.data_);
      return *this;
    }

    io_uring_data_t *get() const noexcept { return data_; }
  };

  FdWrapper sockfd_;
  struct io_uring ring_;
  ObjectPool<io_uring_data_t> pool_;

  std::optional<Status> conn_status_;
  bool recv_armed_{false};

  static constexpr bool is_terminal(Status s) noexcept {
    return s == Status::Eof || s == Status::Error;
  }

public:
  class Buffer {
    PoolEntry entry_;

  public:
    explicit Buffer(PoolEntry &&entry) noexcept : entry_(std::move(entry)) {}

    Buffer(Buffer &&) noexcept = default;
    Buffer &operator=(Buffer &&) noexcept = default;

    std::span<const std::byte> bytes() const noexcept {
      const io_uring_data_t *data = entry_.get();
      return std::span<const std::byte>(data->buf, data->sz);
    }
  };

private:
  void rearm_recv_() {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
    if (sqe == nullptr) [[unlikely]] {
      return;
    }

    io_uring_data_t *data = pool_.get();
    if (!data) [[unlikely]] {
      return;
    }
    data->data_type = io_uring_data_t::DataType::RECV;
    io_uring_prep_recv(sqe, sockfd_.val(), data->buf,
                       io_uring_data_t::IO_URING_BUF_MAX_SZ, 0);
    io_uring_sqe_set_data(sqe, data);
    io_uring_submit(&ring_);
    recv_armed_ = true;
  }

public:
  static constexpr bool messages_may_straddle = true;
  static constexpr uint32_t RING_MAX_ENTRIES = 1024;

  TcpSource(const char *ip, uint16_t port)
      : sockfd_(socket(AF_INET, SOCK_STREAM, 0)) {
    check(sockfd_.val(), "socket()");

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
      std::source_location loc = std::source_location::current();
      throw std::runtime_error(
          std::format("Wrong ip/port: {}/{} passed an argument at {}: {}", ip,
                      port, loc.file_name(), loc.line()));
    }
    check(connect(sockfd_.val(), reinterpret_cast<struct sockaddr *>(&addr),
                  static_cast<socklen_t>(sizeof(addr))),
          "connect()");

    struct io_uring_params params{};
    params.flags = IORING_SETUP_SQPOLL;
    params.sq_thread_idle = 2000;

    check(io_uring_queue_init_params(RING_MAX_ENTRIES, &ring_, &params),
          "io_uring_queue_init_params()");

    rearm_recv_();
  }

  ~TcpSource() { io_uring_queue_exit(&ring_); }

  std::expected<Buffer, Status> next() noexcept {
    if (conn_status_) [[unlikely]] {
      // Terminal status already observed; the socket never recovers.
      return std::unexpected(*conn_status_);
    }

    if (!recv_armed_) [[unlikely]] {
      rearm_recv_();
    }

    struct io_uring_cqe *cqe;
    uint32_t head, count = 0;
    Status err = Status::WouldBlock;
    std::optional<Buffer> received;

    io_uring_for_each_cqe(&ring_, head, cqe) {
      ++count;
      PoolEntry entry(
          &pool_, static_cast<io_uring_data_t *>(io_uring_cqe_get_data(cqe)));
      io_uring_data_t *data = entry.get();

      switch (data->data_type) {
      case io_uring_data_t::DataType::SEND: {
        if (cqe->res < 0) [[unlikely]] {
          err = cqe->res == -EAGAIN ? Status::WouldBlock : Status::Error;
          goto done;
        }
        if (static_cast<size_t>(cqe->res) < data->sz) [[unlikely]] {
          // MSG_WAITALL should prevent this. If it happens, a partial packet is
          // on the wire and the session can no longer be trusted.
          err = Status::Error;
          goto done;
        }
        break;
      }
      case io_uring_data_t::DataType::RECV: {
        recv_armed_ = false;

        if (cqe->res < 0) {
          err = cqe->res == -EAGAIN ? Status::WouldBlock : Status::Error;
          goto done;
        }
        if (cqe->res == 0) [[unlikely]] {
          // Connection closed by the peer.
          err = Status::Eof;
          goto done;
        }

        data->sz = static_cast<size_t>(cqe->res);
        received.emplace(std::move(entry));
        rearm_recv_();
        goto done;
      }
      default:
        err = Status::Error;
        goto done;
      }
    }

  done:
    if (is_terminal(err)) {
      conn_status_ = err;
    }

    io_uring_cq_advance(&ring_, count);

    if (received) {
      return std::move(*received);
    }

    return std::unexpected(err);
  }

  std::expected<void, Status> send(std::span<const std::byte> d) noexcept {
    if (conn_status_) [[unlikely]] {
      // Terminal status already observed; the socket never recovers.
      return std::unexpected(*conn_status_);
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
    if (sqe == nullptr) [[unlikely]] {
      return std::unexpected(Status::WouldBlock);
    }

    io_uring_data_t *data = pool_.get();
    if (data == nullptr) [[unlikely]] {
      return std::unexpected(Status::WouldBlock);
    }
    data->data_type = io_uring_data_t::DataType::SEND;

    if (d.size() > io_uring_data_t::IO_URING_BUF_MAX_SZ) [[unlikely]] {
      pool_.restore(data);
      return std::unexpected(Status::Error);
    }
    std::memcpy(data->buf, d.data(), d.size());
    data->sz = d.size();

    io_uring_prep_send(sqe, sockfd_.val(), data->buf, data->sz,
                       MSG_WAITALL | MSG_NOSIGNAL);
    io_uring_sqe_set_data(sqe, data);
    io_uring_submit(&ring_);

    return {};
  }
};

static_assert(BidirectionalTransport<TcpSource>);