/** @file
 * @brief Semantic tests for mata::nft::lazy.
 *
 * Most tests derive alphabets entirely from the transition delta:
 *  - create_alphabet(nfa)            walks nfa.delta for NFA leaf nodes
 *  - create_relation_alphabet_info() walks nft.delta by level for NFT leaf nodes
 *
 * The normalization regressions below also cover explicitly assigned
 * nfa.alphabet / nft.alphabet objects with differing local symbol ids.
 *
 * For automata with no transitions (empty language / empty relation) the
 * derived alphabet is empty, which is correct: no transitions means no states
 * to explore, so emptiness holds trivially regardless of the universe.
 *
 * Organisation:
 *  1. Leaf NFA emptiness
 *  2. Leaf NFT emptiness
 *  3. Boolean NFA combinations  (union / intersect / complement)
 *  4. Complement – roundtrip and De Morgan identities
 *  5. Pre-image and post-image on singleton relations
 *  6. Pre-image and post-image with edge-case inputs
 *  7. Level-specific alphabet isolation
 *  8. NFT relation complement
 *  9. Composition
 * 10. DAG sharing
 */

#include <catch2/catch_test_macros.hpp>

#include "mata/nft/lazy.hh"
#include "mata/nft/nft.hh"

using namespace mata;
using namespace mata::nft;
using namespace mata::nft::lazy;

namespace {

// ---------------------------------------------------------------------------
// NFA helpers
// ---------------------------------------------------------------------------

/// Accepts nothing.  No transitions — derived alphabet will be empty,
/// which is fine: the traversal has nowhere to go.
[[nodiscard]] nfa::Nfa empty_nfa() {
    nfa::Nfa aut{1};
    aut.initial = {0};
    return aut;
}

/// Accepts only the empty word (ε).  No transitions needed.
[[nodiscard]] nfa::Nfa epsilon_nfa() {
    nfa::Nfa aut{1};
    aut.initial = {0};
    aut.final = {0};
    return aut;
}

/// Accepts exactly the one-symbol word { sym }.
/// State 0 --sym--> state 1 (final).
[[nodiscard]] nfa::Nfa single_symbol_nfa(const Symbol sym) {
    nfa::Nfa aut{2};
    aut.initial = {0};
    aut.final = {1};
    aut.delta.add(0, sym, 1);
    return aut;
}

/// Accepts exactly one named symbol using a caller-provided local id.
[[nodiscard]] nfa::Nfa single_named_symbol_nfa(const std::string& symbol_name, const Symbol local_symbol) {
    auto* alphabet = new OnTheFlyAlphabet{};
    alphabet->add_new_symbol(symbol_name, local_symbol);

    nfa::Nfa aut{2, {0}, {1}, alphabet};
    aut.delta.add(0, local_symbol, 1);
    return aut;
}

/// Accepts exactly one symbol from one of two initial states.
[[nodiscard]] nfa::Nfa two_initials_one_live_nfa(const Symbol sym) {
    nfa::Nfa aut{4};
    aut.initial = {0, 1};
    aut.final = {3};
    aut.delta.add(1, sym, 3);
    return aut;
}

/// Accepts every word over {sym}* (self-loop, initial = final).
[[nodiscard]] nfa::Nfa universal_nfa(const Symbol sym) {
    nfa::Nfa aut{1};
    aut.initial = {0};
    aut.final = {0};
    aut.delta.add(0, sym, 0);
    return aut;
}

// ---------------------------------------------------------------------------
// NFT helpers
//
// Level layout for a 2-level NFT with one input/output step:
//   state 0  (level 0, input)  --in-->  state 1
//   state 1  (level 1, output) --out--> state 2  (final)
//
// For a looping NFT:
//   state 0  (level 0) --in-->  state 1
//   state 1  (level 1) --out--> state 0  (also final)
// ---------------------------------------------------------------------------

/// Relation { (in, out) } — accepts only the single pair of length-1 words.
[[nodiscard]] Nft relation_single_pair(const Symbol in, const Symbol out) {
    Nft nft = Nft::with_levels(2, 3, {0}, {2});
    nft.levels[0] = 0;
    nft.levels[1] = 1;
    nft.levels[2] = 0; // final state; level value unused for accepted pairs
    nft.delta.add(0, in, 1);
    nft.delta.add(1, out, 2);
    return nft;
}

/// Relation { (in, out) } with caller-chosen symbol names and local ids.
[[nodiscard]] Nft relation_single_named_pair(
        const std::string& in_name, const Symbol in_local,
        const std::string& out_name, const Symbol out_local) {
    auto* alphabet = new OnTheFlyAlphabet{};
    alphabet->add_new_symbol(in_name, in_local);
    alphabet->add_new_symbol(out_name, out_local);

    Nft nft = Nft::with_levels(2, 3, {0}, {2}, alphabet);
    nft.levels[0] = 0;
    nft.levels[1] = 1;
    nft.levels[2] = 0;
    nft.delta.add(0, in_local, 1);
    nft.delta.add(1, out_local, 2);
    return nft;
}

/// Relation { (in, out) } with genuinely separate per-tape alphabets.
[[nodiscard]] Nft relation_single_named_pair_per_tape(
        const std::string& in_name, const Symbol in_local,
        const std::string& out_name, const Symbol out_local) {
    auto* input_alphabet = new OnTheFlyAlphabet{};
    auto* output_alphabet = new OnTheFlyAlphabet{};
    input_alphabet->add_new_symbol(in_name, in_local);
    output_alphabet->add_new_symbol(out_name, out_local);

    Nft nft = Nft::with_levels(2, 3, {0}, {2}, std::vector<Alphabet*>{input_alphabet, output_alphabet});
    nft.levels[0] = 0;
    nft.levels[1] = 1;
    nft.levels[2] = 0;
    nft.delta.add(0, in_local, 1);
    nft.delta.add(1, out_local, 2);
    return nft;
}

/// Relation { (in^n, out^n) | n >= 0 } — looping transducer, initial = final.
[[nodiscard]] Nft universal_pair_loop(const Symbol in, const Symbol out) {
    Nft nft = Nft::with_levels(2, 2, {0}, {0});
    nft.levels[0] = 0;
    nft.levels[1] = 1;
    nft.delta.add(0, in, 1);
    nft.delta.add(1, out, 0);
    return nft;
}

/// Universal singleton-tuple loop over genuinely separate named per-tape alphabets.
[[nodiscard]] Nft universal_named_pair_loop_per_tape(
        const std::string& in_name, const Symbol in_local,
        const std::string& out_name, const Symbol out_local) {
    auto* input_alphabet = new OnTheFlyAlphabet{};
    auto* output_alphabet = new OnTheFlyAlphabet{};
    input_alphabet->add_new_symbol(in_name, in_local);
    output_alphabet->add_new_symbol(out_name, out_local);

    Nft nft = Nft::with_levels(2, 2, {0}, {0}, std::vector<Alphabet*>{input_alphabet, output_alphabet});
    nft.levels[0] = 0;
    nft.levels[1] = 1;
    nft.delta.add(0, in_local, 1);
    nft.delta.add(1, out_local, 0);
    return nft;
}

/// NFT with transitions but no final states — accepts no pair.
[[nodiscard]] Nft empty_nft(const Symbol in, const Symbol out) {
    Nft nft = Nft::with_levels(2, 2, {0}, {});
    nft.levels[0] = 0;
    nft.levels[1] = 1;
    nft.delta.add(0, in, 1);
    nft.delta.add(1, out, 0);
    return nft;
}

/// One dead initial state and one live initial state producing a single pair.
[[nodiscard]] Nft two_initials_one_live_nft(const Symbol in, const Symbol out) {
    Nft nft = Nft::with_levels(2, 4, {0, 1}, {3});
    nft.levels[0] = 0;
    nft.levels[1] = 0;
    nft.levels[2] = 1;
    nft.levels[3] = 0;
    nft.delta.add(1, in, 2);
    nft.delta.add(2, out, 3);
    return nft;
}

/// Relation { (a, b, c) } on three tracks.
[[nodiscard]] Nft relation_single_triple(const Symbol a, const Symbol b, const Symbol c) {
    Nft nft = Nft::with_levels(3, 4, {0}, {3});
    nft.levels[0] = 0;
    nft.levels[1] = 1;
    nft.levels[2] = 2;
    nft.levels[3] = 0;
    nft.delta.add(0, a, 1);
    nft.delta.add(1, b, 2);
    nft.delta.add(2, c, 3);
    return nft;
}

} // namespace

