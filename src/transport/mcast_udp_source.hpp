#pragma once

#include <arpa/inet.h>
#include <cstddef>
#include <cstring>
#include <expected>
#include <format>
#include <iostream>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "common.hpp"

class McastUDPSource {
public:
  static constexpr uint32_t MSG_BUF_SZ = 2048;
  static constexpr uint32_t VLEN = 128;
  static constexpr uint32_t MB = 1024 * 1024;
  static constexpr bool messages_may_straddle = false;

private:
  FdWrapper sockfd_;
  size_t msgs_received_;
  size_t next_msg_;
  std::byte bufs_[VLEN][MSG_BUF_SZ];
  struct iovec iovecs_[VLEN];
  struct mmsghdr msgs_[VLEN];

public:
  class Buffer {
    std::span<const std::byte> bytes_;

  public:
    explicit Buffer(std::span<const std::byte> bytes) : bytes_(bytes) {}
    std::span<const std::byte> bytes() const noexcept { return bytes_; }
  };

  McastUDPSource(const char *ip, const char *mcast_ip, uint16_t port)
      : sockfd_(socket(AF_INET, SOCK_DGRAM, 0)), msgs_received_(0),
        next_msg_(0) {

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

    memset(msgs_, 0, sizeof(msgs_));
    for (uint32_t i = 0; i < VLEN; i++) {
      iovecs_[i].iov_base = bufs_[i];
      iovecs_[i].iov_len = MSG_BUF_SZ;
      msgs_[i].msg_hdr.msg_iov = &iovecs_[i];
      msgs_[i].msg_hdr.msg_iovlen = 1;
    }
  }

  ~McastUDPSource() = default;

  McastUDPSource() = delete;
  McastUDPSource(const McastUDPSource &) = delete;
  McastUDPSource(McastUDPSource &&other) = delete;
  McastUDPSource &operator=(const McastUDPSource &) = delete;
  McastUDPSource &operator=(McastUDPSource &&rhs) = delete;

  std::expected<McastUDPSource::Buffer, Status> next() noexcept {
    if (next_msg_ < msgs_received_) {
      std::span<const std::byte> s(bufs_[next_msg_], msgs_[next_msg_].msg_len);
      next_msg_++;
      return Buffer{s};
    }

    int nrecv = recvmmsg(sockfd_.val(), msgs_, VLEN, MSG_DONTWAIT, NULL);
    if (nrecv == 0 ||
        (nrecv == -1 &&
         (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))) {
      return std::unexpected(Status::WouldBlock);
    } else if (nrecv == -1) {
      std::source_location loc = std::source_location::current();
      std::cerr << "[DEBUG] "
                << std::format("recvmmsg failed at {}: {}", loc.file_name(),
                               loc.line());
      return std::unexpected(Status::Error);
    }

    msgs_received_ = nrecv;
    next_msg_ = 0;

    std::span<const std::byte> s(bufs_[next_msg_], msgs_[next_msg_].msg_len);
    next_msg_++;
    return Buffer{s};
  }
};

static_assert(Transport<McastUDPSource>);
