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
// this process. Taken from 10000 to 32767, below every kernel's ephemeral
// range (Linux from 32768, macOS from 49152): a port the kernel picked could
// go to some other process's outgoing connection before the test binds it.
// Each process starts at its own point in the range, so test binaries run
// side by side (another project's among them) rarely probe the same ports;
// each candidate is bound to see that it is free, and the set keeps two
// listeners in one case from being handed the same number before either binds.
inline uint16_t free_port() {
  constexpr uint16_t low = 10000;
  constexpr uint16_t span = 32768 - low;
  static std::mutex mutex;
  static std::set<uint16_t> handed_out;
  static uint32_t next = static_cast<uint32_t>(::getpid()) * 2654435761u % span;
  std::lock_guard lock(mutex);
  for (uint32_t tried = 0; tried < span; ++tried) {
    const auto port = static_cast<uint16_t>(low + next);
    next = (next + 1) % span;
    if (handed_out.count(port)) continue;
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("free_port: socket failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    const bool bound = ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    ::close(fd);
    if (!bound) continue;
    handed_out.insert(port);
    return port;
  }
  throw std::runtime_error("free_port: no unused loopback port below the ephemeral range");
}

}  // namespace dannan::test
