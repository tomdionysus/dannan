//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/fuzz/fuzz.h"

namespace dannan::fuzz {

std::vector<Target>& targets() {
  static std::vector<Target> all;
  return all;
}

}  // namespace dannan::fuzz
