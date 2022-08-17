/* re2parser.cc -- parser transforming re2 regular expressions to our Nfa
 *
 * Copyright (c) 2022 Michal Horky
 *
 * This file is a part of libmata.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <iostream>

// MATA headers
#include <mata/re2parser.hh>

// RE2 headers
#include <re2/re2/regexp.h>
#include <re2/re2/prog.h>
#include <re2/util/logging.h>


namespace {
    class RegexParser {
    private:
        /**
         * Holds all state cache vectors needed throughout the computation. Vector index is the state number
         * state_mapping for each state (vector index), it holds a vector of states that map to it (cause by epsilon transitions)
         * is_final_state determines if the state is final (true) or not (false)
         * is_state_nop_or_cap determines if the state is of type nop/cap (true) or not (false)
         * is_last determines if the state is last (true), meaning it has epsilon transition to the next state, or not (false)
         * has_state_incoming_edge determines if there is an incoming edge to the state (true) or not (false)
         */
        struct StateCache {
            std::vector<std::vector<Mata::Nfa::State>> state_mapping;
            std::vector<bool> is_final_state;
            std::vector<bool> is_state_nop_or_cap;
            std::vector<bool> is_last;
            std::vector<bool> has_state_incoming_edge;
        };

    public:
        /**
         * Default RE2 options
         */
        RE2::Options options;
        StateCache state_cache;

        std::vector<std::vector<std::pair<int, int>>> outgoingEdges;

        RegexParser() = default;

        /**
         * Creates parsed regex (ie. Regexp*) from string regex_string
         * @param regex_string Regex to be parsed as a string
         * @return Parsed regex as RE2 Regexp*
         */
        re2::Regexp* parse_regex_string(const std::string& regex_string) const {
            re2::RegexpStatus status;

            auto parsed_regex = re2::Regexp::Parse(
                    regex_string,
                    static_cast<re2::Regexp::ParseFlags>(options.ParseFlags()),
                    &status);
            if (parsed_regex == nullptr) {
                if (options.log_errors()) {
                    LOG(ERROR) << "Error parsing '" << regex_string << "': "
                               << status.Text();
                }
                exit(EXIT_FAILURE);
            }
            return parsed_regex;
        }

        /**
         * Converts re2's prog to mata::Nfa::Nfa
         * @param prog Prog* to create mata::Nfa::Nfa from
         * @param use_epsilon whether to create NFA with epsilon transitions or not
         * @param epsilon_value value, that will represent epsilon on transitions
         * @return mata::Nfa::Nfa created from prog
         */
        void convert_pro_to_nfa(Mata::Nfa::Nfa* output_nfa, re2::Prog* prog, bool use_epsilon, int epsilon_value) {
            const int start_state = prog->start();
            const int prog_size = prog->size();
            int empty_flag;
            std::vector<Mata::Nfa::Symbol> symbols;
            Mata::Nfa::Nfa explicit_nfa(prog_size);

            // Vectors are saved in this->state_cache after this
            this->create_state_cache(prog, use_epsilon);

            explicit_nfa.make_initial(this->state_cache.state_mapping[start_state][0]);
            this->state_cache.has_state_incoming_edge[this->state_cache.state_mapping[start_state][0]] = true;

            // Used for epsilon closure, it contains tuples (state_reachable_by_epsilon_transitions, source_state_of_epsilon_transitions)
            std::vector<std::pair<int, int>> copyEdgesFromTo;

            // If the start state is nop or cap, and it is not last, it means that it has a transition to more different
            // states. We are creating a new start state as one of the states reachable by epsilon from the start state.
            // We must also include transitions of the other epsilon reachable states to the new start state.
            if (this->state_cache.is_state_nop_or_cap[start_state] && !this->state_cache.is_last[start_state]) {
                for (int index = 1; index < this->state_cache.state_mapping[start_state].size(); index++) {
                    for (auto state: this->state_cache.state_mapping[this->state_cache.state_mapping[start_state][index]]) {
                        copyEdgesFromTo.emplace_back(state, this->state_cache.state_mapping[start_state][0]);
                    }
                }
            }

            this->outgoingEdges = std::vector<std::vector<std::pair<int, int>>> (prog_size);

            // We traverse all the states and create corresponding states and edges in mata::Nfa::Nfa
            for (int current_state = start_state; current_state < prog_size; current_state++) {
                re2::Prog::Inst *inst = prog->inst(current_state);
                // Every type of state can be final (due to epsilon transition), so we check it regardless of its type
                if (this->state_cache.is_final_state[current_state]) {
                    this->make_state_final(current_state, explicit_nfa);
                }
                switch (inst->opcode()) {
                    default:
                        LOG(DFATAL) << "unhandled " << inst->opcode() << " in convertProgTomata::Nfa::Nfa";
                        break;

                    case re2::kInstMatch:
                        // The kInstMatch type of state is a final state,
                        // but all final states are handled before the switch statement above
                        break;

                    case re2::kInstNop:
                    case re2::kInstCapture:
                        if (use_epsilon) {
                            symbols.push_back(epsilon_value);
                            this->create_explicit_nfa_transitions(current_state, inst, symbols, explicit_nfa, use_epsilon, epsilon_value);
                            symbols.clear();
                        }
                        break;
                    case re2::kInstEmptyWidth:
                        empty_flag = static_cast<int>(inst->empty());
                        // ^ - beginning of line
                        if (empty_flag & re2::kEmptyBeginLine) {
                            // TODO Symbol?
                            symbols.push_back(300);
                        }
                        // $ - end of line
                        if (empty_flag & re2::kEmptyEndLine) {
                            // TODO Symbol?
                            symbols.push_back(10);
                        }
                        // \A - beginning of text
                        if (empty_flag & re2::kEmptyBeginText) {
                            // TODO Symbol?
                            symbols.push_back(301);
                        }
                        // \z - end of text
                        if (empty_flag & re2::kEmptyEndText) {
                            // TODO Symbol?
                            symbols.push_back(302);
                        }
                        // \b - word boundary
                        if (empty_flag & re2::kEmptyWordBoundary) {
                            // TODO Symbol?
                            symbols.push_back(303);
                        }
                        // \B - not \b
                        if (empty_flag & re2::kEmptyNonWordBoundary) {
                            // TODO Symbol?
                            symbols.push_back(304);
                        }
                    // kInstByteRange represents states with a "byte range" on the outgoing transition(s)
                    // (it can also be a single byte)
                    case re2::kInstByteRange:
                        if (symbols.empty()) {
                            // Save all symbols that can be used on the current transition
                            for (long int symbol = inst->lo(); symbol <= inst->hi(); symbol++) {
                                symbols.push_back(symbol);
                            }
                        }
                        this->create_explicit_nfa_transitions(current_state, inst, symbols, explicit_nfa, use_epsilon, epsilon_value);

                        if (!use_epsilon) {
                            // There is an epsilon transition to the currentState+1 we will need to copy transitions of
                            // the currentState+1 to the currentState.
                            if (!this->state_cache.is_last[current_state]) {
                                for (auto state: this->state_cache.state_mapping[current_state + 1]) {
                                    copyEdgesFromTo.emplace_back(state, current_state);
                                }
                            }
                        }
                        symbols.clear();
                        break;
                }
            }
            if (!use_epsilon) {
                // We will traverse the vector in reversed order. Like that, we will also handle chains of epsilon transitions
                // 2 -(eps)-> 3 -(eps)-> 4 -(a)-> 5.... We first need to copy transitions of state 4 to state 3, and then
                // we can copy transition of state 3 (which now have copied transitions of state 4) to state 2
                for (auto copyEdgeFromTo = copyEdgesFromTo.rbegin(); copyEdgeFromTo != copyEdgesFromTo.rend(); copyEdgeFromTo++) {
                    re2::Prog::Inst *inst = prog->inst(copyEdgeFromTo->first);
                    // kInstMatch states in RE2 does not have outgoing edges. The other state will also be final
                    if (inst->opcode() == re2::kInstMatch) {
                        this->make_state_final(copyEdgeFromTo->second, explicit_nfa);
                        this->state_cache.is_final_state[copyEdgeFromTo->second] = true;
                        continue;
                    }
                    // The state is final if there are epsilon transition(s) leading to a final state
                    if (this->state_cache.is_final_state[copyEdgeFromTo->first]) {
                        this->make_state_final(copyEdgeFromTo->second, explicit_nfa);
                        this->state_cache.is_final_state[copyEdgeFromTo->second] = true;
                    }
                    for (auto transition: this->outgoingEdges[copyEdgeFromTo->first]) {
                        // We copy transitions only to states that has incoming edge
                        if (this->state_cache.has_state_incoming_edge[copyEdgeFromTo->second]) {
                            explicit_nfa.add_trans(copyEdgeFromTo->second, transition.first, transition.second);
                        }
                        // However, we still need to save the transitions (we could possibly copy them to another state in
                        // the epsilon closure that has incoming edge)
                        this->outgoingEdges[copyEdgeFromTo->second].emplace_back(transition.first, transition.second);
                    }
                }
            }
            RegexParser::renumber_states(output_nfa, prog_size, explicit_nfa);
        }

    private: // private methods
        /**
         * Creates transitions in the passed ExplicitNFA nfa. Transitions are created for each from statesFrom vector with
         * an incoming edge. Transitions are created for each symbol from symbol vector.
         * @param statesFrom states that will be used as source states
         * @param inst RE2 instruction for the current state, it is used to determine the target state for each transition
         * @param symbols symbols that will be used on each transition
         * @param nfa ExplicitNFA in which the transitions should be created
         * @param use_epsilon whether to create NFA with epsilon transitions or not
         * @param epsilon_value value, that will represent epsilon on transitions
         */
        void create_explicit_nfa_transitions(int currentState, re2::Prog::Inst *inst,
                                                       const std::vector<Mata::Nfa::Symbol>& symbols,
                                                       Mata::Nfa::Nfa &nfa, bool use_epsilon, int epsilon_value) {
            for (auto mappedState: this->state_cache.state_mapping[currentState]) {
                for (auto mappedTargetState: this->state_cache.state_mapping[inst->out()]) {
                    // There can be more symbols on the edge
                    for (auto symbol: symbols) {
                        if (!use_epsilon) {
                            // Save all outgoing edges. The vector will be used to get rid of epsilon transitions
                            this->outgoingEdges[mappedState].push_back({symbol, mappedTargetState});
                        }
                        if (this->state_cache.has_state_incoming_edge[mappedState]) {
                            this->state_cache.has_state_incoming_edge[mappedTargetState] = true;
                            nfa.add_trans(mappedState, symbol, mappedTargetState);
                        }
                    }
                }
            }
            if (use_epsilon) {
                // There is an epsilon transition to the currentState+1, so we must handle it
                if (!this->state_cache.is_last[currentState]) {
                    nfa.add_trans(currentState, epsilon_value, currentState + 1);
                }
            }
        }

       /**
        * Creates all state cache vectors needed throughout the computation and saves them to the private variable state_cache.
        * It calls appropriate method based on use_epsilon param
        * @param prog RE2 prog corresponding to the parsed regex
        * @param use_epsilon whether to create NFA with epsilon transitions or not
        */
       void create_state_cache(re2::Prog *prog, bool use_epsilon) {
            if (use_epsilon) {
                this->create_state_cache_with_epsilon(prog);
            } else {
                this->create_state_cache_without_epsilon(prog);
            }
        }

        /**
         * Creates all state cache vectors needed throughout the computation and saves them
         * to the private variable state_cache
         * It creates state cache for creating ExplicitNFA without epsilon transitions
         * @param prog RE2 prog corresponding to the parsed regex
         */
        void create_state_cache_without_epsilon(re2::Prog *prog) {
            std::vector<bool> default_false_vec(prog->size(), false);
            this->state_cache = {
                // state_mapping holds states that map to each state (index) due to epsilon transitions
                {},
                // is_final_state holds true for states that are final, false for the rest
                default_false_vec,
                // is_state_nop_or_cap holds true for states that have type nop or cap, false for the rest
                default_false_vec,
                // is_last holds true for states that are last, false for the rest
                default_false_vec,
                // has_state_incoming_edge holds true for states with an incoming edge, false for the rest
                default_false_vec,
            };
            const int start_state = prog->start();
            const int prog_size = prog->size();

            // Used for the first loop through states
            std::vector<Mata::Nfa::State> tmp_state_mapping(prog_size);
            for (Mata::Nfa::State state = 0; state < prog_size; state++) {
                tmp_state_mapping[state] = state;
                this->state_cache.state_mapping.push_back({state});
            }

            // When there is nop or capture type of state, we will be appending to it
            int append_to_state = -1;
            Mata::Nfa::State mapped_parget_state;

            for (Mata::Nfa::State state = start_state; state < prog_size; state++) {
                re2::Prog::Inst *inst = prog->inst(state);
                if (inst->last()) {
                    this->state_cache.is_last[state] = true;
                }

                if (inst->opcode() == re2::kInstCapture || inst->opcode() == re2::kInstNop) {
                    this->state_cache.state_mapping[state] = this->get_mapped_states(prog, state, inst);
                    this->state_cache.is_state_nop_or_cap[state] = true;
                    mapped_parget_state = tmp_state_mapping[static_cast<Mata::Nfa::State>(inst->out())];
                    tmp_state_mapping[state] = mapped_parget_state;
                    if (append_to_state != -1) {
                        // Nop or capture type of state may or may not have an incoming edge, the target state should have
                        // it only if the current state has it
                        if (this->state_cache.has_state_incoming_edge[state]) {
                            this->state_cache.has_state_incoming_edge[mapped_parget_state] = true;
                        }
                        tmp_state_mapping[append_to_state] = mapped_parget_state;
                    } else {
                        append_to_state = state;
                    }
                } else if (inst->opcode() == re2::kInstMatch) {
                    this->state_cache.is_final_state[state] = true;
                    append_to_state = -1;
                } else {
                    // Other types of states will always have an incoming edge so the target state will always have it too
                    this->state_cache.has_state_incoming_edge[inst->out()] = true;
                    append_to_state = -1;
                }
            }
        }

        /**
          * Creates all state cache vectors needed throughout the computation and saves them to the private variable state_cache.
          * It creates state cache for creating ExplicitNFA with epsilon transitions
          * @param prog RE2 prog corresponding to the parsed regex
          */
        void create_state_cache_with_epsilon(re2::Prog *prog) {
            std::vector<bool> defaultFalseVec(prog->size(), false);
            std::vector<bool> defaultTrueVec(prog->size(), true);
            this->state_cache = {
                    {}, // stateMapping all states are mapped to itself when using epsilon transitions
                    defaultFalseVec, // is_final_state holds true for states that are final, false for the rest
                    defaultFalseVec, // is_state_nop_or_cap not used when using epsilon transition
                    defaultFalseVec, // is_last holds true for states that are last, false for the rest
                    defaultTrueVec, // has_state_incoming_edge holds true all states
            };
            const int progSize = prog->size();

            for (Mata::Nfa::State state = 0; state < progSize; state++) {
                this->state_cache.state_mapping.push_back({state});
                re2::Prog::Inst *inst = prog->inst(state);
                if (inst->last()) {
                    this->state_cache.is_last[state] = true;
                }
                if (inst->opcode() == re2::kInstMatch) {
                    this->state_cache.is_final_state[state] = true;
                }
            }
        }

        /**
         * Makes all states mapped to the state parameter final in the mata::Nfa::Nfa
         * @param state State which should be made final
         * @param nfa mata::Nfa::Nfa in which the states will be made final
         */
        void make_state_final(int state, Mata::Nfa::Nfa &nfa) {
            for (auto target_state: this->state_cache.state_mapping[state]) {
                // States without an incoming edge should not be in the automata
                if (!this->state_cache.has_state_incoming_edge[target_state]) {
                    continue;
                }
                nfa.make_final(target_state);
            }
        }

        /**
         * Renumbers the states of the input_nfa to be from <0, numberOfStates>
         * @param program_size Size of the RE2 prog
         * @param input_nfa mata::Nfa::Nfa which states should be renumbered
         * @return Same mata::Nfa::Nfa as input_nfa but with states from interval <0, numberOfStates>
         */
        static Mata::Nfa::Nfa renumber_states(Mata::Nfa::Nfa* output_nfa,
                                              int program_size,
                                              Mata::Nfa::Nfa &input_nfa) {
            std::vector<unsigned long> renumbered_states(program_size, -1);
            Mata::Nfa::Nfa& renumbered_explicit_nfa = *output_nfa;
            for (int state = 0; state < program_size; state++) {
                const auto& transition_list = input_nfa.get_transitions_from_state(state);
                // If the transition list is empty, the state is not used
                if (transition_list.empty()) {
                    continue;
                } else {
                    // addNewState returns next unused state of the new NFA, so we map it to the original state
                    renumbered_states[state] = renumbered_explicit_nfa.add_new_state();
                }
            }

            for (auto state: input_nfa.finalstates) {
                if (static_cast<int>(renumbered_states[state]) == -1) {
                    renumbered_states[state] = renumbered_explicit_nfa.add_new_state();
                }
                renumbered_explicit_nfa.make_final(renumbered_states[state]);
            }

            for (int state = 0; state < program_size; state++) {
                const auto& transition_list = input_nfa.get_transitions_from_state(state);
                for (const auto& transition: transition_list) {
                    for (auto stateTo: transition.states_to) {
                        if (static_cast<int>(renumbered_states[stateTo]) == -1) {
                            renumbered_states[stateTo] = renumbered_explicit_nfa.add_new_state();
                        }
                        assert(renumbered_states[state] <= renumbered_explicit_nfa.get_num_of_states());
                        assert(renumbered_states[stateTo] <= renumbered_explicit_nfa.get_num_of_states());
                        renumbered_explicit_nfa.add_trans(renumbered_states[state], transition.symbol,
                                                          renumbered_states[stateTo]);
                    }
                }
            }

            for (auto state: input_nfa.initialstates) {
                renumbered_explicit_nfa.make_initial(renumbered_states[state]);
            }

            return renumbered_explicit_nfa;
        }

        /**
         * Gets all states that are mapped to the state (i.e., states that are within epsilon transitions chain)
         * @param prog RE2 prog corresponding to the parsed regex
         * @param state State for which the mapped states should be computed
         * @param inst RE2 instruction for the state
         * @return All states that are mapped to the state
         */
        std::vector<Mata::Nfa::State> get_mapped_states(
                re2::Prog* prog, int state, re2::Prog::Inst *inst) {
            std::vector<Mata::Nfa::State> mappedStates;
            std::vector<Mata::Nfa::State> statesToCheck;
            std::set<Mata::Nfa::State> checkedStates;

            statesToCheck.push_back(state);
            while (!statesToCheck.empty()) {
                state = statesToCheck.back();
                inst = prog->inst(state);
                checkedStates.insert(state);
                statesToCheck.pop_back();
                // If the state is not last, it also has an epsilon transition which we must follow
                if (!inst->last()) {
                    re2::Prog::Inst *nextInst = prog->inst(state + 1);
                    if (nextInst->last()) {
                        this->state_cache.is_last[state + 1] = true;
                    }
                    if (checkedStates.count(state+1) == 0) {
                        statesToCheck.push_back(state+1);
                    }
                } else if (inst->opcode() != re2::kInstCapture && inst->opcode() != re2::kInstNop) {
                    // It is state with "normal" transition. It is the last state in the epsilon transitions chain
                    mappedStates.push_back(state);
                    continue;
                }
                re2::Prog::Inst *outInst = prog->inst(inst->out());
                if (outInst->opcode() == re2::kInstCapture || outInst->opcode() == re2::kInstNop) {
                    // The state has outgoing epsilon transition which we must follow
                    if (checkedStates.count(inst->out()) == 0) {
                        statesToCheck.push_back(inst->out());
                    }
                } else {
                    // It is state with "normal" transition. It is the last state in the epsilon transitions chain
                    mappedStates.push_back(inst->out());
                }
            }
            return mappedStates;
        }
        };
}

 /**
 * The main method, it creates NFA from regex
 * @param pattern regex as string
 * @param use_epsilon whether to create NFA with epsilon transitions or not
 * @param epsilon_value value, that will represent epsilon on transitions
 * @return mata::Nfa::Nfa corresponding to pattern
 */
void Mata::RE2Parser::create_nfa(Nfa::Nfa* nfa, const std::string& pattern, bool use_epsilon, int epsilon_value) {
    if (nfa == NULL) {
        throw std::runtime_error("create_nfa: nfa should not be NULL");
    }

    RegexParser regexParser{};
    auto parsed_regex = regexParser.parse_regex_string(pattern);
    auto program = parsed_regex->CompileToProg(regexParser.options.max_mem() * 2 / 3);
    regexParser.convert_pro_to_nfa(nfa, program, use_epsilon, epsilon_value);
    delete program;
    // Decrements reference count and deletes object if the count reaches 0
    parsed_regex->Decref();
}