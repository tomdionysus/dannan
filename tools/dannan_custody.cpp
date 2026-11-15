//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// dannan-custody: makes and checks a node's key custody, on the node, with the
// node stopped (Macha's deploy runs it between stop and start). Custody is
// sealed under a systemd credential's root, never on disk in clear.
//
//   dannan-custody init --state DIR --credential NAME
//       makes custody; prints the new root (hex) once, for `systemd-creds
//       encrypt`, and nothing else on stdout
//   dannan-custody show --state DIR --credential NAME [--credentials-dir DIR]
//       opens custody with the sealed credential as the unit will
//       ($CREDENTIALS_DIRECTORY unless a directory is given) and prints
//       {"id":...,"public_key":...}
//
// Both refuse while a node holds the state directory's lock.

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>

#include "dannan/identity/crypto.h"
#include "dannan/identity/key_custody.h"
#include "dannan/identity/node_identity.h"

namespace {

using namespace dannan;

std::map<std::string, std::string> parse(int argc, char** argv, int from) {
  std::map<std::string, std::string> out;
  for (int i = from; i < argc; ++i) {
    const std::string option = argv[i];
    if (option.rfind("--", 0) != 0 || i + 1 >= argc) throw std::invalid_argument("expected --option value, at " + option);
    const auto name = option.substr(2);
    if (name != "state" && name != "credential" && name != "credentials-dir") throw std::invalid_argument("unknown option " + option);
    out[name] = argv[++i];
  }
  return out;
}

std::string need(const std::map<std::string, std::string>& options, const std::string& name) {
  const auto found = options.find(name);
  if (found == options.end() || found->second.empty()) throw std::invalid_argument("--" + name + " is required");
  return found->second;
}

}  // namespace

int main(int argc, char** argv) {
  const char* usage =
      "usage: dannan-custody init --state DIR --credential NAME\n"
      "       dannan-custody show --state DIR --credential NAME [--credentials-dir DIR]\n";
  if (argc < 2) {
    std::cerr << usage;
    return 2;
  }
  const std::string command = argv[1];
  std::map<std::string, std::string> options;
  std::filesystem::path state;
  identity::KeySource source;
  try {
    if (command != "init" && command != "show") throw std::invalid_argument("unknown command " + command);
    options = parse(argc, argv, 2);
    if (command == "init" && options.contains("credentials-dir")) throw std::invalid_argument("init reads no credential: --credentials-dir is for show");
    state = need(options, "state");
    const auto directory = options.contains("credentials-dir") ? std::filesystem::path(options.at("credentials-dir")) : std::filesystem::path();
    source = identity::KeySource::systemd_credential(need(options, "credential"), directory);
  } catch (const std::exception& error) {
    std::cerr << "dannan-custody: " << error.what() << "\n" << usage;
    return 2;
  }
  try {
    // show reads and never writes (Macha's review): a mistyped or fresh
    // directory must not gain an id it would print as the node's, nor a lock
    // file, so this is asked before the lock is taken.
    if (command == "show" && !std::filesystem::exists(state / "node-id")) throw std::runtime_error("no node identity in " + state.string());
    // A running node holds this lock for its whole life: custody is never made
    // or read beside one.
    const identity::StateLock lock(state);
    if (command == "init") {
      // The new root goes to stdout and nowhere else. The node's id is made
      // with its custody, as a node makes it at its first start; one there
      // already (Macha's) is kept.
      const auto root = identity::KeyCustody::create(state, source);
      if (!root) throw std::runtime_error("no root was made");
      (void)identity::load_or_create_node_id(state);
      std::printf("%s\n", identity::hex(root->reveal()).c_str());
      return 0;
    }
    const auto custody = identity::KeyCustody::open(state, source);
    std::printf("{\"id\":\"%s\",\"public_key\":\"%s\"}\n", identity::to_string(identity::load_or_create_node_id(state)).c_str(),
                identity::hex(custody.node_key().public_key()).c_str());
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "dannan-custody: " << error.what() << "\n";
    return 1;
  }
}
