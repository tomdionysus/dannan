//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <string_view>

// What a process knows of itself, for the stack: Dannan's telemetry, Dagda's
// memory accounting, anyone naming a thread. One home (Tom, 2026-11-03).
namespace dannan::status {

// This process's resident memory, in bytes; zero where it cannot be read.
uint64_t resident_bytes() noexcept;
// The machine's physical memory, in bytes; zero where it cannot be read.
uint64_t physical_memory_bytes() noexcept;
// Names the calling thread, as debuggers, `top -H` and crash reports show
// it: cut to what the platform keeps (15 bytes on Linux). Best effort.
void set_thread_name(std::string_view name) noexcept;

}  // namespace dannan::status
