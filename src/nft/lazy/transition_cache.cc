/**
 * @file transition_cache.cc
 * @brief Private transition-cache implementation for mata::nft::lazy::detail.
 */

#include "transition_cache.hh"

#include <cassert>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace mata::nft::lazy::detail {

namespace {

    const std::vector<uint8_t>&
    cached_special_symbol_levels(const GenericTransitionCacheEntry& entry, const mata::Symbol special_symbol) {
        return special_symbol == mata::nft::EPSILON ? entry.has_epsilon_by_level : entry.has_dont_care_by_level;
    }

    bool cached_arity2_has_special_symbol(const Arity2TransitionCacheEntry& entry, const mata::Symbol special_symbol) {
        if (special_symbol == mata::nft::EPSILON) {
            return entry.first_has_epsilon || entry.second_has_epsilon;
        }
        return entry.first_has_dont_care || entry.second_has_dont_care;
    }

    inline void
    append_generated_state(std::vector<GeneratedMacroState>& states, const GeneratedMacroState& generated_state) {
        for (GeneratedMacroState& state : states) {
            if (state.id == generated_state.id) {
                state.accepting = state.accepting || generated_state.accepting;
                return;
            }
        }

        states.push_back(generated_state);
    }

    template<typename TransitionMapT, typename KeyFactory>
    void append_leaf_nfa_transitions(
            const std::vector<mata::nfa::Nfa>& nfas, const std::vector<ExecNode>& nodes, const AlphabetStore& alphabets,
            const NodeId node_id, const MacroStateId state, TransitionMapT& transitions, KeyFactory&& make_key) {
        const mata::nfa::Nfa& nfa = nfas[nodes[node_id].lhs];
        const mata::nfa::State source_state = static_cast<mata::nfa::State>(state);
        for (const auto& symbol_post : nfa.delta.state_post(source_state)) {
            mata::Symbol resolved_symbol = 0;
            if (!alphabets.try_translate_local_symbol_to_resolved(
                        nfa, node_id, 0, symbol_post.symbol, resolved_symbol)) {
                continue;
            }

            auto& bucket = transitions[make_key(resolved_symbol)];
            for (const mata::nfa::State target_state : symbol_post.targets) {
                append_generated_state(
                        bucket,
                        GeneratedMacroState{static_cast<MacroStateId>(target_state), nfa.final.contains(target_state)});
            }
        }
    }

    template<typename InputTransitionMapT, typename OutputTransitionMapT>
    void append_union_transitions(
            MacroStateStore& macro_store, const NodeId node_id, const TaggedState& tagged,
            const InputTransitionMapT& child_transitions, OutputTransitionMapT& transitions) {
        for (const auto& [label, child_states] : child_transitions) {
            auto& bucket = transitions[label];
            for (const GeneratedMacroState& child_state : child_states) {
                append_generated_state(
                        bucket, GeneratedMacroState{
                                        macro_store.intern(node_id, TaggedState{child_state.id, tagged.tag}),
                                        child_state.accepting});
            }
        }
    }

    template<typename LhsStates, typename RhsStates>
    void append_pair_product_states(
            MacroStateStore& macro_store, const NodeId node_id, const LhsStates& lhs_states,
            const RhsStates& rhs_states, std::vector<GeneratedMacroState>& bucket) {
        for (const GeneratedMacroState& lhs_state : lhs_states) {
            for (const GeneratedMacroState& rhs_state : rhs_states) {
                append_generated_state(
                        bucket, GeneratedMacroState{
                                        macro_store.intern(node_id, PairState{lhs_state.id, rhs_state.id}),
                                        lhs_state.accepting && rhs_state.accepting});
            }
        }
    }

    template<typename InputTransitionMapT, typename OutputTransitionMapT>
    void append_exact_intersection_transitions(
            MacroStateStore& macro_store, const NodeId node_id, const InputTransitionMapT& lhs_transitions,
            const InputTransitionMapT& rhs_transitions, OutputTransitionMapT& transitions) {
        for (const auto& [label, lhs_states] : lhs_transitions) {
            const auto rhs_it = rhs_transitions.find(label);
            if (rhs_it == rhs_transitions.end()) {
                continue;
            }

            auto& bucket = transitions[label];
            append_pair_product_states(macro_store, node_id, lhs_states, rhs_it->second, bucket);
        }
    }

    SymbolTuple extract_sync_levels(const SymbolTuple& tuple, const std::vector<uint8_t>& levels) {
        SymbolTuple extracted{};
        extracted.reserve(levels.size());
        for (const uint8_t level : levels) {
            extracted.push_back(tuple[level]);
        }
        return extracted;
    }

    template<typename AppendMatch>
    void append_exact_sync_product_matches(
            const TransitionMap& lhs_transitions, const TransitionMap& rhs_transitions,
            const std::vector<uint8_t>& lhs_sync_levels, const std::vector<uint8_t>& rhs_sync_levels,
            AppendMatch&& append_match) {
        std::unordered_map<SymbolTuple, std::vector<const TransitionMap::value_type*>, SymbolTupleHash> sync_index{};
        sync_index.reserve(rhs_transitions.size());
        for (const auto& entry : rhs_transitions) {
            sync_index[extract_sync_levels(entry.first, rhs_sync_levels)].push_back(&entry);
        }

        for (const auto& lhs_entry : lhs_transitions) {
            const SymbolTuple sync_signature = extract_sync_levels(lhs_entry.first, lhs_sync_levels);
            const auto matches_it = sync_index.find(sync_signature);
            if (matches_it == sync_index.end()) {
                continue;
            }

            for (const TransitionMap::value_type* rhs_entry : matches_it->second) {
                append_match(lhs_entry, *rhs_entry);
            }
        }
    }

