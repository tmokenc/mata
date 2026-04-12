/**
 * @file emptiness.cc
 * @brief Private execution engine for mata::nft::lazy::detail.
 */

#include "emptiness.hh"

#include "macrostate_store.hh"
#include "reconstruction.hh"
#include "subsumption.hh"
#include "symbols.hh"
#include "transition_cache.hh"

#include <mata/simlib/explicit_lts.hh>

#include "mata/nfa/algorithms.hh"
#include "mata/nft/algorithms.hh"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mata::nft::lazy::detail {

namespace {
    struct Context {
        using Nfa = mata::nfa::Nfa;
        using Nft = mata::nft::Nft;
        using State = mata::nfa::State;

        using MacroStateVisitor = std::function<bool(const GeneratedMacroState&)>;

        const std::vector<Nfa>& nfas;
        const std::vector<Nft>& nfts;
        const std::vector<SyncPlan>& sync_plans;
        const std::vector<ProjectPlan>& project_plans;

        std::vector<ExecNode> nodes;
        MacroStateStore macro_store;
        std::vector<std::vector<mata::OnTheFlyAlphabet>> level_alphabets;
        SubsumptionEngine subsumption;
        TransitionCache transition_cache;
        NodeId root_id;

        Context(const SymbolicAutomataTree& tree, NodeId root,
                const std::vector<mata::OnTheFlyAlphabet>* root_level_alphabets = nullptr)
            : nfas(tree.nfas), nfts(tree.nfts), sync_plans(tree.sync_plans), project_plans(tree.project_plans), nodes{},
              macro_store{}, level_alphabets{}, subsumption{SubsumptionContext{nfas, nfts, nodes, macro_store}},
              transition_cache{TransitionCacheContext{
                      nfas, nfts, sync_plans, project_plans, nodes, macro_store, level_alphabets}},
              root_id{0} {

            root_id = reconstruct_nodes(tree, root, nodes);
            macro_store = MacroStateStore(nodes, nfas, nfts);
            level_alphabets.resize(nodes.size());

            std::vector<bool> visited(nodes.size(), false);
            resolve_metadata(root_id, visited);

            if (root_level_alphabets != nullptr && root_level_alphabets->size() != nodes[root_id].result_arity) {
                throw std::invalid_argument("The number of root level alphabets must match the root arity");
            }

            canonicalize_level_alphabets(root_level_alphabets);
        }

        bool is_arity1_exec(const NodeId node_id) const noexcept {
            return is_arity1_exec_kind(nodes[node_id].kind);
        }

        // Resolve per-node alphabets and leaf simulation relations bottom-up.
        void resolve_metadata(const NodeId node_id, std::vector<bool>& visited) {
            if (visited[node_id]) {
                return;
            }

            const ExecNode& node = nodes[node_id];
            level_alphabets[node_id].resize(node.result_arity);

            switch (node.kind) {
                case ExecKind::LeafNfa: {
                    fill_resolved_leaf_alphabet(nfas[node.lhs], level_alphabets[node_id][0]);
                    subsumption.set_nfa_simulation(node.lhs, mata::nfa::algorithms::compute_relation(nfas[node.lhs]));
                    break;
                }

                case ExecKind::LeafNft:
                case ExecKind::Arity2LeafNft: {
                    fill_resolved_leaf_level_alphabets(nfts[node.lhs], level_alphabets[node_id]);
                    subsumption.set_nft_simulation(node.lhs, mata::nft::algorithms::compute_relation(nfts[node.lhs]));
                    break;
                }

                case ExecKind::Union:
                case ExecKind::Intersect:
                case ExecKind::Arity1Union:
                case ExecKind::Arity1Intersect:
                case ExecKind::Arity2Union:
                case ExecKind::Arity2Intersect: {
                    resolve_metadata(node.lhs, visited);
                    resolve_metadata(node.rhs, visited);
                    break;
                }

                case ExecKind::Complement:
                case ExecKind::Arity1Complement:
                case ExecKind::Arity2Complement: {
                    resolve_metadata(node.lhs, visited);
                    break;
                }

                case ExecKind::Identity: {
                    resolve_metadata(node.lhs, visited);
                    break;
                }

                case ExecKind::Project:
                case ExecKind::Arity2Project: {
                    resolve_metadata(node.lhs, visited);
                    break;
                }

                case ExecKind::SyncProduct:
                case ExecKind::Arity2SyncProduct: {
                    resolve_metadata(node.lhs, visited);
                    resolve_metadata(node.rhs, visited);
                    break;
                }
            }

            visited[node_id] = true;
        }

