/**
 * @file state_types.hh
 * @brief Shared private state and hashing helpers for mata::nft::lazy::detail.
 */

#pragma once

#include "mata/nft/lazy.hh"
#include "mata/utils/ord-vector.hh"

#include <cstdint>

namespace mata::nft::lazy::detail {

/// DFS visitation state used during lazy DAG traversals.
enum class VisitState : uint8_t {
    Unseen = 0,
    Active = 1,
    Done = 2,
};

/// Reconstructed exec-node kinds used by the lazy runtime.
enum class ExecKind : uint8_t {
    // Generic execution kinds.

    LeafNfa = 0,
    LeafNft,
    Union,
    Intersect,
    Complement,
    Identity,
    Project,
    SyncProduct,

    // Fast path: arity-1 execution kinds.

    Arity1Union,
    Arity1Intersect,
    Arity1Complement,

    // Fast path: arity-2 execution kinds.

    Arity2LeafNft,
    Arity2Union,
    Arity2Intersect,
    Arity2Complement,
    Arity2Project,
    Arity2SyncProduct,
};

/**
 * @brief Compact reconstructed execution node.
 */
struct ExecNode {
    /// Specialized runtime operator kind.
    ExecKind kind;
    /// Result arity of the reconstructed node.
    uint8_t result_arity;
    /// Left child or leaf index.
    NodeId lhs;
    /// Right child when present.
    NodeId rhs;
    /// Index into auxiliary plan tables when needed.
    uint32_t payload;
};

/// Return whether @p kind belongs to the specialized arity-1 fast path.
constexpr bool is_arity1_exec_kind(const ExecKind kind) noexcept {
    return kind == ExecKind::LeafNfa || kind == ExecKind::Arity1Union || kind == ExecKind::Arity1Intersect ||
           kind == ExecKind::Arity1Complement;
}

/**
 * @brief Mix one integer into a stable 64-bit hash state.
 *
 * This follows the SplitMix64-style mixing step: `0x9e3779b97f4a7c15` is the
 * `GOLDEN_GAMMA` increment from Steele, Lea, and Flood's "Fast Splittable
 * Pseudorandom Number Generators" (OOPSLA 2014), while
 * `0xbf58476d1ce4e5b9` and `0x94d049bb133111eb` are the constants from David
 * Stafford's "variant 13" 64-bit mixer, which uses for bit diffusion.
 *
 * @param value Input integer value.
 * @return Mixed 64-bit hash value.
 */
constexpr uint64_t mix_hash64(uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

/**
 * @brief Fold a 64-bit hash down to the 32-bit ids used by the macrostate stores.
 * @param value 64-bit hash value.
 * @return Folded 32-bit hash value.
 */
constexpr uint32_t fold_hash64(uint64_t value) noexcept { return static_cast<uint32_t>(value ^ (value >> 32)); }

/**
 * @brief Pair macrostate stored for binary product-like nodes.
 */
struct PairState {
    MacroStateId lhs;
    MacroStateId rhs;
};

/**
 * @brief Tagged macrostate stored for union-like nodes.
 */
struct TaggedState {
    /// Side tag of the union branch represented by the macrostate.
    enum class Tag : uint8_t {
        Left = 0,
        Right = 1,
    };

    MacroStateId state;
    Tag tag;
};

/// Complement subset macrostate kept in sorted canonical order.
using SetState = mata::utils::OrdVector<MacroStateId>;

/**
 * @brief Canonicalize one subset macrostate in-place.
 *
 * The canonical representation is a sorted vector without duplicates.
 *
 * @param states Subset macrostate to canonicalize.
 */
inline void canonicalize_set_state(SetState& states) { states = SetState{states.begin(), states.end()}; }

/**
 * @brief Hash a subset macrostate.
 * @param states Subset macrostate to hash.
 * @return Hash value used for interning.
 */
inline uint32_t hash_states(const SetState& states) {
    uint64_t hash = mix_hash64(states.size());
    for (const MacroStateId& id : states) {
        hash ^= mix_hash64(id);
    }
    return fold_hash64(mix_hash64(hash));
}

/**
 * @brief Hash a binary-product macrostate.
 * @param pair Pair macrostate to hash.
 * @return Hash value used for interning.
 */
constexpr uint32_t hash_pair(const PairState& pair) noexcept {
    const uint64_t packed = (static_cast<uint64_t>(pair.lhs) << 32) | static_cast<uint64_t>(pair.rhs);
    return fold_hash64(mix_hash64(packed));
}

/**
 * @brief Hash a tagged union macrostate.
 * @param tagged Tagged macrostate to hash.
 * @return Hash value used for interning.
 */
constexpr uint32_t hash_tagged(const TaggedState& tagged) noexcept {
    const uint64_t packed =
            (static_cast<uint64_t>(tagged.state) << 8) | static_cast<uint64_t>(static_cast<uint8_t>(tagged.tag));
    return fold_hash64(mix_hash64(packed));
}

} // namespace mata::nft::lazy::detail
