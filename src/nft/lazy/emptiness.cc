/**
 * @file emptiness.cc
 * @brief Private execution engine for mata::nft::lazy::detail.
 */

#include "emptiness.hh"

#include "alphabet_store.hh"
#include "iterators.hh"
#include "macrostate_store.hh"
#include "reconstruction.hh"
#include "subsumption.hh"
#include "symbols.hh"
#include "transition_cache.hh"

#include <mata/simlib/explicit_lts.hh>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mata::nft::lazy::detail {

namespace {
    struct Context;
    std::shared_ptr<TransitionResolver> make_transition_resolver(Context& ctx);

    struct Context final : IteratorContext {
        using Nfa = mata::nfa::Nfa;
        using Nft = mata::nft::Nft;

        const std::vector<Nfa>& nfas;
        const std::vector<Nft>& nfts;
        const std::vector<SyncPlan>& sync_plans;
        const std::vector<ProjectPlan>& project_plans;

        std::vector<ExecNode> nodes;
        MacroStateStore macro_store;
        AlphabetStore alphabets;
        SubsumptionEngine subsumption;
        TransitionCache transition_cache;
        std::shared_ptr<TransitionResolver> transition_resolver;
        NodeId root_id;

        Context(const SymbolicAutomataTree& tree, NodeId root,
                const std::vector<mata::OnTheFlyAlphabet>* root_level_alphabets = nullptr)
            : nfas(tree.nfas), nfts(tree.nfts), sync_plans(tree.sync_plans), project_plans(tree.project_plans), nodes{},
              macro_store{}, alphabets{}, subsumption{SubsumptionContext{nfas, nfts, nodes, macro_store}},
              transition_cache{
                      TransitionCacheContext{nfas, nfts, sync_plans, project_plans, nodes, macro_store, alphabets}},
              transition_resolver{}, root_id{0} {

            root_id = reconstruct_nodes(tree, root, nodes);
            macro_store = MacroStateStore(nodes, nfas, nfts);
            alphabets = AlphabetStore{nodes, root_id, nfas, nfts, sync_plans, project_plans, root_level_alphabets};
            transition_resolver = make_transition_resolver(*this);
            subsumption.initialize_leaf_simulations(root_id);
        }

        bool is_arity1_exec(const NodeId node_id) const noexcept { return is_arity1_exec_kind(nodes[node_id].kind); }

        MacroStateStore& macro_store_ref() override { return macro_store; }

        // Materialize a generic visible-transition map for nodes that do not expose one directly.
        TransitionMap build_fallback_visible_transitions(const NodeId node_id, const MacroStateId state) {
            TransitionMap transitions{};
            if (!is_complement_exec_kind(nodes[node_id].kind)) {
                for (const auto& [tuple, states] : get_visible_transitions(node_id, state)) {
                    transitions.emplace(tuple, states);
                }
                return transitions;
            }

            // Complement cannot expose a finite transition cache in general because its
            // enabled labels come from the whole visible universe, not only from child
            // labels. Materialize it on demand through the iterator path instead.
            LabelIteratorPtr labels = make_label_iterator(node_id, state);
            while (const std::optional<SymbolTuple> tuple = labels->next()) {
                std::vector<GeneratedMacroState> states{};
                NextStateIteratorPtr next_states = make_next_state_iterator(node_id, state, *tuple);
                while (const std::optional<GeneratedMacroState> next_state = next_states->next()) {
                    append_generated_state(states, *next_state);
                }
                if (states.empty()) {
                    continue;
                }
                transitions.emplace(*tuple, std::move(states));
            }
            return transitions;
        }

        template<typename OutputMap, typename Builder, typename KeyFactory>
        OutputMap
        build_fallback_visible_transitions_as(const NodeId node_id, const MacroStateId state, KeyFactory&& make_key) {
            Builder transitions{};
            for (const auto& [tuple, states] : build_fallback_visible_transitions(node_id, state)) {
                transitions.emplace(make_key(tuple), states);
            }
            return OutputMap::freeze(std::move(transitions));
        }

