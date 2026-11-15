//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Agreement's messages: what a proposer and its acceptors read from each other. Each
// decoder may throw only net::DecodeError.

#include "dannan/agree/wire.h"
#include "dannan/fuzz/fuzz.h"

namespace dannan::fuzz {
namespace {

template <class F>
void decoding(F&& f) {
  try {
    f();
  } catch (const net::DecodeError&) {
  }
}

const Registration agreement(
    "agree_messages",
    [](std::span<const uint8_t> data) {
      decoding([&] { (void)agree::decode_prepare(data); });
      decoding([&] { (void)agree::decode_accept(data); });
      decoding([&] { (void)agree::decode_purge(data); });
      decoding([&] { (void)agree::decode_prepare_answer(data); });
      decoding([&] { (void)agree::decode_accept_answer(data); });
    },
    [] {
      identity::NodeId a;
      a.bytes.fill(1);
      identity::NodeId b;
      b.bytes.fill(2);
      const auto set = agree::ReplicaSet::of({a, b}, 3);
      const agree::Ballot ballot{7, a, b};
      const agree::Name name{'k', 0, 'x'};
      return std::vector<net::Bytes>{agree::encode(agree::PrepareMessage{"mimir", {set, ballot, {name}}}),
                                     agree::encode(agree::AcceptMessage{"lugh/totp", {set, ballot, {name}, {agree::Bytes{1, 2, 3}}, {ballot}}}),
                                     agree::encode(agree::PurgeMessage{"mimir", {set, ballot, {name}}}),
                                     agree::encode(agree::PrepareAnswer{{}, {{ballot, agree::Bytes{9}, ballot}}}),
                                     agree::encode(agree::AcceptAnswer{agree::Refusal{"a higher ballot is promised", ballot, set}})};
    });

}  // namespace
}  // namespace dannan::fuzz
