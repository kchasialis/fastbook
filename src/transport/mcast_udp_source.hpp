#pragma once

#include <arpa/inet.h>
#include <cstddef>
#include <cstring>
#include <expected>
#include <format>
#include <iostream>
#include <netinet/in.h>
#include <optional>
#include <source_location>
#include <span>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

#include "fd_wrapper.hpp"
#include "object_pool.hpp"
#include "transport.hpp"
#include "utils.hpp"

class McastUDPSource {
public:
  static constexpr uint32_t MSG_BUF_SZ = 2048;
  static constexpr uint32_t VLEN = 128;
  static constexpr uint32_t MB = 1024 * 1024;
  static constexpr bool messages_may_straddle = false;

private:
  struct Datagram {
    std::byte buf[MSG_BUF_SZ];
    size_t sz;

    struct Reset {
      static void operator()(Datagram &d) noexcept { d.sz = 0; }
    };
  };

  using PoolType = ObjectPool<Datagram, Datagram::Reset>;

public:
  class Buffer {
    PoolType *pool_;
    Datagram *dgram_;

  public:
    Buffer(PoolType *pool, Datagram *dgram) noexcept
        : pool_(pool), dgram_(dgram) {}
    ~Buffer() {
      if (dgram_) {
        pool_->restore(dgram_);
      }
    }

    Buffer(const Buffer &) = delete;
    Buffer(Buffer &&other) noexcept
        : pool_(other.pool_), dgram_(std::exchange(other.dgram_, nullptr)) {}
    Buffer &operator=(const Buffer &) = delete;
    Buffer &operator=(Buffer &&rhs) noexcept {
      std::swap(pool_, rhs.pool_);
      std::swap(dgram_, rhs.dgram_);
      return *this;
    }

    std::span<const std::byte> bytes() const noexcept {
      return std::span<const std::byte>(dgram_->buf, dgram_->sz);
    }
  };

private:
  FdWrapper sockfd_;
  size_t msgs_received_;
  size_t next_msg_;
  struct iovec iovecs_[VLEN];
  struct mmsghdr msgs_[VLEN]{};
  Datagram *batch_[VLEN];
  PoolType pool_;

  std::optional<uint32_t> refill_() noexcept {
    uint32_t k = 0;
    while (k < VLEN) {
      Datagram *d = pool_.get();
      if (!d) {
        break;
      }
      batch_[k] = d;
      iovecs_[k].iov_base = d->buf;
      iovecs_[k].iov_len = MSG_BUF_SZ;
      msgs_[k].msg_hdr.msg_iov = &iovecs_[k];
      msgs_[k].msg_hdr.msg_iovlen = 1;
      ++k;
    }

    if (k == 0) [[unlikely]] {
      return std::nullopt;
    }
    return k;
  }

  void release_range_(uint32_t from, uint32_t to) noexcept {
    for (uint32_t j = from; j < to; ++j) {
      pool_.restore(batch_[j]);
    }
  }

  Buffer serve_() noexcept {
    Datagram *d = batch_[next_msg_];
    d->sz = msgs_[next_msg_].msg_len;
    ++next_msg_;
    return Buffer{&pool_, d};
  }

public:
  static constexpr uint32_t POOL_SIZE = 4 * VLEN;

  McastUDPSource(const char *ip, const char *mcast_ip, uint16_t port)
      : sockfd_(socket(AF_INET, SOCK_DGRAM, 0)), msgs_received_(0),
        next_msg_(0), pool_(POOL_SIZE) {

    check(sockfd_.val(), "socket()");

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    uint32_t optval = 1;
    check(setsockopt(sockfd_.val(), SOL_SOCKET, SO_REUSEADDR, &optval,
                     sizeof(optval)),
          "setsockopt()");
    check(setsockopt(sockfd_.val(), SOL_SOCKET, SO_REUSEPORT, &optval,
                     sizeof(optval)),
          "setsockopt()");
    check(setsockopt(sockfd_.val(), SOL_SOCKET, SO_RXQ_OVFL, &optval,
                     sizeof(optval)),
          "setsockopt()");

    optval = 16 * MB;
    check(setsockopt(sockfd_.val(), SOL_SOCKET, SO_RCVBUF, &optval,
                     sizeof(optval)),
          "setsockopt()");

    socklen_t len = sizeof(optval);
    check(getsockopt(sockfd_.val(), SOL_SOCKET, SO_RCVBUF, &optval, &len),
          "getsockopt()");
    if (optval < 4 * MB) {
      std::cerr << "[DEBUG]: RCVBUF value is less than requested: " << optval
                << std::endl;
    }

    check(
        bind(sockfd_.val(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)),
        "bind()");

    ip_mreq mreq{};
    if (inet_pton(AF_INET, mcast_ip, &mreq.imr_multiaddr) != 1) {
      throw std::runtime_error(
          std::format("Wrong multicast ip: {} passed as an argument", ip));
    }

    if (inet_pton(AF_INET, ip, &mreq.imr_interface) != 1) {
      throw std::runtime_error(
          std::format("Wrong ip: {} passed as an argument", ip));
    }

    auto sz = static_cast<socklen_t>(sizeof(mreq));
    check(setsockopt(sockfd_.val(), IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sz),
          "setsockopt()");
  }

  ~McastUDPSource() = default;

  McastUDPSource() = delete;
  McastUDPSource(const McastUDPSource &) = delete;
  McastUDPSource(McastUDPSource &&other) = delete;
  McastUDPSource &operator=(const McastUDPSource &) = delete;
  McastUDPSource &operator=(McastUDPSource &&rhs) = delete;

  std::expected<McastUDPSource::Buffer, Status> next() noexcept {
    if (next_msg_ < msgs_received_) {
      return serve_();
    }

    auto claimed = refill_();
    if (!claimed.has_value()) [[unlikely]] {
      return std::unexpected(Status::WouldBlock);
    }
    const uint32_t k = claimed.value();

    int nrecv = recvmmsg(sockfd_.val(), msgs_, k, MSG_DONTWAIT, NULL);
    if (nrecv <= 0) [[unlikely]] {
      release_range_(0, k);
      if (nrecv == 0 || errno == EAGAIN || errno == EWOULDBLOCK ||
          errno == EINTR) {
        return std::unexpected(Status::WouldBlock);
      }
      std::source_location loc = std::source_location::current();
      std::cerr << "[DEBUG] "
                << std::format("recvmmsg failed at {}: {}", loc.file_name(),
                               loc.line());
      return std::unexpected(Status::Error);
    }

    release_range_(static_cast<uint32_t>(nrecv), k);

    msgs_received_ = static_cast<size_t>(nrecv);
    next_msg_ = 0;

    return serve_();
  }
};

static_assert(Transport<McastUDPSource>);
