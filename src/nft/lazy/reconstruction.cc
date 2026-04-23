/**
 * @file reconstruction.cc
 * @brief Private symbolic-formula DAG reconstruction for mata::nft::lazy::detail.
 */

#include "reconstruction.hh"

#include <cassert>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace mata::nft::lazy::detail {

namespace {

    struct ReconstructionMetrics {
        uint32_t depth;
        float log2_total_possible_states;
    };

    float log2_state_count(const size_t state_count) {
        assert(state_count > 0);
        return std::log2(static_cast<float>(state_count));
    }

    float log2_add(const float lhs, const float rhs) {
        if (std::isinf(lhs) || std::isinf(rhs)) {
            return std::numeric_limits<float>::infinity();
        }

        const float max_term = std::max(lhs, rhs);
        return max_term + std::log2(std::exp2(lhs - max_term) + std::exp2(rhs - max_term));
    }

    ReconstructionMetrics complement_metrics(const ReconstructionMetrics& child_metrics) {
        // Subset construction: child has 2^N states (N stored as log2), so complement has up to
        // 2^(2^N), whose log2 is 2^N = exp2(N) = exp2(child.log2_total_possible_states).
        return ReconstructionMetrics{
                child_metrics.depth + 1,
                std::exp2(child_metrics.log2_total_possible_states),
        };
    }

    ReconstructionMetrics unary_metrics(const NodeKind kind, const ReconstructionMetrics& child_metrics) {
        switch (kind) {
            case NodeKind::Complement:
                return complement_metrics(child_metrics);
            case NodeKind::Identity:
            case NodeKind::Project:
                return ReconstructionMetrics{
                        child_metrics.depth + 1,
                        child_metrics.log2_total_possible_states,
                };
            default:
                unreachable_kind(kind, "unary metrics");
        }
    }

    ReconstructionMetrics binary_metrics(
            const NodeKind kind, const ReconstructionMetrics& lhs_metrics, const ReconstructionMetrics& rhs_metrics) {
        switch (kind) {
            case NodeKind::Union:
                return ReconstructionMetrics{
                        1 + std::max(lhs_metrics.depth, rhs_metrics.depth),
                        log2_add(lhs_metrics.log2_total_possible_states, rhs_metrics.log2_total_possible_states),
                };
            case NodeKind::Intersect:
            case NodeKind::SyncProduct:
                return ReconstructionMetrics{
                        1 + std::max(lhs_metrics.depth, rhs_metrics.depth),
                        lhs_metrics.log2_total_possible_states + rhs_metrics.log2_total_possible_states,
                };
            default:
                unreachable_kind(kind, "binary metrics");
        }
    }

    bool lhs_first(const ReconstructionMetrics& lhs_metrics, const ReconstructionMetrics& rhs_metrics) {
        if (lhs_metrics.depth != rhs_metrics.depth) {
            return lhs_metrics.depth < rhs_metrics.depth;
        }

        return lhs_metrics.log2_total_possible_states <= rhs_metrics.log2_total_possible_states;
    }

    bool is_commutative(const NodeKind kind) {
        // SyncProduct is binary but order-sensitive (result_layout pins each output level to a
        // specific side), so swapping its operands would change the result.
        return kind == NodeKind::Union || kind == NodeKind::Intersect;
    }

    void reorder_binary(
            const SymbolicFormula& formula, std::vector<ExecNode>& output,
            const std::vector<NodeId>& reorderable_nodes) {
        std::vector<ReconstructionMetrics> metrics(output.size());

        for (size_t node_index = 0; node_index < output.size(); ++node_index) {
            const NodeId node_id = static_cast<NodeId>(node_index);
            const ExecNode& node = output[node_id];
            switch (node.kind) {
                case NodeKind::LeafNfa:
                    metrics[node_id] =
                            ReconstructionMetrics{0, log2_state_count(formula.nfas[node.lhs].num_of_states())};
                    break;

                case NodeKind::LeafNft:
                    metrics[node_id] =
                            ReconstructionMetrics{0, log2_state_count(formula.nfts[node.lhs].num_of_states())};
                    break;

                case NodeKind::Complement:
                case NodeKind::Identity:
                case NodeKind::Project:
                    metrics[node_id] = unary_metrics(node.kind, metrics[node.lhs]);
                    break;

                case NodeKind::Union:
                case NodeKind::Intersect:
                case NodeKind::SyncProduct:
                    metrics[node_id] = binary_metrics(node.kind, metrics[node.lhs], metrics[node.rhs]);
                    break;
            }
        }

        for (const NodeId node_id : reorderable_nodes) {
            ExecNode& node = output[node_id];
            if (!lhs_first(metrics[node.lhs], metrics[node.rhs])) {
                std::swap(node.lhs, node.rhs);
            }
        }
    }

