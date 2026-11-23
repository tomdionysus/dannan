//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/config/node_settings.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

#include "dannan/identity/certificate.h"
#include "dannan/identity/cluster_key.h"

namespace dannan::config {
namespace {

using Kind = Setting::Kind;

// docs/config.md: the flag is "--" and the key with '.' and '_' as '-'; the variable is the
// prefix, '_', and the key in capitals with '.' and '-' as '_'.
std::string flag_of(const std::string& key) {
  std::string out = "--";
  for (const char c : key) out += c == '.' || c == '_' ? '-' : c;
  return out;
}

std::string variable_of(std::string_view prefix, const std::string& key) {
  std::string out(prefix);
  out += '_';
  for (const char c : key) out += c == '.' || c == '-' ? '_' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

std::string read_file(const Settings& settings, const std::string& key, const std::string& path, bool binary) {
  std::ifstream input(path, binary ? std::ios::binary : std::ios::in);
  if (!input) throw settings.refuse(key, "a file that can be read");
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

net::Endpoint endpoint_of(const Settings& settings, const std::string& key, const std::string& text) {
  const auto colon = text.rfind(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 == text.size()) throw settings.refuse(key, "HOST:PORT");
  try {
    const auto port = std::stoul(text.substr(colon + 1));
    if (port > 65535) throw settings.refuse(key, "HOST:PORT");
    return {text.substr(0, colon), static_cast<uint16_t>(port)};
  } catch (const std::logic_error&) {
    throw settings.refuse(key, "HOST:PORT");
  }
}

}  // namespace

void add_node_settings(Reader& reader, std::string_view prefix) {
  const auto add = [&](std::string key, Kind kind, std::string what) {
    auto flag = flag_of(key);
    auto variable = variable_of(prefix, key);
    reader.settings.push_back({std::move(key), kind, std::move(flag), std::move(variable), std::move(what)});
  };
  add("cluster.key", Kind::value, "the cluster key file");
  add("cluster.bootstrap", Kind::list, "HOST:PORT");
  add("cluster.state", Kind::value, "the state directory");
  add("cluster.name", Kind::value, "a name");
  add("cluster.domain", Kind::value, "a failure domain");
  add("cluster.relay", Kind::toggle, "whether it relays for others");
  add("cluster.listen.address", Kind::value, "an address");
  add("cluster.listen.port", Kind::value, "a port, 0 to 65535");
  add("cluster.listen.advertise", Kind::value, "the host peers dial");
  add("cluster.listen.inbound", Kind::value, "yes, no or auto");
  add("cluster.heartbeat_ms", Kind::value, "milliseconds");
  add("cluster.path_class", Kind::value, "public, punchable or relay_only");
  add("cluster.punch_ms", Kind::value, "milliseconds");
  add("cluster.reprobe_ms", Kind::value, "milliseconds");
  add("cluster.custody.passphrase_env", Kind::value, "a variable holding the custody passphrase");
  add("cluster.custody.credential", Kind::value, "a systemd credential name");
  add("cluster.custody.credentials_dir", Kind::value, "a directory");
  add("cluster.identity.ca", Kind::value, "the CA certificate, PEM");
  add("cluster.identity.certificate", Kind::value, "the node certificate, PEM");
  add("cluster.identity.revocations", Kind::value, "a revocations file");
  add("cluster.identity.no_cluster_key", Kind::toggle, "certificate alone");
}

NodeConfig node_config(const Settings& settings) {
  NodeConfig out;
  const auto ms = [&](const std::string& key) -> std::optional<std::chrono::milliseconds> {
    if (const auto value = settings.number(key, 1, 86400000)) return std::chrono::milliseconds(*value);
    return std::nullopt;
  };
  if (const auto state = settings.path_of("cluster.state")) out.state_path = *state;
  if (const auto key = settings.path_of("cluster.key")) {
    try {
      out.key = identity::load_cluster_key(*key);
    } catch (const std::exception&) {
      throw settings.refuse("cluster.key", "a cluster key file that can be read");
    }
  }
  for (const auto& seed : settings.list("cluster.bootstrap")) out.bootstrap.push_back(endpoint_of(settings, "cluster.bootstrap", seed));
  if (const auto name = settings.text("cluster.name")) out.node_name = *name;
  if (const auto domain = settings.text("cluster.domain")) out.failure_domain = *domain;
  if (const auto relay = settings.toggle("cluster.relay")) out.relay = *relay;
  if (const auto address = settings.text("cluster.listen.address")) out.listen_host = *address;
  if (const auto port = settings.number("cluster.listen.port", 0, 65535)) out.port = static_cast<uint16_t>(*port);
  if (const auto advertise = settings.text("cluster.listen.advertise")) out.advertise_host = *advertise;
  if (const auto mode = settings.choice("cluster.listen.inbound", {"yes", "no", "auto"}))
    out.inbound = *mode == "yes" ? reach::Inbound::yes : *mode == "no" ? reach::Inbound::no : reach::Inbound::automatic;
  if (const auto heartbeat = ms("cluster.heartbeat_ms")) {
    // Liveness keeps NodeConfig's own ratio to the heartbeat: six beats.
    out.heartbeat = *heartbeat;
    out.dead_after = *heartbeat * 6;
  }
  if (const auto name = settings.choice("cluster.path_class", {"public", "punchable", "relay_only"}))
    out.path_class = *name == "public"      ? membership::PathClass::public_path
                     : *name == "punchable" ? membership::PathClass::punchable
                                            : membership::PathClass::relay_only;
  if (const auto punch = ms("cluster.punch_ms")) out.punch_window = *punch;
  if (const auto reprobe = ms("cluster.reprobe_ms")) out.reprobe_while_incapable = *reprobe;
  if (const auto variable = settings.text("cluster.custody.passphrase_env")) {
    // Read when the custody is opened, not now: the passphrase is never held longer than that.
    const auto name = *variable;
    out.custody = identity::KeySource::typed_passphrase([name] {
      const char* value = std::getenv(name.c_str());
      if (!value || !*value) throw ConfigError("the custody passphrase variable " + name + " is empty");
      return identity::Secret(std::span(reinterpret_cast<const uint8_t*>(value), std::char_traits<char>::length(value)));
    });
  } else if (const auto credential = settings.text("cluster.custody.credential")) {
    out.custody = identity::KeySource::systemd_credential(*credential, settings.text("cluster.custody.credentials_dir").value_or(""));
  }
  const auto ca = settings.path_of("cluster.identity.ca");
  const auto certificate = settings.path_of("cluster.identity.certificate");
  const auto alone = settings.toggle("cluster.identity.no_cluster_key").value_or(false);
  if (ca.has_value() != certificate.has_value())
    throw settings.refuse(ca ? "cluster.identity.ca" : "cluster.identity.certificate", "given with the other: the CA and the certificate go together");
  if (alone && !certificate) throw settings.refuse("cluster.identity.no_cluster_key", "set only with a CA and a certificate");
  if (certificate) {
    try {
      out.ca_certificate = identity::Certificate::from_pem(read_file(settings, "cluster.identity.ca", *ca, false));
    } catch (const ConfigError&) {
      throw;
    } catch (const std::exception&) {
      throw settings.refuse("cluster.identity.ca", "a PEM certificate");
    }
    try {
      out.certificate = identity::Certificate::from_pem(read_file(settings, "cluster.identity.certificate", *certificate, false));
    } catch (const ConfigError&) {
      throw;
    } catch (const std::exception&) {
      throw settings.refuse("cluster.identity.certificate", "a PEM certificate");
    }
    out.use_cluster_key = !alone;
  }
  return out;
}

std::vector<identity::Revocation> node_revocations(const Settings& settings) {
  const auto path = settings.path_of("cluster.identity.revocations");
  if (!path) return {};
  const auto bytes = read_file(settings, "cluster.identity.revocations", *path, true);
  try {
    return identity::decode_revocations(std::span(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
  } catch (const std::exception&) {
    throw settings.refuse("cluster.identity.revocations", "a revocations file");
  }
}

}  // namespace dannan::config
