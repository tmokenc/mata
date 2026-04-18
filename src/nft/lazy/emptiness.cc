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

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mata::nft::lazy::detail {

namespace {
    struct TransitionCache {
        using Key = std::pair<NodeId, MacroStateId>;

        struct KeyHash {
            size_t operator()(const Key& key) const noexcept {
                return static_cast<size_t>(
                        mix_hash64((static_cast<uint64_t>(key.first) << 32) | static_cast<uint64_t>(key.second)));
            }
        };

        std::unordered_map<Key, std::vector<GeneratedTransition>, KeyHash> cache{};

        const std::vector<GeneratedTransition>* get(const NodeId node_id, const MacroStateId state) const {
            const auto it = cache.find(Key{node_id, state});
            return it == cache.end() ? nullptr : &it->second;
        }

        const std::vector<GeneratedTransition>&
        set(const NodeId node_id, const MacroStateId state, std::vector<GeneratedTransition> transitions) {
            const auto [it, _] = cache.emplace(Key{node_id, state}, std::move(transitions));
            return it->second;
        }
    };

    std::vector<CompiledSyncPlan> compile_sync_plans(const std::vector<SyncPlan>& sync_plans) {
        std::vector<CompiledSyncPlan> compiled_plans{};
        compiled_plans.reserve(sync_plans.size());

        for (const SyncPlan& plan : sync_plans) {
            CompiledSyncPlan compiled{};
            compiled.lhs_sync_levels = plan.lhs_sync_levels;
            compiled.rhs_sync_levels = plan.rhs_sync_levels;
            compiled.result_layout = plan.result_layout;

            const uint8_t lhs_levels = [&]() -> uint8_t {
                uint8_t max_level = 0;
                for (const uint8_t level : compiled.lhs_sync_levels) {
                    if (level > max_level) {
                        max_level = level;
                    }
                }
                return compiled.lhs_sync_levels.empty() ? 0 : static_cast<uint8_t>(max_level + 1);
            }();

            const uint8_t rhs_levels = [&]() -> uint8_t {
                uint8_t max_level = 0;
                for (const uint8_t level : compiled.rhs_sync_levels) {
                    if (level > max_level) {
                        max_level = level;
                    }
                }
                return compiled.rhs_sync_levels.empty() ? 0 : static_cast<uint8_t>(max_level + 1);
            }();

            compiled.lhs_sync_peer_by_level.assign(lhs_levels, -1);
            compiled.rhs_sync_peer_by_level.assign(rhs_levels, -1);
            for (size_t i = 0; i < compiled.lhs_sync_levels.size(); ++i) {
                compiled.lhs_sync_peer_by_level[compiled.lhs_sync_levels[i]] = static_cast<int16_t>(i);
                compiled.rhs_sync_peer_by_level[compiled.rhs_sync_levels[i]] = static_cast<int16_t>(i);
            }

            compiled_plans.push_back(std::move(compiled));
        }

        return compiled_plans;
    }

    struct Context final : IteratorContext {
        using Nfa = mata::nfa::Nfa;
        using Nft = mata::nft::Nft;

        const std::vector<Nfa>& nfas;
        const std::vector<Nft>& nfts;
        const std::vector<ProjectPlan>& project_plans;

        std::vector<ExecNode> nodes;
        std::vector<CompiledSyncPlan> sync_plans;
        MacroStateStore macro_store;
        AlphabetStore alphabets;
        SubsumptionEngine subsumption;
        TransitionTupleHelper transition_tuple_helper;
        TransitionCache transition_cache;
        NodeId root_id;

        Context(const SymbolicAutomataTree& tree, NodeId root,
                const std::vector<mata::OnTheFlyAlphabet>* root_level_alphabets = nullptr)
            : nfas(tree.nfas), nfts(tree.nfts), project_plans(tree.project_plans), nodes{},
              sync_plans{compile_sync_plans(tree.sync_plans)}, macro_store{}, alphabets{},
              subsumption{SubsumptionContext{nfas, nfts, nodes, macro_store}},
              transition_tuple_helper{nodes, alphabets}, transition_cache{}, root_id{0} {

            root_id = reconstruct_nodes(tree, root, nodes);
            macro_store = MacroStateStore(nodes, nfas, nfts);
            alphabets = AlphabetStore{nodes, root_id, nfas, nfts, tree.sync_plans, project_plans, root_level_alphabets};
            subsumption.initialize_leaf_simulations(root_id);
        }

        MacroStateStore& macro_store_ref() override { return macro_store; }

        TransitionIteratorPtr make_transition_iterator(const NodeId node_id, const MacroStateId state) override {
            if (const std::vector<GeneratedTransition>* cached = transition_cache.get(node_id, state)) {
                return make_buffered_transition_iterator(*this, *cached);
            }

            TransitionIteratorPtr uncached = make_uncached_transition_iterator(node_id, state);
            std::vector<GeneratedTransition> materialized{};
            while (const std::optional<GeneratedTransition> transition = uncached->next()) {
                materialized.push_back(std::move(*transition));
            }

            const std::vector<GeneratedTransition>& cached =
                    transition_cache.set(node_id, state, std::move(materialized));
            return make_buffered_transition_iterator(*this, cached);
        }

    private:
        TransitionIteratorPtr make_uncached_transition_iterator(const NodeId node_id, const MacroStateId state) {
            const ExecNode& node = nodes[node_id];

            switch (node.kind) {
                case ExecKind::LeafNfa:
                    return make_leaf_nfa_transition_iterator(*this, nfas[node.lhs], alphabets, node_id, state);

                case ExecKind::LeafNft:
                case ExecKind::Arity2LeafNft:
                    return make_leaf_nft_transition_iterator(
                            *this, nfts[node.lhs], alphabets, node_id, state, node.result_arity);

                case ExecKind::Union:
                case ExecKind::Arity1Union:
                case ExecKind::Arity2Union: {
                    const TaggedState tagged = macro_store.get_tagged(node_id, state);
                    const NodeId child_id = tagged.tag == TaggedState::Tag::Left ? node.lhs : node.rhs;
                    return make_union_transition_iterator(
                            *this, node_id, tagged.tag, make_transition_iterator(child_id, tagged.state));
                }

                case ExecKind::Intersect:
                case ExecKind::Arity1Intersect:
                case ExecKind::Arity2Intersect: {
                    const PairState pair = macro_store.get_pair(node_id, state);
                    return make_intersect_transition_iterator(
                            *this, transition_tuple_helper, node_id, node.lhs, pair.lhs, node.rhs, pair.rhs);
                }

                case ExecKind::SyncProduct:
                case ExecKind::Arity2SyncProduct: {
                    const PairState pair = macro_store.get_pair(node_id, state);
                    return make_sync_product_transition_iterator(
                            *this, transition_tuple_helper, node_id, node.lhs, pair.lhs, node.rhs, pair.rhs,
                            sync_plans[node.payload]);
                }

                case ExecKind::Complement:
                case ExecKind::Arity1Complement:
                case ExecKind::Arity2Complement: {
                    std::vector<std::vector<mata::Symbol>> level_symbols(node.result_arity);
                    for (uint8_t level = 0; level < node.result_arity; ++level) {
                        level_symbols[level] =
                                alphabets.level_alphabet(node_id, level).get_alphabet_symbols().to_vector();
                    }

                    return make_complement_transition_iterator(
                            *this, node_id, node.lhs, macro_store.get_set(node_id, state), subsumption,
                            std::move(level_symbols));
                }

                case ExecKind::Identity:
                    return make_identity_transition_iterator(*this, make_transition_iterator(node.lhs, state));

                case ExecKind::Project:
                case ExecKind::Arity2Project:
                    return make_project_transition_iterator(
                            *this, project_plans[node.payload], make_transition_iterator(node.lhs, state));
            }

            throw std::logic_error("Unreachable transition reconstruction branch.");
        }

    public:
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
                            *this, node_id, node.lhs, make_initial_state_iterator(node.lhs), subsumption);
                }

                case ExecKind::Identity:
                case ExecKind::Project:
                case ExecKind::Arity2Project:
                    return make_passthrough_initial_state_iterator(*this, make_initial_state_iterator(node.lhs));
            }

            throw std::logic_error("Unreachable initial-state reconstruction branch.");
        }
    };

} // namespace

