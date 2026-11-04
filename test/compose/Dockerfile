#
# Dannan - The Distributed Substrate
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# The compose test bed's one image: dannan-node built from this tree, and
# the tools a NAT router needs. Built from the repository root (compose.yaml
# says so). Arch, because Dannan needs Boost 1.87 or later with its CMake
# configuration, which Arch packages and Debian stable does not yet.
FROM archlinux:latest

RUN pacman -Syu --noconfirm --needed cmake ninja clang boost openssl iproute2 iptables-nft python \
 && pacman -Scc --noconfirm

WORKDIR /src
COPY CMakeLists.txt ./
COPY dannan dannan
COPY dagda dagda
COPY tools tools
RUN cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ -DDANNAN_WARNINGS_AS_ERRORS=ON \
 && cmake --build /build --target dannan-node

COPY test/compose/node.sh test/compose/router.sh /bed/
RUN chmod +x /bed/node.sh /bed/router.sh
