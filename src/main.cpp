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
#include <stdexcept>
#include <thread>

struct Args {
  const char *ip;
  const char *mcast;
  uint16_t port;
};

int parse_args(int argc, char *argv[], Args *args) {
  static option long_opts[] = {{"ip", required_argument, nullptr, 'i'},
                               {"mcast", required_argument, nullptr, 'm'},
                               {"port", required_argument, nullptr, 'p'},
                               {nullptr, 0, nullptr, 0}};

  int opt;
  while ((opt = getopt_long(argc, argv, ":i:m:p:f:", long_opts, nullptr)) !=
         -1) {
    switch (opt) {
    case 'i':
      args->ip = optarg;
      break;
    case 'm':
      args->mcast = optarg;
      break;
    case 'p':
      args->port = static_cast<uint16_t>(atoi(optarg));
      break;
    case ':':
      std::cerr << "option requires an argument" << std::endl;
      return -1;
    case '?':
      std::cerr << "unknown option" << std::endl;
      return -1;
    }
  }

  return 0;
}

void run_producer(FeedReader<UDPSource> &reader, BookBuilder &book_builder) {
  reader.run();
  book_builder.stop();
}

void run_consumer(BookBuilder &book_builder) { book_builder.run(); }

int main(int argc, char *argv[]) {
  Args args;
  if (parse_args(argc, argv, &args) < 0) {
    throw std::runtime_error("Invalid program arguments provided");
  }

  Queue queue;
  UDPSource udp_src(args.ip, args.mcast, args.port);
  OrderBook order_book{};
  Queue::SPSCProducer prod(queue);
  FeedHandler<Queue> feed_handler(prod);
  Queue::SPSCConsumer consumer(queue);
  FeedReader<UDPSource> feed_reader(udp_src, feed_handler);
  MatchingEngine engine(order_book);
  BookBuilder book_builder(consumer, order_book, engine);

  std::thread tproducer(run_producer, std::ref(feed_reader),
                        std::ref(book_builder));
  std::thread tconsumer(run_consumer, std::ref(book_builder));

  tproducer.join();
  tconsumer.join();

  return 0;
}