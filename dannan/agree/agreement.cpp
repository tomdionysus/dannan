//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/agree/agreement.h"

#include <algorithm>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <deque>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>

#include "dannan/agree/wire.h"
#include "dannan/net/routes.h"

namespace dannan::agree {

namespace asio = boost::asio;
using Reply = std::function<void(std::optional<net::Bytes>, std::string)>;

// One space served on this node: its store, the capability its proposers advertise,
// and its requests one at a time (load, the rules, keep, answer).
struct Space {
  std::string capability;
  std::shared_ptr<AcceptorStore> store;
  asio::any_io_executor executor;
  contract::Mutex mutex;
  std::deque<std::function<void(std::function<void()>)>> queue DANNAN_GUARDED_BY(mutex);
  bool busy DANNAN_GUARDED_BY(mutex) = false;

  void enqueue(std::function<void(std::function<void()>)> task, const std::shared_ptr<Space>& self) {
    {
      contract::Lock lock(mutex);
      queue.push_back(std::move(task));
      if (busy) return;
      busy = true;
    }
    next(self);
  }
  // Each step posted: a store that answers at once never recurses.
  static void next(const std::shared_ptr<Space>& self) {
    std::function<void(std::function<void()>)> task;
    {
      contract::Lock lock(self->mutex);
      if (self->queue.empty()) {
        self->busy = false;
        return;
      }
      task = std::move(self->queue.front());
      self->queue.pop_front();
    }
    task([self] { asio::post(self->executor, [self] { next(self); }); });
  }
};

struct Agreement::Core : std::enable_shared_from_this<Core> {
  asio::any_io_executor executor;
  identity::NodeId self;
  identity::NodeId epoch;
  Call call;
  Advertises advertises;
  const contract::TimeSource* time;
  Schedule schedule;
  std::atomic<uint64_t> counter{0};
  // Each change's own, so two on one node never share a backoff sequence.
  std::atomic<uint64_t> runs{0};
  mutable contract::Mutex mutex;
  std::map<std::string, std::shared_ptr<Space>> spaces DANNAN_GUARDED_BY(mutex);
  // Set as the owner goes: `call` reaches into the node, so no change still in flight
  // may use it after. Held shared across each call, so the owner's end waits for one.
  contract::SharedMutex lifetime;
  bool closed DANNAN_GUARDED_BY(lifetime) = false;

  std::shared_ptr<Space> find(const std::string& name) const {
    contract::Lock lock(mutex);
    const auto found = spaces.find(name);
    return found == spaces.end() ? nullptr : found->second;
  }

  // Above every ballot this node has made or heard of.
  Ballot next_ballot(uint64_t above) {
    uint64_t current = counter.load();
    uint64_t next = 0;
    do {
      next = std::max(current, above) + 1;
    } while (!counter.compare_exchange_weak(current, next));
    return Ballot{next, self, epoch};
  }

