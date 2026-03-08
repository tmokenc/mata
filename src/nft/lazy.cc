/** @file
 * @brief Lazy on-the-fly emptiness checking for symbolic combinations of NFAs and (2-level) NFTs.
 *
 *
 *
 */

#include "mata/nft/lazy.hh"
#include <mata/simlib/explicit_lts.hh>
#include "mata/alphabet.hh"
#include "mata/nfa/algorithms.hh"
#include "mata/nfa/delta.hh"
#include "mata/nfa/types.hh"
#include "mata/nft/algorithms.hh"
#include "mata/nft/types.hh"

#include <bitset>
#include <cstdlib>
#include <list>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace mata::nft::lazy;

bool SymbolicAutomataTree::is_valid() const {
    return true; // TODO
}

Term SymbolicAutomataTree::make_term(const nfa::Nfa& nfa) {
    const NodeId nfa_id = static_cast<NodeId>(nfas.size());
    nfas.push_back(nfa);

    return Term{insert_node(NodeKind::LeafNfa, nfa_id)};
}

TermNft SymbolicAutomataTree::make_term(const nft::Nft& nft) {
    // Should be removed if we want to support NFTs with more than 2 levels,
    // but for now we only support 2-level NFTs in the symbolic automata tree,
    // which should be enough for most of the use cases we have in mind (e.g. regular model checking)
    if (nft.levels.num_of_levels != 2) {
        throw std::invalid_argument("Only 2-level NFTs are supported in the symbolic automata tree");
    }

    const NodeId nft_id = static_cast<NodeId>(nfts.size());
    nfts.push_back(nft);

    return TermNft{insert_node(NodeKind::LeafNft, nft_id)};
}

Term SymbolicAutomataTree::union_(const Term& lhs, const Term& rhs) {
    return Term{insert_node(NodeKind::Union, lhs.get_id(), rhs.get_id())};
}

Term SymbolicAutomataTree::intersect(const Term& lhs, const Term& rhs) {
    return Term{insert_node(NodeKind::Intersect, lhs.get_id(), rhs.get_id())};
}

Term SymbolicAutomataTree::complement(const Term& sub) { return Term{insert_node(NodeKind::Complement, sub.get_id())}; }

TermNft SymbolicAutomataTree::complement(const TermNft& sub) {
    return TermNft{insert_node(NodeKind::ComplementNft, sub.get_id())};
}

Term SymbolicAutomataTree::pre_image(const Term& lang_over_output, const TermNft& transducer) {
    return Term{insert_node(NodeKind::PreImage, lang_over_output.get_id(), transducer.get_id())};
}

Term SymbolicAutomataTree::post_image(const Term& lang_over_input, const TermNft& transducer) {
    return Term{insert_node(NodeKind::PostImage, lang_over_input.get_id(), transducer.get_id())};
}

TermNft SymbolicAutomataTree::compose(const TermNft& lhs, const TermNft& rhs) {
    return TermNft{insert_node(NodeKind::Compose, lhs.get_id(), rhs.get_id())};
}

struct PairState {
    MacroStateId lhs;
    MacroStateId rhs;
};

struct TaggedState {
    enum class Tag : uint8_t {
        Left = 0,
        Right = 1,
    };

    MacroStateId state;
    Tag tag;
};

using SetState = std::unordered_set<MacroStateId>;
using BitsetState = std::bitset<64>;

uint32_t hash_states(const SetState& states) {
    uint32_t hash = 0;
    for (const MacroStateId& id : states) {
        // order does not matter for sets, so we can just xor the hashes of the elements
        hash ^= id;
    }
    return hash;
}

uint32_t hash_pair(const PairState& pair) { return pair.lhs ^ pair.rhs; }

uint32_t hash_tagged(const TaggedState& tagged) { return tagged.state ^ (static_cast<uint32_t>(tagged.tag) << 31); }

uint32_t hash_bitset(const BitsetState& bitset) {
    // we can just use the built-in hash function for bitsets
    size_t hash = std::hash<BitsetState>()(bitset);
    return static_cast<uint32_t>(hash);
}

