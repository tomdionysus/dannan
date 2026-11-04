//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Supervision: the inventory's "Supervision" table. Plugins from a
// directory with the build identity checked before any call in; declining
// is unavailable, a throw a failure under backoff and disable; a fault after
// start rebuilds the instance, first reason winning; stop may write;
// builtins supervised alike; supervised threads; the registry by name; the
// startup stall gate; a blocking construction cancelled by the stop token.

#include <gtest/gtest.h>

#include <fstream>
#include <future>
#include <thread>

#include "dannan/supervise/registry.h"
#include "dannan/supervise/startup.h"
#include "dannan/supervise/supervised.h"
#include "dannan/supervise/supervisor.h"
#include "dannan/test/fixtures/stepped_time.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dannan::supervise {
namespace {

using namespace std::chrono_literals;

SupervisorOptions options(const std::filesystem::path& dir = {}) {
  SupervisorOptions out;
  out.plugin_dir = dir;
  out.entry_symbol = "dannan_test_subsystem_entry";
  out.build_identity = "dannan test build";
  out.policy.initial_backoff = 10ms;
  out.policy.max_backoff = 20ms;
  out.policy.max_failures_in_window = 3;
  return out;
}

bool eventually(const std::function<bool()>& condition) {
  for (int i = 0; i < 1000; ++i) {
    if (condition()) return true;
    std::this_thread::sleep_for(5ms);
  }
  return condition();
}

std::optional<SubsystemStatus> status_of(const Supervisor& supervisor, const std::string& name) {
  for (const auto& status : supervisor.statuses())
    if (status.name == name) return status;
  return std::nullopt;
}

struct Builtin final : Subsystem {
  std::string label;
  std::atomic<int>* stops;
  std::function<void(FaultSink&)> on_start;
  FaultSink sink;
  Builtin(std::string label, std::atomic<int>* stops, std::function<void(FaultSink&)> on_start)
      : label(std::move(label)), stops(stops), on_start(std::move(on_start)) {}
  std::string_view name() const noexcept override { return label; }
  void attach_fault_sink(FaultSink s) override { sink = std::move(s); }
  void start() override {
    if (on_start) on_start(sink);
  }
  void stop() override { ++*stops; }
};

TEST(Supervisor, RunsABuiltinAndStopsIt) {
  std::atomic<int> stops{0};
  Supervisor supervisor(options());
  supervisor.add_builtin("fuse", [&](const SubsystemContext&) { return std::make_unique<Builtin>("fuse", &stops, nullptr); });
  supervisor.start({});
  EXPECT_TRUE(eventually([&] { return status_of(supervisor, "fuse")->state == SubsystemState::running; }));
  supervisor.stop();
  EXPECT_EQ(stops.load(), 1);
}

// Declining is "not here": unavailable, never retried.
TEST(Supervisor, ADeclinedSubsystemIsUnavailable) {
  std::atomic<int> created{0};
  Supervisor supervisor(options());
  supervisor.add_builtin("torrent", [&](const SubsystemContext&) -> std::unique_ptr<Subsystem> {
    ++created;
    return nullptr;
  });
  supervisor.start({});
  EXPECT_TRUE(eventually([&] { return status_of(supervisor, "torrent")->state == SubsystemState::unavailable; }));
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(created.load(), 1);
}

// A failing start is retried with backoff and, past the budget, disabled
// for an operator; the last fault is kept.
TEST(Supervisor, AFailingSubsystemIsRetriedThenDisabled) {
  std::atomic<int> created{0};
  Supervisor supervisor(options());
  supervisor.add_builtin("scanner", [&](const SubsystemContext&) -> std::unique_ptr<Subsystem> {
    ++created;
    throw std::runtime_error("no catalogue");
  });
  supervisor.start({});
  EXPECT_TRUE(eventually([&] { return status_of(supervisor, "scanner")->state == SubsystemState::disabled; }));
  const auto status = *status_of(supervisor, "scanner");
  EXPECT_EQ(status.last_fault, "no catalogue");
  EXPECT_EQ(created.load(), 4);
  EXPECT_EQ(status.restart_count, 4u);
}

// A fault after start destroys the instance and builds a fresh one; the
// first reason wins.
TEST(Supervisor, AFaultAfterStartRebuildsTheInstance) {
  std::atomic<int> stops{0};
  std::atomic<int> built{0};
  Supervisor supervisor(options());
  supervisor.add_builtin("fuse", [&](const SubsystemContext&) {
    const int n = ++built;
    return std::make_unique<Builtin>("fuse", &stops, [n](Subsystem::FaultSink& sink) {
      if (n == 1) {
        sink("mount went away");
        sink("second reason");
      }
    });
  });
  supervisor.start({});
  EXPECT_TRUE(eventually([&] { return built.load() == 2 && status_of(supervisor, "fuse")->state == SubsystemState::running; }));
  const auto status = *status_of(supervisor, "fuse");
  EXPECT_EQ(status.last_fault, "mount went away");
  EXPECT_EQ(status.restart_count, 1u);
  EXPECT_EQ(stops.load(), 1);
  supervisor.stop();
}

// A construction that may block is cancelled by the stop token.
TEST(Supervisor, StopCancelsABlockingConstruction) {
  std::promise<void> entered;
  Supervisor supervisor(options());
  supervisor.add_builtin("fuse", [&](const SubsystemContext& context) -> std::unique_ptr<Subsystem> {
    entered.set_value();
    std::mutex mutex;
    std::unique_lock lock(mutex);
    std::condition_variable_any never;
    never.wait(lock, context.startup_stop, [] { return false; });
    return nullptr;
  });
  supervisor.start({});
  entered.get_future().wait();
  const auto started = std::chrono::steady_clock::now();
  supervisor.stop();
  EXPECT_LT(std::chrono::steady_clock::now() - started, 2s);
}

// Plugins come from the directory, each checked against this build before
// any of its code is called.
TEST(Supervisor, LoadsPluginsAndRefusesAnotherBuild) {
  Supervisor supervisor(options(DANNAN_TEST_PLUGIN_DIR));
  supervisor.start({});
  EXPECT_TRUE(eventually([&] { return status_of(supervisor, "running") && status_of(supervisor, "running")->state == SubsystemState::running; }));
  EXPECT_EQ(status_of(supervisor, "mismatched")->state, SubsystemState::disabled);
  EXPECT_FALSE(status_of(supervisor, "no_entry"));
  EXPECT_TRUE(eventually([&] { return status_of(supervisor, "declining")->state == SubsystemState::unavailable; }));
  EXPECT_TRUE(eventually([&] { return status_of(supervisor, "faulting")->state == SubsystemState::disabled; }));
  EXPECT_EQ(status_of(supervisor, "faulting")->last_fault, "cannot open its database");
  EXPECT_TRUE(eventually([&] { return status_of(supervisor, "faulting_after_start")->state == SubsystemState::disabled; }));
  EXPECT_EQ(status_of(supervisor, "faulting_after_start")->last_fault, "lost its disk");
}

TEST(Supervisor, APluginsStopMayWrite) {
  test::TempDir dir;
  const auto marker = (dir.path() / "stopped").string();
  Supervisor supervisor(options(DANNAN_TEST_PLUGIN_DIR));
  SubsystemContext context;
  context.host = marker;
  supervisor.start(context);
  EXPECT_TRUE(eventually([&] { return status_of(supervisor, "writes_on_stop")->state == SubsystemState::running; }));
  supervisor.stop();
  std::ifstream input(marker);
  std::string line;
  std::getline(input, line);
  EXPECT_EQ(line, "stopped cleanly");
}

TEST(Supervisor, AMissingPluginDirectoryLoadsNone) {
  Supervisor supervisor(options("/nonexistent/plugins"));
  supervisor.start({});
  EXPECT_TRUE(supervisor.statuses().empty());
}

// How long a run was is read from the injected time: each run here "takes"
// longer than the ceiling, so every restart waits only the initial backoff.
// Doubling instead, twenty faults would wait for hours.
TEST(SupervisedThreads, ALongRunInTheInjectedTimeEarnsAFreshBackoff) {
  test::SteppedTime time;
  SupervisedThreads threads(1ms, 1h, &contract::system_wall_clock(), &time);
  std::atomic<int> runs{0};
  std::stop_source stop;
  std::thread loop([&] {
    threads.loop("gossip", stop.get_token(), [&] {
      time.advance(2h);
      if (++runs < 20) throw std::runtime_error("peer vanished");
    });
  });
  const bool finished = eventually([&] { return runs.load() >= 20; });
  stop.request_stop();
  loop.join();
  EXPECT_TRUE(finished);
}

// A service loop reruns after a fault with backoff; a task runs once; an
// escalating thread hands its fault up. Every fault is recorded by name.
TEST(SupervisedThreads, RecordAndRestartAsTheirKindSays) {
  SupervisedThreads threads(5ms, 20ms);
  std::atomic<int> runs{0};
  std::stop_source stop;
  std::thread loop([&] {
    threads.loop("gossip", stop.get_token(), [&] {
      if (++runs < 3) throw std::runtime_error("peer vanished");
    });
  });
  loop.join();
  EXPECT_EQ(runs.load(), 3);
  threads.once("scan", [] { throw std::runtime_error("bad file"); });
  std::string escalated;
  threads.escalating("fuse-io", [] { throw std::runtime_error("EIO"); }, [&](std::string why) { escalated = why; });
  EXPECT_EQ(escalated, "EIO");
  std::map<std::string, SupervisedThreadStatus> by_name;
  for (const auto& status : threads.statuses()) by_name[status.name] = status;
  EXPECT_EQ(by_name.at("gossip").faults, 2u);
  EXPECT_EQ(by_name.at("gossip").last_fault, "peer vanished");
  EXPECT_EQ(by_name.at("gossip").last_fault_code, "exception");
  EXPECT_EQ(by_name.at("scan").faults, 1u);
  EXPECT_EQ(by_name.at("fuse-io").running, 0u);
}

TEST(SupervisedThreads, RestartDelaysDoubleToTheCeiling) {
  EXPECT_EQ(SupervisedThreads::restart_delay(1, 1s, 60s), 1s);
  EXPECT_EQ(SupervisedThreads::restart_delay(2, 1s, 60s), 2s);
  EXPECT_EQ(SupervisedThreads::restart_delay(7, 1s, 60s), 60s);
  EXPECT_EQ(SupervisedThreads::restart_delay(40, 1s, 60s), 60s);
}

// Where a hosted system publishes what it offers, by name; a faulted
// instance withdraws only itself.
TEST(Registry, PublishesByNameAndWithdrawsOnlyTheCurrent) {
  struct Torrents {
    int id;
  };
  Registry registry;
  EXPECT_FALSE(registry.find<Torrents>("torrent"));
  auto first = std::make_shared<Torrents>(Torrents{1});
  auto second = std::make_shared<Torrents>(Torrents{2});
  registry.publish("torrent", first);
  registry.publish("torrent", second);
  registry.withdraw("torrent", first.get());
  ASSERT_TRUE(registry.find<Torrents>("torrent"));
  EXPECT_EQ(registry.find<Torrents>("torrent")->id, 2);
  EXPECT_FALSE(registry.find<int>("torrent"));
  registry.withdraw("torrent", second.get());
  EXPECT_FALSE(registry.find<Torrents>("torrent"));
}

// A startup is stopped for making no progress, never for being slow.
TEST(StartupStallGate, StallsOnNoProgressNotOnSlowness) {
  using namespace std::chrono;
  const auto t0 = contract::TimePoint(seconds(100));
  StartupStallGate gate(10s, 0s, t0, 0);
  EXPECT_FALSE(gate.stalled(t0 + 9s, 0));
  EXPECT_FALSE(gate.stalled(t0 + 15s, 1));
  EXPECT_FALSE(gate.stalled(t0 + 24s, 1));
  EXPECT_TRUE(gate.stalled(t0 + 25s, 1));
  EXPECT_EQ(gate.poll_interval(), 1000ms);
  StartupStallGate ceiling(0s, 30s, t0, 0);
  EXPECT_FALSE(ceiling.stalled(t0 + 29s, 0));
  EXPECT_TRUE(ceiling.stalled(t0 + 30s, 5));
  EXPECT_EQ(StartupStallGate(2s, 0s, t0, 0).poll_interval(), 500ms);
  StartupProgress progress;
  progress.note(3);
  EXPECT_EQ(progress.value(), 3u);
}

}  // namespace
}  // namespace dannan::supervise