  // Serves a request: an answer at once, or nothing when `answer` will be called.
  std::optional<net::Message> serve(const identity::NodeId& from, const net::Message& request, std::function<void(net::Message)> answer) {
    const bool preparing = request.type == net::message::agree_prepare;
    std::string space_name;
    std::optional<PrepareMessage> prepare;
    std::optional<AcceptMessage> accept;
    try {
      if (preparing) {
        prepare = decode_prepare(request.payload);
        space_name = prepare->space;
      } else {
        accept = decode_accept(request.payload);
        space_name = accept->space;
      }
    } catch (const net::DecodeError& error) {
      return net::error_reply(400, "invalid_request", error.what());
    }
    const auto space = find(space_name);
    if (!space) return net::error_reply(404, "no_space", "this node serves no space " + space_name);
    // Only the owner's nodes vote or write (Lugh's review).
    if (from != self && !advertises(from, space->capability))
      return net::error_reply(403, "not_permitted", "the sender does not advertise " + space->capability);
    const auto refuse = [&](std::string why) {
      return preparing ? net::Message{net::message::agree_prepare_reply, encode(PrepareAnswer{Refusal{std::move(why), {}, {}}, {}}), {}}
                       : net::Message{net::message::agree_accept_reply, encode(AcceptAnswer{Refusal{std::move(why), {}, {}}}), {}};
    };
    if (!space->store->ready()) return refuse("this acceptor is catching up");
    const auto me = self;
    space->enqueue(
        [space, me, from, preparing, prepare = std::move(prepare), accept = std::move(accept), answer = std::move(answer)](std::function<void()> done) mutable {
          const auto& names = preparing ? prepare->prepare.names : accept->accept.names;
          space->store->load(names, [space, me, from, preparing, prepare, accept, answer, done, names](std::vector<AcceptorState> held) mutable {
            if (preparing) {
              auto step = on_prepare(me, from, prepare->prepare, held);
              if (!step.keep) {
                answer({net::message::agree_prepare_reply, encode(step.answer), {}});
                return done();
              }
              space->store->keep(names, *step.keep, [answer, done, reply = encode(step.answer)](bool kept) {
                answer(kept ? net::Message{net::message::agree_prepare_reply, reply, {}}
                            : net::Message{net::message::agree_prepare_reply, encode(PrepareAnswer{Refusal{"the store could not keep", {}, {}}, {}}), {}});
                done();
              });
            } else {
              auto step = on_accept(me, from, accept->accept, held);
              if (!step.keep) {
                answer({net::message::agree_accept_reply, encode(step.answer), {}});
                return done();
              }
              space->store->keep(names, *step.keep, [answer, done, reply = encode(step.answer)](bool kept) {
                answer(kept ? net::Message{net::message::agree_accept_reply, reply, {}}
                            : net::Message{net::message::agree_accept_reply, encode(AcceptAnswer{Refusal{"the store could not keep", {}, {}}}), {}});
                done();
              });
            }
          });
        },
        space);
    return std::nullopt;
  }

  // This node's own acceptor, without the network.
  void serve_local(net::MessageType type, net::Bytes payload, Reply done) {
    auto shared = std::make_shared<Reply>(std::move(done));
    const auto told = [shared](net::Message message) {
      if (const auto refusal = net::refusal_of(message)) return (*shared)(std::nullopt, refusal->message);
      (*shared)(std::move(message.payload), {});
    };
    if (auto now = serve(self, net::Message{type, std::move(payload), {}}, told)) told(std::move(*now));
  }

  void send(const identity::NodeId& to, net::MessageType type, net::Bytes payload, std::chrono::milliseconds deadline, Reply done) {
    contract::ReadLock lock(lifetime);
    if (closed) return done(std::nullopt, "the node is stopping");
    if (to == self) return serve_local(type, std::move(payload), std::move(done));
    call(to, type, std::move(payload), deadline, std::move(done));
  }
};

// One change, retried until committed, refused for its set, or out of time.
struct Agreement::Run : std::enable_shared_from_this<Run> {
  std::shared_ptr<Core> core;
  asio::strand<asio::any_io_executor> strand;
  std::string space;
  std::vector<Name> names;
  ReplicaSet set;
  Change change;
  Reduce reduce;
  contract::TimePoint deadline;
  asio::any_io_executor reply;
  std::function<void(Outcome)> done;
  std::unique_ptr<Attempt> attempt;
  uint64_t generation = 0;
  size_t outstanding = 0;
  bool finished = false;
  uint32_t attempts = 0;
  // Which backoff is current: a retry superseded by another does nothing.
  uint64_t retries = 0;
  // Seeded from the node and its epoch, so a simulation's run replays exactly.
  std::mt19937_64 random;

  Run(std::shared_ptr<Core> owner, std::string space_name, std::vector<Name> registers, ReplicaSet replicas, Change changing, Reduce reducing,
      contract::TimePoint until, asio::any_io_executor told, std::function<void(Outcome)> then)
      : core(std::move(owner)),
        strand(asio::make_strand(core->executor)),
        space(std::move(space_name)),
        names(std::move(registers)),
        set(std::move(replicas)),
        change(std::move(changing)),
        reduce(std::move(reducing)),
        deadline(until),
        reply(std::move(told)),
        done(std::move(then)),
        random(seed(core->self, core->epoch, ++core->runs)) {}

  static uint64_t seed(const identity::NodeId& node, const identity::NodeId& epoch, uint64_t counter) {
    uint64_t out = counter;
    for (const auto byte : node.bytes) out = out * 131 + byte;
    for (const auto byte : epoch.bytes) out = out * 131 + byte;
    return out;
  }

  std::chrono::milliseconds left() const {
    return std::max(std::chrono::ceil<std::chrono::milliseconds>(deadline - core->time->now()), std::chrono::milliseconds(1));
  }

