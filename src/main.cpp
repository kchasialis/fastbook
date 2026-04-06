#include "book_builder.hpp"
#include "feed_reader.hpp"
#include "file_source.hpp"
#include "matching_engine.hpp"
#include "order_book.hpp"
#include "spsc_queue.hpp"
#include "udp_source.hpp"

#include <cstdint>
#include <getopt.h>
#include <iostream>
#include <thread>

struct Args {
  const char *ip{nullptr};
  const char *mcast{nullptr};
  const char *file{nullptr};
  uint16_t port{0};
};

struct Stats {
  uint64_t msg_count;
  uint32_t best_bid;
  uint32_t best_ask;
};

void usage(const char *prog) {
  std::cerr << "Usage:\n"
            << "  " << prog
            << " -f <file>                        (file replay)\n"
            << "  " << prog << " -i <ip> -m <mcast_ip> -p <port>  (live UDP)\n";
}

int parse_args(int argc, char *argv[], Args &args) {
  static option long_opts[] = {{"ip", required_argument, nullptr, 'i'},
                               {"mcast", required_argument, nullptr, 'm'},
                               {"port", required_argument, nullptr, 'p'},
                               {"file", required_argument, nullptr, 'f'},
                               {nullptr, 0, nullptr, 0}};

  int opt;
  while ((opt = getopt_long(argc, argv, ":i:m:p:f:", long_opts, nullptr)) !=
         -1) {
    switch (opt) {
    case 'i':
      args.ip = optarg;
      break;
    case 'm':
      args.mcast = optarg;
      break;
    case 'p':
      args.port = static_cast<uint16_t>(atoi(optarg));
      break;
    case 'f':
      args.file = optarg;
      break;
    case ':':
      std::cerr << "option requires an argument\n";
      return -1;
    case '?':
      std::cerr << "unknown option\n";
      return -1;
    }
  }

  return 0;
}

template <typename Source>
Stats run(Source &src, Queue &queue, OrderBook &order_book,
          MatchingEngine &engine) {
  auto producer = queue.producer();
  auto consumer = queue.consumer();
  FeedHandler<Queue> feed_handler(producer);
  FeedReader<Source> feed_reader(src, feed_handler);
  BookBuilder book_builder(consumer, order_book, engine);

  std::thread tproducer([&] {
    feed_reader.run();
    book_builder.stop();
  });
  std::thread tconsumer([&] { book_builder.run(); });

  tproducer.join();
  tconsumer.join();

  return {book_builder.message_count(), order_book.best_price(Side::BID),
          order_book.best_price(Side::ASK)};
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    usage(argv[0]);
    return 1;
  }

  Args args;
  if (parse_args(argc, argv, args) < 0) {
    usage(argv[0]);
    return 1;
  }

  Queue queue;
  OrderBook order_book{};
  MatchingEngine engine(order_book);
  Stats stats{};

  if (args.file) {
    FileSource src(args.file);
    stats = run(src, queue, order_book, engine);
  } else if (args.ip && args.mcast && args.port) {
    UDPSource src(args.ip, args.mcast, args.port);
    stats = run(src, queue, order_book, engine);
  } else {
    usage(argv[0]);
    return 1;
  }

  std::cout << "messages : " << stats.msg_count << "\n"
            << "best bid : " << stats.best_bid << "\n"
            << "best ask : " << stats.best_ask << "\n";

  return 0;
}
