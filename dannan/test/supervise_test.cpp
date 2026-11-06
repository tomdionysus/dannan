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

#include <filesystem>
#include <fstream>
#include <future>
#include <regex>
#include <thread>

#include "dannan/status/observation.h"
#include "dannan/supervise/registry.h"
#include "dannan/supervise/startup.h"
#include "dannan/supervise/supervised.h"
#include "dannan/supervise/supervisor.h"
#include "dannan/test/fixtures/recording_sink.h"
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

// A startup that stops progressing is ended, with Macha's line; one that keeps
// ticking is left alone however long it takes.
TEST(StartupWatch, AStalledStartupSaysSoAndIsEnded) {
  test::SteppedTime time;
  auto log = std::make_shared<test::RecordingSink>();
  StartupProgress progress;
  std::promise<std::string> stalled;
  StartupWatch::Options watch;
  watch.no_progress = 4ms;
  watch.describe = [] { return std::string("waiting for metadata"); };
  watch.stalled = [&](const std::string& diagnostic) { stalled.set_value(diagnostic); };
  watch.time = &time;
  StartupWatch watcher(progress, status::ScopedLogger(log, "startup"), watch);
  auto future = stalled.get_future();
  // Ticks, however late, are progress.
  for (int i = 0; i < 20; ++i) {
    progress.note();
    time.advance(3ms);
    std::this_thread::sleep_for(2ms);
  }
  EXPECT_NE(future.wait_for(0ms), std::future_status::ready);
  EXPECT_TRUE(eventually([&] {
    time.advance(1h);
    return future.wait_for(0ms) == std::future_status::ready;
  }));
  EXPECT_EQ(future.get(), "waiting for metadata");
  EXPECT_EQ(log->count(status::LogLevel::error, "service startup stalled: no recovery progress for "), 1u);
  EXPECT_EQ(log->count(status::LogLevel::error, "waiting for metadata; terminating for restart"), 1u);
}

TEST(StartupWatch, AFinishedStartupEndsTheWatchWithoutAWord) {
  test::SteppedTime time;
  auto log = std::make_shared<test::RecordingSink>();
  StartupProgress progress;
  std::atomic<bool> stalled{false};
  StartupWatch::Options watch;
  watch.no_progress = 1h;
  watch.stalled = [&](const std::string&) { stalled = true; };
  watch.time = &time;
  {
    StartupWatch watcher(progress, status::ScopedLogger(log, "startup"), watch);
    watcher.finish();
    time.advance(2h);
  }
  EXPECT_FALSE(stalled.load());
}

// The journal, in Macha's words at Macha's levels.
TEST(SupervisedThreads, AFaultIsJournalledWithWhatHappensNext) {
  auto log = std::make_shared<test::RecordingSink>();
  SupervisedThreads threads(5ms, 20ms, &contract::system_wall_clock(), &contract::steady_time_source(), log);
  threads.once("scan", [] { throw std::runtime_error("bad file"); });
  EXPECT_EQ(log->count(status::LogLevel::error, "thread 'scan' faulted (exception): bad file; thread ended"), 1u);
  threads.escalating("fuse-io", [] { throw 7; }, nullptr);
  EXPECT_EQ(log->count(status::LogLevel::error, "thread 'fuse-io' faulted (unknown_exception): unknown exception; escalated to its subsystem's supervisor"),
            1u);
  std::stop_source stop;
  std::atomic<int> runs{0};
  threads.loop("gossip", stop.get_token(), [&] {
    if (++runs < 2) throw std::runtime_error("peer vanished");
  });
  EXPECT_EQ(log->count(status::LogLevel::error, "thread 'gossip' faulted (exception): peer vanished; restarting in 5 ms"), 1u);
  std::stop_source stopped;
  threads.loop("leaving", stopped.get_token(), [&] {
    stopped.request_stop();
    throw std::runtime_error("on the way out");
  });
  EXPECT_EQ(log->count(status::LogLevel::error, "thread 'leaving' faulted (exception): on the way out; stopping, not restarted"), 1u);
}

