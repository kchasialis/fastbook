// =============================================================================
// AI-GENERATED CODE - written by Claude (Anthropic), not by the project author.
//
// This file is a test tool, not part of the fastbook system. It was generated
// so that the MoldUDP64 gap detection and retransmission recovery in fastbook
// can be exercised end to end without a live exchange. It has been run and
// checked against the file replay, but it has not been reviewed line by line
// to the standard of the rest of the code base.
// =============================================================================
//
// mold_replay: plays the role of a NASDAQ-style feed on the local machine.
//
// - Reads an ITCH capture (2-byte big-endian length + message, back to back),
//   packs the messages into MoldUDP64 datagrams and multicasts them. The
//   capture's message blocks already have MoldUDP64's per-message layout, so a
//   datagram is a 20-byte header followed by a slice of the file, sent with
//   sendmsg() straight from the mapping.
// - Can drop datagrams on purpose (-d / -b) to create sequence gaps.
// - Answers MoldUDP64 re-requests on a unicast UDP port (-r), including for
//   the datagrams it dropped, so the receiver can fill its gaps.
// - Sends an end-of-session datagram once a second for -l seconds at the end,
//   while still answering re-requests.
//
// -g takes a multicast group or a unicast address. For a multicast group the
// TTL is 0, so nothing leaves the host; the kernel loops each datagram back to
// local sockets that joined the group. A unicast address is sent to as is,
// e.g. across a veth pair for AF_XDP or to another machine.

#include "buf_reader.hpp"
#include "buf_writer.hpp"
#include "mold_udp64.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <getopt.h>
#include <limits>
#include <netinet/in.h>
#include <optional>
#include <poll.h>
#include <print>
#include <span>
#include <string_view>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

// MoldUDP64 session id: exactly mold::SESSION_SIZE (10) ASCII characters.
constexpr std::string_view SESSION = "FASTBOOK01";
// Datagrams stay under a typical 1500-byte Ethernet MTU, as real feeds do.
constexpr size_t MAX_DATAGRAM = 1400;

struct Options {
  const char *file = nullptr;
  const char *group = "239.1.1.1";
  uint16_t port = 5000;
  const char *iface = "0.0.0.0";
  uint16_t rerequest_port = 6000;
  uint64_t drop_every = 0;
  uint64_t drop_burst = 1;
  uint64_t rate = 20'000;
  uint64_t linger_s = 5;
};

// One entry per datagram sent (or dropped): its first sequence number and
// where its first message starts in the capture. Re-requests binary-search
// this to find where a requested sequence number lives in the file.
struct IndexEntry {
  uint64_t first_seq;
  size_t offset;
};

// A run of whole messages [begin, end) in the capture that fits one datagram.
struct Batch {
  size_t begin;
  size_t end;
  uint16_t count;
};

struct Stats {
  uint64_t datagrams = 0;
  uint64_t dropped = 0;
  uint64_t messages = 0;
  uint64_t requests = 0;
  uint64_t resent = 0;
};

// Read-only mmap of the capture. Empty bytes() means the file could not be
// opened or mapped.
class MappedFile {
  std::span<const std::byte> bytes_;

public:
  explicit MappedFile(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
      return;
    }
    struct stat st{};
    if (fstat(fd, &st) == 0 && st.st_size > 0) {
      auto size = static_cast<size_t>(st.st_size);
      void *p = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
      if (p != MAP_FAILED) {
        madvise(p, size, MADV_SEQUENTIAL);
        bytes_ = {static_cast<const std::byte *>(p), size};
      }
    }
    close(fd);
  }

  ~MappedFile() {
    if (!bytes_.empty()) {
      munmap(const_cast<std::byte *>(bytes_.data()), bytes_.size());
    }
  }

  MappedFile(const MappedFile &) = delete;
  MappedFile &operator=(const MappedFile &) = delete;

  [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
    return bytes_;
  }
};