    template<
            typename TransitionMapT, typename ExactIndexT, typename WildcardEntriesT, typename KeyOf,
            typename HasWildcard>
    void split_transition_entries(
            const TransitionMapT& transitions, ExactIndexT& exact_index, WildcardEntriesT& wildcard_entries,
            KeyOf&& key_of, HasWildcard&& has_wildcard) {
        exact_index.reserve(transitions.size());
        wildcard_entries.reserve(transitions.size());
        for (const auto& entry : transitions) {
            if (has_wildcard(entry)) {
                wildcard_entries.push_back(&entry);
            } else {
                exact_index[key_of(entry)].push_back(&entry);
            }
        }
    }

    template<
            typename KeyT, typename ExactIndexT, typename WildcardEntriesT, typename TransitionMapT,
            typename AppendMatch>
    void append_matching_entries(
            const KeyT& key, const bool key_has_wildcard, const ExactIndexT& exact_index,
            const WildcardEntriesT& wildcard_entries, const TransitionMapT& all_entries, AppendMatch&& append_match) {
        if (!key_has_wildcard) {
            if (const auto exact_it = exact_index.find(key); exact_it != exact_index.end()) {
                for (const auto* entry : exact_it->second) {
                    append_match(*entry);
                }
            }
            for (const auto* entry : wildcard_entries) {
                append_match(*entry);
            }
            return;
        }

        for (const auto& entry : all_entries) {
            append_match(entry);
        }
    }

} // namespace

TransitionCache::TransitionCache(const TransitionCacheContext& context)
    : nfas{context.nfas}, nfts{context.nfts}, sync_plans{context.sync_plans}, project_plans{context.project_plans},
      nodes{context.nodes}, macro_store{context.macro_store}, alphabets{context.alphabets}, special_symbols_by_level{},
      visible_transition_cache{}, arity1_visible_transition_cache{}, arity2_visible_transition_cache{} {}

// Fast path: arity-1 visible transitions.

const Arity1TransitionMap& TransitionCache::get_arity1_visible_transitions(
        const NodeId node_id, const MacroStateId state, const TransitionResolver& resolver) {
    initialize_special_symbol_cache();
    const ExecNode& node = nodes[node_id];
    switch (node.kind) {
        case ExecKind::LeafNfa:
        case ExecKind::Arity1Union:
        case ExecKind::Arity1Intersect:
            break;

        default:
            throw std::logic_error(
                    "Arity-1 fast-path transitions are available only for non-complement arity-1 nodes.");
    }

    const uint64_t key = state_cache_key(node_id, state);
    const auto it = arity1_visible_transition_cache.find(key);
    if (it != arity1_visible_transition_cache.end()) {
        return it->second;
    }

    Arity1TransitionBuilder transitions{};

    switch (node.kind) {
        case ExecKind::LeafNfa:
            append_leaf_nfa_transitions(
                    nfas, nodes, alphabets, node_id, state, transitions, [](mata::Symbol symbol) { return symbol; });
            break;

        case ExecKind::Arity1Union: {
            const TaggedState tagged = macro_store.get_tagged(node_id, state);
            const NodeId child_id = tagged.tag == TaggedState::Tag::Left ? node.lhs : node.rhs;
            Arity1TransitionMap child_fallback{};
            const Arity1TransitionMap& child_transitions =
                    resolver.resolve_arity1_visible(child_id, tagged.state, child_fallback);
            append_union_transitions(macro_store, node_id, tagged, child_transitions, transitions);
            break;
        }

        case ExecKind::Arity1Intersect: {
            const PairState pair = macro_store.get_pair(node_id, state);
            Arity1TransitionMap lhs_fallback{};
            Arity1TransitionMap rhs_fallback{};
            const Arity1TransitionMap& lhs_transitions =
                    resolver.resolve_arity1_visible(node.lhs, pair.lhs, lhs_fallback);
            if (lhs_transitions.empty()) {
                break;
            }

            const Arity1TransitionMap& rhs_transitions =
                    resolver.resolve_arity1_visible(node.rhs, pair.rhs, rhs_fallback);
            append_exact_intersection_transitions(macro_store, node_id, lhs_transitions, rhs_transitions, transitions);
            break;
        }

        default:
            break;
    }

    return arity1_visible_transition_cache.emplace(key, Arity1TransitionMap::freeze(std::move(transitions)))
            .first->second;
}


// Fast path: arity-2 visible transitions.