        // Canonicalize only levels that are semantically required to share symbols.
        void canonicalize_level_alphabets(const std::vector<mata::OnTheFlyAlphabet>* root_level_alphabets) {
            // Each node level starts with its own local alphabet. We then union together
            // only the levels that must denote the same visible symbols:
            // - corresponding levels of Union / Intersect
            // - child/result levels of Complement / Project
            // - synchronized levels of SyncProduct
            // - both output tapes of Identity
            //
            // This keeps unrelated tapes independent, which is important for true
            // per-tape alphabets.
            std::vector<size_t> level_offsets(nodes.size() + 1, 0);
            for (size_t node_id = 0; node_id < nodes.size(); ++node_id) {
                level_offsets[node_id + 1] = level_offsets[node_id] + nodes[node_id].result_arity;
            }

            const size_t total_levels = level_offsets.back();
            std::vector<size_t> parent(total_levels, 0);
            std::vector<uint8_t> rank(total_levels, 0);
            for (size_t i = 0; i < total_levels; ++i) {
                parent[i] = i;
            }

            const auto level_index = [&](const NodeId node_id, const uint8_t level) {
                return level_offsets[node_id] + level;
            };

            auto find_root = [&](size_t idx) {
                size_t root = idx;
                while (parent[root] != root) {
                    root = parent[root];
                }
                while (parent[idx] != idx) {
                    const size_t next = parent[idx];
                    parent[idx] = root;
                    idx = next;
                }
                return root;
            };

            auto unite = [&](const size_t lhs, const size_t rhs) {
                size_t lhs_root = find_root(lhs);
                size_t rhs_root = find_root(rhs);
                if (lhs_root == rhs_root) {
                    return;
                }

                if (rank[lhs_root] < rank[rhs_root]) {
                    std::swap(lhs_root, rhs_root);
                }

                parent[rhs_root] = lhs_root;
                if (rank[lhs_root] == rank[rhs_root]) {
                    ++rank[lhs_root];
                }
            };

            for (NodeId node_id = 0; node_id < nodes.size(); ++node_id) {
                const ExecNode& node = nodes[node_id];

                switch (node.kind) {
                    case ExecKind::LeafNfa:
                    case ExecKind::LeafNft:
                    case ExecKind::Arity2LeafNft:
                        break;

                    case ExecKind::Union:
                    case ExecKind::Intersect:
                    case ExecKind::Arity1Union:
                    case ExecKind::Arity1Intersect:
                    case ExecKind::Arity2Union:
                    case ExecKind::Arity2Intersect:
                        for (uint8_t level = 0; level < node.result_arity; ++level) {
                            unite(level_index(node_id, level), level_index(node.lhs, level));
                            unite(level_index(node_id, level), level_index(node.rhs, level));
                            unite(level_index(node.lhs, level), level_index(node.rhs, level));
                        }
                        break;

                    case ExecKind::Complement:
                    case ExecKind::Arity1Complement:
                    case ExecKind::Arity2Complement:
                        for (uint8_t level = 0; level < node.result_arity; ++level) {
                            unite(level_index(node_id, level), level_index(node.lhs, level));
                        }
                        break;

                    case ExecKind::Identity:
                        unite(level_index(node_id, 0), level_index(node.lhs, 0));
                        unite(level_index(node_id, 1), level_index(node.lhs, 0));
                        unite(level_index(node_id, 0), level_index(node_id, 1));
                        break;

                    case ExecKind::Project:
                    case ExecKind::Arity2Project: {
                        const ProjectPlan& plan = project_plans[node.payload];
                        for (uint8_t level = 0; level < node.result_arity; ++level) {
                            unite(level_index(node_id, level), level_index(node.lhs, plan.kept_levels[level]));
                        }
                        break;
                    }

                    case ExecKind::SyncProduct:
                    case ExecKind::Arity2SyncProduct: {
                        const SyncPlan& plan = sync_plans[node.payload];
                        for (size_t i = 0; i < plan.lhs_sync_levels.size(); ++i) {
                            unite(level_index(node.lhs, plan.lhs_sync_levels[i]),
                                  level_index(node.rhs, plan.rhs_sync_levels[i]));
                        }

                        for (uint8_t level = 0; level < node.result_arity; ++level) {
                            const LevelRef ref = plan.result_layout[level];
                            unite(level_index(node_id, level), ref.side == LevelRef::Side::Lhs
                                                                       ? level_index(node.lhs, ref.level)
                                                                       : level_index(node.rhs, ref.level));
                        }
                        break;
                    }
                }
            }

            std::vector<mata::OnTheFlyAlphabet> canonical_alphabets(total_levels);
            for (NodeId node_id = 0; node_id < nodes.size(); ++node_id) {
                for (uint8_t level = 0; level < nodes[node_id].result_arity; ++level) {
                    add_symbols_to_canonical(
                            level_alphabets[node_id][level],
                            canonical_alphabets[find_root(level_index(node_id, level))]);
                }
            }

            if (root_level_alphabets != nullptr) {
                for (uint8_t level = 0; level < nodes[root_id].result_arity; ++level) {
                    add_symbols_to_canonical(
                            (*root_level_alphabets)[level],
                            canonical_alphabets[find_root(level_index(root_id, level))]);
                }
            }

            for (NodeId node_id = 0; node_id < nodes.size(); ++node_id) {
                for (uint8_t level = 0; level < nodes[node_id].result_arity; ++level) {
                    level_alphabets[node_id][level] = canonical_alphabets[find_root(level_index(node_id, level))];
                }
            }
        }