// Size of the message block at pos (length prefix included), or nullopt if
// the block runs past the end of the capture (a cut-off tail).
std::optional<size_t> message_size(std::span<const std::byte> data,
                                   size_t pos) {
  if (pos + 2 > data.size()) {
    return std::nullopt;
  }
  auto len = static_cast<size_t>((std::to_integer<unsigned>(data[pos]) << 8) |
                                 std::to_integer<unsigned>(data[pos + 1]));
  if (pos + 2 + len > data.size()) {
    return std::nullopt;
  }
  return 2 + len;
}

// Packs as many whole messages as fit in one datagram, starting at pos, up to
// max_count. Always takes at least one message, even an oversized one. The
// count stays below 0xFFFF, which MoldUDP64 reserves for end of session.
Batch pack(std::span<const std::byte> data, size_t pos, uint64_t max_count) {
  Batch b{pos, pos, 0};
  size_t bytes = mold::HEADER_SIZE;
  while (b.count < max_count && b.count < mold::END_OF_SESSION - 1) {
    auto sz = message_size(data, b.end);
    if (!sz) {
      break;
    }
    if (b.count > 0 && bytes + *sz > MAX_DATAGRAM) {
      break;
    }
    bytes += *sz;
    b.end += *sz;
    b.count++;
  }
  return b;
}

// Sends header + payload as one datagram without copying the payload: the
// header is built on the stack and the payload comes from the mapping.
bool send_datagram(int fd, const sockaddr_in &dst, uint64_t seq, uint16_t count,
                   std::span<const std::byte> payload) {
  std::array<std::byte, mold::HEADER_SIZE> hdr;
  BufWriter writer(hdr);
  mold::write_header(writer,
                     {.session = SESSION, .seq_num = seq, .msg_count = count});

  std::array<iovec, 2> iov{
      iovec{hdr.data(), hdr.size()},
      iovec{const_cast<std::byte *>(payload.data()), payload.size()}};
  msghdr msg{};
  msg.msg_name = const_cast<sockaddr_in *>(&dst);
  msg.msg_namelen = sizeof(dst);
  msg.msg_iov = iov.data();
  msg.msg_iovlen = iov.size();
  return sendmsg(fd, &msg, 0) >= 0;
}

// Answers every re-request waiting on the unicast socket, without blocking.
// A request is a MoldUDP64 header: session, first sequence wanted, count.
// Requests for another session, sequence 0, or messages not sent yet are
// ignored. Replies are ordinary MoldUDP64 datagrams sent back to the
// requester's address and port.
void serve_requests(int fd, std::span<const std::byte> data,
                    const std::vector<IndexEntry> &index, uint64_t next_seq,
                    Stats &stats) {
  for (;;) {
    std::array<std::byte, 64> req;
    sockaddr_in from{};
    socklen_t fromlen = sizeof(from);
    ssize_t n = recvfrom(fd, req.data(), req.size(), MSG_DONTWAIT,
                         reinterpret_cast<sockaddr *>(&from), &fromlen);
    if (n < 0) {
      return;
    }

    BufReader reader(
        std::span<const std::byte>(req.data(), static_cast<size_t>(n)));
    auto hdr = mold::read_header(reader);
    if (!hdr || hdr->session != SESSION || hdr->seq_num == 0 ||
        hdr->seq_num >= next_seq) {
      continue;
    }
    stats.requests++;

    // Clamp the count to what has actually been sent so far.
    uint64_t seq = hdr->seq_num;
    uint64_t remaining = std::min<uint64_t>(hdr->msg_count, next_seq - seq);
    // The datagram containing seq is the last one whose first_seq <= seq.
    // index[0].first_seq is 1 and seq >= 1, so --it never leaves the vector.
    auto it = std::upper_bound(
        index.begin(), index.end(), seq,
        [](uint64_t s, const IndexEntry &e) { return s < e.first_seq; });
    --it;

    // Walk forward inside that datagram to the requested message.
    size_t pos = it->offset;
    for (uint64_t s = it->first_seq; s < seq; s++) {
      pos += *message_size(data, pos);
    }

    // Repack from there: replies don't have to match the original datagrams.
    while (remaining > 0) {
      Batch b = pack(data, pos, remaining);
      if (b.count == 0) {
        break;
      }
      send_datagram(fd, from, seq, b.count,
                    data.subspan(b.begin, b.end - b.begin));
      stats.resent += b.count;
      seq += b.count;
      remaining -= b.count;
      pos = b.end;
    }
  }
}