// ---------------------------------------------------------------------------
// 1. Leaf NFA emptiness
// ---------------------------------------------------------------------------

TEST_CASE("mata::nft::lazy – leaf NFA: empty language") {
    SymbolicAutomataTree tree;
    CHECK(tree.is_empty(tree.make_term(empty_nfa())));
}

TEST_CASE("mata::nft::lazy – leaf NFA: epsilon language is not empty") {
    SymbolicAutomataTree tree;
    CHECK_FALSE(tree.is_empty(tree.make_term(epsilon_nfa())));
}

TEST_CASE("mata::nft::lazy – leaf NFA: single-symbol language is not empty") {
    SymbolicAutomataTree tree;
    CHECK_FALSE(tree.is_empty(tree.make_term(single_symbol_nfa('a'))));
}

TEST_CASE("mata::nft::lazy – leaf NFA: one live initial state is enough") {
    SymbolicAutomataTree tree;
    CHECK_FALSE(tree.is_empty(tree.make_term(two_initials_one_live_nfa('a'))));
}

// ---------------------------------------------------------------------------
// 2. Leaf NFT emptiness
// ---------------------------------------------------------------------------

TEST_CASE("mata::nft::lazy – leaf NFT: singleton relation is not empty") {
    SymbolicAutomataTree tree;
    CHECK_FALSE(tree.is_empty(tree.make_term(relation_single_pair('a', 'b'))));
}

