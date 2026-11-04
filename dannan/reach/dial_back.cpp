//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/reach/dial_back.h"

namespace dannan::reach {

net::Bytes encode_dial_back_request(const net::Endpoint& endpoint) {
  net::Writer writer;
  writer.string(endpoint.host);
  writer.u16(endpoint.port);
  return writer.take();
}

net::Endpoint decode_dial_back_request(std::span<const uint8_t> payload) {
  net::Reader reader(payload);
  net::Endpoint out;
  out.host = reader.string(4096);
  out.port = reader.u16();
  reader.finish();
  return out;
}

net::Bytes encode_dial_back_reply(bool reachable, const std::string& error) {
  net::Writer writer;
  writer.u8(reachable ? 1 : 0);
  writer.string(error);
  return writer.take();
}

std::pair<bool, std::string> decode_dial_back_reply(std::span<const uint8_t> payload) {
  net::Reader reader(payload);
  const bool reachable = reader.u8() != 0;
  auto error = reader.string(4096);
  reader.finish();
  return {reachable, std::move(error)};
}

net::Handled DialBackResponder::answer(const identity::NodeId& asker, std::span<const uint8_t> request) {
  net::Endpoint target;
  try {
    target = decode_dial_back_request(request);
  } catch (const net::DecodeError& error) {
    return net::Handled::failed(400, "invalid_request", error.what());
  }
  if (target.host.empty() || !target.port) return net::Handled::failed(400, "invalid_request", "dial-back probe needs a host and port");
  {
    contract::Lock lock(_mutex);
    const auto now = _time->now();
    const auto last = _last.find(asker);
    if (last != _last.end() && now - last->second < _min_interval) return net::Handled::failed(429, "try_later", "dial-back probe rate limited");
    _last[asker] = now;
  }
  const auto error = _prober(target, asker);
  return net::Handled::processed({net::message::dial_back_reply, encode_dial_back_reply(error.empty(), error), {}});
}

}  // namespace dannan::reach