TEST(Supervisor, TheSubsystemsAreJournalled) {
  auto log = std::make_shared<test::RecordingSink>();
  std::atomic<int> stops{0};
  auto settings = options();
  settings.log = log;
  {
    Supervisor supervisor(settings);
    supervisor.add_builtin("fuse", [&](const SubsystemContext&) { return std::make_unique<Builtin>("fuse", &stops, nullptr); });
    supervisor.add_builtin("torrent", [&](const SubsystemContext&) -> std::unique_ptr<Subsystem> { return nullptr; });
    supervisor.add_builtin("scanner", [&](const SubsystemContext&) -> std::unique_ptr<Subsystem> { throw std::runtime_error("no catalogue"); });
    supervisor.start({});
    EXPECT_TRUE(eventually([&] { return status_of(supervisor, "scanner")->state == SubsystemState::disabled; }));
    EXPECT_TRUE(eventually([&] { return log->count(status::LogLevel::info, "subsystem plugin 'fuse' loaded and running path=built in") == 1; }));
    EXPECT_TRUE(eventually([&] {
      return log->count(status::LogLevel::info, "subsystem plugin 'torrent' loaded but its capability is not enabled on this node path=built in") == 1;
    }));
    EXPECT_EQ(log->count(status::LogLevel::error, "subsystem 'scanner' failed to start: no catalogue"), 4u);
    EXPECT_EQ(log->count(status::LogLevel::error, "subsystem 'scanner' disabled after 4 failed attempts; needs an operator"), 1u);
    supervisor.stop();
  }
}

TEST(Supervisor, AStopThatThrowsIsJournalled) {
  struct Throwing final : Subsystem {
    std::string_view name() const noexcept override { return "bad"; }
    void attach_fault_sink(FaultSink) override {}
    void start() override {}
    void stop() override { throw std::runtime_error("stuck"); }
  };
  auto log = std::make_shared<test::RecordingSink>();
  auto settings = options();
  settings.log = log;
  Supervisor supervisor(settings);
  supervisor.add_builtin("bad", [](const SubsystemContext&) { return std::make_unique<Throwing>(); });
  supervisor.start({});
  EXPECT_TRUE(eventually([&] { return status_of(supervisor, "bad")->state == SubsystemState::running; }));
  supervisor.stop();
  EXPECT_EQ(log->count(status::LogLevel::warn, "subsystem 'bad' stop() threw: stuck"), 1u);
}

TEST(Supervisor, AnEmptyDirectoryLoadsNone) {
  test::TempDir empty;
  Supervisor supervisor(options(empty.path()));
  supervisor.start({});
  EXPECT_TRUE(supervisor.statuses().empty());
  supervisor.stop();
}

TEST(Supervisor, StatusesAreEmptyAfterStop) {
  std::atomic<int> stops{0};
  Supervisor supervisor(options());
  supervisor.add_builtin("fuse", [&](const SubsystemContext&) { return std::make_unique<Builtin>("fuse", &stops, nullptr); });
  supervisor.start({});
  EXPECT_TRUE(eventually([&] { return !supervisor.statuses().empty() && supervisor.statuses().front().state == SubsystemState::running; }));
  supervisor.stop();
  EXPECT_TRUE(supervisor.statuses().empty());
  supervisor.stop();
}

// Its fault sink points into the entry, so an instance is gone before its replacement is built.
TEST(Supervisor, AFaultedInstanceGoesBeforeItsReplacement) {
  struct Counted final : Subsystem {
    std::atomic<int>& alive;
    FaultSink sink;
    const bool fault;
    Counted(std::atomic<int>& alive, bool fault) : alive(alive), fault(fault) { ++alive; }
    ~Counted() override { --alive; }
    std::string_view name() const noexcept override { return "fuse"; }
    void attach_fault_sink(FaultSink s) override { sink = std::move(s); }
    void start() override {
      if (fault) sink("gone");
    }
    void stop() override {}
  };
  std::atomic<int> alive{0};
  std::atomic<int> built{0};
  std::atomic<int> most_alive_when_built{0};
  Supervisor supervisor(options());
  supervisor.add_builtin("fuse", [&](const SubsystemContext&) {
    most_alive_when_built = std::max(most_alive_when_built.load(), alive.load());
    return std::make_unique<Counted>(alive, ++built == 1);
  });
  supervisor.start({});
  EXPECT_TRUE(
      eventually([&] { return built.load() == 2 && status_of(supervisor, "fuse") && status_of(supervisor, "fuse")->state == SubsystemState::running; }));
  EXPECT_EQ(most_alive_when_built.load(), 0);
  supervisor.stop();
  EXPECT_EQ(alive.load(), 0);
}