TEST_CASE("mata::nft::lazy – leaf NFT: looping relation is not empty") {
    SymbolicAutomataTree tree;
    CHECK_FALSE(tree.is_empty(tree.make_term(universal_pair_loop('a', 'b'))));
}

TEST_CASE("mata::nft::lazy – leaf NFT: empty transducer is empty") {
    SymbolicAutomataTree tree;
    CHECK(tree.is_empty(tree.make_term(empty_nft('a', 'b'))));
}

TEST_CASE("mata::nft::lazy – leaf NFT: one live initial state is enough") {
    SymbolicAutomataTree tree;
    CHECK_FALSE(tree.is_empty(tree.make_term(two_initials_one_live_nft('a', 'b'))));
}

TEST_CASE("mata::nft::lazy – leaf NFT: arity-3 singleton relation is not empty") {
    SymbolicAutomataTree tree;
    const Term triple = tree.make_term(relation_single_triple('a', 'b', 'c'));
    CHECK(tree.arity_of(triple) == 3);
    CHECK_FALSE(tree.is_empty(triple));
}

// ---------------------------------------------------------------------------
// 3. Boolean NFA combinations
// ---------------------------------------------------------------------------

TEST_CASE("mata::nft::lazy – union of two disjoint non-empty languages is not empty") {
    SymbolicAutomataTree tree;
    const Term a = tree.make_term(single_symbol_nfa('a'));
    const Term b = tree.make_term(single_symbol_nfa('b'));
    CHECK_FALSE(tree.is_empty(tree.union_(a, b)));
}

TEST_CASE("mata::nft::lazy – union with empty is identity") {
    SymbolicAutomataTree tree;
    const Term a = tree.make_term(single_symbol_nfa('a'));
    const Term empty = tree.make_term(empty_nfa());
    CHECK_FALSE(tree.is_empty(tree.union_(a, empty)));
    CHECK_FALSE(tree.is_empty(tree.union_(empty, a)));
}

TEST_CASE("mata::nft::lazy – union of two empty languages is empty") {
    SymbolicAutomataTree tree;
    const Term e1 = tree.make_term(empty_nfa());
    const Term e2 = tree.make_term(empty_nfa());
    CHECK(tree.is_empty(tree.union_(e1, e2)));
}

TEST_CASE("mata::nft::lazy – intersection of disjoint languages is empty") {
    SymbolicAutomataTree tree;
    const Term a = tree.make_term(single_symbol_nfa('a'));
    const Term b = tree.make_term(single_symbol_nfa('b'));
    CHECK(tree.is_empty(tree.intersect(a, b)));
}

TEST_CASE("mata::nft::lazy – intersection of a language with itself is not empty") {
    SymbolicAutomataTree tree;
    const Term a = tree.make_term(single_symbol_nfa('a'));
    CHECK_FALSE(tree.is_empty(tree.intersect(a, a)));
}

TEST_CASE("mata::nft::lazy – intersection normalizes shared string symbols across local encodings") {
    SymbolicAutomataTree tree;
    const Term lhs = tree.make_term(single_named_symbol_nfa("x", 3));
    const Term rhs = tree.make_term(single_named_symbol_nfa("x", 17));
    CHECK_FALSE(tree.is_empty(tree.intersect(lhs, rhs)));
}

TEST_CASE("mata::nft::lazy – intersection of single-symbol word with epsilon is empty") {
    SymbolicAutomataTree tree;
    const Term a = tree.make_term(single_symbol_nfa('a'));
    const Term eps = tree.make_term(epsilon_nfa());
    CHECK(tree.is_empty(tree.intersect(a, eps)));
}

TEST_CASE("mata::nft::lazy – intersection with empty language is empty") {
    SymbolicAutomataTree tree;
    const Term a = tree.make_term(single_symbol_nfa('a'));
    const Term empty = tree.make_term(empty_nfa());
    CHECK(tree.is_empty(tree.intersect(a, empty)));
}

TEST_CASE("mata::nft::lazy – NFT intersection treats DONT_CARE as a wildcard") {
    SymbolicAutomataTree tree;
    const Term wildcard = tree.make_term(relation_single_pair(DONT_CARE, 'b'));
    const Term concrete = tree.make_term(relation_single_pair('a', 'b'));

    CHECK_FALSE(tree.is_empty(tree.intersect(wildcard, concrete)));
}

// ---------------------------------------------------------------------------
// 4. Complement – basic, roundtrip, and De Morgan
// ---------------------------------------------------------------------------

TEST_CASE("mata::nft::lazy – complement of empty language is not empty") {
    SymbolicAutomataTree tree;
    // complement of ∅ must contain at least ε (the empty macro-set is accepting).
    CHECK_FALSE(tree.is_empty(tree.complement(tree.make_term(empty_nfa()))));
}

TEST_CASE("mata::nft::lazy – complement of single-symbol language is not empty") {
    // {a} is not universal over {a}*, so its complement contains other words.
    SymbolicAutomataTree tree;
    CHECK_FALSE(tree.is_empty(tree.complement(tree.make_term(single_symbol_nfa('a')))));
}

