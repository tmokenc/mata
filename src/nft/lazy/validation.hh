/**
 * @file validation.hh
 * @brief Private structural validation entry points for mata::nft::lazy::detail.
 */

#pragma once

#include "mata/nft/lazy.hh"

#include <cstdint>
#include <vector>

namespace mata::nft::lazy::detail {

bool levels_unique(const std::vector<uint8_t>& levels);
bool level_refs_unique(const std::vector<LevelRef>& refs);
bool is_valid(const SymbolicAutomataTree& tree, const Term& root_node);

} // namespace mata::nft::lazy::detail