// TODO: optimize this
struct MacroStateStore {
    using PairStore = std::unordered_map<MacroStateId, PairState>;
    using SetStore = std::unordered_map<MacroStateId, SetState>;
    using TaggedStore = std::unordered_map<MacroStateId, TaggedState>;
    using BitsetStore = std::unordered_map<MacroStateId, BitsetState>;

    // Index to the store
    // The i-th node is the index to this, which will point to the store for the i-th node
    // type of store is determined by the node kind
    std::vector<size_t> index_pair_stores;

    std::vector<PairStore> pair_stores;
    std::vector<SetStore> set_stores;
    std::vector<TaggedStore> tagged_stores;
    std::vector<BitsetStore> bitset_stores;

    MacroStateStore() = default;

    // Initialize the macro state store for the given nodes. For each node, we will create a store for its macro states
    // and store the index of the store in index_pair_stores. The type of store is determined by the node kind:
    // - For leaf nodes (LeafNfa and LeafNft), do not need to create a store
    // - For union nodes, create a tagged store
    // - For intersect, preimage, postimage, compose nodes, create a pair store
    // - For complement nodes, create a set store
    MacroStateStore(const std::vector<Node>& nodes)
        : index_pair_stores(nodes.size(), 0), pair_stores{}, set_stores{}, tagged_stores{}, bitset_stores{} {
        for (size_t i = 0; i < nodes.size(); i++) {
            Node node = nodes[i];

            switch (node.kind) {
                case NodeKind::Union: {
                    index_pair_stores.push_back(tagged_stores.size());
                    tagged_stores.emplace_back();
                    break;
                }

                case NodeKind::Intersect:
                case NodeKind::PreImage:
                case NodeKind::PostImage:
                case NodeKind::Compose: {
                    index_pair_stores.push_back(pair_stores.size());
                    pair_stores.emplace_back();
                    break;
                }

                case NodeKind::Complement:
                case NodeKind::ComplementNft: {
                    index_pair_stores.push_back(set_stores.size());
                    set_stores.emplace_back();
                    break;
                }

                case NodeKind::LeafNfa:
                case NodeKind::LeafNft:
                    break;
            }
        }
    }

    PairState get_pair(const uint32_t idx, const MacroStateId& id) {
        PairStore& store = pair_stores[index_pair_stores[idx]];
        return store[id];
    }

    SetState get_set(const uint32_t idx, const MacroStateId& id) {
        SetStore& store = set_stores[index_pair_stores[idx]];
        return store[id];
    }

    TaggedState get_tagged(const uint32_t idx, const MacroStateId& id) {
        TaggedStore& store = tagged_stores[index_pair_stores[idx]];
        return store[id];
    }

    BitsetState get_bitset(const uint32_t idx, const MacroStateId& id) {
        BitsetStore& store = bitset_stores[index_pair_stores[idx]];
        return store[id];
    }

    // Intern a macro state and return its id. If the state already exists, return the existing id.
    MacroStateId intern(const uint32_t idx, const std::unordered_set<MacroStateId>&& states) {
        SetStore& store = set_stores[index_pair_stores[idx]];
        MacroStateId id = hash_states(states);

        // Check for the states with the same hash, if they are the same as the input states, return their id,
        // otherwise keep looking for the next id until we find an empty slot or the same states.
        while (store.contains(id)) {
            if (store[id] == states) {
                return id;
            }
            id++;
        }

        store.emplace(id, states);
        return id;
    }

    MacroStateId intern(const uint32_t idx, const PairState&& pair) {
        PairStore& store = pair_stores[index_pair_stores[idx]];
        MacroStateId id = hash_pair(pair);

        while (store.contains(id)) {
            if (store[id].lhs == pair.lhs && store[id].rhs == pair.rhs) {
                return id;
            }
            id++;
        }

        store.emplace(id, pair);
        return id;
    }

    MacroStateId intern(const uint32_t idx, const TaggedState&& tagged) {
        TaggedStore& store = tagged_stores[index_pair_stores[idx]];
        MacroStateId id = hash_tagged(tagged);

        while (store.contains(id)) {
            if (store[id].state == tagged.state && store[id].tag == tagged.tag) {
                return id;
            }
            id++;
        }

        store.emplace(id, tagged);
        return id;
    }

