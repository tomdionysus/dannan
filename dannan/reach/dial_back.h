//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <chrono>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>

#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"
#include "dannan/net/client.h"
#include "dannan/net/scheduler.h"
#include "dannan/status/log.h"

namespace dannan::reach {

// "Can you connect to me here?": dial_back, an address, answered by
// dial_back_reply: whether a fresh connection's handshake authenticated as
// the asker, and why not.
net::Bytes encode_dial_back_request(const net::Endpoint& endpoint);
net::Endpoint decode_dial_back_request(std::span<const uint8_t> payload);
net::Bytes encode_dial_back_reply(bool reachable, const std::string& error);
std::pair<bool, std::string> decode_dial_back_reply(std::span<const uint8_t> payload);

// Answers dial-back requests: a fresh connection to the address that must
// authenticate as the asker, never an existing route. At most one per
// asking peer per `min_interval`, so the probe cannot be used to make this
// node hammer an address. Thread-safe; answer() waits on the probe, so it
// runs on a worker.
class DialBackResponder {
 public:
  // Returns the reason the probe failed, empty on success.
  using Prober = std::function<std::string(const net::Endpoint& endpoint, const identity::NodeId& expected)>;

  DialBackResponder(std::chrono::milliseconds min_interval, Prober prober, const contract::TimeSource* time = &contract::steady_time_source(),
                    std::shared_ptr<status::LogSink> log = nullptr)
      : _min_interval(min_interval), _prober(std::move(prober)), _time(time), _log(log ? std::move(log) : status::null_sink(), "connectivity") {}

  // processed (the reply), or failed: 400 for a request without an
  // address, 429 inside the interval.
  net::Handled answer(const identity::NodeId& asker, std::span<const uint8_t> request);

 private:
  const std::chrono::milliseconds _min_interval;
  const Prober _prober;
  const contract::TimeSource* const _time;
  const status::ScopedLogger _log;
  contract::Mutex _mutex;
  std::unordered_map<identity::NodeId, contract::TimePoint, identity::NodeIdHash> _last DANNAN_GUARDED_BY(_mutex);
};

}  // namespace dannan::reach
