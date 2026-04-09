/**
 * @file emptiness.hh
 * @brief Private emptiness-check entry points for mata::nft::lazy::detail.
 */

#pragma once

#include "mata/nft/lazy.hh"

#include <vector>

namespace mata::nft::lazy::detail {

bool is_empty(
        const SymbolicAutomataTree& tree, const Term& root_node,
        const std::vector<mata::OnTheFlyAlphabet>* level_alphabets = nullptr);

} // namespace mata::nft::lazy::detail
