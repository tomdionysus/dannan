#!/bin/sh
#
# Dannan - The Distributed Substrate
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# A NAT router: forwards between its networks and masquerades what leaves
# by its outside address. NAT=eim keeps a connection's source port where it
# can (an endpoint-independent mapping: punchable); NAT=random picks a fresh
# port for every connection (relay only).
#
#   OUTSIDE_IP  this router's address on the outside network
#   NAT         eim | random
#   GATEWAY     its default route: the outside network's gateway, or the
#               carrier router for the inner router of a carrier pair
set -eu

outside=$(ip -o -4 addr show | while read -r _ name _ address _; do
  if [ "${address%/*}" = "$OUTSIDE_IP" ]; then echo "$name"; fi
done)
if [ -z "$outside" ]; then
  echo "router: no interface holds $OUTSIDE_IP" >&2
  exit 1
fi
if [ -n "${GATEWAY:-}" ]; then ip route replace default via "$GATEWAY"; fi

case "$NAT" in
  eim) iptables -t nat -A POSTROUTING -o "$outside" -j MASQUERADE ;;
  random) iptables -t nat -A POSTROUTING -o "$outside" -j MASQUERADE --random-fully ;;
  *) echo "router: NAT is eim or random" >&2; exit 1 ;;
esac
iptables -P FORWARD ACCEPT
echo "router: $NAT NAT out of $outside ($OUTSIDE_IP)"
exec sleep infinity