// A throw that is not a std::exception, out of the recorder's window, is recorded by its
// supervised loop; it does not end the process.
TEST(SupervisedThreads, ANonStandardThrowFromTheRecorderIsRecorded) {
  test::TempDir dir;
  SupervisedThreads threads(1ms, 5ms);
  status::Observations registry;
  auto log = std::make_shared<status::ObservationLog>(dir.path() / "observations.jsonl", 1 << 20);
  status::ObservationRecorder recorder(
      registry, log, "v", 1ms, []() -> std::map<std::string, uint64_t> { throw 42; }, &contract::system_wall_clock(), &threads);
  recorder.start();
  EXPECT_TRUE(eventually([&] {
    for (const auto& status : threads.statuses())
      if (status.name == "observation-recorder" && status.last_fault_code == "unknown_exception") return true;
    return false;
  }));
  // The stop's own final window throws too; it reaches the caller, not std::terminate.
  try {
    recorder.stop();
  } catch (...) {
  }
}

// A thread escaping its body ends the process: every thread in the library starts
// under supervision, as Macha's source guard demands of its own tree.
TEST(SupervisedThreads, EveryThreadIsStartedSupervised) {
  const std::regex site(R"(std::jthread\(|std::thread\(|_thread\(\[|emplace_back\(\s*\[.*stop_token)");
  std::vector<std::string> unguarded;
  size_t sites = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(DANNAN_SOURCE_DIR)) {
    const auto path = entry.path().generic_string();
    if (entry.path().extension() != ".cpp" || path.find("/test/") != std::string::npos || path.find("/fuzz/") != std::string::npos) continue;
    if (entry.path().filename() == "supervised.cpp") continue;
    std::ifstream file(entry.path());
    std::vector<std::string> lines;
    for (std::string line; std::getline(file, line);) lines.push_back(line);
    for (size_t i = 0; i < lines.size(); ++i) {
      // A formatter may break a call after its opening bracket.
      const auto joined = lines[i] + (i + 1 < lines.size() ? lines[i + 1] : std::string());
      if (!std::regex_search(joined, site) || std::regex_search(i > 0 ? lines[i - 1] : std::string(), std::regex(R"(emplace_back\($)"))) continue;
      ++sites;
      bool guarded = false;
      for (size_t j = i; j < lines.size() && j < i + 3; ++j)
        guarded = guarded || lines[j].find("supervised_loop") != std::string::npos || lines[j].find("_threads.") != std::string::npos;
      if (!guarded) unguarded.push_back(path + ":" + std::to_string(i + 1));
    }
  }
  EXPECT_GE(sites, 11u);
  for (const auto& where : unguarded) ADD_FAILURE() << "a thread not started supervised: " << where;
}