TEST_CASE("mata::nft::lazy – complement of universal language is empty") {
    SymbolicAutomataTree tree;
    CHECK(tree.is_empty(tree.complement(tree.make_term(universal_nfa('a')))));
}

TEST_CASE("mata::nft::lazy – explicit alphabet widens complement of universal NFA") {
    SymbolicAutomataTree tree;
    OnTheFlyAlphabet alphabet{};
    alphabet.add_new_symbol("a", 'a');
    alphabet.add_new_symbol("b", 'b');

    CHECK_FALSE(tree.is_empty(tree.complement(tree.make_term(universal_nfa('a'))), alphabet));
}

TEST_CASE("mata::nft::lazy – double complement of non-empty language is not empty") {
    SymbolicAutomataTree tree;
    const Term a = tree.make_term(single_symbol_nfa('a'));
    CHECK_FALSE(tree.is_empty(tree.complement(tree.complement(a))));
}

TEST_CASE("mata::nft::lazy – double complement of empty language is empty") {
    SymbolicAutomataTree tree;
    const Term e = tree.make_term(empty_nfa());
    CHECK(tree.is_empty(tree.complement(tree.complement(e))));
}

TEST_CASE("mata::nft::lazy – De Morgan: complement(union) and intersect(complements) agree") {
    SymbolicAutomataTree tree;
    const Term a = tree.make_term(single_symbol_nfa('a'));
    const Term eps = tree.make_term(epsilon_nfa());
    const Term lhs = tree.complement(tree.union_(a, eps));
    const Term rhs = tree.intersect(tree.complement(a), tree.complement(eps));
    CHECK(tree.is_empty(lhs) == tree.is_empty(rhs));
}

TEST_CASE("mata::nft::lazy – De Morgan: complement(intersect) and union(complements) agree") {
    SymbolicAutomataTree tree;
    const Term a = tree.make_term(single_symbol_nfa('a'));
    const Term b = tree.make_term(single_symbol_nfa('b'));
    const Term lhs = tree.complement(tree.intersect(a, b));
    const Term rhs = tree.union_(tree.complement(a), tree.complement(b));
    // intersect(a, b) is empty => its complement is not empty
    CHECK(tree.is_empty(lhs) == tree.is_empty(rhs));
    CHECK_FALSE(tree.is_empty(lhs));
}

// ---------------------------------------------------------------------------
// 5. Pre-image and post-image on singleton relations
// ---------------------------------------------------------------------------

TEST_CASE("mata::nft::lazy – post-image of matching input is not empty") {
    SymbolicAutomataTree tree;
    const Term lang_a = tree.make_term(single_symbol_nfa('a'));
    const Term rel_a_to_b = tree.make_term(relation_single_pair('a', 'b'));
    CHECK_FALSE(tree.is_empty(tree.post_image(lang_a, rel_a_to_b)));
}

TEST_CASE("mata::nft::lazy – post-image of non-matching input is empty") {
    SymbolicAutomataTree tree;
    const Term lang_b = tree.make_term(single_symbol_nfa('b'));
    const Term rel_a_to_b = tree.make_term(relation_single_pair('a', 'b'));
    CHECK(tree.is_empty(tree.post_image(lang_b, rel_a_to_b)));
}

TEST_CASE("mata::nft::lazy – post-image of empty language is empty") {
    SymbolicAutomataTree tree;
    const Term empty = tree.make_term(empty_nfa());
    const Term rel_a_to_b = tree.make_term(relation_single_pair('a', 'b'));
    CHECK(tree.is_empty(tree.post_image(empty, rel_a_to_b)));
}

TEST_CASE("mata::nft::lazy – pre-image of matching output is not empty") {
    SymbolicAutomataTree tree;
    const Term lang_b = tree.make_term(single_symbol_nfa('b'));
    const Term rel_a_to_b = tree.make_term(relation_single_pair('a', 'b'));
    CHECK_FALSE(tree.is_empty(tree.pre_image(lang_b, rel_a_to_b)));
}

TEST_CASE("mata::nft::lazy – pre-image of non-matching output is empty") {
    SymbolicAutomataTree tree;
    const Term lang_a = tree.make_term(single_symbol_nfa('a'));
    const Term rel_a_to_b = tree.make_term(relation_single_pair('a', 'b'));
    CHECK(tree.is_empty(tree.pre_image(lang_a, rel_a_to_b)));
}

TEST_CASE("mata::nft::lazy – pre-image direction: rel b->a, pre-image of {a} is non-empty") {
    SymbolicAutomataTree tree;
    const Term lang_a = tree.make_term(single_symbol_nfa('a'));
    const Term rel_b_to_a = tree.make_term(relation_single_pair('b', 'a'));
    CHECK_FALSE(tree.is_empty(tree.pre_image(lang_a, rel_b_to_a)));
}

TEST_CASE("mata::nft::lazy – pre-image of empty language is empty") {
    SymbolicAutomataTree tree;
    const Term empty = tree.make_term(empty_nfa());
    const Term rel_a_to_b = tree.make_term(relation_single_pair('a', 'b'));
    CHECK(tree.is_empty(tree.pre_image(empty, rel_a_to_b)));
}

