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
#include <mutex>
#include <random>
#include <set>
#include <stdexcept>

#include "dannan/agree/wire.h"
#include "dannan/net/routes.h"

namespace dannan::agree {

namespace asio = boost::asio;
using Reply = std::function<void(std::optional<net::Bytes>, std::string)>;

// One space served on this node: its store, the capability its proposers advertise,
// and its requests one at a time (load, the rules, keep, answer).
struct Space {
  std::string name;
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

namespace {

// A request as the acceptor serves it, whichever kind.
struct Request {
  enum class Kind { prepare, accept, purge, settle } kind;
  std::string space;
  Slot slot = 0;
  std::vector<Name> names;
  // A transfer's request loads every name held in the slot as well.
  bool every = false;
  std::optional<Prepare> prepare;
  std::optional<Accept> accept;
  std::optional<Purge> purge;
  std::optional<Settle> settle;
  // The sender's slot table's fingerprint.
  uint64_t table = 0;
  // Whether the store loads values: a value-less prepare's answers carry none.
  bool values = true;
};

Request decode_request(const net::Message& message) {
  namespace m = net::message;
  Request out{};
  if (message.type == m::agree_prepare) {
    auto decoded = decode_prepare(message.payload);
    const bool values = decoded.prepare.values;
    out =
        Request{Request::Kind::prepare, decoded.space, decoded.prepare.slot, decoded.prepare.names, decoded.prepare.to.has_value(), decoded.prepare, {}, {}, {},
                decoded.table,          values};
  } else if (message.type == m::agree_accept) {
    auto decoded = decode_accept(message.payload);
    out = Request{Request::Kind::accept, decoded.space, decoded.accept.slot, decoded.accept.names, decoded.accept.to.has_value(), {}, decoded.accept, {}, {},
                  decoded.table,         true};
  } else if (message.type == m::agree_purge) {
    auto decoded = decode_purge(message.payload);
    out = Request{Request::Kind::purge, decoded.space, decoded.purge.slot, decoded.purge.names, false, {}, {}, decoded.purge, {}, decoded.table, true};
  } else {
    auto decoded = decode_settle(message.payload);
    out = Request{Request::Kind::settle, decoded.space, decoded.settle.slot, {}, false, {}, {}, {}, decoded.settle, decoded.table, true};
  }
  return out;
}

net::MessageType reply_type(Request::Kind kind) {
  namespace m = net::message;
  switch (kind) {
    case Request::Kind::prepare:
      return m::agree_prepare_reply;
    case Request::Kind::accept:
      return m::agree_accept_reply;
    case Request::Kind::purge:
      return m::agree_purge_reply;
    case Request::Kind::settle:
      return m::agree_settle_reply;
  }
  return m::agree_accept_reply;
}

Refusal refusal_of(std::string why) { return Refusal{std::move(why), std::nullopt, std::nullopt, std::nullopt}; }

}  // namespace

struct Agreement::Core : std::enable_shared_from_this<Core> {
  asio::any_io_executor executor;
  identity::NodeId self;
  identity::NodeId epoch;
  Table table;
  // The table's fingerprint, carried on every request and checked on every one served.
  uint64_t fingerprint = 0;
  Call call;
  Advertises advertises;
  const contract::TimeSource* time;
  Schedule schedule;
  Publish publish;
  Adverts adverts;
  // Each space's slot bitmap, as last published. Held across the publish, so two keeps never
  // publish out of order. Lock order: this before the roster's own (publish sets an advert).
  contract::Mutex publishing;
  std::map<std::string, std::vector<uint8_t>> bitmaps DANNAN_GUARDED_BY(publishing);

  // The fast path's cache: this node's last committed change of each name list, by space,
  // slot, set and names. Taken by the next change of the same names, put back by its commit,
  // so a change that fails leaves none and the one after goes the full way.
  struct Last {
    // The ballot it committed at, and the one its accept reserved for the next.
    Ballot ballot;
    Ballot reserved;
    std::vector<std::optional<Bytes>> values;
    std::vector<Ballot> changed_at;
  };
  static constexpr size_t max_lasts = 65536;
  contract::Mutex lasts_mutex;
  std::map<std::string, Last> lasts DANNAN_GUARDED_BY(lasts_mutex);