const Arity2TransitionMap& TransitionCache::get_arity2_visible_transitions(
        const NodeId node_id, const MacroStateId state, const TransitionResolver& resolver) {
    initialize_special_symbol_cache();
    const ExecNode& node = nodes[node_id];
    if (!is_arity2_exec_kind(node.kind) || is_complement_exec_kind(node.kind)) {
        throw std::logic_error("Arity-2 transitions are available only for non-complement arity-2 nodes.");
    }

    const uint64_t key = state_cache_key(node_id, state);
    const auto it = arity2_visible_transition_cache.find(key);
    if (it != arity2_visible_transition_cache.end()) {
        return it->second.transitions;
    }

    Arity2TransitionBuilder transitions{};

    switch (node.kind) {
        case ExecKind::Arity2LeafNft:
            build_leaf_arity2_nft_transitions(node_id, nfts[node.lhs], static_cast<State>(state), 0, 0, 0, transitions);
            break;

        case ExecKind::Arity2Union: {
            const TaggedState tagged = macro_store.get_tagged(node_id, state);
            const NodeId child_id = tagged.tag == TaggedState::Tag::Left ? node.lhs : node.rhs;
            Arity2TransitionMap child_fallback{};
            const Arity2TransitionMap& child_transitions =
                    resolver.resolve_arity2_visible(child_id, tagged.state, child_fallback);
            transitions.reserve(child_transitions.size());
            append_union_transitions(macro_store, node_id, tagged, child_transitions, transitions);
            break;
        }

        case ExecKind::Arity2Intersect: {
            const PairState pair = macro_store.get_pair(node_id, state);
            Arity2TransitionMap lhs_fallback{};
            Arity2TransitionMap rhs_fallback{};
            const Arity2TransitionMap& lhs_transitions =
                    resolver.resolve_arity2_visible(node.lhs, pair.lhs, lhs_fallback);
            if (lhs_transitions.empty()) {
                break;
            }

            const Arity2TransitionMap& rhs_transitions =
                    resolver.resolve_arity2_visible(node.rhs, pair.rhs, rhs_fallback);
            const bool needs_wildcard_matching =
                    arity2_transition_map_has_special_symbol(
                            node.lhs, pair.lhs, lhs_transitions, mata::nft::DONT_CARE) ||
                    arity2_transition_map_has_special_symbol(node.rhs, pair.rhs, rhs_transitions, mata::nft::DONT_CARE);

            if (!needs_wildcard_matching) {
                transitions.reserve(std::min(lhs_transitions.size(), rhs_transitions.size()));
                append_exact_intersection_transitions(
                        macro_store, node_id, lhs_transitions, rhs_transitions, transitions);
                break;
            }

            std::unordered_map<Arity2TransitionKey, std::vector<const Arity2TransitionMap::value_type*>> exact_index{};
            std::vector<const Arity2TransitionMap::value_type*> wildcard_entries{};
            split_transition_entries(
                    rhs_transitions, exact_index, wildcard_entries,
                    [](const Arity2TransitionMap::value_type& entry) { return entry.first; },
                    [&](const Arity2TransitionMap::value_type& entry) {
                        return is_resolved_dont_care(node.rhs, 0, arity2_first_symbol(entry.first)) ||
                               is_resolved_dont_care(node.rhs, 1, arity2_second_symbol(entry.first));
                    });

            for (const auto& [lhs_tuple, lhs_states] : lhs_transitions) {
                const auto append_matches = [&](const Arity2TransitionMap::value_type& rhs_entry) {
                    Arity2TransitionKey merged_tuple = 0;
                    if (!try_merge_arity2_keys(node.lhs, lhs_tuple, node.rhs, rhs_entry.first, merged_tuple)) {
                        return;
                    }

                    auto& bucket = transitions[merged_tuple];
                    append_pair_product_states(macro_store, node_id, lhs_states, rhs_entry.second, bucket);
                };

                const bool lhs_has_dont_care = is_resolved_dont_care(node.lhs, 0, arity2_first_symbol(lhs_tuple)) ||
                                               is_resolved_dont_care(node.lhs, 1, arity2_second_symbol(lhs_tuple));
                append_matching_entries(
                        lhs_tuple, lhs_has_dont_care, exact_index, wildcard_entries, rhs_transitions, append_matches);
            }
            break;
        }

        case ExecKind::Identity: {
            Arity1TransitionMap child_fallback{};
            const Arity1TransitionMap& child_transitions =
                    resolver.resolve_arity1_visible(node.lhs, state, child_fallback);
            transitions.reserve(child_transitions.size());
            for (const auto& [symbol, child_states] : child_transitions) {
                auto& bucket = transitions[pack_arity2_symbols(symbol, symbol)];
                for (const GeneratedMacroState& child_state : child_states) {
                    append_generated_state(bucket, child_state);
                }
            }
            break;
        }

        case ExecKind::Arity2Project: {
            const ProjectPlan& plan = project_plans[node.payload];
            TransitionMap child_fallback{};
            const TransitionMap& child_transitions = resolver.resolve_visible(node.lhs, state, child_fallback);
            transitions.reserve(child_transitions.size());
            for (const auto& [child_tuple, child_states] : child_transitions) {
                auto& bucket = transitions[pack_arity2_symbols(
                        child_tuple[plan.kept_levels[0]], child_tuple[plan.kept_levels[1]])];
                for (const GeneratedMacroState& child_state : child_states) {
                    append_generated_state(bucket, child_state);
                }
            }
            break;
        }

        case ExecKind::Arity2SyncProduct: {
            const PairState pair = macro_store.get_pair(node_id, state);
            const CompiledSyncPlan& plan = sync_plans[node.payload];
            TransitionMap lhs_fallback{};
            TransitionMap rhs_fallback{};
            const TransitionMap& lhs_transitions = resolver.resolve_visible(node.lhs, pair.lhs, lhs_fallback);
            if (lhs_transitions.empty()) {
                break;
            }

            const TransitionMap& rhs_transitions = resolver.resolve_visible(node.rhs, pair.rhs, rhs_fallback);
            const bool needs_wildcard_matching =
                    transition_map_has_special_symbol_on_levels(
                            node.lhs, pair.lhs, lhs_transitions, plan.lhs_sync_levels, mata::nft::DONT_CARE) ||
                    transition_map_has_special_symbol_on_levels(
                            node.rhs, pair.rhs, rhs_transitions, plan.rhs_sync_levels, mata::nft::DONT_CARE);

            SymbolTuple result_tuple{};
            const auto append_matches = [&](const TransitionMap::value_type& lhs_entry,
                                            const TransitionMap::value_type& rhs_entry) {
                if (!build_sync_result_tuple(
                            node.lhs, lhs_entry.first, node.rhs, rhs_entry.first, plan, result_tuple)) {
                    return;
                }

                auto& bucket = transitions[pack_arity2_tuple(result_tuple)];
                append_pair_product_states(macro_store, node_id, lhs_entry.second, rhs_entry.second, bucket);
            };

            if (!needs_wildcard_matching) {
                append_exact_sync_product_matches(
                        lhs_transitions, rhs_transitions, plan.lhs_sync_levels, plan.rhs_sync_levels, append_matches);
                break;
            }

            std::unordered_map<SymbolTuple, std::vector<const TransitionMap::value_type*>, SymbolTupleHash>
                    exact_index{};
            std::vector<const TransitionMap::value_type*> wildcard_entries{};
            split_transition_entries(
                    rhs_transitions, exact_index, wildcard_entries,
                    [&](const TransitionMap::value_type& entry) {
                        return extract_sync_levels(entry.first, plan.rhs_sync_levels);
                    },
                    [&](const TransitionMap::value_type& entry) {
                        return tuple_has_special_symbol_on_levels(
                                node.rhs, entry.first, plan.rhs_sync_levels, mata::nft::DONT_CARE);
                    });

            for (const auto& lhs_entry : lhs_transitions) {
                const bool lhs_has_dont_care = tuple_has_special_symbol_on_levels(
                        node.lhs, lhs_entry.first, plan.lhs_sync_levels, mata::nft::DONT_CARE);
                const SymbolTuple sync_signature = extract_sync_levels(lhs_entry.first, plan.lhs_sync_levels);
                append_matching_entries(
                        sync_signature, lhs_has_dont_care, exact_index, wildcard_entries, rhs_transitions,
                        [&](const TransitionMap::value_type& rhs_entry) {
                            if (!sync_levels_match(
                                        node.lhs, lhs_entry.first, plan.lhs_sync_levels, node.rhs, rhs_entry.first,
                                        plan.rhs_sync_levels)) {
                                return;
                            }
                            append_matches(lhs_entry, rhs_entry);
                        });
            }
            break;
        }

        default:
            break;
    }

    return arity2_visible_transition_cache.emplace(key, build_arity2_cache_entry(node_id, std::move(transitions)))
            .first->second.transitions;
}


