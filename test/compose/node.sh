#!/bin/sh
#
# Dannan - The Distributed Substrate
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# A node of the bed: routes through its NAT router if it has one, writes
# the cluster key, and runs dannan-node, which reports on stdout.
#
#   NAME            the node's name
#   ADVERTISE       its own address
#   INBOUND         yes | no | auto
#   RELAY           1 to forward circuits for others
#   GATEWAY         its router, if behind one
#   BOOTSTRAP       space-separated HOST:PORT seeds
#   CLUSTER_SECRET  the cluster key file's contents
set -eu

if [ -n "${GATEWAY:-}" ]; then ip route replace default via "$GATEWAY"; fi
printf '%s' "$CLUSTER_SECRET" > /tmp/cluster.key

set -- --key /tmp/cluster.key --state /tmp/state --listen 0.0.0.0 --port 7437 --advertise "$ADVERTISE" --inbound "$INBOUND" \
  --name "$NAME" --heartbeat-ms 500 --status-ms 1000 --echo-ms 2000 --reprobe-ms 15000
if [ "${RELAY:-0}" = "1" ]; then set -- "$@" --relay; fi
for seed in ${BOOTSTRAP:-}; do set -- "$@" --bootstrap "$seed"; done
exec /build/dannan-node "$@"
