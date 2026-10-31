#pragma once

#include "feed_filter.skel.h"
#include "logger.hpp"
#include "sys.hpp"
#include "transport.hpp"
#include <arpa/inet.h>
#include <bpf/libbpf.h>
#include <cassert>
#include <cstring>
#include <expected>
#include <format>
#include <linux/if_ether.h>
#include <linux/if_link.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <net/if.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <xdp/libxdp.h>
#include <xdp/xsk.h>

class AfXDPSource {
private:
  static constexpr uint32_t N_FRAMES = 4096;
  static constexpr uint64_t FRAME_SIZE = XSK_UMEM__DEFAULT_FRAME_SIZE;
  static constexpr uint32_t BATCH_SIZE = 32;

  struct xsk_socket *xsk_;
  struct xsk_ring_prod fq_;
  struct xsk_ring_cons rx_;
  uint32_t peeked_rx_;
  uint32_t next_rx_;
  uint32_t batch_rx_;
  void *addr_;
  struct xsk_umem *umem_;
  feed_filter *skel_;
  uint32_t ifindex_;
  uint32_t flags_;

  std::expected<void, TransportStatus> rearm_batch() noexcept {
    if (peeked_rx_) {
      xsk_ring_cons__release(&rx_, peeked_rx_);
    }
    peeked_rx_ = xsk_ring_cons__peek(&rx_, BATCH_SIZE, &next_rx_);
    batch_rx_ = 0;
    if (!peeked_rx_) {
      if (xsk_ring_prod__needs_wakeup(&fq_)) {
        recvfrom(xsk_socket__fd(xsk_), nullptr, 0, MSG_DONTWAIT, nullptr,
                 nullptr);
      }
      return std::unexpected(TransportStatus::WouldBlock);
    }

    return {};
  }

public:
  static constexpr bool messages_may_straddle = false;
  struct Buffer {
    void *base_addr;
    struct xsk_ring_prod *fq;
    xdp_desc desc;

    Buffer(void *base_addr, struct xsk_ring_prod *fq, xdp_desc desc)
        : base_addr(base_addr), fq(fq), desc(desc) {}
    Buffer() : base_addr(nullptr), fq(nullptr), desc() {}
    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;

    Buffer(Buffer &&other) noexcept
        : base_addr(std::exchange(other.base_addr, nullptr)),
          fq(std::exchange(other.fq, nullptr)),
          desc(std::exchange(other.desc, {})) {}
    Buffer &operator=(Buffer &&rhs) noexcept {
      std::swap(base_addr, rhs.base_addr);
      std::swap(fq, rhs.fq);
      std::swap(desc, rhs.desc);

      return *this;
    }

    std::span<const std::byte> bytes() const noexcept {
      auto *data =
          static_cast<std::byte *>(xsk_umem__get_data(base_addr, desc.addr));
      auto *ip = reinterpret_cast<iphdr *>(data + sizeof(ethhdr));
      auto *udp = reinterpret_cast<udphdr *>(reinterpret_cast<std::byte *>(ip) +
                                             static_cast<size_t>(ip->ihl) * 4);

      auto uhdr_sz = sizeof(udphdr);
      auto udp_len = ntohs(udp->len);
      auto *payload = reinterpret_cast<std::byte *>(udp + 1);
      if (udp_len < uhdr_sz || ((payload - data) + (udp_len - 8) > desc.len))
          [[unlikely]] {
        return {};
      }

      auto sz = static_cast<size_t>(udp_len - uhdr_sz);
      return {payload, sz};
    }

    ~Buffer() {
      if (base_addr != nullptr) {
        uint32_t idx;
        auto ret = xsk_ring_prod__reserve(fq, 1, &idx);
        assert(ret == 1);
        *xsk_ring_prod__fill_addr(fq, idx) = xsk_umem__extract_addr(desc.addr);
        xsk_ring_prod__submit(fq, 1);
      }
    }
  };