        // Arity-1 variant of the generic fallback materialization.
        Arity1TransitionMap build_fallback_arity1_visible_transitions(const NodeId node_id, const MacroStateId state) {
            return build_fallback_visible_transitions_as<Arity1TransitionMap, Arity1TransitionBuilder>(
                    node_id, state, [](const SymbolTuple& tuple) {
                        assert(tuple.size() == 1);
                        return tuple[0];
                    });
        }

        Arity2TransitionMap build_fallback_arity2_visible_transitions(const NodeId node_id, const MacroStateId state) {
            return build_fallback_visible_transitions_as<Arity2TransitionMap, Arity2TransitionBuilder>(
                    node_id, state, [](const SymbolTuple& tuple) {
                        assert(tuple.size() == 2);
                        return pack_arity2_tuple(tuple);
                    });
        }

        // Cached symbol-only transitions for arity-1 nodes.
        const Arity1TransitionMap& get_arity1_visible_transitions(const NodeId node_id, const MacroStateId state) {
            return transition_cache.get_arity1_visible_transitions(node_id, state, *transition_resolver);
        }

        // Cached visible transitions for every non-complement node kind.
        const TransitionMap& get_visible_transitions(const NodeId node_id, const MacroStateId state) {
            return transition_cache.get_visible_transitions(node_id, state, *transition_resolver);
        }

        const Arity2TransitionMap& get_arity2_visible_transitions(const NodeId node_id, const MacroStateId state) {
            return transition_cache.get_arity2_visible_transitions(node_id, state, *transition_resolver);
        }

        LabelIteratorPtr make_label_iterator(const NodeId node_id, const MacroStateId state) {
            if (!is_complement_exec_kind(nodes[node_id].kind)) {
                return make_transition_label_iterator(get_visible_transitions(node_id, state));
            }

            std::vector<std::vector<mata::Symbol>> level_symbols(nodes[node_id].result_arity);
            for (uint8_t level = 0; level < nodes[node_id].result_arity; ++level) {
                level_symbols[level] = alphabets.level_alphabet(node_id, level).get_alphabet_symbols().to_vector();
            }
            return make_universe_label_iterator(std::move(level_symbols));
        }

        Arity1LabelIteratorPtr make_arity1_label_iterator(const NodeId node_id, const MacroStateId state) {
            if (nodes[node_id].result_arity != 1) {
                throw std::logic_error("Symbol-driven label queries are available only for arity-1 nodes.");
            }

            if (nodes[node_id].kind == ExecKind::Arity1Complement || nodes[node_id].kind == ExecKind::Complement) {
                return make_symbol_label_iterator(
                        alphabets.level_alphabet(node_id, 0).get_alphabet_symbols().to_vector());
            }

            if (is_arity1_exec(node_id)) {
                return make_arity1_transition_label_iterator(get_arity1_visible_transitions(node_id, state));
            }

            return make_tuple_to_arity1_label_iterator(make_label_iterator(node_id, state));
        }

        Arity2LabelIteratorPtr make_arity2_label_iterator(const NodeId node_id, const MacroStateId state) {
            if (nodes[node_id].result_arity != 2) {
                throw std::logic_error("Arity-2 tuple-driven label queries are available only for arity-2 nodes.");
            }

            if (!is_complement_exec_kind(nodes[node_id].kind)) {
                return make_arity2_transition_label_iterator(get_arity2_visible_transitions(node_id, state));
            }

            return make_tuple_to_arity2_label_iterator(make_label_iterator(node_id, state));
        }

        template<typename TransitionMapT, typename Key>
        static const std::vector<GeneratedMacroState>&
        find_cached_next_states(const TransitionMapT& transitions, const Key& key) {
            const auto it = transitions.find(key);
            static const std::vector<GeneratedMacroState> empty_states{};
            return it == transitions.end() ? empty_states : it->second;
        }

