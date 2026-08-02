#pragma once

#include <arpa/inet.h>
#include <cstddef>
#include <cstring>
#include <format>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

class UDPSource {
private:
  int fd_;

public:
  UDPSource(const char *ip, const char *mcast_ip, uint16_t port) {
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
      throw std::runtime_error(
          std::format("Wrong ip: {} passed as an argument", ip));
    }

    if ((fd_ = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
      throw std::runtime_error(
          std::format("socket() failed for ip: {} and port: {}, error: {}", ip,
                      port, strerror(errno)));
    }

    if (bind(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
      close(fd_);
      throw std::runtime_error(
          std::format("bind() failed for ip: {} and port: {}, error: {}", ip,
                      port, strerror(errno)));
    }

    ip_mreq mreq{};
    if (inet_pton(AF_INET, mcast_ip, &mreq.imr_multiaddr) != 1) {
      close(fd_);
      throw std::runtime_error(std::format(
          "inet_pton() failed for mcast_ip: {} and port: {}, error: {}",
          mcast_ip, port, strerror(errno)));
    }
    mreq.imr_interface = addr.sin_addr;
    auto sz = static_cast<socklen_t>(sizeof(mreq));
    if (setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sz) < 0) {
      close(fd_);
      throw std::runtime_error(
          std::format("setsockopt() failed for ip: {} and port: {}, error: {}",
                      ip, port, strerror(errno)));
    }
  }

  ~UDPSource() { close(fd_); }

  UDPSource() = delete;
  UDPSource(const UDPSource &) = delete;
  UDPSource(UDPSource &&) = delete;
  UDPSource &operator=(const UDPSource &) = delete;
  UDPSource &operator=(UDPSource &&) = delete;

  ssize_t read(void *buf, size_t count) { return ::read(fd_, buf, count); }
};