/**
 * @file emptiness.hh
 * @brief Private emptiness-check entry points for mata::nft::lazy::detail.
 */

#pragma once

#include "mata/nft/lazy.hh"

#include <vector>

namespace mata::nft::lazy::detail {

/**
 * @brief Check emptiness of a reconstructed lazy symbolic relation tree.
 * @param tree Source symbolic tree storage.
 * @param root_node Root symbolic term to evaluate.
 * @param level_alphabets Optional explicit root-level visible alphabets.
 * @return `true` when the relation is empty, `false` otherwise.
 */
bool is_empty(
        const SymbolicAutomataTree& tree, const Term& root_node,
        const std::vector<mata::OnTheFlyAlphabet>* level_alphabets = nullptr);

} // namespace mata::nft::lazy::detail