        // Use the arity-1 path when possible, otherwise materialize a generic fallback map.
        const Arity1TransitionMap& get_arity1_transitions_with_fallback(
                const NodeId node_id, const MacroStateId state, Arity1TransitionMap& fallback) {
            if (is_complement_exec_kind(nodes[node_id].kind) || !is_arity1_exec(node_id)) {
                fallback = build_fallback_arity1_visible_transitions(node_id, state);
                return fallback;
            }

            return get_arity1_visible_transitions(node_id, state);
        }

        // Get cached visible transitions, falling back to on-demand materialization for complement.
        const TransitionMap&
        get_transitions_with_fallback(const NodeId node_id, const MacroStateId state, TransitionMap& fallback) {
            if (is_complement_exec_kind(nodes[node_id].kind)) {
                fallback = build_fallback_visible_transitions(node_id, state);
                return fallback;
            }

            return get_visible_transitions(node_id, state);
        }

        const Arity2TransitionMap&
        get_arity2_transitions_with_fallback(const NodeId node_id, const MacroStateId state, Arity2TransitionMap& fallback) {
            if (is_complement_exec_kind(nodes[node_id].kind) || !is_arity2_exec_kind(nodes[node_id].kind)) {
                fallback = build_fallback_arity2_visible_transitions(node_id, state);
                return fallback;
            }

            return get_arity2_visible_transitions(node_id, state);
        }

        struct NextStateIterator;
        using NextStateIteratorPtr = std::unique_ptr<NextStateIterator>;

        // Rust/Haskell-style iterator interface for successor generation,
        // as the cpp version looks kinda intimidating.
        // Allow for lazy exploration of complement states
        // without materializing the whole transition map at once.
        struct NextStateIterator {
            Context& ctx;

            explicit NextStateIterator(Context& context) : ctx{context} {}
            virtual ~NextStateIterator() = default;
            virtual std::optional<GeneratedMacroState> next() = 0;
        };

        // Simple iterator wrapper for already materialized successor sets.
        struct BufferedNextStateIterator final : NextStateIterator {
            std::vector<GeneratedMacroState> states;
            size_t index;

            BufferedNextStateIterator(Context& context, std::vector<GeneratedMacroState> generated_states)
                : NextStateIterator{context}, states{std::move(generated_states)}, index{0} {}

            std::optional<GeneratedMacroState> next() override {
                if (index >= states.size()) {
                    return std::nullopt;
                }

                return states[index++];
            }
        };