        // Read the cached successors for one exact visible tuple.
        const std::vector<GeneratedMacroState>&
        get_next_states(const NodeId node_id, const MacroStateId state, const SymbolTuple& tuple) {
            if (is_complement_exec_kind(nodes[node_id].kind)) {
                throw std::logic_error("Complement next states should be queried through the iterator path.");
            }

            return find_cached_next_states(get_visible_transitions(node_id, state), tuple);
        }

        InitialStateIteratorPtr make_initial_state_iterator(const NodeId node_id) override {
            const ExecNode& node = nodes[node_id];

            switch (node.kind) {
                case ExecKind::LeafNfa:
                    return make_leaf_nfa_initial_state_iterator(*this, nfas[node.lhs]);

                case ExecKind::LeafNft:
                case ExecKind::Arity2LeafNft:
                    return make_leaf_nft_initial_state_iterator(*this, nfts[node.lhs]);

                case ExecKind::Union:
                case ExecKind::Arity1Union:
                case ExecKind::Arity2Union: {
                    return make_union_initial_state_iterator(
                            *this, node_id, make_initial_state_iterator(node.lhs),
                            make_initial_state_iterator(node.rhs));
                }

                case ExecKind::Intersect:
                case ExecKind::SyncProduct:
                case ExecKind::Arity1Intersect:
                case ExecKind::Arity2Intersect:
                case ExecKind::Arity2SyncProduct: {
                    return make_product_initial_state_iterator(
                            *this, node_id, node.rhs, make_initial_state_iterator(node.lhs));
                }

                case ExecKind::Complement:
                case ExecKind::Arity1Complement:
                case ExecKind::Arity2Complement: {
                    return make_complement_initial_state_iterator(
                            *this, node_id, make_initial_state_iterator(node.lhs));
                }

                case ExecKind::Identity:
                case ExecKind::Project:
                case ExecKind::Arity2Project:
                    return make_passthrough_initial_state_iterator(*this, make_initial_state_iterator(node.lhs));
            }

            throw std::logic_error("Unreachable initial-state reconstruction branch.");
        }

        // Build the iterator used to enumerate next states for one exact tuple.
        NextStateIteratorPtr
        make_next_state_iterator(const NodeId node_id, const MacroStateId state, const SymbolTuple& tuple) override {
            if (!is_complement_exec_kind(nodes[node_id].kind)) {
                return make_buffered_next_state_iterator(*this, get_next_states(node_id, state, tuple));
            }
            const ExecNode& node = nodes[node_id];
            const SetState& sub_states = macro_store.get_set(node_id, state);
            return make_complement_next_state_iterator(*this, node_id, node.lhs, sub_states, tuple);
        }

        NextStateIteratorPtr
        make_arity1_next_state_iterator(const NodeId node_id, const MacroStateId state, const mata::Symbol symbol) {
            if (nodes[node_id].result_arity != 1) {
                throw std::logic_error("Symbol-driven next-state queries are available only for arity-1 nodes.");
            }

            if (!is_complement_exec_kind(nodes[node_id].kind) && is_arity1_exec(node_id)) {
                return make_buffered_next_state_iterator(
                        *this, find_cached_next_states(get_arity1_visible_transitions(node_id, state), symbol));
            }

            return make_next_state_iterator(node_id, state, SymbolTuple{symbol});
        }

        NextStateIteratorPtr make_arity2_next_state_iterator(
                const NodeId node_id, const MacroStateId state, const Arity2TransitionKey tuple) {
            if (nodes[node_id].result_arity != 2) {
                throw std::logic_error("Arity-2 tuple-driven next-state queries are available only for arity-2 nodes.");
            }

            if (!is_complement_exec_kind(nodes[node_id].kind)) {
                return make_buffered_next_state_iterator(
                        *this, find_cached_next_states(get_arity2_visible_transitions(node_id, state), tuple));
            }

            return make_next_state_iterator(node_id, state, unpack_arity2_tuple(tuple));
        }
    };

