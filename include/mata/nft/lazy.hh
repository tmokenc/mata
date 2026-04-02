/** @file lazy.hh
 * @brief Lazy on-the-fly emptiness checking for symbolic combinations of NFAs and (2-level) NFTs.
 *
 * The API is intentionally small:
 *  - build a @ref Term using the factory functions,
 *  - call @ref is_empty.
 */

#ifndef MATA_NFT_LAZY_HH
#define MATA_NFT_LAZY_HH

#include <vector>

#include "mata/nfa/nfa.hh"
#include "mata/nft/nft.hh"

namespace mata::nft::lazy {

using NodeId = uint32_t;
using MacroStateId = uint32_t;


class Term {
    NodeId id;

public:
    Term(NodeId id) : id{id} {};
    NodeId get_id() const { return id; }
};

class TermNft {
    NodeId id;

public:
    TermNft(NodeId id) : id{id} {};
    NodeId get_id() const { return id; }
};


enum class NodeKind : uint8_t {
    LeafNfa,
    LeafNft,
    Union,
    Intersect,
    Complement,
    ComplementNft,
    PreImage,
    PostImage,
    Compose,
};

struct Node {
    NodeKind kind;
    NodeId lhs;
    NodeId rhs;
};

/// The main class that implements the lazy on-the-fly emptiness checking for symbolic combinations of NFAs and
/// (2-level) NFTs. The main idea is to represent the symbolic combination of NFAs and NFTs as a tree, where the
/// leaves are NFAs and NFTs, and the internal nodes are the operations. For now, the tree is represented as a
/// vector of nodes, where each node has a kind (the operation) and two children (the operands). each child is
/// represented by its id, which is the index of the node in the vector, or the index of the NFA/NFT in the
/// corresponding vector for leaf nodes.
///
/// Building the tree is done using the factory functions (make_nfa_term, make_nft_term, union_, intersect,
/// complement, pre_image, post_image, compose) which are just thin wrappers around the insert_node function, which
/// inserts a new node into the vector and returns its id in form of a Term or TermNft. This way, the built tree is
/// always valid thank to the type system. However, it should not allow for loops, that is a node cannot be its own
/// ancestor, as this would lead to infinite recursion. The factory functions does not check for this, there is a
/// separate function is_valid that checks if the tree is valid.
///
/// This representation does allows for DAG structure instead of a tree, which allows for sharing of subterms.
class SymbolicAutomataTree {
    NodeId insert_node(NodeKind kind, NodeId lhs, NodeId rhs = 0) {
        const NodeId id = static_cast<NodeId>(nodes.size());
        nodes.push_back(Node{kind, lhs, rhs});
        return id;
    }

public:
    std::vector<nfa::Nfa> nfas;
    std::vector<nft::Nft> nfts;
    std::vector<Node> nodes;

    SymbolicAutomataTree() : nfas{}, nfts{}, nodes{} {};

    Term make_term(const nfa::Nfa& nfa);
    TermNft make_term(const nft::Nft& nft);
    Term union_(const Term& lhs, const Term& rhs);
    Term intersect(const Term& lhs, const Term& rhs);
    Term complement(const Term& sub);
    TermNft complement(const TermNft& sub);
    Term pre_image(const Term& lang_over_output, const TermNft& transducer);
    Term post_image(const Term& lang_over_input, const TermNft& transducer);
    TermNft compose(const TermNft& t1, const TermNft& t2);

    /// Check if the given term is valid, that is does not contain any loop.
    bool is_valid(const Term& root_node) const;
    bool is_valid(const TermNft& root_node) const;

    /// Decide emptiness of the nfa / nft represented by @p root_node.
    bool is_empty(const Term& root_node);
    bool is_empty(const TermNft& root_node);
    bool is_empty(const Term& root_node, const OnTheFlyAlphabet& alphabet);
    bool is_empty(const TermNft& root_node, const OnTheFlyAlphabet& alphabet);
};


} // namespace mata::nft::lazy

#endif // MATA_NFA_LAZY_HH
