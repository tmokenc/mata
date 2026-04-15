/**
 * @file transition_cache.hh
 * @brief Private transition-cache declarations for mata::nft::lazy::detail.
 */

#pragma once

#include "alphabet_store.hh"
#include "macrostate_store.hh"
#include "symbols.hh"

#include <algorithm>
#include <cassert>
#include <optional>
#include <unordered_map>
#include <vector>

namespace mata::nft::lazy::detail {

/**
 * @brief One generated successor macrostate paired with its acceptance flag.
 */
struct GeneratedMacroState {
    MacroStateId id;
    bool accepting;
};

/**
 * @brief Cached special resolved symbols for one node level.
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

// Generic transition tables.

/// Generic visible-transition cache map keyed by full tuples.
using TransitionMap = std::unordered_map<SymbolTuple, std::vector<GeneratedMacroState>, SymbolTupleHash>;

/**
 * @brief Cached metadata for generic visible-transition maps.
 */
struct GenericTransitionCacheEntry {
    TransitionMap transitions{};
    std::vector<uint8_t> has_epsilon_by_level{};
    std::vector<uint8_t> has_dont_care_by_level{};
};

/**
 * @brief Sorted immutable transition table used by arity-specialized caches.
 * @tparam Key Visible-label key type.
 */
template<typename Key>
class FlatTransitionTable {
public:
    using value_type = std::pair<Key, std::vector<GeneratedMacroState>>;
    using Storage = std::vector<value_type>;
    using const_iterator = Storage::const_iterator;

    FlatTransitionTable() = default;

    /// Freeze an unordered builder map into sorted lookup storage.
    static FlatTransitionTable freeze(std::unordered_map<Key, std::vector<GeneratedMacroState>>&& builder) {
        Storage entries{};
        entries.reserve(builder.size());
        for (auto& [key, states] : builder) {
            entries.emplace_back(key, std::move(states));
        }

        std::sort(entries.begin(), entries.end(), [](const value_type& lhs, const value_type& rhs) {
            return lhs.first < rhs.first;
        });

        return FlatTransitionTable{std::move(entries)};
    }

    /// Number of distinct labels stored in the table.
    size_t size() const noexcept { return entries_.size(); }
    /// Return whether the table has no labels.
    bool empty() const noexcept { return entries_.empty(); }

    /// Iterator to the first stored transition bucket.
    const_iterator begin() const noexcept { return entries_.begin(); }
    /// Iterator past the last stored transition bucket.
    const_iterator end() const noexcept { return entries_.end(); }

    /// Find one label bucket, or return `end()` when absent.
    const_iterator find(const Key& key) const noexcept {
        const auto it = std::lower_bound(
                entries_.begin(), entries_.end(), key,
                [](const value_type& entry, const Key& target_key) { return entry.first < target_key; });
        if (it == entries_.end() || it->first != key) {
            return entries_.end();
        }
        return it;
    }

private:
    explicit FlatTransitionTable(Storage&& entries) : entries_{std::move(entries)} {}