        // Iterator for complement states, which must query the child lazily over the whole universe.
        struct ComplementNextStateIterator final : NextStateIterator {
            const NodeId parent_id;
            const NodeId child_id;
            const SetState& sub_states;
            const SymbolTuple tuple;
            bool emitted;

            ComplementNextStateIterator(
                    Context& context, const NodeId node_id, const NodeId next_child_id, const SetState& child_states,
                    SymbolTuple transition_tuple)
                : NextStateIterator{context}, parent_id{node_id}, child_id{next_child_id}, sub_states{child_states},
                  tuple{std::move(transition_tuple)}, emitted{false} {}

            std::optional<GeneratedMacroState> next() override {
                if (emitted) {
                    return std::nullopt;
                }
                emitted = true;

                SetState next_sub_states{};
                next_sub_states.reserve(sub_states.size());
                bool accepting = true;

                for (const MacroStateId sub_state : sub_states) {
                    NextStateIteratorPtr child_iter = ctx.make_next_state_iterator(child_id, sub_state, tuple);
                    while (true) {
                        const std::optional<GeneratedMacroState> child_next_state = child_iter->next();
                        if (!child_next_state.has_value()) {
                            break;
                        }

                        next_sub_states.insert(child_next_state->id);
                        accepting = accepting && !child_next_state->accepting;
                    }
                }
                return GeneratedMacroState{ctx.macro_store.intern(parent_id, std::move(next_sub_states)), accepting};
            }
        };

        // Check whether a visible tuple satisfies the fixed coordinates of a partial tuple.
        static bool matches_partial_tuple(const SymbolTuple& tuple, const OptionalTuple& partial_tuple) {
            if (tuple.size() != partial_tuple.size()) {
                return false;
            }

            for (size_t i = 0; i < tuple.size(); ++i) {
                if (partial_tuple[i].has_value() && tuple[i] != *partial_tuple[i]) {
                    return false;
                }
            }

            return true;
        }

        // Enumerate the visible universe that matches one partially fixed tuple.
        template<typename Visitor>
        bool enumerate_partial_tuples(
                const NodeId node_id, const OptionalTuple& partial_tuple, SymbolTuple& current_tuple, size_t next_level,
                Visitor&& visitor) {
            if (next_level == partial_tuple.size()) {
                return visitor(current_tuple);
            }

            if (partial_tuple[next_level].has_value()) {
                current_tuple[next_level] = *partial_tuple[next_level];
                return enumerate_partial_tuples(node_id, partial_tuple, current_tuple, next_level + 1, visitor);
            }

            for (const mata::Symbol symbol : level_alphabets[node_id][next_level].get_alphabet_symbols()) {
                current_tuple[next_level] = symbol;
                if (!enumerate_partial_tuples(node_id, partial_tuple, current_tuple, next_level + 1, visitor)) {
                    return false;
                }
            }

            return true;
        }

        // Entry point for partial tuple enumeration.
        template<typename Visitor>
        bool enumerate_partial_tuples(const NodeId node_id, const OptionalTuple& partial_tuple, Visitor&& visitor) {
            SymbolTuple current_tuple(partial_tuple.size(), 0);
            return enumerate_partial_tuples(node_id, partial_tuple, current_tuple, 0, std::forward<Visitor>(visitor));
        }

        // Visit each enabled visible tuple together with its generated successors.
        template<typename Visitor>
        bool for_each_visible_transition(const NodeId node_id, const MacroStateId state, Visitor&& visitor) {
            if (!is_complement_exec_kind(nodes[node_id].kind)) {
                for (const auto& [tuple, states] : get_visible_transitions(node_id, state)) {
                    if (!visitor(tuple, states)) {
                        return false;
                    }
                }
                return true;
            }

            // Complement cannot expose a finite transition cache in general because its
            // enabled labels come from the whole visible universe, not only from child
            // labels. Materialize it on demand through the iterator path instead.
            return for_each_enabled_label_tuple(node_id, state, [&](const SymbolTuple& tuple) {
                std::vector<GeneratedMacroState> states{};
                bool enumerated_all =
                        for_each_next_macro_state(node_id, state, tuple, [&](const GeneratedMacroState& next_state) {
                            append_generated_state(states, next_state);
                            return true;
                        });
                if (!enumerated_all) {
                    return false;
                }
                if (states.empty()) {
                    return true;
                }
                return visitor(tuple, states);
            });
        }