TEST_CASE("mata::nft::lazy – post-image normalizes named symbols across local encodings") {
    SymbolicAutomataTree tree;
    const Term lang_in = tree.make_term(single_named_symbol_nfa("in", 5));
    const Term lang_out = tree.make_term(single_named_symbol_nfa("out", 41));
    const Term rel = tree.make_term(relation_single_named_pair("in", 17, "out", 29));
    const Term post = tree.post_image(lang_in, rel);

    CHECK_FALSE(tree.is_empty(post));
    CHECK_FALSE(tree.is_empty(tree.intersect(post, lang_out)));
}

TEST_CASE("mata::nft::lazy – post-image respects genuinely separate per-tape alphabets") {
    SymbolicAutomataTree tree;
    const Term lang_in = tree.make_term(single_named_symbol_nfa("src", 5));
    const Term lang_out = tree.make_term(single_named_symbol_nfa("dst", 11));
    const Term rel = tree.make_term(relation_single_named_pair_per_tape("src", 7, "dst", 7));
    const Term post = tree.post_image(lang_in, rel);

    CHECK_FALSE(tree.is_empty(post));
    CHECK_FALSE(tree.is_empty(tree.intersect(post, lang_out)));
}

TEST_CASE("mata::nft::lazy – pre-image normalizes named symbols across local encodings") {
    SymbolicAutomataTree tree;
    const Term lang_out = tree.make_term(single_named_symbol_nfa("out", 7));
    const Term lang_in = tree.make_term(single_named_symbol_nfa("in", 50));
    const Term rel = tree.make_term(relation_single_named_pair("in", 13, "out", 19));
    const Term pre = tree.pre_image(lang_out, rel);

    CHECK_FALSE(tree.is_empty(pre));
    CHECK_FALSE(tree.is_empty(tree.intersect(pre, lang_in)));
}

TEST_CASE("mata::nft::lazy – post-image through empty transducer is empty") {
    SymbolicAutomataTree tree;
    const Term lang_a = tree.make_term(single_symbol_nfa('a'));
    const Term empty_rel = tree.make_term(empty_nft('a', 'b'));
    CHECK(tree.is_empty(tree.post_image(lang_a, empty_rel)));
}

TEST_CASE("mata::nft::lazy – pre-image through empty transducer is empty") {
    SymbolicAutomataTree tree;
    const Term lang_b = tree.make_term(single_symbol_nfa('b'));
    const Term empty_rel = tree.make_term(empty_nft('a', 'b'));
    CHECK(tree.is_empty(tree.pre_image(lang_b, empty_rel)));
}

// ---------------------------------------------------------------------------
// 6. Pre-image / post-image with epsilon input
// ---------------------------------------------------------------------------

TEST_CASE("mata::nft::lazy – post-image of epsilon through length-1 relation is empty") {
    SymbolicAutomataTree tree;
    const Term eps = tree.make_term(epsilon_nfa());
    const Term rel_a_to_b = tree.make_term(relation_single_pair('a', 'b'));
    CHECK(tree.is_empty(tree.post_image(eps, rel_a_to_b)));
}

TEST_CASE("mata::nft::lazy – pre-image of epsilon through length-1 relation is empty") {
    SymbolicAutomataTree tree;
    const Term eps = tree.make_term(epsilon_nfa());
    const Term rel_a_to_b = tree.make_term(relation_single_pair('a', 'b'));
    CHECK(tree.is_empty(tree.pre_image(eps, rel_a_to_b)));
}

TEST_CASE("mata::nft::lazy – post-image of epsilon through epsilon-preserving loop is not empty") {
    SymbolicAutomataTree tree;
    const Term eps = tree.make_term(epsilon_nfa());
    const Term rel = tree.make_term(universal_pair_loop('a', 'b'));
    CHECK_FALSE(tree.is_empty(tree.post_image(eps, rel)));
}

TEST_CASE("mata::nft::lazy – pre-image of epsilon through epsilon-preserving loop is not empty") {
    SymbolicAutomataTree tree;
    const Term eps = tree.make_term(epsilon_nfa());
    const Term rel = tree.make_term(universal_pair_loop('a', 'b'));
    CHECK_FALSE(tree.is_empty(tree.pre_image(eps, rel)));
}

// ---------------------------------------------------------------------------
// 7. Level-specific alphabet isolation
// ---------------------------------------------------------------------------

TEST_CASE("mata::nft::lazy – post-image uses only the output alphabet") {
    // rel maps a* -> b*.  post-image of a* is b*.
    // complement within output alphabet {b} must be empty.
    SymbolicAutomataTree tree;
    const Term lang_a_star = tree.make_term(universal_nfa('a'));
    const Term rel = tree.make_term(universal_pair_loop('a', 'b'));
    const Term post = tree.post_image(lang_a_star, rel);
    CHECK_FALSE(tree.is_empty(post));
    CHECK(tree.is_empty(tree.complement(post)));
}

