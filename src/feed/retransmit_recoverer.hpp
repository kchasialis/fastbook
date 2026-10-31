#pragma once

#include "buf_writer.hpp"
#include "framer.hpp"
#include "mold_udp64.hpp"
#include "mold_udp64_framer.hpp"
#include "recovery.hpp"
#include "timing.hpp"
#include "unicast_udp_source.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <expected>
#include <span>
#include <string_view>

class MoldRetransmitRecoverer {
private:
  UnicastUDPSource transp_;
  uint32_t max_attempts_;
  uint16_t max_per_request_;
  std::chrono::microseconds max_recovery_time_;

  std::expected<void, TransportStatus>
  send_request(std::string_view session, uint64_t seq, uint16_t count) {
    std::array<std::byte, mold::HEADER_SIZE> buf;
    BufWriter writer(buf);
    mold::write_header(
        writer, {.session = session, .seq_num = seq, .msg_count = count});
    return transp_.send(buf);
  }

public:
  MoldRetransmitRecoverer(const char *ip, uint16_t port,
                          std::chrono::microseconds reply_timeout,
                          std::chrono::microseconds max_recovery_time,
                          uint32_t max_attempts, uint16_t max_per_request)
      : transp_(ip, port, reply_timeout), max_attempts_(max_attempts),
        max_per_request_(max_per_request),
        max_recovery_time_(max_recovery_time) {}

  template <MessageSink Sink>
  std::expected<void, RecoveryError> recover(MoldUDP64Framer &framer,
                                             Sink &sink) noexcept {
    auto target = framer.gap_end();
    auto deadline = timing::MonoClock::now() + max_recovery_time_;
    uint32_t attempts = 0;

    while (framer.expected() < target) {
      if (attempts == max_attempts_) {
        return std::unexpected(RecoveryError::RetriesExhausted);
      }

      auto first = framer.expected();
      auto count = static_cast<uint16_t>(
          std::min<uint64_t>(target - first, max_per_request_));

      auto ret = send_request(framer.session(), first, count);
      if (!ret) {
        return std::unexpected(RecoveryError::TransportError);
      }

      attempts++;

      while (true) {
        if (timing::MonoClock::now() >= deadline) {
          return std::unexpected(RecoveryError::Timeout);
        }

        auto reply = transp_.next();
        if (!reply) {
          if (reply.error() == TransportStatus::WouldBlock) {
            break;
          }
          return std::unexpected(RecoveryError::TransportError);
        }

        auto prev_exp = framer.expected();

        auto val = reply.value();
        auto res = framer.feed(val.bytes(), sink);
        bool stop = false;
        if (!res) {
          switch (res.error()) {
          case MoldUDP64Framer::Error::Gap: {
            stop = true;
            break;
          }
          case MoldUDP64Framer::Error::Malformed: {
            continue;
          }
          case MoldUDP64Framer::Error::SessionChanged: {
            return std::unexpected(RecoveryError::SessionChanged);
          }
          case MoldUDP64Framer::Error::EndOfSession: {
            return std::unexpected(RecoveryError::SessionEnded);
          }
          default:
            assert(false);
            __builtin_unreachable();
          }
        } else {
          if (framer.expected() >= target) {
            return {};
          }

          if (framer.expected() > prev_exp) {
            // we made some progress, reset the counter.
            attempts = 0;
          }

          if (framer.expected() >= first + count) {
            break;
          }
        }

        if (stop) {
          break;
        }
      }
    }

    return {};
  }
};