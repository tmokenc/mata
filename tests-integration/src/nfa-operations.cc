/**
 * NOTE: Input automata, that are of type `NFA-bits` are mintermized!
 *  - If you want to skip mintermization, set the variable `SKIP_MINTERMIZATION` below to `false`
 */

#include "utils/utils.hh"

#include "mata/nfa/nfa.hh"

#include <iostream>
#include <iomanip>
#include <chrono>
#include <string>

using namespace Mata::Nfa;

const bool SKIP_MINTERMIZATION{ false };

int main(int argc, char *argv[]) {
    if (argc != 2) {
        std::cerr << "Input file missing\n";
        return EXIT_FAILURE;
    }

    std::string filename = argv[1];

    Nfa aut;
    Mata::OnTheFlyAlphabet alphabet{};
    if (load_automaton(filename, aut, alphabet, SKIP_MINTERMIZATION) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    // Setting precision of the times to fixed points and 4 decimal places
    std::cout << std::fixed << std::setprecision(5);

    Nfa trimmed_aut = aut;
    TIME_BEGIN(trim);
    trimmed_aut.trim();
    auto end = std::chrono::system_clock::now();
    TIME_END(trim);

    return EXIT_SUCCESS;
}
