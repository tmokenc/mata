/**
 * @file iterators.hh
 * @brief Private iterator helpers for mata::nft::lazy::detail.
 */

#pragma once

#include "transition_cache.hh"

#include <memory>
#include <optional>

namespace mata::nft::lazy::detail {

struct InitialStateIterator;
struct NextStateIterator;
struct LabelIterator;
struct Arity1LabelIterator;
struct Arity2LabelIterator;

using InitialStateIteratorPtr = std::unique_ptr<InitialStateIterator>;
using NextStateIteratorPtr = std::unique_ptr<NextStateIterator>;
using LabelIteratorPtr = std::unique_ptr<LabelIterator>;
using Arity1LabelIteratorPtr = std::unique_ptr<Arity1LabelIterator>;
using Arity2LabelIteratorPtr = std::unique_ptr<Arity2LabelIterator>;

struct IteratorContext {
    virtual ~IteratorContext() = default;

    virtual MacroStateStore& macro_store_ref() = 0;
    virtual InitialStateIteratorPtr make_initial_state_iterator(NodeId node_id) = 0;
    virtual NextStateIteratorPtr
    make_next_state_iterator(NodeId node_id, MacroStateId state, const SymbolTuple& tuple) = 0;
};

struct InitialStateIterator {
    IteratorContext& ctx;

    explicit InitialStateIterator(IteratorContext& context) : ctx{context} {}
    virtual ~InitialStateIterator() = default;
    virtual std::optional<GeneratedMacroState> next() = 0;
};

struct NextStateIterator {
    IteratorContext& ctx;

    explicit NextStateIterator(IteratorContext& context) : ctx{context} {}
    virtual ~NextStateIterator() = default;
    virtual std::optional<GeneratedMacroState> next() = 0;
};

struct LabelIterator {
    virtual ~LabelIterator() = default;
    virtual std::optional<SymbolTuple> next() = 0;
};

struct Arity1LabelIterator {
    virtual ~Arity1LabelIterator() = default;
    virtual std::optional<mata::Symbol> next() = 0;
};

struct Arity2LabelIterator {
    virtual ~Arity2LabelIterator() = default;
    virtual std::optional<Arity2TransitionKey> next() = 0;
};

InitialStateIteratorPtr make_leaf_nfa_initial_state_iterator(
        IteratorContext& context, const mata::nfa::Nfa& automaton);
InitialStateIteratorPtr make_leaf_nft_initial_state_iterator(
        IteratorContext& context, const mata::nft::Nft& automaton);
InitialStateIteratorPtr make_union_initial_state_iterator(
        IteratorContext& context, NodeId node_id, InitialStateIteratorPtr lhs_initial_iter,
        InitialStateIteratorPtr rhs_initial_iter);
InitialStateIteratorPtr make_product_initial_state_iterator(
        IteratorContext& context, NodeId node_id, NodeId rhs_id, InitialStateIteratorPtr lhs_initial_iter);
InitialStateIteratorPtr make_complement_initial_state_iterator(
        IteratorContext& context, NodeId node_id, InitialStateIteratorPtr child_initial_iter);
InitialStateIteratorPtr
make_passthrough_initial_state_iterator(IteratorContext& context, InitialStateIteratorPtr child_initial_iter);

NextStateIteratorPtr
make_buffered_next_state_iterator(IteratorContext& context, std::vector<GeneratedMacroState> generated_states);
NextStateIteratorPtr make_complement_next_state_iterator(
        IteratorContext& context, NodeId node_id, NodeId child_id, const SetState& child_states,
        SymbolTuple transition_tuple);

LabelIteratorPtr make_transition_label_iterator(const TransitionMap& transitions);
LabelIteratorPtr make_universe_label_iterator(std::vector<std::vector<mata::Symbol>> level_symbols);
Arity1LabelIteratorPtr make_arity1_transition_label_iterator(const Arity1TransitionMap& transitions);
Arity1LabelIteratorPtr make_symbol_label_iterator(std::vector<mata::Symbol> symbols);
Arity1LabelIteratorPtr make_tuple_to_arity1_label_iterator(LabelIteratorPtr child_iterator);
Arity2LabelIteratorPtr make_arity2_transition_label_iterator(const Arity2TransitionMap& transitions);
Arity2LabelIteratorPtr make_tuple_to_arity2_label_iterator(LabelIteratorPtr child_iterator);

} // namespace mata::nft::lazy::detail
