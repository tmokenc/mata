/** @file lazy.cc
 * @brief Lazy on-the-fly emptiness checking for symbolic combinations of NFAs and (2-level) NFTs.
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

    const SetState& get_set(const uint32_t idx, const MacroStateId& id) {
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
        while (true) {
            const auto it = store.find(id);
            if (it == store.end()) {
                break;
            }
            if (it->second == states) {
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

        while (true) {
            const auto it = store.find(id);
            if (it == store.end()) {
                break;
            }
            if (it->second.lhs == pair.lhs && it->second.rhs == pair.rhs) {
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

        while (true) {
            const auto it = store.find(id);
            if (it == store.end()) {
                break;
            }
            if (it->second.state == tagged.state && it->second.tag == tagged.tag) {
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

    struct GeneratedMacroState {
        MacroStateId id;
        bool accepting;
    };

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

    Context(const SymbolicAutomataTree& tree, NodeId root, const mata::OnTheFlyAlphabet* shared_alphabet = nullptr)
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
        if (shared_alphabet != nullptr) {
            resolve_metadata(root_id, visited, shared_alphabet);
        } else {
            resolve_metadata(root_id, visited);
            normalize_alphabets(alphabets, input_alphabets, output_alphabets);
        }
    };

    // This function is used to resolve the metadata for each node,
    // which includes the alphabets and precomputed simulations.
    void resolve_metadata(
            const NodeId node_id, std::vector<bool>& visited, const mata::OnTheFlyAlphabet* shared_alphabet = nullptr) {
        if (visited[node_id]) {
            return;
        }

        const Node& node = nodes[node_id];

        switch (node.kind) {
            case NodeKind::LeafNfa: {
                const Nfa& nfa = nfas[node.lhs];
                if (shared_alphabet == nullptr) {
                    fill_resolved_leaf_alphabet(nfa, alphabets[node_id]);
                }
                precomputed_simulations[node.lhs] = mata::nfa::algorithms::compute_relation(nfa);
                break;
            }

            case NodeKind::LeafNft: {
                const Nft& nft = nfts[node.lhs];
                if (shared_alphabet == nullptr) {
                    fill_resolved_leaf_alphabets(
                            nft, alphabets[node_id], input_alphabets[node_id], output_alphabets[node_id]);
                }
                precomputed_simulations[node.lhs + nfas.size()] = mata::nft::algorithms::compute_relation(nft);
                break;
            }

            case NodeKind::Union:
            case NodeKind::Intersect: {
                resolve_metadata(node.lhs, visited, shared_alphabet);
                resolve_metadata(node.rhs, visited, shared_alphabet);
                if (shared_alphabet == nullptr) {
                    alphabets[node_id] = merge_alphabets(alphabets[node.lhs], alphabets[node.rhs]);
                }
                break;
            }

            case NodeKind::Complement: {
                resolve_metadata(node.lhs, visited, shared_alphabet);
                if (shared_alphabet == nullptr) {
                    alphabets[node_id] = alphabets[node.lhs];
                }
                break;
            }

            case NodeKind::PreImage: {
                resolve_metadata(node.lhs, visited, shared_alphabet);
                resolve_metadata(node.rhs, visited, shared_alphabet);
                if (shared_alphabet == nullptr) {
                    alphabets[node_id] = input_alphabets[node.rhs];
                }
                break;
            }

            case NodeKind::PostImage: {
                resolve_metadata(node.lhs, visited, shared_alphabet);
                resolve_metadata(node.rhs, visited, shared_alphabet);
                if (shared_alphabet == nullptr) {
                    alphabets[node_id] = output_alphabets[node.rhs];
                }
                break;
            }

            case NodeKind::ComplementNft: {
                resolve_metadata(node.lhs, visited, shared_alphabet);
                if (shared_alphabet == nullptr) {
                    input_alphabets[node_id] = input_alphabets[node.lhs];
                    output_alphabets[node_id] = output_alphabets[node.lhs];
                    alphabets[node_id] = merge_alphabets(input_alphabets[node_id], output_alphabets[node_id]);
                }
                break;
            }

            case NodeKind::Compose: {
                resolve_metadata(node.lhs, visited, shared_alphabet);
                resolve_metadata(node.rhs, visited, shared_alphabet);
                if (shared_alphabet == nullptr) {
                    input_alphabets[node_id] = input_alphabets[node.lhs];
                    output_alphabets[node_id] = output_alphabets[node.rhs];
                    alphabets[node_id] = merge_alphabets(input_alphabets[node_id], output_alphabets[node_id]);
                }
                break;
            }
        }

        if (shared_alphabet != nullptr) {
            alphabets[node_id] = *shared_alphabet;
            input_alphabets[node_id] = *shared_alphabet;
            output_alphabets[node_id] = *shared_alphabet;
        }

        visited[node_id] = true;
    }

    // Visit initial macro states lazily. Returning false from the visitor stops
    // the traversal early and propagates to the caller.
    using MacroStateVisitor = std::function<bool(const GeneratedMacroState&)>;

    bool emit_leaf_initial_states(const Nfa& nfa, const MacroStateVisitor& visitor) {
        for (const State initial_state : nfa.initial) {
            if (!visitor(
                        GeneratedMacroState{
                                static_cast<MacroStateId>(initial_state), nfa.final.contains(initial_state)})) {
                return false;
            }
        }

        return true;
    }

    bool emit_leaf_initial_states(const Nft& nft, const MacroStateVisitor& visitor) {
        for (const State initial_state : nft.initial) {
            if (!visitor(
                        GeneratedMacroState{
                                static_cast<MacroStateId>(initial_state), nft.final.contains(initial_state)})) {
                return false;
            }
        }

        return true;
    }

    bool emit_paired_successors(
            const NodeId parent_id, const NodeId lhs_id, const MacroStateId lhs_state, const mata::Symbol lhs_sym,
            const mata::Symbol lhs_sym2, const NodeId rhs_id, const MacroStateId rhs_state, const mata::Symbol rhs_sym,
            const mata::Symbol rhs_sym2, const MacroStateVisitor& visitor) {
        return for_each_next_macro_state(
                lhs_id, lhs_state, lhs_sym, lhs_sym2, [&](const GeneratedMacroState& next_lhs_state) {
                    return for_each_next_macro_state(
                            rhs_id, rhs_state, rhs_sym, rhs_sym2, [&](const GeneratedMacroState& next_rhs_state) {
                                PairState next_pair{next_lhs_state.id, next_rhs_state.id};
                                const MacroStateId next_id = macro_store.intern(parent_id, std::move(next_pair));
                                return visitor(
                                        GeneratedMacroState{
                                                next_id, next_lhs_state.accepting && next_rhs_state.accepting});
                            });
                });
    }

    bool emit_complement_successor(
            const NodeId parent_id, const NodeId child_id, const SetState& sub_states, const mata::Symbol sym,
            const mata::Symbol sym2, const MacroStateVisitor& visitor) {
        SetState next_sub_states{};
        next_sub_states.reserve(sub_states.size());
        bool accepting = true;

        for (const MacroStateId sub_state : sub_states) {
            if (!for_each_next_macro_state(
                        child_id, sub_state, sym, sym2, [&](const GeneratedMacroState& child_next_state) {
                            next_sub_states.insert(child_next_state.id);
                            accepting = accepting && !child_next_state.accepting;
                            return true;
                        })) {
                return false;
            }
        }

        return visitor(GeneratedMacroState{macro_store.intern(parent_id, std::move(next_sub_states)), accepting});
    }

    bool for_each_initial_macro_state(const NodeId node_id, const MacroStateVisitor& visitor) {
        const Node& node = nodes[node_id];

        switch (node.kind) {
            case NodeKind::LeafNfa: {
                const Nfa& nfa = nfas[node.lhs];
                return emit_leaf_initial_states(nfa, visitor);
            }

            case NodeKind::LeafNft: {
                const Nft& nft = nfts[node.lhs];
                return emit_leaf_initial_states(nft, visitor);
            }

            // For union, it is the disjoint union of the initial states of the two subterms,
            // tagged with which side they come from (left or right).
            case NodeKind::Union: {
                if (!for_each_initial_macro_state(node.lhs, [&](const GeneratedMacroState& lhs_state) {
                        TaggedState tagged{lhs_state.id, TaggedState::Tag::Left};
                        const MacroStateId id = macro_store.intern(node_id, std::move(tagged));
                        return visitor(GeneratedMacroState{id, lhs_state.accepting});
                    })) {
                    return false;
                }

                if (!for_each_initial_macro_state(node.rhs, [&](const GeneratedMacroState& rhs_state) {
                        TaggedState tagged{rhs_state.id, TaggedState::Tag::Right};
                        const MacroStateId id = macro_store.intern(node_id, std::move(tagged));
                        return visitor(GeneratedMacroState{id, rhs_state.accepting});
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
                if (!for_each_initial_macro_state(node.lhs, [&](const GeneratedMacroState& lhs_state) {
                        return for_each_initial_macro_state(node.rhs, [&](const GeneratedMacroState& rhs_state) {
                            PairState pair{lhs_state.id, rhs_state.id};
                            const MacroStateId id = macro_store.intern(node_id, std::move(pair));
                            return visitor(GeneratedMacroState{id, lhs_state.accepting && rhs_state.accepting});
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
                SetState sub_initial_states{};
                sub_initial_states.reserve(8);
                bool accepting = true;
                if (!for_each_initial_macro_state(node.lhs, [&](const GeneratedMacroState& sub_initial_state) {
                        sub_initial_states.insert(sub_initial_state.id);
                        accepting = accepting && !sub_initial_state.accepting;
                        return true;
                    })) {
                    return false;
                }

                MacroStateId id = macro_store.intern(node_id, std::move(sub_initial_states));
                if (!visitor(GeneratedMacroState{id, accepting})) {
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

                for (const State next_state : nfa.delta.get_successors(s, local_sym)) {
                    if (!visitor(
                                GeneratedMacroState{
                                        static_cast<MacroStateId>(next_state), nfa.final.contains(next_state)})) {
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
                        if (!visitor(
                                    GeneratedMacroState{
                                            static_cast<MacroStateId>(after_output),
                                            nft.final.contains(after_output)})) {
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
                            next_node_id, tagged.state, sym, sym2, [&](const GeneratedMacroState& next_state) {
                                TaggedState next_tagged{next_state.id, tagged.tag};
                                const MacroStateId next_id = macro_store.intern(node_id, std::move(next_tagged));
                                return visitor(GeneratedMacroState{next_id, next_state.accepting});
                            })) {
                    return false;
                }

                break;
            }


            case NodeKind::Intersect: {
                const PairState pair = macro_store.get_pair(node_id, state);

                if (!emit_paired_successors(
                            node_id, node.lhs, pair.lhs, sym, sym2, node.rhs, pair.rhs, sym, sym2, visitor)) {
                    return false;
                }

                break;
            }


            case NodeKind::PreImage: {
                const PairState pair = macro_store.get_pair(node_id, state);

                const NodeId nfa_id = node.lhs;
                const NodeId nft_id = node.rhs;

                for (const mata::Symbol sync_sym : output_alphabets[nft_id].get_alphabet_symbols()) {
                    if (!emit_paired_successors(
                                node_id, nfa_id, pair.lhs, sync_sym, 0, nft_id, pair.rhs, sym, sync_sym, visitor)) {
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
                    if (!emit_paired_successors(
                                node_id, nfa_id, pair.lhs, sync_sym, 0, nft_id, pair.rhs, sync_sym, sym, visitor)) {
                        return false;
                    }
                }

                break;
            }

            case NodeKind::Compose: {
                const PairState pair = macro_store.get_pair(node_id, state);

                for (const mata::Symbol sync_sym : output_alphabets[node.lhs].get_alphabet_symbols()) {
                    if (!emit_paired_successors(
                                node_id, node.lhs, pair.lhs, sym, sync_sym, node.rhs, pair.rhs, sync_sym, sym2,
                                visitor)) {
                        return false;
                    }
                }

                break;
            }

            case NodeKind::Complement: {
                const SetState& sub_states = macro_store.get_set(node_id, state);
                if (!emit_complement_successor(node_id, node.lhs, sub_states, sym, 0, visitor)) {
                    return false;
                }

                break;
            }

            case NodeKind::ComplementNft: {
                const SetState& sub_states = macro_store.get_set(node_id, state);
                if (!emit_complement_successor(node_id, node.lhs, sub_states, sym, sym2, visitor)) {
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

    // Check if state1 is subsumed by state2,
    // which means that the language represented by state1 is a subset of the language
    bool subsumed_state(const NodeId node_id, const MacroStateId state1, const MacroStateId state2) {
        if (state1 == state2) {
            return true;
        }

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
                const SetState& lhs_sub_states = macro_store.get_set(node_id, state2);
                const SetState& rhs_sub_states = macro_store.get_set(node_id, state1);

                for (const MacroStateId& lhs_sub_state : lhs_sub_states) {
                    bool subsumed = false;

                    for (const MacroStateId& rhs_sub_state : rhs_sub_states) {
                        if (subsumed_state(node.lhs, lhs_sub_state, rhs_sub_state)) {
                            subsumed = true;
                            break;
                        }
                    }

                    if (!subsumed) {
                        return false;
                    }
                }

                return true;
            }
        }

        return false;
    }

    bool is_subsumed(
            const MacroStateId state, std::unordered_set<MacroStateId>& visited, std::list<MacroStateId>& worklist) {
        for (const MacroStateId& visited_state : visited) {
            if (subsumed_state(root_id, state, visited_state)) {
                return true;
            }
        }

        for (const MacroStateId& worklist_state : worklist) {
            if (subsumed_state(root_id, state, worklist_state)) {
                return true;
            }
        }

        // Maintaining only minimal states in the visited and worklist sets,
        // so we remove all states that are subsumed by the new state.
        std::erase_if(visited, [&](const MacroStateId& visited_state) {
            return subsumed_state(root_id, visited_state, state);
        });

        std::erase_if(worklist, [&](const MacroStateId& worklist_state) {
            return subsumed_state(root_id, worklist_state, state);
        });

        return false;
    }
};

bool is_empty_impl(Context& ctx, bool is_nft) {
    std::list<MacroStateId> worklist{};
    std::unordered_set<MacroStateId> visited{};

    const auto enqueue_if_relevant = [&](const Context::GeneratedMacroState& generated_state) {
        if (generated_state.accepting) {
            return false;
        }

        if (ctx.is_subsumed(generated_state.id, visited, worklist)) {
            return true;
        }

        worklist.push_back(generated_state.id);
        return true;
    };

    const bool initial_states_fully_processed = ctx.for_each_initial_macro_state(ctx.root_id, enqueue_if_relevant);

    if (!initial_states_fully_processed) {
        return false;
    }

    const auto& alphabet = ctx.alphabets[ctx.root_id];
    const auto& input_alphabet = ctx.input_alphabets[ctx.root_id];
    const auto& output_alphabet = ctx.output_alphabets[ctx.root_id];

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
                    const bool should_continue =
                            ctx.for_each_next_macro_state(ctx.root_id, current_state, sym, sym2, enqueue_if_relevant);

                    if (!should_continue) {
                        return false;
                    }
                }
            }

        } else {
            for (const mata::Symbol sym : alphabet.get_alphabet_symbols()) {
                const bool should_continue =
                        ctx.for_each_next_macro_state(ctx.root_id, current_state, sym, enqueue_if_relevant);

                if (!should_continue) {
                    return false;
                }
            }
        }
    }

    return true;
}

bool SymbolicAutomataTree::is_empty(const Term& root_node) {
    Context ctx = Context(*this, root_node.get_id());
    return is_empty_impl(ctx, false);
}

bool SymbolicAutomataTree::is_empty(const Term& root_node, const mata::OnTheFlyAlphabet& alphabet) {
    Context ctx = Context(*this, root_node.get_id(), &alphabet);
    return is_empty_impl(ctx, false);
}

bool SymbolicAutomataTree::is_empty(const TermNft& root_node) {
    Context ctx = Context(*this, root_node.get_id());
    return is_empty_impl(ctx, true);
}

bool SymbolicAutomataTree::is_empty(const TermNft& root_node, const mata::OnTheFlyAlphabet& alphabet) {
    Context ctx = Context(*this, root_node.get_id(), &alphabet);
    return is_empty_impl(ctx, true);
}