    MacroStateId intern(const uint32_t idx, const std::bitset<64>&& bitset) {
        BitsetStore& store = bitset_stores[index_pair_stores[idx]];
        MacroStateId id = hash_bitset(bitset);

        while (store.contains(id)) {
            if (store[id] == bitset) {
                return id;
            }
            id++;
        }

        store.emplace(id, bitset);
        return id;
    }
};

/// Node reconstruction
/// Create a new node tree (in vector) with only reachable nodes from the root.
/// Also try to push the complement down as much as possible,
/// so that the resulting tree is more suitable for on-the-fly emptiness checking.
NodeId reconstruct_nodes(
        const std::vector<Node>& original_nodes, NodeId id, std::vector<Node> output,
        std::unordered_set<NodeId> visited = {}) {
    if (visited.contains(id)) {
        // Error: cycle detected in the original tree
        // It is possible that it a false positive as if this can be a DAG instead of a tree,
        // meaning that a node can have multiple parents,
        // then we will visit the same node multiple times and it will be detected as a cycle.
        //
        // However, for simplicity, let's just assume that the input is a tree for now
        // In case we want to support DAGs, this part need to be changed
        // to check for cycles in the DAG instead of just checking for visited nodes.
        throw std::runtime_error("Cycle detected in the symbolic automata tree");
    }

    Node node = original_nodes[id];

    switch (node.kind) {
        case NodeKind::LeafNfa:
        case NodeKind::LeafNft: {
            output.push_back({node.kind, node.lhs, 0});
            break;
        }

        case NodeKind::Union:
        case NodeKind::Intersect:
        case NodeKind::PreImage:
        case NodeKind::PostImage:
        case NodeKind::Compose: {
            NodeId lhs_id = reconstruct_nodes(original_nodes, node.lhs, output, visited);
            NodeId rhs_id = reconstruct_nodes(original_nodes, node.rhs, output, visited);

            output.push_back({node.kind, lhs_id, rhs_id});
            break;
        }

        case NodeKind::Complement: {
            Node child_node = original_nodes[node.lhs];

            switch (child_node.kind) {
                case NodeKind::Union: {
                    // De Morgan's law: complement of union is intersect of complements
                    NodeId lhs_id = reconstruct_nodes(original_nodes, child_node.lhs, output, visited);
                    NodeId rhs_id = reconstruct_nodes(original_nodes, child_node.rhs, output, visited);

                    NodeId complement_lhs_id = static_cast<NodeId>(output.size());
                    NodeId complement_rhs_id = complement_lhs_id + 1;

                    output.push_back({NodeKind::Complement, lhs_id, 0});
                    output.push_back({NodeKind::Complement, rhs_id, 0});
                    output.push_back({NodeKind::Intersect, complement_lhs_id, complement_rhs_id});
                    break;
                }

                case NodeKind::Intersect: {
                    // De Morgan's law: complement of intersect is union of complements
                    NodeId lhs_id = reconstruct_nodes(original_nodes, child_node.lhs, output, visited);
                    NodeId rhs_id = reconstruct_nodes(original_nodes, child_node.rhs, output, visited);

                    NodeId complement_lhs_id = static_cast<NodeId>(output.size());
                    NodeId complement_rhs_id = complement_lhs_id + 1;

                    output.push_back({NodeKind::Complement, lhs_id, 0});
                    output.push_back({NodeKind::Complement, rhs_id, 0});
                    output.push_back({NodeKind::Union, complement_lhs_id, complement_rhs_id});
                    break;
                }

                case NodeKind::Complement: {
                    // Double negation: complement of complement is the original node
                    visited.insert(id);
                    return reconstruct_nodes(original_nodes, child_node.lhs, output, visited);
                }

                default:
                    // For other kinds of nodes, we cannot push the complement down,
                    // so we just keep the complement at the current level.
                    NodeId child_id = reconstruct_nodes(original_nodes, node.lhs, output, visited);
                    output.push_back({NodeKind::Complement, child_id, 0});
                    break;
            }

            break;
        }

        case NodeKind::ComplementNft: {
            Node child_node = original_nodes[node.lhs];

            if (child_node.kind == NodeKind::ComplementNft) {
                // Double negation: complement of complement is the original node
                visited.insert(id);
                return reconstruct_nodes(original_nodes, child_node.lhs, output, visited);
            }

            NodeId id = reconstruct_nodes(original_nodes, node.lhs, output, visited);
            output.push_back({NodeKind::ComplementNft, id, 0});
            break;
        }
    }

    visited.insert(id);
    return static_cast<NodeId>(output.size() - 1);
}

