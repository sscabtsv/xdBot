#pragma once

#include "gdr.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace xb_format {

// Cheap magic-byte sniff, used to decide whether a file should be routed to
// importXB at all.
bool isXBData(std::span<uint8_t const> data);

std::vector<uint8_t> exportXB(BotReplay const& replay);

gdr::Result<BotReplay> importXB(std::span<uint8_t const> data);

} // namespace xb_format