        // Materialize a generic visible-transition map for nodes that do not expose one directly.
        TransitionMap build_fallback_visible_transitions(const NodeId node_id, const MacroStateId state) {
            TransitionMap transitions{};
            for_each_visible_transition(
                    node_id, state, [&](const SymbolTuple& tuple, const std::vector<GeneratedMacroState>& states) {
                        transitions.emplace(tuple, states);
                        return true;
                    });
            return transitions;
        }

        // Arity-1 variant of the generic fallback materialization.
        Arity1TransitionMap build_fallback_arity1_visible_transitions(const NodeId node_id, const MacroStateId state) {
            Arity1TransitionBuilder transitions{};
            for (const auto& [tuple, states] : build_fallback_visible_transitions(node_id, state)) {
                assert(tuple.size() == 1);
                transitions.emplace(tuple[0], states);
            }
            return Arity1TransitionMap::freeze(std::move(transitions));
        }

        Arity2TransitionMap build_fallback_arity2_visible_transitions(const NodeId node_id, const MacroStateId state) {
            Arity2TransitionBuilder transitions{};
            for (const auto& [tuple, states] : build_fallback_visible_transitions(node_id, state)) {
                assert(tuple.size() == 2);
                transitions.emplace(pack_arity2_tuple(tuple), states);
            }
            return Arity2TransitionMap::freeze(std::move(transitions));
        }

        // Cached symbol-only transitions for arity-1 nodes.
        const Arity1TransitionMap& get_arity1_visible_transitions(const NodeId node_id, const MacroStateId state) {
            return transition_cache.get_arity1_visible_transitions(
                    node_id, state,
                    [&](const NodeId child_id, const MacroStateId child_state, Arity1TransitionMap& fallback)
                            -> const Arity1TransitionMap& {
                        return get_arity1_transitions_with_fallback(child_id, child_state, fallback);
                    });
        }

        // Cached visible transitions for every non-complement node kind.
        const TransitionMap& get_visible_transitions(const NodeId node_id, const MacroStateId state) {
            return transition_cache.get_visible_transitions(
                    node_id, state,
                    [&](const NodeId child_id, const MacroStateId child_state,
                        TransitionMap& fallback) -> const TransitionMap& {
                        return get_transitions_with_fallback(child_id, child_state, fallback);
                    });
        }

        const Arity2TransitionMap& get_arity2_visible_transitions(const NodeId node_id, const MacroStateId state) {
            return transition_cache.get_arity2_visible_transitions(
                    node_id, state,
                    [&](const NodeId child_id, const MacroStateId child_state, Arity2TransitionMap& fallback)
                            -> const Arity2TransitionMap& {
                        return get_arity2_transitions_with_fallback(child_id, child_state, fallback);
                    },
                    [&](const NodeId child_id, const MacroStateId child_state, Arity1TransitionMap& fallback)
                            -> const Arity1TransitionMap& {
                        return get_arity1_transitions_with_fallback(child_id, child_state, fallback);
                    },
                    [&](const NodeId child_id, const MacroStateId child_state, TransitionMap& fallback)
                            -> const TransitionMap& { return get_transitions_with_fallback(child_id, child_state, fallback); });
        }

        // Read the cached successors for one exact visible tuple.
        const std::vector<GeneratedMacroState>&
        get_next_states(const NodeId node_id, const MacroStateId state, const SymbolTuple& tuple) {
            if (is_complement_exec_kind(nodes[node_id].kind)) {
                throw std::logic_error("Complement next states should be queried through the iterator path.");
            }

            const TransitionMap& transitions = get_visible_transitions(node_id, state);
            const auto it = transitions.find(tuple);
            static const std::vector<GeneratedMacroState> empty_states{};
            return it == transitions.end() ? empty_states : it->second;
        }

        // Enumerate all enabled visible tuples that match a partial assignment.
        template<typename Visitor>
        bool for_each_enabled_label_tuple(
                const NodeId node_id, const MacroStateId state, const OptionalTuple& partial_tuple, Visitor&& visitor) {
            if (is_complement_exec_kind(nodes[node_id].kind)) {
                // Complement is enabled on the whole visible universe. The child only constrains
                // acceptance of those tuples, not whether the tuple exists syntactically.
                return enumerate_partial_tuples(node_id, partial_tuple, visitor);
            }

            for (const auto& [tuple, _] : get_visible_transitions(node_id, state)) {
                if (matches_partial_tuple(tuple, partial_tuple) && !visitor(tuple)) {
                    return false;
                }
            }

            return true;
        }

