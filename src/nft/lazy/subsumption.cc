/**
 * @file subsumption.cc
 * @brief Private subsumption and antichain-pruning implementation for mata::nft::lazy::detail.
 */

#include "subsumption.hh"

#include "mata/nfa/algorithms.hh"
#include "mata/nft/algorithms.hh"

namespace mata::nft::lazy::detail {

namespace {

    constexpr bool
    is_antichain_prune_eligible(const uint64_t inserted_iteration, const uint64_t current_iteration) noexcept {
        return current_iteration >= inserted_iteration &&
               current_iteration - inserted_iteration >= kMinAntichainPruneAgeIterations;
    }

} // namespace

std::optional<bool> SubsumptionCache::get(MacroStateId state1, MacroStateId state2) const {
    auto it = cache.find({state1, state2});

    if (it != cache.end()) {
        return it->second;
    }

    return std::nullopt;
}

void SubsumptionCache::set(MacroStateId state1, MacroStateId state2, bool result) { cache[{state1, state2}] = result; }

SubsumptionEngine::SubsumptionEngine(const SubsumptionContext& context)
    : nfas{context.nfas}, nfts{context.nfts}, nodes{context.nodes}, macro_store{context.macro_store},
      precomputed_simulation_nfas(context.nfas.size()), precomputed_simulation_nfts(context.nfts.size()), antichain{},
      current_iteration{0}, caches{} {}

void SubsumptionEngine::initialize_leaf_simulations(const NodeId root_id) {
    antichain.clear();
    caches.clear();
    caches.resize(nodes.size());
    current_iteration = 0;

    std::vector<bool> visited(nodes.size(), false);
    initialize_leaf_simulations_impl(root_id, visited);
}

void SubsumptionEngine::initialize_leaf_simulations_impl(const NodeId node_id, std::vector<bool>& visited) {
    if (visited[node_id]) {
        return;
    }

    const ExecNode& node = nodes[node_id];

    switch (node.kind) {
        case ExecKind::LeafNfa: {
            Simlib::Util::BinaryRelation relation = mata::nfa::algorithms::compute_relation(nfas[node.lhs]);
            precomputed_simulation_nfas[node.lhs] = std::move(relation);
            break;
        }

        case ExecKind::LeafNft:
        case ExecKind::Arity2LeafNft: {
            Simlib::Util::BinaryRelation relation = mata::nft::algorithms::compute_relation(nfts[node.lhs]);
            precomputed_simulation_nfts[node.lhs] = std::move(relation);
            break;
        }

        case ExecKind::Union:
        case ExecKind::Intersect:
        case ExecKind::Arity1Union:
        case ExecKind::Arity1Intersect:
        case ExecKind::Arity2Union:
        case ExecKind::Arity2Intersect:
        case ExecKind::SyncProduct:
        case ExecKind::Arity2SyncProduct:
            initialize_leaf_simulations_impl(node.lhs, visited);
            initialize_leaf_simulations_impl(node.rhs, visited);
            break;

        case ExecKind::Complement:
        case ExecKind::Identity:
        case ExecKind::Project:
        case ExecKind::Arity1Complement:
        case ExecKind::Arity2Complement:
        case ExecKind::Arity2Project:
            initialize_leaf_simulations_impl(node.lhs, visited);
            break;
    }

    visited[node_id] = true;
}

bool SubsumptionEngine::subsumed_state(const NodeId node_id, const MacroStateId state1, const MacroStateId state2) {
    if (state1 == state2) {
        return true;
    }

    const ExecNode& node = nodes[node_id];
    const State s1 = static_cast<State>(state1);
    const State s2 = static_cast<State>(state2);
    bool result = false;

    switch (node.kind) {
        case ExecKind::LeafNfa:
            result = precomputed_simulation_nfas[node.lhs].get(s1, s2);
            break;

        case ExecKind::LeafNft:
        case ExecKind::Arity2LeafNft:
            result = precomputed_simulation_nfts[node.lhs].get(s1, s2);
            break;

        case ExecKind::Union:
        case ExecKind::Arity1Union:
        case ExecKind::Arity2Union: {
            if (const std::optional<bool> cached_result = caches[node_id].get(state1, state2)) {
                return *cached_result;
            }

            const TaggedState tagged1 = macro_store.get_tagged(node_id, state1);
            const TaggedState tagged2 = macro_store.get_tagged(node_id, state2);
            if (tagged1.tag != tagged2.tag) {
                result = false;
                break;
            }

            result = tagged1.tag == TaggedState::Tag::Left ? subsumed_state(node.lhs, tagged1.state, tagged2.state)
                                                           : subsumed_state(node.rhs, tagged1.state, tagged2.state);
            caches[node_id].set(state1, state2, result);
            break;
        }

        case ExecKind::Intersect:
        case ExecKind::SyncProduct:
        case ExecKind::Arity1Intersect:
        case ExecKind::Arity2Intersect:
        case ExecKind::Arity2SyncProduct: {
            if (const std::optional<bool> cached_result = caches[node_id].get(state1, state2)) {
                return *cached_result;
            }

            const PairState pair1 = macro_store.get_pair(node_id, state1);
            const PairState pair2 = macro_store.get_pair(node_id, state2);

            result = subsumed_state(node.lhs, pair1.lhs, pair2.lhs) && subsumed_state(node.rhs, pair1.rhs, pair2.rhs);
            caches[node_id].set(state1, state2, result);
            break;
        }

        case ExecKind::Complement:
        case ExecKind::Arity1Complement:
        case ExecKind::Arity2Complement: {
            if (const std::optional<bool> cached_result = caches[node_id].get(state1, state2)) {
                return *cached_result;
            }

            const SetState& lhs_sub_states = macro_store.get_set(node_id, state2);
            const SetState& rhs_sub_states = macro_store.get_set(node_id, state1);

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

        case ExecKind::Identity:
        case ExecKind::Project:
        case ExecKind::Arity2Project:
            result = subsumed_state(node.lhs, state1, state2);
            break;
    }

    return result;
}

bool SubsumptionEngine::is_subsumed(const NodeId root_id, const MacroStateId state) {
    ++current_iteration;

    for (const auto& [antichain_state, inserted_iteration] : antichain) {
        if (!is_antichain_prune_eligible(inserted_iteration, current_iteration)) {
            continue;
        }

        if (subsumed_state(root_id, state, antichain_state)) {
            return true;
        }
    }

    std::vector<MacroStateId> erased_states{};
    for (const auto& [other_state, inserted_iteration] : antichain) {
        if (!is_antichain_prune_eligible(inserted_iteration, current_iteration)) {
            continue;
        }

        if (subsumed_state(root_id, other_state, state)) {
            erased_states.push_back(other_state);
        }
    }

    for (const MacroStateId erased_state : erased_states) {
        antichain.erase(erased_state);
    }

    antichain.insert_or_assign(state, current_iteration);

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

} // namespace mata::nft::lazy::detail
