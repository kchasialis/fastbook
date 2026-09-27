#include "feed_handler.hpp"
#include "file_source.hpp"
#include "logger.hpp"
#include "mcast_udp_source.hpp"
#include "types.hpp"

#include <charconv>
#include <csignal>
#include <cstdint>
#include <exception>
#include <getopt.h>
#include <memory>
#include <optional>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <variant>

struct Args {
  fs::path config_fpath;
  std::optional<Exchange> exchange;
  const char *exchange_name{nullptr};
  // Exactly one source: a capture file to replay, or a multicast group.
  const char *file{nullptr};
  const char *mcast_group{nullptr};
  const char *iface{"0.0.0.0"};
  uint16_t port{0};
};

struct Stats {
  uint64_t msg_count;
};

std::string usage(const char *prog) {
  std::ostringstream ss;

  ss << "Usage:\n"
     << "  " << prog << " -e <exchange> -c <thread-config> -f <capture-file>\n"
     << "  " << prog
     << " -e <exchange> -c <thread-config> -g <mcast-group> -p <port> "
        "[-i <iface-ip>]\n"
     << "\n"
     << "  -e, --exchange  nasdaq | cme | cboe | nyse | eurex\n"
     << "  -c, --config    thread config (feed and builder cores)\n"
     << "  -f, --file      replay a capture file\n"
     << "  -g, --group     multicast group to join\n"
     << "  -p, --port      multicast port\n"
     << "  -i, --iface     local interface ip for the group (default "
        "0.0.0.0)\n"
     << "  -h, --help      print this message\n";

  return ss.str();
}

bool parse_port(std::string_view s, uint16_t &out) {
  const char *end = s.data() + s.size();
  auto [ptr, ec] = std::from_chars(s.data(), end, out);
  return ec == std::errc{} && ptr == end && out != 0;
}

bool parse_args(int argc, char *argv[], Args &args) {
  static option long_opts[] = {{"exchange", required_argument, nullptr, 'e'},
                               {"config", required_argument, nullptr, 'c'},
                               {"file", required_argument, nullptr, 'f'},
                               {"group", required_argument, nullptr, 'g'},
                               {"port", required_argument, nullptr, 'p'},
                               {"iface", required_argument, nullptr, 'i'},
                               {"help", no_argument, nullptr, 'h'},
                               {nullptr, 0, nullptr, 0}};

  int opt;
  while ((opt = getopt_long(argc, argv, ":e:c:f:g:p:i:h", long_opts,
                            nullptr)) != -1) {
    switch (opt) {
    case 'e':
      args.exchange = str_to_exchange(optarg);
      args.exchange_name = optarg;
      if (!args.exchange.has_value()) {
        LOG_ERROR("Unknown exchange '{}'", optarg);
        return false;
      }
      break;
    case 'c':
      args.config_fpath = fs::path(optarg);
      break;
    case 'f':
      args.file = optarg;
      break;
    case 'g':
      args.mcast_group = optarg;
      break;
    case 'p':
      if (!parse_port(optarg, args.port)) {
        LOG_ERROR("Invalid port '{}'", optarg);
        return false;
      }
      break;
    case 'i':
      args.iface = optarg;
      break;
    case 'h':
      return false;
    case ':':
      LOG_ERROR("Option '{}' requires an argument", argv[optind - 1]);
      return false;
    default:
      LOG_ERROR("Unknown option '{}'", argv[optind - 1]);
      return false;
    }
  }

  if (optind < argc) {
    LOG_ERROR("Unexpected argument '{}'", argv[optind]);
    return false;
  }

  if (!args.exchange.has_value() || args.config_fpath.empty()) {
    LOG_ERROR("--exchange and --config are required");
    return false;
  }

  bool live = args.mcast_group != nullptr || args.port != 0;
  if ((args.file != nullptr) == live) {
    LOG_ERROR("Pass either --file, or --group and --port");
    return false;
  }

  if (live && (args.mcast_group == nullptr || args.port == 0)) {
    LOG_ERROR("--group and --port go together");
    return false;
  }

  return true;
}

// Different types of FeedHandlers, based on what the system currently supports.
using ItchFile =
    FeedHandler<FileSource, LengthPrefixFramer<true, 4096>, ITCHParser>;
using NasdaqUDP =
    FeedHandler<McastUDPSource, LengthPrefixFramer<false, 4096>, ITCHParser>;
using Pipeline = std::variant<std::monostate, ItchFile, NasdaqUDP>;

template <class... Ts> struct overloaded : Ts... {
  using Ts::operator()...;
};

int main(int argc, char *argv[]) {
  Args args;
  if (!parse_args(argc, argv, args)) {
    std::print(stderr, "{}", usage(argv[0]));
    return 1;
  }

  // We only want the main thread to receive the stop signals
  // Block them before threads are created so that they inherit the block mask.
  sigset_t sigs;
  sigemptyset(&sigs);
  sigaddset(&sigs, SIGINT);
  sigaddset(&sigs, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &sigs, nullptr);

  std::jthread log_thread(logger::logger_thread);

  auto pipeline = std::make_unique<Pipeline>();

  try {
    switch (*args.exchange) {
    case Exchange::NASDAQ:
      if (args.file != nullptr) {
        pipeline->emplace<ItchFile>(args.config_fpath, args.file);
      } else {
        pipeline->emplace<NasdaqUDP>(args.config_fpath, args.iface,
                                     args.mcast_group, args.port);
      }
      break;
    case Exchange::CME:
    case Exchange::CBOE:
    case Exchange::NYSE:
    case Exchange::EUREX:
      LOG_ERROR("Exchange '{}' is not supported yet", args.exchange_name);
      return 1;
    }
  } catch (const std::exception &e) {
    LOG_ERROR("Failed to create the pipeline: {}", e.what());
    return 1;
  }

  const bool ok = std::visit(overloaded{[](std::monostate) { return false; },
                                        [](auto &fh) { return fh.start(); }},
                             *pipeline);
  if (!ok) {
    return 1;
  }

  int sig = 0;
  sigwait(&sigs, &sig);
  LOG_INFO("Received {}, shutting down", sig == SIGINT ? "SIGINT" : "SIGTERM");

  return 0;
}
