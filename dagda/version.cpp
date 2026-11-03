//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/version.h"

#include "dannan/version.h"

namespace dagda {

const char* library_version() noexcept { return dannan::kVersion; }

}  // namespace dagda
