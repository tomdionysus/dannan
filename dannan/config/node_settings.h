//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

/// @file
/// The node settings every node binary takes, under cluster.*.

#include <string_view>
#include <vector>

#include "dannan/config/config.h"
#include "dannan/node.h"

namespace dannan::config {

/// Adds every node setting to `reader`, beside the binary's own (0.11.0; asked by Lir, Kvasir, Louhi
/// and Mimir, so none keeps a copy). All live under cluster.*, a section no hosted system uses for
/// its own listener:
///
/// | Key | Values |
/// |---|---|
/// | cluster.key | the cluster key file |
/// | cluster.bootstrap | a list of HOST:PORT seeds |
/// | cluster.state | the state directory |
/// | cluster.name, cluster.domain | the node's name and failure domain |
/// | cluster.relay | toggle: forward circuits for others |
/// | cluster.listen.address, .port, .advertise | where it listens, and the host peers dial |
/// | cluster.listen.inbound | yes, no or auto |
/// | cluster.heartbeat_ms | the heartbeat; liveness is six of them |
/// | cluster.path_class | public, punchable or relay_only; none: classified |
/// | cluster.punch_ms, cluster.reprobe_ms | a punch's window; the reprobe while incapable |
/// | cluster.custody.passphrase_env, .credential, .credentials_dir | the key custody's source |
/// | cluster.identity.ca, .certificate, .revocations, .no_cluster_key | certificates |
///
/// Each has its flag and its variable by docs/config.md's rule under the project's prefix:
/// cluster.listen.port is --cluster-listen-port and LIR_CLUSTER_LISTEN_PORT. The node-program helper
/// (supervise::run_node_program()) adds them itself. Waits on nothing.
/// @param prefix the project's, in capitals ("LIR").
void add_node_settings(Reader& reader, std::string_view prefix);

/// A NodeConfig from what the layers gave; what none gave keeps NodeConfig's default.
///
/// Requires nothing to be present: whether a state or a key must be given is the binary's own rule.
/// Reads the files named (the cluster key, the certificates) and, for a custody passphrase, the
/// variable named when the custody is opened.
/// @throws ConfigError, in the reader's words, for a value that cannot be used, a file that cannot
///   be read, or a pair given half: the CA and the certificate go together, and no_cluster_key
///   needs them.
NodeConfig node_config(const Settings& settings);

/// The revocations in the file cluster.identity.revocations names, to give the started node
/// (Context::identity().revoke()); none when it names none.
/// @throws ConfigError as node_config() does.
std::vector<identity::Revocation> node_revocations(const Settings& settings);

}  // namespace dannan::config