// Generic visible transitions.

const TransitionMap& TransitionCache::get_visible_transitions(
        const NodeId node_id, const MacroStateId state, const TransitionResolver& resolver) {
    initialize_special_symbol_cache();
    const ExecNode& node = nodes[node_id];
    if (is_complement_exec_kind(node.kind)) {
        throw std::logic_error("Complement does not expose a finite enabled-transition cache.");
    }

    const uint64_t key = state_cache_key(node_id, state);
    const auto it = visible_transition_cache.find(key);
    if (it != visible_transition_cache.end()) {
        return it->second.transitions;
    }

    TransitionMap transitions{};

    switch (node.kind) {
        case ExecKind::LeafNfa:
            append_leaf_nfa_transitions(
                    nfas, nodes, alphabets, node_id, state, transitions,
                    [](const mata::Symbol symbol) { return SymbolTuple{symbol}; });
            break;

        case ExecKind::LeafNft:
        case ExecKind::Arity2LeafNft: {
            SymbolTuple tuple(node.result_arity, 0);
            build_leaf_nft_transitions(node_id, nfts[node.lhs], static_cast<State>(state), tuple, 0, transitions);
            break;
        }

        case ExecKind::Union:
        case ExecKind::Arity1Union:
        case ExecKind::Arity2Union: {
            const TaggedState tagged = macro_store.get_tagged(node_id, state);
            const NodeId child_id = tagged.tag == TaggedState::Tag::Left ? node.lhs : node.rhs;
            TransitionMap child_fallback{};
            const TransitionMap& child_transitions = resolver.resolve_visible(child_id, tagged.state, child_fallback);
            append_union_transitions(macro_store, node_id, tagged, child_transitions, transitions);
            break;
        }

        case ExecKind::Intersect:
        case ExecKind::Arity1Intersect:
        case ExecKind::Arity2Intersect: {
            const PairState pair = macro_store.get_pair(node_id, state);
            TransitionMap lhs_fallback{};
            TransitionMap rhs_fallback{};
            const TransitionMap& lhs_transitions = resolver.resolve_visible(node.lhs, pair.lhs, lhs_fallback);
            if (lhs_transitions.empty()) {
                break;
            }

            const TransitionMap& rhs_transitions = resolver.resolve_visible(node.rhs, pair.rhs, rhs_fallback);
            const bool needs_wildcard_matching =
                    transition_map_has_special_symbol(node.lhs, pair.lhs, lhs_transitions, mata::nft::DONT_CARE) ||
                    transition_map_has_special_symbol(node.rhs, pair.rhs, rhs_transitions, mata::nft::DONT_CARE);

            if (!needs_wildcard_matching) {
                append_exact_intersection_transitions(
                        macro_store, node_id, lhs_transitions, rhs_transitions, transitions);
                break;
            }

            std::unordered_map<SymbolTuple, std::vector<const TransitionMap::value_type*>, SymbolTupleHash>
                    exact_index{};
            std::vector<const TransitionMap::value_type*> wildcard_entries{};
            split_transition_entries(
                    rhs_transitions, exact_index, wildcard_entries,
                    [](const TransitionMap::value_type& entry) -> const SymbolTuple& { return entry.first; },
                    [&](const TransitionMap::value_type& entry) {
                        return tuple_has_special_symbol(node.rhs, entry.first, mata::nft::DONT_CARE);
                    });

            SymbolTuple merged_tuple{};
            for (const auto& [lhs_tuple, lhs_states] : lhs_transitions) {
                const auto append_matches = [&](const TransitionMap::value_type& rhs_entry) {
                    if (!try_merge_tuples(node.lhs, lhs_tuple, node.rhs, rhs_entry.first, merged_tuple)) {
                        return;
                    }

                    auto& bucket = transitions[merged_tuple];
                    append_pair_product_states(macro_store, node_id, lhs_states, rhs_entry.second, bucket);
                };

                append_matching_entries(
                        lhs_tuple, tuple_has_special_symbol(node.lhs, lhs_tuple, mata::nft::DONT_CARE), exact_index,
                        wildcard_entries, rhs_transitions, append_matches);
            }
            break;
        }

        case ExecKind::Identity: {
            TransitionMap child_fallback{};
            const TransitionMap& child_transitions = resolver.resolve_visible(node.lhs, state, child_fallback);
            for (const auto& [tuple, child_states] : child_transitions) {
                SymbolTuple diagonal{tuple[0], tuple[0]};
                auto& bucket = transitions[diagonal];
                for (const GeneratedMacroState& child_state : child_states) {
                    append_generated_state(bucket, child_state);
                }
            }
            break;
        }

        case ExecKind::Project:
        case ExecKind::Arity2Project: {
            const ProjectPlan& plan = project_plans[node.payload];
            TransitionMap child_fallback{};
            const TransitionMap& child_transitions = resolver.resolve_visible(node.lhs, state, child_fallback);
            for (const auto& [child_tuple, child_states] : child_transitions) {
                SymbolTuple projected{};
                projected.reserve(plan.kept_levels.size());
                for (const uint8_t level : plan.kept_levels) {
                    projected.push_back(child_tuple[level]);
                }

                auto& bucket = transitions[projected];
                for (const GeneratedMacroState& child_state : child_states) {
                    append_generated_state(bucket, child_state);
                }
            }
            break;
        }

        case ExecKind::SyncProduct:
        case ExecKind::Arity2SyncProduct: {
            const PairState pair = macro_store.get_pair(node_id, state);
            const CompiledSyncPlan& plan = sync_plans[node.payload];
            TransitionMap lhs_fallback{};
            TransitionMap rhs_fallback{};
            const TransitionMap& lhs_transitions = resolver.resolve_visible(node.lhs, pair.lhs, lhs_fallback);
            if (lhs_transitions.empty()) {
                break;
            }

            const TransitionMap& rhs_transitions = resolver.resolve_visible(node.rhs, pair.rhs, rhs_fallback);
            const bool needs_wildcard_matching =
                    transition_map_has_special_symbol_on_levels(
                            node.lhs, pair.lhs, lhs_transitions, plan.lhs_sync_levels, mata::nft::DONT_CARE) ||
                    transition_map_has_special_symbol_on_levels(
                            node.rhs, pair.rhs, rhs_transitions, plan.rhs_sync_levels, mata::nft::DONT_CARE);

            SymbolTuple result_tuple{};
            const auto append_match = [&](const TransitionMap::value_type& lhs_entry,
                                          const TransitionMap::value_type& rhs_entry) {
                if (!build_sync_result_tuple(
                            node.lhs, lhs_entry.first, node.rhs, rhs_entry.first, plan, result_tuple)) {
                    return;
                }

                auto& bucket = transitions[result_tuple];
                append_pair_product_states(macro_store, node_id, lhs_entry.second, rhs_entry.second, bucket);
            };

            if (!needs_wildcard_matching) {
                append_exact_sync_product_matches(
                        lhs_transitions, rhs_transitions, plan.lhs_sync_levels, plan.rhs_sync_levels, append_match);
                break;
            }

            std::unordered_map<SymbolTuple, std::vector<const TransitionMap::value_type*>, SymbolTupleHash>
                    exact_index{};
            std::vector<const TransitionMap::value_type*> wildcard_entries{};
            split_transition_entries(
                    rhs_transitions, exact_index, wildcard_entries,
                    [&](const TransitionMap::value_type& entry) {
                        return extract_sync_levels(entry.first, plan.rhs_sync_levels);
                    },
                    [&](const TransitionMap::value_type& entry) {
                        return tuple_has_special_symbol_on_levels(
                                node.rhs, entry.first, plan.rhs_sync_levels, mata::nft::DONT_CARE);
                    });

            for (const auto& lhs_entry : lhs_transitions) {
                const bool lhs_has_dont_care = tuple_has_special_symbol_on_levels(
                        node.lhs, lhs_entry.first, plan.lhs_sync_levels, mata::nft::DONT_CARE);
                const SymbolTuple sync_signature = extract_sync_levels(lhs_entry.first, plan.lhs_sync_levels);
                append_matching_entries(
                        sync_signature, lhs_has_dont_care, exact_index, wildcard_entries, rhs_transitions,
                        [&](const TransitionMap::value_type& rhs_entry) {
                            if (!sync_levels_match(
                                        node.lhs, lhs_entry.first, plan.lhs_sync_levels, node.rhs, rhs_entry.first,
                                        plan.rhs_sync_levels)) {
                                return;
                            }
                            append_match(lhs_entry, rhs_entry);
                        });
            }
            break;
        }

        case ExecKind::Complement:
        case ExecKind::Arity1Complement:
        case ExecKind::Arity2Complement:
            break;
    }

    return visible_transition_cache.emplace(key, build_generic_cache_entry(node_id, std::move(transitions)))
            .first->second.transitions;
}


