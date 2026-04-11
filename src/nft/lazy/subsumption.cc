/**
 * @file subsumption.cc
 * @brief Private subsumption and antichain bucketing implementation for mata::nft::lazy::detail.
 */

#include "subsumption.hh"

namespace mata::nft::lazy::detail {

SubsumptionEngine::SubsumptionEngine(const SubsumptionContext& context)
    : nfas{context.nfas}, nfts{context.nfts}, nodes{context.nodes}, macro_store{context.macro_store},
      precomputed_simulations(context.nfas.size() + context.nfts.size()) {}

void SubsumptionEngine::set_nfa_simulation(const size_t nfa_index, Simlib::Util::BinaryRelation relation) {
    precomputed_simulations[nfa_index] = std::move(relation);
}

void SubsumptionEngine::set_nft_simulation(const size_t nft_index, Simlib::Util::BinaryRelation relation) {
    precomputed_simulations[nfas.size() + nft_index] = std::move(relation);
}

bool SubsumptionEngine::subsumed_state(
        const NodeId node_id, const MacroStateId state1, const MacroStateId state2) const {
    if (state1 == state2) {
        return true;
    }

    const ExecNode& node = nodes[node_id];
    const State s1 = static_cast<State>(state1);
    const State s2 = static_cast<State>(state2);

    switch (node.kind) {
        case ExecKind::LeafNfa:
            return precomputed_simulations[node.lhs].get(s1, s2);

        case ExecKind::LeafNft:
        case ExecKind::Arity2LeafNft:
            return precomputed_simulations[node.lhs + nfas.size()].get(s1, s2);

        case ExecKind::Union:
        case ExecKind::Arity1Union:
        case ExecKind::Arity2Union: {
            const TaggedState tagged1 = macro_store.get_tagged(node_id, state1);
            const TaggedState tagged2 = macro_store.get_tagged(node_id, state2);
            if (tagged1.tag != tagged2.tag) {
                return false;
            }

            return tagged1.tag == TaggedState::Tag::Left ? subsumed_state(node.lhs, tagged1.state, tagged2.state)
                                                         : subsumed_state(node.rhs, tagged1.state, tagged2.state);
        }

        case ExecKind::Intersect:
        case ExecKind::SyncProduct:
        case ExecKind::Arity1Intersect:
        case ExecKind::Arity2Intersect:
        case ExecKind::Arity2SyncProduct: {
            const PairState pair1 = macro_store.get_pair(node_id, state1);
            const PairState pair2 = macro_store.get_pair(node_id, state2);

            return subsumed_state(node.lhs, pair1.lhs, pair2.lhs) && subsumed_state(node.rhs, pair1.rhs, pair2.rhs);
        }

        case ExecKind::Complement:
        case ExecKind::Arity1Complement:
        case ExecKind::Arity2Complement: {
            const SetState& lhs_sub_states = macro_store.get_set(node_id, state2);
            const SetState& rhs_sub_states = macro_store.get_set(node_id, state1);
            if (lhs_sub_states.size() > rhs_sub_states.size()) {
                return false;
            }

            for (const MacroStateId lhs_sub_state : lhs_sub_states) {
                bool subsumed = false;
                for (const MacroStateId rhs_sub_state : rhs_sub_states) {
                    if (subsumed_state(node.lhs, lhs_sub_state, rhs_sub_state)) {
                        subsumed = true;
                        break;
                    }
                }

                if (!subsumed) {
                    return false;
                }
            }

            return true;
        }

        case ExecKind::Identity:
        case ExecKind::Project:
        case ExecKind::Arity2Project:
            return subsumed_state(node.lhs, state1, state2);
    }

    return false;
}

bool SubsumptionEngine::is_subsumed(
        const NodeId root_id, const MacroStateId state, std::unordered_set<MacroStateId>& visited,
        std::unordered_set<MacroStateId>& queued) const {
    for (const MacroStateId visited_state : visited) {
        if (subsumed_state(root_id, state, visited_state)) {
            return true;
        }
    }

    for (const MacroStateId queued_state : queued) {
        if (subsumed_state(root_id, state, queued_state)) {
            return true;
        }
    }

    const auto is_subsumed_by_state = [&](const MacroStateId other_state) {
        return subsumed_state(root_id, other_state, state);
    };

    std::erase_if(visited, is_subsumed_by_state);
    std::erase_if(queued, is_subsumed_by_state);

    return false;
}

} // namespace mata::nft::lazy::detail
