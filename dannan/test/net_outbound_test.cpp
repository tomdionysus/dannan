//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// A session's outbound queue: the inventory's transport rows "frame priority
// order on a transport queue", "outbound queue limits per connection with a
// reserve for small messages", "an in-flight call can be cancelled or
// promoted", and 0.91.3's "a reply to an admitted request is never dropped
// for capacity". Laws 1 to 4.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "dannan/net/outbound.h"

namespace dannan::net {
namespace {

using sched::FrameType;

Message msg(size_t bytes, MessageType type = MessageType{1029}) { return {type, Bytes(bytes, 0x5a), {}}; }

// Sends everything queued, one fragment at a time; returns the request ids
// in the order their fragments left.
std::vector<uint64_t> drain(OutboundQueue& queue, size_t max_frame) {
  std::vector<uint64_t> order;
  while (auto next = queue.next(max_frame)) {
    order.push_back(next->request_id);
    queue.written();
  }
  return order;
}

TEST(OutboundRefusal, AnEmptyQueueTakesEither) {
  EXPECT_EQ(outbound_refusal(0, 0, 1), nullptr);
  EXPECT_EQ(outbound_refusal(0, 0, small_message + 1), nullptr);
}

// No number of large messages crowds a small one out.
TEST(OutboundRefusal, LargeMessagesStopShortOfTheSmallReserve) {
  const size_t large = small_message + 1;
  const size_t large_cap = max_outbound_messages - small_reserve_messages;
  const size_t large_bytes = max_outbound_bytes - small_reserve_bytes;
  EXPECT_EQ(outbound_refusal(large_cap - 1, 0, large), nullptr);
  EXPECT_NE(outbound_refusal(large_cap, 0, large), nullptr);
  EXPECT_EQ(outbound_refusal(large_cap, 0, small_message), nullptr);
  EXPECT_EQ(outbound_refusal(0, large_bytes - large, large), nullptr);
  EXPECT_NE(outbound_refusal(0, large_bytes - large + 1, large), nullptr);
  EXPECT_EQ(outbound_refusal(0, large_bytes, 100), nullptr);
  EXPECT_EQ(outbound_refusal(max_outbound_messages - 1, 0, 100), nullptr);
  EXPECT_NE(outbound_refusal(max_outbound_messages, 0, 100), nullptr);
  EXPECT_NE(outbound_refusal(0, max_outbound_bytes - 99, 100), nullptr);
  EXPECT_NE(outbound_refusal(0, 0, max_outbound_bytes + 1), nullptr);
}

TEST(Outbound, TheMostUrgentClassLeavesFirst) {
  OutboundQueue queue;
  queue.push(1, FrameType::speculative, msg(10), false);
  queue.push(3, FrameType::loader, msg(10), false);
  queue.push(5, FrameType::read_ahead, msg(10), false);
  queue.push(7, FrameType::foreground, msg(10), false);
  queue.push(9, FrameType::control, msg(10, message::members), false);
  EXPECT_EQ(drain(queue, 4096), (std::vector<uint64_t>{9, 7, 5, 3, 1}));
}

TEST(Outbound, AMessageLargerThanAFrameLeavesInFragments) {
  OutboundQueue queue;
  queue.push(1, FrameType::loader, msg(10000), false);
  auto first = queue.next(4096);
  ASSERT_TRUE(first);
  EXPECT_TRUE(first->first);
  EXPECT_FALSE(first->last);
  EXPECT_EQ(first->payload.size(), 4096u);
  queue.written();
  auto second = queue.next(4096);
  EXPECT_FALSE(second->first);
  EXPECT_FALSE(second->last);
  queue.written();
  auto third = queue.next(4096);
  EXPECT_TRUE(third->last);
  EXPECT_EQ(third->payload.size(), 10000u - 8192u);
  queue.written();
  EXPECT_FALSE(queue.next(4096));
}

// The writer reconsiders after every frame: urgent work overtakes a long
// transfer already under way.
TEST(Outbound, UrgentWorkOvertakesATransferBetweenItsFrames) {
  OutboundQueue queue;
  queue.push(1, FrameType::speculative, msg(20000), false);
  auto first = queue.next(4096);
  EXPECT_EQ(first->request_id, 1u);
  queue.push(3, FrameType::foreground, msg(10), false);
  queue.written();
  EXPECT_EQ(queue.next(4096)->request_id, 3u);
}

// Equal classes share the lane frame by frame.
TEST(Outbound, EqualClassesTakeTurnsByFrame) {
  OutboundQueue queue;
  queue.push(1, FrameType::loader, msg(8192), false);
  queue.push(3, FrameType::loader, msg(8192), false);
  EXPECT_EQ(drain(queue, 4096), (std::vector<uint64_t>{1, 3, 1, 3}));
}

TEST(Outbound, ARequestPastTheLimitIsRefusedWithItsReason) {
  OutboundQueue queue;
  for (size_t i = 0; i < max_outbound_messages - small_reserve_messages; ++i) queue.push(2 * i + 1, FrameType::loader, msg(small_message + 1), false);
  try {
    queue.push(99999, FrameType::loader, msg(small_message + 1), false);
    FAIL() << "a large request past the limit was queued";
  } catch (const OutboundRefused& refused) {
    EXPECT_NE(std::string(refused.what()).find("queue full"), std::string::npos);
  }
  EXPECT_TRUE(queue.push(100001, FrameType::control, msg(10, message::ping), false));
}

// 0.91.3: a reply the queue would refuse goes as a 503 transport_busy in its
// place, so the caller is answered.
TEST(Outbound, AReplyIsNeverDroppedForCapacity) {
  OutboundQueue queue;
  for (size_t i = 0; i < max_outbound_messages; ++i) queue.push(2 * i + 1, FrameType::control, msg(10, message::ping), false);
  queue.register_inbound(2, FrameType::control);
  EXPECT_TRUE(queue.push(2, FrameType::control, msg(10, message::ok), true));
  EXPECT_EQ(queue.size(), max_outbound_messages + 1);
  std::optional<MessageType> type;
  Bytes payload;  // copied: a fragment's span lives only until written()
  while (auto next = queue.next(4096)) {
    if (next->request_id == 2) {
      type = next->type;
      payload.assign(next->payload.begin(), next->payload.end());
    }
    queue.written();
  }
  ASSERT_TRUE(type);
  EXPECT_EQ(*type, message::error);
  const auto refusal = refusal_of({message::error, payload, {}});
  ASSERT_TRUE(refusal);
  EXPECT_EQ(refusal->status, 503);
  EXPECT_EQ(refusal->code, "transport_busy");
  EXPECT_EQ(refusal->message.rfind("reply not sent: ", 0), 0u);
}

// Past one substitute per reply a caller can be awaiting, the queue gives up
// and the session closes, which answers every caller.
TEST(Outbound, SubstitutesStopAtTheirOwnBound) {
  OutboundQueue queue;
  for (size_t i = 0; i < max_outbound_with_refusals; ++i) queue.push(2 * i + 1, FrameType::control, msg(10, message::ping), i >= max_outbound_messages);
  EXPECT_THROW(queue.push(2, FrameType::control, msg(10, message::ok), true), OutboundRefused);
}

// The session queues a reply at its request's class; a promotion of the
// request since raises it.
TEST(Outbound, AReplyTravelsAtItsRequestsClassAndRisesWithIt) {
  OutboundQueue queue;
  queue.register_inbound(2, FrameType::loader);
  queue.push(2, FrameType::loader, msg(10, message::ok), true);
  queue.push(1, FrameType::foreground, msg(10), false);
  EXPECT_EQ(queue.next(4096)->request_id, 1u);
  queue.written();
  auto reply = queue.next(4096);
  EXPECT_EQ(reply->request_id, 2u);
  EXPECT_EQ(reply->frame, FrameType::loader);
  queue.written();

  queue.register_inbound(4, FrameType::speculative);
  queue.push(4, FrameType::speculative, msg(10, message::ok), true);
  queue.push(3, FrameType::loader, msg(10), false);
  queue.promote_inbound(4, FrameType::foreground);
  auto first = queue.next(4096);
  EXPECT_EQ(first->request_id, 4u);
  EXPECT_EQ(first->frame, FrameType::foreground);
}

// The peer cancelled its request: its queued reply goes, and a reply the
// handler produces later is dropped.
TEST(Outbound, ACancelledRequestsReplyIsNeverSent) {
  OutboundQueue queue;
  queue.register_inbound(2, FrameType::loader);
  queue.push(2, FrameType::loader, msg(10, message::ok), true);
  queue.cancel_inbound(2);
  EXPECT_FALSE(queue.next(4096));
  queue.register_inbound(4, FrameType::loader);
  queue.cancel_inbound(4);
  EXPECT_FALSE(queue.push(4, FrameType::loader, msg(10, message::ok), true));
  // An unregistered id makes no tombstone: a peer cannot grow the set.
  queue.cancel_inbound(99);
  EXPECT_TRUE(queue.push(99, FrameType::loader, msg(10, message::ok), true));
}

TEST(Outbound, APromotedRequestOvertakesItsOldPeers) {
  OutboundQueue queue;
  queue.push(1, FrameType::speculative, msg(10), false);
  queue.push(3, FrameType::speculative, msg(10), false);
  queue.promote_outgoing(3, FrameType::foreground);
  EXPECT_EQ(drain(queue, 4096), (std::vector<uint64_t>{3, 1}));
}

// A promotion that arrives while a fragment is being written applies to the
// rest of that message.
TEST(Outbound, APromotionDuringAWriteAppliesToTheRest) {
  OutboundQueue queue;
  queue.push(1, FrameType::speculative, msg(8192), false);
  queue.push(3, FrameType::loader, msg(10), false);
  auto first = queue.next(4096);
  EXPECT_EQ(first->request_id, 3u);
  queue.written();
  first = queue.next(4096);
  EXPECT_EQ(first->request_id, 1u);
  queue.promote_outgoing(1, FrameType::foreground);
  queue.push(5, FrameType::loader, msg(10), false);
  queue.written();
  const auto rest = queue.next(4096);
  EXPECT_EQ(rest->request_id, 1u);
  EXPECT_EQ(rest->frame, FrameType::foreground);
}

TEST(Outbound, ACancelledRequestLeavesTheQueueEvenMidTransfer) {
  OutboundQueue queue;
  queue.push(1, FrameType::loader, msg(10), false);
  queue.cancel_outgoing(1);
  EXPECT_FALSE(queue.next(4096));
  queue.push(3, FrameType::loader, msg(10000), false);
  ASSERT_TRUE(queue.next(4096));
  queue.cancel_outgoing(3);
  queue.written();
  EXPECT_FALSE(queue.next(4096));
}

TEST(Outbound, ANotificationIsToldWhetherItWasSent) {
  OutboundQueue queue;
  std::vector<bool> told;
  queue.push(0, FrameType::control, msg(10, message::telemetry), false, [&](bool sent) { told.push_back(sent); });
  queue.push(0, FrameType::control, msg(10, message::telemetry), false, [&](bool sent) { told.push_back(sent); });
  ASSERT_TRUE(queue.next(4096));
  queue.written();
  queue.abandon();
  EXPECT_EQ(told, (std::vector<bool>{true, false}));
  EXPECT_EQ(queue.size(), 0u);
}

TEST(Outbound, CountsItsQueuedBytes) {
  OutboundQueue queue;
  queue.push(1, FrameType::loader, msg(1000), false);
  queue.push(3, FrameType::loader, msg(500), false);
  EXPECT_EQ(queue.bytes(), 1500u);
  ASSERT_TRUE(queue.next(4096));
  queue.written();
  EXPECT_EQ(queue.bytes(), 500u);
}

TEST(Outbound, ChargesQueuedMessagesToTheMemoryLedger) {
  sched::RetainedMemoryLedger memory(4096, 512, 1024, 512, 512);
  OutboundQueue queue(&memory);
  queue.push(1, FrameType::loader, msg(1000), false);
  EXPECT_GE(memory.stats().used_bytes, 1000u);
  EXPECT_THROW(queue.push(3, FrameType::loader, msg(3000), false), OutboundRefused);
  // A reply the ledger refuses is substituted, never dropped.
  queue.register_inbound(2, FrameType::loader);
  EXPECT_TRUE(queue.push(2, FrameType::loader, msg(3000, message::ok), true));
  drain(queue, 4096);
  EXPECT_EQ(memory.stats().used_bytes, 0u);
}

}  // namespace
}  // namespace dannan::net