bool is_empty(
        const SymbolicAutomataTree& tree, const Term& root_node,
        const std::vector<mata::OnTheFlyAlphabet>* level_alphabets) {
    Context ctx(tree, root_node.get_id(), level_alphabets);

    std::vector<MacroStateId> worklist{};
    std::unordered_set<MacroStateId> visited{};

    const auto enqueue_if_relevant = [&](const GeneratedMacroState& generated_state) {
        const MacroStateId state = generated_state.id;

        if (!visited.insert(state).second) {
            return;
        }

        if (ctx.subsumption.is_subsumed(ctx.root_id, state)) {
            return;
        }

        worklist.push_back(state);
    };

    InitialStateIteratorPtr initial_states = ctx.make_initial_state_iterator(ctx.root_id);

    while (const std::optional<GeneratedMacroState> initial_state = initial_states->next()) {
        if (initial_state->accepting) {
            return false;
        }

        enqueue_if_relevant(*initial_state);
    }

    while (!worklist.empty()) {
        const MacroStateId current_state = worklist.back();
        worklist.pop_back();

        if (visited.contains(current_state)) {
            continue;
        }

        if (ctx.subsumption.is_pruned(current_state)) {
            continue;
        }

        TransitionIteratorPtr transitions = ctx.make_transition_iterator(ctx.root_id, current_state);
        while (const std::optional<GeneratedTransition> transition = transitions->next()) {
            if (transition->state.accepting) {
                return false;
            }

            enqueue_if_relevant(transition->state);
        }
    }

    return true;
}

} // namespace mata::nft::lazy::detail