        // Enumerate all enabled visible tuples for one macrostate.
        template<typename Visitor>
        bool for_each_enabled_label_tuple(const NodeId node_id, const MacroStateId state, Visitor&& visitor) {
            OptionalTuple partial_tuple(nodes[node_id].result_arity);
            return for_each_enabled_label_tuple(node_id, state, partial_tuple, std::forward<Visitor>(visitor));
        }

        // Arity-1 shortcut that enumerates only visible symbols.
        template<typename Visitor>
        bool for_each_enabled_arity1_symbol(const NodeId node_id, const MacroStateId state, Visitor&& visitor) {
            if (nodes[node_id].result_arity != 1) {
                throw std::logic_error("Symbol enumeration is available only for arity-1 nodes.");
            }

            if (nodes[node_id].kind == ExecKind::Arity1Complement || nodes[node_id].kind == ExecKind::Complement) {
                for (const mata::Symbol symbol : level_alphabets[node_id][0].get_alphabet_symbols()) {
                    if (!visitor(symbol)) {
                        return false;
                    }
                }
                return true;
            }

            if (is_arity1_exec(node_id)) {
                for (const auto& [symbol, _] : get_arity1_visible_transitions(node_id, state)) {
                    if (!visitor(symbol)) {
                        return false;
                    }
                }
                return true;
            }

            return for_each_enabled_label_tuple(node_id, state, [&](const SymbolTuple& tuple) {
                assert(tuple.size() == 1);
                return visitor(tuple[0]);
            });
        }

        template<typename Visitor>
        bool for_each_enabled_arity2_tuple(const NodeId node_id, const MacroStateId state, Visitor&& visitor) {
            if (nodes[node_id].result_arity != 2) {
                throw std::logic_error("Arity-2 tuple enumeration is available only for arity-2 nodes.");
            }

            if (nodes[node_id].kind == ExecKind::Arity2Complement || nodes[node_id].kind == ExecKind::Complement) {
                return for_each_enabled_label_tuple(node_id, state, [&](const SymbolTuple& tuple) {
                    assert(tuple.size() == 2);
                    return visitor(pack_arity2_tuple(tuple));
                });
            }

            for (const auto& [tuple, _] : get_arity2_visible_transitions(node_id, state)) {
                if (!visitor(tuple)) {
                    return false;
                }
            }

            return true;
        }

        // Emit leaf initial states for an automaton leaf node.
        template<typename Automaton>
        bool emit_leaf_initial_states(const Automaton& automaton, const MacroStateVisitor& visitor) {
            for (const State initial_state : automaton.initial) {
                if (!visitor(
                            GeneratedMacroState{
                                    static_cast<MacroStateId>(initial_state),
                                    automaton.final.contains(initial_state)})) {
                    return false;
                }
            }

            return true;
        }

