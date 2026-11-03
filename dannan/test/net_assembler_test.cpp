//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Reassembly and refusals: the inventory's transport rows "fragment
// reassembly bounded in messages and bytes beyond the per-message limit; a
// message's memory leases follow it through executor handoffs" and "a route
// that refuses says why".

#include <gtest/gtest.h>

#include "dannan/net/assembler.h"
#include "dannan/net/message.h"

namespace dannan::net {
namespace {

using sched::FrameType;

WireFragment fragment(uint64_t id, bool first, bool last, size_t bytes, FrameType frame = FrameType::loader, MessageType type = MessageType{5}) {
  return WireFragment{id, frame, type, first, last, Bytes(bytes, 0x5a)};
}

TEST(Assembler, JoinsFragmentsIntoOneMessage) {
  MessageAssembler assembler(4, 1024, 1024);
  EXPECT_FALSE(assembler.push(fragment(1, true, false, 3)));
  EXPECT_FALSE(assembler.push(fragment(1, false, false, 4)));
  const auto done = assembler.push(fragment(1, false, true, 5));
  ASSERT_TRUE(done);
  EXPECT_EQ(done->request_id, 1u);
  EXPECT_EQ(done->message.payload.size(), 12u);
  EXPECT_EQ(assembler.incomplete_messages(), 0u);
  EXPECT_EQ(assembler.incomplete_bytes(), 0u);
}

TEST(Assembler, BoundsTheIncompleteMessagesTheirBytesAndEachMessage) {
  MessageAssembler assembler(2, 8, 16);
  EXPECT_FALSE(assembler.push(fragment(1, true, false, 3)));
  EXPECT_FALSE(assembler.push(fragment(2, true, false, 3)));
  EXPECT_THROW(assembler.push(fragment(3, true, false, 1)), FrameRefused);
  EXPECT_THROW(assembler.push(fragment(1, false, false, 3)), FrameRefused);
  EXPECT_EQ(assembler.incomplete_bytes(), 6u);
  assembler.discard(2);
  EXPECT_EQ(assembler.incomplete_messages(), 1u);
  EXPECT_EQ(assembler.incomplete_bytes(), 3u);
  MessageAssembler per_message(4, 64, 4);
  EXPECT_FALSE(per_message.push(fragment(9, true, false, 3)));
  EXPECT_THROW(per_message.push(fragment(9, false, true, 2)), FrameRefused);
}

TEST(Assembler, RefusesFragmentsThatBreakTheSequence) {
  MessageAssembler assembler(4, 1024, 1024);
  EXPECT_THROW(assembler.push(fragment(1, false, true, 1)), FrameRefused);
  EXPECT_FALSE(assembler.push(fragment(2, true, false, 1)));
  EXPECT_THROW(assembler.push(fragment(2, true, false, 1)), FrameRefused);
  EXPECT_THROW(assembler.push(fragment(2, false, true, 1, FrameType::loader, MessageType{6})), FrameRefused);
}

// A message may be promoted on the way, never demoted.
TEST(Assembler, AClassMayRiseDuringATransferButNeverFall) {
  MessageAssembler assembler(4, 1024, 1024);
  EXPECT_FALSE(assembler.push(fragment(1, true, false, 1, FrameType::loader)));
  EXPECT_FALSE(assembler.push(fragment(1, false, false, 1, FrameType::foreground)));
  EXPECT_THROW(assembler.push(fragment(1, false, true, 1, FrameType::loader)), FrameRefused);
  EXPECT_FALSE(assembler.push(fragment(2, true, false, 1, FrameType::speculative)));
  assembler.promote(2, FrameType::read_ahead);
  const auto done = assembler.push(fragment(2, false, true, 1, FrameType::read_ahead));
  ASSERT_TRUE(done);
  EXPECT_EQ(done->frame, FrameType::read_ahead);
}

TEST(Assembler, ANotificationMustFitOneFrame) {
  MessageAssembler assembler(4, 1024, 1024);
  EXPECT_THROW(assembler.push(fragment(0, true, false, 1)), FrameRefused);
  const auto done = assembler.push(fragment(0, true, true, 4, FrameType::control, message::telemetry));
  ASSERT_TRUE(done);
  EXPECT_EQ(done->request_id, 0u);
}

// The bytes are charged as reassembly from the first fragment until the
// last copy of the finished message goes.
TEST(Assembler, ChargesItsBytesUntilTheMessageIsGone) {
  sched::RetainedMemoryLedger memory(4096, 512, 1024, 512, 512);
  const auto held = [&] {
    const auto stats = memory.stats();
    const auto found = stats.owner_bytes.find(sched::MemoryOwner::rpc_frame);
    return found == stats.owner_bytes.end() ? uint64_t{0} : found->second;
  };
  MessageAssembler assembler(4, 2048, 2048, &memory);
  EXPECT_FALSE(assembler.push(fragment(1, true, false, 700)));
  EXPECT_GE(held(), 700u);
  auto done = assembler.push(fragment(1, false, true, 300));
  ASSERT_TRUE(done);
  EXPECT_GE(held(), 1000u);
  auto handed_off = std::move(done->message);
  done.reset();
  EXPECT_GE(held(), 1000u);
  handed_off = {};
  EXPECT_EQ(held(), 0u);
}

TEST(Assembler, RefusesWhatTheMemoryLedgerCannotHold) {
  sched::RetainedMemoryLedger memory(4096, 512, 1024, 512, 512);
  MessageAssembler assembler(4, 4096, 4096, &memory);
  EXPECT_FALSE(assembler.push(fragment(2, true, false, 2000)));
  EXPECT_THROW(assembler.push(fragment(3, true, false, 1800)), FrameRefused);
}

TEST(Refusal, AnErrorReplyCarriesItsStatusCodeAndText) {
  const auto reply = error_reply(503, "transport_busy", "reply not sent: peer outbound queue full");
  EXPECT_EQ(reply.type, message::error);
  const auto refusal = refusal_of(reply);
  ASSERT_TRUE(refusal);
  EXPECT_EQ(refusal->status, 503);
  EXPECT_EQ(refusal->code, "transport_busy");
  EXPECT_EQ(refusal->message, "reply not sent: peer outbound queue full");
  EXPECT_TRUE(try_later(*refusal));
}

// The text first, then the status and code: an older reader that takes only
// the text still reads it.
TEST(Refusal, TheTextComesFirst) {
  const auto reply = error_reply(404, "not_found", "no such thing");
  Reader reader(reply.payload);
  EXPECT_EQ(reader.string(), "no such thing");
  EXPECT_EQ(reader.u16(), 404);
  EXPECT_EQ(reader.string(), "not_found");
}

TEST(Refusal, OnlySomeStatusesMayBeTriedAgain) {
  for (const uint16_t status : {uint16_t{0}, uint16_t{429}, uint16_t{500}, uint16_t{502}, uint16_t{503}, uint16_t{504}})
    EXPECT_TRUE(try_later({status, "", ""})) << status;
  for (const uint16_t status : {uint16_t{400}, uint16_t{403}, uint16_t{404}, uint16_t{409}, uint16_t{501}}) EXPECT_FALSE(try_later({status, "", ""})) << status;
}

TEST(Refusal, AnEmptyOrForeignErrorStillReads) {
  EXPECT_EQ(refusal_of({message::error, {}, {}})->status, 0);
  EXPECT_FALSE(refusal_of({message::ok, {}, {}}).has_value());
}

}  // namespace
}  // namespace dannan::net
