/** @file lazy.cc
 * @brief Lazy on-the-fly emptiness checking for symbolic combinations of NFAs and (2-level) NFTs.
 *
 * TODO: Coonvert into iterator-based API to make it more lazy,
 *       as the number of macro states can be very large,
 *       and we may not want to generate all of them at once.
 */

#include "mata/nft/lazy.hh"
#include <mata/simlib/explicit_lts.hh>
#include "mata/alphabet.hh"
#include "mata/nfa/algorithms.hh"
#include "mata/nfa/delta.hh"
#include "mata/nfa/types.hh"
#include "mata/nft/algorithms.hh"
#include "mata/nft/types.hh"

#include <cstdlib>
#include <functional>
#include <limits>
#include <list>
#include <unordered_map>
#include <unordered_set>
#include <vector>


using namespace mata::nft::lazy;


enum class ResultSort : uint8_t {
    Nfa = 0,
    Nft = 1,
};

ResultSort result_sort(const NodeKind kind) {
    switch (kind) {
        case NodeKind::LeafNfa:
        case NodeKind::Union:
        case NodeKind::Intersect:
        case NodeKind::Complement:
        case NodeKind::PreImage:
        case NodeKind::PostImage:
            return ResultSort::Nfa;
        case NodeKind::LeafNft:
        case NodeKind::ComplementNft:
        case NodeKind::Compose:
            return ResultSort::Nft;
    }

    // Should not be reached, add here just in case we add new node kinds
    // in the future and forget to update this function.
    throw std::logic_error("Unknown node kind");
}

/// helper enum
enum class VisitState : uint8_t {
    Unseen = 0,
    Active = 1,
    Done = 2,
};

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

bool try_parse_symbol_name(const std::string& symbol_name, mata::Symbol& symbol) {
    try {
        size_t parsed_chars = 0;
        const unsigned long parsed_symbol = std::stoul(symbol_name, &parsed_chars, 10);
        if (parsed_chars != symbol_name.size() || parsed_symbol > std::numeric_limits<mata::Symbol>::max()) {
            return false;
        }

        symbol = static_cast<mata::Symbol>(parsed_symbol);
        return true;
    } catch (const std::exception&) { return false; }
}

bool try_translate_symbol_name(mata::Alphabet* alphabet, const std::string& symbol_name, mata::Symbol& symbol) {
    if (alphabet != nullptr) {
        try {
            symbol = alphabet->translate_symb(symbol_name);
            return true;
        } catch (const std::runtime_error&) {
            // Fall back to the raw numeric symbol when the named alphabet does not know the symbol.
        }
    }

    return try_parse_symbol_name(symbol_name, symbol);
}

template<typename Automaton>
void fill_resolved_leaf_alphabet(const Automaton& automaton, mata::OnTheFlyAlphabet& alphabet_to_fill) {
    for (const auto& state_post : automaton.delta) {
        for (const auto& symbol_post : state_post) {
            alphabet_to_fill.translate_symb(symbol_name_for(automaton.alphabet, symbol_post.symbol));
        }
    }
}

void fill_resolved_leaf_alphabets(
        const mata::nft::Nft& nft, mata::OnTheFlyAlphabet& alphabet_to_fill,
        mata::OnTheFlyAlphabet& input_alphabet_to_fill, mata::OnTheFlyAlphabet& output_alphabet_to_fill) {
    for (mata::nfa::State state = 0; state < nft.delta.num_of_states(); ++state) {
        for (const auto& symbol_post : nft.delta.state_post(state)) {
            const std::string symbol_name = symbol_name_for(nft.alphabet, symbol_post.symbol);
            alphabet_to_fill.translate_symb(symbol_name);

            if (nft.levels[state] == 0) {
                input_alphabet_to_fill.translate_symb(symbol_name);
            } else {
                output_alphabet_to_fill.translate_symb(symbol_name);
            }
        }
    }
}

template<typename Automaton>
bool try_translate_resolved_symbol_to_local(
        const Automaton& automaton, const mata::OnTheFlyAlphabet& resolved_alphabet, const mata::Symbol resolved_symbol,
        mata::Symbol& local_symbol) {
    try {
        const std::string symbol_name = resolved_alphabet.reverse_translate_symbol(resolved_symbol);
        return try_translate_symbol_name(automaton.alphabet, symbol_name, local_symbol);
    } catch (const std::runtime_error&) { return false; }
}