// A loop waiting out its backoff stops when asked, and a throw that is no
// std::exception is recorded too.
TEST(SupervisedThreads, ALoopInBackoffStopsWhenAsked) {
  SupervisedThreads threads(1h, 2h);
  std::stop_source stop;
  std::atomic<int> runs{0};
  std::thread loop([&] {
    threads.loop("gossip", stop.get_token(), [&] {
      ++runs;
      throw 7;
    });
  });
  ASSERT_TRUE(eventually([&] { return !threads.statuses().empty() && threads.statuses().front().restarting == 1; }));
  stop.request_stop();
  loop.join();
  EXPECT_EQ(runs.load(), 1);
  EXPECT_EQ(threads.statuses().front().last_fault_code, "unknown_exception");
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
  EXPECT_EQ(escalated, "thread 'fuse-io' faulted: EIO");
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

// The plugin lines, level and words (first cutover, section 5).
TEST(Supervisor, ThePluginsThatCannotRunAreJournalled) {
  auto log = std::make_shared<test::RecordingSink>();
  auto settings = options(DANNAN_TEST_PLUGIN_DIR);
  settings.log = log;
  settings.system_name = "macha";
  {
    Supervisor supervisor(settings);
    supervisor.start({});
    EXPECT_TRUE(eventually([&] { return status_of(supervisor, "running") && status_of(supervisor, "running")->state == SubsystemState::running; }));
    supervisor.stop();
  }
  EXPECT_EQ(log->count(status::LogLevel::debug,
                       "subsystem plugin candidate 'no_entry' has no entry symbol 'dannan_test_subsystem_entry', skipping as not a macha plugin: "),
            1u);
  EXPECT_EQ(log->count(status::LogLevel::error, "subsystem plugin 'null_entry' entry function returned null"), 1u);
  EXPECT_EQ(
      log->count(status::LogLevel::error,
                 "subsystem plugin 'mismatched' build identity mismatch: plugin=some other build core=dannan test build; refusing to load (partial deploy?)"),
      1u);
}

TEST(Supervisor, ADirectoryThatCannotBeListedIsWarnedOf) {
  test::TempDir dir;
  const auto locked = dir.path() / "locked";
  std::filesystem::create_directories(locked);
  std::filesystem::permissions(locked, std::filesystem::perms::none);
  auto log = std::make_shared<test::RecordingSink>();
  auto settings = options(locked);
  settings.log = log;
  {
    Supervisor supervisor(settings);
    supervisor.start({});
    supervisor.stop();
  }
  std::filesystem::permissions(locked, std::filesystem::perms::owner_all);
  EXPECT_EQ(log->count(status::LogLevel::warn, "subsystem plugin discovery failed dir=" + locked.string() + " error="), 1u);
}

TEST(Supervisor, AStopThatThrowsAnythingElseIsJournalled) {
  struct Throwing final : Subsystem {
    std::string_view name() const noexcept override { return "bad"; }
    void attach_fault_sink(FaultSink) override {}
    void start() override {}
    void stop() override { throw 7; }
  };
  auto log = std::make_shared<test::RecordingSink>();
  auto settings = options();
  settings.log = log;
  Supervisor supervisor(settings);
  supervisor.add_builtin("bad", [](const SubsystemContext&) { return std::make_unique<Throwing>(); });
  supervisor.start({});
  EXPECT_TRUE(eventually([&] { return status_of(supervisor, "bad")->state == SubsystemState::running; }));
  supervisor.stop();
  EXPECT_EQ(log->count(status::LogLevel::warn, "subsystem 'bad' stop() threw an unknown exception"), 1u);
}

TEST(Supervisor, AFaultWhileRunningIsJournalled) {
  auto log = std::make_shared<test::RecordingSink>();
  std::atomic<int> stops{0};
  std::atomic<int> built{0};
  auto settings = options();
  settings.log = log;
  Supervisor supervisor(settings);
  supervisor.add_builtin("fuse", [&](const SubsystemContext&) {
    const int n = ++built;
    return std::make_unique<Builtin>("fuse", &stops, [n](Subsystem::FaultSink& sink) {
      if (n == 1) sink("mount went away");
    });
  });
  supervisor.start({});
  EXPECT_TRUE(eventually([&] { return built.load() == 2 && status_of(supervisor, "fuse")->state == SubsystemState::running; }));
  EXPECT_EQ(log->count(status::LogLevel::error, "subsystem 'fuse' faulted while running: mount went away"), 1u);
  supervisor.stop();
}

// A sink kept past the stop is ignored: first reason wins, and nothing is left
// to rebuild.
TEST(Supervisor, AFaultAfterStoppingIsIgnored) {
  auto log = std::make_shared<test::RecordingSink>();
  std::atomic<int> stops{0};
  std::atomic<int> built{0};
  Subsystem::FaultSink kept;
  auto settings = options();
  settings.log = log;
  Supervisor supervisor(settings);
  supervisor.add_builtin("fuse", [&](const SubsystemContext&) {
    ++built;
    return std::make_unique<Builtin>("fuse", &stops, [&kept](Subsystem::FaultSink& sink) { kept = sink; });
  });
  supervisor.start({});
  EXPECT_TRUE(eventually([&] { return status_of(supervisor, "fuse") && status_of(supervisor, "fuse")->state == SubsystemState::running; }));
  supervisor.stop();
  kept("too late");
  EXPECT_EQ(built.load(), 1);
  EXPECT_EQ(log->count(status::LogLevel::error, "too late"), 0u);
}

// When it faulted, for `threads` in Status.
TEST(SupervisedThreads, AFaultSaysWhenItHappened) {
  SupervisedThreads threads(5ms, 20ms);
  threads.once("scan", [] { throw std::runtime_error("bad file"); });
  const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
  const auto statuses = threads.statuses();
  ASSERT_EQ(statuses.size(), 1u);
  EXPECT_GT(statuses.front().last_fault_unix_ms, now - 60000);
  EXPECT_LE(statuses.front().last_fault_unix_ms, now + 1000);
}

}  // namespace
}  // namespace dannan::supervise
