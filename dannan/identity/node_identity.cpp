//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/identity/node_identity.h"

#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

#include "dannan/identity/durable_file.h"

namespace dannan::identity {

StateLock::StateLock(const std::filesystem::path& root, std::string_view name, storage::FileSystem& fs) {
  // As create_directories made them: the process's umask decides.
  if (const auto made = fs.make_directories(root, 0777); !made) throw storage::StorageError(made.error());
  const auto path = root / std::string(name);
  auto locked = fs.lock(path);
  if (!locked) {
    const auto& error = locked.error();
    if (error.operation == "open lock") throw std::runtime_error("cannot open the state lock " + path.string() + ": " + std::strerror(error.code));
    throw std::runtime_error("the state path " + root.string() + " is already in use: " + std::strerror(error.code));
  }
  _lock = std::move(locked).value();
}

StateLock::~StateLock() = default;

NodeId load_or_create_node_id(const std::filesystem::path& state) {
  std::filesystem::create_directories(state);
  const auto file = state / "node-id";
  if (std::filesystem::exists(file)) {
    std::ifstream input(file);
    std::string text;
    input >> text;
    const auto id = NodeId::parse(text);
    if (!id) throw std::runtime_error("invalid node id in " + file.string());
    return *id;
  }
  const auto id = NodeId::random();
  durable_replace_file(file, to_string(id) + "\n");
  return id;
}

NodeIdentity::NodeIdentity(const std::filesystem::path& state, ClusterKey cluster_key, std::optional<NodeId> epoch)
    : key(std::move(cluster_key)), id(load_or_create_node_id(state)), durability_epoch(epoch ? *epoch : NodeId::random()) {}

}  // namespace dannan::identity