struct Context {
    using Nfa = mata::nfa::Nfa;
    using Nft = mata::nft::Nft;
    using State = mata::nfa::State;

    // immutable references to std::vector<Nfa> and std::vector<nft::Nft> in SymbolicAutomataTree,
    const std::vector<Nfa>& nfas;
    const std::vector<Nft>& nfts;

    // Nodes of the symbolic automata tree
    // This one is reconstructed from the nodes in the SymbolicAutomataTree
    // to simplify some of the operations, so it not necessarily the same as the nodes in the SymbolicAutomataTree.
    std::vector<Node> nodes;

    // Storage for macro states.
    MacroStateStore macro_store;

    // Resolved alphabets for each node.
    // For leaf nodes, this is just the alphabet of the NFA/NFT.
    std::vector<mata::OnTheFlyAlphabet> alphabets;

    // Precomputed simulations for each leaf node.
    // The size will be nfas.size() + nfts.size(),
    // the order will be the same as in nfas and nfts (first all NFAs, then all NFTs).
    std::vector<Simlib::Util::BinaryRelation> precomputed_simulations;

    NodeId root_id;

    Context();

    Context(const SymbolicAutomataTree& tree, NodeId root)
        : nfas(tree.nfas), nfts(tree.nfts), nodes{}, macro_store{}, alphabets{}, precomputed_simulations{} {
        root_id = reconstruct_nodes(tree.nodes, root, nodes);
        macro_store = MacroStateStore(nodes);

        // resolve the alphabets for every reachable node,
        // also precompute the simulation on reachable base automata, which will be used for subsumption checking.
        alphabets.reserve(nfas.size() + nfts.size());
        precomputed_simulations.reserve(nfas.size() + nfts.size());

        std::list<NodeId> worklist = {root_id};
        std::unordered_set<NodeId> processed = {};

        while (!worklist.empty()) {
            NodeId node_id = worklist.front();
            worklist.pop_front();

            Node node = nodes[node_id];

            switch (node.kind) {
                case NodeKind::LeafNfa: {
                    const Nfa& nfa = nfas[node.lhs];
                    alphabets[node_id] = create_alphabet(nfa);
                    precomputed_simulations.push_back(mata::nfa::algorithms::compute_relation(nfa));
                    processed.insert(node_id);
                    break;
                }

                case NodeKind::LeafNft: {
                    const Nft& nft = nfts[node.lhs];
                    alphabets[node_id] = create_alphabet(nft);
                    precomputed_simulations.push_back(mata::nft::algorithms::compute_relation(nft));
                    processed.insert(node_id);
                    break;
                }

                case NodeKind::Union:
                case NodeKind::Intersect:
                case NodeKind::PreImage:
                case NodeKind::PostImage:
                case NodeKind::Compose: {
                    if (processed.contains(node.lhs) && processed.contains(node.rhs)) {
                        // if both sides are visited, we can resolve the alphabet for this node and do not need to add
                        // it to the worklist.
                        const mata::OnTheFlyAlphabet& alphabet_lhs = alphabets[node.lhs];
                        const mata::OnTheFlyAlphabet& alphabet_rhs = alphabets[node.rhs];

                        mata::OnTheFlyAlphabet combined_alphabet{};

                        for (const mata::Symbol sym : alphabet_lhs.get_alphabet_symbols()) {
                            std::string name = alphabet_lhs.reverse_translate_symbol(sym);
                            combined_alphabet.try_add_new_symbol(name, combined_alphabet.get_next_value());
                        }

                        for (const mata::Symbol sym : alphabet_rhs.get_alphabet_symbols()) {
                            std::string name = alphabet_rhs.reverse_translate_symbol(sym);
                            combined_alphabet.try_add_new_symbol(name, combined_alphabet.get_next_value());
                        }

                        alphabets[node_id] = combined_alphabet;
                        processed.insert(node_id);
                    } else {
                        worklist.push_back(node.lhs);
                        worklist.push_back(node.rhs);
                    }
                    break;
                }

                case NodeKind::Complement:
                case NodeKind::ComplementNft: {
                    if (processed.contains(node.lhs)) {
                        // the alphabet of the complement node is the same as the alphabet of its child.
                        alphabets[node_id] = alphabets[node.lhs];
                        processed.insert(node_id);
                    } else {
                        worklist.push_back(node.lhs);
                    }

                    break;
                }
            }
        }
    };

