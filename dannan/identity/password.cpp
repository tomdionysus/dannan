//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/identity/password.h"

#include <algorithm>
#include <stdexcept>

#include "dannan/encoding/base.h"
#include "dannan/identity/crypto.h"

namespace dannan::identity {
namespace {

constexpr std::string_view phc_prefix = "$argon2id$v=19$";

// PHC's base64: the standard alphabet, unpadded.
std::string unpadded_base64(std::span<const uint8_t> data) {
  auto out = encoding::base64(data);
  while (!out.empty() && out.back() == '=') out.pop_back();
  return out;
}

template <size_t N>
std::optional<std::array<uint8_t, N>> exact_base64(std::string_view text) {
  if (text.find('=') != std::string_view::npos) return std::nullopt;
  std::string padded(text);
  while (padded.size() % 4) padded.push_back('=');
  const auto decoded = encoding::base64_decode(padded);
  if (!decoded || decoded->size() != N) return std::nullopt;
  std::array<uint8_t, N> out{};
  std::copy(decoded->begin(), decoded->end(), out.begin());
  return out;
}

// A decimal with no sign, no leading zero and no more than a uint32 holds.
std::optional<uint32_t> decimal(std::string_view text) {
  if (text.empty() || text.size() > 10 || (text.size() > 1 && text.front() == '0')) return std::nullopt;
  uint64_t out = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return std::nullopt;
    out = out * 10 + static_cast<uint64_t>(c - '0');
  }
  if (out > UINT32_MAX) return std::nullopt;
  return static_cast<uint32_t>(out);
}

// `text` from its start up to `stop`, or nothing if `stop` is not there; `text` moves past it.
std::optional<std::string_view> take_until(std::string_view& text, char stop) {
  const auto at = text.find(stop);
  if (at == std::string_view::npos) return std::nullopt;
  const auto out = text.substr(0, at);
  text.remove_prefix(at + 1);
  return out;
}

std::optional<uint32_t> named(std::string_view field, std::string_view name) {
  if (field.size() <= name.size() || field.substr(0, name.size()) != name) return std::nullopt;
  return decimal(field.substr(name.size()));
}

std::array<uint8_t, password_hash_bytes> hash_of(std::span<const uint8_t> password, std::span<const uint8_t> salt, const Argon2Costs& costs) {
  const auto tag = argon2id(password, salt, costs, password_hash_bytes);
  std::array<uint8_t, password_hash_bytes> out{};
  std::copy(tag.reveal().begin(), tag.reveal().end(), out.begin());
  return out;
}

}  // namespace

PasswordHash make_password(std::span<const uint8_t> password, const Argon2Costs& costs) {
  if (!costs.valid()) throw std::invalid_argument("password costs out of bounds");
  PasswordHash out;
  out.costs = costs;
  const auto salt = random_bytes(password_salt_bytes);
  std::copy(salt.begin(), salt.end(), out.salt.begin());
  out.hash = hash_of(password, out.salt, costs);
  return out;
}

bool verify_password(std::span<const uint8_t> password, const PasswordHash& stored) {
  if (!stored.costs.valid()) return false;
  auto computed = hash_of(password, stored.salt, stored.costs);
  const bool same = constant_time_equal(computed, stored.hash);
  cleanse(computed);
  return same;
}

bool verify_decoy(std::span<const uint8_t> password, const Argon2Costs& costs) {
  if (!costs.valid()) return false;
  // A fixed salt is enough: nothing is stored or compared, only the work is spent.
  static constexpr std::array<uint8_t, password_salt_bytes> salt{0x64, 0x65, 0x63, 0x6f, 0x79};
  auto computed = hash_of(password, salt, costs);
  cleanse(computed);
  return false;
}

bool needs_rehash(const PasswordHash& stored, const Argon2Costs& wanted) { return stored.costs != wanted; }

std::string to_phc(const PasswordHash& stored) {
  return std::string(phc_prefix) + "m=" + std::to_string(stored.costs.memory_kib) + ",t=" + std::to_string(stored.costs.iterations) +
         ",p=" + std::to_string(stored.costs.lanes) + "$" + unpadded_base64(stored.salt) + "$" + unpadded_base64(stored.hash);
}

std::optional<PasswordHash> from_phc(std::string_view text) {
  if (text.substr(0, phc_prefix.size()) != phc_prefix) return std::nullopt;
  text.remove_prefix(phc_prefix.size());
  const auto m = take_until(text, ',');
  const auto t = m ? take_until(text, ',') : std::nullopt;
  const auto p = t ? take_until(text, '$') : std::nullopt;
  const auto salt_text = p ? take_until(text, '$') : std::nullopt;
  if (!salt_text) return std::nullopt;
  const auto memory = named(*m, "m="), passes = named(*t, "t="), lanes = named(*p, "p=");
  if (!memory || !passes || !lanes) return std::nullopt;
  PasswordHash out;
  out.costs = Argon2Costs{*memory, *passes, *lanes};
  if (!out.costs.valid()) return std::nullopt;
  // What is left is the hash, all of it: a '$' or anything else after it fails its decode.
  const auto salt = exact_base64<password_salt_bytes>(*salt_text);
  const auto hash = exact_base64<password_hash_bytes>(text);
  if (!salt || !hash) return std::nullopt;
  out.salt = *salt;
  out.hash = *hash;
  return out;
}

}  // namespace dannan::identity
