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
#   IDENTITY        "certificate": run by certificate alone, from
#                   /identity (compose.certificates.yaml), its custody
#                   sealed under CUSTODY_PASSPHRASE
set -eu

if [ -n "${GATEWAY:-}" ]; then ip route replace default via "$GATEWAY"; fi

set -- --cluster-state /tmp/state --cluster-listen-address 0.0.0.0 --cluster-listen-port 7437 \
  --cluster-listen-advertise "$ADVERTISE" --cluster-listen-inbound "$INBOUND" --cluster-name "$NAME" \
  --cluster-heartbeat-ms 500 --status-ms 1000 --echo-ms 2000 --cluster-reprobe-ms 15000
if [ "${IDENTITY:-}" = "certificate" ]; then
  # The identity the test made becomes the state on first start; a restart
  # keeps the state, as a node's would.
  if [ ! -d /tmp/state ]; then cp -R "/identity/$NAME" /tmp/state; fi
  set -- "$@" --cluster-custody-passphrase-env CUSTODY_PASSPHRASE --cluster-identity-ca /identity/ca.pem \
    --cluster-identity-certificate /tmp/state/node.pem --cluster-identity-no-cluster-key
  if [ -f /identity/revocations ]; then set -- "$@" --cluster-identity-revocations /identity/revocations; fi
else
  printf '%s' "$CLUSTER_SECRET" > /tmp/cluster.key
  set -- "$@" --cluster-key /tmp/cluster.key
fi
if [ "${RELAY:-0}" = "1" ]; then set -- "$@" --cluster-relay; fi
for seed in ${BOOTSTRAP:-}; do set -- "$@" --cluster-bootstrap "$seed"; done
exec /build/dannan-node "$@"
