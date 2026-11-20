//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/ip/address.hpp>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "dannan/listen/connector.h"
#include "dannan/listen/listener.h"
#include "dannan/listen/memory.h"
#include "dannan/test/fixtures/sim/nodes.h"

// Streams inside the world (TODO/2026-11-20-stack-plan.md, item 17; Lir's step 2): a hosted
// system's listeners on SimNodes' nodes, reached by address with no socket. A listener is added
// under the endpoint it would advertise; a connector given dial() reaches it there as over TCP:
// refused at once when the node is down or nothing listens, never answered (cut by the connector's
// deadline on the world's clock) when the dialler's node is cut off from the listener's. Stopping a
// node severs every stream it held. Single-threaded, as the world is.
namespace dannan::test::sim {

class StreamDirectory {
 public:
  explicit StreamDirectory(SimNodes& nodes) : _nodes(nodes), _self(std::make_shared<StreamDirectory*>(this)) {
    // Held weakly: a test declares the directory after its nodes, so it goes first, and the
    // nodes' own stop at their end must not reach it.
    _nodes.stopping.push_back([weak = std::weak_ptr<StreamDirectory*>(_self)](size_t node) {
      if (const auto self = weak.lock()) (*self)->node_stopping(node);
    });
  }
  StreamDirectory(const StreamDirectory&) = delete;
  StreamDirectory& operator=(const StreamDirectory&) = delete;

  // `listener`, run by node `node`, reachable at `host`:`port` until removed or the node stops.
  void add(size_t node, const std::string& host, uint16_t port, listen::StreamListener& listener) { _listeners[{host, port}] = Entry{node, &listener}; }
  void remove(const std::string& host, uint16_t port) { _listeners.erase({host, port}); }

  // ConnectorConfig::dial for a client on node `from`, or one outside every node (none), whose
  // streams report `address` as their peer.
  listen::ConnectorConfig::Dial dial(std::optional<size_t> from, boost::asio::ip::address address = boost::asio::ip::make_address("127.0.0.1")) {
    return [this, from, address](const std::string& host, uint16_t port, boost::asio::any_io_executor executor,
                                 std::function<void(std::unique_ptr<listen::Carrier>, std::string)> done) {
      const auto found = _listeners.find({host, port});
      if (found == _listeners.end() || !_nodes.up(found->second.node)) return done(nullptr, "connection refused");
      const auto to = found->second.node;
      // Cut off: the connect is never answered, as a SYN into a partition is not.
      if (from && *from != to && (_nodes.network().blocked(*from, to) || _nodes.network().blocked(to, *from))) return;
      listen::MemoryOptions options;
      options.client_address = {address, _next_port++};
      boost::system::error_code literal;
      const auto server = boost::asio::ip::make_address(host, literal);
      options.server_address = {literal ? boost::asio::ip::make_address("127.0.0.1") : server, port};
      try {
        auto injected = found->second.listener->inject(std::move(executor), options);
        _links[to].push_back(injected.link);
        if (from) _links[*from].push_back(injected.link);
        done(std::move(injected.client), {});
      } catch (const std::exception& error) {
        done(nullptr, error.what());
      }
    };
  }

  // Every link made to or from node `node`, for a test's faults (sever, stall, resume).
  const std::vector<std::shared_ptr<listen::MemoryLink>>& links(size_t node) { return _links[node]; }

 private:
  struct Entry {
    size_t node = 0;
    listen::StreamListener* listener = nullptr;
  };

  void node_stopping(size_t node) {
    for (const auto& link : _links[node]) link->sever();
    _links[node].clear();
    for (auto it = _listeners.begin(); it != _listeners.end();) it = it->second.node == node ? _listeners.erase(it) : std::next(it);
  }

  SimNodes& _nodes;
  std::map<std::pair<std::string, uint16_t>, Entry> _listeners;
  std::map<size_t, std::vector<std::shared_ptr<listen::MemoryLink>>> _links;
  uint16_t _next_port = 40000;
  // What the stop hook holds weakly.
  std::shared_ptr<StreamDirectory*> _self;
};

}  // namespace dannan::test::sim