    Storage entries_{};
};

// Fast path: arity-1 transition tables.

/// Builder map for arity-1 transition tables.
using Arity1TransitionBuilder = std::unordered_map<mata::Symbol, std::vector<GeneratedMacroState>>;
/// Frozen arity-1 transition table keyed by visible symbols.
using Arity1TransitionMap = FlatTransitionTable<mata::Symbol>;

// Fast path: arity-2 transition tables.

/// Packed key used by the specialized arity-2 transition cache.
using Arity2TransitionKey = uint64_t;
/// Builder map for arity-2 transition tables.
using Arity2TransitionBuilder = std::unordered_map<Arity2TransitionKey, std::vector<GeneratedMacroState>>;
/// Frozen arity-2 transition table keyed by packed symbol pairs.
using Arity2TransitionMap = FlatTransitionTable<Arity2TransitionKey>;

/// Pack two visible symbols into one arity-2 transition key.
inline constexpr Arity2TransitionKey pack_arity2_symbols(const mata::Symbol first, const mata::Symbol second) noexcept {
    return (static_cast<uint64_t>(first) << 32) | static_cast<uint64_t>(second);
}

/// Pack a 2-tuple into one arity-2 transition key.
inline Arity2TransitionKey pack_arity2_tuple(const SymbolTuple& tuple) {
    assert(tuple.size() == 2);
    return pack_arity2_symbols(tuple[0], tuple[1]);
}

/// Extract the first symbol from a packed arity-2 transition key.
inline constexpr mata::Symbol arity2_first_symbol(const Arity2TransitionKey tuple) noexcept {
    return static_cast<mata::Symbol>(tuple >> 32);
}

/// Extract the second symbol from a packed arity-2 transition key.
inline constexpr mata::Symbol arity2_second_symbol(const Arity2TransitionKey tuple) noexcept {
    return static_cast<mata::Symbol>(tuple & 0xffffffffULL);
}

/// Unpack an arity-2 transition key back into a generic tuple.
inline SymbolTuple unpack_arity2_tuple(const Arity2TransitionKey tuple) {
    return SymbolTuple{arity2_first_symbol(tuple), arity2_second_symbol(tuple)};
}

/**
 * @brief Cached metadata for specialized arity-2 visible-transition tables.
 */
struct Arity2TransitionCacheEntry {
    Arity2TransitionMap transitions{};
    bool first_has_epsilon{ false };
    bool second_has_epsilon{ false };
    bool first_has_dont_care{ false };
    bool second_has_dont_care{ false };
};

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
 * @brief References needed by the transition cache.
 */
struct TransitionCacheContext {
    const std::vector<mata::nfa::Nfa>& nfas;
    const std::vector<mata::nft::Nft>& nfts;
    const std::vector<CompiledSyncPlan>& sync_plans;
    const std::vector<ProjectPlan>& project_plans;
    const std::vector<ExecNode>& nodes;
    MacroStateStore& macro_store;
    const AlphabetStore& alphabets;
};

/**
 * @brief Recursive resolver interface used by transition-cache recursion.
 */
class TransitionResolver {
public:
    /// Virtual destructor for polymorphic use.
    virtual ~TransitionResolver() = default;

    // Generic transitions.

    /// Resolve generic visible transitions for one child, materializing into @p fallback when needed.
    virtual const TransitionMap& resolve_visible(NodeId node_id, MacroStateId state, TransitionMap& fallback) const = 0;

    // Fast path: arity-1 transitions.

    /// Resolve arity-1 visible transitions for one child, materializing into @p fallback when needed.
    virtual const Arity1TransitionMap&
    resolve_arity1_visible(NodeId node_id, MacroStateId state, Arity1TransitionMap& fallback) const = 0;

    // Fast path: arity-2 transitions.

    /// Resolve arity-2 visible transitions for one child, materializing into @p fallback when needed.
    virtual const Arity2TransitionMap&
    resolve_arity2_visible(NodeId node_id, MacroStateId state, Arity2TransitionMap& fallback) const = 0;
};

/**
 * @brief Memoized transition generator for reconstructed lazy exec nodes.
 */
class TransitionCache {
public:
    /// Construct the transition cache over one reconstructed exec DAG.
    explicit TransitionCache(const TransitionCacheContext& context);

    // Fast path: arity-1 transitions.

    /// Get or build the arity-1 visible transition table for one state.
    const Arity1TransitionMap&
    get_arity1_visible_transitions(NodeId node_id, MacroStateId state, const TransitionResolver& resolver);

    // Fast path: arity-2 transitions.

    /// Get or build the arity-2 visible transition table for one state.
    const Arity2TransitionMap&
    get_arity2_visible_transitions(NodeId node_id, MacroStateId state, const TransitionResolver& resolver);

    // Generic transitions.

    /// Get or build the generic visible transition map for one state.
    const TransitionMap&
    get_visible_transitions(NodeId node_id, MacroStateId state, const TransitionResolver& resolver);

private:
    using Nfa = mata::nfa::Nfa;
    using Nft = mata::nft::Nft;
    using State = mata::nfa::State;

    const std::vector<Nfa>& nfas;
    const std::vector<Nft>& nfts;
    const std::vector<CompiledSyncPlan>& sync_plans;
    const std::vector<ProjectPlan>& project_plans;
    const std::vector<ExecNode>& nodes;
    MacroStateStore& macro_store;
    const AlphabetStore& alphabets;
    std::vector<std::vector<ResolvedSpecialSymbols>> special_symbols_by_level;

