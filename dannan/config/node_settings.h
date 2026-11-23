//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

// The node settings every node binary in the stack takes (0.11.0; asked by Lir, Kvasir, Louhi and
// Mimir, so none keeps a copy), all under cluster.*, a section no hosted system uses for its own
// listener (the maintainer, 2026-11-22):
//
//   cluster.key, cluster.bootstrap, cluster.state, cluster.name, cluster.domain, cluster.relay,
//   cluster.listen.{address,port,advertise,inbound}, cluster.heartbeat_ms, cluster.path_class,
//   cluster.punch_ms, cluster.reprobe_ms, cluster.custody.{passphrase_env,credential,
//   credentials_dir}, cluster.identity.{ca,certificate,revocations,no_cluster_key}
//
// Each has its flag and its variable by docs/config.md's rule under the project's prefix:
// cluster.listen.port is --cluster-listen-port and LIR_CLUSTER_LISTEN_PORT.

#include <string_view>
#include <vector>

#include "dannan/config/config.h"
#include "dannan/node.h"

namespace dannan::config {

// Adds every node setting to `reader`, beside the binary's own. `prefix` is the project's, in
// capitals ("LIR"). Waits on nothing.
void add_node_settings(Reader& reader, std::string_view prefix);

// A NodeConfig from what the layers gave; what none gave keeps NodeConfig's default. Requires
// nothing to be present: whether a state or a key must be given is the binary's own rule.
// Reads the files named (the cluster key, the certificates) and, for a custody passphrase, the
// variable named when the custody is opened. Throws ConfigError, in the reader's words, for a
// value that cannot be used, a file that cannot be read, or a pair given half: the CA and the
// certificate go together, and no_cluster_key needs them.
NodeConfig node_config(const Settings& settings);

// The revocations in the file cluster.identity.revocations names, to give the started node
// (Context::identity().revoke); none when it names none. Throws ConfigError as above.
std::vector<identity::Revocation> node_revocations(const Settings& settings);

}  // namespace dannan::config
