/**
 * @file iterators.cc
 * @brief Private iterator helpers for mata::nft::lazy::detail.
 */

#include "iterators.hh"

#include <algorithm>
#include <cassert>
#include <limits>
#include <stdexcept>
#include <utility>
#include <version>

namespace mata::nft::lazy::detail {

namespace {
    constexpr size_t kComplementIndexedSweepThreshold = 256;

    template<typename Item, typename IteratorPtr>
    class ReplayBuffer {
    public:
        ReplayBuffer() = default;

        explicit ReplayBuffer(IteratorPtr iterator) : live_iterator{std::move(iterator)}, replayed_items{} {}

        const Item* next() {
            // Keep the first full pass so product-style iterators can replay the RHS
            // for each new LHS item without rebuilding the child iterator.
            if (replay_index < replayed_items.size()) {
                return &replayed_items[replay_index++];
            }

            if (replay_complete || live_iterator == nullptr) {
                return nullptr;
            }

            const std::optional<Item> item = live_iterator->next();
            if (!item.has_value()) {
                live_iterator.reset();
                replay_complete = true;
                return nullptr;
            }

            replayed_items.push_back(std::move(*item));
            replay_index = replayed_items.size();
            return &replayed_items.back();
        }

        void rewind() { replay_index = 0; }

    private:
        IteratorPtr live_iterator{};
        std::vector<Item> replayed_items{};
        size_t replay_index{0};
        bool replay_complete{false};
    };

    template<typename Item, typename IteratorPtr>
    class ReplayJoinCursor {
    public:
        ReplayJoinCursor(IteratorPtr lhs_iterator, IteratorPtr rhs_iterator)
            : lhs_iter{std::move(lhs_iterator)}, rhs_items{std::move(rhs_iterator)}, current_lhs{} {
            advance_lhs();
        }

        const Item* lhs() const { return current_lhs.has_value() ? &*current_lhs : nullptr; }

        const Item* next_rhs() { return rhs_items.next(); }

        bool advance_lhs() {
            // The cursor always exposes one fixed LHS item together with a replayable
            // sweep over all RHS items before it advances the LHS side.
            current_lhs = lhs_iter->next();
            if (!current_lhs.has_value()) {
                return false;
            }

            rhs_items.rewind();
            return true;
        }

    private:
        IteratorPtr lhs_iter;
        ReplayBuffer<Item, IteratorPtr> rhs_items;
        std::optional<Item> current_lhs;
    };

    template<typename Mapper>
    class MappedTransitionIterator final : public TransitionIterator {
    public:
        MappedTransitionIterator(IteratorContext& context, TransitionIteratorPtr child_transition_iter, Mapper mapper)
            : TransitionIterator{context}, child_iter{std::move(child_transition_iter)}, map{std::move(mapper)} {}

        std::optional<GeneratedTransition> next() override {
            const std::optional<GeneratedTransition> child_transition = child_iter->next();
            if (!child_transition.has_value()) {
                return std::nullopt;
            }

            return map(this->ctx, *child_transition);
        }

    private:
        TransitionIteratorPtr child_iter;
        Mapper map;
    };

    template<typename Mapper>
    TransitionIteratorPtr make_mapped_transition_iterator(
            IteratorContext& context, TransitionIteratorPtr child_transition_iter, Mapper mapper) {
        return std::make_unique<MappedTransitionIterator<Mapper>>(
                context, std::move(child_transition_iter), std::move(mapper));
    }

    // TODO: replace this with `std::mul_sat` when compile for C++26
    //       Currently Mata is compiled with C++20
    size_t saturating_multiply(const size_t lhs, const size_t rhs) noexcept {
        if (lhs == 0 || rhs == 0) {
            return 0;
        }

        constexpr size_t kMax = std::numeric_limits<size_t>::max();
        if (lhs > kMax / rhs) {
            return kMax;
        }

        return lhs * rhs;
    }

    size_t estimate_complement_tuple_sweep_cost(
            const std::vector<std::vector<mata::Symbol>>& level_symbols, const size_t child_state_count) noexcept {
        size_t universe_size = 1;
        for (const auto& symbols : level_symbols) {
            if (symbols.empty()) {
                return 0;
            }
            universe_size = saturating_multiply(universe_size, symbols.size());
        }

        return saturating_multiply(universe_size, std::max<size_t>(child_state_count, 1));
    }

} // namespace

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
        ReplayJoinCursor<GeneratedMacroState, InitialStateIteratorPtr> state_pairs;