    std::unordered_map<uint64_t, GenericTransitionCacheEntry> visible_transition_cache;
    std::unordered_map<uint64_t, Arity1TransitionMap> arity1_visible_transition_cache;
    std::unordered_map<uint64_t, Arity2TransitionCacheEntry> arity2_visible_transition_cache;

    /// Combine node/state identity into one cache key.
    static constexpr uint64_t state_cache_key(NodeId node_id, MacroStateId state) noexcept {
        return (static_cast<uint64_t>(node_id) << 32) | static_cast<uint64_t>(state);
    }

    void initialize_special_symbol_cache();
    std::optional<mata::Symbol> resolve_special_symbol_id(NodeId node_id, uint8_t level, mata::Symbol special_symbol) const;
    bool is_resolved_epsilon(NodeId node_id, uint8_t level, mata::Symbol resolved_symbol) const;
    bool is_resolved_dont_care(NodeId node_id, uint8_t level, mata::Symbol resolved_symbol) const;
    std::vector<uint8_t> collect_special_symbol_levels(
            NodeId node_id, const TransitionMap& transitions, mata::Symbol special_symbol) const;
    Arity2TransitionCacheEntry build_arity2_cache_entry(NodeId node_id, Arity2TransitionBuilder&& transitions) const;
    GenericTransitionCacheEntry build_generic_cache_entry(NodeId node_id, TransitionMap&& transitions) const;

    bool is_resolved_special_symbol(
            NodeId node_id, uint8_t level, mata::Symbol resolved_symbol, mata::Symbol special_symbol) const;
    bool tuple_has_special_symbol_on_levels(
            NodeId node_id, const SymbolTuple& tuple, const std::vector<uint8_t>& levels,
            mata::Symbol special_symbol) const;
    bool tuple_has_special_symbol(NodeId node_id, const SymbolTuple& tuple, mata::Symbol special_symbol) const;
    bool transition_map_has_special_symbol_on_levels(
            NodeId node_id, MacroStateId state, const TransitionMap& transitions, const std::vector<uint8_t>& levels,
            mata::Symbol special_symbol) const;
    bool transition_map_has_special_symbol(
            NodeId node_id, MacroStateId state, const TransitionMap& transitions, mata::Symbol special_symbol) const;
    bool arity2_transition_map_has_special_symbol(
            NodeId node_id, MacroStateId state, const Arity2TransitionMap& transitions, mata::Symbol special_symbol) const;
    bool try_merge_symbols(
            NodeId lhs_node_id, uint8_t lhs_level, mata::Symbol lhs_symbol, NodeId rhs_node_id, uint8_t rhs_level,
            mata::Symbol rhs_symbol, mata::Symbol& merged_symbol) const;
    bool try_merge_tuples(
            NodeId lhs_node_id, const SymbolTuple& lhs_tuple, NodeId rhs_node_id, const SymbolTuple& rhs_tuple,
            SymbolTuple& merged_tuple) const;
    bool try_merge_arity2_keys(
            NodeId lhs_node_id, Arity2TransitionKey lhs_tuple, NodeId rhs_node_id, Arity2TransitionKey rhs_tuple,
            Arity2TransitionKey& merged_tuple) const;
    bool sync_levels_match(
            NodeId lhs_node_id, const SymbolTuple& lhs_tuple, const std::vector<uint8_t>& lhs_levels,
            NodeId rhs_node_id, const SymbolTuple& rhs_tuple, const std::vector<uint8_t>& rhs_levels) const;
    bool build_sync_result_tuple(
            NodeId lhs_node_id, const SymbolTuple& lhs_tuple, NodeId rhs_node_id, const SymbolTuple& rhs_tuple,
            const CompiledSyncPlan& plan, SymbolTuple& result_tuple) const;
    /// Recursively enumerate generic NFT leaf transitions.
    void build_leaf_nft_transitions(
            NodeId node_id, const Nft& nft, State source_state, SymbolTuple& current_tuple, size_t next_level,
            TransitionMap& transitions);
    /// Recursively enumerate specialized arity-2 NFT leaf transitions.
    void build_leaf_arity2_nft_transitions(
            NodeId node_id, const Nft& nft, State source_state, mata::Symbol first_symbol, mata::Symbol second_symbol,
            size_t next_level, Arity2TransitionBuilder& transitions);
};

} // namespace mata::nft::lazy::detail
