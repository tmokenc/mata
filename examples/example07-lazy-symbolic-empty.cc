// example07 - lazy symbolic emptiness over nontrivial NFA/NFT combinations

#include "mata/nfa/nfa.hh"
#include "mata/nft/lazy.hh"
#include "mata/nft/nft.hh"

#include <cassert>
#include <iostream>

using namespace mata;
using namespace mata::nfa;
using namespace mata::nft;
using namespace mata::nft::lazy;

namespace {

Nfa input_words_a_then_any_then_b() {
    // Accepts a(a|b)*b.
    Nfa aut{4};
    aut.initial = {0};
    aut.final = {2};

    aut.delta.add(0, 'a', 1);

    aut.delta.add(1, 'a', 1);
    aut.delta.add(1, 'b', 2);

    aut.delta.add(2, 'a', 1);
    aut.delta.add(2, 'b', 2);

    return aut;
}

Nfa outputs_start_0_end_1() {
    // Accepts 0(0|1)*1.
    Nfa aut{4};
    aut.initial = {0};
    aut.final = {2};

    aut.delta.add(0, '0', 1);

    aut.delta.add(1, '0', 1);
    aut.delta.add(1, '1', 2);

    aut.delta.add(2, '0', 1);
    aut.delta.add(2, '1', 2);

    return aut;
}

Nfa outputs_end_0() {
    Nfa aut{2};
    aut.initial = {0};
    aut.final = {1};

    aut.delta.add(0, '0', 1);
    aut.delta.add(0, '1', 0);
    aut.delta.add(1, '0', 1);
    aut.delta.add(1, '1', 0);

    return aut;
}

Nft parity_sensitive_ab_to_xyz() {
    // Reads words over {a,b}. On each 'a', toggles parity and outputs:
    //   even parity -> x
    //   odd parity  -> y
    // On 'b', outputs z and keeps parity.
    //
    // Level-0 states:
    //   0 = even parity (initial)
    //   1 = odd parity
    // Level-1 states encode the pending output symbol and next parity.
    Nft nft = Nft::with_levels(2, 8, {0}, {0, 1});

    nft.levels[0] = 0;
    nft.levels[1] = 0;
    nft.levels[2] = 1;
    nft.levels[3] = 1;
    nft.levels[4] = 1;
    nft.levels[5] = 1;
    nft.levels[6] = 1;
    nft.levels[7] = 1;

    // even --a/x--> odd
    nft.delta.add(0, 'a', 2);
    nft.delta.add(2, 'x', 1);

    // odd --a/y--> even
    nft.delta.add(1, 'a', 3);
    nft.delta.add(3, 'y', 0);

    // even --b/z--> even
    nft.delta.add(0, 'b', 4);
    nft.delta.add(4, 'z', 0);

    // odd --b/z--> odd
    nft.delta.add(1, 'b', 5);
    nft.delta.add(5, 'z', 1);

    return nft;
}

Nft xyz_to_bits() {
    // x -> 0, y -> 1, z -> 1
    Nft nft = Nft::with_levels(2, 7, {0}, {0});

    nft.levels[0] = 0;
    nft.levels[1] = 1;
    nft.levels[2] = 1;
    nft.levels[3] = 1;
    nft.levels[4] = 0;
    nft.levels[5] = 0;
    nft.levels[6] = 0;

    nft.delta.add(0, 'x', 1);
    nft.delta.add(1, '0', 0);

    nft.delta.add(0, 'y', 2);
    nft.delta.add(2, '1', 0);

    nft.delta.add(0, 'z', 3);
    nft.delta.add(3, '1', 0);

    return nft;
}

} // namespace

int main() {
    SymbolicAutomataTree tree;

    const Term input_lang = tree.make_term(input_words_a_then_any_then_b());
    const Term good_outputs = tree.make_term(outputs_start_0_end_1());
    const Term outputs_ending_in_0 = tree.make_term(outputs_end_0());

    const TermNft first_step = tree.make_term(parity_sensitive_ab_to_xyz());
    const TermNft second_step = tree.make_term(xyz_to_bits());
    const TermNft transduction = tree.compose(first_step, second_step);

    // Symbolic image of the input language through the composed transducer.
    const Term reachable_outputs = tree.post_image(input_lang, transduction);

    // Inclusion via emptiness:
    //   reachable_outputs <= good_outputs
    // iff
    //   reachable_outputs /\ complement(good_outputs) is empty.
    const Term outputs_outside_spec = tree.intersect(reachable_outputs, tree.complement(good_outputs));
    const bool image_is_within_spec = tree.is_empty(outputs_outside_spec);

    // Can the composed transducer produce an output ending in 0?
    const bool can_end_in_0 = !tree.is_empty(tree.intersect(reachable_outputs, outputs_ending_in_0));

    // Pre-image query: which inputs can reach the good output specification?
    const Term inputs_reaching_good_outputs = tree.pre_image(good_outputs, transduction);
    const bool some_input_reaches_good_outputs =
            !tree.is_empty(tree.intersect(input_lang, inputs_reaching_good_outputs));

    assert(tree.is_valid(outputs_outside_spec));

    std::cout << std::boolalpha;
    std::cout << "Every reachable output satisfies 0(0|1)*1: " << image_is_within_spec << '\n';
    std::cout << "Some reachable output ends in 0: " << can_end_in_0 << '\n';
    std::cout << "Some accepted input reaches an output satisfying 0(0|1)*1: " << some_input_reaches_good_outputs
              << '\n';

    return 0;
}
