//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/sched/class_gate.h"

#include <algorithm>
#include <ctime>
#include <stdexcept>

#include "dannan/sched/cpu_pacing.h"

namespace dannan::sched {

namespace {

GateConfig checked(WorkClass work, GateConfig config) {
  if (work == WorkClass::control || work == WorkClass::foreground) throw std::invalid_argument("only loader and speculative work is paced by a gate");
  if (!(config.cpu_target > 0.0 && config.cpu_target <= 1.0)) throw std::invalid_argument("a gate's cpu_target is in (0, 1]");
  if (!(config.min_scale > 0.0 && config.min_scale <= 1.0)) throw std::invalid_argument("a gate's min_scale is in (0, 1]");
  if (config.cpu_window.count() <= 0) throw std::invalid_argument("a gate's cpu_window is positive");
  return config;
}

double seconds(contract::Clock::duration duration) { return std::chrono::duration<double>(duration).count(); }

}  // namespace

double process_cpu_seconds() noexcept { return static_cast<double>(std::clock()) / CLOCKS_PER_SEC; }

ClassGate::ClassGate(WorkClass work, GateConfig config, const contract::TimeSource* time, ProcessCpu process_cpu)
    : _work(work),
      _config(checked(work, config)),
      _time(time),
      _process_cpu(process_cpu ? std::move(process_cpu) : ProcessCpu(process_cpu_seconds)),
      _share(_config.higher_weight, _config.own_weight, _config.slice) {}

void ClassGate::set_contended(std::function<bool()> reading) { _hosted = std::move(reading); }

bool ClassGate::contended() const { return _higher.load(std::memory_order_relaxed) > 0 || (_hosted && _hosted()); }

void ClassGate::note_higher(int delta) noexcept { _higher.fetch_add(delta, std::memory_order_relaxed); }

bool ClassGate::may_start() {
  const auto now = _time->now();
  {
    contract::Lock lock(_mutex);
    if (now < _cpu_rest_until) return false;
  }
  return _share.may_start(now, contended());
}

void ClassGate::started(bool serving) { _share.started(_time->now(), contended(), serving); }

void ClassGate::serving() { _share.serving(_time->now(), contended()); }

bool ClassGate::should_yield() { return _share.should_yield(_time->now(), contended()); }

std::chrono::milliseconds ClassGate::finished() { return _share.finished(_time->now(), contended()); }

std::chrono::milliseconds ClassGate::rest() {
  const auto now = _time->now();
  auto out = _share.wait(now, contended());
  contract::Lock lock(_mutex);
  if (now < _cpu_rest_until) out = std::max(out, std::chrono::ceil<std::chrono::milliseconds>(_cpu_rest_until - now));
  return out;
}

double ClassGate::scale_locked(contract::TimePoint now) {
  if (!_sampled) {
    _sampled = true;
    _sampled_at = now;
    _process_at = _process_cpu();
    _own_at = _own;
    _scale = 1.0;
    return _scale;
  }
  if (now - _sampled_at < _config.cpu_window) return _scale;
  const double process = _process_cpu();
  const double raw = background_cpu_scale(process - _process_at, _own - _own_at, seconds(now - _sampled_at), _config.cpu_target);
  _scale = std::max(_config.min_scale, raw);
  _sampled_at = now;
  _process_at = process;
  _own_at = _own;
  return _scale;
}

double ClassGate::cpu_scale() {
  contract::Lock lock(_mutex);
  return scale_locked(_time->now());
}

void ClassGate::charge(double cpu_seconds) {
  contract::Lock lock(_mutex);
  _own += std::max(0.0, cpu_seconds);
}

std::chrono::milliseconds ClassGate::charge_and_rest(double cpu_seconds) {
  const auto now = _time->now();
  contract::Lock lock(_mutex);
  const double used = std::max(0.0, cpu_seconds);
  _own += used;
  const double scale = scale_locked(now);
  if (scale >= 1.0 || used <= 0.0) return {};
  const auto rest = std::chrono::round<std::chrono::milliseconds>(std::chrono::duration<double>(used * (1.0 / scale - 1.0)));
  if (rest.count() <= 0) return {};
  _cpu_rest_until = std::max(_cpu_rest_until, now) + rest;
  return rest;
}

ClassGates::ClassGates(GateConfig loader, GateConfig speculative, const contract::TimeSource* time, ProcessCpu process_cpu)
    : _loader(WorkClass::loader, loader, time, process_cpu), _speculative(WorkClass::speculative, speculative, time, process_cpu) {}

ClassGate& ClassGates::gate(WorkClass work) {
  if (work == WorkClass::loader) return _loader;
  if (work == WorkClass::speculative) return _speculative;
  throw std::invalid_argument("only loader and speculative work has a gate");
}

void ClassGates::note(WorkClass work, int delta) noexcept {
  // What each class yields to, as Macha's maintenance reads it: loader yields to
  // foreground, speculative to foreground and loader. Control is the node's own
  // and never paces anything.
  if (work == WorkClass::foreground) {
    _loader.note_higher(delta);
    _speculative.note_higher(delta);
  } else if (work == WorkClass::loader) {
    _speculative.note_higher(delta);
  }
}

}  // namespace dannan::sched
