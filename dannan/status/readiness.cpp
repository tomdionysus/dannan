//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/status/readiness.h"

#include <algorithm>
#include <boost/asio/post.hpp>
#include <stdexcept>

namespace dannan::status {

const char* health_name(Health health) noexcept {
  switch (health) {
    case Health::ready:
      return "ready";
    case Health::degraded:
      return "degraded";
    case Health::draining:
      return "draining";
    case Health::not_ready:
      return "not_ready";
  }
  return "unknown";
}

Health health_of(const membership::NodeRecord& node) {
  const auto advert = node.advert(health_advert);
  if (!advert || advert->size() != 1) return Health::ready;
  const auto value = (*advert)[0];
  if (value < static_cast<uint8_t>(Health::degraded) || value > static_cast<uint8_t>(Health::not_ready)) return Health::ready;
  return static_cast<Health>(value);
}

void set_health_advert(membership::NodeRecord& node, Health health) {
  if (health == Health::ready)
    node.adverts.erase(health_advert);
  else
    node.adverts[health_advert] = net::Bytes{static_cast<uint8_t>(health)};
}

namespace {
constexpr const char* drain_check = "dannan.drain";
}

Readiness::Readiness(std::vector<std::string> planes, const contract::WallClock* wall) : _wall(wall), _started_unix_ms(wall->unix_ms()) {
  contract::Lock lock(_mutex);
  for (auto& plane : planes) _planes.emplace(std::move(plane), false);
}

void Readiness::declare(const std::string& plane) {
  if (plane.empty()) throw std::invalid_argument("a readiness plane has a name");
  contract::Lock lock(_mutex);
  if (_closed) throw std::logic_error("readiness plane " + plane + " declared after the node started");
  _planes.emplace(plane, false);
}

void Readiness::hold_until_marked() {
  contract::Lock lock(_mutex);
  if (_closed) throw std::logic_error("readiness held after the node started");
  _held = true;
}

void Readiness::mark_complete() {
  {
    contract::Lock lock(_mutex);
    _marked = true;
    if (complete_locked() && !_ready_unix_ms) _ready_unix_ms = _wall->unix_ms();
    changed_locked();
  }
  _changed.notify_all();
}

void Readiness::close_declarations() {
  contract::Lock lock(_mutex);
  _closed = true;
}

void Readiness::control_plane(bool online) {
  contract::Lock lock(_mutex);
  _online = online;
  changed_locked();
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
    changed_locked();
  }
  _changed.notify_all();
}

void Readiness::fail(std::string error) {
  {
    contract::Lock lock(_mutex);
    if (!_failed) _error = std::move(error);
    _failed = true;
    changed_locked();
  }
  _changed.notify_all();
}

bool Readiness::has(const std::string& plane) const {
  contract::Lock lock(_mutex);
  const auto found = _planes.find(plane);
  return found != _planes.end() && found->second;
}

bool Readiness::complete_locked() const {
  return (!_held || _marked) && std::all_of(_planes.begin(), _planes.end(), [](const auto& plane) { return plane.second; });
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

Health Readiness::health_locked(uint64_t now) const {
  if (_failed || !_online || !complete_locked()) return Health::not_ready;
  auto out = Health::ready;
  for (const auto& [_, check] : _checks) {
    const auto health = check.valid_until_unix_ms && now >= check.valid_until_unix_ms ? Health::not_ready : check.health;
    out = std::max(out, health);
  }
  return out;
}

void Readiness::changed_locked() {
  const auto now = health_locked(_wall->unix_ms());
  if (now == _told) return;
  _told = now;
  for (const auto& [_, watcher] : _watchers) boost::asio::post(watcher.executor, [handler = watcher.handler, now] { handler(now); });
}

void Readiness::set_locked(const std::string& check, Health health, std::string reason, uint64_t valid_until) {
  const auto now = _wall->unix_ms();
  auto [found, made] = _checks.try_emplace(check);
  auto& entry = found->second;
  // An expired verdict was not_ready: set again, it changes from that.
  const bool expired = !made && entry.valid_until_unix_ms && now >= entry.valid_until_unix_ms;
  if (made || expired || entry.health != health) entry.since_unix_ms = now;
  entry.health = health;
  entry.reason = health == Health::ready ? std::string() : std::move(reason);
  entry.valid_until_unix_ms = valid_until;
  changed_locked();
}

void Readiness::set(const std::string& check, Health health, std::string reason, std::optional<std::chrono::milliseconds> valid_for) {
  if (check.empty()) throw std::invalid_argument("a health check has a name");
  contract::Lock lock(_mutex);
  const auto until = valid_for ? _wall->unix_ms() + static_cast<uint64_t>(std::max<int64_t>(valid_for->count(), 0)) : 0;
  set_locked(check, health, std::move(reason), until);
}

void Readiness::clear(const std::string& check) {
  contract::Lock lock(_mutex);
  if (_checks.erase(check)) changed_locked();
}

void Readiness::drain(bool on, std::string reason) {
  contract::Lock lock(_mutex);
  if (on)
    set_locked(drain_check, Health::draining, std::move(reason), 0);
  else if (_checks.erase(drain_check))
    changed_locked();
}

Health Readiness::health() const {
  contract::Lock lock(_mutex);
  return health_locked(_wall->unix_ms());
}

std::vector<CheckState> Readiness::checks() const {
  contract::Lock lock(_mutex);
  const auto now = _wall->unix_ms();
  std::vector<CheckState> out;
  out.reserve(_checks.size());
  for (const auto& [name, check] : _checks) {
    if (check.valid_until_unix_ms && now >= check.valid_until_unix_ms)
      out.push_back(CheckState{name, Health::not_ready, "stopped reporting: not set again within its valid time", check.valid_until_unix_ms});
    else
      out.push_back(CheckState{name, check.health, check.reason, check.since_unix_ms});
  }
  return out;
}

uint64_t Readiness::on_change(boost::asio::any_io_executor executor, std::function<void(Health)> handler) {
  contract::Lock lock(_mutex);
  const auto id = _next_watcher++;
  _watchers.emplace(id, Watcher{std::move(executor), std::move(handler)});
  return id;
}

void Readiness::cancel(uint64_t id) {
  contract::Lock lock(_mutex);
  _watchers.erase(id);
}

void Readiness::evaluate() {
  contract::Lock lock(_mutex);
  changed_locked();
}

}  // namespace dannan::status