        // Enumerate initial macrostates for any node kind.
        bool for_each_initial_macro_state(const NodeId node_id, const MacroStateVisitor& visitor) {
            const ExecNode& node = nodes[node_id];

            switch (node.kind) {
                case ExecKind::LeafNfa:
                    return emit_leaf_initial_states(nfas[node.lhs], visitor);

                case ExecKind::LeafNft:
                case ExecKind::Arity2LeafNft:
                    return emit_leaf_initial_states(nfts[node.lhs], visitor);

                case ExecKind::Union:
                case ExecKind::Arity1Union:
                case ExecKind::Arity2Union: {
                    if (!for_each_initial_macro_state(node.lhs, [&](const GeneratedMacroState& lhs_state) {
                            const MacroStateId id =
                                    macro_store.intern(node_id, TaggedState{lhs_state.id, TaggedState::Tag::Left});
                            return visitor(GeneratedMacroState{id, lhs_state.accepting});
                        })) {
                        return false;
                    }

                    return for_each_initial_macro_state(node.rhs, [&](const GeneratedMacroState& rhs_state) {
                        const MacroStateId id =
                                macro_store.intern(node_id, TaggedState{rhs_state.id, TaggedState::Tag::Right});
                        return visitor(GeneratedMacroState{id, rhs_state.accepting});
                    });
                }

                case ExecKind::Intersect:
                case ExecKind::SyncProduct:
                case ExecKind::Arity1Intersect:
                case ExecKind::Arity2Intersect:
                case ExecKind::Arity2SyncProduct: {
                    return for_each_initial_macro_state(node.lhs, [&](const GeneratedMacroState& lhs_state) {
                        return for_each_initial_macro_state(node.rhs, [&](const GeneratedMacroState& rhs_state) {
                            const MacroStateId id = macro_store.intern(node_id, PairState{lhs_state.id, rhs_state.id});
                            return visitor(GeneratedMacroState{id, lhs_state.accepting && rhs_state.accepting});
                        });
                    });
                }

                case ExecKind::Complement:
                case ExecKind::Arity1Complement:
                case ExecKind::Arity2Complement: {
                    SetState sub_initial_states{};
                    bool accepting = true;
                    if (!for_each_initial_macro_state(node.lhs, [&](const GeneratedMacroState& sub_initial_state) {
                            sub_initial_states.insert(sub_initial_state.id);
                            accepting = accepting && !sub_initial_state.accepting;
                            return true;
                        })) {
                        return false;
                    }
                    const MacroStateId id = macro_store.intern(node_id, std::move(sub_initial_states));
                    return visitor(GeneratedMacroState{id, accepting});
                }

                case ExecKind::Identity:
                case ExecKind::Project:
                case ExecKind::Arity2Project:
                    return for_each_initial_macro_state(node.lhs, visitor);
            }

            return true;
        }

        // Build the iterator used to enumerate next states for one exact tuple.
        NextStateIteratorPtr
        make_next_state_iterator(const NodeId node_id, const MacroStateId state, const SymbolTuple& tuple) {
            if (!is_complement_exec_kind(nodes[node_id].kind)) {
                return std::make_unique<BufferedNextStateIterator>(*this, get_next_states(node_id, state, tuple));
            }
            const ExecNode& node = nodes[node_id];
            const SetState& sub_states = macro_store.get_set(node_id, state);
            return std::make_unique<ComplementNextStateIterator>(*this, node_id, node.lhs, sub_states, tuple);
        }

        // Visit every successor macrostate for one exact visible tuple.
        template<typename Visitor>
        bool for_each_next_macro_state(
                const NodeId& node_id, const MacroStateId& state, const SymbolTuple& tuple, Visitor&& visitor) {
            if (!is_complement_exec_kind(nodes[node_id].kind)) {
                for (const GeneratedMacroState& next_state : get_next_states(node_id, state, tuple)) {
                    if (!visitor(next_state)) {
                        return false;
                    }
                }
                return true;
            }

            NextStateIteratorPtr iter = make_next_state_iterator(node_id, state, tuple);
            while (true) {
                const std::optional<GeneratedMacroState> next_state = iter->next();
                if (!next_state.has_value()) {
                    return true;
                }

                if (!visitor(*next_state)) {
                    return false;
                }
            }
        }

        // Arity-1 shortcut for successor generation on one symbol.
        template<typename Visitor>
        bool for_each_next_macro_state_on_arity1_symbol(
                const NodeId node_id, const MacroStateId state, const mata::Symbol symbol, Visitor&& visitor) {
            if (nodes[node_id].result_arity != 1) {
                throw std::logic_error("Symbol-driven next-state queries are available only for arity-1 nodes.");
            }

            if (!is_complement_exec_kind(nodes[node_id].kind) && is_arity1_exec(node_id)) {
                const auto& transitions = get_arity1_visible_transitions(node_id, state);
                const auto it = transitions.find(symbol);
                if (it == transitions.end()) {
                    return true;
                }

                for (const GeneratedMacroState& next_state : it->second) {
                    if (!visitor(next_state)) {
                        return false;
                    }
                }
                return true;
            }

            return for_each_next_macro_state(node_id, state, SymbolTuple{symbol}, visitor);
        }