  // The deadline on the injected time, through the core's schedule.
  void arm_watch() {
    const auto remaining = std::max(std::chrono::ceil<std::chrono::milliseconds>(deadline - core->time->now()), std::chrono::milliseconds(0));
    core->schedule(remaining, [self = shared_from_this()] {
      asio::post(self->strand, [self] {
        if (self->finished) return;
        if (self->core->time->now() >= self->deadline) return self->fail(self->attempt ? self->attempt->why() : std::string(), true);
        self->arm_watch();
      });
    });
  }

  void begin(uint64_t above) {
    if (finished) return;
    if (core->time->now() >= deadline) return fail(attempt ? attempt->why() : std::string(), true);
    ++generation;
    ++attempts;
    attempt = std::make_unique<Attempt>(set, core->next_ballot(above), names, change, reduce);
    outstanding = set.nodes.size();
    const auto request = encode(PrepareMessage{space, attempt->prepare()});
    for (const auto& node : set.nodes)
      core->send(node, net::message::agree_prepare, request, left(),
                 [self = shared_from_this(), at = generation, node](std::optional<net::Bytes> answer, std::string) {
                   asio::post(self->strand, [self, at, node, answer = std::move(answer)] { self->prepared(at, node, answer); });
                 });
  }

  void prepared(uint64_t at, const identity::NodeId& node, const std::optional<net::Bytes>& answer) {
    if (finished || at != generation || attempt->phase() != Attempt::Phase::preparing) return;
    --outstanding;
    std::optional<Accept> accept;
    if (answer) {
      try {
        accept = attempt->on_prepare(node, decode_prepare_answer(*answer));
      } catch (const net::DecodeError&) {
      }
    }
    if (accept) {
      outstanding = set.nodes.size();
      const auto request = encode(AcceptMessage{space, *accept});
      for (const auto& to : set.nodes)
        core->send(to, net::message::agree_accept, request, left(), [self = shared_from_this(), at, to](std::optional<net::Bytes> replied, std::string) {
          asio::post(self->strand, [self, at, to, replied = std::move(replied)] { self->accepted(at, to, replied); });
        });
      return;
    }
    settle();
  }

  void accepted(uint64_t at, const identity::NodeId& node, const std::optional<net::Bytes>& answer) {
    if (finished || at != generation || attempt->phase() != Attempt::Phase::accepting) return;
    --outstanding;
    if (answer) {
      try {
        attempt->on_accept(node, decode_accept_answer(*answer));
      } catch (const net::DecodeError&) {
      }
    }
    if (attempt->phase() == Attempt::Phase::committed) return succeed();
    settle();
  }

  // Refused, or every replica heard from without a majority: another ballot, or the end.
  void settle() {
    if (attempt->phase() == Attempt::Phase::refused) {
      // Another set serves these names: this one is wrong, whatever a retry would say.
      if (attempt->serving()) return fail(attempt->why(), false);
      return retry(attempt->higher() ? attempt->higher()->counter : 0);
    }
    if (outstanding == 0) retry(0);
  }

  // Jittered and growing, so two proposers duelling on one name pull apart.
  void retry(uint64_t above) {
    const uint64_t ceiling = std::min<uint64_t>(50, uint64_t{1} << std::min<uint32_t>(attempts, 6));
    // Drawn without the standard library's distributions, which differ between libraries.
    const auto wait = std::chrono::milliseconds(1 + static_cast<int64_t>(random() % ceiling));
    const auto current = ++retries;
    core->schedule(wait, [self = shared_from_this(), above, current] {
      asio::post(self->strand, [self, above, current] {
        if (!self->finished && current == self->retries) self->begin(above);
      });
    });
  }

  Outcome evidence() const {
    Outcome out;
    out.attempts = attempts;
    if (!attempt) return out;
    out.ballot = attempt->prepare().ballot;
    out.accepted = attempt->accepted();
    out.refused = attempt->refused();
    for (const auto& node : set.nodes)
      if (std::find(out.accepted.begin(), out.accepted.end(), node) == out.accepted.end() &&
          std::find(out.refused.begin(), out.refused.end(), node) == out.refused.end())
        out.silent.push_back(node);
    return out;
  }