    // TODO: should make this lazy by returning an iterator?
    // It is possible that the number of initial macro states to be very large.
    // so we can generate them on the fly when we need them.
    std::list<MacroStateId> initial_macro_states(const Term& term) {
        std::list<MacroStateId> initial_states = {};

        NodeId node_id = term.get_id();
        Node node = nodes[node_id];

        switch (node.kind) {
            case NodeKind::LeafNfa: {
                const Nfa& nfa = nfas[node.lhs];
                for (const State initial_state : nfa.initial) {
                    // State is unsigned int, which is the same as MacroStateId
                    initial_states.push_back(static_cast<MacroStateId>(initial_state));
                }

                break;
            }

            case NodeKind::LeafNft: {
                const Nft& nft = nfts[node.lhs];
                for (const State initial_state : nft.initial) {
                    // State is unsigned int, which is the same as MacroStateId
                    initial_states.push_back(static_cast<MacroStateId>(initial_state));
                }
                break;
            }

            // For union, it is the disjoint union of the initial states of the two subterms,
            // tagged with which side they come from (left or right).
            case NodeKind::Union: {
                for (const MacroStateId& lhs_id : initial_macro_states(Term{node.lhs})) {
                    TaggedState tagged{lhs_id, TaggedState::Tag::Left};
                    MacroStateId id = macro_store.intern(node_id, std::move(tagged));
                    initial_states.push_back(id);
                }

                for (const MacroStateId& rhs_id : initial_macro_states(Term{node.rhs})) {
                    TaggedState tagged{rhs_id, TaggedState::Tag::Right};
                    MacroStateId id = macro_store.intern(node_id, std::move(tagged));
                    initial_states.push_back(id);
                }

                break;
            }

            // For intersection, compose, it is the Cartesian product of the initial states of the two subterms.
            case NodeKind::Intersect:
            case NodeKind::PreImage:
            case NodeKind::PostImage:
            case NodeKind::Compose: {
                for (const MacroStateId& lhs_id : initial_macro_states(Term{node.lhs})) {
                    for (const MacroStateId& rhs_id : initial_macro_states(Term{node.rhs})) {
                        PairState pair{lhs_id, rhs_id};
                        MacroStateId id = macro_store.intern(node_id, std::move(pair));
                        initial_states.push_back(id);
                    }
                }

                break;
            }

            // For complement, we do determinization on the fly, so the initial states
            // are just one macrostate that contains the all initial states of the subterm.
            case NodeKind::Complement:
            case NodeKind::ComplementNft: {
                std::list<MacroStateId> sub_initial_states = initial_macro_states(Term{node.lhs});
                // Convert the set to unordered_set for complmenet's macro state store
                std::unordered_set<MacroStateId> sub_initial_states_set(
                        sub_initial_states.begin(), sub_initial_states.end());

                MacroStateId id = macro_store.intern(node_id, std::move(sub_initial_states_set));
                initial_states.push_back(id);
                break;
            }
        }

        return initial_states;
    }

