//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <atomic>
#include <boost/asio/any_io_executor.hpp>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "dannan/agree/caspaxos.h"
#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"
#include "dannan/net/codec.h"
#include "dannan/net/message.h"

// Leaderless per-key agreement over the cluster (TODO/2026-11-15-agreement-design.md):
// the runtime around caspaxos.h's rules. A hosted system serves its space's acceptors with
// a store of its own, and changes registers from any node; Dannan stores nothing.
namespace dannan::agree {

// One space's acceptor state, the hosted system's to keep: in memory, or durably before
// keep() says it is done. Called one request at a time per space; done may be called from
// any thread.
class AcceptorStore {
 public:
  virtual ~AcceptorStore() = default;
  // The state held for `names`, one per name in order; a name never seen is a default
  // AcceptorState.
  virtual void load(std::span<const Name> names, std::function<void(std::vector<AcceptorState>)> done) = 0;
  // Keeps `states` for `names`; done(false) if it could not, and the request is refused.
  virtual void keep(std::span<const Name> names, std::span<const AcceptorState> states, std::function<void(bool)> done) = 0;
  // Whether this acceptor may vote: false after a restart that lost its state, until it
  // has caught up (the design's rule; a store that votes with lost state can commit two
  // values for one name).
  virtual bool ready() const = 0;
};

// What a change did, and who said what: never a boolean.
struct Outcome {
  bool committed = false;
  // Committed: one value per name, the ballot that last altered each (a fencing token),
  // and the change's result from the attempt that committed.
  std::vector<std::optional<Bytes>> values;
  std::vector<Ballot> changed_at;
  Bytes result;
  Ballot ballot;
  std::vector<identity::NodeId> accepted, refused, silent;
  // Not committed: why. A set refusal names the set the names are served by.
  std::string why;
  std::optional<ReplicaSet> serving;
  // Attempts made, retries included.
  uint32_t attempts = 0;
};

class Agreement {
 public:
  // The way to call a peer: the reply's payload, or nothing and why. Told once.
  using Call = std::function<void(const identity::NodeId& to, net::MessageType type, net::Bytes payload, std::chrono::milliseconds deadline,
                                  std::function<void(std::optional<net::Bytes>, std::string)> done)>;
  // Whether a node advertises `capability` (a space's owner's).
  using Advertises = std::function<bool(const identity::NodeId& node, const std::string& capability)>;
  // Runs `task` once `after` has passed on the injected time: retries' backoff and the
  // deadline. Empty: a timer on `executor` reading `time`. The simulation harness gives its
  // own, so a run's every wait is an event in its world.
  using Schedule = std::function<void(std::chrono::milliseconds after, std::function<void()> task)>;

  // `executor` runs the proposers' and acceptors' steps; `epoch` is the node's durability
  // epoch, so a restart never reuses a ballot.
  Agreement(boost::asio::any_io_executor executor, identity::NodeId self, identity::NodeId epoch, Call call, Advertises advertises,
            const contract::TimeSource* time = &contract::steady_time_source(), Schedule schedule = {});
  ~Agreement();
  Agreement(const Agreement&) = delete;
  Agreement& operator=(const Agreement&) = delete;

  // Serves `space`'s acceptors on this node from `store`, to proposers advertising
  // `capability` (Lugh's review: only the owner's nodes vote or write). Throws
  // std::invalid_argument for an empty or overlong space, or one already served.
  void serve(std::string space, std::string capability, std::shared_ptr<AcceptorStore> store);
  void unserve(const std::string& space);

  // Changes `names` together over `replicas` (version `version` of their configuration):
  // `change` is given their current values and returns the new ones and a result. Retries
  // above any higher ballot it hears of, with backoff, until committed, refused for its
  // set, or `deadline` passes on the injected time. `done` is posted to `executor` once.
  // From any thread.
  void change(std::string space, std::vector<Name> names, std::vector<identity::NodeId> replicas, uint64_t version, Change change, Reduce reduce,
              std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, std::function<void(Outcome)> done);
  // A strict read: an identity change, so it returns only what a majority holds (Mimir's
  // ruling 5); a minority side is refused.
  void read(std::string space, std::vector<Name> names, std::vector<identity::NodeId> replicas, uint64_t version, Reduce reduce,
            std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, std::function<void(Outcome)> done);

  // A peer's agree_prepare or agree_accept: answered through `answer`, later. Returns the
  // message to answer at once when it cannot be served (an unknown space, a refusal before
  // the store is asked), or nothing when the answer will be given.
  std::optional<net::Message> serve_request(const identity::NodeId& from, const net::Message& request, std::function<void(net::Message)> answer);

 private:
  struct Core;
  struct Run;

  // Shared with every change in flight, so none outlives what it uses.
  std::shared_ptr<Core> _core;
};

}  // namespace dannan::agree
