/**
 * @file subsumption.hh
 * @brief Private subsumption and antichain bucketing declarations for mata::nft::lazy::detail.
 */

#pragma once

#include "macrostate_store.hh"

#include <mata/simlib/explicit_lts.hh>

#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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

/// Cache for previously computed subsumption results between pairs of macro states at one exec node.
struct SubsumptionCache {
    struct KeyHash {
        size_t operator()(const std::pair<MacroStateId, MacroStateId>& key) const noexcept {
            return static_cast<size_t>(
                    mix_hash64((static_cast<uint64_t>(key.first) << 32) | static_cast<uint64_t>(key.second)));
        }
    };

    std::unordered_map<std::pair<MacroStateId, MacroStateId>, bool, KeyHash> cache;

    SubsumptionCache() : cache{} {}

    std::optional<bool> get(MacroStateId state1, MacroStateId state2) const;
    void set(MacroStateId state1, MacroStateId state2, bool result);
};

/**
 * @brief Subsumption and antichain-pruning engine for lazy emptiness.
 */
class SubsumptionEngine {
public:
    /// Construct the engine over one reconstructed exec DAG.
    explicit SubsumptionEngine(const SubsumptionContext& context);

    void initialize_leaf_simulations(NodeId root_id);
    /// Store the precomputed simulation relation of one NFA leaf.
    void set_nfa_simulation(size_t nfa_index, Simlib::Util::BinaryRelation relation);
    /// Store the precomputed simulation relation of one NFT leaf.
    void set_nft_simulation(size_t nft_index, Simlib::Util::BinaryRelation relation);

    /// Check whether @p state is subsumed, updating antichains by removing weaker states.
    bool is_subsumed(NodeId root_id, MacroStateId state);

    /// Check whether @p state is pruned by the current antichain.
    /// Note that the state **MUST** be inserted by a previous call to `is_subsumed` before this check is made,
    /// otherwise it will be considered pruned by default.
    bool is_pruned(MacroStateId state) const;

private:
    using State = mata::nfa::State;

    // External references.
    const std::vector<mata::nfa::Nfa>& nfas;
    const std::vector<mata::nft::Nft>& nfts;
    const std::vector<ExecNode>& nodes;
    const MacroStateStore& macro_store;

    std::vector<Simlib::Util::BinaryRelation> precomputed_simulation_nfas;
    std::vector<Simlib::Util::BinaryRelation> precomputed_simulation_nfts;
    std::unordered_set<MacroStateId> antichain;
    std::vector<SubsumptionCache> caches;

    /// Recursive worker for reachable-leaf simulation initialization.
    void initialize_leaf_simulations_impl(NodeId node_id, std::vector<bool>& visited);
    /// Semantic subsumption test for two states at one exec node.
    bool subsumed_state(NodeId node_id, MacroStateId state1, MacroStateId state2);
};

} // namespace mata::nft::lazy::detail
