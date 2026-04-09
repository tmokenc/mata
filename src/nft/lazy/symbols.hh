/**
 * @file symbols.hh
 * @brief Shared private symbol and alphabet helpers for mata::nft::lazy::detail.
 */

#pragma once

#include "mata/nft/lazy.hh"

#include <cassert>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace mata::nft::lazy::detail {

using SymbolTuple = std::vector<mata::Symbol>;
using OptionalTuple = std::vector<std::optional<mata::Symbol>>;

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

std::string symbol_name_for(mata::Alphabet* alphabet, mata::Symbol symbol);

template<typename Automaton>
// Map a leaf-local symbol into the resolved visible alphabet of the node.
bool try_translate_local_symbol_to_resolved(
        const Automaton& automaton, const mata::OnTheFlyAlphabet& resolved_alphabet, const mata::Symbol local_symbol,
        mata::Symbol& resolved_symbol) {
    try {
        const std::string symbol_name = symbol_name_for(automaton.alphabet, local_symbol);
        const auto it = resolved_alphabet.get_symbol_map().find(symbol_name);
        if (it == resolved_alphabet.get_symbol_map().end()) {
            return false;
        }

        resolved_symbol = it->second;
        return true;
    } catch (const std::runtime_error&) { return false; }
}

// NFT leaves may use per-level alphabets, so the level is part of the lookup.
bool try_translate_local_symbol_to_resolved(
        const mata::nft::Nft& nft, const uint8_t level, const mata::OnTheFlyAlphabet& resolved_alphabet,
        mata::Symbol local_symbol, mata::Symbol& resolved_symbol);

template<typename Automaton>
// Collect all visible symbols used by a unary leaf.
void fill_resolved_leaf_alphabet(const Automaton& automaton, mata::OnTheFlyAlphabet& alphabet_to_fill) {
    for (const auto& state_post : automaton.delta) {
        for (const auto& symbol_post : state_post) {
            alphabet_to_fill.translate_symb(symbol_name_for(automaton.alphabet, symbol_post.symbol));
        }
    }
}

// Collect visible symbols level-by-level for a multi-tape NFT leaf.
void fill_resolved_leaf_level_alphabets(
        const mata::nft::Nft& nft, std::vector<mata::OnTheFlyAlphabet>& level_alphabets_to_fill);

// Merge one alphabet into the canonical alphabet chosen for an equivalence class of levels.
void add_symbols_to_canonical(const mata::OnTheFlyAlphabet& alphabet, mata::OnTheFlyAlphabet& canonical_alphabet);

} // namespace mata::nft::lazy::detail
