/**
 * @file subsumption.hh
 * @brief Private subsumption and antichain-pruning declarations for mata::nft::lazy::detail.
 */

#pragma once

#include "macrostate_store.hh"
#include "state_types.hh"

#include <mata/simlib/explicit_lts.hh>

#include <cstdint>
#include <map>
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

    /**
     * @brief Print statistics about the current subsumption engine state, size of the antichain, ratio between true and
     * false subsumption results. Result for local caches and global
     *
     **/
    void print_statistics() const;

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
    /// Current antichain of non-subsumed states (flat view used by is_pruned).
    std::unordered_set<MacroStateId> antichain;
    /// Antichain bucketed by structural fingerprint to prune incompatible candidates early.
    std::map<uint32_t, std::vector<MacroStateId>> antichain_buckets;
    /// Per-node subsumption caches.
    std::vector<SubsumptionCache> caches;

    /**
     * @brief Kind of structural fingerprint used to bucket the antichain.
     *
     * Chosen from the root node kind when @c initialize_leaf_simulations is called.
     * The fingerprint is a cheap necessary condition on subsumption at the root,
     * letting the antichain scan skip entries that cannot possibly subsume a query.
     */
    enum class AntichainFilterKind : uint8_t {
        /// No structural filter — fall back to scanning every entry.
        None = 0,
        /// Root is Union*: fingerprint = TaggedState::Tag (subsumption requires equal tag).
        Tag,
        /// Root is Complement*: fingerprint = |SetState| (subsumption reverses subset order).
        RootSetSize,
        /// Root is Intersect*/SyncProduct* with a Complement rhs:
        /// fingerprint = |SetState of pair.rhs|.
        IntersectRhsSetSize,
    };

    /// Chosen filter kind for the current root.
    AntichainFilterKind filter_kind = AntichainFilterKind::None;
    /// Execution node whose macrostate view is used to compute the fingerprint.
    NodeId filter_fingerprint_source_node = 0;
    /// Root exec node used when computing fingerprints.
    NodeId filter_root_node = 0;

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

    /// Choose an antichain filter strategy for the current root.
    void configure_antichain_filter(NodeId root_id);

    /// Compute the structural fingerprint of @p state under the current filter.
    uint32_t compute_fingerprint(MacroStateId state) const;

    /// Return true if @p entry_fp could belong to an antichain entry that subsumes a query of fingerprint @p state_fp.
    bool fingerprint_can_subsume(uint32_t entry_fp, uint32_t state_fp) const noexcept;

    /// Return true if @p entry_fp could belong to an antichain entry that is subsumed by a query of fingerprint @p state_fp.
    bool fingerprint_can_be_subsumed(uint32_t entry_fp, uint32_t state_fp) const noexcept;
};

} // namespace mata::nft::lazy::detail