TEST_CASE("mata::nft::lazy – pre-image uses only the input alphabet") {
    // rel maps a* -> b*.  pre-image of b* is a*.
    // complement within input alphabet {a} must be empty.
    SymbolicAutomataTree tree;
    const Term lang_b_star = tree.make_term(universal_nfa('b'));
    const Term rel = tree.make_term(universal_pair_loop('a', 'b'));
    const Term pre = tree.pre_image(lang_b_star, rel);
    CHECK_FALSE(tree.is_empty(pre));
    CHECK(tree.is_empty(tree.complement(pre)));
}

TEST_CASE("mata::nft::lazy – post-image result does not contain input symbols") {
    // post-image of a* through rel (a->b) lives in alphabet {b}.
    // Intersecting with {a} (input-alphabet language) must be empty.
    SymbolicAutomataTree tree;
    const Term lang_a_star = tree.make_term(universal_nfa('a'));
    const Term rel = tree.make_term(universal_pair_loop('a', 'b'));
    const Term post = tree.post_image(lang_a_star, rel);
    const Term lang_a = tree.make_term(single_symbol_nfa('a'));
    CHECK(tree.is_empty(tree.intersect(post, lang_a)));
}

// ---------------------------------------------------------------------------
// 8. NFT relation complement
// ---------------------------------------------------------------------------

TEST_CASE("mata::nft::lazy – complement of universal NFT relation is empty") {
    SymbolicAutomataTree tree;
    const Term univ = tree.make_term(universal_pair_loop('a', 'b'));
    CHECK_FALSE(tree.is_empty(univ));
    CHECK(tree.is_empty(tree.complement(univ)));
}

TEST_CASE("mata::nft::lazy – explicit alphabet widens complement of universal NFT relation") {
    SymbolicAutomataTree tree;
    const Term univ = tree.make_term(universal_pair_loop('a', 'b'));

    OnTheFlyAlphabet alphabet{};
    alphabet.add_new_symbol("a", 'a');
    alphabet.add_new_symbol("b", 'b');
    alphabet.add_new_symbol("c", 'c');

    CHECK_FALSE(tree.is_empty(tree.complement(univ), alphabet));
}

TEST_CASE("mata::nft::lazy – explicit per-level alphabets define complement universe tape-wise") {
    SymbolicAutomataTree tree;
    const Term univ = tree.make_term(universal_named_pair_loop_per_tape("in", 7, "out", 13));

    std::vector<OnTheFlyAlphabet> level_alphabets(2);
    level_alphabets[0].add_new_symbol("in", 101);
    level_alphabets[1].add_new_symbol("out", 202);
    CHECK(tree.is_empty(tree.complement(univ), level_alphabets));

    level_alphabets[1].add_new_symbol("other_out", 303);
    CHECK_FALSE(tree.is_empty(tree.complement(univ), level_alphabets));
}

TEST_CASE("mata::nft::lazy – double complement of non-empty NFT relation is not empty") {
    SymbolicAutomataTree tree;
    const Term rel = tree.make_term(relation_single_pair('a', 'b'));
    CHECK_FALSE(tree.is_empty(tree.complement(tree.complement(rel))));
}

TEST_CASE("mata::nft::lazy – complement of empty NFT relation is not empty") {
    SymbolicAutomataTree tree;
    const Term rel = tree.make_term(empty_nft('a', 'b'));
    CHECK_FALSE(tree.is_empty(tree.complement(rel)));
}

TEST_CASE("mata::nft::lazy – double complement of empty NFT relation is empty") {
    SymbolicAutomataTree tree;
    const Term rel = tree.make_term(empty_nft('a', 'b'));
    CHECK(tree.is_empty(tree.complement(tree.complement(rel))));
}

// ---------------------------------------------------------------------------
// 9. Composition
// ---------------------------------------------------------------------------

TEST_CASE("mata::nft::lazy – composition chains two singleton relations correctly") {
    // ab ∘ bc = ac
    SymbolicAutomataTree tree;
    const Term lang_a = tree.make_term(single_symbol_nfa('a'));
    const Term lang_c = tree.make_term(single_symbol_nfa('c'));
    const Term ab = tree.make_term(relation_single_pair('a', 'b'));
    const Term bc = tree.make_term(relation_single_pair('b', 'c'));
    const Term ac = tree.compose(ab, bc);

    CHECK_FALSE(tree.is_empty(tree.post_image(lang_a, ac))); // produces {c}
    CHECK_FALSE(tree.is_empty(tree.pre_image(lang_c, ac))); // produces {a}
    CHECK(tree.is_empty(tree.pre_image(lang_a, ac))); // a not an output of ac
}

TEST_CASE("mata::nft::lazy – composition with mismatched sync symbol is empty") {
    // ab maps a->b, cd maps c->d; sync alphabet (b ∩ c) is empty.
    SymbolicAutomataTree tree;
    const Term lang_a = tree.make_term(single_symbol_nfa('a'));
    const Term ab = tree.make_term(relation_single_pair('a', 'b'));
    const Term cd = tree.make_term(relation_single_pair('c', 'd'));
    const Term comp = tree.compose(ab, cd);
    CHECK(tree.is_empty(comp));
    CHECK(tree.is_empty(tree.post_image(lang_a, comp)));
}

