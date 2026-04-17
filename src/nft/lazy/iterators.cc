/**
 * @file iterators.cc
 * @brief Private iterator helpers for mata::nft::lazy::detail.
 */

#include "iterators.hh"

#include <utility>
#include "subsumption.hh"

namespace mata::nft::lazy::detail {

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
                sub_initial_states.insert(sub_initial_state->id);
                accepting = accepting && !sub_initial_state->accepting;
            }

            subsumption.minimize(child_id, sub_initial_states);
            MacroStateId next_id = this->ctx.macro_store_ref().intern(parent_id, std::move(sub_initial_states));

            return GeneratedMacroState{next_id, accepting};
        }
    };

    struct PassthroughInitialStateIterator final : InitialStateIterator {
        InitialStateIteratorPtr child_iter;

        PassthroughInitialStateIterator(IteratorContext& context, InitialStateIteratorPtr child_initial_iter)
            : InitialStateIterator{context}, child_iter{std::move(child_initial_iter)} {}

        std::optional<GeneratedMacroState> next() override { return child_iter->next(); }
    };

    struct BufferedNextStateIterator final : NextStateIterator {
        std::vector<GeneratedMacroState> states;
        size_t index;

        BufferedNextStateIterator(IteratorContext& context, std::vector<GeneratedMacroState> generated_states)
            : NextStateIterator{context}, states{std::move(generated_states)}, index{0} {}

        std::optional<GeneratedMacroState> next() override {
            if (index >= states.size()) {
                return std::nullopt;
            }

            return states[index++];
        }
    };

    struct ComplementNextStateIterator final : NextStateIterator {
        const NodeId parent_id;
        const NodeId child_id;
        const SetState& sub_states;
        const SymbolTuple tuple;
        SubsumptionEngine& subsumption;
        bool emitted;

        ComplementNextStateIterator(
                IteratorContext& context, const NodeId node_id, const NodeId next_child_id,
                const SetState& child_states, SymbolTuple transition_tuple, SubsumptionEngine& subsumption)
            : NextStateIterator{context}, parent_id{node_id}, child_id{next_child_id}, sub_states{child_states},
              tuple{std::move(transition_tuple)}, subsumption{subsumption}, emitted{false} {}

        std::optional<GeneratedMacroState> next() override {
            if (emitted) {
                return std::nullopt;
            }
            emitted = true;

            SetState next_sub_states{};
            next_sub_states.reserve(sub_states.size());
            bool accepting = true;

            for (const MacroStateId sub_state : sub_states) {
                NextStateIteratorPtr child_iter = this->ctx.make_next_state_iterator(child_id, sub_state, tuple);
                while (true) {
                    const std::optional<GeneratedMacroState> child_next_state = child_iter->next();
                    if (!child_next_state.has_value()) {
                        break;
                    }

                    next_sub_states.insert(child_next_state->id);
                    accepting = accepting && !child_next_state->accepting;
                }
            }

            subsumption.minimize(child_id, next_sub_states);

            MacroStateId next_id = this->ctx.macro_store_ref().intern(parent_id, std::move(next_sub_states));

            return GeneratedMacroState{next_id, accepting};
        }
    };

    struct TransitionLabelIterator final : LabelIterator {
        TransitionMap::const_iterator current;
        TransitionMap::const_iterator end;

        explicit TransitionLabelIterator(const TransitionMap& transitions)
            : current{transitions.begin()}, end{transitions.end()} {}

        std::optional<SymbolTuple> next() override {
            if (current == end) {
                return std::nullopt;
            }

            return current++->first;
        }
    };

    struct UniverseLabelIterator final : LabelIterator {
        std::vector<std::vector<mata::Symbol>> level_symbols;
        std::vector<size_t> indices;
        SymbolTuple current_tuple;
        bool finished;

        explicit UniverseLabelIterator(std::vector<std::vector<mata::Symbol>> symbols_per_level)
            : level_symbols{std::move(symbols_per_level)}, indices(level_symbols.size(), 0),
              current_tuple(level_symbols.size(), 0), finished{false} {
            for (size_t level = 0; level < level_symbols.size(); ++level) {
                if (level_symbols[level].empty()) {
                    finished = true;
                    return;
                }

                current_tuple[level] = level_symbols[level][0];
            }
        }

        std::optional<SymbolTuple> next() override {
            if (finished) {
                return std::nullopt;
            }

            const SymbolTuple tuple = current_tuple;
            advance();
            return tuple;
        }

    private:
        void advance() {
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

    struct Arity1TransitionLabelIterator final : Arity1LabelIterator {
        Arity1TransitionMap::const_iterator current;
        Arity1TransitionMap::const_iterator end;

        explicit Arity1TransitionLabelIterator(const Arity1TransitionMap& transitions)
            : current{transitions.begin()}, end{transitions.end()} {}

        std::optional<mata::Symbol> next() override {
            if (current == end) {
                return std::nullopt;
            }

            return current++->first;
        }
    };

    struct SymbolLabelIterator final : Arity1LabelIterator {
        std::vector<mata::Symbol> symbols;
        size_t index;

        explicit SymbolLabelIterator(std::vector<mata::Symbol> label_symbols)
            : symbols{std::move(label_symbols)}, index{0} {}

        std::optional<mata::Symbol> next() override {
            if (index >= symbols.size()) {
                return std::nullopt;
            }

            return symbols[index++];
        }
    };

    struct TupleToArity1LabelIterator final : Arity1LabelIterator {
        LabelIteratorPtr child_iterator;

        explicit TupleToArity1LabelIterator(LabelIteratorPtr child) : child_iterator{std::move(child)} {}

        std::optional<mata::Symbol> next() override {
            const std::optional<SymbolTuple> tuple = child_iterator->next();
            if (!tuple.has_value()) {
                return std::nullopt;
            }

            assert(tuple->size() == 1);
            return (*tuple)[0];
        }
    };

    struct Arity2TransitionLabelIterator final : Arity2LabelIterator {
        Arity2TransitionMap::const_iterator current;
        Arity2TransitionMap::const_iterator end;

        explicit Arity2TransitionLabelIterator(const Arity2TransitionMap& transitions)
            : current{transitions.begin()}, end{transitions.end()} {}

        std::optional<Arity2TransitionKey> next() override {
            if (current == end) {
                return std::nullopt;
            }

            return current++->first;
        }
    };

    struct TupleToArity2LabelIterator final : Arity2LabelIterator {
        LabelIteratorPtr child_iterator;

        explicit TupleToArity2LabelIterator(LabelIteratorPtr child) : child_iterator{std::move(child)} {}

        std::optional<Arity2TransitionKey> next() override {
            const std::optional<SymbolTuple> tuple = child_iterator->next();
            if (!tuple.has_value()) {
                return std::nullopt;
            }

            assert(tuple->size() == 2);
            return pack_arity2_tuple(*tuple);
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

NextStateIteratorPtr
make_buffered_next_state_iterator(IteratorContext& context, std::vector<GeneratedMacroState> generated_states) {
    return std::make_unique<BufferedNextStateIterator>(context, std::move(generated_states));
}

NextStateIteratorPtr make_complement_next_state_iterator(
        IteratorContext& context, const NodeId node_id, const NodeId child_id, const SetState& child_states,
        SymbolTuple transition_tuple, SubsumptionEngine& subsumption) {
    return std::make_unique<ComplementNextStateIterator>(
            context, node_id, child_id, child_states, std::move(transition_tuple), subsumption);
}

LabelIteratorPtr make_transition_label_iterator(const TransitionMap& transitions) {
    return std::make_unique<TransitionLabelIterator>(transitions);
}

LabelIteratorPtr make_universe_label_iterator(std::vector<std::vector<mata::Symbol>> level_symbols) {
    return std::make_unique<UniverseLabelIterator>(std::move(level_symbols));
}

Arity1LabelIteratorPtr make_arity1_transition_label_iterator(const Arity1TransitionMap& transitions) {
    return std::make_unique<Arity1TransitionLabelIterator>(transitions);
}

Arity1LabelIteratorPtr make_symbol_label_iterator(std::vector<mata::Symbol> symbols) {
    return std::make_unique<SymbolLabelIterator>(std::move(symbols));
}

Arity1LabelIteratorPtr make_tuple_to_arity1_label_iterator(LabelIteratorPtr child_iterator) {
    return std::make_unique<TupleToArity1LabelIterator>(std::move(child_iterator));
}

Arity2LabelIteratorPtr make_arity2_transition_label_iterator(const Arity2TransitionMap& transitions) {
    return std::make_unique<Arity2TransitionLabelIterator>(transitions);
}

Arity2LabelIteratorPtr make_tuple_to_arity2_label_iterator(LabelIteratorPtr child_iterator) {
    return std::make_unique<TupleToArity2LabelIterator>(std::move(child_iterator));
}

} // namespace mata::nft::lazy::detail