// Parses the whole string as a number; trailing characters are an error.
template <class T> bool parse_num(const char *s, T &out) {
  std::string_view sv(s);
  auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), out);
  return ec == std::errc{} && ptr == sv.data() + sv.size();
}

void usage(const char *prog) {
  std::println(stderr,
               "Usage: {} -f <capture> [-g <group-or-unicast-ip>] [-p <port>] [-i <iface-ip>]"
               " [-r <rerequest-port>] [-d <drop-every>] [-b <drop-burst>]"
               " [-R <datagrams/s>] [-l <linger-seconds>]",
               prog);
}

bool parse_args(int argc, char *argv[], Options &opt) {
  static option long_opts[] = {
      {"file", required_argument, nullptr, 'f'},
      {"group", required_argument, nullptr, 'g'},
      {"dest", required_argument, nullptr, 'g'},
      {"port", required_argument, nullptr, 'p'},
      {"iface", required_argument, nullptr, 'i'},
      {"rerequest-port", required_argument, nullptr, 'r'},
      {"drop-every", required_argument, nullptr, 'd'},
      {"drop-burst", required_argument, nullptr, 'b'},
      {"rate", required_argument, nullptr, 'R'},
      {"linger", required_argument, nullptr, 'l'},
      {nullptr, 0, nullptr, 0}};
  int c;
  while ((c = getopt_long(argc, argv, "f:g:p:i:r:d:b:R:l:", long_opts,
                          nullptr)) != -1) {
    bool ok = true;
    switch (c) {
    case 'f':
      opt.file = optarg;
      break;
    case 'g':
      opt.group = optarg;
      break;
    case 'p':
      ok = parse_num(optarg, opt.port);
      break;
    case 'i':
      opt.iface = optarg;
      break;
    case 'r':
      ok = parse_num(optarg, opt.rerequest_port);
      break;
    case 'd':
      ok = parse_num(optarg, opt.drop_every);
      break;
    case 'b':
      ok = parse_num(optarg, opt.drop_burst);
      break;
    case 'R':
      ok = parse_num(optarg, opt.rate);
      break;
    case 'l':
      ok = parse_num(optarg, opt.linger_s);
      break;
    default:
      return false;
    }
    if (!ok) {
      std::println(stderr, "Invalid value '{}' for -{}", optarg,
                   static_cast<char>(c));
      return false;
    }
  }
  return opt.file != nullptr;
}

} // namespace