        ProductInitialStateIterator(
                IteratorContext& context, const NodeId node_id, const NodeId next_rhs_id,
                InitialStateIteratorPtr lhs_initial_iter)
            : InitialStateIterator{context}, parent_id{node_id},
              state_pairs{std::move(lhs_initial_iter), this->ctx.make_initial_state_iterator(next_rhs_id)} {}

        std::optional<GeneratedMacroState> next() override {
            while (const GeneratedMacroState* lhs_state = state_pairs.lhs()) {
                while (const GeneratedMacroState* rhs_state = state_pairs.next_rhs()) {
                    return GeneratedMacroState{
                            this->ctx.macro_store_ref().intern(parent_id, PairState{lhs_state->id, rhs_state->id}),
                            lhs_state->accepting && rhs_state->accepting};
                }

                if (!state_pairs.advance_lhs()) {
                    break;
                }
            }

            return std::nullopt;
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
                return next_empty_transition();
            }

            // `frames` and `current_tuple` encode one DFS path through the per-level
            // NFT relation. Each emitted transition is the currently completed path.
            initialize_once();
            while (skip_exhausted_frames()) {
                if (const std::optional<GeneratedTransition> transition = try_emit_current_transition()) {
                    return transition;
                }
            }

            return std::nullopt;
        }

    private:
        std::optional<GeneratedTransition> next_empty_transition() {
            if (emitted_empty) {
                return std::nullopt;
            }

            emitted_empty = true;
            return GeneratedTransition{
                    SymbolTuple{},
                    GeneratedMacroState{static_cast<MacroStateId>(source_state), nft.final.contains(source_state)}};
        }

        void initialize_once() {
            if (!initialized) {
                initialized = true;
                push_frame(source_state);
            }
        }

        bool skip_exhausted_frames() {
            // Backtrack until the current level still has an unexplored move.
            while (!frames.empty() && frames.back().current == frames.back().end) {
                frames.pop_back();
                if (!frames.empty()) {
                    ++frames.back().current;
                }
            }

            return !frames.empty();
        }

        std::optional<GeneratedTransition> try_emit_current_transition() {
            const size_t level = frames.size() - 1;
            Frame& frame = frames.back();
            const mata::nfa::Move move = *frame.current;

            mata::Symbol resolved_symbol = 0;
            if (!try_translate_symbol(level, move.symbol, resolved_symbol)) {
                // Skip untranslatable moves while keeping the current DFS path alive.
                ++frame.current;
                return std::nullopt;
            }

            current_tuple[level] = resolved_symbol;
            if (frames.size() == arity) {
                // We have completed a full path through the NFT relation, so emit the current tuple and backtrack.
                ++frame.current;
                return GeneratedTransition{
                        current_tuple,
                        GeneratedMacroState{static_cast<MacroStateId>(move.target), nft.final.contains(move.target)}};
            }

            push_frame(move.target);
            return std::nullopt;
        }

        bool
        try_translate_symbol(const size_t level, const mata::Symbol local_symbol, mata::Symbol& resolved_symbol) const {
            return alphabets.try_translate_local_symbol_to_resolved(
                    nft, static_cast<uint8_t>(level), node_id, static_cast<uint8_t>(level), local_symbol,
                    resolved_symbol);
        }

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


    struct IntersectTransitionIterator final : TransitionIterator {
        TransitionTupleHelper& transition_tuple_helper;
        const NodeId parent_id;
        const NodeId lhs_id;
        const NodeId rhs_id;
        ReplayJoinCursor<GeneratedTransition, TransitionIteratorPtr> transition_pairs;

        IntersectTransitionIterator(
                IteratorContext& context, TransitionTupleHelper& tuple_helper, const NodeId node_id,
                const NodeId next_lhs_id, const MacroStateId lhs_state, const NodeId next_rhs_id,
                const MacroStateId next_rhs_state)
            : TransitionIterator{context}, transition_tuple_helper{tuple_helper}, parent_id{node_id},
              lhs_id{next_lhs_id}, rhs_id{next_rhs_id},
              transition_pairs{
                      this->ctx.make_transition_iterator(lhs_id, lhs_state),
                      this->ctx.make_transition_iterator(next_rhs_id, next_rhs_state)} {}

        std::optional<GeneratedTransition> next() override {
            while (const GeneratedTransition* lhs_transition = transition_pairs.lhs()) {
                while (const GeneratedTransition* rhs_transition = transition_pairs.next_rhs()) {
                    SymbolTuple merged_tuple{};
                    if (!transition_tuple_helper.merge_visible_tuples(
                                lhs_id, lhs_transition->tuple, rhs_id, rhs_transition->tuple, merged_tuple)) {
                        continue;
                    }

                    return GeneratedTransition{
                            std::move(merged_tuple),
                            GeneratedMacroState{
                                    this->ctx.macro_store_ref().intern(
                                            parent_id, PairState{lhs_transition->state.id, rhs_transition->state.id}),
                                    lhs_transition->state.accepting && rhs_transition->state.accepting}};
                }

                if (!transition_pairs.advance_lhs()) {
                    break;
                }
            }

            return std::nullopt;
        }
    };

    struct SyncProductTransitionIterator final : TransitionIterator {
        TransitionTupleHelper& transition_tuple_helper;
        const NodeId parent_id;
        const NodeId lhs_id;
        const NodeId rhs_id;
        const CompiledSyncPlan& plan;
        ReplayJoinCursor<GeneratedTransition, TransitionIteratorPtr> transition_pairs;

        SyncProductTransitionIterator(
                IteratorContext& context, TransitionTupleHelper& tuple_helper, const NodeId node_id,
                const NodeId next_lhs_id, const MacroStateId lhs_state, const NodeId next_rhs_id,
                const MacroStateId next_rhs_state, const CompiledSyncPlan& compiled_plan)
            : TransitionIterator{context}, transition_tuple_helper{tuple_helper}, parent_id{node_id},
              lhs_id{next_lhs_id}, rhs_id{next_rhs_id}, plan{compiled_plan},
              transition_pairs{
                      this->ctx.make_transition_iterator(lhs_id, lhs_state),
                      this->ctx.make_transition_iterator(next_rhs_id, next_rhs_state)} {}

        std::optional<GeneratedTransition> next() override {
            while (const GeneratedTransition* lhs_transition = transition_pairs.lhs()) {
                while (const GeneratedTransition* rhs_transition = transition_pairs.next_rhs()) {
                    SymbolTuple result_tuple{};
                    if (!transition_tuple_helper.build_visible_sync_result(
                                lhs_id, lhs_transition->tuple, rhs_id, rhs_transition->tuple, plan, result_tuple)) {
                        continue;
                    }

                    return GeneratedTransition{
                            std::move(result_tuple),
                            GeneratedMacroState{
                                    this->ctx.macro_store_ref().intern(
                                            parent_id, PairState{lhs_transition->state.id, rhs_transition->state.id}),
                                    lhs_transition->state.accepting && rhs_transition->state.accepting}};
                }

                if (!transition_pairs.advance_lhs()) {
                    break;
                }
            }

            return std::nullopt;
        }
    };

    struct ComplementTransitionIterator final : TransitionIterator {
        struct IndexedChildTransitions {
            std::vector<GeneratedTransition> transitions;
            size_t next_index{0};
        };

        const NodeId parent_id;
        const NodeId child_id;
        const SetState& sub_states;
        SubsumptionEngine& subsumption;
        const std::vector<std::vector<mata::Symbol>>& level_symbols;
        std::vector<IndexedChildTransitions> indexed_child_transitions;
        std::vector<size_t> indices;
        SymbolTuple current_tuple;
        bool use_monotonic_child_index;
        bool finished;

        ComplementTransitionIterator(
                IteratorContext& context, const NodeId node_id, const NodeId next_child_id,
                const SetState& child_states, SubsumptionEngine& subsumption,
                const std::vector<std::vector<mata::Symbol>>& symbols_per_level)
            : TransitionIterator{context}, parent_id{node_id}, child_id{next_child_id}, sub_states{child_states},
              subsumption{subsumption}, level_symbols{symbols_per_level}, indexed_child_transitions{},
              indices(level_symbols.size(), 0), current_tuple(level_symbols.size(), 0),
              use_monotonic_child_index{
                      estimate_complement_tuple_sweep_cost(symbols_per_level, child_states.size()) >=
                      kComplementIndexedSweepThreshold},
              finished{false} {
            for (size_t level = 0; level < level_symbols.size(); ++level) {
                if (level_symbols[level].empty()) {
                    finished = true;
                    return;
                }

                current_tuple[level] = level_symbols[level][0];
            }

            if (use_monotonic_child_index) {
                build_child_transition_index();
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

            collect_matching_successors(tuple, next_sub_states, accepting);

            subsumption.minimize(child_id, next_sub_states);
            const MacroStateId next_id = this->ctx.macro_store_ref().intern(parent_id, std::move(next_sub_states));

            return GeneratedTransition{tuple, GeneratedMacroState{next_id, accepting}};
        }

    private:
        void collect_matching_successors(const SymbolTuple& tuple, SetState& next_sub_states, bool& accepting) {
            // Complement enumerates tuples in lexicographic order. When the visible
            // universe is large enough, a monotonic per-child sweep avoids rescanning
            // already smaller child tuples for every next complement tuple.
            if (use_monotonic_child_index) {
                collect_matching_successors_indexed(tuple, next_sub_states, accepting);
            } else {
                collect_matching_successors_direct(tuple, next_sub_states, accepting);
            }
        }

        void build_child_transition_index() {
            indexed_child_transitions.reserve(sub_states.size());

            for (const MacroStateId sub_state : sub_states) {
                IndexedChildTransitions indexed_child{};
                TransitionIteratorPtr child_iter = this->ctx.make_transition_iterator(child_id, sub_state);
                while (const std::optional<GeneratedTransition> child_transition = child_iter->next()) {
                    indexed_child.transitions.push_back(std::move(*child_transition));
                }

                std::sort(
                        indexed_child.transitions.begin(), indexed_child.transitions.end(),
                        [](const GeneratedTransition& lhs, const GeneratedTransition& rhs) {
                            return lhs.tuple < rhs.tuple;
                        });
                indexed_child_transitions.push_back(std::move(indexed_child));
            }
        }

        void collect_matching_successors_direct(const SymbolTuple& tuple, SetState& next_sub_states, bool& accepting) {
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
        }

        void collect_matching_successors_indexed(const SymbolTuple& tuple, SetState& next_sub_states, bool& accepting) {
            for (IndexedChildTransitions& indexed_child : indexed_child_transitions) {
                while (indexed_child.next_index < indexed_child.transitions.size() &&
                       indexed_child.transitions[indexed_child.next_index].tuple < tuple) {
                    ++indexed_child.next_index;
                }

                size_t match_index = indexed_child.next_index;
                while (match_index < indexed_child.transitions.size() &&
                       !(tuple < indexed_child.transitions[match_index].tuple) &&
                       !(indexed_child.transitions[match_index].tuple < tuple)) {
                    next_sub_states.push_back(indexed_child.transitions[match_index].state.id);
                    accepting = accepting && !indexed_child.transitions[match_index].state.accepting;
                    ++match_index;
                }

                indexed_child.next_index = match_index;
            }
        }

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
    return make_mapped_transition_iterator(
            context, std::move(child_transition_iter),
            [node_id, branch_tag](IteratorContext& iterator_context, const GeneratedTransition& child_transition) {
                return GeneratedTransition{
                        child_transition.tuple,
                        GeneratedMacroState{
                                iterator_context.macro_store_ref().intern(
                                        node_id, TaggedState{child_transition.state.id, branch_tag}),
                                child_transition.state.accepting}};
            });
}

TransitionIteratorPtr
make_identity_transition_iterator(IteratorContext& context, TransitionIteratorPtr child_transition_iter) {
    return make_mapped_transition_iterator(
            context, std::move(child_transition_iter),
            [](IteratorContext&, const GeneratedTransition& child_transition) {
                assert(child_transition.tuple.size() == 1);
                return GeneratedTransition{
                        SymbolTuple{child_transition.tuple[0], child_transition.tuple[0]}, child_transition.state};
            });
}

TransitionIteratorPtr make_project_transition_iterator(
        IteratorContext& context, const ProjectPlan& project_plan, TransitionIteratorPtr child_transition_iter) {
    return make_mapped_transition_iterator(
            context, std::move(child_transition_iter),
            [&project_plan](IteratorContext&, const GeneratedTransition& child_transition) {
                SymbolTuple projected{};
                projected.reserve(project_plan.kept_levels.size());
                for (const uint8_t level : project_plan.kept_levels) {
                    projected.push_back(child_transition.tuple[level]);
                }

                return GeneratedTransition{std::move(projected), child_transition.state};
            });
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
