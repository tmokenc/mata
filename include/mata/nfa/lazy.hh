/** @file lazy.hh
 * @brief Lazy on-the-fly emptiness checking for symbolic combinations of NFAs.
 *
 * This module wraps @c nft::lazy::SymbolicFormula, but keeps only the
 * arity-1 Boolean language fragment visible at the API level.
 *
 * If the workflow needs to work with transducers or N-tape relations, include
 * @c mata/nft/lazy.hh and use @c nft::lazy::SymbolicFormula directly.
 */

#ifndef MATA_NFA_LAZY_HH
#define MATA_NFA_LAZY_HH

#include "mata/nft/lazy.hh"

namespace mata::nfa::lazy {

class SymbolicFormula;

/// Opaque handle to a lazy NFA term.
class Term {
    mata::nft::lazy::Term inner;

    // Keep the handle opaque inside the NFA-only facade.
    explicit Term(const mata::nft::lazy::Term& t) : inner{t} {}

    friend class SymbolicFormula;

public:
    Term() = default;
};

/// NFA-only facade over the lazy symbolic emptiness engine.
class SymbolicFormula {
    mata::nft::lazy::SymbolicFormula formula;

public:
    SymbolicFormula() : formula{} {}

    /**
     * Insert a concrete NFA leaf.
     * @param nfa Language automaton to store as an arity-1 leaf.
     * @return Handle to the inserted symbolic term.
     */
    Term make_term(const mata::nfa::Nfa& nfa) { return Term{formula.make_term(nfa)}; }

    /**
     * Union of two language terms.
     * @param lhs Left operand.
     * @param rhs Right operand.
     * @return Symbolic term for `lhs ∪ rhs`.
     */
    Term unite(const Term& lhs, const Term& rhs) { return Term{formula.unite(lhs.inner, rhs.inner)}; }
    /**
     * Intersection of two language terms.
     * @param lhs Left operand.
     * @param rhs Right operand.
     * @return Symbolic term for `lhs ∩ rhs`.
     */
    Term intersect(const Term& lhs, const Term& rhs) { return Term{formula.intersect(lhs.inner, rhs.inner)}; }
    /**
     * Complement of a language term.
     * @param sub Operand to complement.
     * @return Symbolic term for the complement of @p sub.
     */
    Term complement(const Term& sub) { return Term{formula.complement(sub.inner)}; }

    /**
     * Check that @p root refers to a valid node in this DAG.
     * @param root Symbolic term handle to validate.
     * @return `true` if the handle refers to a structurally valid reachable node.
     */
    bool is_valid(const Term& root) const { return formula.is_valid(root.inner); }

    /**
     * Check emptiness using alphabets inferred from the leaves.
     * @param root Root of the symbolic language to test.
     * @return `true` if the denoted language is empty.
     */
    bool is_empty(const Term& root) { return formula.is_empty(root.inner); }
    /**
     * Check emptiness using one shared alphabet for the whole language.
     * @param root Root of the symbolic language to test.
     * @param alphabet Alphabet used for the whole language universe.
     * @return `true` if the denoted language is empty.
     */
    bool is_empty(const Term& root, const mata::OnTheFlyAlphabet& alphabet) {
        return formula.is_empty(root.inner, alphabet);
    }
};

} // namespace mata::nfa::lazy

#endif // MATA_NFA_LAZY_HH