    class RecursiveTransitionResolverNode final : public TransitionResolver {
    public:
        enum class Mode : uint8_t {
            Generic,
            Arity1,
            Arity2,
        };

        Context& ctx;
        Mode mode;
        std::weak_ptr<RecursiveTransitionResolverNode> generic;
        std::weak_ptr<RecursiveTransitionResolverNode> arity1;
        std::weak_ptr<RecursiveTransitionResolverNode> arity2;

        RecursiveTransitionResolverNode(Context& context, const Mode resolver_mode)
            : ctx{context}, mode{resolver_mode}, generic{}, arity1{}, arity2{} {}

        const TransitionMap&
        resolve_visible(NodeId node_id, MacroStateId state, TransitionMap& fallback) const override {
            return generic_node().resolve_visible_impl(node_id, state, fallback);
        }

        const Arity1TransitionMap&
        resolve_arity1_visible(NodeId node_id, MacroStateId state, Arity1TransitionMap& fallback) const override {
            return arity1_node().resolve_arity1_visible_impl(node_id, state, fallback);
        }

        const Arity2TransitionMap&
        resolve_arity2_visible(NodeId node_id, MacroStateId state, Arity2TransitionMap& fallback) const override {
            return arity2_node().resolve_arity2_visible_impl(node_id, state, fallback);
        }

    private:
        static const RecursiveTransitionResolverNode&
        linked_node(const std::weak_ptr<RecursiveTransitionResolverNode>& link) {
            const std::shared_ptr<RecursiveTransitionResolverNode> node = link.lock();
            assert(node);
            return *node;
        }

        const RecursiveTransitionResolverNode& generic_node() const { return linked_node(generic); }
        const RecursiveTransitionResolverNode& arity1_node() const { return linked_node(arity1); }
        const RecursiveTransitionResolverNode& arity2_node() const { return linked_node(arity2); }

        const TransitionMap& resolve_visible_impl(NodeId node_id, MacroStateId state, TransitionMap& fallback) const {
            assert(mode == Mode::Generic);
            if (is_complement_exec_kind(ctx.nodes[node_id].kind)) {
                fallback = ctx.build_fallback_visible_transitions(node_id, state);
                return fallback;
            }

            return ctx.transition_cache.get_visible_transitions(node_id, state, *this);
        }

        const Arity1TransitionMap&
        resolve_arity1_visible_impl(NodeId node_id, MacroStateId state, Arity1TransitionMap& fallback) const {
            assert(mode == Mode::Arity1);
            if (is_complement_exec_kind(ctx.nodes[node_id].kind) || !ctx.is_arity1_exec(node_id)) {
                fallback = ctx.build_fallback_arity1_visible_transitions(node_id, state);
                return fallback;
            }

            return ctx.transition_cache.get_arity1_visible_transitions(node_id, state, *this);
        }

        const Arity2TransitionMap&
        resolve_arity2_visible_impl(NodeId node_id, MacroStateId state, Arity2TransitionMap& fallback) const {
            assert(mode == Mode::Arity2);
            if (is_complement_exec_kind(ctx.nodes[node_id].kind) || !is_arity2_exec_kind(ctx.nodes[node_id].kind)) {
                fallback = ctx.build_fallback_arity2_visible_transitions(node_id, state);
                return fallback;
            }

            return ctx.transition_cache.get_arity2_visible_transitions(node_id, state, *this);
        }
    };

    class RecursiveTransitionResolver final : public TransitionResolver {
    public:
        explicit RecursiveTransitionResolver(Context& ctx)
            : generic{std::make_shared<RecursiveTransitionResolverNode>(
                      ctx, RecursiveTransitionResolverNode::Mode::Generic)},
              arity1{std::make_shared<RecursiveTransitionResolverNode>(
                      ctx, RecursiveTransitionResolverNode::Mode::Arity1)},
              arity2{std::make_shared<RecursiveTransitionResolverNode>(
                      ctx, RecursiveTransitionResolverNode::Mode::Arity2)} {
            generic->generic = generic;
            generic->arity1 = arity1;
            generic->arity2 = arity2;

            arity1->generic = generic;
            arity1->arity1 = arity1;
            arity1->arity2 = arity2;

            arity2->generic = generic;
            arity2->arity1 = arity1;
            arity2->arity2 = arity2;
        }