        template<typename Visitor>
        bool for_each_next_macro_state_on_arity2_tuple(
                const NodeId node_id, const MacroStateId state, const Arity2TransitionKey tuple, Visitor&& visitor) {
            if (nodes[node_id].result_arity != 2) {
                throw std::logic_error("Arity-2 tuple-driven next-state queries are available only for arity-2 nodes.");
            }

            if (!is_complement_exec_kind(nodes[node_id].kind)) {
                const auto& transitions = get_arity2_visible_transitions(node_id, state);
                const auto it = transitions.find(tuple);
                if (it == transitions.end()) {
                    return true;
                }

                for (const GeneratedMacroState& next_state : it->second) {
                    if (!visitor(next_state)) {
                        return false;
                    }
                }
                return true;
            }

            return for_each_next_macro_state(node_id, state, unpack_arity2_tuple(tuple), visitor);
        }

        // Expand one root macrostate using the cheapest label representation for the root arity.
    };

} // namespace

bool is_empty(
        const SymbolicAutomataTree& tree, const Term& root_node,
        const std::vector<mata::OnTheFlyAlphabet>* level_alphabets) {
    Context ctx(tree, root_node.get_id(), level_alphabets);

    std::list<MacroStateId> worklist{};

    // The queued is a mirror of worklist, but in a hash set instead of a list
    // this is to speed up the subsumption process by having constant-time removal
    std::unordered_set<MacroStateId> queued{};
    std::unordered_set<MacroStateId> visited{};

    const auto enqueue_if_relevant = [&](const GeneratedMacroState& generated_state) {
        if (generated_state.accepting) {
            return false;
        }

        if (ctx.subsumption.is_subsumed(ctx.root_id, generated_state.id, visited, queued)) {
            return true;
        }

        queued.insert(generated_state.id);
        worklist.push_back(generated_state.id);
        return true;
    };

    const bool initial_states_fully_processed = ctx.for_each_initial_macro_state(ctx.root_id, enqueue_if_relevant);
    if (!initial_states_fully_processed) {
        return false;
    }

    const auto pop_next_pending_state = [&]() -> std::optional<MacroStateId> {
        while (!worklist.empty()) {
            const MacroStateId current_state = worklist.back();
            worklist.pop_back();
            if (!queued.erase(current_state)) {
                continue;
            }

            visited.insert(current_state);
            return current_state;
        }

        return std::nullopt;
    };

    if (ctx.nodes[ctx.root_id].result_arity == 1) {
        // fast path for arity-1 roots, which are common in practice and allow for cheaper symbol-driven expansion.
        // I don't know if there are some compiler optimizations that would make the more generic tuple-driven code path
        // just as fast, but this is simple enough and guaranteed to be fast without relying on fancy inlining.
        while (const std::optional<MacroStateId> current_state = pop_next_pending_state()) {
            const auto process = [&](const mata::Symbol symbol) {
                return ctx.for_each_next_macro_state_on_arity1_symbol(
                        ctx.root_id, *current_state, symbol, enqueue_if_relevant);
            };

            const bool fully_expanded =
                    ctx.for_each_enabled_arity1_symbol(ctx.root_id, *current_state, std::move(process));
            if (!fully_expanded) {
                return false;
            }
        }

        return true;
    }

    if (ctx.nodes[ctx.root_id].result_arity == 2) {
        while (const std::optional<MacroStateId> current_state = pop_next_pending_state()) {
            const auto process = [&](const Arity2TransitionKey tuple) {
                return ctx.for_each_next_macro_state_on_arity2_tuple(
                        ctx.root_id, *current_state, tuple, enqueue_if_relevant);
            };

            const bool fully_expanded =
                    ctx.for_each_enabled_arity2_tuple(ctx.root_id, *current_state, std::move(process));
            if (!fully_expanded) {
                return false;
            }
        }

        return true;
    }

    while (const std::optional<MacroStateId> current_state = pop_next_pending_state()) {
        const auto process = [&](const SymbolTuple& tuple) {
            return ctx.for_each_next_macro_state(ctx.root_id, *current_state, tuple, enqueue_if_relevant);
        };

        const bool fully_expanded = ctx.for_each_enabled_label_tuple(ctx.root_id, *current_state, std::move(process));
        if (!fully_expanded) {
            return false;
        }
    }

    return true;
}

} // namespace mata::nft::lazy::detail