    // Get the next states from the given state on the given symbol.
    // Similar to initial_macro_states, this function should also be lazy.
    // The sym2 is used when transducer is involved, which is the symbol on the
    // output side of the transducer.
    std::list<MacroStateId>
    next_macro_states(const NodeId& id, const MacroStateId& state, mata::Symbol sym, mata::Symbol sym2 = 0) {
        std::list<MacroStateId> next_states = {};

        switch (nodes[id].kind) {
            case NodeKind::LeafNfa: {
                Nfa nfa = nfas[nodes[id].lhs];
                State s = static_cast<State>(state);

                for (State next_state : nfa.delta.get_successors(s, sym)) {
                    next_states.push_back(static_cast<MacroStateId>(next_state));
                }

                break;
            }
            case NodeKind::LeafNft: {
                Nft nft = nfts[nodes[id].lhs];
                State s = static_cast<State>(state);

                // Move 2 steps in the NFT, first on sym on the input side, then on sym2 on the output side.
                for (State next_state : nft.delta.get_successors(s, sym)) {
                    for (State next_state2 : nft.delta.get_successors(next_state, sym2)) {
                        next_states.push_back(static_cast<MacroStateId>(next_state2));
                    }
                }

                break;
            }


            case NodeKind::Union: {
                TaggedState tagged = macro_store.get_tagged(id, state);
                NodeId next_node_id = (tagged.tag == TaggedState::Tag::Left) ? nodes[id].lhs : nodes[id].rhs;

                for (const MacroStateId& next_state : next_macro_states(next_node_id, tagged.state, sym)) {
                    TaggedState next_tagged{next_state, tagged.tag};
                    MacroStateId next_id = macro_store.intern(id, std::move(next_tagged));
                    next_states.push_back(next_id);
                }

                break;
            }


            case NodeKind::Intersect: {
                PairState pair = macro_store.get_pair(id, state);

                std::list<MacroStateId> next_lhs_states = next_macro_states(nodes[id].lhs, pair.lhs, sym);
                std::list<MacroStateId> next_rhs_states = next_macro_states(nodes[id].rhs, pair.rhs, sym);

                for (const MacroStateId& next_lhs_state : next_lhs_states) {
                    for (const MacroStateId& next_rhs_state : next_rhs_states) {
                        PairState next_pair{next_lhs_state, next_rhs_state};
                        MacroStateId next_id = macro_store.intern(id, std::move(next_pair));
                        next_states.push_back(next_id);
                    }
                }

                break;
            }


            case NodeKind::PreImage: {
                PairState pair = macro_store.get_pair(id, state);

                NodeId nfa_id = nodes[id].lhs;
                NodeId nft_id = nodes[id].rhs;

                for (const mata::Symbol sync_sym : alphabets[nft_id].get_alphabet_symbols()) {
                    std::list<MacroStateId> next_lhs_states = next_macro_states(nfa_id, pair.lhs, sync_sym);
                    std::list<MacroStateId> next_rhs_states = next_macro_states(nft_id, pair.rhs, sync_sym, sym);

                    for (const MacroStateId& next_lhs_state : next_lhs_states) {
                        for (const MacroStateId& next_rhs_state : next_rhs_states) {
                            PairState next_pair{next_lhs_state, next_rhs_state};
                            MacroStateId next_id = macro_store.intern(id, std::move(next_pair));
                            next_states.push_back(next_id);
                        }
                    }
                }


                break;
            }

            case NodeKind::PostImage: {
                PairState pair = macro_store.get_pair(id, state);

                NodeId nfa_id = nodes[id].lhs;
                NodeId nft_id = nodes[id].rhs;

                for (const mata::Symbol sync_sym : alphabets[nft_id].get_alphabet_symbols()) {
                    std::list<MacroStateId> next_lhs_states = next_macro_states(nfa_id, pair.lhs, sync_sym);
                    std::list<MacroStateId> next_rhs_states = next_macro_states(nft_id, pair.rhs, sym, sync_sym);

                    for (const MacroStateId& next_lhs_state : next_lhs_states) {
                        for (const MacroStateId& next_rhs_state : next_rhs_states) {
                            PairState next_pair{next_lhs_state, next_rhs_state};
                            MacroStateId next_id = macro_store.intern(id, std::move(next_pair));
                            next_states.push_back(next_id);
                        }
                    }
                }


                break;
            }

            case NodeKind::Compose: {
                PairState pair = macro_store.get_pair(id, state);

                for (const mata::Symbol sync_sym : alphabets[nodes[id].rhs].get_alphabet_symbols()) {
                    std::list<MacroStateId> next_lhs = next_macro_states(nodes[id].lhs, pair.lhs, sym, sync_sym);
                    std::list<MacroStateId> next_rhs = next_macro_states(nodes[id].rhs, pair.rhs, sync_sym, sym2);

                    for (const MacroStateId& next_lhs_state : next_lhs) {
                        for (const MacroStateId& next_rhs_state : next_rhs) {
                            PairState next_pair{next_lhs_state, next_rhs_state};
                            MacroStateId next_id = macro_store.intern(id, std::move(next_pair));
                            next_states.push_back(next_id);
                        }
                    }
                }


                break;
            }

            case NodeKind::Complement: {
                std::unordered_set<MacroStateId> sub_states = macro_store.get_set(id, state);
                std::unordered_set<MacroStateId> next_sub_states = {};

                const MacroStateId shrinked_state = macro_store.intern(id, std::move(next_sub_states));

                for (const MacroStateId& sub_state : sub_states) {
                    std::list<MacroStateId> next_sub_states_list = next_macro_states(nodes[id].lhs, sub_state, sym);
                    if (next_sub_states_list.empty()) {
                        // If there is no next state for one of the sub-states, then the complement will have a next
                        // state, which is the shrinked state with empty sub-states.
                        next_states.push_back(shrinked_state);
                        break;
                    } else {
                        next_sub_states.insert(next_sub_states_list.begin(), next_sub_states_list.end());
                    }
                }

                break;
            }

            case NodeKind::ComplementNft: {
                // TODO
                break;
            }
        }

        return next_states;
    }

