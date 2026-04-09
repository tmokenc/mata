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

struct MacroStateStore {
    using SetStore = std::unordered_map<MacroStateId, SetState>;
    using TaggedStore = std::unordered_map<MacroStateId, TaggedState>;
    static constexpr size_t DENSE_PAIR_MAX_MATRIX_SIZE = 10'000'000;

    static bool can_use_dense_pair_store(std::optional<size_t> lhs_bound, std::optional<size_t> rhs_bound);

    struct PairStore {
        enum class Mode : uint8_t {
            SparseHash,
            DenseMatrix,
        };

        Mode mode;
        std::unordered_map<MacroStateId, PairState> sparse_pairs;
        std::vector<PairState> dense_pairs;
        std::unique_ptr<mata::utils::TwoDimensionalMap<MacroStateId, false, DENSE_PAIR_MAX_MATRIX_SIZE>> dense_index;

        PairStore();
        explicit PairStore(size_t lhs_bound, size_t rhs_bound);

        PairState get(MacroStateId id) const;
        MacroStateId intern(PairState pair);
    };

    std::vector<size_t> node_to_store_index;
    std::vector<PairStore> pair_stores;
    std::vector<SetStore> set_stores;
    std::vector<TaggedStore> tagged_stores;

    MacroStateStore();
    explicit MacroStateStore(
            const std::vector<ExecNode>& nodes, const std::vector<mata::nfa::Nfa>& nfas,
            const std::vector<mata::nft::Nft>& nfts);

    PairState get_pair(NodeId idx, MacroStateId id) const;
    const SetState& get_set(NodeId idx, MacroStateId id) const;
    TaggedState get_tagged(NodeId idx, MacroStateId id) const;

    MacroStateId intern(const NodeId idx, const SetState states);
    MacroStateId intern(const NodeId idx, const PairState pair);
    MacroStateId intern(const NodeId idx, const TaggedState& tagged);
};

} // namespace mata::nft::lazy::detail
