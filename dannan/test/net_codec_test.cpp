//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The wire codec: protocol 23's encoding of integers, blobs and strings,
// byte for byte, and decoding that refuses rather than guesses. No byte on
// the wire changes before level 0.

#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <string>

#include "dannan/net/codec.h"

namespace dannan::net {
namespace {

// Big-endian, as every protocol-23 record is written.
TEST(Codec, IntegersAreBigEndian) {
  Writer writer;
  writer.u8(0x01);
  writer.u16(0x0203);
  writer.u32(0x04050607);
  writer.u64(0x08090a0b0c0d0e0full);
  const Bytes expected{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
  EXPECT_EQ(writer.data(), expected);
}

// A blob and a string are a u32 length then the bytes; a fixed array is raw.
TEST(Codec, BlobsAndStringsCarryAFourByteLength) {
  Writer writer;
  writer.bytes(Bytes{0xaa, 0xbb});
  writer.string("hi");
  writer.fixed(std::array<uint8_t, 3>{1, 2, 3});
  const Bytes expected{0, 0, 0, 2, 0xaa, 0xbb, 0, 0, 0, 2, 'h', 'i', 1, 2, 3};
  EXPECT_EQ(writer.data(), expected);
}

TEST(Codec, RoundTrips) {
  Writer writer;
  writer.u8(255);
  writer.u16(65535);
  writer.u32(std::numeric_limits<uint32_t>::max());
  writer.u64(std::numeric_limits<uint64_t>::max());
  writer.i64(-2);
  writer.string("macha");
  writer.bytes(Bytes(1000, 7));
  writer.fixed(std::array<uint8_t, 16>{9});
  const auto data = writer.take();
  Reader reader(data);
  EXPECT_EQ(reader.u8(), 255);
  EXPECT_EQ(reader.u16(), 65535);
  EXPECT_EQ(reader.u32(), std::numeric_limits<uint32_t>::max());
  EXPECT_EQ(reader.u64(), std::numeric_limits<uint64_t>::max());
  EXPECT_EQ(reader.i64(), -2);
  EXPECT_EQ(reader.string(), "macha");
  EXPECT_EQ(reader.bytes(), Bytes(1000, 7));
  EXPECT_EQ((reader.fixed<16>()), (std::array<uint8_t, 16>{9}));
  EXPECT_EQ(reader.remaining(), 0u);
  EXPECT_NO_THROW(reader.finish());
}

TEST(Codec, ATruncatedInputIsRefused) {
  const Bytes data{0, 0, 0};
  Reader reader(data);
  EXPECT_THROW(reader.u32(), DecodeError);
  const Bytes short_blob{0, 0, 0, 5, 1, 2};
  Reader blob(short_blob);
  EXPECT_THROW(blob.bytes(), DecodeError);
}

// A length larger than the caller allows is refused before anything is read
// or allocated: a peer cannot make a node allocate what it claims.
TEST(Codec, ABlobOverItsBoundIsRefused) {
  Writer writer;
  writer.string(std::string(100, 'x'));
  const auto data = writer.take();
  Reader reader(data);
  EXPECT_THROW(reader.string(99), DecodeError);
  Reader again(data);
  EXPECT_EQ(again.string(100).size(), 100u);
}

TEST(Codec, TrailingInputIsRefused) {
  const Bytes data{1, 2};
  Reader reader(data);
  reader.u8();
  EXPECT_THROW(reader.finish(), DecodeError);
}

TEST(Codec, ViewsBorrowWithoutCopying) {
  Writer writer;
  writer.bytes(Bytes{1, 2, 3});
  const auto data = writer.take();
  Reader reader(data);
  const auto view = reader.view_bytes();
  ASSERT_EQ(view.size(), 3u);
  EXPECT_EQ(view.data(), data.data() + 4);
}

}  // namespace
}  // namespace dannan::net