    bool is_accepting(const NodeId& id, const MacroStateId& state) {
        Node node = nodes[id];

        switch (node.kind) {
            // For leaf nodes, we just need to check if the state is in the final states of the NFA/NFT.
            case NodeKind::LeafNfa: {
                const Nfa& nfa = nfas[node.lhs];
                return nfa.final.contains(static_cast<State>(state));
            }

            case NodeKind::LeafNft: {
                const Nft& nft = nfts[node.lhs];
                return nft.final.contains(static_cast<State>(state));
            }

            // For union, the state is accepting if the tagged state it contains is accepting.
            case NodeKind::Union: {
                TaggedState tagged = macro_store.get_tagged(id, state);

                switch (tagged.tag) {
                    case TaggedState::Tag::Left:
                        return is_accepting(node.lhs, tagged.state);
                    case TaggedState::Tag::Right:
                        return is_accepting(node.rhs, tagged.state);
                }
            }

            // For intersection, preimage, postimage, compose, the state is accepting if both states in the pair are
            // accepting.
            case NodeKind::Intersect:
            case NodeKind::PreImage:
            case NodeKind::PostImage:
            case NodeKind::Compose: {
                PairState pair = macro_store.get_pair(id, state);
                return is_accepting(node.lhs, pair.lhs) && is_accepting(node.rhs, pair.rhs);
            }

            // For complement, the state is accepting if one of the sub-states is not accepting.
            case NodeKind::Complement:
            case NodeKind::ComplementNft: {
                std::unordered_set<MacroStateId> sub_states = macro_store.get_set(id, state);

                for (const MacroStateId& sub_state : sub_states) {
                    if (!is_accepting(node.lhs, sub_state)) {
                        return true;
                    }
                }
                break;
            }
        }

        return false;
    }