mata::OnTheFlyAlphabet merge_alphabets(const mata::OnTheFlyAlphabet& lhs, const mata::OnTheFlyAlphabet& rhs) {
    mata::OnTheFlyAlphabet merged{};

    const auto add_symbols = [&merged](const mata::OnTheFlyAlphabet& alphabet) {
        for (const mata::Symbol sym : alphabet.get_alphabet_symbols()) {
            const std::string name = alphabet.reverse_translate_symbol(sym);
            merged.translate_symb(name);
        }
    };

    add_symbols(lhs);
    add_symbols(rhs);
    return merged;
}

void normalize_alphabet_vector(
        std::vector<mata::OnTheFlyAlphabet>& alphabets, const mata::OnTheFlyAlphabet& canonical_alphabet) {
    for (mata::OnTheFlyAlphabet& alphabet : alphabets) {
        mata::OnTheFlyAlphabet normalized_alphabet{};

        for (const mata::Symbol symbol : alphabet.get_alphabet_symbols()) {
            const std::string symbol_name = alphabet.reverse_translate_symbol(symbol);
            const auto canonical_it = canonical_alphabet.get_symbol_map().find(symbol_name);
            assert(canonical_it != canonical_alphabet.get_symbol_map().end());
            normalized_alphabet.add_new_symbol(symbol_name, canonical_it->second);
        }

        alphabet = std::move(normalized_alphabet);
    }
}

void add_symbols_to_canonical(
        const std::vector<mata::OnTheFlyAlphabet>& alphabets, mata::OnTheFlyAlphabet& canonical_alphabet) {
    for (const mata::OnTheFlyAlphabet& alphabet : alphabets) {
        for (const mata::Symbol symbol : alphabet.get_alphabet_symbols()) {
            canonical_alphabet.translate_symb(alphabet.reverse_translate_symbol(symbol));
        }
    }
}

void normalize_alphabets(
        std::vector<mata::OnTheFlyAlphabet>& alphabets, std::vector<mata::OnTheFlyAlphabet>& input_alphabets,
        std::vector<mata::OnTheFlyAlphabet>& output_alphabets) {
    assert(root_id < alphabets.size());
    mata::OnTheFlyAlphabet canonical_alphabet{};
    add_symbols_to_canonical(alphabets, canonical_alphabet);
    add_symbols_to_canonical(input_alphabets, canonical_alphabet);
    add_symbols_to_canonical(output_alphabets, canonical_alphabet);

    normalize_alphabet_vector(alphabets, canonical_alphabet);
    normalize_alphabet_vector(input_alphabets, canonical_alphabet);
    normalize_alphabet_vector(output_alphabets, canonical_alphabet);
}

bool validate_node(
        const SymbolicAutomataTree& tree, const NodeId node_id, const ResultSort expected,
        std::vector<VisitState>& marks) {
    if (node_id >= tree.nodes.size()) {
        return false;
    }

    if (result_sort(tree.nodes[node_id].kind) != expected) {
        return false;
    }

    switch (marks[node_id]) {
        case VisitState::Unseen:
            // continue
            break;
        case VisitState::Active:
            // If the node is already visited and marked as done, we can just return true.
            return false;
        case VisitState::Done:
            // If the node is already visited and marked as active,
            // it means there is a cycle in the tree, which is invalid.
            return true;
    }

    marks[node_id] = VisitState::Active;
    const Node& node = tree.nodes[node_id];
    bool ok = true;

    switch (node.kind) {
        case NodeKind::LeafNfa:
            ok = node.lhs < tree.nfas.size();
            break;

        case NodeKind::LeafNft:
            ok = node.lhs < tree.nfts.size();
            break;

        case NodeKind::Union:
        case NodeKind::Intersect:
            ok = validate_node(tree, node.lhs, ResultSort::Nfa, marks) &&
                 validate_node(tree, node.rhs, ResultSort::Nfa, marks);
            break;

        case NodeKind::Complement:
            ok = validate_node(tree, node.lhs, ResultSort::Nfa, marks);
            break;

        case NodeKind::ComplementNft:
            ok = validate_node(tree, node.lhs, ResultSort::Nft, marks);
            break;

        case NodeKind::PreImage:
        case NodeKind::PostImage:
            ok = validate_node(tree, node.lhs, ResultSort::Nfa, marks) &&
                 validate_node(tree, node.rhs, ResultSort::Nft, marks);
            break;

        case NodeKind::Compose:
            ok = validate_node(tree, node.lhs, ResultSort::Nft, marks) &&
                 validate_node(tree, node.rhs, ResultSort::Nft, marks);
            break;
    }

    marks[node_id] = ok ? VisitState::Done : VisitState::Unseen;
    return ok;
}

