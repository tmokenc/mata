/**
 * @file state_types.cc
 * @brief Non-template state and hashing helpers for mata::nft::lazy::detail.
 */

#include "state_types.hh"

namespace mata::nft::lazy::detail {

uint32_t hash_states(const SetState& states) {
    uint64_t hash = mix_hash64(states.size());
    for (const MacroStateId& id : states) {
        hash ^= mix_hash64(id);
    }
    return fold_hash64(mix_hash64(hash));
}

} // namespace mata::nft::lazy::detail
