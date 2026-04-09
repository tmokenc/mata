/**
 * @file symbols.cc
 * @brief Non-template symbol and alphabet helpers for mata::nft::lazy::detail.
 */

#include "symbols.hh"

namespace mata::nft::lazy::detail {

std::string symbol_name_for(mata::Alphabet* alphabet, const mata::Symbol symbol) {
    if (alphabet != nullptr) {
        try {
            return alphabet->reverse_translate_symbol(symbol);
        } catch (const std::runtime_error&) {
            // Fall back to the raw numeric symbol when no string name is available.
        }
    }

    return std::to_string(symbol);
}

bool try_translate_local_symbol_to_resolved(
        const mata::nft::Nft& nft, const uint8_t level, const mata::OnTheFlyAlphabet& resolved_alphabet,
        const mata::Symbol local_symbol, mata::Symbol& resolved_symbol) {
    try {
        const std::string symbol_name =
                symbol_name_for(const_cast<mata::Alphabet*>(nft.alphabet_of_level(level)), local_symbol);
        const auto it = resolved_alphabet.get_symbol_map().find(symbol_name);
        if (it == resolved_alphabet.get_symbol_map().end()) {
            return false;
        }

        resolved_symbol = it->second;
        return true;
    } catch (const std::runtime_error&) { return false; }
}

void fill_resolved_leaf_level_alphabets(
        const mata::nft::Nft& nft, std::vector<mata::OnTheFlyAlphabet>& level_alphabets_to_fill) {
    for (mata::nfa::State state = 0; state < nft.delta.num_of_states(); ++state) {
        const uint8_t level = static_cast<uint8_t>(nft.levels[state]);
        assert(level < level_alphabets_to_fill.size());

        for (const auto& symbol_post : nft.delta.state_post(state)) {
            level_alphabets_to_fill[level].translate_symb(
                    symbol_name_for(const_cast<mata::Alphabet*>(nft.alphabet_of_level(level)), symbol_post.symbol));
        }
    }
}

void add_symbols_to_canonical(const mata::OnTheFlyAlphabet& alphabet, mata::OnTheFlyAlphabet& canonical_alphabet) {
    for (const mata::Symbol symbol : alphabet.get_alphabet_symbols()) {
        canonical_alphabet.translate_symb(alphabet.reverse_translate_symbol(symbol));
    }
}

} // namespace mata::nft::lazy::detail
