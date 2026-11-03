//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/status/readiness.h"

#include <algorithm>
#include <stdexcept>

namespace dannan::status {

Readiness::Readiness(std::vector<std::string> planes, const contract::WallClock* wall) : _wall(wall) {
  contract::Lock lock(_mutex);
  for (auto& plane : planes) _planes.emplace(std::move(plane), false);
}

void Readiness::control_plane(bool online) {
  contract::Lock lock(_mutex);
  _online = online;
}

bool Readiness::control_plane_online() const {
  contract::Lock lock(_mutex);
  return _online;
}

void Readiness::ready(const std::string& plane) {
  {
    contract::Lock lock(_mutex);
    const auto found = _planes.find(plane);
    if (found == _planes.end()) throw std::invalid_argument("readiness plane " + plane + " was never declared");
    found->second = true;
    if (complete_locked() && !_ready_unix_ms) _ready_unix_ms = _wall->unix_ms();
  }
  _changed.notify_all();
}

void Readiness::fail(std::string error) {
  {
    contract::Lock lock(_mutex);
    _failed = true;
    _error = std::move(error);
  }
  _changed.notify_all();
}

bool Readiness::has(const std::string& plane) const {
  contract::Lock lock(_mutex);
  const auto found = _planes.find(plane);
  return found != _planes.end() && found->second;
}

bool Readiness::complete_locked() const {
  return std::all_of(_planes.begin(), _planes.end(), [](const auto& plane) { return plane.second; });
}

bool Readiness::complete() const {
  contract::Lock lock(_mutex);
  return complete_locked();
}

bool Readiness::failed() const {
  contract::Lock lock(_mutex);
  return _failed;
}

std::string Readiness::error() const {
  contract::Lock lock(_mutex);
  return _error;
}

uint64_t Readiness::ready_unix_ms() const {
  contract::Lock lock(_mutex);
  return _ready_unix_ms;
}

Phase Readiness::phase() const {
  contract::Lock lock(_mutex);
  if (_failed) return Phase::recovering;
  if (complete_locked() && _online) return Phase::ready;
  return _online ? Phase::recovering : Phase::starting;
}

bool Readiness::wait_complete(std::chrono::milliseconds timeout) {
  contract::Lock lock(_mutex);
  _changed.wait_for(lock.native(), timeout, [&]() DANNAN_REQUIRES(_mutex) { return _failed || complete_locked(); });
  return !_failed && complete_locked();
}

}  // namespace dannan::status
