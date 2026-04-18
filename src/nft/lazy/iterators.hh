/**
 * @file iterators.hh
 * @brief Private iterator helpers for mata::nft::lazy::detail.
 */

#pragma once

#include "alphabet_store.hh"
#include "subsumption.hh"

#include <memory>
#include <optional>
#include <vector>

namespace mata::nft::lazy::detail {

/// Visible label tuple used by the lazy transition iterators.
using SymbolTuple = std::vector<mata::Symbol>;

/// One generated successor macrostate paired with its acceptance flag.
struct GeneratedMacroState {
    MacroStateId id;
    bool accepting;
};

/// Abstract iterator over generated initial macrostates.
struct InitialStateIterator;
/// Abstract iterator over visible outgoing transitions.
struct TransitionIterator;

/// Owning pointer to an initial-state iterator.
using InitialStateIteratorPtr = std::unique_ptr<InitialStateIterator>;
/// Owning pointer to a transition iterator.
using TransitionIteratorPtr = std::unique_ptr<TransitionIterator>;

/**
 * @brief Internal sync-plan form with precomputed per-level peer lookup.
 */
struct CompiledSyncPlan {
    std::vector<uint8_t> lhs_sync_levels{};
    std::vector<uint8_t> rhs_sync_levels{};
    std::vector<LevelRef> result_layout{};
    std::vector<int16_t> lhs_sync_peer_by_level{};
    std::vector<int16_t> rhs_sync_peer_by_level{};
};

/**
 * @brief Resolved special symbols cached per exec node and level.
 */
struct ResolvedSpecialSymbols {
    enum Flag : uint8_t {
        HasEpsilon = 1U << 0,
        HasDontCare = 1U << 1,
    };

    mata::Symbol epsilon{};
    mata::Symbol dont_care{};
    uint8_t flags{0};
};

/**
 * @brief Shared tuple-compatibility helper used by transition iterators.
 */
class TransitionTupleHelper {
public:
    /**
     * @brief Construct the helper over one reconstructed exec DAG.
     * @param nodes Reconstructed exec nodes.
     * @param alphabets Resolved visible alphabets for the exec DAG.
     */
    TransitionTupleHelper(const std::vector<ExecNode>& nodes, const AlphabetStore& alphabets);

    /**
     * @brief Merge two visible tuples according to wildcard and epsilon semantics.
     * @param lhs_node_id Reconstructed left-hand exec node id.
     * @param lhs_tuple Left visible tuple.
     * @param rhs_node_id Reconstructed right-hand exec node id.
     * @param rhs_tuple Right visible tuple.
     * @param merged_tuple Output parameter receiving the merged tuple on success.
     * @return `true` if the tuples are compatible, `false` otherwise.
     */
    bool merge_visible_tuples(
            NodeId lhs_node_id, const SymbolTuple& lhs_tuple, NodeId rhs_node_id, const SymbolTuple& rhs_tuple,
            SymbolTuple& merged_tuple);

    /**
     * @brief Build one visible sync-product tuple when the synchronized levels are compatible.
     * @param lhs_node_id Reconstructed left-hand exec node id.
     * @param lhs_tuple Left visible tuple.
     * @param rhs_node_id Reconstructed right-hand exec node id.
     * @param rhs_tuple Right visible tuple.
     * @param plan Compiled sync-product layout.
     * @param result_tuple Output parameter receiving the result tuple on success.
     * @return `true` if the synchronized levels are compatible, `false` otherwise.
     */
    bool build_visible_sync_result(
            NodeId lhs_node_id, const SymbolTuple& lhs_tuple, NodeId rhs_node_id, const SymbolTuple& rhs_tuple,
            const CompiledSyncPlan& plan, SymbolTuple& result_tuple);

private:
    const std::vector<ExecNode>& nodes;
    const AlphabetStore& alphabets;
    std::vector<std::vector<ResolvedSpecialSymbols>> special_symbols_by_level;

    /**
     * @brief Populate the cached resolved special symbols for all node levels.
     */
    void initialize_special_symbol_cache();

    /**
     * @brief Resolve one special symbol inside the visible alphabet of a node level.
     * @param node_id Reconstructed exec node id.
     * @param level Visible level within the node.
     * @param special_symbol Logical special symbol to resolve.
     * @return Resolved symbol value, or `std::nullopt` when absent.
     */
    std::optional<mata::Symbol>
    resolve_special_symbol_id(NodeId node_id, uint8_t level, mata::Symbol special_symbol) const;