        const TransitionMap&
        resolve_visible(NodeId node_id, MacroStateId state, TransitionMap& fallback) const override {
            return generic->resolve_visible(node_id, state, fallback);
        }

        const Arity1TransitionMap&
        resolve_arity1_visible(NodeId node_id, MacroStateId state, Arity1TransitionMap& fallback) const override {
            return arity1->resolve_arity1_visible(node_id, state, fallback);
        }

        const Arity2TransitionMap&
        resolve_arity2_visible(NodeId node_id, MacroStateId state, Arity2TransitionMap& fallback) const override {
            return arity2->resolve_arity2_visible(node_id, state, fallback);
        }

    private:
        std::shared_ptr<RecursiveTransitionResolverNode> generic;
        std::shared_ptr<RecursiveTransitionResolverNode> arity1;
        std::shared_ptr<RecursiveTransitionResolverNode> arity2;
    };

    std::shared_ptr<TransitionResolver> make_transition_resolver(Context& ctx) {
        return std::make_shared<RecursiveTransitionResolver>(ctx);
    }

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

    InitialStateIteratorPtr initial_states = ctx.make_initial_state_iterator(ctx.root_id);
    while (const std::optional<GeneratedMacroState> initial_state = initial_states->next()) {
        if (!enqueue_if_relevant(*initial_state)) {
            return false;
        }
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

    const auto drain_next_states = [&](NextStateIteratorPtr next_states) {
        while (const std::optional<GeneratedMacroState> next_state = next_states->next()) {
            if (!enqueue_if_relevant(*next_state)) {
                return false;
            }
        }

        return true;
    };

    const auto expand_worklist = [&](auto make_labels, auto make_next_states) {
        while (const std::optional<MacroStateId> current_state = pop_next_pending_state()) {
            auto labels = make_labels(*current_state);
            while (const auto label = labels->next()) {
                if (!drain_next_states(make_next_states(*current_state, *label))) {
                    return false;
                }
            }
        }

        return true;
    };

    if (ctx.nodes[ctx.root_id].result_arity == 1) {
        // fast path for arity-1 roots, which are common in practice and allow for cheaper symbol-driven expansion.
        // I don't know if there are some compiler optimizations that would make the more generic tuple-driven code path
        // just as fast, but this is simple enough and guaranteed to be fast without relying on fancy inlining.
        return expand_worklist(
                [&](const MacroStateId current_state) {
                    return ctx.make_arity1_label_iterator(ctx.root_id, current_state);
                },
                [&](const MacroStateId current_state, const mata::Symbol symbol) {
                    return ctx.make_arity1_next_state_iterator(ctx.root_id, current_state, symbol);
                });
    }

    if (ctx.nodes[ctx.root_id].result_arity == 2) {
        // Same as above, but for arity-2 roots
        return expand_worklist(
                [&](const MacroStateId current_state) {
                    return ctx.make_arity2_label_iterator(ctx.root_id, current_state);
                },
                [&](const MacroStateId current_state, const Arity2TransitionKey tuple) {
                    return ctx.make_arity2_next_state_iterator(ctx.root_id, current_state, tuple);
                });
    }

    return expand_worklist(
            [&](const MacroStateId current_state) { return ctx.make_label_iterator(ctx.root_id, current_state); },
            [&](const MacroStateId current_state, const SymbolTuple& tuple) {
                return ctx.make_next_state_iterator(ctx.root_id, current_state, tuple);
            });
}

} // namespace mata::nft::lazy::detail