TEST_CASE("mata::nft::lazy – composition is not commutative") {
    SymbolicAutomataTree tree;
    const Term lang_a = tree.make_term(single_symbol_nfa('a'));
    const Term ab = tree.make_term(relation_single_pair('a', 'b'));
    const Term bc = tree.make_term(relation_single_pair('b', 'c'));
    const Term ab_then_bc = tree.compose(ab, bc);
    const Term bc_then_ab = tree.compose(bc, ab);
    CHECK_FALSE(tree.is_empty(tree.post_image(lang_a, ab_then_bc)));
    CHECK(tree.is_empty(tree.post_image(lang_a, bc_then_ab)));
}

TEST_CASE("mata::nft::lazy – composition normalizes named sync symbols across local encodings") {
    SymbolicAutomataTree tree;
    const Term lang_src = tree.make_term(single_named_symbol_nfa("src", 101));
    const Term lang_dst = tree.make_term(single_named_symbol_nfa("dst", 205));
    const Term left = tree.make_term(relation_single_named_pair("src", 3, "mid", 11));
    const Term right = tree.make_term(relation_single_named_pair("mid", 47, "dst", 59));
    const Term composed = tree.compose(left, right);

    CHECK_FALSE(tree.is_empty(composed));
    CHECK_FALSE(tree.is_empty(tree.post_image(lang_src, composed)));
    CHECK_FALSE(tree.is_empty(tree.intersect(tree.post_image(lang_src, composed), lang_dst)));
    CHECK_FALSE(tree.is_empty(tree.intersect(tree.pre_image(lang_dst, composed), lang_src)));
}

TEST_CASE("mata::nft::lazy – composition matches DONT_CARE on synchronized levels") {
    SymbolicAutomataTree tree;
    const Term lang_a = tree.make_term(single_symbol_nfa('a'));
    const Term lang_c = tree.make_term(single_symbol_nfa('c'));
    const Term left = tree.make_term(relation_single_pair('a', DONT_CARE));
    const Term right = tree.make_term(relation_single_pair('b', 'c'));
    const Term composed = tree.compose(left, right);

    CHECK_FALSE(tree.is_empty(composed));
    CHECK_FALSE(tree.is_empty(tree.post_image(lang_a, composed)));
    CHECK_FALSE(tree.is_empty(tree.intersect(tree.post_image(lang_a, composed), lang_c)));
}

TEST_CASE("mata::nft::lazy – identity maps a language to the diagonal relation") {
    SymbolicAutomataTree tree;
    const Term lang_a = tree.make_term(single_symbol_nfa('a'));
    const Term lang_b = tree.make_term(single_symbol_nfa('b'));
    const Term diag = tree.identity(lang_a);

    CHECK_FALSE(tree.is_empty(diag));
    CHECK_FALSE(tree.is_empty(tree.post_image(lang_a, diag)));
    CHECK(tree.is_empty(tree.post_image(lang_b, diag)));
}

TEST_CASE("mata::nft::lazy – identity of epsilon language contains the epsilon pair") {
    SymbolicAutomataTree tree;
    const Term eps = tree.make_term(epsilon_nfa());
    const Term diag = tree.identity(eps);

    CHECK_FALSE(tree.is_empty(diag));
    CHECK_FALSE(tree.is_empty(tree.post_image(eps, diag)));
    CHECK_FALSE(tree.is_empty(tree.pre_image(eps, diag)));
}

TEST_CASE("mata::nft::lazy – identity forces the same alphabet on both result tapes") {
    SymbolicAutomataTree tree;
    const Term lang_x = tree.make_term(single_named_symbol_nfa("x", 3));
    const Term diag = tree.identity(lang_x);

    std::vector<OnTheFlyAlphabet> level_alphabets(2);
    level_alphabets[0].add_new_symbol("x", 17);
    level_alphabets[1].add_new_symbol("y", 29);

    CHECK_FALSE(tree.is_empty(diag, level_alphabets));
}

TEST_CASE("mata::nft::lazy – project can drop inner levels from a higher-arity relation") {
    SymbolicAutomataTree tree;
    const Term triple = tree.make_term(relation_single_triple('a', 'b', 'c'));
    const Term projected = tree.project(triple, {0, 2});
    const Term lang_a = tree.make_term(single_symbol_nfa('a'));
    const Term lang_c = tree.make_term(single_symbol_nfa('c'));

    CHECK(tree.arity_of(projected) == 2);
    CHECK_FALSE(tree.is_empty(projected));
    CHECK_FALSE(tree.is_empty(tree.sync_product(lang_a, projected, {0}, {0}, {LevelRef{LevelRef::Side::Rhs, 1}})));
    CHECK_FALSE(tree.is_empty(tree.sync_product(lang_c, projected, {0}, {1}, {LevelRef{LevelRef::Side::Rhs, 0}})));
}

