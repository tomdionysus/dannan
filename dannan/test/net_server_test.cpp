//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The server over loopback TCP: the inventory's transport rows "session
// reaping; a cap on sessions not yet authenticated", "a probe lane that
// authenticates and closes", and "BBR, keepalive and no Nagle on every peer
// socket".

#include <gtest/gtest.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <array>
#include <fstream>
#include <future>
#include <thread>

#include "dannan/net/server.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dannan::net {
namespace {

namespace asio = boost::asio;
using namespace std::chrono_literals;
using sched::FrameType;

struct Io {
  asio::io_context context;
  asio::executor_work_guard<asio::io_context::executor_type> work{context.get_executor()};
  std::vector<std::thread> threads;
  Io() {
    for (int i = 0; i < 3; ++i) threads.emplace_back([this] { context.run(); });
  }
  ~Io() {
    work.reset();
    context.stop();
    for (auto& thread : threads) thread.join();
  }
};

identity::ClusterKey key_from(const std::string& secret) {
  test::TempDir dir;
  std::ofstream(dir.path() / "k") << secret;
  return identity::load_cluster_key(dir.path() / "k");
}

membership::NodeRecord record(const std::string& host) {
  membership::NodeRecord node;
  node.id = identity::NodeId::random();
  node.host = host;
  return node;
}

std::shared_ptr<const RouteTable> routes() { return std::make_shared<const RouteTable>(RouteTable::with_dannan_types()); }

ServerOptions options(const identity::ClusterKey& key) {
  ServerOptions out;
  out.credentials = key;
  const auto self = record("127.0.0.1");
  out.local = [self] { return self; };
  out.routes = routes();
  out.handshake_timeout = 2s;
  return out;
}

// Dials the server on `lane` and returns the established session, or the
// reason the handshake failed.
struct Dialled {
  std::shared_ptr<Session> session;
  std::string refused;
};

Dialled dial(Io& io, uint16_t port, const identity::ClusterKey& key, Lane lane = Lane::control) {
  asio::ip::tcp::socket tcp(io.context);
  tcp.connect({asio::ip::address_v4::loopback(), port});
  auto socket = std::make_shared<Session::Socket>(io.context, Session::Socket::protocol_type(AF_INET, SOCK_STREAM), tcp.release());
  std::promise<std::pair<std::exception_ptr, Established>> done;
  async_dial_handshake(*socket, key, record("127.0.0.2"), 65536, lane,
                       [&](std::exception_ptr error, Established established) { done.set_value({error, std::move(established)}); });
  auto [error, established] = done.get_future().get();
  if (error) {
    try {
      std::rethrow_exception(error);
    } catch (const std::exception& e) {
      return {nullptr, e.what()};
    }
  }
  auto session = Session::create(std::move(*socket), std::move(established), Role::dialler, routes(), {});
  session->start();
  return {session, {}};
}

CallOutcome call(Io& io, Session& session, MessageType type) {
  auto done = std::make_shared<std::promise<CallOutcome>>();
  auto future = done->get_future();
  const auto admitted =
      session.call(type, {}, FrameType::control, io.context.get_executor(), [done](CallOutcome outcome) { done->set_value(std::move(outcome)); });
  if (!admitted.refused.empty()) return {std::nullopt, admitted.refused, {}};
  if (future.wait_for(5s) != std::future_status::ready) return {std::nullopt, "timed out", {}};
  return future.get();
}

RequestHandler members_reply() {
  return [](const membership::NodeRecord&, FrameType, const Message& request) {
    return Handled::processed({request.type == message::members ? message::members_reply : message::ok, {}, {}});
  };
}

bool eventually(const std::function<bool()>& condition) {
  for (int spin = 0; spin < 1000; ++spin) {
    if (condition()) return true;
    std::this_thread::sleep_for(5ms);
  }
  return condition();
}

TEST(Server, AnswersADialledSessionsCalls) {
  Io io;
  const auto key = key_from("cluster");
  Server server(io.context, "127.0.0.1", 0, options(key), members_reply());
  server.start();
  auto dialled = dial(io, server.bound_port(), key);
  ASSERT_TRUE(dialled.session) << dialled.refused;
  const auto outcome = call(io, *dialled.session, message::members);
  ASSERT_TRUE(outcome.reply) << outcome.failure;
  EXPECT_EQ(outcome.reply->type, message::members_reply);
  EXPECT_TRUE(eventually([&] { return server.sessions() == 1; }));
  dialled.session->close("done");
  server.stop();
}

// Finished sessions are reaped: the count follows them down.
TEST(Server, ReapsSessionsThatEnd) {
  Io io;
  const auto key = key_from("cluster");
  Server server(io.context, "127.0.0.1", 0, options(key), members_reply());
  server.start();
  std::vector<std::shared_ptr<Session>> sessions;
  for (int i = 0; i < 3; ++i) {
    auto dialled = dial(io, server.bound_port(), key);
    ASSERT_TRUE(dialled.session);
    ASSERT_TRUE(call(io, *dialled.session, message::ping).reply);
    sessions.push_back(dialled.session);
  }
  EXPECT_TRUE(eventually([&] { return server.sessions() == 3; }));
  EXPECT_EQ(server.work_stats().sessions, 3u);
  for (auto& session : sessions) session->close("done");
  EXPECT_TRUE(eventually([&] { return server.sessions() == 0; }));
  EXPECT_EQ(server.work_stats().sessions, 0u);
  server.stop();
}

// A probe authenticates and is closed: it never becomes a session.
TEST(Server, AProbeAuthenticatesAndIsNeverASession) {
  Io io;
  const auto key = key_from("cluster");
  Server server(io.context, "127.0.0.1", 0, options(key), members_reply());
  server.start();
  auto probe = dial(io, server.bound_port(), key, Lane::probe);
  ASSERT_TRUE(probe.session) << probe.refused;
  EXPECT_TRUE(eventually([&] { return !probe.session->usable(); }));
  EXPECT_EQ(server.sessions(), 0u);
  server.stop();
}

TEST(Server, RefusesAnotherClustersKey) {
  Io io;
  Server server(io.context, "127.0.0.1", 0, options(key_from("ours")), members_reply());
  server.start();
  const auto dialled = dial(io, server.bound_port(), key_from("theirs"));
  EXPECT_FALSE(dialled.session);
  EXPECT_EQ(server.sessions(), 0u);
  server.stop();
}

// Connections that have not authenticated are bounded: beyond the bound an
// idle one is closed at once; the bound frees as they go.
TEST(Server, BoundsUnauthenticatedConnections) {
  Io io;
  const auto key = key_from("cluster");
  auto opts = options(key);
  opts.max_pre_auth = 8;
  Server server(io.context, "127.0.0.1", 0, opts, members_reply());
  server.start();
  std::vector<int> idle;
  for (int i = 0; i < 16; ++i) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(server.bound_port());
    ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    idle.push_back(fd);
  }
  EXPECT_TRUE(eventually([&] { return server.pre_auth() == 8; }));
  size_t closed = 0;
  for (const int fd : idle) {
    pollfd poll_fd{fd, POLLIN, 0};
    if (::poll(&poll_fd, 1, 200) > 0) {
      uint8_t byte;
      if (::recv(fd, &byte, 1, MSG_DONTWAIT) <= 0) ++closed;
    }
  }
  EXPECT_EQ(closed, 8u);
  for (const int fd : idle) ::close(fd);
  EXPECT_TRUE(eventually([&] { return server.pre_auth() == 0; }));
  auto dialled = dial(io, server.bound_port(), key);
  ASSERT_TRUE(dialled.session);
  EXPECT_TRUE(call(io, *dialled.session, message::ping).reply);
  dialled.session->close("done");
  server.stop();
}

