/**
 * Benchmark: Lazy emptiness for inclusion-shaped workloads
 *
 * Reuses the automata inclusion benchmark corpus and measures the lazy symbolic
 * emptiness check corresponding to `is_included(lhs, rhs)`, i.e. emptiness of
 * `lhs ∩ complement(rhs)`.
 *
 * NOTE: Input automata that are of type `NFA-bits` are mintermized.
 */

/**
 * Benchmark: Automata Inclusion (b-armc-incl)
 *
 * The benchmark program reproduces the results of CADE'23 for benchmarks in directory
 * /nfa-bench/benchmarks/automata_inclusion
 *
 * Optimal Inputs: inputs/bench-double-automata-inclusion.in
 *
 * The original benchmark had the following statistics:
 *   1. Average: 1.9s
 *   2. Median: 0.8s
 *   3. Timeouts: 0
 *
 * NOTE: Input automata, that are of type `NFA-bits` are mintermized!
 *  - If you want to skip mintermization, set the variable `MINTERMIZE_AUTOMATA` below to `false`
 */

#include "mata/nft/lazy.hh"
#include "utils/utils.hh"

constexpr bool MINTERMIZE_AUTOMATA{true};

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Input files missing\n";
        return EXIT_FAILURE;
    }

    std::vector<std::string> filenames{argv[1], argv[2]};
    std::vector<Nfa> automata;
    mata::OnTheFlyAlphabet alphabet;
    if (load_automata(filenames, automata, alphabet, MINTERMIZE_AUTOMATA) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    // This might be less-efficient, but more readable.
    Nfa lhs = automata[0];
    Nfa rhs = automata[1];

    ParameterMap params;

    // Setting precision of the times to fixed points and 4 decimal places
    std::cout << std::fixed << std::setprecision(4);

    params["algorithm"] = "naive";
    TIME_BEGIN(automata_inclusion_naive);
    bool naive_result = mata::nfa::is_included(lhs, rhs, &alphabet, params);
    TIME_END(automata_inclusion_naive);

    params["algorithm"] = "antichains";
    TIME_BEGIN(automata_inclusion_antichain);
    bool antichains = mata::nfa::is_included(lhs, rhs, &alphabet, params);
    TIME_END(automata_inclusion_antichain);

    mata::nft::lazy::SymbolicFormula tree;

    auto term_lhs = tree.make_term(lhs);
    auto term_rhs = tree.make_term(rhs);

    TIME_BEGIN(automata_inclusion_lazy);
    bool lazy_result = tree.is_empty(tree.intersect(term_lhs, tree.complement(term_rhs)), alphabet);

    TIME_END(automata_inclusion_lazy);


    if (naive_result != antichains || naive_result != lazy_result) {
        std::cerr << "Results do not match!\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