  AfXDPSource(const char *ifname, int queue, uint16_t port)
      : peeked_rx_(0), next_rx_(0), batch_rx_(0), addr_(nullptr) {
    int prot = PROT_READ | PROT_WRITE;
    int flags = MAP_ANONYMOUS | MAP_PRIVATE | MAP_HUGETLB | MAP_HUGE_2MB;
    size_t mem_size = N_FRAMES * FRAME_SIZE;
    addr_ = mmap(nullptr, mem_size, prot, flags, -1, 0);
    if (addr_ == MAP_FAILED) {
      flags = MAP_ANONYMOUS | MAP_PRIVATE;
      addr_ = mmap(nullptr, mem_size, prot, flags, -1, 0);
      if (addr_ == MAP_FAILED) {
        throw std::runtime_error(
            std::format("mmap() failed: {}", strerror(errno)));
      }
    }

    ifindex_ = if_nametoindex(ifname);
    if (!ifindex_) {
      throw std::runtime_error(
          std::format("if_nametoindex() failed: {}", strerror(errno)));
    }

    struct xsk_umem_config ucfg = {
        .fill_size = N_FRAMES,
        .comp_size = XSK_RING_CONS__DEFAULT_NUM_DESCS,
        .frame_size = FRAME_SIZE,
        .frame_headroom = 0,
        .flags = 0,
    };

    struct xsk_ring_cons cq;
    if (xsk_umem__create(&umem_, addr_, mem_size, &fq_, &cq, &ucfg)) {
      throw std::runtime_error(
          std::format("xsk_umem__create() failed: {}", strerror(errno)));
    }

    struct xsk_socket_config scfg = {
        .rx_size = XSK_RING_CONS__DEFAULT_NUM_DESCS,
        .libxdp_flags = XSK_LIBXDP_FLAGS__INHIBIT_PROG_LOAD,
        .bind_flags = XDP_USE_NEED_WAKEUP,
    };
    if (xsk_socket__create(&xsk_, ifname, queue, umem_, &rx_, nullptr, &scfg)) {
      throw std::runtime_error(
          std::format("xsk_socket__create() failed: {}", strerror(errno)));
    }

    uint32_t idx;
    if (xsk_ring_prod__reserve(&fq_, N_FRAMES, &idx) != N_FRAMES) {
      throw std::runtime_error(
          std::format("xsk_ring_prod__reserve() failed: {}", strerror(errno)));
    }

    for (uint32_t i = 0; i < N_FRAMES; i++) {
      *xsk_ring_prod__fill_addr(&fq_, idx + i) = i * FRAME_SIZE;
    }
    xsk_ring_prod__submit(&fq_, N_FRAMES);

    skel_ = feed_filter__open();
    if (!skel_) {
      throw std::runtime_error(std::format("feed_filter__open()"));
    }

    skel_->rodata->feed_port = port;
    check_neg(feed_filter__load(skel_), "feed_filter_load");

    check_neg(xsk_socket__update_xskmap(xsk_, bpf_map__fd(skel_->maps.xsks)),
              "xsk_socket__update_xskmap");
    flags_ = XDP_FLAGS_DRV_MODE;
    if (bpf_xdp_attach(ifindex_, bpf_program__fd(skel_->progs.feed_filter),
                       flags_, nullptr) != 0) {
      flags_ = XDP_FLAGS_SKB_MODE;
      check_neg(bpf_xdp_attach(ifindex_,
                               bpf_program__fd(skel_->progs.feed_filter),
                               flags_, nullptr),
                "bpf_xdp_attach");
      LOG_INFO("AF_XDP MODE: XDP_FLAGS_SKB_MODE");
    } else {
      LOG_INFO("AF_XDP MODE: XDP_FLAGS_DRV_MODE");
    }
  }

  ~AfXDPSource() {
    bpf_xdp_detach(ifindex_, flags_, nullptr);
    feed_filter__destroy(skel_);
    xsk_socket__delete(xsk_);
    xsk_umem__delete(umem_);
    munmap(addr_, N_FRAMES * FRAME_SIZE);
  }

  AfXDPSource(const AfXDPSource &) = delete;
  AfXDPSource(AfXDPSource &&) = delete;

  AfXDPSource &operator=(const AfXDPSource &) = delete;
  AfXDPSource &operator=(AfXDPSource &&) = delete;

  std::expected<Buffer, TransportStatus> next() noexcept {
    if (batch_rx_ == peeked_rx_) {
      auto val = rearm_batch();
      if (!val.has_value()) {
        return std::unexpected(val.error());
      }
    }

    ++batch_rx_;
    return Buffer{addr_, &fq_, *xsk_ring_cons__rx_desc(&rx_, next_rx_++)};
  }
};

static_assert(Transport<AfXDPSource>);