  static std::string last_key(const std::string& space, Slot slot, const ReplicaSet& set, const std::vector<Name>& names) {
    net::Writer out;
    out.string(space);
    out.u32(slot);
    out.u64(set.version);
    for (const auto& node : set.nodes) out.fixed(node.bytes);
    for (const auto& name : names) out.bytes(name);
    const auto bytes = out.take();
    return std::string(bytes.begin(), bytes.end());
  }
  std::optional<Last> take_last(const std::string& key) {
    contract::Lock lock(lasts_mutex);
    const auto found = lasts.find(key);
    if (found == lasts.end()) return std::nullopt;
    auto out = std::move(found->second);
    lasts.erase(found);
    return out;
  }
  void keep_last(const std::string& key, Last last) {
    contract::Lock lock(lasts_mutex);
    // Bounded: past it, the cache starts again, and the next change of each goes the full way.
    if (lasts.size() >= max_lasts) lasts.clear();
    lasts[key] = std::move(last);
  }
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

  // A record of `slot` of `space` was just kept here: whether this node serves it goes into
  // the space's slot advert, published when it changes.
  void note(const std::string& space, Slot slot, const SlotState& record) {
    if (!publish) return;
    const bool serving = record.served && record.served->contains(self);
    contract::Lock lock(publishing);
    auto& bitmap = bitmaps[space];
    bitmap.resize((table.slots + 7) / 8);
    const auto bit = static_cast<uint8_t>(1u << (slot % 8));
    auto& byte = bitmap[slot / 8];
    const auto next = static_cast<uint8_t>(serving ? (byte | bit) : (byte & ~bit));
    if (next == byte) return;
    byte = next;
    publish(Agreement::slot_advert(space), net::Bytes(bitmap.begin(), bitmap.end()));
  }

  void withdraw(const std::string& space) {
    if (!publish) return;
    contract::Lock lock(publishing);
    bitmaps.erase(space);
    publish(Agreement::slot_advert(space), std::nullopt);
  }

  // Serves a request: an answer at once, or nothing when `answer` will be called.
  std::optional<net::Message> serve(const identity::NodeId& from, const net::Message& message, std::function<void(net::Message)> answer) {
    if (message.type == net::message::agree_fetch) return serve_fetch(from, message, std::move(answer));
    Request request;
    try {
      request = decode_request(message);
    } catch (const net::DecodeError& error) {
      return net::error_reply(400, "invalid_request", error.what());
    }
    const auto replied = reply_type(request.kind);
    if (request.slot >= table.slots) return net::error_reply(400, "invalid_request", "slot " + std::to_string(request.slot) + " is past this cluster's table");
    const auto space = find(request.space);
    if (!space) return net::error_reply(404, "no_space", "this node serves no space " + request.space);
    // Only the owner's nodes vote or write (Lugh's review).
    if (from != self && !advertises(from, space->capability))
      return net::error_reply(403, "not_permitted", "the sender does not advertise " + space->capability);
    const auto refusal = [kind = request.kind, replied](std::string why) {
      return kind == Request::Kind::prepare ? net::Message{replied, encode(PrepareAnswer{refusal_of(std::move(why)), {}, {}, {}, {}}), {}}
                                            : net::Message{replied, encode(AcceptAnswer{refusal_of(std::move(why))}), {}};
    };
    if (request.table != fingerprint) return refusal("the sender's slot table (count or genesis set) differs from this node's");
    if (!space->store->ready()) return refusal("this acceptor is catching up");
    space->enqueue(
        [core = shared_from_this(), space, me = self, genesis = table.genesis, from, refusal, request = std::move(request), replied,
         answer = std::move(answer)](std::function<void()> done) mutable {
          const auto slot = request.slot;
          const auto every = request.every;
          const auto values = request.values;
          auto names = request.names;
          space->store->load(
              slot, names, every, values, [core, space, me, genesis, from, refusal, request = std::move(request), replied, answer, done](Held held) mutable {
                if (held.states.size() != held.names.size() || held.names.size() < request.names.size()) {
                  answer(refusal("the store loaded a different count"));
                  return done();
                }
                if (request.every && held.names.size() > max_names) {
                  answer(refusal("the slot holds more names than one transfer carries"));
                  return done();
                }
                std::optional<Held> keep;
                net::Bytes reply;
                switch (request.kind) {
                  case Request::Kind::prepare: {
                    auto step = on_prepare(me, from, *request.prepare, held, genesis);
                    keep = std::move(step.keep);
                    reply = encode(step.answer);
                    break;
                  }
                  case Request::Kind::accept: {
                    auto step = on_accept(me, from, *request.accept, held, genesis);
                    keep = std::move(step.keep);
                    reply = encode(step.answer);
                    break;
                  }
                  case Request::Kind::purge: {
                    auto step = on_purge(me, from, *request.purge, held);
                    reply = encode(AcceptAnswer{step.refused});
                    if (!step.refused) keep = std::move(step.keep);
                    break;
                  }
                  case Request::Kind::settle: {
                    auto step = on_settle(me, from, *request.settle, held);
                    keep = std::move(step.keep);
                    reply = encode(step.answer);
                    break;
                  }
                }
                if (!keep) {
                  answer({replied, std::move(reply), {}});
                  return done();
                }
                space->store->keep(
                    request.slot, *keep,
                    [core, space, slot = request.slot, record = keep->slot, answer, done, replied, refusal, reply = std::move(reply)](bool kept) {
                      if (kept) core->note(space->name, slot, record);
                      answer(kept ? net::Message{replied, reply, {}} : refusal("the store could not keep"));
                      done();
                    });
              });
        },
        space);
    return std::nullopt;
  }

