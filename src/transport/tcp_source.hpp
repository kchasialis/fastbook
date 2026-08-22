#pragma once

#include "fd_wrapper.hpp"
#include "object_pool.hpp"
#include "transport.hpp"
#include "utils.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
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
    std::byte buf[IO_URING_BUF_MAX_SZ];
    size_t sz;

    struct Reset {
      static void operator()(io_uring_data_t &obj) noexcept { obj.sz = 0; }
    };
  };

  using PoolType = ObjectPool<io_uring_data_t, io_uring_data_t::Reset>;

  // RAII for restore.
  class PoolEntry {
    PoolType *pool_;
    io_uring_data_t *data_;

  public:
    PoolEntry(PoolType *pool, io_uring_data_t *data) noexcept
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
  FdWrapper sendfd_;
  struct io_uring ring_;
  PoolType pool_;

  std::atomic<Status> conn_status_;
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
  std::unexpected<Status> fail_(Status err) noexcept {
    shutdown(sockfd_.val(), SHUT_RDWR);
    conn_status_.store(err, std::memory_order_relaxed);
    return std::unexpected(err);
  }

  bool rearm_recv_() noexcept {
    io_uring_data_t *data = pool_.get();
    if (!data) [[unlikely]] {
      return false;
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
    if (sqe == nullptr) [[unlikely]] {
      pool_.restore(data);
      return false;
    }

    io_uring_prep_recv(sqe, sockfd_.val(), data->buf,
                       io_uring_data_t::IO_URING_BUF_MAX_SZ, 0);
    io_uring_sqe_set_data(sqe, data);
    if (io_uring_submit(&ring_) <= 0) [[unlikely]] {
      pool_.restore(data);
      fail_(Status::Error);
      return false;
    }
    recv_armed_ = true;

    return true;
  }

public:
  static constexpr bool messages_may_straddle = true;
  static constexpr uint32_t RING_MAX_ENTRIES = 1024;
  // One recv in flight. 16 should be more than enough
  static constexpr uint32_t POOL_SIZE = 16;

  TcpSource(const char *ip, uint16_t port)
      : sockfd_(socket(AF_INET, SOCK_STREAM, 0)), pool_(POOL_SIZE),
        conn_status_(Status::WouldBlock) {
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

    sendfd_ = check(dup(sockfd_.val()), "dup()");

    struct io_uring_params params{};
    params.flags = IORING_SETUP_SQPOLL;
    params.sq_thread_idle = 2000;

    check(io_uring_queue_init_params(RING_MAX_ENTRIES, &ring_, &params),
          "io_uring_queue_init_params()");

    (void)rearm_recv_();
  }

  ~TcpSource() {
    shutdown(sockfd_.val(), SHUT_RDWR);
    if (recv_armed_) {
      struct io_uring_cqe *cqe;
      io_uring_wait_cqe(&ring_, &cqe);
    }
    io_uring_queue_exit(&ring_);
  }

  std::expected<Buffer, Status> next() noexcept {
    const Status s = conn_status_.load(std::memory_order_relaxed);
    if (is_terminal(s)) [[unlikely]] {
      return std::unexpected(s);
    }

    if (!recv_armed_) [[unlikely]] {
      if (!rearm_recv_()) {
        return std::unexpected(conn_status_.load(std::memory_order_relaxed));
      }
    }

    struct io_uring_cqe *cqe;
    if (io_uring_peek_cqe(&ring_, &cqe) < 0) {
      return std::unexpected(Status::WouldBlock);
    }

    const int res = cqe->res;
    auto *data = static_cast<io_uring_data_t *>(io_uring_cqe_get_data(cqe));
    io_uring_cqe_seen(&ring_, cqe);

    PoolEntry entry(&pool_, data); // Need to wrap here for RAII.
    recv_armed_ = false;

    if (res < 0) [[unlikely]] {
      if (res == -EAGAIN || res == -EINTR) {
        return std::unexpected(Status::WouldBlock);
      }
      return fail_(Status::Error);
    }
    if (res == 0) [[unlikely]] {
      // Connection closed by the peer, or shutdown() was called.
      return fail_(Status::Eof);
    }

    data->sz = static_cast<size_t>(res);
    Buffer buf(std::move(entry));
    rearm_recv_();

    return buf;
  }

  std::expected<void, Status> send(std::span<const std::byte> d) noexcept {
    const Status s = conn_status_.load(std::memory_order_relaxed);
    if (is_terminal(s)) [[unlikely]] {
      // Terminal status already observed; the socket never recovers.
      return std::unexpected(s);
    }

    // Blocking, so it writes the whole payload or fails. MSG_NOSIGNAL keeps a
    // dead peer an EPIPE instead of a SIGPIPE.
    ssize_t ret;
    do {
      ret = ::send(sendfd_.val(), d.data(), d.size(), MSG_NOSIGNAL);
    } while (ret < 0 && errno == EINTR);

    if (ret < 0) [[unlikely]] {
      return fail_(Status::Error);
    }
    if (static_cast<size_t>(ret) != d.size()) [[unlikely]] {
      return fail_(Status::Error);
    }

    return {};
  }
};

static_assert(BidirectionalTransport<TcpSource>);