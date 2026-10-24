#pragma once

#include "logger.hpp"
#include "sys.hpp"
#include "transport.hpp"
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <span>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>

class UnicastUDPSource {
private:
  static constexpr uint32_t MAX_DATAGRAM = 1 << 16;

  FdWrapper sockfd_;
  std::array<std::byte, MAX_DATAGRAM> buf_;

public:
  UnicastUDPSource(const char *ip, uint16_t port,
                   std::chrono::microseconds recv_timeout)
      : sockfd_(socket(AF_INET, SOCK_DGRAM, 0)) {

    check(*sockfd_, "UnicastUDPSource::socket()");

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
      throw std::runtime_error(
          std::format("Wrong ip: {} passed as an argument", ip));
    }

    // connect() on UDP sockets filters packets that do not come from `addr`
    check(connect(*sockfd_, reinterpret_cast<sockaddr *>(&addr),
                  static_cast<socklen_t>(sizeof(addr))),
          "UnicastUDPSource::connect()");

    auto secs = std::chrono::duration_cast<std::chrono::seconds>(recv_timeout);
    struct timeval tv = {
        .tv_sec = static_cast<time_t>(secs.count()),
        .tv_usec = static_cast<suseconds_t>((recv_timeout - secs).count())};
    check(setsockopt(*sockfd_, SOL_SOCKET, SO_RCVTIMEO, &tv,
                     static_cast<socklen_t>(sizeof(tv))),
          "UnicastUDPSource::setsockopt()");
  }

  static constexpr bool messages_may_straddle = false;

  struct Buffer {
    std::span<const std::byte> bytes_;

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
      return bytes_;
    }
  };

  std::expected<Buffer, TransportStatus> next() noexcept {
    auto nrecv = recv(*sockfd_, buf_.data(), MAX_DATAGRAM, MSG_TRUNC);
    if (nrecv > static_cast<ssize_t>(buf_.size())) [[unlikely]] {
      LOG_ERROR("datagram of {} bytes cut to {}", nrecv, buf_.size());
      return std::unexpected(TransportStatus::Error);
    }

    if (nrecv <= 0) {
      if (nrecv == 0 || errno == EAGAIN || errno == EWOULDBLOCK ||
          errno == EINTR) {
        return std::unexpected(TransportStatus::WouldBlock);
      }
      LOG_ERROR("recv failed: {}", std::strerror(errno));
      return std::unexpected(TransportStatus::Error);
    }

    return Buffer{{buf_.data(), static_cast<size_t>(nrecv)}};
  }

  std::expected<void, TransportStatus>
  send(std::span<const std::byte> d) noexcept {
    auto nsent = ::send(*sockfd_, d.data(), d.size(), MSG_DONTWAIT);
    if (nsent <= 0) {
      if (nsent == 0 || errno == EAGAIN || errno == EWOULDBLOCK) {
        return std::unexpected(TransportStatus::WouldBlock);
      }
      LOG_ERROR("send failed: {}", std::strerror(errno));
      return std::unexpected(TransportStatus::Error);
    }

    return {};
  }
};

static_assert(BidirectionalTransport<UnicastUDPSource>);