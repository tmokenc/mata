/**
 * @file macrostate_store.cc
 * @brief Private macrostate storage for mata::nft::lazy::detail.
 */

#include "macrostate_store.hh"

#include <algorithm>
#include <cassert>
#include <limits>
#include <utility>

namespace mata::nft::lazy::detail {

bool MacroStateStore::can_use_dense_pair_store(
        const std::optional<size_t> lhs_bound, const std::optional<size_t> rhs_bound) {
    return lhs_bound.has_value() && rhs_bound.has_value() && *lhs_bound <= DENSE_PAIR_MAX_MATRIX_SIZE &&
           *rhs_bound <= DENSE_PAIR_MAX_MATRIX_SIZE &&
           *lhs_bound <= DENSE_PAIR_MAX_MATRIX_SIZE / std::max<size_t>(*rhs_bound, 1);
}

MacroStateStore::PairStore::PairStore() : mode{Mode::SparseHash}, sparse_pairs{}, dense_pairs{}, dense_index{} {}

MacroStateStore::PairStore::PairStore(const size_t lhs_bound, const size_t rhs_bound)
    : mode{Mode::DenseMatrix}, sparse_pairs{}, dense_pairs{},
      dense_index{std::make_unique<mata::utils::TwoDimensionalMap<MacroStateId, false, DENSE_PAIR_MAX_MATRIX_SIZE>>(
              lhs_bound, rhs_bound)} {}

PairState MacroStateStore::PairStore::get(const MacroStateId id) const {
    if (mode == Mode::DenseMatrix) {
        assert(id < dense_pairs.size());
        return dense_pairs[id];
    }

    const auto it = sparse_pairs.find(id);
    assert(it != sparse_pairs.end());
    return it->second;
}

MacroStateId MacroStateStore::PairStore::intern(const PairState pair) {
    if (mode == Mode::DenseMatrix) {
        const MacroStateId existing = dense_index->get(pair.lhs, pair.rhs);
        if (existing != std::numeric_limits<MacroStateId>::max()) {
            return existing;
        }

        const MacroStateId id = static_cast<MacroStateId>(dense_pairs.size());
        dense_pairs.push_back(pair);
        dense_index->insert(pair.lhs, pair.rhs, id);
        return id;
    }

    MacroStateId id = hash_pair(pair);
    while (true) {
        const auto it = sparse_pairs.find(id);
        if (it == sparse_pairs.end()) {
            break;
        }
        if (it->second.lhs == pair.lhs && it->second.rhs == pair.rhs) {
            return id;
        }
        ++id;
    }

    sparse_pairs.emplace(id, pair);
    return id;
}

MacroStateStore::MacroStateStore() : node_to_store_index{}, pair_stores{}, set_stores{}, tagged_stores{} {}

MacroStateStore::MacroStateStore(
        const std::vector<ExecNode>& nodes, const std::vector<mata::nfa::Nfa>& nfas,
        const std::vector<mata::nft::Nft>& nfts)
    : node_to_store_index(nodes.size(), 0), pair_stores{}, set_stores{}, tagged_stores{} {
    size_t pair_node_count = 0;
    size_t set_node_count = 0;
    size_t tagged_node_count = 0;
    for (const ExecNode& node : nodes) {
        switch (node.kind) {
            case ExecKind::Union:
            case ExecKind::Arity1Union:
            case ExecKind::Arity2Union:
                tagged_node_count += 1;
                break;
            case ExecKind::Intersect:
            case ExecKind::SyncProduct:
            case ExecKind::Arity1Intersect:
            case ExecKind::Arity2Intersect:
            case ExecKind::Arity2SyncProduct:
                pair_node_count += 1;
                break;
            case ExecKind::Complement:
            case ExecKind::Arity1Complement:
            case ExecKind::Arity2Complement:
                set_node_count += 1;
                break;
            case ExecKind::LeafNfa:
            case ExecKind::LeafNft:
            case ExecKind::Identity:
            case ExecKind::Project:
            case ExecKind::Arity2LeafNft:
            case ExecKind::Arity2Project:
                break;
        }
    }
    pair_stores.reserve(pair_node_count);
    set_stores.reserve(set_node_count);
    tagged_stores.reserve(tagged_node_count);

    std::vector<std::optional<size_t>> dense_macrostate_bounds(nodes.size(), std::nullopt);

    for (size_t i = 0; i < nodes.size(); ++i) {
        switch (nodes[i].kind) {
            case ExecKind::LeafNfa:
                dense_macrostate_bounds[i] = nfas[nodes[i].lhs].num_of_states();
                break;

            case ExecKind::LeafNft:
            case ExecKind::Arity2LeafNft:
                dense_macrostate_bounds[i] = nfts[nodes[i].lhs].num_of_states();
                break;

            case ExecKind::Union:
            case ExecKind::Arity1Union:
            case ExecKind::Arity2Union:
                node_to_store_index[i] = tagged_stores.size();
                tagged_stores.emplace_back();
                dense_macrostate_bounds[i] = std::nullopt;
                break;

            case ExecKind::Intersect:
            case ExecKind::SyncProduct:
            case ExecKind::Arity1Intersect:
            case ExecKind::Arity2Intersect:
            case ExecKind::Arity2SyncProduct: {
                const std::optional<size_t> lhs_bound = dense_macrostate_bounds[nodes[i].lhs];
                const std::optional<size_t> rhs_bound = dense_macrostate_bounds[nodes[i].rhs];
                node_to_store_index[i] = pair_stores.size();
                if (can_use_dense_pair_store(lhs_bound, rhs_bound)) {
                    pair_stores.emplace_back(*lhs_bound, *rhs_bound);
                    dense_macrostate_bounds[i] = (*lhs_bound) * (*rhs_bound);
                } else {
                    pair_stores.emplace_back();
                    dense_macrostate_bounds[i] = std::nullopt;
                }
                break;
            }

            case ExecKind::Complement:
            case ExecKind::Arity1Complement:
            case ExecKind::Arity2Complement:
                node_to_store_index[i] = set_stores.size();
                set_stores.emplace_back();
                dense_macrostate_bounds[i] = std::nullopt;
                break;

            case ExecKind::Identity:
            case ExecKind::Project:
            case ExecKind::Arity2Project:
                dense_macrostate_bounds[i] = dense_macrostate_bounds[nodes[i].lhs];
                break;
        }
    }
}

PairState MacroStateStore::get_pair(const NodeId idx, const MacroStateId id) const {
    const PairStore& store = pair_stores[node_to_store_index[idx]];
    return store.get(id);
}

const SetState& MacroStateStore::get_set(const NodeId idx, const MacroStateId id) const {
    const SetStore& store = set_stores[node_to_store_index[idx]];
    const auto it = store.find(id);
    assert(it != store.end());
    return it->second;
}

TaggedState MacroStateStore::get_tagged(const NodeId idx, const MacroStateId id) const {
    const TaggedStore& store = tagged_stores[node_to_store_index[idx]];
    const auto it = store.find(id);
    assert(it != store.end());
    return it->second;
}

MacroStateId MacroStateStore::intern(const NodeId idx, SetState states) {
    SetStore& store = set_stores[node_to_store_index[idx]];
    MacroStateId id = hash_states(states);

    while (true) {
        const auto it = store.find(id);
        if (it == store.end()) {
            break;
        }
        if (it->second == states) {
            return id;
        }
        ++id;
    }

    store.emplace(id, std::move(states));
    return id;
}

MacroStateId MacroStateStore::intern(const NodeId idx, const PairState pair) {
    PairStore& store = pair_stores[node_to_store_index[idx]];
    return store.intern(pair);
}

MacroStateId MacroStateStore::intern(const NodeId idx, const TaggedState& tagged) {
    TaggedStore& store = tagged_stores[node_to_store_index[idx]];
    MacroStateId id = hash_tagged(tagged);

    while (true) {
        const auto it = store.find(id);
        if (it == store.end()) {
            break;
        }
        if (it->second.state == tagged.state && it->second.tag == tagged.tag) {
            return id;
        }
        ++id;
    }

    store.emplace(id, tagged);
    return id;
}

} // namespace mata::nft::lazy::detail
