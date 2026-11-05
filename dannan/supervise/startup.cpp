//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/supervise/startup.h"

#include <cstdlib>
#include <utility>

namespace dannan::supervise {

StartupWatch::StartupWatch(const StartupProgress& progress, status::ScopedLogger log, Options options)
    : _progress(progress), _log(std::move(log)), _options(std::move(options)), _thread([this](std::stop_token stop) { watch(stop); }) {}

StartupWatch::~StartupWatch() { finish(); }

void StartupWatch::finish() {
  _thread.request_stop();
  if (_thread.joinable() && _thread.get_id() != std::this_thread::get_id()) _thread.join();
}

void StartupWatch::watch(std::stop_token stop) {
  const auto& time = *_options.time;
  StartupStallGate gate(_options.no_progress, _options.ceiling, time.now(), _progress.value());
  const auto ms = [](auto span) { return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(span).count()); };
  while (true) {
    {
      std::unique_lock lock(_wait_mutex);
      _wake.wait_for(lock, stop, gate.poll_interval(), [] { return false; });
    }
    if (stop.stop_requested()) return;
    const auto now = time.now();
    if (!gate.stalled(now, _progress.value())) continue;
    const auto diagnostic = _options.describe ? _options.describe() : std::string();
    _log.error("service startup stalled: no recovery progress for " + ms(now - gate.last_progress_at()) + "ms (gate " +
               std::to_string(_options.no_progress.count()) + "ms, ceiling " + std::to_string(_options.ceiling.count()) + "ms, elapsed " +
               ms(now - gate.started()) + "ms); " + diagnostic + "; terminating for restart");
    if (_options.stalled) return _options.stalled(diagnostic);
    std::_Exit(1);
  }
}

}  // namespace dannan::supervise
