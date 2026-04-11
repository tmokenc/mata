/**
 * @file state_types.hh
 * @brief Shared private state and hashing helpers for mata::nft::lazy::detail.
 */

#pragma once

#include "mata/nft/lazy.hh"

#include <cstdint>
#include <unordered_set>

namespace mata::nft::lazy::detail {

enum class VisitState : uint8_t {
    Unseen = 0,
    Active = 1,
    Done = 2,
};

enum class ExecKind : uint8_t {
    LeafNfa = 0,
    LeafNft,
    Union,
    Intersect,
    Complement,
    Identity,
    Project,
    SyncProduct,

    // Fast path, for now these 3 are most uses and are hottest path
    // Not sure if the Project and SyncProduct should be added as well
    // For my current usage (Regular Model Checking), they add very
    // little performance gains and all the works around them not worth the trouble
    // Also one weird case where LeafNft has arity 1, it is entire possible
    // but really not a realistic case so I decide to not add here.
    Arity1Union,
    Arity1Intersect,
    Arity1Complement,

    // Fast path for Arity 2
    Arity2LeafNft,
    Arity2Union,
    Arity2Intersect,
    Arity2Complement,
    Arity2Project,
    Arity2SyncProduct,
};

struct ExecNode {
    ExecKind kind;
    NodeId lhs;
    NodeId rhs;
    uint32_t payload;
    uint8_t result_arity;
};

constexpr bool is_complement_exec_kind(const ExecKind kind) noexcept {
    return kind == ExecKind::Complement || kind == ExecKind::Arity1Complement || kind == ExecKind::Arity2Complement;
}

constexpr bool is_arity1_exec_kind(const ExecKind kind) noexcept {
    return kind == ExecKind::LeafNfa || kind == ExecKind::Arity1Union || kind == ExecKind::Arity1Intersect ||
           kind == ExecKind::Arity1Complement;
}

constexpr bool is_arity2_exec_kind(const ExecKind kind) noexcept {
    return kind == ExecKind::Arity2LeafNft || kind == ExecKind::Arity2Union || kind == ExecKind::Arity2Intersect ||
           kind == ExecKind::Arity2Complement || kind == ExecKind::Identity || kind == ExecKind::Arity2Project ||
           kind == ExecKind::Arity2SyncProduct;
}

// Mix one integer into a stable 64-bit hash state.
constexpr uint64_t mix_hash64(uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

// Fold a 64-bit hash down to the 32-bit ids used by the macrostate stores.
constexpr uint32_t fold_hash64(uint64_t value) noexcept { return static_cast<uint32_t>(value ^ (value >> 32)); }

struct PairState {
    MacroStateId lhs;
    MacroStateId rhs;
};

struct TaggedState {
    enum class Tag : uint8_t {
        Left = 0,
        Right = 1,
    };

    MacroStateId state;
    Tag tag;
};

using SetState = std::unordered_set<MacroStateId>;
using AntichainBucketKey = uint64_t;

uint32_t hash_states(const SetState& states);

// Hash a binary-product macrostate.
constexpr uint32_t hash_pair(const PairState& pair) noexcept {
    const uint64_t packed = (static_cast<uint64_t>(pair.lhs) << 32) | static_cast<uint64_t>(pair.rhs);
    return fold_hash64(mix_hash64(packed));
}

// Hash a tagged union macrostate.
constexpr uint32_t hash_tagged(const TaggedState& tagged) noexcept {
    const uint64_t packed =
            (static_cast<uint64_t>(tagged.state) << 8) | static_cast<uint64_t>(static_cast<uint8_t>(tagged.tag));
    return fold_hash64(mix_hash64(packed));
}

// Extend an antichain bucket key with one more structural component.
constexpr AntichainBucketKey mix_bucket_key(AntichainBucketKey seed, AntichainBucketKey value) noexcept {
    constexpr AntichainBucketKey k_mul = 0x9e3779b97f4a7c15ULL;
    seed ^= value + k_mul + (seed << 6) + (seed >> 2);
    return seed;
}

} // namespace mata::nft::lazy::detail