// A connection that never completes its handshake is closed at the timeout.
TEST(Server, ClosesAHandshakeThatNeverCompletes) {
  Io io;
  const auto key = key_from("cluster");
  auto opts = options(key);
  opts.handshake_timeout = 200ms;
  Server server(io.context, "127.0.0.1", 0, opts, members_reply());
  server.start();
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(server.bound_port());
  ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
  pollfd poll_fd{fd, POLLIN, 0};
  ASSERT_GT(::poll(&poll_fd, 1, 3000), 0);
  uint8_t byte;
  EXPECT_LE(::recv(fd, &byte, 1, 0), 0);
  ::close(fd);
  EXPECT_TRUE(eventually([&] { return server.pre_auth() == 0; }));
  server.stop();
}

// Keepalive probing at 60 s keeps a NAT mapping warm; no Nagle delay.
TEST(PeerSocket, KeepsAliveAtSixtySecondsWithoutNagle) {
  asio::io_context io;
  asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
  asio::ip::tcp::socket dialled(io);
  dialled.connect(acceptor.local_endpoint());
  auto accepted = acceptor.accept();
  for (auto* socket : {&dialled, &accepted}) {
    const int fd = socket->native_handle();
    (void)peer_socket_options(fd, status::ScopedLogger(status::null_sink(), "net"));
    int value = 0;
    socklen_t length = sizeof(value);
    ASSERT_EQ(::getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, &length), 0);
    EXPECT_NE(value, 0);
    ASSERT_EQ(::getsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &value, &length), 0);
    EXPECT_NE(value, 0);
