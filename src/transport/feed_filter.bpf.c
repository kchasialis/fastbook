#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>

struct {
  __uint(type, BPF_MAP_TYPE_XSKMAP);
  __uint(max_entries, 64);
  __type(key, __u32);
  __type(value, __u32);
} xsks SEC(".maps");

volatile const __u16 feed_port = 0;

SEC("xdp")
int feed_filter(struct xdp_md *ctx) {
  void *data = (void *)(long)ctx->data;
  void *data_end = (void *)(long)ctx->data_end;

  struct ethhdr *eth = data;
  if ((void *)(eth + 1) > data_end || eth->h_proto != bpf_htons(ETH_P_IP)) {
    return XDP_PASS;
  }

  struct iphdr *ip = (void *)(eth + 1);
  if ((void *)(ip + 1) > data_end || ip->protocol != IPPROTO_UDP ||
      ip->ihl != 5 || (ip->frag_off & bpf_htons(0x3FFF))) {
    return XDP_PASS;
  }

  struct udphdr *udp = (void *)(ip + 1);
  if ((void *)(udp + 1) > data_end || udp->dest != bpf_htons(feed_port)) {
    return XDP_PASS;
  }

  return bpf_redirect_map(&xsks, ctx->rx_queue_index, XDP_PASS);
}

char LICENSE[] SEC("license") = "GPL";
