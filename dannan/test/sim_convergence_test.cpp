//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The convergence checker (TODO/2026-11-15-simulation-harness.md, Mimir's review, ruling 4):
// replicas agree once quiet, and hold every write the faults do not excuse.

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "dannan/test/fixtures/sim/convergence.h"

namespace dannan::test::sim {
namespace {

// The owner's merge here: a value is a set of bytes; it holds a write if it holds each byte.
bool holds(const Bytes& value, const Bytes& write) {
  return std::all_of(write.begin(), write.end(), [&](uint8_t b) { return std::find(value.begin(), value.end(), b) != value.end(); });
}

MergeWrite write(uint8_t b, AckLevel level = AckLevel::replicated, uint64_t node = 0, uint64_t at = 10) {
  return MergeWrite{Bytes{b}, level, node, at, at + 1};
}

std::vector<Replica> replicas(std::vector<Bytes> values) {
  std::vector<Replica> out;
  for (size_t i = 0; i < values.size(); ++i) out.push_back(Replica{i, values[i]});
  return out;
}

TEST(Convergence, ReplicasThatAgreeAndHoldEveryWritePass) {
  const auto verdict = check_convergence(replicas({{1, 2}, {1, 2}, {1, 2}}), {write(1), write(2)}, {}, 3, holds);
  EXPECT_TRUE(verdict.ok) << verdict.why;
  EXPECT_TRUE(verdict.converged);
}

TEST(Convergence, AReplicaThatDiffersIsRefused) {
  const auto verdict = check_convergence(replicas({{1, 2}, {1}, {1, 2}}), {write(1)}, {}, 3, holds);
  EXPECT_FALSE(verdict.ok);
  EXPECT_FALSE(verdict.converged);
  EXPECT_NE(verdict.why.find("node 1"), std::string::npos) << verdict.why;
}

TEST(Convergence, AReplicaHoldingNothingDiffersFromOneHoldingTheEmptyValue) {
  std::vector<Replica> some{{0, Bytes{}}, {1, std::nullopt}};
  EXPECT_FALSE(check_convergence(some, {}, {}, 2, holds).converged);
}

// A replicated write is lost only with a majority's disks.
TEST(Convergence, AReplicatedWriteLostIsADefectUnlessAMajoritysDisksWent) {
  const auto agreed = replicas({{1}, {1}, {1}});
  const auto plain = check_convergence(agreed, {write(1), write(2)}, {}, 3, holds);
  EXPECT_FALSE(plain.ok);
  EXPECT_EQ(plain.lost, (std::vector<size_t>{1}));
  const std::vector<Fault> one{{20, "wipe", {0}, ""}};
  EXPECT_FALSE(check_convergence(agreed, {write(1), write(2)}, one, 3, holds).ok);
  const std::vector<Fault> two{{20, "wipe", {0, 1}, ""}};
  const auto excused = check_convergence(agreed, {write(1), write(2)}, two, 3, holds);
  EXPECT_TRUE(excused.ok) << excused.why;
  EXPECT_EQ(excused.excused, (std::vector<size_t>{1}));
}

// An ephemeral write is lost with its node; a fault before it was written excuses nothing.
TEST(Convergence, AnEphemeralWriteIsLostWithItsNodeAfterItWasWritten) {
  const auto agreed = replicas({{}, {}, {}});
  const std::vector<Fault> later{{20, "kill", {2}, ""}};
  EXPECT_TRUE(check_convergence(agreed, {write(5, AckLevel::ephemeral, 2)}, later, 3, holds).ok);
  const std::vector<Fault> other{{20, "kill", {1}, ""}};
  EXPECT_FALSE(check_convergence(agreed, {write(5, AckLevel::ephemeral, 2)}, other, 3, holds).ok);
  const std::vector<Fault> earlier{{5, "kill", {2}, ""}};
  EXPECT_FALSE(check_convergence(agreed, {write(5, AckLevel::ephemeral, 2)}, earlier, 3, holds).ok);
}

}  // namespace
}  // namespace dannan::test::sim
