/** @file lazy.hh
 * @brief Lazy on-the-fly emptiness checking for symbolic combinations of NFAs.
 *
 * This module wraps the implementation in nft::lazy, but keeps only the
 * arity-1 Boolean language fragment visible at the API level.
 *
 * If the workflow needs to work with transducers, include
 * nft::lazy directly instead of mixing both interfaces.
 */

#ifndef MATA_NFA_LAZY_HH
#define MATA_NFA_LAZY_HH

#include "mata/nft/lazy.hh"

namespace mata::nfa::lazy {

class SymbolicAutomataTree;

/// Opaque handle to a lazy NFA term.
class Term {
    mata::nft::lazy::Term inner_;

    // Keep the handle opaque inside the NFA-only facade.
    explicit Term(const mata::nft::lazy::Term& inner) : inner_{inner} {}

    friend class SymbolicAutomataTree;

public:
    Term() = default;
};

/// NFA-only facade over the lazy symbolic emptiness engine.
class SymbolicAutomataTree {
    mata::nft::lazy::SymbolicAutomataTree tree_;

public:
    /// Create an empty NFA-only lazy tree.
    SymbolicAutomataTree() : tree_{} {}

    /**
     * Insert a concrete NFA leaf.
     * @param nfa Language automaton to store as an arity-1 leaf.
     * @return Handle to the inserted symbolic term.
     */
    Term make_term(const mata::nfa::Nfa& nfa) { return Term{tree_.make_term(nfa)}; }

    /**
     * Union of two language terms.
     * @param lhs Left operand.
     * @param rhs Right operand.
     * @return Symbolic term for `lhs ∪ rhs`.
     */
    Term union_(const Term& lhs, const Term& rhs) { return Term{tree_.union_(lhs.inner_, rhs.inner_)}; }
    /**
     * Intersection of two language terms.
     * @param lhs Left operand.
     * @param rhs Right operand.
     * @return Symbolic term for `lhs ∩ rhs`.
     */
    Term intersect(const Term& lhs, const Term& rhs) { return Term{tree_.intersect(lhs.inner_, rhs.inner_)}; }
    /**
     * Complement of a language term.
     * @param sub Operand to complement.
     * @return Symbolic term for the complement of @p sub.
     */
    Term complement(const Term& sub) { return Term{tree_.complement(sub.inner_)}; }

    /**
     * Check that @p root refers to a valid node in this tree.
     * @param root Symbolic term handle to validate.
     * @return `true` if the handle refers to a structurally valid reachable node.
     */
    bool is_valid(const Term& root) const { return tree_.is_valid(root.inner_); }

    /**
     * Check emptiness using alphabets inferred from the leaves.
     * @param root Root of the symbolic language to test.
     * @return `true` if the denoted language is empty.
     */
    bool is_empty(const Term& root) { return tree_.is_empty(root.inner_); }
    /**
     * Check emptiness using one shared alphabet for the whole language.
     * @param root Root of the symbolic language to test.
     * @param alphabet Alphabet used for the whole language universe.
     * @return `true` if the denoted language is empty.
     */
    bool is_empty(const Term& root, const mata::OnTheFlyAlphabet& alphabet) {
        return tree_.is_empty(root.inner_, alphabet);
    }
};

} // namespace mata::nfa::lazy

#endif // MATA_NFA_LAZY_HH