    /**
     * @brief Check whether a resolved symbol is the visible epsilon on one node level.
     * @param node_id Reconstructed exec node id.
     * @param level Visible level within the node.
     * @param resolved_symbol Resolved visible symbol to test.
     * @return `true` if the symbol is epsilon on the given level, `false` otherwise.
     */
    bool is_resolved_epsilon(NodeId node_id, uint8_t level, mata::Symbol resolved_symbol) const;

    /**
     * @brief Check whether a resolved symbol is the visible dont-care on one node level.
     * @param node_id Reconstructed exec node id.
     * @param level Visible level within the node.
     * @param resolved_symbol Resolved visible symbol to test.
     * @return `true` if the symbol is dont-care on the given level, `false` otherwise.
     */
    bool is_resolved_dont_care(NodeId node_id, uint8_t level, mata::Symbol resolved_symbol) const;

    /**
     * @brief Check whether a resolved symbol matches one logical special symbol on a node level.
     * @param node_id Reconstructed exec node id.
     * @param level Visible level within the node.
     * @param resolved_symbol Resolved visible symbol to test.
     * @param special_symbol Logical special symbol being queried.
     * @return `true` if the symbol matches, `false` otherwise.
     */
    bool is_resolved_special_symbol(
            NodeId node_id, uint8_t level, mata::Symbol resolved_symbol, mata::Symbol special_symbol) const;

    /**
     * @brief Merge two visible symbols under epsilon and dont-care semantics.
     * @param lhs_node_id Reconstructed left-hand exec node id.
     * @param lhs_level Visible level of the left symbol.
     * @param lhs_symbol Left resolved symbol.
     * @param rhs_node_id Reconstructed right-hand exec node id.
     * @param rhs_level Visible level of the right symbol.
     * @param rhs_symbol Right resolved symbol.
     * @param merged_symbol Output parameter receiving the merged symbol on success.
     * @return `true` if the symbols are compatible, `false` otherwise.
     */
    bool try_merge_symbols(
            NodeId lhs_node_id, uint8_t lhs_level, mata::Symbol lhs_symbol, NodeId rhs_node_id, uint8_t rhs_level,
            mata::Symbol rhs_symbol, mata::Symbol& merged_symbol) const;

    /**
     * @brief Merge two full visible tuples under epsilon and dont-care semantics.
     * @param lhs_node_id Reconstructed left-hand exec node id.
     * @param lhs_tuple Left visible tuple.
     * @param rhs_node_id Reconstructed right-hand exec node id.
     * @param rhs_tuple Right visible tuple.
     * @param merged_tuple Output parameter receiving the merged tuple on success.
     * @return `true` if the tuples are compatible, `false` otherwise.
     */
    bool try_merge_tuples(
            NodeId lhs_node_id, const SymbolTuple& lhs_tuple, NodeId rhs_node_id, const SymbolTuple& rhs_tuple,
            SymbolTuple& merged_tuple) const;

    /**
     * @brief Check compatibility of the synchronized levels of two visible tuples.
     * @param lhs_node_id Reconstructed left-hand exec node id.
     * @param lhs_tuple Left visible tuple.
     * @param lhs_levels Synchronized levels selected from the left tuple.
     * @param rhs_node_id Reconstructed right-hand exec node id.
     * @param rhs_tuple Right visible tuple.
     * @param rhs_levels Synchronized levels selected from the right tuple.
     * @return `true` if all synchronized levels are compatible, `false` otherwise.
     */
    bool sync_levels_match(
            NodeId lhs_node_id, const SymbolTuple& lhs_tuple, const std::vector<uint8_t>& lhs_levels,
            NodeId rhs_node_id, const SymbolTuple& rhs_tuple, const std::vector<uint8_t>& rhs_levels) const;

    /**
     * @brief Build the output tuple of one successful sync-product combination.
     * @param lhs_node_id Reconstructed left-hand exec node id.
     * @param lhs_tuple Left visible tuple.
     * @param rhs_node_id Reconstructed right-hand exec node id.
     * @param rhs_tuple Right visible tuple.
     * @param plan Compiled sync-product layout.
     * @param result_tuple Output parameter receiving the result tuple on success.
     * @return `true` if the tuple can be built, `false` otherwise.
     */
    bool build_sync_result_tuple(
            NodeId lhs_node_id, const SymbolTuple& lhs_tuple, NodeId rhs_node_id, const SymbolTuple& rhs_tuple,
            const CompiledSyncPlan& plan, SymbolTuple& result_tuple) const;
};

/**
 * @brief Iterator-facing services provided by the lazy emptiness context.
 */
struct IteratorContext {
    /// Virtual destructor for polymorphic use.
    virtual ~IteratorContext() = default;