  void succeed() {
    finished = true;
    auto out = evidence();
    out.committed = true;
    out.values = attempt->values();
    out.changed_at = attempt->changed_at();
    out.result = attempt->result();
    asio::post(reply, [done = std::move(done), out = std::move(out)] { done(out); });
  }

  void fail(std::string why, bool timed_out) {
    if (finished) return;
    finished = true;
    auto out = evidence();
    out.serving = attempt ? attempt->serving() : std::nullopt;
    out.why = timed_out ? "not committed within the deadline" + (why.empty() ? std::string() : ": " + why) : std::move(why);
    asio::post(reply, [done = std::move(done), out = std::move(out)] { done(out); });
  }
};

namespace {

// The default schedule: a timer on the executor, looked at again as the time source says,
// so a stepped source in a test moves it without a real wait.
void after_on(const asio::any_io_executor& executor, const contract::TimeSource* time, contract::TimePoint at, std::function<void()> task) {
  auto timer = std::make_shared<asio::steady_timer>(executor);
  const auto remaining = std::max(std::chrono::ceil<std::chrono::milliseconds>(at - time->now()), std::chrono::milliseconds(0));
  timer->expires_after(time->wake_within(remaining));
  timer->async_wait([timer, executor, time, at, task = std::move(task)](const boost::system::error_code& error) mutable {
    if (error) return;
    if (time->now() >= at) return task();
    after_on(executor, time, at, std::move(task));
  });
}

}  // namespace

Agreement::Agreement(asio::any_io_executor executor, identity::NodeId self, identity::NodeId epoch, Call call, Advertises advertises,
                     const contract::TimeSource* time, Schedule schedule)
    : _core(std::make_shared<Core>()) {
  _core->executor = std::move(executor);
  _core->self = self;
  _core->epoch = epoch;
  _core->call = std::move(call);
  _core->advertises = std::move(advertises);
  _core->time = time;
  _core->schedule = schedule ? std::move(schedule) : Schedule([executor = _core->executor, time](std::chrono::milliseconds after, std::function<void()> task) {
    after_on(executor, time, time->now() + after, std::move(task));
  });
}

Agreement::~Agreement() {
  contract::WriteLock lock(_core->lifetime);
  _core->closed = true;
}

void Agreement::serve(std::string space, std::string capability, std::shared_ptr<AcceptorStore> store) {
  if (space.empty() || space.size() > max_space) throw std::invalid_argument("a space is named in 1 to 255 bytes");
  if (!store) throw std::invalid_argument("a served space needs its store");
  auto served = std::make_shared<Space>();
  served->capability = std::move(capability);
  served->store = std::move(store);
  served->executor = _core->executor;
  contract::Lock lock(_core->mutex);
  if (!_core->spaces.emplace(space, std::move(served)).second) throw std::invalid_argument("the space " + space + " is already served");
}

void Agreement::unserve(const std::string& space) {
  contract::Lock lock(_core->mutex);
  _core->spaces.erase(space);
}

void Agreement::change(std::string space, std::vector<Name> names, std::vector<identity::NodeId> replicas, uint64_t version, Change change, Reduce reduce,
                       std::chrono::milliseconds deadline, asio::any_io_executor executor, std::function<void(Outcome)> done) {
  auto run = std::make_shared<Run>(_core, std::move(space), std::move(names), ReplicaSet::of(std::move(replicas), version), std::move(change),
                                   std::move(reduce), _core->time->now() + deadline, std::move(executor), std::move(done));
  asio::post(run->strand, [run] {
    run->arm_watch();
    run->begin(0);
  });
}

void Agreement::read(std::string space, std::vector<Name> names, std::vector<identity::NodeId> replicas, uint64_t version, Reduce reduce,
                     std::chrono::milliseconds deadline, asio::any_io_executor executor, std::function<void(Outcome)> done) {
  change(
      std::move(space), std::move(names), std::move(replicas), version,
      [](std::span<const std::optional<Bytes>> current) { return Changed{std::vector<std::optional<Bytes>>(current.begin(), current.end()), {}}; },
      std::move(reduce), deadline, std::move(executor), std::move(done));
}

std::optional<net::Message> Agreement::serve_request(const identity::NodeId& from, const net::Message& request, std::function<void(net::Message)> answer) {
  return _core->serve(from, request, std::move(answer));
}

}  // namespace dannan::agree
