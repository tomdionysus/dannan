#
# Dannan - The Distributed Substrate
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# Debian 13 with its own Boost and compiler, as the nodes have them: the tree's Linux
# build, with and without the sanitizers. run.sh is what runs in it.
FROM debian:trixie-slim
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ cmake ninja-build libboost-dev libboost-system-dev libssl-dev libyaml-cpp-dev libgtest-dev libgmock-dev openssl ca-certificates \
 && rm -rf /var/lib/apt/lists
