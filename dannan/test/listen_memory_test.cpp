//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The in-memory carrier (TODO/2026-11-20-stack-plan.md, item 17; Lir's, Kvasir's and Lugh's
// asks): a pipe that behaves as a TCP connection does, on one executor or two, with the test's
// hand on it: bytes in pieces it chooses, a stall, a resume, a sever, a half-close.

#include <gtest/gtest.h>

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <optional>
#include <string>
#include <vector>

#include "dannan/listen/memory.h"

namespace dannan::listen {
namespace {

namespace asio = boost::asio;
using error_code = boost::system::error_code;

struct Pipe {
  asio::io_context io;
  MemoryPair pair = memory_pair(io.get_executor(), io.get_executor());
  Carrier& client() { return *pair.client; }
  Carrier& server() { return *pair.server; }
  MemoryLink& link() { return *pair.link; }

  void write(Carrier& from, const std::string& text) {
    std::optional<error_code> result;
    asio::async_write(from, asio::buffer(text), [&](error_code e, std::size_t) { result = e; });
    io.run();
    io.restart();
    ASSERT_TRUE(result);
    ASSERT_FALSE(*result) << result->message();
  }
  // One read_some of at most `most` bytes: what came, and the error.
  std::pair<std::string, error_code> read_some(Carrier& at, size_t most = 1024) {
    std::string into(most, '\0');
    std::optional<std::pair<size_t, error_code>> result;
    at.async_read_some(asio::buffer(into), [&](error_code e, std::size_t n) { result = std::make_pair(n, e); });
    io.run();
    io.restart();
    if (!result) return {"", asio::error::would_block};
    into.resize(result->first);
    return {into, result->second};
  }
};

TEST(MemoryCarrier, BytesWrittenAtOneEndAreReadAtTheOther) {
  Pipe p;
  p.write(p.client(), "hello");
  auto [text, error] = p.read_some(p.server());
  EXPECT_FALSE(error);
  EXPECT_EQ(text, "hello");
  p.write(p.server(), "back");
  EXPECT_EQ(p.read_some(p.client()).first, "back");
}

TEST(MemoryCarrier, ACompletionIsPostedNeverCalledInline) {
  Pipe p;
  bool done = false;
  const std::string text = "x";
  p.client().async_write_some(asio::buffer(text), [&](error_code, std::size_t) { done = true; });
  EXPECT_FALSE(done);
  p.io.run();
  EXPECT_TRUE(done);
}

TEST(MemoryCarrier, AReadWaitsForBytesThenTakesThem) {
  Pipe p;
  std::string into(16, '\0');
  std::optional<size_t> got;
  p.server().async_read_some(asio::buffer(into), [&](error_code, std::size_t n) { got = n; });
  p.io.run();
  p.io.restart();
  EXPECT_FALSE(got) << "a read with nothing to take completed";
  const std::string text = "late";
  asio::async_write(p.client(), asio::buffer(text), [](error_code, std::size_t) {});
  p.io.run();
  ASSERT_TRUE(got);
  EXPECT_EQ(into.substr(0, *got), "late");
}

TEST(MemoryCarrier, PiecesAreTheTestsToChoose) {
  Pipe p;
  p.link().set_pieces(End::client, [](size_t) { return size_t{3}; });
  p.write(p.client(), "abcdefgh");
  EXPECT_EQ(p.read_some(p.server()).first, "abc");
  EXPECT_EQ(p.read_some(p.server()).first, "def");
  EXPECT_EQ(p.read_some(p.server()).first, "gh");
}

TEST(MemoryCarrier, AStallHoldsTheBytesAndAResumeDeliversThemInOrder) {
  Pipe p;
  p.link().stall();
  EXPECT_TRUE(p.link().stalled());
  p.write(p.client(), "one");
  p.write(p.client(), "two");
  error_code ignored;
  EXPECT_EQ(p.server().available(ignored), 0u);
  p.link().resume();
  p.io.run();
  p.io.restart();
  EXPECT_EQ(p.read_some(p.server()).first, "onetwo");
}

TEST(MemoryCarrier, AReadParkedBeforeAResumeIsAnswered) {
  Pipe p;
  p.link().stall();
  p.write(p.client(), "held");
  std::string into(16, '\0');
  std::optional<size_t> got;
  p.server().async_read_some(asio::buffer(into), [&](error_code, std::size_t n) { got = n; });
  p.io.run();
  p.io.restart();
  EXPECT_FALSE(got);
  p.link().resume();
  p.io.run();
  ASSERT_TRUE(got);
  EXPECT_EQ(into.substr(0, *got), "held");
}

TEST(MemoryCarrier, ASeverResetsBothEndsAndAnyReadWaiting) {
  Pipe p;
  std::string into(16, '\0');
  std::optional<error_code> waiting;
  p.server().async_read_some(asio::buffer(into), [&](error_code e, std::size_t) { waiting = e; });
  p.io.run();
  p.io.restart();
  p.link().sever();
  p.io.run();
  p.io.restart();
  ASSERT_TRUE(waiting);
  EXPECT_EQ(*waiting, asio::error::connection_reset);
  EXPECT_EQ(p.read_some(p.client()).second, asio::error::connection_reset);
  std::optional<error_code> wrote;
  const std::string text = "x";
  p.client().async_write_some(asio::buffer(text), [&](error_code e, std::size_t) { wrote = e; });
  p.io.run();
  ASSERT_TRUE(wrote);
  EXPECT_EQ(*wrote, asio::error::connection_reset);
  EXPECT_TRUE(p.link().severed());
}

TEST(MemoryCarrier, AHalfCloseIsTheEndOfTheStreamOnceDrained) {
  Pipe p;
  p.write(p.client(), "last");
  error_code error;
  p.client().shutdown_send(error);
  EXPECT_FALSE(error);
  EXPECT_EQ(p.read_some(p.server()).first, "last");
  EXPECT_EQ(p.read_some(p.server()).second, asio::error::eof);
  // The other way still flows.
  p.write(p.server(), "reply");
  EXPECT_EQ(p.read_some(p.client()).first, "reply");
}

TEST(MemoryCarrier, ACloseIsSeenAsTheEndAndTheClosedEndRefusesItsOwnOperations) {
  Pipe p;
  error_code error;
  p.client().close(error);
  EXPECT_FALSE(p.client().is_open());
  EXPECT_EQ(p.read_some(p.server()).second, asio::error::eof);
  EXPECT_EQ(p.read_some(p.client()).second, asio::error::bad_descriptor);
  std::optional<error_code> wrote;
  const std::string text = "x";
  p.server().async_write_some(asio::buffer(text), [&](error_code e, std::size_t) { wrote = e; });
  p.io.run();
  ASSERT_TRUE(wrote);
  EXPECT_EQ(*wrote, asio::error::broken_pipe);
}

TEST(MemoryCarrier, AWriteToAFullPipeWaitsForTheReader) {
  asio::io_context io;
  MemoryOptions options;
  options.capacity = 4;
  auto pair = memory_pair(io.get_executor(), io.get_executor(), options);
  const std::string text = "abcdefgh";
  std::optional<size_t> written;
  asio::async_write(*pair.client, asio::buffer(text), [&](error_code, std::size_t n) { written = n; });
  io.run();
  io.restart();
  EXPECT_FALSE(written) << "the whole write fitted a four-byte pipe";
  std::string into(8, '\0');
  std::optional<size_t> read;
  asio::async_read(*pair.server, asio::buffer(into), [&](error_code, std::size_t n) { read = n; });
  io.run();
  ASSERT_TRUE(written);
  EXPECT_EQ(*written, 8u);
  ASSERT_TRUE(read);
  EXPECT_EQ(into, text);
}

TEST(MemoryCarrier, CancelAbortsAReadWaiting) {
  Pipe p;
  std::string into(16, '\0');
  std::optional<error_code> result;
  p.server().async_read_some(asio::buffer(into), [&](error_code e, std::size_t) { result = e; });
  p.io.run();
  p.io.restart();
  error_code ignored;
  p.server().cancel(ignored);
  p.io.run();
  ASSERT_TRUE(result);
  EXPECT_EQ(*result, asio::error::operation_aborted);
}

TEST(MemoryCarrier, APerOperationCancelAbortsJustThatRead) {
  Pipe p;
  std::string into(16, '\0');
  std::optional<error_code> result;
  asio::cancellation_signal signal;
  p.server().async_read_some(asio::buffer(into), asio::bind_cancellation_slot(signal.slot(), [&](error_code e, std::size_t) { result = e; }));
  p.io.run();
  p.io.restart();
  signal.emit(asio::cancellation_type::terminal);
  p.io.run();
  ASSERT_TRUE(result);
  EXPECT_EQ(*result, asio::error::operation_aborted);
  p.write(p.client(), "still");
  EXPECT_EQ(p.read_some(p.server()).first, "still");
}

TEST(MemoryCarrier, EachEndKnowsTheOthersAddress) {
  asio::io_context io;
  MemoryOptions options;
  options.client_address = {asio::ip::make_address("192.0.2.7"), 51000};
  options.server_address = {asio::ip::make_address("198.51.100.1"), 3306};
  auto pair = memory_pair(io.get_executor(), io.get_executor(), options);
  error_code error;
  EXPECT_EQ(pair.server->remote_endpoint(error), options.client_address);
  EXPECT_EQ(pair.client->remote_endpoint(error), options.server_address);
  EXPECT_STREQ(pair.client->transport(), "memory");
}

TEST(MemoryCarrier, NoDelayIsKeptAndChangesNothing) {
  Pipe p;
  error_code error;
  p.client().set_no_delay(true, error);
  EXPECT_TRUE(p.client().no_delay(error));
  p.client().set_no_delay(false, error);
  EXPECT_FALSE(p.client().no_delay(error));
}

TEST(MemoryCarrier, AnEmptyReadOrWriteCompletesAtOnce) {
  Pipe p;
  std::optional<size_t> read, wrote;
  p.server().async_read_some(asio::mutable_buffer(), [&](error_code, std::size_t n) { read = n; });
  p.client().async_write_some(asio::const_buffer(), [&](error_code, std::size_t n) { wrote = n; });
  p.io.run();
  EXPECT_EQ(read, 0u);
  EXPECT_EQ(wrote, 0u);
}

}  // namespace
}  // namespace dannan::listen