// Generic helpers shared by the generic path and arity-specialized fast paths.

void TransitionCache::initialize_special_symbol_cache() {
    if (special_symbols_by_level.size() == nodes.size()) {
        return;
    }

    special_symbols_by_level.clear();
    special_symbols_by_level.resize(nodes.size());
    for (NodeId node_id = 0; node_id < nodes.size(); ++node_id) {
        special_symbols_by_level[node_id].resize(nodes[node_id].result_arity);
        for (uint8_t level = 0; level < nodes[node_id].result_arity; ++level) {
            ResolvedSpecialSymbols& resolved = special_symbols_by_level[node_id][level];
            if (const std::optional<mata::Symbol> epsilon =
                        resolve_special_symbol_id(node_id, level, mata::nft::EPSILON)) {
                resolved.epsilon = *epsilon;
                resolved.flags |= ResolvedSpecialSymbols::HasEpsilon;
            }
            if (const std::optional<mata::Symbol> dont_care =
                        resolve_special_symbol_id(node_id, level, mata::nft::DONT_CARE)) {
                resolved.dont_care = *dont_care;
                resolved.flags |= ResolvedSpecialSymbols::HasDontCare;
            }
        }
    }
}

std::optional<mata::Symbol> TransitionCache::resolve_special_symbol_id(
        const NodeId node_id, const uint8_t level, const mata::Symbol special_symbol) const {
    const mata::Alphabet& alphabet = alphabets.level_alphabet(node_id, level);
    if (dynamic_cast<const mata::IntAlphabet*>(&alphabet) != nullptr) {
        return special_symbol;
    }

    try {
        for (const mata::Symbol symbol : alphabet.get_alphabet_symbols()) {
            if (alphabet.reverse_translate_symbol(symbol) == std::to_string(special_symbol)) {
                return symbol;
            }
        }
    } catch (const std::runtime_error&) {}

    return std::nullopt;
}

