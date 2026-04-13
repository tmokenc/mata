/**
 * @file subsumption.hh
 * @brief Private subsumption and antichain bucketing declarations for mata::nft::lazy::detail.
 */

#pragma once

#include "macrostate_store.hh"

#include <mata/simlib/explicit_lts.hh>

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mata::nft::lazy::detail {

/**
 * @brief External references needed by the lazy subsumption engine.
 */
struct SubsumptionContext {
    const std::vector<mata::nfa::Nfa>& nfas;
    const std::vector<mata::nft::Nft>& nfts;
    const std::vector<ExecNode>& nodes;
    const MacroStateStore& macro_store;
};

/**
 * @brief Subsumption and antichain-pruning engine for lazy emptiness.
 */
class SubsumptionEngine {
public:
    /// Construct the engine over one reconstructed exec DAG.
    explicit SubsumptionEngine(const SubsumptionContext& context);

    /// Precompute simulation relations for all reachable leaf automata below @p root_id.
    void initialize_leaf_simulations(NodeId root_id);
    /// Store the precomputed simulation relation of one NFA leaf.
    void set_nfa_simulation(size_t nfa_index, Simlib::Util::BinaryRelation relation);
    /// Store the precomputed simulation relation of one NFT leaf.
    void set_nft_simulation(size_t nft_index, Simlib::Util::BinaryRelation relation);

    /// Check whether @p state is subsumed, updating antichains by removing weaker states.
    bool is_subsumed(
            NodeId root_id, MacroStateId state, std::unordered_set<MacroStateId>& visited,
            std::unordered_set<MacroStateId>& queued) const;

private:
    using State = mata::nfa::State;

    const std::vector<mata::nfa::Nfa>& nfas;
    const std::vector<mata::nft::Nft>& nfts;
    const std::vector<ExecNode>& nodes;
    const MacroStateStore& macro_store;
    std::vector<Simlib::Util::BinaryRelation> precomputed_simulations;

    /// Recursive worker for reachable-leaf simulation initialization.
    void initialize_leaf_simulations_impl(NodeId node_id, std::vector<bool>& visited);
    /// Semantic subsumption test for two states at one exec node.
    bool subsumed_state(NodeId node_id, MacroStateId state1, MacroStateId state2) const;
};

} // namespace mata::nft::lazy::detail
