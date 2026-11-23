//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "dannan/identity/argon2.h"

// A stored password, for the whole stack (TODO/2026-11-20-stack-plan.md, item 11; Lugh's and
// Kvasir's, moved here so both link one copy): Argon2id with a fresh 16-byte salt and a 32-byte
// hash, kept with the costs it was made under so the costs can rise without anyone's password
// being lost. Each function that spends waits on the CPU and memory the costs name, so it runs on
// a worker; none takes a lock.
namespace dannan::identity {

inline constexpr size_t password_salt_bytes = 16;
inline constexpr size_t password_hash_bytes = 32;

// Binary fields, so a record may keep them as they are; the PHC string is the text form.
struct PasswordHash {
  Argon2Costs costs;
  std::array<uint8_t, password_salt_bytes> salt{};
  std::array<uint8_t, password_hash_bytes> hash{};

  bool operator==(const PasswordHash&) const = default;
};

// A new hash of `password` under `costs`, with a fresh salt. Throws std::invalid_argument for
// costs that are not valid(), before any work.
PasswordHash make_password(std::span<const uint8_t> password, const Argon2Costs& costs = {});

// Whether `password` is the one `stored` was made from, compared in constant time. False at once,
// spending nothing, for stored costs that are not valid(): costs read from a record are checked
// before anything is spent on them.
bool verify_password(std::span<const uint8_t> password, const PasswordHash& stored);

// The same work as verify_password under `costs`, for a name that has no password, so an absent
// name takes as long as a wrong password and cannot be told apart by timing. Always false.
bool verify_decoy(std::span<const uint8_t> password, const Argon2Costs& costs = {});

// Whether `stored` was made under costs other than `wanted`: make it again from the password just
// verified.
bool needs_rehash(const PasswordHash& stored, const Argon2Costs& wanted);

// The PHC string: $argon2id$v=19$m=<KiB>,t=<passes>,p=<lanes>$<salt>$<hash>, the salt and hash in
// unpadded standard base64.
std::string to_phc(const PasswordHash& stored);
// Nothing for any other text: another algorithm or version, a cost out of bounds, a salt or hash
// of another length, padding, or anything after.
std::optional<PasswordHash> from_phc(std::string_view text);

}  // namespace dannan::identity
