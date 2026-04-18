/**
 * @file subsumption.hh
 * @brief Private subsumption and antichain-pruning declarations for mata::nft::lazy::detail.
 */

#pragma once

#include "macrostate_store.hh"
#include "state_types.hh"

#include <mata/simlib/explicit_lts.hh>

#include <cstdint>
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
    /// NFAs referenced by the execution DAG.
    const std::vector<mata::nfa::Nfa>& nfas;
    /// NFTs referenced by the execution DAG.
    const std::vector<mata::nft::Nft>& nfts;
    /// Execution DAG nodes used during lazy evaluation.
    const std::vector<ExecNode>& nodes;
    /// Shared store of macro states.
    const MacroStateStore& macro_store;
};

/**
 * @brief Cache for previously computed subsumption results between pairs of macro states.
 */
struct SubsumptionCache {
    /**
     * @brief Hash for pairs of macro-state identifiers.
     */
    struct KeyHash {
        /**
         * @brief Hash one pair of macro-state identifiers.
         * @param key Pair of macro-state identifiers.
         * @return Hash value for @p key.
         */
        size_t operator()(const std::pair<MacroStateId, MacroStateId>& key) const noexcept {
            return static_cast<size_t>(
                    mix_hash64((static_cast<uint64_t>(key.first) << 32) | static_cast<uint64_t>(key.second)));
        }
    };

    /// Stored cache entries.
    std::unordered_map<std::pair<MacroStateId, MacroStateId>, bool, KeyHash> cache;

    /**
     * @brief Construct an empty subsumption cache.
     */
    SubsumptionCache() : cache{} {}

    /**
     * @brief Get a cached subsumption result if present.
     *
     * @param state1 First macro state.
     * @param state2 Second macro state.
     * @return Cached result, or @c std::nullopt if missing.
     */
    std::optional<bool> get(MacroStateId state1, MacroStateId state2) const;

    /**
     * @brief Store a subsumption result in the cache.
     *
     * @param state1 First macro state.
     * @param state2 Second macro state.
     * @param result Cached subsumption result.
     */
    void set(MacroStateId state1, MacroStateId state2, bool result);
};

/**
 * @brief Subsumption and antichain-pruning engine for lazy emptiness.
 */
class SubsumptionEngine {
public:
    /**
     * @brief Construct the engine over one reconstructed execution DAG.
     *
     * @param context External references used by the engine.
     */
    explicit SubsumptionEngine(const SubsumptionContext& context);

    /**
     * @brief Initialize simulation relations for leaves reachable from @p root_id.
     *
     * @param root_id Root node of the execution DAG.
     */
    void initialize_leaf_simulations(NodeId root_id);

    /**
     * @brief Store the precomputed simulation relation of one NFA leaf.
     *
     * @param nfa_index Index of the NFA leaf.
     * @param relation Precomputed simulation relation.
     */
    void set_nfa_simulation(size_t nfa_index, Simlib::Util::BinaryRelation relation);

    /**
     * @brief Store the precomputed simulation relation of one NFT leaf.
     *
     * @param nft_index Index of the NFT leaf.
     * @param relation Precomputed simulation relation.
     */
    void set_nft_simulation(size_t nft_index, Simlib::Util::BinaryRelation relation);

    /**
     * @brief Check whether @p state is subsumed.
     *
     * Updates antichains by removing weaker states.
     *
     * @param root_id Root node of the macro state.
     * @param state Macro state to test.
     * @return True if the state is subsumed, false otherwise.
     */
    bool is_subsumed(NodeId root_id, MacroStateId state);

    /**
     * @brief Minimize the @p state by removing all subsumed states from it, acocording to
     * the Optimization 2 described in the paper Simulation meets antichains.
     *
     * This function do a fixed-point iteration to remove all subsumed states from @p state
     *
     * @param root_id The root node of the macro state.
     * @param state The state to minimize. It will be modified in-place.
     */
    void minimize(NodeId root_id, SetState& state);

    /**
     * @brief Check whether @p state is pruned by the current antichain.
     *
     * The state must be inserted by a previous call to @c is_subsumed before this check is made,
     * otherwise it will be considered pruned by default.
     *
     * @param state Macro state to check.
     * @return True if the state is pruned, false otherwise.
     */
    bool is_pruned(MacroStateId state) const;

private:
    using State = mata::nfa::State;

    /// External NFA references.
    const std::vector<mata::nfa::Nfa>& nfas;
    /// External NFT references.
    const std::vector<mata::nft::Nft>& nfts;
    /// Execution DAG nodes.
    const std::vector<ExecNode>& nodes;
    /// Shared macro-state storage.
    const MacroStateStore& macro_store;

    /// Precomputed simulation relations for NFA leaves.
    std::vector<Simlib::Util::BinaryRelation> precomputed_simulation_nfas;
    /// Precomputed simulation relations for NFT leaves.
    std::vector<Simlib::Util::BinaryRelation> precomputed_simulation_nfts;
    /// Current antichain of non-subsumed states.
    std::unordered_set<MacroStateId> antichain;
    /// Per-node subsumption caches.
    std::vector<SubsumptionCache> caches;

    /**
     * @brief Recursive worker for reachable-leaf simulation initialization.
     *
     * @param node_id Current node.
     * @param visited Marks already processed nodes.
     */
    void initialize_leaf_simulations_impl(NodeId node_id, std::vector<bool>& visited);

    /**
     * @brief Semantic subsumption test for two states at one execution node.
     *
     * @param node_id Execution node where subsumption is checked.
     * @param state1 First macro state.
     * @param state2 Second macro state.
     * @return True if @p state1 is subsumed by @p state2, false otherwise.
     */
    bool subsumed_state(NodeId node_id, MacroStateId state1, MacroStateId state2);
};

} // namespace mata::nft::lazy::detail