TEST_CASE("mata::nft::lazy – project is existential over removed coordinates") {
    SymbolicAutomataTree tree;
    const Term left = tree.make_term(relation_single_triple('a', 'b', 'c'));
    const Term right = tree.make_term(relation_single_triple('a', 'x', 'c'));
    const Term projected = tree.project(tree.union_(left, right), {0, 2});
    const Term expected = tree.make_term(relation_single_pair('a', 'c'));
    const Term unexpected = tree.make_term(relation_single_pair('a', 'd'));

    CHECK(tree.arity_of(projected) == 2);
    CHECK_FALSE(tree.is_empty(projected));
    CHECK_FALSE(tree.is_empty(tree.intersect(projected, expected)));
    CHECK(tree.is_empty(tree.intersect(projected, unexpected)));
}

TEST_CASE("mata::nft::lazy – project can produce a non-empty zero-arity relation") {
    SymbolicAutomataTree tree;
    const Term triple = tree.make_term(relation_single_triple('a', 'b', 'c'));
    const Term projected = tree.project(triple, {});

    CHECK(tree.arity_of(projected) == 0);
    CHECK_FALSE(tree.is_empty(projected));
}

TEST_CASE("mata::nft::lazy – project of an empty relation to zero arity stays empty") {
    SymbolicAutomataTree tree;
    const Term rel = tree.make_term(empty_nft('a', 'b'));
    const Term projected = tree.project(rel, {});

    CHECK(tree.arity_of(projected) == 0);
    CHECK(tree.is_empty(projected));
}

TEST_CASE("mata::nft::lazy – generic compose on arity-3 relations keeps non-synchronized tracks") {
    SymbolicAutomataTree tree;
    const Term left = tree.make_term(relation_single_triple('a', 'b', 'x'));
    const Term right = tree.make_term(relation_single_triple('x', 'c', 'd'));
    const Term composed = tree.compose(left, right, {2}, {0});

    CHECK(tree.arity_of(composed) == 4);
    CHECK_FALSE(tree.is_empty(composed));
}

TEST_CASE("mata::nft::lazy – sync_product can collapse completely to a zero-arity witness") {
    SymbolicAutomataTree tree;
    const Term lhs = tree.make_term(single_symbol_nfa('a'));
    const Term rhs = tree.make_term(single_symbol_nfa('a'));
    const Term synced = tree.sync_product(lhs, rhs, {0}, {0}, {});

    CHECK(tree.arity_of(synced) == 0);
    CHECK_FALSE(tree.is_empty(synced));
}

TEST_CASE("mata::nft::lazy – zero-arity sync_product is empty when synchronized labels do not match") {
    SymbolicAutomataTree tree;
    const Term lhs = tree.make_term(single_symbol_nfa('a'));
    const Term rhs = tree.make_term(single_symbol_nfa('b'));
    const Term synced = tree.sync_product(lhs, rhs, {0}, {0}, {});

    CHECK(tree.arity_of(synced) == 0);
    CHECK(tree.is_empty(synced));
}

TEST_CASE("mata::nft::lazy – sync_product keeps the concrete synchronized label when matched through DONT_CARE") {
    SymbolicAutomataTree tree;
    const Term left = tree.make_term(relation_single_pair('a', DONT_CARE));
    const Term right = tree.make_term(relation_single_pair('b', 'c'));
    const Term synced = tree.sync_product(
            left, right,
            {1}, {0},
            {
                    LevelRef{LevelRef::Side::Lhs, 0},
                    LevelRef{LevelRef::Side::Lhs, 1},
                    LevelRef{LevelRef::Side::Rhs, 1},
            });
    const Term expected = tree.make_term(relation_single_triple('a', 'b', 'c'));

    CHECK(tree.arity_of(synced) == 3);
    CHECK_FALSE(tree.is_empty(synced));
    CHECK_FALSE(tree.is_empty(tree.intersect(synced, expected)));
}

// ---------------------------------------------------------------------------
// 10. DAG sharing
// ---------------------------------------------------------------------------

TEST_CASE("mata::nft::lazy – shared subterm is evaluated correctly") {
    // intersect(union(a, a), union(a, a)) — the union node is shared.
    SymbolicAutomataTree tree;
    const Term a = tree.make_term(single_symbol_nfa('a'));
    const Term shared = tree.union_(a, a);
    const Term root = tree.intersect(shared, shared);
    CHECK(tree.is_valid(root));
    CHECK_FALSE(tree.is_empty(root));
}

TEST_CASE("mata::nft::lazy – complement of shared subterm under intersect is not empty") {
    // intersect(comp(shared), comp(shared)) — both arms reference the same
    // complement node.  Targets the memoised reconstruction path in v2 and
    // verifies no false cycle error is thrown.
    SymbolicAutomataTree tree;
    const Term a = tree.make_term(single_symbol_nfa('a'));
    const Term b = tree.make_term(single_symbol_nfa('b'));
    const Term shared = tree.union_(a, b);
    const Term comp = tree.complement(shared);
    const Term root = tree.intersect(comp, comp);
    CHECK(tree.is_valid(root));
    CHECK_FALSE(tree.is_empty(root));
}
