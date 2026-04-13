/**
 * @file iterators.hh
 * @brief Private iterator helpers for mata::nft::lazy::detail.
 */

#pragma once

#include "transition_cache.hh"

#include <memory>
#include <optional>

namespace mata::nft::lazy::detail {

/// Abstract iterator over generated initial macrostates.
struct InitialStateIterator;
/// Abstract iterator over generated successor macrostates.
struct NextStateIterator;
/// Generic label iterator over full visible tuples.
struct LabelIterator;
/// Abstract iterator over arity-1 visible labels.
struct Arity1LabelIterator;
/// Abstract iterator over packed arity-2 visible labels.
struct Arity2LabelIterator;

/// Owning pointer to an initial-state iterator.
using InitialStateIteratorPtr = std::unique_ptr<InitialStateIterator>;
/// Owning pointer to a next-state iterator.
using NextStateIteratorPtr = std::unique_ptr<NextStateIterator>;
/// Owning pointer to a generic tuple-label iterator.
using LabelIteratorPtr = std::unique_ptr<LabelIterator>;
/// Owning pointer to an arity-1 label iterator.
using Arity1LabelIteratorPtr = std::unique_ptr<Arity1LabelIterator>;
/// Owning pointer to an arity-2 label iterator.
using Arity2LabelIteratorPtr = std::unique_ptr<Arity2LabelIterator>;

/**
 * @brief Iterator-facing services provided by the lazy emptiness context.
 */
struct IteratorContext {
    /// Virtual destructor for polymorphic use.
    virtual ~IteratorContext() = default;

    /// Access the macrostate store used for interning generated states.
    virtual MacroStateStore& macro_store_ref() = 0;
    /// Build an iterator over initial macrostates of @p node_id.
    virtual InitialStateIteratorPtr make_initial_state_iterator(NodeId node_id) = 0;
    /// Build an iterator over successors of one exact visible tuple.
    virtual NextStateIteratorPtr
    make_next_state_iterator(NodeId node_id, MacroStateId state, const SymbolTuple& tuple) = 0;
};

/**
 * @brief Polymorphic iterator over initial macrostates.
 */
struct InitialStateIterator {
    IteratorContext& ctx;

    /// Bind the iterator to the owning context.
    explicit InitialStateIterator(IteratorContext& context) : ctx{context} {}
    /// Virtual destructor for polymorphic use.
    virtual ~InitialStateIterator() = default;
    /// Return the next generated initial macrostate, or `std::nullopt` when exhausted.
    virtual std::optional<GeneratedMacroState> next() = 0;
};

/**
 * @brief Polymorphic iterator over successor macrostates.
 */
struct NextStateIterator {
    IteratorContext& ctx;

    /// Bind the iterator to the owning context.
    explicit NextStateIterator(IteratorContext& context) : ctx{context} {}
    /// Virtual destructor for polymorphic use.
    virtual ~NextStateIterator() = default;
    /// Return the next generated successor macrostate, or `std::nullopt` when exhausted.
    virtual std::optional<GeneratedMacroState> next() = 0;
};

/**
 * @brief Polymorphic iterator over generic visible label tuples.
 */
struct LabelIterator {
    /// Virtual destructor for polymorphic use.
    virtual ~LabelIterator() = default;
    /// Return the next visible tuple, or `std::nullopt` when exhausted.
    virtual std::optional<SymbolTuple> next() = 0;
};

/**
 * @brief Polymorphic iterator over arity-1 visible labels.
 */
struct Arity1LabelIterator {
    /// Virtual destructor for polymorphic use.
    virtual ~Arity1LabelIterator() = default;
    /// Return the next visible symbol, or `std::nullopt` when exhausted.
    virtual std::optional<mata::Symbol> next() = 0;
};

/**
 * @brief Polymorphic iterator over packed arity-2 visible labels.
 */
struct Arity2LabelIterator {
    /// Virtual destructor for polymorphic use.
    virtual ~Arity2LabelIterator() = default;
    /// Return the next packed arity-2 tuple, or `std::nullopt` when exhausted.
    virtual std::optional<Arity2TransitionKey> next() = 0;
};

// Generic initial-state iterators.

/// Build an iterator over initial states of an NFA leaf.
InitialStateIteratorPtr make_leaf_nfa_initial_state_iterator(IteratorContext& context, const mata::nfa::Nfa& automaton);
/// Build an iterator over initial states of an NFT leaf.
InitialStateIteratorPtr make_leaf_nft_initial_state_iterator(IteratorContext& context, const mata::nft::Nft& automaton);
/// Build an iterator over initial states of a union node.
InitialStateIteratorPtr make_union_initial_state_iterator(
        IteratorContext& context, NodeId node_id, InitialStateIteratorPtr lhs_initial_iter,
        InitialStateIteratorPtr rhs_initial_iter);
/// Build an iterator over initial states of a binary product-style node.
InitialStateIteratorPtr make_product_initial_state_iterator(
        IteratorContext& context, NodeId node_id, NodeId rhs_id, InitialStateIteratorPtr lhs_initial_iter);
/// Build an iterator over initial states of a complement node.
InitialStateIteratorPtr make_complement_initial_state_iterator(
        IteratorContext& context, NodeId node_id, InitialStateIteratorPtr child_initial_iter);
/// Build an iterator that forwards child initial states unchanged.
InitialStateIteratorPtr
make_passthrough_initial_state_iterator(IteratorContext& context, InitialStateIteratorPtr child_initial_iter);

// Generic next-state iterators.

/// Wrap an already materialized successor list as an iterator.
NextStateIteratorPtr
make_buffered_next_state_iterator(IteratorContext& context, std::vector<GeneratedMacroState> generated_states);
/// Build a lazy successor iterator for complement states.
NextStateIteratorPtr make_complement_next_state_iterator(
        IteratorContext& context, NodeId node_id, NodeId child_id, const SetState& child_states,
        SymbolTuple transition_tuple);

// Generic label iterators.

/// Build a tuple-label iterator over a materialized transition table.
LabelIteratorPtr make_transition_label_iterator(const TransitionMap& transitions);
/// Build a tuple-label iterator over the full visible universe of each level.
LabelIteratorPtr make_universe_label_iterator(std::vector<std::vector<mata::Symbol>> level_symbols);

// Fast path: arity-1 label iterators.

/// Build an arity-1 label iterator over a materialized arity-1 table.
Arity1LabelIteratorPtr make_arity1_transition_label_iterator(const Arity1TransitionMap& transitions);
/// Build an arity-1 label iterator over a fixed symbol list.
Arity1LabelIteratorPtr make_symbol_label_iterator(std::vector<mata::Symbol> symbols);
/// Adapt a tuple-label iterator to arity-1 labels.
Arity1LabelIteratorPtr make_tuple_to_arity1_label_iterator(LabelIteratorPtr child_iterator);

// Fast path: arity-2 label iterators.

/// Build an arity-2 label iterator over a materialized arity-2 table.
Arity2LabelIteratorPtr make_arity2_transition_label_iterator(const Arity2TransitionMap& transitions);
/// Adapt a tuple-label iterator to packed arity-2 labels.
Arity2LabelIteratorPtr make_tuple_to_arity2_label_iterator(LabelIteratorPtr child_iterator);

} // namespace mata::nft::lazy::detail