    bool sumsumed_state(const NodeId& id, const MacroStateId& state1, const MacroStateId& state2) {
        Node node = nodes[id];
        State s1 = static_cast<State>(state1);
        State s2 = static_cast<State>(state2);

        switch (node.kind) {
            case NodeKind::LeafNfa: {
                const Simlib::Util::BinaryRelation& sim = precomputed_simulations[node.lhs];
                return sim.get(s1, s2);
            }
            case NodeKind::LeafNft: {
                const size_t nft_index = node.lhs + nfas.size(); // index of the NFT in the nfts vector
                const Simlib::Util::BinaryRelation& sim = precomputed_simulations[nft_index];
                return sim.get(s1, s2);
            }

            case NodeKind::Union: {
                TaggedState tagged1 = macro_store.get_tagged(id, state1);
                TaggedState tagged2 = macro_store.get_tagged(id, state2);

                if (tagged1.tag != tagged2.tag) {
                    // If the two states are from different sides of the union, then they are not subsumed.
                    return false;
                }

                if (tagged1.tag == TaggedState::Tag::Left) {
                    return sumsumed_state(node.lhs, tagged1.state, tagged2.state);
                } else {
                    return sumsumed_state(node.rhs, tagged1.state, tagged2.state);
                }
            }

            case NodeKind::Intersect:
            case NodeKind::PreImage:
            case NodeKind::PostImage:
            case NodeKind::Compose: {
                PairState pair1 = macro_store.get_pair(id, state1);
                PairState pair2 = macro_store.get_pair(id, state2);

                return sumsumed_state(node.lhs, pair1.lhs, pair2.lhs) && sumsumed_state(node.rhs, pair1.rhs, pair2.rhs);
            }

            case NodeKind::Complement:
            case NodeKind::ComplementNft: {
                std::unordered_set<MacroStateId> sub_states1 = macro_store.get_set(id, state1);
                std::unordered_set<MacroStateId> sub_states2 = macro_store.get_set(id, state2);

                for (const MacroStateId& sub_state1 : sub_states1) {
                    // TODO
                    bool subsumed = false;

                    for (const MacroStateId& sub_state2 : sub_states2) {
                        if (sumsumed_state(node.lhs, sub_state1, sub_state2)) {
                            subsumed = true;
                            break;
                        }
                    }

                    if (!subsumed) {
                        return false;
                    }
                }


                break;
            }
        }

        return false;
    }

    bool is_subsumed(
            const MacroStateId& state, const std::unordered_set<MacroStateId>& visited,
            const std::list<MacroStateId>& worklist) {
        for (const MacroStateId& visited_state : visited) {
            if (visited_state == state) {
                // If the state is already visited, then it is subsumed by itself.
                return true;
            }

            if (sumsumed_state(root_id, state, visited_state)) {
                return true;
            }
        }

        for (const MacroStateId& worklist_state : worklist) {
            if (worklist_state == state) {
                // If the state is already in the worklist, then it is subsumed by itself.
                return true;
            }

            if (sumsumed_state(root_id, state, worklist_state)) {
                return true;
            }
        }

        // TODO maintain only the minimal states in visited and worklist, so that we can reduce the number of
        // subsumption checks we need to do here.

        return false;
    }
};

bool SymbolicAutomataTree::is_empty(const Term& term) {
    Context ctx(*this, term.get_id());

    std::list<MacroStateId> worklist = ctx.initial_macro_states(term);
    std::unordered_set<MacroStateId> visited = {};

    while (worklist.size() > 0) {
        const MacroStateId macro_id = worklist.front();
        worklist.pop_front();

        // NextStateIterator iter = NextStateIterator{ctx, ctx.root_id, macro_id};
        // std::optional<MacroStateId> next_state = iter.next();

        // while (next_state.has_value()) {
        //    const MacroStateId state = next_state.value();

        // loop through symbols
        for (const mata::Symbol sym : ctx.alphabets[ctx.root_id].get_alphabet_symbols()) {
            for (const MacroStateId& state : ctx.next_macro_states(ctx.root_id, macro_id, sym)) {
                // if not in visited and worklist and not subsumed, add to worklist
                if (!ctx.is_subsumed(state, visited, worklist)) {
                    if (ctx.is_accepting(ctx.root_id, state)) {
                        return false;
                    }

                    worklist.push_back(state);
                    visited.insert(state);
                }

                // next_state = iter.next();
            }
        }
    }

    return true;
}
