/**
 * @file macrostate_store.hh
 * @brief Private macrostate storage declarations for mata::nft::lazy::detail.
 */

#pragma once

#include "state_types.hh"

#include "mata/utils/two-dimensional-map.hh"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace mata::nft::lazy::detail {

/**
 * @brief Interning storage for reconstructed lazy macrostates.
 */
struct MacroStateStore {
    /// Sparse storage used for complement subset states.
    using SetStore = std::unordered_map<MacroStateId, SetState>;
    /// Sparse storage used for union tag states.
    using TaggedStore = std::unordered_map<MacroStateId, TaggedState>;
    /// Upper bound for enabling the dense pair-store matrix optimization.
    static constexpr size_t DENSE_PAIR_MAX_MATRIX_SIZE = 10'000'000;

    /// Return whether a dense matrix pair store is feasible for the given bounds.
    static bool can_use_dense_pair_store(std::optional<size_t> lhs_bound, std::optional<size_t> rhs_bound);

    /**
     * @brief Interning store for pair macrostates.
     */
    struct PairStore {
        /// Storage mode chosen for the pair store.
        enum class Mode : uint8_t {
            SparseHash,
            DenseMatrix,
        };

        Mode mode;
        std::unordered_map<MacroStateId, PairState> sparse_pairs;
        std::vector<PairState> dense_pairs;
        std::unique_ptr<mata::utils::TwoDimensionalMap<MacroStateId, false, DENSE_PAIR_MAX_MATRIX_SIZE>> dense_index;

        /// Build an empty sparse pair store.
        PairStore();
        /// Build a pair store using bounds that allow dense indexing.
        explicit PairStore(size_t lhs_bound, size_t rhs_bound);

        /// Lookup an interned pair by id.
        PairState get(MacroStateId id) const;
        /// Intern one pair state and return its canonical id.
        MacroStateId intern(PairState pair);
    };

    std::vector<size_t> node_to_store_index;
    std::vector<PairStore> pair_stores;
    std::vector<SetStore> set_stores;
    std::vector<TaggedStore> tagged_stores;

    /// Build an empty macrostate store.
    MacroStateStore();
    /// Build stores sized for the reconstructed exec DAG.
    explicit MacroStateStore(
            const std::vector<ExecNode>& nodes, const std::vector<mata::nfa::Nfa>& nfas,
            const std::vector<mata::nft::Nft>& nfts);

    /// Lookup an interned pair state.
    PairState get_pair(NodeId idx, MacroStateId id) const;
    /// Lookup an interned subset state.
    const SetState& get_set(NodeId idx, MacroStateId id) const;
    /// Lookup an interned tagged state.
    TaggedState get_tagged(NodeId idx, MacroStateId id) const;

    /// Intern a subset state for one exec node.
    MacroStateId intern(const NodeId idx, const SetState states);
    /// Intern a pair state for one exec node.
    MacroStateId intern(const NodeId idx, const PairState pair);
    /// Intern a tagged state for one exec node.
    MacroStateId intern(const NodeId idx, const TaggedState& tagged);
};

} // namespace mata::nft::lazy::detail
