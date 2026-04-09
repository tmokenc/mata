/** @file
 * @brief tests for the nfa::lazy facade.
 *
 * This only contains simple test for the mata::nfa::lazy, since it just a wrapper around the mata::nft::lazy
 * The real test is in tests/nft/lazy.cc
 */

#include <catch2/catch_test_macros.hpp>

#include "mata/nfa/lazy.hh"
#include "mata/nfa/nfa.hh"

using namespace mata;

namespace {

nfa::Nfa single_symbol_nfa(Symbol symbol) {
    nfa::Nfa aut{2};
    aut.initial = {0};
    aut.final = {1};
    aut.delta.add(0, symbol, 1);
    return aut;
}

} // namespace

TEST_CASE("mata::nfa::lazy exposes the NFA-only boolean fragment") {
    nfa::lazy::SymbolicAutomataTree tree;

    const nfa::lazy::Term a = tree.make_term(single_symbol_nfa('a'));
    const nfa::lazy::Term b = tree.make_term(single_symbol_nfa('b'));
    const nfa::lazy::Term union_term = tree.union_(a, b);

    CHECK(tree.is_valid(union_term));
    CHECK_FALSE(tree.is_empty(union_term));
}

TEST_CASE("mata::nfa::lazy keeps complement available but hides NFT-only operators") {
    nfa::lazy::SymbolicAutomataTree tree;

    const nfa::lazy::Term a = tree.make_term(single_symbol_nfa('a'));
    const nfa::lazy::Term not_a = tree.complement(a);

    CHECK_FALSE(tree.is_empty(not_a));
}
