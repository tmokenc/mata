/**
 * @file alphabet_store.cc
 * @brief Private alphabet storage and resolution helpers for mata::nft::lazy::detail.
 */

#include "alphabet_store.hh"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace mata::nft::lazy::detail {

namespace {

void add_symbols_to_canonical(const mata::OnTheFlyAlphabet& alphabet, mata::OnTheFlyAlphabet& canonical_alphabet) {
    for (const mata::Symbol symbol : alphabet.get_alphabet_symbols()) {
        canonical_alphabet.translate_symb(alphabet.reverse_translate_symbol(symbol));
    }
}

void collect_local_level_alphabets(
        const std::vector<ExecNode>& nodes, const NodeId node_id, const std::vector<mata::nfa::Nfa>& nfas,
        const std::vector<mata::nft::Nft>& nfts, std::vector<std::vector<mata::OnTheFlyAlphabet>>& level_alphabets,
        std::vector<bool>& visited) {
    if (visited[node_id]) {
        return;
    }

    const ExecNode& node = nodes[node_id];
    level_alphabets[node_id].resize(node.result_arity);

    switch (node.kind) {
        case ExecKind::LeafNfa: {
            const mata::nfa::Nfa& nfa = nfas[node.lhs];
            mata::OnTheFlyAlphabet& alphabet = level_alphabets[node_id][0];
            for (const auto& state_post : nfa.delta) {
                for (const auto& symbol_post : state_post) {
                    alphabet.translate_symb(AlphabetStore::symbol_name_for(nfa.alphabet, symbol_post.symbol));
                }
            }
            break;
        }

        case ExecKind::LeafNft:
        case ExecKind::Arity2LeafNft: {
            const mata::nft::Nft& nft = nfts[node.lhs];
            std::vector<mata::OnTheFlyAlphabet>& node_level_alphabets = level_alphabets[node_id];
            for (mata::nfa::State state = 0; state < nft.delta.num_of_states(); ++state) {
                const uint8_t level = static_cast<uint8_t>(nft.levels[state]);
                assert(level < node_level_alphabets.size());

                for (const auto& symbol_post : nft.delta.state_post(state)) {
                    node_level_alphabets[level].translate_symb(AlphabetStore::symbol_name_for(
                            const_cast<mata::Alphabet*>(nft.alphabet_of_level(level)), symbol_post.symbol));
                }
            }
            break;
        }

        case ExecKind::Union:
        case ExecKind::Intersect:
        case ExecKind::Arity1Union:
        case ExecKind::Arity1Intersect:
        case ExecKind::Arity2Union:
        case ExecKind::Arity2Intersect:
        case ExecKind::SyncProduct:
        case ExecKind::Arity2SyncProduct:
            collect_local_level_alphabets(nodes, node.lhs, nfas, nfts, level_alphabets, visited);
            collect_local_level_alphabets(nodes, node.rhs, nfas, nfts, level_alphabets, visited);
            break;

        case ExecKind::Complement:
        case ExecKind::Identity:
        case ExecKind::Project:
        case ExecKind::Arity1Complement:
        case ExecKind::Arity2Complement:
        case ExecKind::Arity2Project:
            collect_local_level_alphabets(nodes, node.lhs, nfas, nfts, level_alphabets, visited);
            break;
    }

    visited[node_id] = true;
}

void canonicalize_level_alphabets(
        const std::vector<ExecNode>& nodes, const NodeId root_id, const std::vector<SyncPlan>& sync_plans,
        const std::vector<ProjectPlan>& project_plans, std::vector<std::vector<mata::OnTheFlyAlphabet>>& level_alphabets,
        const std::vector<mata::OnTheFlyAlphabet>* root_level_alphabets) {
    std::vector<size_t> level_offsets(nodes.size() + 1, 0);
    for (size_t node_id = 0; node_id < nodes.size(); ++node_id) {
        level_offsets[node_id + 1] = level_offsets[node_id] + nodes[node_id].result_arity;
    }

    const size_t total_levels = level_offsets.back();
    std::vector<size_t> parent(total_levels, 0);
    std::vector<uint8_t> rank(total_levels, 0);
    for (size_t i = 0; i < total_levels; ++i) {
        parent[i] = i;
    }

    const auto level_index = [&](const NodeId node_id, const uint8_t level) { return level_offsets[node_id] + level; };

    auto find_root = [&](size_t idx) {
        size_t root = idx;
        while (parent[root] != root) {
            root = parent[root];
        }
        while (parent[idx] != idx) {
            const size_t next = parent[idx];
            parent[idx] = root;
            idx = next;
        }
        return root;
    };

    auto unite = [&](const size_t lhs, const size_t rhs) {
        size_t lhs_root = find_root(lhs);
        size_t rhs_root = find_root(rhs);
        if (lhs_root == rhs_root) {
            return;
        }

        if (rank[lhs_root] < rank[rhs_root]) {
            std::swap(lhs_root, rhs_root);
        }

        parent[rhs_root] = lhs_root;
        if (rank[lhs_root] == rank[rhs_root]) {
            ++rank[lhs_root];
        }
    };

    for (size_t node_index = 0; node_index < nodes.size(); ++node_index) {
        const NodeId node_id = static_cast<NodeId>(node_index);
        const ExecNode& node = nodes[node_id];

        switch (node.kind) {
            case ExecKind::LeafNfa:
            case ExecKind::LeafNft:
            case ExecKind::Arity2LeafNft:
                break;

            case ExecKind::Union:
            case ExecKind::Intersect:
            case ExecKind::Arity1Union:
            case ExecKind::Arity1Intersect:
            case ExecKind::Arity2Union:
            case ExecKind::Arity2Intersect:
                for (uint8_t level = 0; level < node.result_arity; ++level) {
                    unite(level_index(node_id, level), level_index(node.lhs, level));
                    unite(level_index(node_id, level), level_index(node.rhs, level));
                }
                break;

            case ExecKind::Complement:
            case ExecKind::Arity1Complement:
            case ExecKind::Arity2Complement:
                for (uint8_t level = 0; level < node.result_arity; ++level) {
                    unite(level_index(node_id, level), level_index(node.lhs, level));
                }
                break;

            case ExecKind::Identity:
                unite(level_index(node_id, 0), level_index(node.lhs, 0));
                unite(level_index(node_id, 1), level_index(node.lhs, 0));
                break;

            case ExecKind::Project:
            case ExecKind::Arity2Project: {
                const ProjectPlan& plan = project_plans[node.payload];
                for (uint8_t level = 0; level < node.result_arity; ++level) {
                    unite(level_index(node_id, level), level_index(node.lhs, plan.kept_levels[level]));
                }
                break;
            }

            case ExecKind::SyncProduct:
            case ExecKind::Arity2SyncProduct: {
                const SyncPlan& plan = sync_plans[node.payload];
                for (size_t i = 0; i < plan.lhs_sync_levels.size(); ++i) {
                    unite(level_index(node.lhs, plan.lhs_sync_levels[i]), level_index(node.rhs, plan.rhs_sync_levels[i]));
                }

                for (uint8_t level = 0; level < node.result_arity; ++level) {
                    const LevelRef ref = plan.result_layout[level];
                    unite(level_index(node_id, level), ref.side == LevelRef::Side::Lhs
                                                           ? level_index(node.lhs, ref.level)
                                                           : level_index(node.rhs, ref.level));
                }
                break;
            }
        }
    }

    std::vector<mata::OnTheFlyAlphabet> canonical_alphabets(total_levels);
    for (size_t node_index = 0; node_index < nodes.size(); ++node_index) {
        const NodeId node_id = static_cast<NodeId>(node_index);
        for (uint8_t level = 0; level < nodes[node_id].result_arity; ++level) {
            add_symbols_to_canonical(
                    level_alphabets[node_id][level], canonical_alphabets[find_root(level_index(node_id, level))]);
        }
    }

    if (root_level_alphabets != nullptr) {
        if (root_level_alphabets->size() != nodes[root_id].result_arity) {
            throw std::invalid_argument("The number of root level alphabets must match the root arity");
        }

        for (uint8_t level = 0; level < nodes[root_id].result_arity; ++level) {
            add_symbols_to_canonical(
                    (*root_level_alphabets)[level], canonical_alphabets[find_root(level_index(root_id, level))]);
        }
    }

    for (size_t node_index = 0; node_index < nodes.size(); ++node_index) {
        const NodeId node_id = static_cast<NodeId>(node_index);
        for (uint8_t level = 0; level < nodes[node_id].result_arity; ++level) {
            level_alphabets[node_id][level] = canonical_alphabets[find_root(level_index(node_id, level))];
        }
    }
}

} // namespace