    /// Access the macrostate store used for interning generated states.
    virtual MacroStateStore& macro_store_ref() = 0;
    /// Build an iterator over outgoing visible transitions of one node/state pair.
    virtual TransitionIteratorPtr make_transition_iterator(NodeId node_id, MacroStateId state) = 0;
    /// Build an iterator over initial macrostates of @p node_id.
    virtual InitialStateIteratorPtr make_initial_state_iterator(NodeId node_id) = 0;
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
 * @brief One generated visible transition paired with its successor macrostate.
 */
struct GeneratedTransition {
    SymbolTuple tuple;
    GeneratedMacroState state;
};

/**
 * @brief Polymorphic iterator over outgoing visible transitions.
 */
struct TransitionIterator {
    IteratorContext& ctx;

    /// Bind the iterator to the owning context.
    explicit TransitionIterator(IteratorContext& context) : ctx{context} {}
    /// Virtual destructor for polymorphic use.
    virtual ~TransitionIterator() = default;
    /// Return the next generated transition, or `std::nullopt` when exhausted.
    virtual std::optional<GeneratedTransition> next() = 0;
};

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
        IteratorContext& context, NodeId node_id, NodeId child_id, InitialStateIteratorPtr child_initial_iter,
        SubsumptionEngine& subsumption);

/// Build an iterator that forwards child initial states unchanged.
InitialStateIteratorPtr
make_passthrough_initial_state_iterator(IteratorContext& context, InitialStateIteratorPtr child_initial_iter);

/// Build an iterator over visible transitions of an NFA leaf.
TransitionIteratorPtr make_leaf_nfa_transition_iterator(
        IteratorContext& context, const mata::nfa::Nfa& automaton, const AlphabetStore& alphabet_store, NodeId node_id,
        MacroStateId state);

/// Build an iterator over visible transitions of an NFT leaf.
TransitionIteratorPtr make_leaf_nft_transition_iterator(
        IteratorContext& context, const mata::nft::Nft& automaton, const AlphabetStore& alphabet_store, NodeId node_id,
        MacroStateId state, size_t result_arity);

/// Build an iterator that replays already materialized transitions from a cache entry.
TransitionIteratorPtr
make_buffered_transition_iterator(IteratorContext& context, const std::vector<GeneratedTransition>& transitions);

/// Build an iterator that retags child transitions for a union node.
TransitionIteratorPtr make_union_transition_iterator(
        IteratorContext& context, NodeId node_id, TaggedState::Tag branch_tag,
        TransitionIteratorPtr child_transition_iter);

/// Build an iterator that duplicates the single visible level of an identity node.
TransitionIteratorPtr
make_identity_transition_iterator(IteratorContext& context, TransitionIteratorPtr child_transition_iter);

/// Build an iterator that projects away removed levels from child transitions.
TransitionIteratorPtr make_project_transition_iterator(
        IteratorContext& context, const ProjectPlan& project_plan, TransitionIteratorPtr child_transition_iter);

/// Build an iterator over visible transitions of an intersection node.
TransitionIteratorPtr make_intersect_transition_iterator(
        IteratorContext& context, TransitionTupleHelper& tuple_helper, NodeId node_id, NodeId lhs_id,
        MacroStateId lhs_state, NodeId rhs_id, MacroStateId rhs_state);

/// Build an iterator over visible transitions of a sync-product node.
TransitionIteratorPtr make_sync_product_transition_iterator(
        IteratorContext& context, TransitionTupleHelper& tuple_helper, NodeId node_id, NodeId lhs_id,
        MacroStateId lhs_state, NodeId rhs_id, MacroStateId rhs_state, const CompiledSyncPlan& compiled_plan);

/// Build an iterator over visible transitions of a complement node.
TransitionIteratorPtr make_complement_transition_iterator(
        IteratorContext& context, NodeId node_id, NodeId child_id, const SetState& child_states,
        SubsumptionEngine& subsumption, const std::vector<std::vector<mata::Symbol>>& symbols_per_level);

} // namespace mata::nft::lazy::detail
