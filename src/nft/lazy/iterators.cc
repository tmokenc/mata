/**
 * @file iterators.cc
 * @brief Private iterator helpers for mata::nft::lazy::detail.
 */

#include "iterators.hh"

#include <cassert>
#include <stdexcept>
#include <utility>

namespace mata::nft::lazy::detail {

TransitionTupleHelper::TransitionTupleHelper(
        const std::vector<ExecNode>& exec_nodes, const AlphabetStore& alphabet_store)
    : nodes{exec_nodes}, alphabets{alphabet_store}, special_symbols_by_level{} {}

bool TransitionTupleHelper::merge_visible_tuples(
        const NodeId lhs_node_id, const SymbolTuple& lhs_tuple, const NodeId rhs_node_id, const SymbolTuple& rhs_tuple,
        SymbolTuple& merged_tuple) {
    initialize_special_symbol_cache();
    return try_merge_tuples(lhs_node_id, lhs_tuple, rhs_node_id, rhs_tuple, merged_tuple);
}

bool TransitionTupleHelper::build_visible_sync_result(
        const NodeId lhs_node_id, const SymbolTuple& lhs_tuple, const NodeId rhs_node_id, const SymbolTuple& rhs_tuple,
        const CompiledSyncPlan& plan, SymbolTuple& result_tuple) {
    initialize_special_symbol_cache();
    if (!sync_levels_match(
                lhs_node_id, lhs_tuple, plan.lhs_sync_levels, rhs_node_id, rhs_tuple, plan.rhs_sync_levels)) {
        return false;
    }
    return build_sync_result_tuple(lhs_node_id, lhs_tuple, rhs_node_id, rhs_tuple, plan, result_tuple);
}

void TransitionTupleHelper::initialize_special_symbol_cache() {
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

std::optional<mata::Symbol> TransitionTupleHelper::resolve_special_symbol_id(
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

bool TransitionTupleHelper::is_resolved_epsilon(
        const NodeId node_id, const uint8_t level, const mata::Symbol resolved_symbol) const {
    const ResolvedSpecialSymbols& resolved = special_symbols_by_level[node_id][level];
    return (resolved.flags & ResolvedSpecialSymbols::HasEpsilon) != 0 && resolved_symbol == resolved.epsilon;
}

bool TransitionTupleHelper::is_resolved_dont_care(
        const NodeId node_id, const uint8_t level, const mata::Symbol resolved_symbol) const {
    const ResolvedSpecialSymbols& resolved = special_symbols_by_level[node_id][level];
    return (resolved.flags & ResolvedSpecialSymbols::HasDontCare) != 0 && resolved_symbol == resolved.dont_care;
}

bool TransitionTupleHelper::is_resolved_special_symbol(
        const NodeId node_id, const uint8_t level, const mata::Symbol resolved_symbol,
        const mata::Symbol special_symbol) const {
    return special_symbol == mata::nft::EPSILON ? is_resolved_epsilon(node_id, level, resolved_symbol)
                                                : is_resolved_dont_care(node_id, level, resolved_symbol);
}

bool TransitionTupleHelper::try_merge_symbols(
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

bool TransitionTupleHelper::try_merge_tuples(
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

bool TransitionTupleHelper::sync_levels_match(
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

bool TransitionTupleHelper::build_sync_result_tuple(
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

namespace {

    template<typename Automaton>
    struct LeafInitialStateIterator final : InitialStateIterator {
        using InitialIterator = decltype(std::declval<const Automaton&>().initial.begin());

        const Automaton& automaton;
        InitialIterator current;
        InitialIterator end;

        LeafInitialStateIterator(IteratorContext& context, const Automaton& source)
            : InitialStateIterator{context}, automaton{source}, current{automaton.initial.begin()},
              end{automaton.initial.end()} {}

        std::optional<GeneratedMacroState> next() override {
            if (current == end) {
                return std::nullopt;
            }

            const auto initial_state = *current;
            ++current;
            return GeneratedMacroState{
                    static_cast<MacroStateId>(initial_state), automaton.final.contains(initial_state)};
        }
    };

    struct UnionInitialStateIterator final : InitialStateIterator {
        const NodeId parent_id;
        InitialStateIteratorPtr lhs_iter;
        InitialStateIteratorPtr rhs_iter;

        UnionInitialStateIterator(
                IteratorContext& context, const NodeId node_id, InitialStateIteratorPtr lhs_initial_iter,
                InitialStateIteratorPtr rhs_initial_iter)
            : InitialStateIterator{context}, parent_id{node_id}, lhs_iter{std::move(lhs_initial_iter)},
              rhs_iter{std::move(rhs_initial_iter)} {}

        std::optional<GeneratedMacroState> next() override {
            if (lhs_iter != nullptr) {
                if (const std::optional<GeneratedMacroState> lhs_state = lhs_iter->next(); lhs_state.has_value()) {
                    return GeneratedMacroState{
                            this->ctx.macro_store_ref().intern(
                                    parent_id, TaggedState{lhs_state->id, TaggedState::Tag::Left}),
                            lhs_state->accepting};
                }

                lhs_iter.reset();
            }

            if (rhs_iter == nullptr) {
                return std::nullopt;
            }

            if (const std::optional<GeneratedMacroState> rhs_state = rhs_iter->next(); rhs_state.has_value()) {
                return GeneratedMacroState{
                        this->ctx.macro_store_ref().intern(
                                parent_id, TaggedState{rhs_state->id, TaggedState::Tag::Right}),
                        rhs_state->accepting};
            }

            rhs_iter.reset();
            return std::nullopt;
        }
    };

    struct ProductInitialStateIterator final : InitialStateIterator {
        const NodeId parent_id;
        const NodeId rhs_id;
        InitialStateIteratorPtr lhs_iter;
        InitialStateIteratorPtr rhs_iter;
        std::optional<GeneratedMacroState> current_lhs;

        ProductInitialStateIterator(
                IteratorContext& context, const NodeId node_id, const NodeId next_rhs_id,
                InitialStateIteratorPtr lhs_initial_iter)
            : InitialStateIterator{context}, parent_id{node_id}, rhs_id{next_rhs_id},
              lhs_iter{std::move(lhs_initial_iter)}, rhs_iter{}, current_lhs{} {
            advance_lhs();
        }

        std::optional<GeneratedMacroState> next() override {
            while (current_lhs.has_value()) {
                if (const std::optional<GeneratedMacroState> rhs_state = rhs_iter->next(); rhs_state.has_value()) {
                    return GeneratedMacroState{
                            this->ctx.macro_store_ref().intern(parent_id, PairState{current_lhs->id, rhs_state->id}),
                            current_lhs->accepting && rhs_state->accepting};
                }

                if (!advance_lhs()) {
                    break;
                }
            }

            return std::nullopt;
        }

    private:
        bool advance_lhs() {
            current_lhs = lhs_iter->next();
            if (!current_lhs.has_value()) {
                rhs_iter.reset();
                return false;
            }

            rhs_iter = this->ctx.make_initial_state_iterator(rhs_id);
            return true;
        }
    };

    struct ComplementInitialStateIterator final : InitialStateIterator {
        const NodeId parent_id;
        const NodeId child_id;
        InitialStateIteratorPtr child_iter;
        SubsumptionEngine& subsumption;
        bool emitted;

        ComplementInitialStateIterator(
                IteratorContext& context, const NodeId node_id, const NodeId next_child_id,
                InitialStateIteratorPtr child_initial_iter, SubsumptionEngine& subsumption)
            : InitialStateIterator{context}, parent_id{node_id}, child_id{next_child_id},
              child_iter{std::move(child_initial_iter)}, subsumption{subsumption}, emitted{false} {}

        std::optional<GeneratedMacroState> next() override {
            if (emitted) {
                return std::nullopt;
            }
            emitted = true;

            SetState sub_initial_states{};
            bool accepting = true;
            while (const std::optional<GeneratedMacroState> sub_initial_state = child_iter->next()) {
                sub_initial_states.push_back(sub_initial_state->id);
                accepting = accepting && !sub_initial_state->accepting;
            }

            subsumption.minimize(child_id, sub_initial_states);
            const MacroStateId next_id = this->ctx.macro_store_ref().intern(parent_id, std::move(sub_initial_states));

            return GeneratedMacroState{next_id, accepting};
        }
    };

    struct PassthroughInitialStateIterator final : InitialStateIterator {
        InitialStateIteratorPtr child_iter;

        PassthroughInitialStateIterator(IteratorContext& context, InitialStateIteratorPtr child_initial_iter)
            : InitialStateIterator{context}, child_iter{std::move(child_initial_iter)} {}

        std::optional<GeneratedMacroState> next() override { return child_iter->next(); }
    };

    struct LeafNfaTransitionIterator final : TransitionIterator {
        using MoveIterator = mata::nfa::StatePost::Moves::const_iterator;

        const mata::nfa::Nfa& nfa;
        const AlphabetStore& alphabets;
        const NodeId node_id;
        mata::nfa::StatePost::Moves moves;
        MoveIterator current;
        MoveIterator end;

        LeafNfaTransitionIterator(
                IteratorContext& context, const mata::nfa::Nfa& automaton, const AlphabetStore& alphabet_store,
                const NodeId exec_node_id, const MacroStateId state)
            : TransitionIterator{context}, nfa{automaton}, alphabets{alphabet_store}, node_id{exec_node_id},
              moves{nfa.delta.state_post(static_cast<mata::nfa::State>(state)).moves()}, current{moves.begin()},
              end{mata::nfa::StatePost::Moves::end()} {}

        std::optional<GeneratedTransition> next() override {
            while (current != end) {
                const mata::nfa::Move move = *current;
                ++current;

                mata::Symbol resolved_symbol = 0;
                if (!alphabets.try_translate_local_symbol_to_resolved(nfa, node_id, 0, move.symbol, resolved_symbol)) {
                    continue;
                }

                return GeneratedTransition{
                        SymbolTuple{resolved_symbol},
                        GeneratedMacroState{static_cast<MacroStateId>(move.target), nfa.final.contains(move.target)}};
            }

            return std::nullopt;
        }
    };

    struct LeafNftTransitionIterator final : TransitionIterator {
        using MoveIterator = mata::nfa::StatePost::Moves::const_iterator;

        struct Frame {
            mata::nfa::StatePost::Moves moves;
            MoveIterator current;
            MoveIterator end;

            explicit Frame(const mata::nfa::StatePost& state_post)
                : moves{state_post.moves()}, current{moves.begin()}, end{mata::nfa::StatePost::Moves::end()} {}
        };

        const mata::nft::Nft& nft;
        const AlphabetStore& alphabets;
        const NodeId node_id;
        const size_t arity;
        const mata::nfa::State source_state;
        std::vector<Frame> frames;
        SymbolTuple current_tuple;
        bool initialized;
        bool emitted_empty;

        LeafNftTransitionIterator(
                IteratorContext& context, const mata::nft::Nft& automaton, const AlphabetStore& alphabet_store,
                const NodeId exec_node_id, const MacroStateId state, const size_t result_arity)
            : TransitionIterator{context}, nft{automaton}, alphabets{alphabet_store}, node_id{exec_node_id},
              arity{result_arity}, source_state{static_cast<mata::nfa::State>(state)}, frames{},
              current_tuple(result_arity, 0), initialized{false}, emitted_empty{false} {}

        std::optional<GeneratedTransition> next() override {
            if (arity == 0) {
                if (emitted_empty) {
                    return std::nullopt;
                }
                emitted_empty = true;
                return GeneratedTransition{
                        SymbolTuple{},
                        GeneratedMacroState{static_cast<MacroStateId>(source_state), nft.final.contains(source_state)}};
            }

            if (!initialized) {
                initialized = true;
                push_frame(source_state);
            }

            while (!frames.empty()) {
                const size_t level = frames.size() - 1;
                Frame& frame = frames.back();
                if (frame.current == frame.end) {
                    frames.pop_back();
                    if (!frames.empty()) {
                        ++frames.back().current;
                    }
                    continue;
                }

                const mata::nfa::Move move = *frame.current;
                mata::Symbol resolved_symbol = 0;
                if (!alphabets.try_translate_local_symbol_to_resolved(
                            nft, static_cast<uint8_t>(level), node_id, static_cast<uint8_t>(level), move.symbol,
                            resolved_symbol)) {
                    ++frame.current;
                    continue;
                }

                current_tuple[level] = resolved_symbol;
                if (frames.size() == arity) {
                    ++frame.current;
                    return GeneratedTransition{
                            current_tuple,
                            GeneratedMacroState{
                                    static_cast<MacroStateId>(move.target), nft.final.contains(move.target)}};
                }

                push_frame(move.target);
            }

            return std::nullopt;
        }

    private:
        void push_frame(const mata::nfa::State state) { frames.emplace_back(nft.delta.state_post(state)); }
    };

    struct BufferedTransitionIterator final : TransitionIterator {
        const std::vector<GeneratedTransition>& transitions;
        size_t index;

        BufferedTransitionIterator(
                IteratorContext& context, const std::vector<GeneratedTransition>& buffered_transitions)
            : TransitionIterator{context}, transitions{buffered_transitions}, index{0} {}

        std::optional<GeneratedTransition> next() override {
            if (index >= transitions.size()) {
                return std::nullopt;
            }

            return transitions[index++];
        }
    };

    struct UnionTransitionIterator final : TransitionIterator {
        const NodeId parent_id;
        const TaggedState::Tag tag;
        TransitionIteratorPtr child_iter;

        UnionTransitionIterator(
                IteratorContext& context, const NodeId node_id, const TaggedState::Tag branch_tag,
                TransitionIteratorPtr child_transition_iter)
            : TransitionIterator{context}, parent_id{node_id}, tag{branch_tag},
              child_iter{std::move(child_transition_iter)} {}

        std::optional<GeneratedTransition> next() override {
            const std::optional<GeneratedTransition> child_transition = child_iter->next();
            if (!child_transition.has_value()) {
                return std::nullopt;
            }

            return GeneratedTransition{
                    child_transition->tuple,
                    GeneratedMacroState{
                            this->ctx.macro_store_ref().intern(parent_id, TaggedState{child_transition->state.id, tag}),
                            child_transition->state.accepting}};
        }
    };

    struct IdentityTransitionIterator final : TransitionIterator {
        TransitionIteratorPtr child_iter;

        IdentityTransitionIterator(IteratorContext& context, TransitionIteratorPtr child_transition_iter)
            : TransitionIterator{context}, child_iter{std::move(child_transition_iter)} {}

        std::optional<GeneratedTransition> next() override {
            const std::optional<GeneratedTransition> child_transition = child_iter->next();
            if (!child_transition.has_value()) {
                return std::nullopt;
            }

            assert(child_transition->tuple.size() == 1);
            return GeneratedTransition{
                    SymbolTuple{child_transition->tuple[0], child_transition->tuple[0]}, child_transition->state};
        }
    };

    struct ProjectTransitionIterator final : TransitionIterator {
        const ProjectPlan& plan;
        TransitionIteratorPtr child_iter;

        ProjectTransitionIterator(
                IteratorContext& context, const ProjectPlan& project_plan, TransitionIteratorPtr child_transition_iter)
            : TransitionIterator{context}, plan{project_plan}, child_iter{std::move(child_transition_iter)} {}

        std::optional<GeneratedTransition> next() override {
            const std::optional<GeneratedTransition> child_transition = child_iter->next();
            if (!child_transition.has_value()) {
                return std::nullopt;
            }

            SymbolTuple projected{};
            projected.reserve(plan.kept_levels.size());
            for (const uint8_t level : plan.kept_levels) {
                projected.push_back(child_transition->tuple[level]);
            }

            return GeneratedTransition{std::move(projected), child_transition->state};
        }
    };

    struct IntersectTransitionIterator final : TransitionIterator {
        TransitionTupleHelper& transition_tuple_helper;
        const NodeId parent_id;
        const NodeId lhs_id;
        const NodeId rhs_id;
        const MacroStateId rhs_state;
        TransitionIteratorPtr lhs_iter;
        TransitionIteratorPtr rhs_iter;
        std::optional<GeneratedTransition> current_lhs;

        IntersectTransitionIterator(
                IteratorContext& context, TransitionTupleHelper& tuple_helper, const NodeId node_id,
                const NodeId next_lhs_id, const MacroStateId lhs_state, const NodeId next_rhs_id,
                const MacroStateId next_rhs_state)
            : TransitionIterator{context}, transition_tuple_helper{tuple_helper}, parent_id{node_id},
              lhs_id{next_lhs_id}, rhs_id{next_rhs_id}, rhs_state{next_rhs_state},
              lhs_iter{this->ctx.make_transition_iterator(lhs_id, lhs_state)}, rhs_iter{}, current_lhs{} {
            advance_lhs();
        }

        std::optional<GeneratedTransition> next() override {
            while (current_lhs.has_value()) {
                while (const std::optional<GeneratedTransition> rhs_transition = rhs_iter->next()) {
                    SymbolTuple merged_tuple{};
                    if (!transition_tuple_helper.merge_visible_tuples(
                                lhs_id, current_lhs->tuple, rhs_id, rhs_transition->tuple, merged_tuple)) {
                        continue;
                    }

                    return GeneratedTransition{
                            std::move(merged_tuple),
                            GeneratedMacroState{
                                    this->ctx.macro_store_ref().intern(
                                            parent_id, PairState{current_lhs->state.id, rhs_transition->state.id}),
                                    current_lhs->state.accepting && rhs_transition->state.accepting}};
                }

                if (!advance_lhs()) {
                    break;
                }
            }

            return std::nullopt;
        }

    private:
        bool advance_lhs() {
            current_lhs = lhs_iter->next();
            if (!current_lhs.has_value()) {
                rhs_iter.reset();
                return false;
            }

            rhs_iter = this->ctx.make_transition_iterator(rhs_id, rhs_state);
            return true;
        }
    };

    struct SyncProductTransitionIterator final : TransitionIterator {
        TransitionTupleHelper& transition_tuple_helper;
        const NodeId parent_id;
        const NodeId lhs_id;
        const NodeId rhs_id;
        const MacroStateId rhs_state;
        const CompiledSyncPlan& plan;
        TransitionIteratorPtr lhs_iter;
        TransitionIteratorPtr rhs_iter;
        std::optional<GeneratedTransition> current_lhs;

        SyncProductTransitionIterator(
                IteratorContext& context, TransitionTupleHelper& tuple_helper, const NodeId node_id,
                const NodeId next_lhs_id, const MacroStateId lhs_state, const NodeId next_rhs_id,
                const MacroStateId next_rhs_state, const CompiledSyncPlan& compiled_plan)
            : TransitionIterator{context}, transition_tuple_helper{tuple_helper}, parent_id{node_id},
              lhs_id{next_lhs_id}, rhs_id{next_rhs_id}, rhs_state{next_rhs_state}, plan{compiled_plan},
              lhs_iter{this->ctx.make_transition_iterator(lhs_id, lhs_state)}, rhs_iter{}, current_lhs{} {
            advance_lhs();
        }

        std::optional<GeneratedTransition> next() override {
            while (current_lhs.has_value()) {
                while (const std::optional<GeneratedTransition> rhs_transition = rhs_iter->next()) {
                    SymbolTuple result_tuple{};
                    if (!transition_tuple_helper.build_visible_sync_result(
                                lhs_id, current_lhs->tuple, rhs_id, rhs_transition->tuple, plan, result_tuple)) {
                        continue;
                    }

                    return GeneratedTransition{
                            std::move(result_tuple),
                            GeneratedMacroState{
                                    this->ctx.macro_store_ref().intern(
                                            parent_id, PairState{current_lhs->state.id, rhs_transition->state.id}),
                                    current_lhs->state.accepting && rhs_transition->state.accepting}};
                }

                if (!advance_lhs()) {
                    break;
                }
            }

            return std::nullopt;
        }

    private:
        bool advance_lhs() {
            current_lhs = lhs_iter->next();
            if (!current_lhs.has_value()) {
                rhs_iter.reset();
                return false;
            }

            rhs_iter = this->ctx.make_transition_iterator(rhs_id, rhs_state);
            return true;
        }
    };

    struct ComplementTransitionIterator final : TransitionIterator {
        const NodeId parent_id;
        const NodeId child_id;
        const SetState& sub_states;
        SubsumptionEngine& subsumption;
        const std::vector<std::vector<mata::Symbol>>& level_symbols;
        std::vector<size_t> indices;
        SymbolTuple current_tuple;
        bool finished;

        ComplementTransitionIterator(
                IteratorContext& context, const NodeId node_id, const NodeId next_child_id,
                const SetState& child_states, SubsumptionEngine& subsumption,
                const std::vector<std::vector<mata::Symbol>>& symbols_per_level)
            : TransitionIterator{context}, parent_id{node_id}, child_id{next_child_id}, sub_states{child_states},
              subsumption{subsumption}, level_symbols{symbols_per_level}, indices(level_symbols.size(), 0),
              current_tuple(level_symbols.size(), 0), finished{false} {
            for (size_t level = 0; level < level_symbols.size(); ++level) {
                if (level_symbols[level].empty()) {
                    finished = true;
                    return;
                }

                current_tuple[level] = level_symbols[level][0];
            }
        }

        std::optional<GeneratedTransition> next() override {
            if (finished) {
                return std::nullopt;
            }

            const SymbolTuple tuple = current_tuple;
            advance_tuple();

            SetState next_sub_states{};
            next_sub_states.reserve(sub_states.size());
            bool accepting = true;

            for (const MacroStateId sub_state : sub_states) {
                TransitionIteratorPtr child_iter = this->ctx.make_transition_iterator(child_id, sub_state);
                while (const std::optional<GeneratedTransition> child_transition = child_iter->next()) {
                    if (child_transition->tuple != tuple) {
                        continue;
                    }

                    next_sub_states.push_back(child_transition->state.id);
                    accepting = accepting && !child_transition->state.accepting;
                }
            }

            subsumption.minimize(child_id, next_sub_states);
            const MacroStateId next_id = this->ctx.macro_store_ref().intern(parent_id, std::move(next_sub_states));

            return GeneratedTransition{tuple, GeneratedMacroState{next_id, accepting}};
        }

    private:
        void advance_tuple() {
            for (size_t level = indices.size(); level-- > 0;) {
                if (++indices[level] < level_symbols[level].size()) {
                    current_tuple[level] = level_symbols[level][indices[level]];
                    for (size_t reset_level = level + 1; reset_level < indices.size(); ++reset_level) {
                        indices[reset_level] = 0;
                        current_tuple[reset_level] = level_symbols[reset_level][0];
                    }
                    return;
                }
            }

            finished = true;
        }
    };

} // namespace

InitialStateIteratorPtr
make_leaf_nfa_initial_state_iterator(IteratorContext& context, const mata::nfa::Nfa& automaton) {
    return std::make_unique<LeafInitialStateIterator<mata::nfa::Nfa>>(context, automaton);
}

InitialStateIteratorPtr
make_leaf_nft_initial_state_iterator(IteratorContext& context, const mata::nft::Nft& automaton) {
    return std::make_unique<LeafInitialStateIterator<mata::nft::Nft>>(context, automaton);
}

InitialStateIteratorPtr make_union_initial_state_iterator(
        IteratorContext& context, const NodeId node_id, InitialStateIteratorPtr lhs_initial_iter,
        InitialStateIteratorPtr rhs_initial_iter) {
    return std::make_unique<UnionInitialStateIterator>(
            context, node_id, std::move(lhs_initial_iter), std::move(rhs_initial_iter));
}

InitialStateIteratorPtr make_product_initial_state_iterator(
        IteratorContext& context, const NodeId node_id, const NodeId rhs_id, InitialStateIteratorPtr lhs_initial_iter) {
    return std::make_unique<ProductInitialStateIterator>(context, node_id, rhs_id, std::move(lhs_initial_iter));
}

InitialStateIteratorPtr make_complement_initial_state_iterator(
        IteratorContext& context, const NodeId node_id, const NodeId child_id,
        InitialStateIteratorPtr child_initial_iter, SubsumptionEngine& subsumption) {
    return std::make_unique<ComplementInitialStateIterator>(
            context, node_id, child_id, std::move(child_initial_iter), subsumption);
}

InitialStateIteratorPtr
make_passthrough_initial_state_iterator(IteratorContext& context, InitialStateIteratorPtr child_initial_iter) {
    return std::make_unique<PassthroughInitialStateIterator>(context, std::move(child_initial_iter));
}

TransitionIteratorPtr make_leaf_nfa_transition_iterator(
        IteratorContext& context, const mata::nfa::Nfa& automaton, const AlphabetStore& alphabet_store,
        const NodeId node_id, const MacroStateId state) {
    return std::make_unique<LeafNfaTransitionIterator>(context, automaton, alphabet_store, node_id, state);
}

TransitionIteratorPtr make_leaf_nft_transition_iterator(
        IteratorContext& context, const mata::nft::Nft& automaton, const AlphabetStore& alphabet_store,
        const NodeId node_id, const MacroStateId state, const size_t result_arity) {
    return std::make_unique<LeafNftTransitionIterator>(
            context, automaton, alphabet_store, node_id, state, result_arity);
}

TransitionIteratorPtr
make_buffered_transition_iterator(IteratorContext& context, const std::vector<GeneratedTransition>& transitions) {
    return std::make_unique<BufferedTransitionIterator>(context, transitions);
}

TransitionIteratorPtr make_union_transition_iterator(
        IteratorContext& context, const NodeId node_id, const TaggedState::Tag branch_tag,
        TransitionIteratorPtr child_transition_iter) {
    return std::make_unique<UnionTransitionIterator>(context, node_id, branch_tag, std::move(child_transition_iter));
}

TransitionIteratorPtr
make_identity_transition_iterator(IteratorContext& context, TransitionIteratorPtr child_transition_iter) {
    return std::make_unique<IdentityTransitionIterator>(context, std::move(child_transition_iter));
}

TransitionIteratorPtr make_project_transition_iterator(
        IteratorContext& context, const ProjectPlan& project_plan, TransitionIteratorPtr child_transition_iter) {
    return std::make_unique<ProjectTransitionIterator>(context, project_plan, std::move(child_transition_iter));
}

TransitionIteratorPtr make_intersect_transition_iterator(
        IteratorContext& context, TransitionTupleHelper& tuple_helper, const NodeId node_id, const NodeId lhs_id,
        const MacroStateId lhs_state, const NodeId rhs_id, const MacroStateId rhs_state) {
    return std::make_unique<IntersectTransitionIterator>(
            context, tuple_helper, node_id, lhs_id, lhs_state, rhs_id, rhs_state);
}

TransitionIteratorPtr make_sync_product_transition_iterator(
        IteratorContext& context, TransitionTupleHelper& tuple_helper, const NodeId node_id, const NodeId lhs_id,
        const MacroStateId lhs_state, const NodeId rhs_id, const MacroStateId rhs_state,
        const CompiledSyncPlan& compiled_plan) {
    return std::make_unique<SyncProductTransitionIterator>(
            context, tuple_helper, node_id, lhs_id, lhs_state, rhs_id, rhs_state, compiled_plan);
}

TransitionIteratorPtr make_complement_transition_iterator(
        IteratorContext& context, const NodeId node_id, const NodeId child_id, const SetState& child_states,
        SubsumptionEngine& subsumption, const std::vector<std::vector<mata::Symbol>>& symbols_per_level) {
    return std::make_unique<ComplementTransitionIterator>(
            context, node_id, child_id, child_states, subsumption, symbols_per_level);
}

} // namespace mata::nft::lazy::detail
