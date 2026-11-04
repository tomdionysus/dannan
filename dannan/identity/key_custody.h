//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "dannan/identity/argon2.h"
#include "dannan/identity/secure_memory.h"
#include "dannan/identity/signing.h"

// Node key custody, the early slice of step 10: what a node holds that
// must never be on disk in clear. The node's private key (ES256, the key
// step 10's certificate will name) and a seal root that hosted systems
// derive node-local keys from (Dagda seals its node-local domain keys
// under one), both sealed in `<state>/identity/custody` under a root that
// is itself never on disk in clear.
//
// The root comes from one of two places. A systemd credential: made at
// create, handed to the operator to seal (`systemd-creds encrypt`, bound to
// the TPM where there is one), and unsealed by the unit at each start into
// `$CREDENTIALS_DIRECTORY`. Or a passphrase typed at each start, stretched
// with Argon2id under the salt and costs the file records.
//
// The file: "DNNKEY01", the source (the credential's name, or the Argon2id
// costs and salt), a random file id, then the body sealed with AES-256-GCM
// under HKDF(root, file id, "dannan/custody/1") with everything before it
// as associated data. A wrong root and an altered file are refused alike.
namespace dannan::identity {

struct KeySource {
  enum class Kind : uint8_t { credential = 1, passphrase = 2 };

  // A systemd credential `name`, read from `directory` (empty:
  // $CREDENTIALS_DIRECTORY).
  static KeySource systemd_credential(std::string name, std::filesystem::path directory = {});
  // A passphrase, asked once per create or open; `costs` apply at create,
  // and open uses what the file records.
  static KeySource typed_passphrase(std::function<Secret()> ask, Argon2Costs costs = {});

  Kind kind = Kind::credential;
  std::string credential;
  std::filesystem::path credentials_directory;
  std::function<Secret()> ask;
  Argon2Costs costs;
};

// A node's custody, open. Immutable; thread-safe.
class KeyCustody {
 public:
  static bool exists(const std::filesystem::path& state);
  // Makes custody: a fresh node key and seal root, sealed under the root
  // the source names. For a credential the root is new and returned, for
  // the operator to seal and never to store in clear; for a passphrase
  // nothing is returned. Throws std::runtime_error if custody exists: it is
  // never replaced. Waits on the state device (and Argon2id).
  static std::optional<Secret> create(const std::filesystem::path& state, const KeySource& source);
  // Throws std::runtime_error, saying what is wrong in the operator's terms:
  // no custody, the credential missing, or a root that does not open it
  // (wrong, or the file altered). Waits on the state device (and Argon2id).
  static KeyCustody open(const std::filesystem::path& state, const KeySource& source);

  const SigningKey& node_key() const noexcept { return _node_key; }
  // A key for a hosted system's node-local use, derived from the seal root
  // under `label` (its own: "dagda/node-local/1"). Throws
  // std::invalid_argument for an empty label.
  Secret derive(std::string_view label) const;

 private:
  KeyCustody(SigningKey node_key, Secret seal_root) : _node_key(std::move(node_key)), _seal_root(std::move(seal_root)) {}

  SigningKey _node_key;
  Secret _seal_root;
};

}  // namespace dannan::identity
