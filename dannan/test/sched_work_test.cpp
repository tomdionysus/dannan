//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Work classes and waits: the inventory's "Work classes, waits and budgets"
// table, rows "one definition of class from frame type" and "declared waits
// per operation and the runtime wait guard"; laws 1 to 4.

#include <gtest/gtest.h>

#include <atomic>
#include <stdexcept>
#include <string>
#include <vector>

#include "dannan/sched/work.h"
#include "dannan/status/log.h"
#include "dannan/test/fixtures/recording_sink.h"

namespace dannan::sched {
namespace {

using namespace std::chrono_literals;

// The wire values are protocol 23's and never change.
TEST(FrameType, WireValuesAreFixed) {
  EXPECT_EQ(static_cast<int>(FrameType::control), 1);
  EXPECT_EQ(static_cast<int>(FrameType::foreground), 2);
  EXPECT_EQ(static_cast<int>(FrameType::read_ahead), 3);
  EXPECT_EQ(static_cast<int>(FrameType::speculative), 4);
  EXPECT_EQ(static_cast<int>(FrameType::loader), 5);
}

// The laws' ranking on a transport queue: control, then foreground with
// read-ahead behind it, then loader, then speculative.
TEST(FrameType, PriorityFollowsTheLaws) {
  EXPECT_LT(frame_priority(FrameType::control), frame_priority(FrameType::foreground));
  EXPECT_LT(frame_priority(FrameType::foreground), frame_priority(FrameType::read_ahead));
  EXPECT_LT(frame_priority(FrameType::read_ahead), frame_priority(FrameType::loader));
  EXPECT_LT(frame_priority(FrameType::loader), frame_priority(FrameType::speculative));
}

TEST(FrameType, DecodesOnlyTheFiveWireValues) {
  for (int value = 0; value < 256; ++value) {
    const auto decoded = decode_frame_type(static_cast<uint8_t>(value));
    EXPECT_EQ(decoded.has_value(), value >= 1 && value <= 5) << value;
    if (decoded) {
      EXPECT_EQ(static_cast<int>(*decoded), value);
    }
  }
}

TEST(FrameType, NamesAreStable) {
  EXPECT_EQ(std::string(frame_type_name(FrameType::control)), "control");
  EXPECT_EQ(std::string(frame_type_name(FrameType::foreground)), "foreground");
  EXPECT_EQ(std::string(frame_type_name(FrameType::read_ahead)), "read-ahead");
  EXPECT_EQ(std::string(frame_type_name(FrameType::loader)), "loader");
  EXPECT_EQ(std::string(frame_type_name(FrameType::speculative)), "speculative");
}

// One definition: read-ahead is foreground work travelling behind it.
TEST(WorkClass, ComesFromTheFrameTypeAlone) {
  EXPECT_EQ(work_class(FrameType::control), WorkClass::control);
  EXPECT_EQ(work_class(FrameType::foreground), WorkClass::foreground);
  EXPECT_EQ(work_class(FrameType::read_ahead), WorkClass::foreground);
  EXPECT_EQ(work_class(FrameType::loader), WorkClass::loader);
  EXPECT_EQ(work_class(FrameType::speculative), WorkClass::speculative);
}

TEST(WorkClass, FrameForRoundTripsThroughWorkClass) {
  for (const auto work : {WorkClass::control, WorkClass::foreground, WorkClass::loader, WorkClass::speculative}) EXPECT_EQ(work_class(frame_for(work)), work);
  EXPECT_EQ(frame_for(WorkClass::foreground), FrameType::foreground);
}

// Law 1: control never waits on the DATA device or the network.
TEST(Waits, ControlMayWaitOnlyOnThisNode) {
  EXPECT_TRUE(includes(default_waits(WorkClass::control), Waits::state_device));
  EXPECT_TRUE(includes(default_waits(WorkClass::control), Waits::locks));
  EXPECT_FALSE(includes(default_waits(WorkClass::control), Waits::data_device));
  EXPECT_FALSE(includes(default_waits(WorkClass::control), Waits::network));
  for (const auto work : {WorkClass::foreground, WorkClass::loader, WorkClass::speculative})
    for (const auto wait : {Waits::state_device, Waits::data_device, Waits::network, Waits::locks}) EXPECT_TRUE(includes(default_waits(work), wait));
}

TEST(Waits, MayEnterOnlyWhatEveryDeclaredWaitAllows) {
  EXPECT_TRUE(may_enter(local_waits, Waits::none));
  EXPECT_TRUE(may_enter(local_waits, Waits::state_device));
  EXPECT_TRUE(may_enter(local_waits, Waits::state_device | Waits::locks));
  EXPECT_FALSE(may_enter(local_waits, Waits::network));
  EXPECT_FALSE(may_enter(local_waits, Waits::state_device | Waits::network));
  EXPECT_TRUE(may_enter(Waits::none, Waits::none));
  EXPECT_FALSE(may_enter(Waits::none, Waits::locks));
}

// A context narrows its class's waits and never widens them.
TEST(WorkContext, AllowedNarrowsTheClassDefaultAndNeverWidens) {
  const WorkContext memory_only(FrameType::foreground, {}, nullptr, "test", Waits::locks);
  EXPECT_EQ(memory_only.allowed(), Waits::locks);
  const WorkContext control_asking_for_network(FrameType::control, {}, nullptr, "test", Waits::network | Waits::locks);
  EXPECT_FALSE(includes(control_asking_for_network.allowed(), Waits::network));
  EXPECT_TRUE(includes(control_asking_for_network.allowed(), Waits::locks));
  const WorkContext loader(FrameType::loader);
  EXPECT_EQ(loader.allowed(), default_waits(WorkClass::loader));
}

TEST(WorkContext, ReportsCancellationAndExpiryAtTheTimeItIsGiven) {
  std::atomic_bool cancelled{false};
  const auto start = TimePoint{} + std::chrono::seconds(100);
  const WorkContext context(FrameType::loader, start + std::chrono::seconds(5), &cancelled, "test");
  EXPECT_FALSE(context.cancelled());
  EXPECT_FALSE(context.expired(start));
  EXPECT_FALSE(context.expired(start + std::chrono::milliseconds(4999)));
  EXPECT_TRUE(context.expired(start + std::chrono::seconds(5)));
  cancelled = true;
  EXPECT_TRUE(context.cancelled());
}

TEST(WorkContext, NoDeadlineNeverExpires) {
  const WorkContext context(FrameType::loader);
  EXPECT_FALSE(context.expired(TimePoint::max()));
}

class CapturingSink final : public status::LogSink {
 public:
  bool enabled(status::LogLevel) const noexcept override { return true; }
  void write(status::LogLevel level, std::string_view scope, std::string_view message) override {
    lines.push_back(std::string(status::log_level_name(level)) + " " + std::string(scope) + ": " + std::string(message));
  }
  std::vector<std::string> lines;
};

TEST(WaitGuard, AdmitsWhatTheWorkAllows) {
  auto sink = std::make_shared<CapturingSink>();
  WaitGuard guard(status::ScopedLogger(sink, "waits"));
  const WorkContext loader(FrameType::loader, {}, nullptr, "loader work");
  EXPECT_TRUE(guard.enter(loader, Waits::network | Waits::data_device, "fetch"));
  EXPECT_EQ(guard.violations(), 0u);
  EXPECT_TRUE(sink->lines.empty());
}

// A refusal is counted every time and reported once per (origin, operation),
// so a hot path reached from control does not flood the log.
TEST(WaitGuard, CountsEveryRefusalAndReportsEachPathOnce) {
  auto sink = std::make_shared<CapturingSink>();
  WaitGuard guard(status::ScopedLogger(sink, "waits"));
  const WorkContext control(FrameType::control, {}, nullptr, "status poll");
  EXPECT_FALSE(guard.enter(control, Waits::network, "peer call"));
  EXPECT_FALSE(guard.enter(control, Waits::network, "peer call"));
  EXPECT_FALSE(guard.enter(control, Waits::data_device, "disk read"));
  EXPECT_EQ(guard.violations(), 3u);
  ASSERT_EQ(sink->lines.size(), 2u);
  EXPECT_NE(sink->lines[0].find("status poll"), std::string::npos);
  EXPECT_NE(sink->lines[0].find("peer call"), std::string::npos);
  EXPECT_EQ(sink->lines[0].rfind("WARN waits: ", 0), 0u);
}

TEST(WaitGuard, ThrowsOnARefusalWhenTestsAskIt) {
  auto sink = std::make_shared<CapturingSink>();
  WaitGuard guard(status::ScopedLogger(sink, "waits"), WaitGuard::Mode::throw_on_violation);
  const WorkContext control(FrameType::control, {}, nullptr, "status poll");
  EXPECT_THROW(guard.enter(control, Waits::network, "peer call"), std::logic_error);
  EXPECT_EQ(guard.violations(), 1u);
  EXPECT_NO_THROW(guard.enter(control, Waits::locks, "table read"));
}

TEST(Waits, ComposeAndIncludeOverEveryMask) {
  for (unsigned a = 0; a < 16; ++a)
    for (unsigned b = 0; b < 16; ++b) {
      const auto combined = static_cast<Waits>(a) | static_cast<Waits>(b);
      EXPECT_EQ(static_cast<unsigned>(combined), a | b);
      for (const unsigned bit : {1U, 2U, 4U, 8U}) EXPECT_EQ(includes(combined, static_cast<Waits>(bit)), ((a | b) & bit) != 0);
    }
  EXPECT_FALSE(includes(Waits::none, Waits::network));
}

// Work may allow itself less, never more; allowed means a subset, every mask against
// every mask.
TEST(Waits, MayEnterIsASubsetOverEveryMask) {
  const WorkContext read(FrameType::foreground, {}, nullptr, "read", Waits::state_device | Waits::locks);
  EXPECT_TRUE(may_enter(read.allowed(), Waits::state_device));
  EXPECT_FALSE(may_enter(read.allowed(), Waits::network));
  const WorkContext control(FrameType::control, {}, nullptr, "control", Waits::state_device | Waits::data_device | Waits::network | Waits::locks);
  EXPECT_FALSE(may_enter(control.allowed(), Waits::network));
  EXPECT_FALSE(may_enter(control.allowed(), Waits::data_device));
  for (unsigned allowed = 0; allowed < 16; ++allowed)
    for (unsigned declared = 0; declared < 16; ++declared)
      EXPECT_EQ(may_enter(static_cast<Waits>(allowed), static_cast<Waits>(declared)), (declared & ~allowed) == 0);
}

// Macha's words and level, once per origin and operation: contract/work.cpp:43.
TEST(WaitGuard, AViolationIsWarnedOfOnceInMachasWords) {
  auto sink = std::make_shared<test::RecordingSink>();
  WaitGuard guard(status::ScopedLogger(sink, "waits"), WaitGuard::Mode::record);
  const WorkContext context(FrameType::foreground, {}, nullptr, "a read", Waits::locks);
  EXPECT_FALSE(guard.enter(context, Waits::network, "the network"));
  EXPECT_FALSE(guard.enter(context, Waits::network, "the network"));
  EXPECT_EQ(sink->count(status::LogLevel::warn, "work (a read) entered the network, which may wait on what the work does not allow"), 1u);
}

}  // namespace
}  // namespace dannan::sched
