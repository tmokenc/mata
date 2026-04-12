/**
 * @file reconstruction.hh
 * @brief Private symbolic-tree reconstruction declarations for mata::nft::lazy::detail.
 */

#pragma once

#include "state_types.hh"

#include <vector>

namespace mata::nft::lazy::detail {

NodeId reconstruct_nodes(const SymbolicAutomataTree& tree, NodeId id, std::vector<ExecNode>& output);

} // namespace mata::nft::lazy::detail
