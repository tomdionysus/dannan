//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/net/message.h"

namespace dannan::net {

Message error_reply(uint16_t status, std::string_view code, std::string_view text) {
  Writer writer;
  writer.string(std::string(text));
  writer.u16(status);
  writer.string(std::string(code));
  return {message::error, writer.take(), {}};
}

std::optional<Refusal> refusal_of(const Message& reply) {
  if (reply.type != message::error) return std::nullopt;
  Refusal out;
  try {
    Reader reader(reply.payload);
    if (!reader.remaining()) return out;
    out.message = reader.string();
    if (!reader.remaining()) return out;
    out.status = reader.u16();
    out.code = reader.string();
  } catch (const DecodeError&) {
    // An older or malformed error keeps what was read.
  }
  return out;
}

bool try_later(const Refusal& refusal) noexcept {
  switch (refusal.status) {
    case 0:
    case 429:
    case 500:
    case 502:
    case 503:
    case 504:
      return true;
    default:
      return false;
  }
}

}  // namespace dannan::net