bool TransitionCache::is_resolved_epsilon(
        const NodeId node_id, const uint8_t level, const mata::Symbol resolved_symbol) const {
    const ResolvedSpecialSymbols& resolved = special_symbols_by_level[node_id][level];
    return (resolved.flags & ResolvedSpecialSymbols::HasEpsilon) != 0 && resolved_symbol == resolved.epsilon;
}

bool TransitionCache::is_resolved_dont_care(
        const NodeId node_id, const uint8_t level, const mata::Symbol resolved_symbol) const {
    const ResolvedSpecialSymbols& resolved = special_symbols_by_level[node_id][level];
    return (resolved.flags & ResolvedSpecialSymbols::HasDontCare) != 0 && resolved_symbol == resolved.dont_care;
}

std::vector<uint8_t> TransitionCache::collect_special_symbol_levels(
        const NodeId node_id, const TransitionMap& transitions, const mata::Symbol special_symbol) const {
    std::vector<uint8_t> levels(nodes[node_id].result_arity, 0);
    for (const auto& [tuple, _] : transitions) {
        for (size_t level = 0; level < tuple.size(); ++level) {
            if (!levels[level] &&
                is_resolved_special_symbol(node_id, static_cast<uint8_t>(level), tuple[level], special_symbol)) {
                levels[level] = 1;
            }
        }
    }
    return levels;
}

Arity2TransitionCacheEntry
TransitionCache::build_arity2_cache_entry(const NodeId node_id, Arity2TransitionBuilder&& transitions) const {
    Arity2TransitionCacheEntry entry{};
    entry.transitions = Arity2TransitionMap::freeze(std::move(transitions));
    for (const auto& [tuple, _] : entry.transitions) {
        entry.first_has_epsilon =
                entry.first_has_epsilon || is_resolved_epsilon(node_id, 0, arity2_first_symbol(tuple));
        entry.second_has_epsilon =
                entry.second_has_epsilon || is_resolved_epsilon(node_id, 1, arity2_second_symbol(tuple));
        entry.first_has_dont_care =
                entry.first_has_dont_care || is_resolved_dont_care(node_id, 0, arity2_first_symbol(tuple));
        entry.second_has_dont_care =
                entry.second_has_dont_care || is_resolved_dont_care(node_id, 1, arity2_second_symbol(tuple));
    }
    return entry;
}

GenericTransitionCacheEntry
TransitionCache::build_generic_cache_entry(const NodeId node_id, TransitionMap&& transitions) const {
    GenericTransitionCacheEntry entry{};
    entry.has_epsilon_by_level = collect_special_symbol_levels(node_id, transitions, mata::nft::EPSILON);
    entry.has_dont_care_by_level = collect_special_symbol_levels(node_id, transitions, mata::nft::DONT_CARE);
    entry.transitions = std::move(transitions);
    return entry;
}

bool TransitionCache::is_resolved_special_symbol(
        const NodeId node_id, const uint8_t level, const mata::Symbol resolved_symbol,
        const mata::Symbol special_symbol) const {
    return special_symbol == mata::nft::EPSILON ? is_resolved_epsilon(node_id, level, resolved_symbol)
                                                : is_resolved_dont_care(node_id, level, resolved_symbol);
}

bool TransitionCache::tuple_has_special_symbol_on_levels(
        const NodeId node_id, const SymbolTuple& tuple, const std::vector<uint8_t>& levels,
        const mata::Symbol special_symbol) const {
    for (const uint8_t level : levels) {
        if (is_resolved_special_symbol(node_id, level, tuple[level], special_symbol)) {
            return true;
        }
    }
    return false;
}

