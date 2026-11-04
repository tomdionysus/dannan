//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <chrono>
#include <string>
#include <string_view>

namespace dannan::reach {

// The address in a lookup service's answer: an IPv4 literal, surrounding
// space trimmed. Throws std::runtime_error for anything else.
std::string external_ip_from_body(std::string_view body);

// This node's address as the internet sees it, from
// https://checkip.amazonaws.com/ over IPv4, the certificate verified
// against the system's roots and the host name. Throws with the reason.
// Waits on the network, within `timeout`.
std::string checkip_external_ip(std::chrono::milliseconds timeout);

}  // namespace dannan::reach
