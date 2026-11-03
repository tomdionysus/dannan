//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <mutex>
#include <set>
#include <stdexcept>

namespace dannan::test {

// A loopback TCP port nothing is listening on, never the same one twice in
// this process: the kernel picks it, and the set keeps two listeners in one
// case from being handed the same number before either binds.
inline uint16_t free_port() {
  static std::mutex mutex;
  static std::set<uint16_t> handed_out;
  std::lock_guard lock(mutex);
  for (int attempt = 0; attempt < 64; ++attempt) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("free_port: socket failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    socklen_t size = sizeof(address);
    const bool bound =
        ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0;
    ::close(fd);
    if (!bound) continue;
    const uint16_t port = ntohs(address.sin_port);
    if (handed_out.insert(port).second) return port;
  }
  throw std::runtime_error("free_port: no unused loopback port found");
}

}  // namespace dannan::test
