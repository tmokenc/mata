/**
 * @file reconstruction.hh
 * @brief Private symbolic-formula DAG reconstruction declarations for mata::nft::lazy::detail.
 */

#pragma once

#include "state_types.hh"

#include <vector>

namespace mata::nft::lazy::detail {

/**
 * @brief Reconstruct and normalize the reachable exec DAG below one symbolic term.
 * @param formula Source symbolic formula DAG.
 * @param id Root symbolic node to reconstruct.
 * @param output Destination exec-node array appended in child-before-parent order.
 * @return Exec-node id of the reconstructed root inside @p output.
 */
NodeId reconstruct_nodes(const SymbolicFormula& formula, NodeId id, std::vector<ExecNode>& output);

} // namespace mata::nft::lazy::detail
