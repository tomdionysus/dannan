//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "dannan/net/routes.h"

// Macha's message types as protocol 23 states them (Macha's `class_allowed`
// and `allowed_on_lane` at 0.91.2), for the tools and tests that put a
// Dannan node in a Macha cluster: Macha sends its metadata and storage
// traffic to every peer, in any frame class, and a node that did not know
// those types would refuse the frames and lose the session. Macha registers
// these itself once it runs on Dannan.
namespace dannan::tools {

template <class Add>
void macha_protocol23_types(Add&& add_type) {
  using net::Frames;
  using net::Lanes;
  auto add = [&](uint16_t number, const char* name, Lanes lanes, Frames frames, bool notification = false) {
    add_type(net::MessageType{number}, net::Registration{name, lanes, frames, notification});
  };
  // Objects: the DATA lane, never the control frame.
  for (const auto& [number, name] : std::vector<std::pair<uint16_t, const char*>>{
           {4, "get_object"}, {5, "put_object"}, {22, "put_object_deferred"}, {23, "object_durability_barrier"}, {104, "object_reply"}})
    add(number, name, Lanes::data, Frames::not_control);
  // Metadata, presence and node records: the CONTROL lane, any frame class.
  for (const auto& [number, name] : std::vector<std::pair<uint16_t, const char*>>{{3, "have_object"},
                                                                                  {6, "get_metadata"},
                                                                                  {7, "cas_metadata"},
                                                                                  {8, "seed_metadata"},
                                                                                  {11, "checkpoint_metadata"},
                                                                                  {13, "delete_object"},
                                                                                  {17, "commit_metadata"},
                                                                                  {18, "cas_metadata_delta"},
                                                                                  {19, "get_control_object"},
                                                                                  {20, "put_control_object"},
                                                                                  {21, "get_metadata_identity"},
                                                                                  {26, "get_metadata_history_entry"},
                                                                                  {27, "has_metadata_history_entry"},
                                                                                  {28, "put_metadata_history_entry"},
                                                                                  {29, "get_metadata_heads"},
                                                                                  {30, "put_metadata_commit"},
                                                                                  {31, "accept_metadata_commit"},
                                                                                  {32, "retain_objects"},
                                                                                  {33, "get_ingest_jobs"},
                                                                                  {34, "ingest_job_action"},
                                                                                  {35, "get_torrent_jobs"},
                                                                                  {36, "torrent_job_action"},
                                                                                  {40, "have_objects"},
                                                                                  {41, "get_metadata_history_record"},
                                                                                  {46, "torrent_intent"},
                                                                                  {47, "have_valid_objects"},
                                                                                  {49, "trie_diff"},
                                                                                  {50, "get_unmatched_hints"},
                                                                                  {51, "submit_ingest_job"},
                                                                                  {105, "metadata_reply"},
                                                                                  {106, "cas_reply"},
                                                                                  {107, "control_object_reply"},
                                                                                  {108, "metadata_identity_reply"},
                                                                                  {111, "metadata_history_entry_reply"},
                                                                                  {112, "metadata_heads_reply"},
                                                                                  {113, "ingest_jobs_reply"},
                                                                                  {114, "ingest_job_action_reply"},
                                                                                  {115, "torrent_jobs_reply"},
                                                                                  {116, "torrent_job_action_reply"},
                                                                                  {118, "have_objects_reply"},
                                                                                  {122, "torrent_intent_reply"},
                                                                                  {123, "have_valid_objects_reply"},
                                                                                  {125, "trie_diff_reply"},
                                                                                  {126, "unmatched_hints_reply"},
                                                                                  {127, "ingest_submit_reply"}})
    add(number, name, Lanes::control, Frames::any);
  // Gossip sent without a reply.
  add(39, "session_sync", Lanes::control, Frames::control_only, true);
  add(42, "user_sync", Lanes::control, Frames::control_only, true);
}

}  // namespace dannan::tools