std::string AlphabetStore::symbol_name_for(mata::Alphabet* alphabet, const mata::Symbol symbol) {
    if (alphabet == nullptr) {
        return std::to_string(symbol);
    }

    return alphabet->reverse_translate_symbol(symbol);
}

AlphabetStore::AlphabetStore(
        const std::vector<ExecNode>& nodes, const NodeId root_id, const std::vector<mata::nfa::Nfa>& nfas,
        const std::vector<mata::nft::Nft>& nfts, const std::vector<SyncPlan>& sync_plans,
        const std::vector<ProjectPlan>& project_plans, const std::vector<mata::OnTheFlyAlphabet>* root_level_alphabets)
    : level_alphabets(nodes.size()) {
    std::vector<bool> visited(nodes.size(), false);
    collect_local_level_alphabets(nodes, root_id, nfas, nfts, level_alphabets, visited);
    canonicalize_level_alphabets(nodes, root_id, sync_plans, project_plans, level_alphabets, root_level_alphabets);
}

bool AlphabetStore::try_translate_local_symbol_to_resolved(
        const mata::nft::Nft& nft, const uint8_t source_level, const NodeId node_id, const uint8_t result_level,
        const mata::Symbol local_symbol, mata::Symbol& resolved_symbol) const {
    try {
        const std::string symbol_name =
                symbol_name_for(const_cast<mata::Alphabet*>(nft.alphabet_of_level(source_level)), local_symbol);
        return try_translate_symbol_name_to_resolved(node_id, result_level, symbol_name, resolved_symbol);
    } catch (const std::runtime_error&) { return false; }
}

const mata::OnTheFlyAlphabet& AlphabetStore::level_alphabet(const NodeId node_id, const uint8_t level) const {
    return level_alphabets[node_id][level];
}

bool AlphabetStore::try_translate_symbol_name_to_resolved(
        const NodeId node_id, const uint8_t level, const std::string& symbol_name,
        mata::Symbol& resolved_symbol) const {
    const mata::OnTheFlyAlphabet& resolved_alphabet = level_alphabet(node_id, level);
    const auto it = resolved_alphabet.get_symbol_map().find(symbol_name);
    if (it == resolved_alphabet.get_symbol_map().end()) {
        return false;
    }

    resolved_symbol = it->second;
    return true;
}

} // namespace mata::nft::lazy::detail