bool TransitionCache::tuple_has_special_symbol(
        const NodeId node_id, const SymbolTuple& tuple, const mata::Symbol special_symbol) const {
    for (size_t level = 0; level < tuple.size(); ++level) {
        if (is_resolved_special_symbol(node_id, static_cast<uint8_t>(level), tuple[level], special_symbol)) {
            return true;
        }
    }
    return false;
}

bool TransitionCache::transition_map_has_special_symbol_on_levels(
        const NodeId node_id, const MacroStateId state, const TransitionMap& transitions,
        const std::vector<uint8_t>& levels, const mata::Symbol special_symbol) const {
    if (!is_complement_exec_kind(nodes[node_id].kind)) {
        const auto it = visible_transition_cache.find(state_cache_key(node_id, state));
        if (it != visible_transition_cache.end()) {
            const std::vector<uint8_t>& cached_levels = cached_special_symbol_levels(it->second, special_symbol);
            for (const uint8_t level : levels) {
                if (level < cached_levels.size() && cached_levels[level]) {
                    return true;
                }
            }
            return false;
        }
    }

    for (const auto& [tuple, _] : transitions) {
        if (tuple_has_special_symbol_on_levels(node_id, tuple, levels, special_symbol)) {
            return true;
        }
    }
    return false;
}

bool TransitionCache::transition_map_has_special_symbol(
        const NodeId node_id, const MacroStateId state, const TransitionMap& transitions,
        const mata::Symbol special_symbol) const {
    if (!is_complement_exec_kind(nodes[node_id].kind)) {
        const auto it = visible_transition_cache.find(state_cache_key(node_id, state));
        if (it != visible_transition_cache.end()) {
            const std::vector<uint8_t>& cached_levels = cached_special_symbol_levels(it->second, special_symbol);
            for (const uint8_t has_special_symbol : cached_levels) {
                if (has_special_symbol) {
                    return true;
                }
            }
            return false;
        }
    }

    for (const auto& [tuple, _] : transitions) {
        if (tuple_has_special_symbol(node_id, tuple, special_symbol)) {
            return true;
        }
    }
    return false;
}

bool TransitionCache::arity2_transition_map_has_special_symbol(
        const NodeId node_id, const MacroStateId state, const Arity2TransitionMap& transitions,
        const mata::Symbol special_symbol) const {
    if (!is_complement_exec_kind(nodes[node_id].kind)) {
        const auto it = arity2_visible_transition_cache.find(state_cache_key(node_id, state));
        if (it != arity2_visible_transition_cache.end()) {
            return cached_arity2_has_special_symbol(it->second, special_symbol);
        }
    }

    for (const auto& [tuple, _] : transitions) {
        if (is_resolved_special_symbol(node_id, 0, arity2_first_symbol(tuple), special_symbol) ||
            is_resolved_special_symbol(node_id, 1, arity2_second_symbol(tuple), special_symbol)) {
            return true;
        }
    }
    return false;
}

bool TransitionCache::try_merge_symbols(
        const NodeId lhs_node_id, const uint8_t lhs_level, const mata::Symbol lhs_symbol, const NodeId rhs_node_id,
        const uint8_t rhs_level, const mata::Symbol rhs_symbol, mata::Symbol& merged_symbol) const {
    const bool lhs_is_epsilon = is_resolved_special_symbol(lhs_node_id, lhs_level, lhs_symbol, mata::nft::EPSILON);
    const bool rhs_is_epsilon = is_resolved_special_symbol(rhs_node_id, rhs_level, rhs_symbol, mata::nft::EPSILON);
    if (lhs_is_epsilon || rhs_is_epsilon) {
        if (!(lhs_is_epsilon && rhs_is_epsilon)) {
            return false;
        }
        merged_symbol = lhs_symbol;
        return true;
    }

    const bool lhs_is_dont_care = is_resolved_special_symbol(lhs_node_id, lhs_level, lhs_symbol, mata::nft::DONT_CARE);
    const bool rhs_is_dont_care = is_resolved_special_symbol(rhs_node_id, rhs_level, rhs_symbol, mata::nft::DONT_CARE);
    if (!(lhs_symbol == rhs_symbol || lhs_is_dont_care || rhs_is_dont_care)) {
        return false;
    }

    merged_symbol = lhs_is_dont_care ? rhs_symbol : lhs_symbol;
    return true;
}

bool TransitionCache::try_merge_tuples(
        const NodeId lhs_node_id, const SymbolTuple& lhs_tuple, const NodeId rhs_node_id, const SymbolTuple& rhs_tuple,
        SymbolTuple& merged_tuple) const {
    if (lhs_tuple.size() != rhs_tuple.size()) {
        return false;
    }

    merged_tuple.clear();
    merged_tuple.reserve(lhs_tuple.size());
    for (size_t i = 0; i < lhs_tuple.size(); ++i) {
        mata::Symbol merged_symbol = 0;
        if (!try_merge_symbols(
                    lhs_node_id, static_cast<uint8_t>(i), lhs_tuple[i], rhs_node_id, static_cast<uint8_t>(i),
                    rhs_tuple[i], merged_symbol)) {
            return false;
        }
        merged_tuple.push_back(merged_symbol);
    }

    return true;
}

