//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The stack's callback vocabulary and ack levels (TODO/2026-11-20-stack-plan.md, item 13):
// the executor rule (a completion is posted to the caller's executor, never called inline,
// even when the answer is known), one answer per operation by its deadline, one fault kind
// for every hosted system, and the four ack levels in order.

#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "dannan/contract/ack_level.h"
#include "dannan/contract/completion.h"

namespace dannan::contract {
namespace {

using namespace std::chrono_literals;

// A schedule the test fires by hand: the deadline is an event, never a real wait.
struct ManualSchedule {
  std::vector<std::function<void()>> pending;
  std::vector<std::chrono::milliseconds> delays;
  Schedule schedule() {
    return [this](std::chrono::milliseconds delay, std::function<void()> task) {
      delays.push_back(delay);
      pending.push_back(std::move(task));
    };
  }
  void fire_all() {
    auto tasks = std::move(pending);
    pending.clear();
    for (auto& task : tasks) task();
  }
};

TEST(Completion, AFinishIsPostedNeverCalledInline) {
  boost::asio::io_context io;
  ManualSchedule schedule;
  std::vector<int> answers;
  auto completion = Completion<int>::start(5s, io.get_executor(), [&](int v) { answers.push_back(v); }, [] { return -1; }, schedule.schedule());
  completion->finish(7);
  EXPECT_TRUE(answers.empty()) << "called inline";
  EXPECT_TRUE(completion->answered());
  io.run();
  EXPECT_EQ(answers, std::vector<int>{7});
}

TEST(Completion, TheScheduleIsAskedForTheDeadline) {
  boost::asio::io_context io;
  ManualSchedule schedule;
  auto completion = Completion<int>::start(1234ms, io.get_executor(), [](int) {}, [] { return 0; }, schedule.schedule());
  ASSERT_EQ(schedule.delays.size(), 1u);
  EXPECT_EQ(schedule.delays[0], 1234ms);
}

TEST(Completion, OnlyTheFirstFinishIsAnswered) {
  boost::asio::io_context io;
  ManualSchedule schedule;
  std::vector<int> answers;
  auto completion = Completion<int>::start(5s, io.get_executor(), [&](int v) { answers.push_back(v); }, [] { return -1; }, schedule.schedule());
  completion->finish(1);
  completion->finish(2);
  io.run();
  EXPECT_EQ(answers, std::vector<int>{1});
}

TEST(Completion, ADeadlineThatPassesFirstAnswersLateAndDropsALaterFinish) {
  boost::asio::io_context io;
  ManualSchedule schedule;
  std::vector<int> answers;
  auto completion = Completion<int>::start(5s, io.get_executor(), [&](int v) { answers.push_back(v); }, [] { return -1; }, schedule.schedule());
  schedule.fire_all();
  EXPECT_TRUE(answers.empty()) << "the late answer was called inline";
  EXPECT_TRUE(completion->answered());
  completion->finish(9);
  io.run();
  EXPECT_EQ(answers, std::vector<int>{-1});
}

TEST(Completion, ADeadlineAfterTheFinishChangesNothing) {
  boost::asio::io_context io;
  ManualSchedule schedule;
  std::vector<int> answers;
  auto completion = Completion<int>::start(5s, io.get_executor(), [&](int v) { answers.push_back(v); }, [] { return -1; }, schedule.schedule());
  completion->finish(3);
  schedule.fire_all();
  io.run();
  EXPECT_EQ(answers, std::vector<int>{3});
}

TEST(Completion, ADeadlineFiringAfterTheCompletionIsGoneDoesNothing) {
  boost::asio::io_context io;
  ManualSchedule schedule;
  int answers = 0;
  {
    auto completion = Completion<int>::start(5s, io.get_executor(), [&](int) { ++answers; }, [] { return -1; }, schedule.schedule());
    completion->finish(1);
  }
  io.run();
  schedule.fire_all();
  io.restart();
  io.run();
  EXPECT_EQ(answers, 1);
}

TEST(Completion, AFinishRacingTheDeadlineAnswersExactlyOnce) {
  for (int round = 0; round < 200; ++round) {
    boost::asio::io_context io;
    ManualSchedule schedule;
    std::atomic<int> answers{0};
    auto completion = Completion<int>::start(5s, io.get_executor(), [&](int) { ++answers; }, [] { return -1; }, schedule.schedule());
    std::thread deadline([&] { schedule.fire_all(); });
    completion->finish(1);
    deadline.join();
    io.run();
    ASSERT_EQ(answers.load(), 1) << "round " << round;
  }
}

TEST(Completion, TheDefaultTimerAnswersLateOnItsExecutor) {
  boost::asio::io_context io;
  std::optional<int> answer;
  auto completion = Completion<int>::start(0ms, io.get_executor(), [&](int v) { answer = v; }, [] { return -1; });
  io.run();
  ASSERT_TRUE(answer);
  EXPECT_EQ(*answer, -1);
}

TEST(Completion, TheDefaultTimerIsCancelledByAFinish) {
  boost::asio::io_context io;
  std::vector<int> answers;
  auto completion = Completion<int>::start(std::chrono::hours(1), io.get_executor(), [&](int v) { answers.push_back(v); }, [] { return -1; });
  completion->finish(4);
  // run() returns only once nothing is pending: a timer left armed would hold it for an hour.
  io.run();
  EXPECT_EQ(answers, std::vector<int>{4});
}

TEST(AnswerNow, PostsAKnownAnswerRatherThanCallingIt) {
  boost::asio::io_context io;
  std::optional<std::string> answer;
  answer_now<std::string>(io.get_executor(), [&](std::string v) { answer = v; }, "known");
  EXPECT_FALSE(answer) << "called inline";
  io.run();
  EXPECT_EQ(answer, "known");
}

TEST(Outcome, SuccessCarriesItsValue) {
  const auto ok = Outcome<int>::success(5);
  EXPECT_TRUE(ok.ok());
  EXPECT_EQ(ok.fault, Fault::none);
  EXPECT_EQ(ok.value, 5);
  EXPECT_TRUE(ok.message.empty());
}

TEST(Outcome, FailureCarriesItsFaultAndMessage) {
  const auto refused = Outcome<int>::failure(Fault::refused, "the last factor");
  EXPECT_FALSE(refused.ok());
  EXPECT_EQ(refused.fault, Fault::refused);
  EXPECT_EQ(refused.message, "the last factor");
}

TEST(Outcome, FromCarriesAFailureAcrossTypes) {
  const auto missing = Outcome<std::string>::failure(Fault::not_found, "no such user");
  const auto carried = Outcome<int>::from(missing);
  EXPECT_EQ(carried.fault, Fault::not_found);
  EXPECT_EQ(carried.message, "no such user");
}

// A failure may keep the evidence it has beside its fault (agreed with Dagda for 0.11.0): which
// replicas had a write that then timed out, say.
TEST(Outcome, AFailureMayKeepItsEvidence) {
  const auto late = Outcome<std::vector<int>>::failure(Fault::timed_out, "two of three answered", {1, 2});
  EXPECT_FALSE(late.ok());
  EXPECT_EQ(late.fault, Fault::timed_out);
  EXPECT_EQ(late.message, "two of three answered");
  EXPECT_EQ(late.value, (std::vector<int>{1, 2}));
}

TEST(Outcome, NothingIsTheDefaultValue) {
  const Outcome<> done = Outcome<>::success();
  EXPECT_TRUE(done.ok());
}

TEST(Fault, EveryKindHasItsName) {
  EXPECT_STREQ(fault_name(Fault::none), "none");
  EXPECT_STREQ(fault_name(Fault::not_found), "not_found");
  EXPECT_STREQ(fault_name(Fault::conflict), "conflict");
  EXPECT_STREQ(fault_name(Fault::duplicate), "duplicate");
  EXPECT_STREQ(fault_name(Fault::invalid), "invalid");
  EXPECT_STREQ(fault_name(Fault::refused), "refused");
  EXPECT_STREQ(fault_name(Fault::unavailable), "unavailable");
  EXPECT_STREQ(fault_name(Fault::timed_out), "timed_out");
  EXPECT_STREQ(fault_name(Fault::failed), "failed");
  EXPECT_STREQ(fault_name(Fault::cancelled), "cancelled");
}

// Added last, so every kind before it keeps its number (agreed with Dagda for 0.11.0).
TEST(Fault, CancelledComesLast) {
  EXPECT_EQ(static_cast<int>(Fault::failed), 8);
  EXPECT_EQ(static_cast<int>(Fault::cancelled), 9);
}

TEST(Fault, AValueOutOfRangeIsNamedUnknown) { EXPECT_STREQ(fault_name(static_cast<Fault>(200)), "unknown"); }

TEST(AckLevel, AValueOutOfRangeIsNamedUnknown) { EXPECT_STREQ(ack_level_name(static_cast<AckLevel>(200)), "unknown"); }

TEST(AckLevel, TheFourAreOrderedWeakestFirst) {
  EXPECT_LT(AckLevel::ephemeral, AckLevel::persisted);
  EXPECT_LT(AckLevel::persisted, AckLevel::decided);
  EXPECT_LT(AckLevel::decided, AckLevel::replicated);
}

TEST(AckLevel, NamesRoundTrip) {
  for (const auto level : {AckLevel::ephemeral, AckLevel::persisted, AckLevel::decided, AckLevel::replicated}) {
    const auto parsed = parse_ack_level(ack_level_name(level));
    ASSERT_TRUE(parsed) << ack_level_name(level);
    EXPECT_EQ(*parsed, level);
  }
}

TEST(AckLevel, TheNamesAreTheStacksStandard) {
  const auto names = ack_level_names();
  ASSERT_EQ(names.size(), 4u);
  EXPECT_EQ(names[0], "ephemeral");
  EXPECT_EQ(names[1], "persisted");
  EXPECT_EQ(names[2], "decided");
  EXPECT_EQ(names[3], "replicated");
}

TEST(AckLevel, AnUnknownNameIsNotALevel) {
  EXPECT_FALSE(parse_ack_level("durable"));
  EXPECT_FALSE(parse_ack_level(""));
  EXPECT_FALSE(parse_ack_level("Replicated"));
}

}  // namespace
}  // namespace dannan::contract
