//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Level 0's payloads and the dial-back request: what a broker, a relay and
// their peers read from each other. Each decoder may throw only
// net::DecodeError.

#include "dannan/fuzz/fuzz.h"
#include "dannan/reach/dial_back.h"
#include "dannan/reach/paths.h"

namespace dannan::fuzz {
namespace {

template <class F>
void decoding(F&& f) {
  try {
    f();
  } catch (const net::DecodeError&) {
  }
}

const Registration level0(
    "level0_payloads",
    [](std::span<const uint8_t> data) {
      // One input, every decoder: each is a different reading of the same
      // untrusted bytes.
      decoding([&] { (void)reach::decode_punch_request(data); });
      decoding([&] { (void)reach::decode_punch_go(data); });
      decoding([&] { (void)reach::decode_punch_reply(data); });
      decoding([&] { (void)reach::decode_relay_open(data); });
      decoding([&] { (void)reach::decode_relay_open_reply(data); });
      decoding([&] { (void)reach::decode_relay_offer(data); });
      decoding([&] { (void)reach::decode_relay_offer_reply(data); });
      decoding([&] { (void)reach::decode_relay_data(data); });
      decoding([&] { (void)reach::decode_relay_close(data); });
      decoding([&] { (void)reach::decode_dial_back_request(data); });
      decoding([&] { (void)reach::decode_dial_back_reply(data); });
    },
    [] {
      net::PathToken token{};
      token.fill(3);
      identity::NodeId id;
      id.bytes.fill(4);
      return std::vector<net::Bytes>{reach::encode(reach::PunchRequest{id, net::Lane::control, {"198.51.100.1", 4000}}),
                                     reach::encode(reach::PunchGo{id, net::Lane::data, token, {"h", 5}}),
                                     reach::encode(reach::PunchReply{true, "", {"h", 6}, token}),
                                     reach::encode(reach::RelayOffer{9, id, net::Lane::control, token}),
                                     reach::encode(reach::RelayData{9, {1, 2, 3}}),
                                     reach::encode_dial_back_request({"203.0.113.1", 7437})};
    });

}  // namespace
}  // namespace dannan::fuzz