#ifdef TCP_KEEPIDLE
    ASSERT_EQ(::getsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &value, &length), 0);
#else
    ASSERT_EQ(::getsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &value, &length), 0);
#endif
    EXPECT_EQ(value, 60);
  }
}

// A hello whose whole range lies above ours is closed without a word: nothing tells
// a stranger what we speak.
TEST(Server, ClosesAHelloOfAnotherVersionUnanswered) {
  Io io;
  const auto key = key_from("cluster");
  Server server(io.context, "127.0.0.1", 0, options(key), [](const membership::NodeRecord&, FrameType, const Message&) { return Handled::processed(); });
  server.start();
  ClientHandshake hello(key, record("127.0.0.1"), 65536, Lane::control);
  auto blob = hello.hello_blob();
  // The blob's length, the hello's length, then its version range.
  blob[8] = 0;
  blob[9] = 9;
  blob[10] = 0;
  blob[11] = 9;
  asio::ip::tcp::socket tcp(io.context);
  tcp.connect({asio::ip::address_v4::loopback(), server.bound_port()});
  asio::write(tcp, asio::buffer(blob));
  std::array<uint8_t, 16> answer{};
  boost::system::error_code error;
  const auto got = tcp.read_some(asio::buffer(answer), error);
  EXPECT_EQ(got, 0u);
  EXPECT_TRUE(error);
  EXPECT_EQ(server.sessions(), 0u);
  server.stop();
}

// Where this process may choose the congestion control, both ends carry bbr and the
// call says so. Only Linux has the choice: elsewhere there is nothing to assert.
TEST(PeerSocket, ChoosesBbrWhereOffered) {
#ifdef TCP_CONGESTION
  asio::io_context io;
  asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
  asio::ip::tcp::socket dialled(io);
  dialled.connect(acceptor.local_endpoint());
  auto accepted = acceptor.accept();
  const int plain = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(plain, 0);
  static constexpr char bbr[] = "bbr";
  const bool offered = ::setsockopt(plain, IPPROTO_TCP, TCP_CONGESTION, bbr, sizeof(bbr) - 1) == 0;
  ::close(plain);
  const status::ScopedLogger quiet(status::null_sink(), "net");
  for (auto* socket : {&dialled, &accepted}) {
    EXPECT_EQ(peer_socket_options(socket->native_handle(), quiet), offered);
    if (!offered) continue;
    char chosen[16] = {};
    socklen_t length = sizeof(chosen);
    ASSERT_EQ(::getsockopt(socket->native_handle(), IPPROTO_TCP, TCP_CONGESTION, chosen, &length), 0);
    EXPECT_STREQ(chosen, "bbr");
  }
#else
  GTEST_SKIP() << "no choice of congestion control here";
#endif
}

}  // namespace
}  // namespace dannan::net
