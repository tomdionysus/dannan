//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/contract/thread_safety.h"

namespace dannan::contract {

// The analysis needs an object to name; nothing reads or writes it.
NoIo no_io;

}  // namespace dannan::contract
