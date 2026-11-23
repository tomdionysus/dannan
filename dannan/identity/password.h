//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

/// @file
/// A stored password: Argon2id, its PHC string, constant-time verification and a decoy.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "dannan/identity/argon2.h"

namespace dannan::identity {

/// A stored password's salt length in bytes.
inline constexpr size_t password_salt_bytes = 16;
/// A stored password's hash length in bytes.
inline constexpr size_t password_hash_bytes = 32;

/// A stored password (0.12.0; Lugh's and Kvasir's, moved here so both link one copy): Argon2id
/// over the password and a fresh random salt, kept with the costs it was made under.
///
/// Why the costs are kept: so they can rise without anyone's password being lost. A hash made
/// under old costs still verifies; needs_rehash() says when to make it again, from the password
/// the user has just given.
///
/// The fields are binary, so a record may keep them as they are; to_phc() is the text form.
///
/// @code
/// // At sign-up, on a worker: the spend is the costs' (64 MiB and three passes by default).
/// auto stored = identity::make_password(password_bytes);
/// record.password = identity::to_phc(stored);
///
/// // At sign-in.
/// auto parsed = identity::from_phc(record.password);
/// if (!parsed) { identity::verify_decoy(attempt_bytes); return refuse(); }  // same work
/// if (!identity::verify_password(attempt_bytes, *parsed)) return refuse();
/// if (identity::needs_rehash(*parsed, current_costs))
///   record.password = identity::to_phc(identity::make_password(attempt_bytes, current_costs));
/// @endcode
struct PasswordHash {
  /// The Argon2id costs it was made under.
  Argon2Costs costs;
  /// Its salt, fresh for each make_password().
  std::array<uint8_t, password_salt_bytes> salt{};
  /// Argon2id of the password under the salt and costs.
  std::array<uint8_t, password_hash_bytes> hash{};

  /// Equal when every field is.
  bool operator==(const PasswordHash&) const = default;
};

/// Makes a new stored password under a fresh salt.
///
/// Waits on the CPU and memory `costs` name, so it runs on a worker, never an io thread; takes no
/// lock.
/// @param password the password's bytes, of any length, empty included (RFC 9106 allows it).
/// @param costs the Argon2id costs; the default is RFC 9106's second recommendation.
/// @return the stored password.
/// @throws std::invalid_argument for costs that are not Argon2Costs::valid(), before any work.
PasswordHash make_password(std::span<const uint8_t> password, const Argon2Costs& costs = {});

/// Whether `password` is the one `stored` was made from, compared in constant time.
///
/// Costs read from a record are checked before anything is spent on them, so a record whose costs
/// are out of bounds is refused at once and cannot make the node allocate gigabytes. Waits on the
/// CPU and memory the stored costs name; takes no lock.
/// @param password the password given.
/// @param stored what was kept.
/// @return true when it matches; false when it does not, or at once, spending nothing, when the
///   stored costs are not Argon2Costs::valid().
bool verify_password(std::span<const uint8_t> password, const PasswordHash& stored);

/// Spends the same work as verify_password() under `costs`, for a name that has no password.
///
/// Why: an absent name must take as long as a wrong password, or timing tells an attacker which
/// names exist.
/// @param password the password given.
/// @param costs the costs a real stored password would have; invalid costs spend nothing.
/// @return always false.
bool verify_decoy(std::span<const uint8_t> password, const Argon2Costs& costs = {});

/// Whether `stored` was made under costs other than `wanted`. When true, make the password again
/// from the one just verified and store that.
bool needs_rehash(const PasswordHash& stored, const Argon2Costs& wanted);

/// The PHC string form: `$argon2id$v=19$m=<KiB>,t=<passes>,p=<lanes>$<salt>$<hash>`, the salt and
/// hash in unpadded standard base64, as the reference argon2 tool writes it.
std::string to_phc(const PasswordHash& stored);
/// Reads a PHC string.
/// @return the stored password, or nothing for any other text: another algorithm or version, a
///   cost out of bounds, a salt or hash of another length, padding, or anything after the hash.
std::optional<PasswordHash> from_phc(std::string_view text);

}  // namespace dannan::identity