    NodeId reconstruct_nodes(
            const SymbolicFormula& formula, const NodeId id, std::vector<ExecNode>& output,
            std::vector<NodeId>& reorderable_nodes, std::vector<std::optional<NodeId>>& rebuilt_ids,
            std::vector<bool>& active_path) {
        if (const std::optional<NodeId> rebuilt_id = rebuilt_ids[id]; rebuilt_id.has_value()) {
            return *rebuilt_id;
        }

        if (active_path[id]) {
            throw std::runtime_error("Cycle detected in the symbolic formula DAG");
        }

        const Node& node = formula.nodes[id];
        active_path[id] = true;
        const auto finish = [&](const NodeId new_id) {
            active_path[id] = false;
            rebuilt_ids[id] = new_id;
            return new_id;
        };

        switch (node.kind) {
            case NodeKind::LeafNfa:
            case NodeKind::LeafNft:
                output.push_back(ExecNode{node.kind, node.result_arity, node.lhs, node.rhs, node.payload});
                return finish(static_cast<NodeId>(output.size() - 1));

            case NodeKind::Union:
            case NodeKind::Intersect: {
                const NodeId lhs_id =
                        reconstruct_nodes(formula, node.lhs, output, reorderable_nodes, rebuilt_ids, active_path);
                const NodeId rhs_id =
                        reconstruct_nodes(formula, node.rhs, output, reorderable_nodes, rebuilt_ids, active_path);

                output.push_back(ExecNode{node.kind, node.result_arity, lhs_id, rhs_id, node.payload});
                if (is_commutative(node.kind)) {
                    reorderable_nodes.push_back(static_cast<NodeId>(output.size() - 1));
                }
                return finish(static_cast<NodeId>(output.size() - 1));
            }

            case NodeKind::SyncProduct: {
                const NodeId lhs_id =
                        reconstruct_nodes(formula, node.lhs, output, reorderable_nodes, rebuilt_ids, active_path);
                const NodeId rhs_id =
                        reconstruct_nodes(formula, node.rhs, output, reorderable_nodes, rebuilt_ids, active_path);

                output.push_back(ExecNode{node.kind, node.result_arity, lhs_id, rhs_id, node.payload});
                return finish(static_cast<NodeId>(output.size() - 1));
            }

            case NodeKind::Complement: {
                const Node& child_node = formula.nodes[node.lhs];

                // De Morgan: ¬(A ∪ B) → ¬A ∩ ¬B,  ¬(A ∩ B) → ¬A ∪ ¬B.
                const auto push_demorgan = [&](const NodeKind dual_kind) {
                    const NodeId lhs_id = reconstruct_nodes(
                            formula, child_node.lhs, output, reorderable_nodes, rebuilt_ids, active_path);
                    const NodeId rhs_id = reconstruct_nodes(
                            formula, child_node.rhs, output, reorderable_nodes, rebuilt_ids, active_path);

                    const NodeId complement_lhs_id = static_cast<NodeId>(output.size());
                    const NodeId complement_rhs_id = complement_lhs_id + 1;

                    output.push_back(ExecNode{NodeKind::Complement, node.result_arity, lhs_id, 0, NO_PAYLOAD});
                    output.push_back(ExecNode{NodeKind::Complement, node.result_arity, rhs_id, 0, NO_PAYLOAD});
                    output.push_back(
                            ExecNode{dual_kind, node.result_arity, complement_lhs_id, complement_rhs_id, NO_PAYLOAD});
                    reorderable_nodes.push_back(static_cast<NodeId>(output.size() - 1));
                    return finish(static_cast<NodeId>(output.size() - 1));
                };

                switch (child_node.kind) {
                    case NodeKind::Union:
                        return push_demorgan(NodeKind::Intersect);

                    case NodeKind::Intersect:
                        return push_demorgan(NodeKind::Union);

                    case NodeKind::Complement:
                        // ¬¬X reduces to X — skip both complements and reconstruct the grandchild directly.
                        return finish(reconstruct_nodes(
                                formula, child_node.lhs, output, reorderable_nodes, rebuilt_ids, active_path));

                    case NodeKind::LeafNfa:
                    case NodeKind::LeafNft:
                    case NodeKind::Identity:
                    case NodeKind::Project:
                    case NodeKind::SyncProduct: {
                        const NodeId child_id = reconstruct_nodes(
                                formula, node.lhs, output, reorderable_nodes, rebuilt_ids, active_path);
                        output.push_back(ExecNode{NodeKind::Complement, node.result_arity, child_id, 0, NO_PAYLOAD});
                        return finish(static_cast<NodeId>(output.size() - 1));
                    }
                }

                unreachable_kind(child_node.kind, "complement child");
            }

            case NodeKind::Identity:
            case NodeKind::Project: {
                const NodeId child_id =
                        reconstruct_nodes(formula, node.lhs, output, reorderable_nodes, rebuilt_ids, active_path);
                output.push_back(ExecNode{node.kind, node.result_arity, child_id, 0, node.payload});
                return finish(static_cast<NodeId>(output.size() - 1));
            }
        }

        unreachable_kind(node.kind, "reconstruction");
    }

} // namespace

NodeId reconstruct_nodes(const SymbolicFormula& formula, const NodeId id, std::vector<ExecNode>& output) {
    std::vector<NodeId> reorderable_nodes{};
    std::vector<std::optional<NodeId>> rebuilt_ids(formula.nodes.size(), std::nullopt);
    std::vector<bool> active_path(formula.nodes.size(), false);
    const NodeId root_id = reconstruct_nodes(formula, id, output, reorderable_nodes, rebuilt_ids, active_path);
    reorder_binary(formula, output, reorderable_nodes);
    return root_id;
}

} // namespace mata::nft::lazy::detail