bool SymbolicAutomataTree::is_valid(const Term& root_node) const {
    std::vector<VisitState> marks(nodes.size(), VisitState::Unseen);
    return validate_node(*this, root_node.get_id(), ResultSort::Nfa, marks);
}

bool SymbolicAutomataTree::is_valid(const TermNft& root_node) const {
    std::vector<VisitState> marks(nodes.size(), VisitState::Unseen);
    return validate_node(*this, root_node.get_id(), ResultSort::Nft, marks);
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

// TODO: optimize this
struct MacroStateStore {
    using PairStore = std::unordered_map<MacroStateId, PairState>;
    using SetStore = std::unordered_map<MacroStateId, SetState>;
    using TaggedStore = std::unordered_map<MacroStateId, TaggedState>;

    // Index to the store
    // The i-th node is the index to this, which will point to the store for the i-th node
    // type of store is determined by the node kind
    std::vector<size_t> node_to_store_index;

    std::vector<PairStore> pair_stores;
    std::vector<SetStore> set_stores;
    std::vector<TaggedStore> tagged_stores;

    MacroStateStore() : node_to_store_index{}, pair_stores{}, set_stores{}, tagged_stores{} {};

    // Initialize the macro state store for the given nodes. For each node, we will create a store for its macro states
    // and store the index of the store in index_pair_stores. The type of store is determined by the node kind:
    // - For leaf nodes (LeafNfa and LeafNft), do not need to create a store
    // - For union nodes, create a tagged store
    // - For intersect, preimage, postimage, compose nodes, create a pair store
    // - For complement nodes, create a set store
    explicit MacroStateStore(const std::vector<Node>& nodes)
        : node_to_store_index(nodes.size(), 0), pair_stores{}, set_stores{}, tagged_stores{} {
        for (size_t i = 0; i < nodes.size(); i++) {
            switch (nodes[i].kind) {
                case NodeKind::Union: {
                    node_to_store_index[i] = tagged_stores.size();
                    tagged_stores.emplace_back();
                    break;
                }

                case NodeKind::Intersect:
                case NodeKind::PreImage:
                case NodeKind::PostImage:
                case NodeKind::Compose: {
                    node_to_store_index[i] = pair_stores.size();
                    pair_stores.emplace_back();
                    break;
                }

                case NodeKind::Complement:
                case NodeKind::ComplementNft: {
                    node_to_store_index[i] = set_stores.size();
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
        PairStore& store = pair_stores[node_to_store_index[idx]];
        const auto& it = store.find(id);
        assert(it != store.end());
        return it->second;
    }

    SetState get_set(const uint32_t idx, const MacroStateId& id) {
        SetStore& store = set_stores[node_to_store_index[idx]];
        const auto& it = store.find(id);
        assert(it != store.end());
        return it->second;
    }

    TaggedState get_tagged(const uint32_t idx, const MacroStateId& id) {
        TaggedStore& store = tagged_stores[node_to_store_index[idx]];
        const auto& it = store.find(id);
        assert(it != store.end());
        return it->second;
    }

    // Intern a macro state and return its id. If the state already exists, return the existing id.
    MacroStateId intern(const NodeId idx, SetState states) {
        SetStore& store = set_stores[node_to_store_index[idx]];
        MacroStateId id = hash_states(states);

        // Check for the states with the same hash, if they are the same as the input states, return their id,
        // otherwise keep looking for the next id until we find an empty slot or the same states.
        while (store.contains(id)) {
            if (store[id] == states) {
                return id;
            }
            id++;
        }

        store.emplace(id, std::move(states));
        return id;
    }

    MacroStateId intern(const uint32_t idx, const PairState pair) {
        PairStore& store = pair_stores[node_to_store_index[idx]];
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
        TaggedStore& store = tagged_stores[node_to_store_index[idx]];
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
};

/// Node reconstruction
/// Create a new node tree (in vector) with only reachable nodes from the root.
/// Also try to push the complement down as much as possible,
/// so that the resulting tree is more suitable for on-the-fly emptiness checking.
NodeId reconstruct_nodes(
        const std::vector<Node>& original_nodes, NodeId id, std::vector<Node>& output,
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
    visited.insert(id);

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
                return reconstruct_nodes(original_nodes, child_node.lhs, output, visited);
            }

            NodeId id = reconstruct_nodes(original_nodes, node.lhs, output, visited);
            output.push_back({NodeKind::ComplementNft, id, 0});
            break;
        }
    }

    return static_cast<NodeId>(output.size() - 1);
}

struct Context {
    using Nfa = mata::nfa::Nfa;
    using Nft = mata::nft::Nft;
    using State = mata::nfa::State;
    using MacroStateVisitor = std::function<bool(MacroStateId)>;

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
    // For NFA-producing nodes, this is the alphabet of the resulting language.
    // For NFT-producing nodes, this is the union of symbols used on both tracks.
    std::vector<mata::OnTheFlyAlphabet> alphabets;
    std::vector<mata::OnTheFlyAlphabet> input_alphabets;
    std::vector<mata::OnTheFlyAlphabet> output_alphabets;

    // Precomputed simulations for each leaf node.
    // The size will be nfas.size() + nfts.size(),
    // the order will be the same as in nfas and nfts (first all NFAs, then all NFTs).
    std::vector<Simlib::Util::BinaryRelation> precomputed_simulations;

    NodeId root_id;

    Context();

    Context(const SymbolicAutomataTree& tree, NodeId root)
        : nfas(tree.nfas), nfts(tree.nfts), nodes{}, macro_store{}, alphabets{}, input_alphabets{}, output_alphabets{},
          precomputed_simulations{}, root_id{0} {

        root_id = reconstruct_nodes(tree.nodes, root, nodes);
        macro_store = MacroStateStore(nodes);

        // resolve the alphabets for every reachable node,
        // also precompute the simulation on reachable base automata, which will be used for subsumption checking.
        alphabets.resize(nodes.size());
        input_alphabets.resize(nodes.size());
        output_alphabets.resize(nodes.size());
        precomputed_simulations.resize(nfas.size() + nfts.size());

        std::vector<bool> visited(nodes.size(), false);
        resolve_metadata(root_id, visited);
        normalize_alphabets(alphabets, input_alphabets, output_alphabets);
    };

    // This function is used to resolve the metadata for each node,
    // which includes the alphabets and precomputed simulations.
    void resolve_metadata(const NodeId node_id, std::vector<bool>& visited) {
        if (visited[node_id]) {
            return;
        }

        const Node& node = nodes[node_id];

        switch (node.kind) {
            case NodeKind::LeafNfa: {
                const Nfa& nfa = nfas[node.lhs];
                fill_resolved_leaf_alphabet(nfa, alphabets[node_id]);
                precomputed_simulations[node.lhs] = mata::nfa::algorithms::compute_relation(nfa);
                break;
            }

            case NodeKind::LeafNft: {
                const Nft& nft = nfts[node.lhs];
                fill_resolved_leaf_alphabets(
                        nft, alphabets[node_id], input_alphabets[node_id], output_alphabets[node_id]);
                precomputed_simulations[node.lhs + nfas.size()] = mata::nft::algorithms::compute_relation(nft);
                break;
            }

            case NodeKind::Union:
            case NodeKind::Intersect: {
                resolve_metadata(node.lhs, visited);
                resolve_metadata(node.rhs, visited);
                alphabets[node_id] = merge_alphabets(alphabets[node.lhs], alphabets[node.rhs]);
                break;
            }

            case NodeKind::Complement: {
                resolve_metadata(node.lhs, visited);
                alphabets[node_id] = alphabets[node.lhs];
                break;
            }

            case NodeKind::PreImage: {
                resolve_metadata(node.lhs, visited);
                resolve_metadata(node.rhs, visited);
                alphabets[node_id] = input_alphabets[node.rhs];
                break;
            }

            case NodeKind::PostImage: {
                resolve_metadata(node.lhs, visited);
                resolve_metadata(node.rhs, visited);
                alphabets[node_id] = output_alphabets[node.rhs];
                break;
            }

            case NodeKind::ComplementNft: {
                resolve_metadata(node.lhs, visited);
                input_alphabets[node_id] = input_alphabets[node.lhs];
                output_alphabets[node_id] = output_alphabets[node.lhs];
                alphabets[node_id] = merge_alphabets(input_alphabets[node_id], output_alphabets[node_id]);
                break;
            }

            case NodeKind::Compose: {
                resolve_metadata(node.lhs, visited);
                resolve_metadata(node.rhs, visited);
                input_alphabets[node_id] = input_alphabets[node.lhs];
                output_alphabets[node_id] = output_alphabets[node.rhs];
                alphabets[node_id] = merge_alphabets(input_alphabets[node_id], output_alphabets[node_id]);
                break;
            }
        }

        visited[node_id] = true;
    }

    // Visit initial macro states lazily. Returning false from the visitor stops
    // the traversal early and propagates to the caller.
    bool for_each_initial_macro_state(const Term& term, const MacroStateVisitor& visitor) {
        NodeId node_id = term.get_id();
        Node node = nodes[node_id];

        switch (node.kind) {
            case NodeKind::LeafNfa: {
                const Nfa& nfa = nfas[node.lhs];
                for (const State initial_state : nfa.initial) {
                    // State is unsigned int, which is the same as MacroStateId
                    if (!visitor(static_cast<MacroStateId>(initial_state))) {
                        return false;
                    }
                }

                break;
            }

            case NodeKind::LeafNft: {
                const Nft& nft = nfts[node.lhs];
                for (const State initial_state : nft.initial) {
                    // State is unsigned int, which is the same as MacroStateId
                    if (!visitor(static_cast<MacroStateId>(initial_state))) {
                        return false;
                    }
                }
                break;
            }

            // For union, it is the disjoint union of the initial states of the two subterms,
            // tagged with which side they come from (left or right).
            case NodeKind::Union: {
                if (!for_each_initial_macro_state(Term{node.lhs}, [&](const MacroStateId lhs_id) {
                        TaggedState tagged{lhs_id, TaggedState::Tag::Left};
                        MacroStateId id = macro_store.intern(node_id, std::move(tagged));
                        return visitor(id);
                    })) {
                    return false;
                }

                if (!for_each_initial_macro_state(Term{node.rhs}, [&](const MacroStateId rhs_id) {
                        TaggedState tagged{rhs_id, TaggedState::Tag::Right};
                        MacroStateId id = macro_store.intern(node_id, std::move(tagged));
                        return visitor(id);
                    })) {
                    return false;
                }

                break;
            }

            // For intersection, compose, it is the Cartesian product of the initial states of the two subterms.
            case NodeKind::Intersect:
            case NodeKind::PreImage:
            case NodeKind::PostImage:
            case NodeKind::Compose: {
                if (!for_each_initial_macro_state(Term{node.lhs}, [&](const MacroStateId lhs_id) {
                        return for_each_initial_macro_state(Term{node.rhs}, [&](const MacroStateId rhs_id) {
                            PairState pair{lhs_id, rhs_id};
                            MacroStateId id = macro_store.intern(node_id, std::move(pair));
                            return visitor(id);
                        });
                    })) {
                    return false;
                }

                break;
            }

            // For complement, we do determinization on the fly, so the initial states
            // are just one macrostate that contains the all initial states of the subterm.
            case NodeKind::Complement:
            case NodeKind::ComplementNft: {
                std::list<MacroStateId> sub_initial_states = {};
                if (!for_each_initial_macro_state(Term{node.lhs}, [&](const MacroStateId sub_initial_state) {
                        sub_initial_states.push_back(sub_initial_state);
                        return true;
                    })) {
                    return false;
                }
                // Convert the set to unordered_set for complmenet's macro state store
                std::unordered_set<MacroStateId> sub_initial_states_set(
                        sub_initial_states.begin(), sub_initial_states.end());

                MacroStateId id = macro_store.intern(node_id, std::move(sub_initial_states_set));
                if (!visitor(id)) {
                    return false;
                }
                break;
            }
        }

        return true;
    }

    // Visit the next states from the given state on the given symbol.
    // The sym2 is used when transducer is involved, which is the symbol on the
    // output side of the transducer. Returning false from the visitor stops
    // the traversal early and propagates to the caller.
    bool for_each_next_macro_state(
            const NodeId& node_id, const MacroStateId& state, mata::Symbol sym, mata::Symbol sym2,
            const MacroStateVisitor& visitor) {
        const Node& node = nodes[node_id];

        switch (node.kind) {
            case NodeKind::LeafNfa: {
                const Nfa& nfa = nfas[node.lhs];
                const State s = static_cast<State>(state);
                mata::Symbol local_sym = 0;

                if (!try_translate_resolved_symbol_to_local(nfa, alphabets[node_id], sym, local_sym)) {
                    break;
                }

                printf("LeafNfa: node_id=%u, state=%zu, sym=%u\n", node_id, static_cast<size_t>(s), sym);

                for (const State next_state : nfa.delta.get_successors(s, local_sym)) {
                    printf("LeafNfa: adding next_state=%zu\n", static_cast<size_t>(next_state));
                    if (!visitor(static_cast<MacroStateId>(next_state))) {
                        return false;
                    }
                }

                break;
            }
            case NodeKind::LeafNft: {
                const Nft& nft = nfts[node.lhs];
                const State s = static_cast<State>(state);
                mata::Symbol local_sym = 0;
                mata::Symbol local_sym2 = 0;

                if (!try_translate_resolved_symbol_to_local(nft, input_alphabets[node_id], sym, local_sym) ||
                    !try_translate_resolved_symbol_to_local(nft, output_alphabets[node_id], sym2, local_sym2)) {
                    break;
                }

                // Move 2 steps in the NFT, first on sym on the input side, then on sym2 on the output side.
                for (const State after_input : nft.delta.get_successors(s, local_sym)) {
                    for (const State after_output : nft.delta.get_successors(after_input, local_sym2)) {
                        if (!visitor(static_cast<MacroStateId>(after_output))) {
                            return false;
                        }
                    }
                }

                break;
            }


            case NodeKind::Union: {
                const TaggedState tagged = macro_store.get_tagged(node_id, state);
                const NodeId next_node_id = (tagged.tag == TaggedState::Tag::Left) ? node.lhs : node.rhs;

                if (!for_each_next_macro_state(
                            next_node_id, tagged.state, sym, sym2, [&](const MacroStateId next_state) {
                                TaggedState next_tagged{next_state, tagged.tag};
                                MacroStateId next_id = macro_store.intern(node_id, std::move(next_tagged));
                                return visitor(next_id);
                            })) {
                    return false;
                }

                break;
            }


            case NodeKind::Intersect: {
                const PairState pair = macro_store.get_pair(node_id, state);
                printf("Intersect: node_id=%u, state=(%u, %u), sym=%u, sym2=%u\n", node_id, pair.lhs, pair.rhs, sym,
                       sym2);

                if (!for_each_next_macro_state(node.lhs, pair.lhs, sym, sym2, [&](const MacroStateId next_lhs_state) {
                        return for_each_next_macro_state(
                                node.rhs, pair.rhs, sym, sym2, [&](const MacroStateId next_rhs_state) {
                                    PairState next_pair{next_lhs_state, next_rhs_state};
                                    MacroStateId next_id = macro_store.intern(node_id, std::move(next_pair));
                                    return visitor(next_id);
                                });
                    })) {
                    return false;
                }

                break;
            }


            case NodeKind::PreImage: {
                const PairState pair = macro_store.get_pair(node_id, state);

                const NodeId nfa_id = node.lhs;
                const NodeId nft_id = node.rhs;

                for (const mata::Symbol sync_sym : output_alphabets[nft_id].get_alphabet_symbols()) {
                    if (!for_each_next_macro_state(
                                nfa_id, pair.lhs, sync_sym, 0, [&](const MacroStateId next_lhs_state) {
                                    return for_each_next_macro_state(
                                            nft_id, pair.rhs, sym, sync_sym, [&](const MacroStateId next_rhs_state) {
                                                PairState next_pair{next_lhs_state, next_rhs_state};
                                                MacroStateId next_id =
                                                        macro_store.intern(node_id, std::move(next_pair));
                                                return visitor(next_id);
                                            });
                                })) {
                        return false;
                    }
                }

                break;
            }

            case NodeKind::PostImage: {
                const PairState pair = macro_store.get_pair(node_id, state);

                const NodeId nfa_id = node.lhs;
                const NodeId nft_id = node.rhs;

                for (const mata::Symbol sync_sym : input_alphabets[nft_id].get_alphabet_symbols()) {
                    if (!for_each_next_macro_state(
                                nfa_id, pair.lhs, sync_sym, 0, [&](const MacroStateId next_lhs_state) {
                                    return for_each_next_macro_state(
                                            nft_id, pair.rhs, sync_sym, sym, [&](const MacroStateId next_rhs_state) {
                                                PairState next_pair{next_lhs_state, next_rhs_state};
                                                MacroStateId next_id =
                                                        macro_store.intern(node_id, std::move(next_pair));
                                                return visitor(next_id);
                                            });
                                })) {
                        return false;
                    }
                }

                break;
            }

            case NodeKind::Compose: {
                const PairState pair = macro_store.get_pair(node_id, state);

                for (const mata::Symbol sync_sym : output_alphabets[node.lhs].get_alphabet_symbols()) {
                    if (!for_each_next_macro_state(
                                node.lhs, pair.lhs, sym, sync_sym, [&](const MacroStateId next_lhs_state) {
                                    return for_each_next_macro_state(
                                            node.rhs, pair.rhs, sync_sym, sym2, [&](const MacroStateId next_rhs_state) {
                                                PairState next_pair{next_lhs_state, next_rhs_state};
                                                MacroStateId next_id =
                                                        macro_store.intern(node_id, std::move(next_pair));
                                                return visitor(next_id);
                                            });
                                })) {
                        return false;
                    }
                }

                break;
            }

            case NodeKind::Complement: {
                const std::unordered_set<MacroStateId> sub_states = macro_store.get_set(node_id, state);
                SetState next_sub_states = {};

                for (const MacroStateId sub_state : sub_states) {
                    if (!for_each_next_macro_state(
                                node.lhs, sub_state, sym, 0, [&](const MacroStateId child_next_state) {
                                    next_sub_states.insert(child_next_state);
                                    return true;
                                })) {
                        return false;
                    }
                    // Note that if the child_next_states is empty,
                    // it means that there is no transition on the given symbol from the sub_state,
                    // the next_sub_states will be empty and it acts like a sink state,
                    // which is what we want for the complement.
                }

                if (!visitor(macro_store.intern(node_id, std::move(next_sub_states)))) {
                    return false;
                }

                break;
            }

            // Not sure that this can be safely merged with the complement case above, as the transition relation
            // for NFTs is more complicated than NFAs, The only difference is that here we add the sym2 for the
            // transition, which is default to be 0 for the normal complement case, but it can be non-zero for the
            // complement of NFTs. Not sure if this can cause any issue, so for now we just keep them separate.
            case NodeKind::ComplementNft: {
                const SetState sub_states = macro_store.get_set(node_id, state);
                SetState next_sub_states = {};

                for (const MacroStateId sub_state : sub_states) {
                    if (!for_each_next_macro_state(
                                node.lhs, sub_state, sym, sym2, [&](const MacroStateId child_next_state) {
                                    next_sub_states.insert(child_next_state);
                                    return true;
                                })) {
                        return false;
                    }
                }

                if (!visitor(macro_store.intern(node_id, std::move(next_sub_states)))) {
                    return false;
                }

                break;
            }
        }

        return true;
    }

    bool for_each_next_macro_state(
            const NodeId& node_id, const MacroStateId& state, mata::Symbol sym, const MacroStateVisitor& visitor) {
        return for_each_next_macro_state(node_id, state, sym, 0, visitor);
    }

    bool is_accepting(const NodeId& node_id, const MacroStateId& state) {
        Node node = nodes[node_id];

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
                const TaggedState tagged = macro_store.get_tagged(node_id, state);
                return tagged.tag == TaggedState::Tag::Left ? is_accepting(node.lhs, tagged.state)
                                                            : is_accepting(node.rhs, tagged.state);
            }

            // For intersection, preimage, postimage, compose, the state is accepting if both states in the pair are
            // accepting.
            case NodeKind::Intersect:
            case NodeKind::PreImage:
            case NodeKind::PostImage:
            case NodeKind::Compose: {
                const PairState pair = macro_store.get_pair(node_id, state);
                return is_accepting(node.lhs, pair.lhs) && is_accepting(node.rhs, pair.rhs);
            }

            // For complement, the state is accepting if one of the sub-states is not accepting.
            case NodeKind::Complement:
            case NodeKind::ComplementNft: {
                const SetState sub_states = macro_store.get_set(node_id, state);

                for (const MacroStateId& sub_state : sub_states) {
                    if (is_accepting(node.lhs, sub_state)) {
                        return false;
                    }
                }

                return true;
            }
        }

        // Should not reach here
        return false;
    }

    bool subsumed_state(const NodeId node_id, const MacroStateId state1, const MacroStateId state2) {
        const Node node = nodes[node_id];
        const State s1 = static_cast<State>(state1);
        const State s2 = static_cast<State>(state2);

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
                const TaggedState tagged1 = macro_store.get_tagged(node_id, state1);
                const TaggedState tagged2 = macro_store.get_tagged(node_id, state2);

                if (tagged1.tag != tagged2.tag) {
                    // If the two states are from different sides of the union, then they are not subsumed.
                    return false;
                }

                return tagged1.tag == TaggedState::Tag::Left ? subsumed_state(node.lhs, tagged1.state, tagged2.state)
                                                             : subsumed_state(node.rhs, tagged1.state, tagged2.state);
            }

            case NodeKind::Intersect:
            case NodeKind::PreImage:
            case NodeKind::PostImage:
            case NodeKind::Compose: {
                const PairState pair1 = macro_store.get_pair(node_id, state1);
                const PairState pair2 = macro_store.get_pair(node_id, state2);

                return subsumed_state(node.lhs, pair1.lhs, pair2.lhs) && subsumed_state(node.rhs, pair1.rhs, pair2.rhs);
            }

            case NodeKind::Complement:
            case NodeKind::ComplementNft: {
                const SetState sub_states1 = macro_store.get_set(node_id, state1);
                const SetState sub_states2 = macro_store.get_set(node_id, state2);

                for (const MacroStateId& sub_state1 : sub_states1) {
                    bool subsumed = false;

                    for (const MacroStateId& sub_state2 : sub_states2) {
                        if (subsumed_state(node.lhs, sub_state1, sub_state2)) {
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
            const MacroStateId state, const std::unordered_set<MacroStateId>& visited,
            const std::list<MacroStateId>& worklist) {
        for (const MacroStateId& visited_state : visited) {
            // If the state is already visited, then it is subsumed by itself.
            if (visited_state == state || subsumed_state(root_id, state, visited_state)) {
                return true;
            }
        }

        for (const MacroStateId& worklist_state : worklist) {
            if (worklist_state == state || subsumed_state(root_id, state, worklist_state)) {
                return true;
            }
        }

        // TODO maintain only the minimal states in visited and worklist, so that we can reduce the number of
        // subsumption checks we need to do here.

        return false;
    }
};

bool is_empty_impl(Context& ctx, bool is_nft) {
    printf("Start emptiness checking for %s\n", is_nft ? "NFT" : "NFA");

    std::list<MacroStateId> worklist = {};
    std::unordered_set<MacroStateId> visited = {};

    const bool initial_states_fully_processed =
            ctx.for_each_initial_macro_state(ctx.root_id, [&](const MacroStateId initial_state) {
                if (ctx.is_accepting(ctx.root_id, initial_state)) {
                    return false;
                }

                if (ctx.is_subsumed(initial_state, visited, worklist)) {
                    return true;
                }

                worklist.push_back(initial_state);
                return true;
            });

    if (!initial_states_fully_processed) {
        return false;
    }

    printf("Initial worklist size: %zu\n", worklist.size());

    const auto& alphabet = ctx.alphabets[ctx.root_id];
    const auto& input_alphabet = ctx.input_alphabets[ctx.root_id];
    const auto& output_alphabet = ctx.output_alphabets[ctx.root_id];

    printf("Alphabet size: %zu\n", alphabet.get_alphabet_symbols().size());

    while (!worklist.empty()) {
        const MacroStateId current_state = worklist.back(); // DFS
        worklist.pop_back();
        visited.insert(current_state);

        // This if is likely to be optimized away by the branch predictor of the CPU,
        // as the kind of the root node is fixed, so it will always go to the same branch.
        // not sure, need benchmark...
        if (is_nft) {
            for (const mata::Symbol sym : input_alphabet.get_alphabet_symbols()) {
                for (const mata::Symbol sym2 : output_alphabet.get_alphabet_symbols()) {
                    const bool should_continue = ctx.for_each_next_macro_state(
                            ctx.root_id, current_state, sym, sym2, [&](const MacroStateId next_state) {
                                // is_accepting is cheaper than is_subsumed, so check it first
                                if (ctx.is_accepting(ctx.root_id, next_state)) {
                                    return false;
                                }

                                if (ctx.is_subsumed(next_state, visited, worklist)) {
                                    return true;
                                }

                                worklist.push_back(next_state);
                                return true;
                            });

                    if (!should_continue) {
                        return false;
                    }
                }
            }

        } else {
            for (const mata::Symbol sym : alphabet.get_alphabet_symbols()) {
                printf("Current state: %u, symbol: %u\n", current_state, sym);
                const bool should_continue = ctx.for_each_next_macro_state(
                        ctx.root_id, current_state, sym, [&](const MacroStateId next_state) {
                            printf("Current state: %u, next state: %u, symbol: %u\n", current_state, next_state, sym);
                            // is_accepting is cheaper than is_subsumed, so check it first
                            if (ctx.is_accepting(ctx.root_id, next_state)) {
                                return false;
                            }

                            if (ctx.is_subsumed(next_state, visited, worklist)) {
                                return true;
                            }

                            worklist.push_back(next_state);
                            return true;
                        });

                if (!should_continue) {
                    return false;
                }
            }
        }
    }

    return true;
}

bool SymbolicAutomataTree::is_empty(const Term& root_node) {
    fprintf(stderr, "Start emptiness checking for NFA\n");
    Context ctx = Context(*this, root_node.get_id());
    fprintf(stderr, "Finish initialization for NFA, start the main loop\n");
    return is_empty_impl(ctx, false);
}

bool SymbolicAutomataTree::is_empty(const TermNft& root_node) {
    Context ctx = Context(*this, root_node.get_id());
    return is_empty_impl(ctx, true);
}