  // A fetch: the value held at exactly the ballot asked, the piece from the offset asked.
  // Read in the space's order with its other requests; nothing is kept.
  std::optional<net::Message> serve_fetch(const identity::NodeId& from, const net::Message& message, std::function<void(net::Message)> answer) {
    namespace m = net::message;
    FetchMessage fetch;
    try {
      fetch = decode_fetch(message.payload);
    } catch (const net::DecodeError& error) {
      return net::error_reply(400, "invalid_request", error.what());
    }
    if (fetch.slot >= table.slots) return net::error_reply(400, "invalid_request", "slot " + std::to_string(fetch.slot) + " is past this cluster's table");
    const auto space = find(fetch.space);
    if (!space) return net::error_reply(404, "no_space", "this node serves no space " + fetch.space);
    if (from != self && !advertises(from, space->capability))
      return net::error_reply(403, "not_permitted", "the sender does not advertise " + space->capability);
    const auto refusal = [](std::string why) {
      return net::Message{m::agree_fetch_reply, encode(FetchAnswer{refusal_of(std::move(why)), false, 0, 0, {}}), {}};
    };
    if (fetch.table != fingerprint) return refusal("the sender's slot table (count or genesis set) differs from this node's");
    if (!fetch.set.contains(self)) return refusal("this node is not in the replica set named");
    if (!fetch.set.contains(from)) return refusal("the sender is not in the replica set named");
    if (!space->store->ready()) return refusal("this acceptor is catching up");
    space->enqueue(
        [space, fetch = std::move(fetch), refusal, answer = std::move(answer)](std::function<void()> done) mutable {
          const auto slot = fetch.slot;
          const auto name = fetch.name;
          const auto ballot = fetch.ballot;
          const auto since = fetch.since;
          space->store->value(slot, name, ballot, since, [fetch = std::move(fetch), refusal, answer, done](std::optional<AcceptorStore::Fetched> fetched) {
            if (!fetched) {
              answer(net::Message{m::agree_fetch_reply, encode(FetchAnswer{}), {}});
              return done();
            }
            const auto& value = fetched->bytes;
            if (value.size() > max_value || fetch.offset > value.size() || (fetched->past && !fetch.since)) {
              answer(refusal(value.size() > max_value        ? "the value is over the largest value"
                             : fetched->past && !fetch.since ? "the store answered past a summary it was not asked for"
                                                             : "the offset is past the value"));
              return done();
            }
            const auto start = static_cast<size_t>(fetch.offset);
            const auto length = std::min(max_piece, value.size() - start);
            FetchAnswer piece{std::nullopt,
                              true,
                              value.size(),
                              fetch.offset,
                              Bytes(value.begin() + static_cast<std::ptrdiff_t>(start), value.begin() + static_cast<std::ptrdiff_t>(start + length)),
                              fetched->past};
            answer(net::Message{m::agree_fetch_reply, encode(piece), {}});
            done();
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

// One change or transfer, retried until committed, refused for its set, or out of time. A
// change that finds a transfer to its set unfinished finishes it first.
struct Agreement::Run : std::enable_shared_from_this<Run> {
  // Makes the run's own attempt under a ballot.
  using Make = std::function<Attempt(Ballot)>;

  std::shared_ptr<Core> core;
  asio::strand<asio::any_io_executor> strand;
  std::string space;
  Slot slot;
  Make make;
  // A plain change's: its first try on the fast path, when this node's last commit of its
  // names is cached; and what its commit leaves in the cache.
  std::function<std::optional<Attempt>()> make_fast;
  std::function<void(const Attempt&, const Ballot& reserved)> on_commit;
  // Every accept this run has sent for its own change, with that attempt's result.
  std::vector<std::pair<Ballot, Bytes>> sent;
  // The ballot the last accept sent reserved.
  std::optional<Ballot> reserved;
  contract::TimePoint deadline;
  asio::any_io_executor reply;
  std::function<void(Outcome)> done;
  std::unique_ptr<Attempt> attempt;
  // An unfinished transfer being finished before the run's own attempt: from, to.
  std::optional<std::pair<ReplicaSet, ReplicaSet>> finishing;
  uint32_t finished_transfers = 0;
  uint64_t generation = 0;
  size_t outstanding = 0;
  // The current attempt's fetches already asked for, by their id.
  std::set<size_t> fetches_started;
  bool finished = false;
  uint32_t attempts = 0;
  // Which backoff is current: a retry superseded by another does nothing.
  uint64_t retries = 0;
  // Seeded from the node and its epoch, so a simulation's run replays exactly.
  std::mt19937_64 random;

  Run(std::shared_ptr<Core> owner, std::string space_name, Slot slot_of_run, Make making, contract::TimePoint until, asio::any_io_executor told,
      std::function<void(Outcome)> then)
      : core(std::move(owner)),
        strand(asio::make_strand(core->executor)),
        space(std::move(space_name)),
        slot(slot_of_run),
        make(std::move(making)),
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
    fetches_started.clear();
    const auto ballot = core->next_ballot(above);
    // The first try of a plain change whose names this node committed last: one round.
    if (!finishing && attempts == 1 && make_fast)
      if (auto fast = make_fast()) {
        attempt = std::make_unique<Attempt>(std::move(*fast));
        return send_accept(*attempt->accept());
      }
    attempt = std::make_unique<Attempt>(finishing ? Attempt::transfer(finishing->first, finishing->second, slot, ballot) : make(ballot));
    if (!attempt->prepare().to) {
      attempt->own(sent);
      // No answer carries a value; the bases are fetched, from this node's own acceptor when it
      // holds them (a change with a reduction still asks for every value).
      attempt->value_less();
    }
    outstanding = attempt->recipients().size();
    const auto request = encode(PrepareMessage{space, attempt->prepare(), core->fingerprint});
    for (const auto& node : attempt->recipients())
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
    if (accept) return send_accept(*accept);
    if (attempt->phase() == Attempt::Phase::fetching) return fetch_bases();
    settle_or_retry();
  }

  // One value being fetched: its holders in the order tried, and what has come so far.
  struct Fetching {
    size_t id = 0;
    size_t name = 0;
    Ballot ballot;
    std::optional<Bytes> since;
    std::vector<identity::NodeId> holders;
    size_t holder = 0;
    Bytes value;
    std::optional<uint64_t> total;
    // Whether what has come is only what lies past `since`: every piece from one holder says
    // the same, and a fetch moving to another holder starts again.
    bool past = false;
  };

  // Every value the attempt wants and this run has not yet asked for, each from its holders:
  // this node's own acceptor first, which needs no message. A holder that has moved on,
  // refuses or is silent gives way to the next, from the same offset, since every holder of
  // the ballot and summary holds the same value (past a summary, from the beginning).
  void fetch_bases() {
    for (const auto& wanted : attempt->wanted()) {
      if (!fetches_started.insert(wanted.id).second) continue;
      auto fetching = std::make_shared<Fetching>();
      fetching->id = wanted.id;
      fetching->name = wanted.name;
      fetching->ballot = wanted.ballot;
      fetching->since = wanted.since;
      fetching->holders = wanted.holders;
      const auto own = std::find(fetching->holders.begin(), fetching->holders.end(), core->self);
      if (own != fetching->holders.end()) std::rotate(fetching->holders.begin(), own, own + 1);
      fetch_piece(fetching);
    }
  }

  void fetch_piece(const std::shared_ptr<Fetching>& fetching) {
    if (fetching->holder >= fetching->holders.size()) {
      attempt->unfetched("no acceptor holding a base's ballot gave its value");
      return settle_or_retry();
    }
    const auto& prepare = attempt->prepare();
    const auto request = encode(
        FetchMessage{space, prepare.set, slot, prepare.names[fetching->name], fetching->ballot, fetching->value.size(), core->fingerprint, fetching->since});
    core->send(fetching->holders[fetching->holder], net::message::agree_fetch, request, left(),
               [self = shared_from_this(), at = generation, fetching](std::optional<net::Bytes> answer, std::string) {
                 asio::post(self->strand, [self, at, fetching, answer = std::move(answer)] { self->fetched_piece(at, fetching, answer); });
               });
  }

  void fetched_piece(uint64_t at, const std::shared_ptr<Fetching>& fetching, const std::optional<net::Bytes>& answer) {
    if (finished || at != generation || attempt->phase() != Attempt::Phase::fetching) return;
    std::optional<FetchAnswer> piece;
    if (answer) {
      try {
        piece = decode_fetch_answer(*answer);
      } catch (const net::DecodeError&) {
      }
    }
    const bool usable = piece && !piece->refused && piece->held && piece->offset == fetching->value.size() &&
                        (!fetching->total || (*fetching->total == piece->total && fetching->past == piece->past)) &&
                        (!piece->piece.empty() || piece->total == piece->offset) && (!piece->past || fetching->since);
    if (!usable) {
      ++fetching->holder;
      // Past a summary, one holder may answer what lies past it and another the whole value:
      // the next starts from the beginning.
      if (fetching->since) {
        fetching->value.clear();
        fetching->total.reset();
        fetching->past = false;
      }
      return fetch_piece(fetching);
    }
    fetching->total = piece->total;
    fetching->past = piece->past;
    fetching->value.insert(fetching->value.end(), piece->piece.begin(), piece->piece.end());
    if (fetching->value.size() < *fetching->total) return fetch_piece(fetching);
    if (auto accept = attempt->fetched(fetching->id, std::move(fetching->value), fetching->past)) return send_accept(*accept);
    // The owner's reduction asked for more, or refused.
    if (attempt->phase() == Attempt::Phase::fetching) return fetch_bases();
    if (attempt->phase() == Attempt::Phase::refused) settle_or_retry();
  }

  void send_accept(Accept accept) {
    if (!accept.to) {
      // Remembered, so a later attempt that finds it newest commits it rather than changing it again.
      sent.emplace_back(accept.ballot, attempt->result());
      // This node's next ballot, reserved by the acceptors that take this, for the fast path.
      accept.reserve = core->next_ballot(0);
      reserved = accept.reserve;
    }
    outstanding = attempt->recipients().size();
    const auto request = encode(AcceptMessage{space, accept, core->fingerprint});
    for (const auto& to : attempt->recipients())
      core->send(to, net::message::agree_accept, request, left(),
                 [self = shared_from_this(), at = generation, to](std::optional<net::Bytes> replied, std::string) {
                   asio::post(self->strand, [self, at, to, replied = std::move(replied)] { self->accepted(at, to, replied); });
                 });
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
    if (attempt->phase() == Attempt::Phase::committed) return committed();
    settle_or_retry();
  }

  // A committed transfer is settled at both sets before anything relies on it; a finished
  // one gives way to the run's own attempt at once.
  void committed() {
    const auto& prepare = attempt->prepare();
    if (prepare.to) {
      const auto request = encode(SettleMessage{space, Settle{prepare.set, *prepare.to, slot, prepare.ballot}, core->fingerprint});
      for (const auto& node : attempt->recipients())
        core->send(node, net::message::agree_settle, request, left(), [](std::optional<net::Bytes>, std::string) {});
    }
    if (finishing) {
      finishing.reset();
      ++finished_transfers;
      return begin(0);
    }
    if (on_commit && reserved) on_commit(*attempt, *reserved);
    succeed();
  }

  // Refused, or every recipient heard from without enough: another ballot, a transfer to
  // finish, or the end.
  void settle_or_retry() {
    // The fast path missed: the full way at once, no backoff (Mimir's "one round, kept one").
    const bool missed = attempt->phase() == Attempt::Phase::refused || outstanding == 0;
    if (attempt->is_fast() && missed && !(attempt->phase() == Attempt::Phase::refused && attempt->serving() && !attempt->unfinished()))
      return begin(attempt->higher() ? attempt->higher()->counter : 0);
    if (attempt->phase() == Attempt::Phase::refused) {
      if (attempt->unfinished() && !finishing) {
        finishing = std::make_pair(*attempt->unfinished(), attempt->prepare().set);
        return retry(attempt->higher() ? attempt->higher()->counter : 0);
      }
      // Another set serves the slot: this one is wrong, whatever a retry would say.
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
    out.finished = finished_transfers;
    if (!attempt) return out;
    out.ballot = attempt->prepare().ballot;
    out.accepted = attempt->accepted();
    out.refused = attempt->refused();
    for (const auto& node : attempt->recipients())
      if (std::find(out.accepted.begin(), out.accepted.end(), node) == out.accepted.end() &&
          std::find(out.refused.begin(), out.refused.end(), node) == out.refused.end())
        out.silent.push_back(node);
    return out;
  }

  void succeed() {
    finished = true;
    auto out = evidence();
    out.committed = true;
    out.fast = attempt->is_fast();
    out.values = attempt->values();
    out.changed_at = attempt->changed_at();
    out.result = attempt->result();
    if (attempt->prepare().to) out.names = attempt->names();
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

  static void start(const std::shared_ptr<Run>& run) {
    asio::post(run->strand, [run] {
      run->arm_watch();
      run->begin(0);
    });
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

Agreement::Agreement(asio::any_io_executor executor, identity::NodeId self, identity::NodeId epoch, Table table, Call call, Advertises advertises,
                     const contract::TimeSource* time, Schedule schedule, Publish publish, Adverts adverts)
    : _core(std::make_shared<Core>()) {
  if (table.slots == 0 || table.slots > max_slots) throw std::invalid_argument("a slot table holds 1 to 8192 slots");
  _core->executor = std::move(executor);
  _core->self = self;
  _core->epoch = epoch;
  _core->table = std::move(table);
  _core->fingerprint = table_fingerprint(_core->table.slots, _core->table.genesis);
  _core->call = std::move(call);
  _core->advertises = std::move(advertises);
  _core->publish = std::move(publish);
  _core->adverts = std::move(adverts);
  _core->time = time;
  _core->schedule = schedule ? std::move(schedule) : Schedule([executor = _core->executor, time](std::chrono::milliseconds after, std::function<void()> task) {
    after_on(executor, time, time->now() + after, std::move(task));
  });
}

Agreement::~Agreement() {
  contract::WriteLock lock(_core->lifetime);
  _core->closed = true;
}

const Agreement::Table& Agreement::table() const noexcept { return _core->table; }

void Agreement::serve(std::string space, std::string capability, std::shared_ptr<AcceptorStore> store) {
  if (space.empty() || space.size() > max_space) throw std::invalid_argument("a space is named in 1 to 255 bytes");
  if (!store) throw std::invalid_argument("a served space needs its store");
  auto served = std::make_shared<Space>();
  served->name = space;
  served->capability = std::move(capability);
  served->store = store;
  served->executor = _core->executor;
  {
    contract::Lock lock(_core->mutex);
    if (!_core->spaces.emplace(space, std::move(served)).second) throw std::invalid_argument("the space " + space + " is already served");
  }
  // The slot advert from what the store already holds.
  store->records([core = _core, space](std::vector<std::pair<Slot, SlotState>> records) {
    for (const auto& [slot, record] : records)
      if (slot < core->table.slots) core->note(space, slot, record);
  });
}

void Agreement::unserve(const std::string& space) {
  {
    contract::Lock lock(_core->mutex);
    _core->spaces.erase(space);
  }
  _core->withdraw(space);
}

std::vector<identity::NodeId> Agreement::hint(const std::string& space, Slot slot) const {
  std::vector<identity::NodeId> out;
  if (!_core->adverts || slot >= _core->table.slots) return out;
  for (const auto& [node, bitmap] : _core->adverts(slot_advert(space)))
    if (slot / 8 < bitmap.size() && ((bitmap[slot / 8] >> (slot % 8)) & 1u)) out.push_back(node);
  std::sort(out.begin(), out.end());
  return out;
}

namespace {

void check_slot(Slot slot, const Agreement::Table& table) {
  if (slot >= table.slots) throw std::invalid_argument("slot " + std::to_string(slot) + " is past this cluster's table of " + std::to_string(table.slots));
}

}  // namespace

void Agreement::change(std::string space, Slot slot, std::vector<Name> names, std::vector<identity::NodeId> replicas, uint64_t version, Change change,
                       Reduce reduce, std::chrono::milliseconds deadline, asio::any_io_executor executor, std::function<void(Outcome)> done) {
  check_slot(slot, _core->table);
  const auto set = ReplicaSet::of(std::move(replicas), version);
  const auto key = Core::last_key(space, slot, set, names);
  // An owner's reduction sees what changed outside agreement only in a prepare's answers, so a
  // change with one never takes the fast path (Mimir's merge writes).
  const bool may_be_fast = !reduce;
  auto make = [set, slot, names, change, reduce = std::move(reduce), genesis = _core->table.genesis](Ballot ballot) {
    return Attempt(set, slot, ballot, names, change, reduce, genesis);
  };
  auto run = std::make_shared<Run>(_core, std::move(space), slot, std::move(make), _core->time->now() + deadline, std::move(executor), std::move(done));
  if (may_be_fast) {
    run->make_fast = [core = _core, key, set, slot, names, change]() -> std::optional<Attempt> {
      auto last = core->take_last(key);
      if (!last) return std::nullopt;
      return Attempt::fast(set, slot, last->reserved, names, change, last->ballot, last->values, last->changed_at);
    };
    run->on_commit = [core = _core, key](const Attempt& committed, const Ballot& reserved) {
      core->keep_last(key, Core::Last{committed.prepare().ballot, reserved, committed.values(), committed.changed_at()});
    };
  }
  Run::start(run);
}

void Agreement::read(std::string space, Slot slot, std::vector<Name> names, std::vector<identity::NodeId> replicas, uint64_t version, Reduce reduce,
                     std::chrono::milliseconds deadline, asio::any_io_executor executor, std::function<void(Outcome)> done) {
  change(
      std::move(space), slot, std::move(names), std::move(replicas), version,
      [](std::span<const std::optional<Bytes>> current) { return Changed{std::vector<std::optional<Bytes>>(current.begin(), current.end()), {}}; },
      std::move(reduce), deadline, std::move(executor), std::move(done));
}

void Agreement::transfer(std::string space, Slot slot, std::vector<identity::NodeId> from, uint64_t from_version, std::vector<identity::NodeId> to,
                         uint64_t to_version, std::chrono::milliseconds deadline, asio::any_io_executor executor, std::function<void(Outcome)> done) {
  check_slot(slot, _core->table);
  if (!(to_version > from_version)) throw std::invalid_argument("a transfer's new set has a version above the old one's");
  auto make = [old_set = ReplicaSet::of(std::move(from), from_version), new_set = ReplicaSet::of(std::move(to), to_version), slot](Ballot ballot) {
    return Attempt::transfer(old_set, new_set, slot, ballot);
  };
  Run::start(std::make_shared<Run>(_core, std::move(space), slot, std::move(make), _core->time->now() + deadline, std::move(executor), std::move(done)));
}

void Agreement::found(std::string space, std::chrono::milliseconds deadline, asio::any_io_executor executor, std::function<void(Outcome)> done) {
  struct Founding {
    std::mutex mutex;
    uint32_t next = 0;
    uint32_t left = 0;
    Outcome out;
    std::function<void(Outcome)> done;
  };
  const auto& table = _core->table;
  if (table.genesis.nodes.empty()) {
    Outcome out;
    out.why = "this node has no genesis set";
    asio::post(executor, [done = std::move(done), out = std::move(out)] { done(out); });
    return;
  }
  auto founding = std::make_shared<Founding>();
  founding->left = table.slots;
  founding->out.committed = true;
  founding->done = std::move(done);
  // A window of slots at a time, so a table of 8192 is not 8192 rounds in flight.
  constexpr uint32_t window = 16;
  auto step = std::make_shared<std::function<void()>>();
  *step = [core = _core, founding, step = std::weak_ptr(step), space, deadline, executor, slots = table.slots, genesis = table.genesis]() {
    Slot slot = 0;
    {
      std::lock_guard lock(founding->mutex);
      if (founding->next == slots) return;
      slot = founding->next++;
    }
    const auto self = step.lock();
    auto make = [genesis, slot](Ballot ballot) {
      return Attempt(genesis, slot, ballot, {}, [](std::span<const std::optional<Bytes>>) { return Changed{}; }, {}, genesis);
    };
    Run::start(std::make_shared<Run>(core, space, slot, std::move(make), core->time->now() + deadline, executor, [founding, self, slot](Outcome outcome) {
      bool last = false;
      {
        std::lock_guard lock(founding->mutex);
        founding->out.attempts += outcome.attempts;
        if (!outcome.committed && founding->out.committed) {
          founding->out.committed = false;
          founding->out.why = "slot " + std::to_string(slot) + ": " + outcome.why;
          founding->out.serving = outcome.serving;
        }
        last = --founding->left == 0;
      }
      if (last) return founding->done(std::move(founding->out));
      if (self) (*self)();
    }));
  };
  // Kept alive by the steps in flight, which hold it strongly while they run.
  for (uint32_t i = 0; i < std::min(window, table.slots); ++i) {
    auto held = step;
    asio::post(executor, [held] { (*held)(); });
  }
}

namespace {

// Sends one request to every node and collects each answer (none for a node that did not
// answer), told once all are in.
void fan_out(const std::function<void(const identity::NodeId&, net::MessageType, net::Bytes, std::chrono::milliseconds,
                                      std::function<void(std::optional<net::Bytes>, std::string)>)>& send,
             const std::vector<identity::NodeId>& nodes, net::MessageType type, const net::Bytes& payload, std::chrono::milliseconds deadline,
             asio::any_io_executor executor, std::function<void(std::vector<std::optional<net::Bytes>>)> done) {
  struct Gathered {
    std::mutex mutex;
    std::vector<std::optional<net::Bytes>> answers;
    size_t left;
  };
  auto gathered = std::make_shared<Gathered>();
  gathered->answers.resize(nodes.size());
  gathered->left = nodes.size();
  auto shared = std::make_shared<std::function<void(std::vector<std::optional<net::Bytes>>)>>(std::move(done));
  for (size_t i = 0; i < nodes.size(); ++i)
    send(nodes[i], type, payload, deadline, [gathered, shared, executor, i](std::optional<net::Bytes> answer, std::string) {
      bool last = false;
      {
        std::lock_guard lock(gathered->mutex);
        gathered->answers[i] = std::move(answer);
        last = --gathered->left == 0;
      }
      if (last) asio::post(executor, [gathered, shared] { (*shared)(std::move(gathered->answers)); });
    });
}

}  // namespace

void Agreement::forget(std::string space, Slot slot, std::vector<Name> names, std::vector<identity::NodeId> replicas, uint64_t version,
                       std::chrono::milliseconds deadline, asio::any_io_executor executor, std::function<void(Outcome)> done) {
  const auto set = ReplicaSet::of(replicas, version);
  auto core = _core;
  const auto send = [core](const identity::NodeId& to, net::MessageType type, net::Bytes payload, std::chrono::milliseconds within, Reply reply) {
    core->send(to, type, std::move(payload), within, std::move(reply));
  };
  change(
      space, slot, names, replicas, version,
      [](std::span<const std::optional<Bytes>> current) { return Changed{std::vector<std::optional<Bytes>>(current.size()), {}}; }, {}, deadline, executor,
      [space, slot, names, set, send, deadline, executor, fingerprint = core->fingerprint, done = std::move(done)](Outcome tombstone) mutable {
        if (!tombstone.committed) return done(std::move(tombstone));
        // Every replica must hold the tombstone before any forgets it: sent again, the same
        // accept, to all.
        const Accept again{set,          std::nullopt, slot, tombstone.ballot, names, std::vector<std::optional<Bytes>>(names.size()), tombstone.changed_at,
                           std::nullopt, std::nullopt};
        fan_out(send, set.nodes, net::message::agree_accept, encode(AcceptMessage{space, again, fingerprint}), deadline, executor,
                [space, slot, names, set, send, deadline, executor, fingerprint, done = std::move(done),
                 tombstone](std::vector<std::optional<net::Bytes>> answers) mutable {
                  for (const auto& answer : answers) {
                    bool held = false;
                    if (answer) try {
                        held = !decode_accept_answer(*answer).refused;
                      } catch (const net::DecodeError&) {
                      }
                    if (!held) {
                      tombstone.why = "the tombstone is committed but not every replica holds it; it stays, and reads as absent";
                      return done(std::move(tombstone));
                    }
                  }
                  fan_out(send, set.nodes, net::message::agree_purge, encode(PurgeMessage{space, Purge{set, slot, tombstone.ballot, names}, fingerprint}),
                          deadline, executor, [done = std::move(done), tombstone](std::vector<std::optional<net::Bytes>> purged) mutable {
                            tombstone.forgotten = std::all_of(purged.begin(), purged.end(), [](const std::optional<net::Bytes>& answer) {
                              try {
                                return answer && !decode_accept_answer(*answer).refused;
                              } catch (const net::DecodeError&) {
                                return false;
                              }
                            });
                            if (!tombstone.forgotten) tombstone.why = "some replicas did not purge; the tombstone stays there, and reads as absent";
                            done(std::move(tombstone));
                          });
                });
      });
}

void Agreement::catch_up(std::string space, Slot slot, std::vector<Name> names, std::vector<identity::NodeId> replicas, uint64_t version,
                         std::chrono::milliseconds deadline, asio::any_io_executor executor, std::function<void(Outcome)> done) {
  const auto set = ReplicaSet::of(replicas, version);
  auto core = _core;
  // This node's store refuses to vote while catching up, so the read commits on a majority
  // of the others; what committed is installed here at its ballot, and the slot recorded as
  // served by the set, which the committed read proves.
  read(space, slot, names, replicas, version, {}, deadline, executor, [core, space, slot, names, set, done = std::move(done)](Outcome outcome) mutable {
    const auto served = core->find(space);
    if (!outcome.committed || !served) return done(std::move(outcome));
    // Every state is replaced whole, so none is loaded with its value.
    served->store->load(slot, names, false, false, [core, space, served, slot, set, outcome = std::move(outcome), done = std::move(done)](Held held) mutable {
      for (size_t i = 0; i < held.states.size() && i < outcome.values.size(); ++i)
        held.states[i] = AcceptorState{outcome.ballot, outcome.ballot, outcome.values[i], outcome.changed_at[i], {}, outcome.values[i].has_value()};
      held.slot.served = set;
      held.slot.pending_from.reset();
      served->store->keep(slot, held, [core, space, slot, record = held.slot, done = std::move(done), outcome = std::move(outcome)](bool kept) mutable {
        if (kept) core->note(space, slot, record);
        if (!kept) {
          outcome.committed = false;
          outcome.why = "the store could not keep what was caught up";
        }
        done(std::move(outcome));
      });
    });
  });
}

std::optional<net::Message> Agreement::serve_request(const identity::NodeId& from, const net::Message& request, std::function<void(net::Message)> answer) {
  return _core->serve(from, request, std::move(answer));
}

}  // namespace dannan::agree