int main(int argc, char *argv[]) {
  Options opt;
  if (!parse_args(argc, argv, opt)) {
    usage(argv[0]);
    return 1;
  }

  MappedFile capture(opt.file);
  std::span<const std::byte> data = capture.bytes();
  if (data.empty()) {
    std::println(stderr, "Cannot map '{}': {}", opt.file, std::strerror(errno));
    return 1;
  }

  int mcast_fd = socket(AF_INET, SOCK_DGRAM, 0);
  int rr_fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (mcast_fd < 0 || rr_fd < 0) {
    std::println(stderr, "socket(): {}", std::strerror(errno));
    return 1;
  }

  sockaddr_in group{};
  group.sin_family = AF_INET;
  group.sin_port = htons(opt.port);
  if (inet_pton(AF_INET, opt.group, &group.sin_addr) != 1) {
    std::println(stderr, "Invalid destination '{}'", opt.group);
    return 1;
  }

  in_addr iface{};
  if (inet_pton(AF_INET, opt.iface, &iface) != 1) {
    std::println(stderr, "Invalid interface ip '{}'", opt.iface);
    return 1;
  }

  // The multicast options only apply to a group address. A unicast
  // destination is routed like any other packet.
  if (IN_MULTICAST(ntohl(group.sin_addr.s_addr))) {
    // TTL 0 keeps the datagrams on this host; loopback delivers them to local
    // receivers that joined the group.
    unsigned char ttl = 0;
    unsigned char loop = 1;
    setsockopt(mcast_fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    setsockopt(mcast_fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    // Optional outgoing interface. With 0.0.0.0 the routing table picks it.
    if (iface.s_addr != htonl(INADDR_ANY) &&
        setsockopt(mcast_fd, IPPROTO_IP, IP_MULTICAST_IF, &iface,
                   sizeof(iface)) != 0) {
      std::println(stderr, "IP_MULTICAST_IF: {}", std::strerror(errno));
      return 1;
    }
  }

  // The re-request server: a plain UDP socket on a fixed port. The receiver
  // connect()s to it, so replies must come from this same socket and port.
  int one = 1;
  setsockopt(rr_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in rr_addr{};
  rr_addr.sin_family = AF_INET;
  rr_addr.sin_port = htons(opt.rerequest_port);
  rr_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(rr_fd, reinterpret_cast<sockaddr *>(&rr_addr), sizeof(rr_addr)) !=
      0) {
    std::println(stderr, "bind(re-request port {}): {}", opt.rerequest_port,
                 std::strerror(errno));
    return 1;
  }

  using clock = std::chrono::steady_clock;
  Stats stats;
  std::vector<IndexEntry> index;
  uint64_t seq = 1;
  size_t pos = 0;
  auto start = clock::now();

  // Main loop: one datagram per iteration. Dropped datagrams still get an
  // index entry and still use up their sequence numbers, exactly like a
  // datagram lost on the network, so they can be re-requested.
  for (;;) {
    Batch b = pack(data, pos, std::numeric_limits<uint64_t>::max());
    if (b.count == 0) {
      break;
    }
    index.push_back({seq, b.begin});
    stats.datagrams++;

    // Drops drop_burst datagrams in a row every drop_every datagrams.
    bool drop =
        opt.drop_every > 0 && stats.datagrams % opt.drop_every < opt.drop_burst;
    if (drop) {
      stats.dropped++;
    } else {
      send_datagram(mcast_fd, group, seq, b.count,
                    data.subspan(b.begin, b.end - b.begin));
    }
    seq += b.count;
    stats.messages += b.count;
    pos = b.end;

    // Re-requests are answered between datagrams, so the receiver doesn't
    // wait for the whole replay to finish before its gaps are filled.
    serve_requests(rr_fd, data, index, seq, stats);

    // Pacing against the start time, not per datagram, so oversleeping once
    // is made up for by not sleeping on the next few datagrams.
    if (opt.rate > 0) {
      auto due = start + std::chrono::nanoseconds(stats.datagrams *
                                                  1'000'000'000 / opt.rate);
      if (clock::now() < due) {
        std::this_thread::sleep_until(due);
      }
    }
  }

  // End of session. Its sequence number is the next one that would have been
  // sent, so a receiver missing the tail sees a gap first and recovers it.
  // Repeated once a second in case one is lost, while re-requests are still
  // served.
  auto linger_end = clock::now() + std::chrono::seconds(opt.linger_s);
  auto next_eos = clock::now();
  while (clock::now() < linger_end) {
    if (clock::now() >= next_eos) {
      send_datagram(mcast_fd, group, seq, mold::END_OF_SESSION, {});
      next_eos += std::chrono::seconds(1);
    }
    pollfd pfd{rr_fd, POLLIN, 0};
    poll(&pfd, 1, 100);
    serve_requests(rr_fd, data, index, seq, stats);
  }

  std::println("datagrams={} dropped={} messages={} requests={} resent={}",
               stats.datagrams, stats.dropped, stats.messages, stats.requests,
               stats.resent);
  close(mcast_fd);
  close(rr_fd);
  return 0;
}
