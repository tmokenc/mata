/**
 * @file subsumption.cc
 * @brief Private subsumption and antichain-pruning implementation for mata::nft::lazy::detail.
 */

#include "subsumption.hh"

#include "mata/nfa/algorithms.hh"
#include "mata/nft/algorithms.hh"

#include <algorithm>
#include <iostream>

namespace mata::nft::lazy::detail {

SubsumptionEngine::SubsumptionEngine(const SubsumptionContext& context)
    : context(context), precomputed_simulation_nfas(context.nfas.size()),
      precomputed_simulation_nfts(context.nfts.size()), antichain{}, buckets{}, caches{}, filter_lhs_nfa_index{},
      lhs_fwd_sim_neighbors{}, lhs_bwd_sim_neighbors{} {}

void SubsumptionEngine::initialize_leaf_simulations(const NodeId root_id) {
    antichain.clear();
    buckets.clear();
    caches.clear();
    caches.resize(context.nodes.size());
    lhs_fwd_sim_neighbors.clear();
    lhs_bwd_sim_neighbors.clear();
    filter_lhs_nfa_index = std::nullopt;

    configure_antichain_filter(root_id);

    std::vector<bool> visited(context.nodes.size(), false);
    initialize_leaf_simulations_impl(root_id, visited);

    build_lhs_sim_neighbors();
}

void SubsumptionEngine::configure_antichain_filter(const NodeId root_id) {
    filter_root_node = root_id;
    filter_kind = AntichainFilterKind::None;
    filter_fingerprint_source_node = root_id;

    if (root_id >= context.nodes.size()) {
        return;
    }

    const NodeKind root_kind = context.nodes[root_id].kind;
    switch (root_kind) {
        case NodeKind::Union:
            // Subsumption requires matching tags — use the tag as a discriminator so only
            // same-side entries are compared.
            filter_kind = AntichainFilterKind::Tag;
            break;

        case NodeKind::Complement:
            // Subsumption for complement reverses the subset order: s1 ⊑ s2 iff sub(s2) ⊆ sub(s1).
            // Smaller set states can only subsume larger or equal ones, so bucket by set size and
            // check only entries with size ≤ the query.
            filter_kind = AntichainFilterKind::RootSetSize;
            break;

        case NodeKind::Intersect:
        case NodeKind::SyncProduct: {
            // Language-inclusion pattern: Intersect(L, Complement(R)).  The rhs set state grows as
            // the search progresses; bucket by its size for the same reason as RootSetSize.
            const NodeId rhs = context.nodes[root_id].rhs;
            if (rhs < context.nodes.size() && context.nodes[rhs].kind == NodeKind::Complement) {
                filter_kind = AntichainFilterKind::IntersectRhsSetSize;
                filter_fingerprint_source_node = rhs;

                // When the lhs is a plain NFA leaf we can also exploit its simulation relation to
                // widen the antichain check across simulated lhs states.
                const NodeId lhs = context.nodes[root_id].lhs;
                if (context.nodes[lhs].kind == NodeKind::LeafNfa) {
                    filter_lhs_nfa_index = context.nodes[lhs].lhs;
                }
            }
            break;
        }

        default:
            break;
    }
}

uint32_t SubsumptionEngine::compute_fingerprint(const MacroStateId state) const {
    switch (filter_kind) {
        case AntichainFilterKind::Tag:
            return static_cast<uint32_t>(context.macro_store.get_tagged(filter_root_node, state).tag);

        case AntichainFilterKind::RootSetSize:
            return static_cast<uint32_t>(context.macro_store.get_set(filter_root_node, state).size());

        case AntichainFilterKind::IntersectRhsSetSize: {
            const PairState pair = context.macro_store.get_pair(filter_root_node, state);
            return static_cast<uint32_t>(context.macro_store.get_set(filter_fingerprint_source_node, pair.rhs).size());
        }

        case AntichainFilterKind::None:
            break;
    }

    return 0;
}

SubsumptionEngine::InnerBuckets& SubsumptionEngine::inner_bucket(const MacroStateId state) {
    const MacroStateId key = (filter_kind == AntichainFilterKind::IntersectRhsSetSize)
                                     ? context.macro_store.get_pair(filter_root_node, state).lhs
                                     : MacroStateId{0};
    return buckets[key];
}

std::vector<MacroStateId>& SubsumptionEngine::get_or_insert_bucket(InnerBuckets& inner, const uint32_t key) {
    const auto cmp = [](const InnerBucket& b, uint32_t k) { return b.first < k; };
    auto it = std::lower_bound(inner.begin(), inner.end(), key, cmp);
    if (it == inner.end() || it->first != key) {
        it = inner.insert(it, {key, {}});
    }
    return it->second;
}

// check_range: the buckets that might contain an antichain entry subsuming the query state.
// remove_range: the buckets that might contain entries the query state now makes redundant.
//
// For Tag: subsumption requires equal tags, so only the exact-match bucket matters for both.
// For set-size filters: s2 subsumes s1 iff sub(s2) ⊆ sub(s1), which means sub(s2) can be no
// larger than sub(s1).  So:
//   - check: candidates have size ≤ fp  (smaller or equal sets can subsume the query)
//   - remove: candidates have size ≥ fp  (larger or equal sets may now be dominated)

SubsumptionEngine::BucketsRange SubsumptionEngine::check_range(InnerBuckets& inner, const uint32_t fp) const {
    const auto lower_cmp = [](const InnerBucket& b, uint32_t k) { return b.first < k; };
    switch (filter_kind) {
        case AntichainFilterKind::Tag: {
            const auto it = std::lower_bound(inner.begin(), inner.end(), fp, lower_cmp);
            return (it != inner.end() && it->first == fp) ? BucketsRange{it, std::next(it)}
                                                          : BucketsRange{inner.end(), inner.end()};
        }
        case AntichainFilterKind::RootSetSize:
        case AntichainFilterKind::IntersectRhsSetSize: {
            const auto upper_cmp = [](uint32_t k, const InnerBucket& b) { return k < b.first; };
            return {inner.begin(), std::upper_bound(inner.begin(), inner.end(), fp, upper_cmp)};
        }
        case AntichainFilterKind::None:
            return {inner.begin(), inner.end()};
    }
    return {inner.begin(), inner.end()};
}

SubsumptionEngine::BucketsRange SubsumptionEngine::remove_range(InnerBuckets& inner, const uint32_t fp) const {
    const auto lower_cmp = [](const InnerBucket& b, uint32_t k) { return b.first < k; };
    switch (filter_kind) {
        case AntichainFilterKind::Tag: {
            const auto it = std::lower_bound(inner.begin(), inner.end(), fp, lower_cmp);
            return (it != inner.end() && it->first == fp) ? BucketsRange{it, std::next(it)}
                                                          : BucketsRange{inner.end(), inner.end()};
        }
        case AntichainFilterKind::RootSetSize:
        case AntichainFilterKind::IntersectRhsSetSize:
            return {std::lower_bound(inner.begin(), inner.end(), fp, lower_cmp), inner.end()};
        case AntichainFilterKind::None:
            return {inner.begin(), inner.end()};
    }
    return {inner.begin(), inner.end()};
}

void SubsumptionEngine::initialize_leaf_simulations_impl(const NodeId node_id, std::vector<bool>& visited) {
    if (visited[node_id]) {
        return;
    }

    const ExecNode& node = context.nodes[node_id];

    switch (node.kind) {
        case NodeKind::LeafNfa: {
            Simlib::Util::BinaryRelation relation = mata::nfa::algorithms::compute_relation(context.nfas[node.lhs]);
            precomputed_simulation_nfas[node.lhs] = std::move(relation);
            break;
        }

        case NodeKind::LeafNft: {
            Simlib::Util::BinaryRelation relation = mata::nft::algorithms::compute_relation(context.nfts[node.lhs]);
            precomputed_simulation_nfts[node.lhs] = std::move(relation);
            break;
        }

        case NodeKind::Union:
        case NodeKind::Intersect:
        case NodeKind::SyncProduct:
        case NodeKind::DiagonalSlice:
            initialize_leaf_simulations_impl(node.lhs, visited);
            initialize_leaf_simulations_impl(node.rhs, visited);
            break;

        case NodeKind::Complement:
        case NodeKind::Identity:
        case NodeKind::Project:
            initialize_leaf_simulations_impl(node.lhs, visited);
            break;
    }

    visited[node_id] = true;
}

bool SubsumptionEngine::subsumed_state(const NodeId node_id, const MacroStateId state1, const MacroStateId state2) {
    if (state1 == state2) {
        return true;
    }

    const ExecNode& node = context.nodes[node_id];
    const State s1 = static_cast<State>(state1);
    const State s2 = static_cast<State>(state2);
    bool result = false;

    switch (node.kind) {
        case NodeKind::LeafNfa:
            result = precomputed_simulation_nfas[node.lhs].get(s1, s2);
            break;

        case NodeKind::LeafNft:
            result = precomputed_simulation_nfts[node.lhs].get(s1, s2);
            break;

        case NodeKind::Union: {
            if (const std::optional<bool> cached_result = caches[node_id].get(state1, state2)) {
                return *cached_result;
            }

            const TaggedState tagged1 = context.macro_store.get_tagged(node_id, state1);
            const TaggedState tagged2 = context.macro_store.get_tagged(node_id, state2);
            if (tagged1.tag != tagged2.tag) {
                result = false;
                break;
            }

            result = tagged1.tag == TaggedState::Tag::Left ? subsumed_state(node.lhs, tagged1.state, tagged2.state)
                                                           : subsumed_state(node.rhs, tagged1.state, tagged2.state);
            caches[node_id].set(state1, state2, result);
            break;
        }

        case NodeKind::Intersect:
        case NodeKind::SyncProduct:
        case NodeKind::DiagonalSlice: {
            if (const std::optional<bool> cached_result = caches[node_id].get(state1, state2)) {
                return *cached_result;
            }

            const PairState pair1 = context.macro_store.get_pair(node_id, state1);
            const PairState pair2 = context.macro_store.get_pair(node_id, state2);

            result = subsumed_state(node.lhs, pair1.lhs, pair2.lhs) && subsumed_state(node.rhs, pair1.rhs, pair2.rhs);
            caches[node_id].set(state1, state2, result);
            break;
        }

        case NodeKind::Complement: {
            if (const std::optional<bool> cached_result = caches[node_id].get(state1, state2)) {
                return *cached_result;
            }

            // Complement reverses the subsumption order: complement(s1) ⊑ complement(s2) iff
            // sub(s2) ⊆ sub(s1).  The argument roles are therefore intentionally swapped here.
            const SetState& lhs_sub_states = context.macro_store.get_set(node_id, state2);
            const SetState& rhs_sub_states = context.macro_store.get_set(node_id, state1);

            if (lhs_sub_states.size() > rhs_sub_states.size()) {
                result = false;
                break;
            }

            result = true;
            for (const MacroStateId lhs_sub_state : lhs_sub_states) {
                bool subsumed = false;

                for (const MacroStateId rhs_sub_state : rhs_sub_states) {
                    if (subsumed_state(node.lhs, lhs_sub_state, rhs_sub_state)) {
                        subsumed = true;
                        break;
                    }
                }

                if (!subsumed) {
                    result = false;
                    break;
                }
            }

            caches[node_id].set(state1, state2, result);

            break;
        }

        case NodeKind::Identity:
        case NodeKind::Project:
            result = subsumed_state(node.lhs, state1, state2);
            break;
    }

    return result;
}

void SubsumptionEngine::build_lhs_sim_neighbors() {
    if (!filter_lhs_nfa_index.has_value())
        return;

    const size_t nfa_idx = *filter_lhs_nfa_index;
    const Simlib::Util::BinaryRelation& sim = precomputed_simulation_nfas[nfa_idx];
    const size_t n = context.nfas[nfa_idx].num_of_states();

    lhs_fwd_sim_neighbors.resize(n);
    lhs_bwd_sim_neighbors.resize(n);

    // TODO: Should unroll this loop?
    for (size_t q = 0; q < n; ++q) {
        for (size_t q2 = 0; q2 < n; ++q2) {
            if (sim.get(q, q2))
                lhs_fwd_sim_neighbors[q].push_back(static_cast<MacroStateId>(q2));
            if (sim.get(q2, q))
                lhs_bwd_sim_neighbors[q].push_back(static_cast<MacroStateId>(q2));
        }
    }
}

bool SubsumptionEngine::is_subsumed_with_lhs_sim(const NodeId root_id, const MacroStateId state, const uint32_t fp) {
    const MacroStateId lhs = context.macro_store.get_pair(filter_root_node, state).lhs;

    // Check: find any antichain entry (q', S') in outer bucket q' where lhs ≤_sim q'.
    if (lhs < static_cast<MacroStateId>(lhs_fwd_sim_neighbors.size())) {
        for (const MacroStateId sim_key : lhs_fwd_sim_neighbors[lhs]) {
            const auto outer_it = buckets.find(sim_key);
            if (outer_it == buckets.end())
                continue;
            auto [cb, ce] = check_range(outer_it->second, fp);
            for (auto it = cb; it != ce; ++it) {
                for (const MacroStateId entry : it->second) {
                    if (subsumed_state(root_id, state, entry))
                        return true;
                }
            }
        }
    }

    // Remove: find antichain entries (q', S') in outer bucket q' where q' ≤_sim lhs.
    if (lhs < static_cast<MacroStateId>(lhs_bwd_sim_neighbors.size())) {
        for (const MacroStateId sim_key : lhs_bwd_sim_neighbors[lhs]) {
            const auto outer_it = buckets.find(sim_key);
            if (outer_it == buckets.end())
                continue;
            auto [rb, re] = remove_range(outer_it->second, fp);
            for (auto it = rb; it != re; ++it) {
                std::erase_if(it->second, [&](const MacroStateId entry) {
                    if (subsumed_state(root_id, entry, state)) {
                        antichain.erase(entry);
                        return true;
                    }
                    return false;
                });
            }
        }
    }

    InnerBuckets& inner = inner_bucket(state);
    antichain.insert(state);
    get_or_insert_bucket(inner, fp).push_back(state);
    return false;
}

bool SubsumptionEngine::is_subsumed(const NodeId root_id, const MacroStateId state) {
    const uint32_t fp = compute_fingerprint(state);

    if (!lhs_fwd_sim_neighbors.empty()) {
        return is_subsumed_with_lhs_sim(root_id, state, fp);
    }

    InnerBuckets& inner = inner_bucket(state);

    auto [cb, ce] = check_range(inner, fp);
    for (auto it = cb; it != ce; ++it) {
        for (const MacroStateId entry : it->second) {
            if (subsumed_state(root_id, state, entry))
                return true;
        }
    }

    auto [rb, re] = remove_range(inner, fp);
    for (auto it = rb; it != re; ++it) {
        std::erase_if(it->second, [&](const MacroStateId entry) {
            if (subsumed_state(root_id, entry, state)) {
                antichain.erase(entry);
                return true;
            }
            return false;
        });
    }

    antichain.insert(state);
    get_or_insert_bucket(inner, fp).push_back(state);
    return false;
}

void SubsumptionEngine::minimize(const NodeId root_id, SetState& state) {
    canonicalize_set_state(state);

    bool changed = true;
    while (changed) {
        changed = false;

        for (auto candidate_it = state.begin(); candidate_it != state.end(); ++candidate_it) {
            const MacroStateId candidate = *candidate_it;
            bool remove_candidate = false;

            for (const MacroStateId other : state) {
                if (candidate != other && subsumed_state(root_id, candidate, other)) {
                    remove_candidate = true;
                    break;
                }
            }

            if (remove_candidate) {
                state.erase(candidate_it);
                changed = true;
                break;
            }
        }
    }
}

bool SubsumptionEngine::is_pruned(const MacroStateId state) const { return !antichain.contains(state); }

void SubsumptionEngine::print_statistics() const {
    size_t cache_stored = 0;
    size_t cached_true = 0;

    for (const SubsumptionCache& cache : caches) {
        cache_stored += cache.size();
        cached_true += cache.count_true();
    }

    std::cout << "Subsumption cache size: " << cache_stored << std::endl;
    std::cout << "Subsumption cache true ratio: "
              << (cache_stored == 0 ? 0 : static_cast<double>(cached_true) / static_cast<double>(cache_stored))
              << std::endl;
    std::cout << "Current antichain size: " << antichain.size() << std::endl;
}

} // namespace mata::nft::lazy::detail
