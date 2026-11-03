#!/usr/bin/env python3
#
# Dannan - The Distributed Substrate
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

"""The compose bed's scenario runner (wire v1, section 7).

Builds the image, starts the bed, and asserts from each node's status lines:

  1. one cluster: every node lists every other;
  2. path classes: the hubs public, a1 and d1 punchable, b1 and c1 relay only;
  3. paths: a1 and d1 punched to each other, every other pair of NATed nodes
     relayed, everything with a hub ordinary; echo answers on both lanes over
     every one of them;
  4. a partition heals: router-d stops, d1 drops out, router-d starts, and
     a1 and d1 punch again;
  5. a relay fails over: hub1 stops, the relayed pairs move to hub2, hub1
     starts again.

Run by hand from the repository root, with Docker running:

  python3 test/compose/scenario.py [--keep] [--no-build]

Never part of the per-commit gate.
"""

import argparse
import json
import os
import subprocess
import sys
import time

COMPOSE = ["docker", "compose", "-f", os.path.join(os.path.dirname(os.path.abspath(__file__)), "compose.yaml")]
HUBS = ["hub1", "hub2"]
NATED = ["a1", "d1", "b1", "c1"]
NODES = HUBS + NATED
CLASSES = {"hub1": "public", "hub2": "public", "a1": "punchable", "d1": "punchable", "b1": "relay_only", "c1": "relay_only"}
PUNCHABLE = {"a1", "d1"}


def run(*args, check=True, capture=False):
    result = subprocess.run(COMPOSE + list(args), check=False, capture_output=capture, text=True)
    if check and result.returncode != 0:
        sys.exit("docker compose " + " ".join(args) + " failed:\n" + (result.stderr or ""))
    return result


def status(node):
    """The node's latest status line, or None."""
    out = run("logs", "--no-log-prefix", "--tail", "40", node, check=False, capture=True).stdout
    latest = None
    for line in out.splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            record = json.loads(line)
        except json.JSONDecodeError:
            continue
        if record.get("event") == "status":
            latest = record
    return latest


def snapshot(nodes):
    return {node: status(node) for node in nodes}


def names(statuses):
    """Node id to name, from each node's own record."""
    out = {}
    for node, record in statuses.items():
        if record:
            out[record["self"]["id"]] = node
    return out


def peers(record, ids):
    """A node's view of its peers, by name."""
    out = {}
    for peer in record.get("peers", []):
        name = ids.get(peer["record"]["id"])
        if name:
            out[name] = peer
    return out


def expected_path(a, b):
    if a in HUBS or b in HUBS:
        return "ordinary"
    if a in PUNCHABLE and b in PUNCHABLE:
        return "punched"
    return "relayed"


def problems(statuses, nodes, pairs=True):
    """What is not yet as the scenario expects, empty when all is."""
    out = []
    ids = names(statuses)
    for node in nodes:
        record = statuses.get(node)
        if not record:
            out.append(f"{node}: no status yet")
            continue
        if record["self"]["path_class"] != CLASSES[node]:
            out.append(f"{node}: path class {record['self']['path_class']}, expected {CLASSES[node]}")
        seen = peers(record, ids)
        for other in nodes:
            if other == node:
                continue
            if other not in seen:
                out.append(f"{node}: does not list {other}")
                continue
            if not pairs:
                continue
            peer = seen[other]
            want = expected_path(node, other)
            for lane in ("control", "data"):
                verdict = peer.get("echo", {}).get(lane)
                if verdict != "ok":
                    out.append(f"{node} -> {other}: echo on {lane}: {verdict}")
            # The NATed nodes open their own paths; a hub's routes to them
            # are theirs.
            if node in NATED:
                got = peer.get("path", {}).get("control")
                if got != want:
                    out.append(f"{node} -> {other}: path {got}, expected {want}")
    return out


def wait(what, check, deadline_s):
    print(f"-- {what}", flush=True)
    until = time.time() + deadline_s
    last = []
    while time.time() < until:
        last = check()
        if not last:
            print(f"   ok ({what})", flush=True)
            return
        time.sleep(2)
    print(f"   FAILED ({what}) after {deadline_s} s:", flush=True)
    for line in last[:40]:
        print("   " + line, flush=True)
    raise SystemExit(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--keep", action="store_true", help="leave the bed running")
    parser.add_argument("--no-build", action="store_true", help="use the image already built")
    args = parser.parse_args()

    if not args.no_build:
        print("-- building the image", flush=True)
        run("build", "hub1")
    run("down", "-v", "--remove-orphans", check=False, capture=True)
    run("up", "-d", "--pull", "never")
    try:
        wait("one cluster, path classes", lambda: problems(snapshot(NODES), NODES, pairs=False), 180)
        wait("every pair's path and echo on both lanes", lambda: problems(snapshot(NODES), NODES), 180)

        print("-- partition: router-d stops", flush=True)
        run("stop", "router-d")
        others = [n for n in NODES if n != "d1"]

        def d1_gone():
            statuses = snapshot(others)
            ids = names({**statuses, "d1": status("d1")})
            listed = [n for n in others if "d1" in peers(statuses[n] or {}, ids)]
            return [f"{n} still lists d1" for n in listed]

        wait("d1 drops out of the cluster", d1_gone, 120)
        print("-- partition heals: router-d starts", flush=True)
        run("start", "router-d")
        wait("d1 back, punched to a1, every pair answering", lambda: problems(snapshot(NODES), NODES), 180)

        print("-- relay failover: hub1 stops", flush=True)
        run("stop", "hub1")
        rest = [n for n in NODES if n != "hub1"]
        wait("the relayed pairs move to hub2", lambda: problems(snapshot(rest), rest), 180)
        print("-- hub1 starts", flush=True)
        run("start", "hub1")
        wait("all six as at the start", lambda: problems(snapshot(NODES), NODES), 180)

        relayed = sum((status(h) or {}).get("level0", {}).get("relayed_bytes", 0) for h in HUBS)
        brokered = sum((status(h) or {}).get("level0", {}).get("punches_brokered", 0) for h in HUBS)
        print(f"== passed: {brokered} punches brokered, {relayed} bytes relayed by the hubs now running", flush=True)
    finally:
        if not args.keep:
            run("down", "-v", "--remove-orphans", check=False, capture=True)


if __name__ == "__main__":
    main()
