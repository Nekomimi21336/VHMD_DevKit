// JSON protocol parser. See docs/protocol.md for the wire format.
#pragma once

#include <string>
#include <string_view>

#include "protocol/messages.h"

namespace vhmd {

// Parses one UDP datagram containing either a single JSON object or a JSON
// array of objects (batch). Invalid entries are dropped and reported in
// ParseResult::errors; valid entries in the same batch are still returned.
// Never throws.
ParseResult ParseJsonPacket(std::string_view data, const ParseLimits& limits = {});

// True if the datagram looks like JSON (first non-space byte is '{' or '[').
bool LooksLikeJson(std::string_view data);

} // namespace vhmd
