//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/identity/node_identity.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

#include "dannan/identity/durable_file.h"

namespace dannan::identity {

StateLock::StateLock(const std::filesystem::path& root, std::string_view name) {
  std::filesystem::create_directories(root);
  const auto path = root / std::string(name);
  _fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (_fd < 0) throw std::runtime_error("cannot open the state lock " + path.string() + ": " + std::strerror(errno));
  if (::flock(_fd, LOCK_EX | LOCK_NB) != 0) {
    const std::string why = std::strerror(errno);
    ::close(_fd);
    _fd = -1;
    throw std::runtime_error("the state path " + root.string() + " is already in use: " + why);
  }
}

StateLock::~StateLock() {
  if (_fd < 0) return;
  (void)::flock(_fd, LOCK_UN);
  ::close(_fd);
}

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

NodeIdentity::NodeIdentity(const std::filesystem::path& state, ClusterKey cluster_key)
    : key(std::move(cluster_key)), id(load_or_create_node_id(state)), durability_epoch(NodeId::random()) {}

}  // namespace dannan::identity
