/**
 * @file subsumption.cc
 * @brief Private subsumption and antichain bucketing implementation for mata::nft::lazy::detail.
 */

#include "subsumption.hh"

#include <utility>

namespace mata::nft::lazy::detail {

SubsumptionEngine::SubsumptionEngine(const SubsumptionContext& context)
    : nfas{context.nfas},
      nfts{context.nfts},
      nodes{context.nodes},
      macro_store{context.macro_store},
      precomputed_simulations(context.nfas.size() + context.nfts.size()),
      bucket_key_cache{} {}

void SubsumptionEngine::set_nfa_simulation(const size_t nfa_index, Simlib::Util::BinaryRelation relation) {
    precomputed_simulations[nfa_index] = std::move(relation);
}

void SubsumptionEngine::set_nft_simulation(const size_t nft_index, Simlib::Util::BinaryRelation relation) {
    precomputed_simulations[nfas.size() + nft_index] = std::move(relation);
}

bool SubsumptionEngine::subsumed_state(const NodeId node_id, const MacroStateId state1, const MacroStateId state2) const {
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

            return tagged1.tag == TaggedState::Tag::Left
                           ? subsumed_state(node.lhs, tagged1.state, tagged2.state)
                           : subsumed_state(node.rhs, tagged1.state, tagged2.state);
        }

        case ExecKind::Intersect:
        case ExecKind::SyncProduct:
        case ExecKind::Arity1Intersect:
        case ExecKind::Arity2Intersect:
        case ExecKind::Arity2SyncProduct: {
            const PairState pair1 = macro_store.get_pair(node_id, state1);
            const PairState pair2 = macro_store.get_pair(node_id, state2);

            return subsumed_state(node.lhs, pair1.lhs, pair2.lhs) &&
                   subsumed_state(node.rhs, pair1.rhs, pair2.rhs);
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
        case ExecKind::Arity2Identity:
        case ExecKind::Arity2Project:
            return subsumed_state(node.lhs, state1, state2);
    }

    return false;
}

AntichainBucketKey SubsumptionEngine::bucket_key(const NodeId node_id, const MacroStateId state) {
    const uint64_t cache_key = node_state_key(node_id, state);
    const auto it = bucket_key_cache.find(cache_key);
    if (it != bucket_key_cache.end()) {
        return it->second;
    }

    const ExecNode& node = nodes[node_id];
    AntichainBucketKey key = static_cast<AntichainBucketKey>(static_cast<uint8_t>(node.kind)) + 1;
    AntichainBucketKey result = key;

    switch (node.kind) {
        case ExecKind::LeafNfa:
        case ExecKind::LeafNft:
        case ExecKind::Arity2LeafNft:
            result = mix_bucket_key(key, state);
            break;

        case ExecKind::Union:
        case ExecKind::Arity1Union:
        case ExecKind::Arity2Union: {
            const TaggedState tagged = macro_store.get_tagged(node_id, state);
            const NodeId child_id = tagged.tag == TaggedState::Tag::Left ? node.lhs : node.rhs;
            key = mix_bucket_key(key, static_cast<AntichainBucketKey>(tagged.tag));
            result = mix_bucket_key(key, bucket_key(child_id, tagged.state));
            break;
        }

        case ExecKind::Intersect:
        case ExecKind::SyncProduct:
        case ExecKind::Arity1Intersect:
        case ExecKind::Arity2Intersect:
        case ExecKind::Arity2SyncProduct: {
            const PairState pair = macro_store.get_pair(node_id, state);
            key = mix_bucket_key(key, bucket_key(node.lhs, pair.lhs));
            result = mix_bucket_key(key, bucket_key(node.rhs, pair.rhs));
            break;
        }

        case ExecKind::Complement:
        case ExecKind::Arity1Complement:
        case ExecKind::Arity2Complement: {
            const SetState& set_state = macro_store.get_set(node_id, state);
            key = mix_bucket_key(key, set_state.size());
            AntichainBucketKey children_hash = 0;
            for (const MacroStateId sub_state : set_state) {
                children_hash ^= mix_hash64(bucket_key(node.lhs, sub_state));
            }
            result = mix_bucket_key(key, children_hash);
            break;
        }

        case ExecKind::Identity:
        case ExecKind::Project:
        case ExecKind::Arity2Identity:
        case ExecKind::Arity2Project:
            result = mix_bucket_key(key, bucket_key(node.lhs, state));
            break;
    }

    bucket_key_cache.emplace(cache_key, result);
    return result;
}

AntichainBucketKey SubsumptionEngine::root_bucket_key(const NodeId root_id, const MacroStateId state) {
    return bucket_key(root_id, state);
}

bool SubsumptionEngine::is_subsumed(
        const NodeId root_id, const MacroStateId state, const AntichainBucketKey bucket_key,
        std::unordered_set<MacroStateId>& visited, std::unordered_set<MacroStateId>& queued,
        std::unordered_map<AntichainBucketKey, std::vector<MacroStateId>>& visited_buckets,
        std::unordered_map<AntichainBucketKey, std::vector<MacroStateId>>& queued_buckets) const {
    auto& visited_bucket = visited_buckets[bucket_key];
    for (const MacroStateId visited_state : visited_bucket) {
        if (visited.contains(visited_state) && subsumed_state(root_id, state, visited_state)) {
            return true;
        }
    }

    auto& queued_bucket = queued_buckets[bucket_key];
    for (const MacroStateId queued_state : queued_bucket) {
        if (queued.contains(queued_state) && subsumed_state(root_id, state, queued_state)) {
            return true;
        }
    }

    for (const MacroStateId visited_state : visited_bucket) {
        if (visited.contains(visited_state) && subsumed_state(root_id, visited_state, state)) {
            visited.erase(visited_state);
        }
    }

    for (const MacroStateId queued_state : queued_bucket) {
        if (queued.contains(queued_state) && subsumed_state(root_id, queued_state, state)) {
            queued.erase(queued_state);
        }
    }

    return false;
}

} // namespace mata::nft::lazy::detail