bool TransitionCache::try_merge_arity2_keys(
        const NodeId lhs_node_id, const Arity2TransitionKey lhs_tuple, const NodeId rhs_node_id,
        const Arity2TransitionKey rhs_tuple, Arity2TransitionKey& merged_tuple) const {
    mata::Symbol merged_first = 0;
    if (!try_merge_symbols(
                lhs_node_id, 0, arity2_first_symbol(lhs_tuple), rhs_node_id, 0, arity2_first_symbol(rhs_tuple),
                merged_first)) {
        return false;
    }

    mata::Symbol merged_second = 0;
    if (!try_merge_symbols(
                lhs_node_id, 1, arity2_second_symbol(lhs_tuple), rhs_node_id, 1, arity2_second_symbol(rhs_tuple),
                merged_second)) {
        return false;
    }

    merged_tuple = pack_arity2_symbols(merged_first, merged_second);
    return true;
}

bool TransitionCache::sync_levels_match(
        const NodeId lhs_node_id, const SymbolTuple& lhs_tuple, const std::vector<uint8_t>& lhs_levels,
        const NodeId rhs_node_id, const SymbolTuple& rhs_tuple, const std::vector<uint8_t>& rhs_levels) const {
    for (size_t i = 0; i < lhs_levels.size(); ++i) {
        mata::Symbol merged_symbol = 0;
        if (!try_merge_symbols(
                    lhs_node_id, lhs_levels[i], lhs_tuple[lhs_levels[i]], rhs_node_id, rhs_levels[i],
                    rhs_tuple[rhs_levels[i]], merged_symbol)) {
            return false;
        }
    }

    return true;
}

bool TransitionCache::build_sync_result_tuple(
        const NodeId lhs_node_id, const SymbolTuple& lhs_tuple, const NodeId rhs_node_id, const SymbolTuple& rhs_tuple,
        const CompiledSyncPlan& plan, SymbolTuple& result_tuple) const {
    result_tuple.clear();
    result_tuple.reserve(plan.result_layout.size());

    for (const LevelRef ref : plan.result_layout) {
        mata::Symbol symbol = ref.side == LevelRef::Side::Lhs ? lhs_tuple[ref.level] : rhs_tuple[ref.level];
        const std::vector<int16_t>& peer_by_level =
                ref.side == LevelRef::Side::Lhs ? plan.lhs_sync_peer_by_level : plan.rhs_sync_peer_by_level;
        if (ref.level < peer_by_level.size() && peer_by_level[ref.level] >= 0) {
            const size_t sync_peer = static_cast<size_t>(peer_by_level[ref.level]);
            const mata::Symbol peer_symbol = ref.side == LevelRef::Side::Lhs
                                                     ? rhs_tuple[plan.rhs_sync_levels[sync_peer]]
                                                     : lhs_tuple[plan.lhs_sync_levels[sync_peer]];
            if (!try_merge_symbols(
                        ref.side == LevelRef::Side::Lhs ? lhs_node_id : rhs_node_id, ref.level, symbol,
                        ref.side == LevelRef::Side::Lhs ? rhs_node_id : lhs_node_id,
                        ref.side == LevelRef::Side::Lhs ? plan.rhs_sync_levels[sync_peer]
                                                        : plan.lhs_sync_levels[sync_peer],
                        peer_symbol, symbol)) {
                return false;
            }
        }

        result_tuple.push_back(symbol);
    }

    return true;
}


// Generic leaf NFT transition enumeration.

void TransitionCache::build_leaf_nft_transitions(
        const NodeId node_id, const Nft& nft, const State source_state, SymbolTuple& current_tuple,
        const size_t next_level, TransitionMap& transitions) {
    if (next_level == current_tuple.size()) {
        append_generated_state(
                transitions[current_tuple],
                GeneratedMacroState{static_cast<MacroStateId>(source_state), nft.final.contains(source_state)});
        return;
    }

    for (const auto& symbol_post : nft.delta.state_post(source_state)) {
        mata::Symbol resolved_symbol = 0;
        if (!alphabets.try_translate_local_symbol_to_resolved(
                    nft, static_cast<uint8_t>(next_level), node_id, static_cast<uint8_t>(next_level),
                    symbol_post.symbol, resolved_symbol)) {
            continue;
        }

        current_tuple[next_level] = resolved_symbol;
        for (const State target_state : symbol_post.targets) {
            build_leaf_nft_transitions(node_id, nft, target_state, current_tuple, next_level + 1, transitions);
        }
    }
}


// Fast path: arity-2 leaf NFT transition enumeration.

void TransitionCache::build_leaf_arity2_nft_transitions(
        const NodeId node_id, const Nft& nft, const State source_state, const mata::Symbol first_symbol,
        const mata::Symbol second_symbol, const size_t next_level, Arity2TransitionBuilder& transitions) {
    if (next_level == 2) {
        append_generated_state(
                transitions[pack_arity2_symbols(first_symbol, second_symbol)],
                GeneratedMacroState{static_cast<MacroStateId>(source_state), nft.final.contains(source_state)});
        return;
    }

    for (const auto& symbol_post : nft.delta.state_post(source_state)) {
        mata::Symbol resolved_symbol = 0;
        if (!alphabets.try_translate_local_symbol_to_resolved(
                    nft, static_cast<uint8_t>(next_level), node_id, static_cast<uint8_t>(next_level),
                    symbol_post.symbol, resolved_symbol)) {
            continue;
        }

        const mata::Symbol next_first_symbol = next_level == 0 ? resolved_symbol : first_symbol;
        const mata::Symbol next_second_symbol = next_level == 1 ? resolved_symbol : second_symbol;
        for (const State target_state : symbol_post.targets) {
            build_leaf_arity2_nft_transitions(
                    node_id, nft, target_state, next_first_symbol, next_second_symbol, next_level + 1, transitions);
        }
    }
}

} // namespace mata::nft::lazy::detail
