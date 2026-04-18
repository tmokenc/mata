/**
 * @file symbols.hh
 * @brief Shared private tuple helpers for mata::nft::lazy::detail.
 */

#pragma once

#include "mata/nft/lazy.hh"

#include <cstddef>
#include <vector>

namespace mata::nft::lazy::detail {

/// Visible label tuple used by the lazy transition iterators.
using SymbolTuple = std::vector<mata::Symbol>;

/**
 * @brief Hash functor for visible symbol tuples.
 */
struct SymbolTupleHash {
    // Hash a visible tuple so it can be used as a cache key.
    size_t operator()(const SymbolTuple& tuple) const {
        size_t seed = 0;
        for (const mata::Symbol sym : tuple) {
            // Keep tuple hashing local and cheap. The tuple lengths in the lazy core are
            // typically small, so a simple combine is enough here.
            seed ^= static_cast<size_t>(sym) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
        }
        return seed;
    }
};

} // namespace mata::nft::lazy::detail
