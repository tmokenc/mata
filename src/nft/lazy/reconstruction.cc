/**
 * @file reconstruction.cc
 * @brief Private symbolic-tree reconstruction for mata::nft::lazy::detail.
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
            return std::numeric_limits<long double>::infinity();
        }

        const float max_term = std::max(lhs, rhs);
        return max_term + std::log2(std::exp2(lhs - max_term) + std::exp2(rhs - max_term));
    }

    ReconstructionMetrics complement_metrics(const ReconstructionMetrics& child_metrics) {
        return ReconstructionMetrics{
                child_metrics.depth + 1,
                std::exp2(child_metrics.log2_total_possible_states),
        };
    }

    ReconstructionMetrics unary_metrics(const ExecKind kind, const ReconstructionMetrics& child_metrics) {
        switch (kind) {
            case ExecKind::Complement:
            case ExecKind::Arity1Complement:
            case ExecKind::Arity2Complement:
                return complement_metrics(child_metrics);

            case ExecKind::Identity:
            case ExecKind::Project:
            case ExecKind::Arity2Project:
                return ReconstructionMetrics{
                        child_metrics.depth + 1,
                        child_metrics.log2_total_possible_states,
                };

            case ExecKind::LeafNfa:
            case ExecKind::LeafNft:
            case ExecKind::Union:
            case ExecKind::Intersect:
            case ExecKind::SyncProduct:
            case ExecKind::Arity1Union:
            case ExecKind::Arity1Intersect:
            case ExecKind::Arity2LeafNft:
            case ExecKind::Arity2Union:
            case ExecKind::Arity2Intersect:
            case ExecKind::Arity2SyncProduct:
                break;
        }

        throw std::logic_error("Unreachable unary metrics branch.");
    }

    ReconstructionMetrics binary_metrics(
            const ExecKind kind, const ReconstructionMetrics& lhs_metrics, const ReconstructionMetrics& rhs_metrics) {
        switch (kind) {
            case ExecKind::Union:
            case ExecKind::Arity1Union:
            case ExecKind::Arity2Union:
                return ReconstructionMetrics{
                        1 + std::max(lhs_metrics.depth, rhs_metrics.depth),
                        log2_add(lhs_metrics.log2_total_possible_states, rhs_metrics.log2_total_possible_states),
                };

            case ExecKind::Intersect:
            case ExecKind::SyncProduct:
            case ExecKind::Arity1Intersect:
            case ExecKind::Arity2Intersect:
            case ExecKind::Arity2SyncProduct:
                return ReconstructionMetrics{
                        1 + std::max(lhs_metrics.depth, rhs_metrics.depth),
                        lhs_metrics.log2_total_possible_states + rhs_metrics.log2_total_possible_states,
                };

            case ExecKind::LeafNfa:
            case ExecKind::LeafNft:
            case ExecKind::Complement:
            case ExecKind::Identity:
            case ExecKind::Project:
            case ExecKind::Arity1Complement:
            case ExecKind::Arity2LeafNft:
            case ExecKind::Arity2Complement:
            case ExecKind::Arity2Project:
                break;
        }

        throw std::logic_error("Unreachable binary metrics branch.");
    }

    bool should_place_left_first(const ReconstructionMetrics& lhs_metrics, const ReconstructionMetrics& rhs_metrics) {
        if (lhs_metrics.depth != rhs_metrics.depth) {
            return lhs_metrics.depth < rhs_metrics.depth;
        }

        return lhs_metrics.log2_total_possible_states <= rhs_metrics.log2_total_possible_states;
    }

    bool is_reorderable_binary_kind(const ExecKind kind) {
        switch (kind) {
            case ExecKind::Union:
            case ExecKind::Intersect:
            case ExecKind::Arity1Union:
            case ExecKind::Arity1Intersect:
            case ExecKind::Arity2Union:
            case ExecKind::Arity2Intersect:
                return true;

            case ExecKind::LeafNfa:
            case ExecKind::LeafNft:
            case ExecKind::Complement:
            case ExecKind::Identity:
            case ExecKind::Project:
            case ExecKind::SyncProduct:
            case ExecKind::Arity1Complement:
            case ExecKind::Arity2LeafNft:
            case ExecKind::Arity2Complement:
            case ExecKind::Arity2Project:
            case ExecKind::Arity2SyncProduct:
                return false;
        }

        throw std::logic_error("Unreachable reorderable-binary-kind branch.");
    }

    void reorder_reconstructed_binary_nodes(
            const SymbolicAutomataTree& tree, std::vector<ExecNode>& output,
            const std::vector<NodeId>& reorderable_nodes) {
        std::vector<ReconstructionMetrics> metrics(output.size());

        for (size_t node_index = 0; node_index < output.size(); ++node_index) {
            const NodeId node_id = static_cast<NodeId>(node_index);
            const ExecNode& node = output[node_id];
            switch (node.kind) {
                case ExecKind::LeafNfa:
                    metrics[node_id] = ReconstructionMetrics{0, log2_state_count(tree.nfas[node.lhs].num_of_states())};
                    break;

                case ExecKind::LeafNft:
                case ExecKind::Arity2LeafNft:
                    metrics[node_id] = ReconstructionMetrics{0, log2_state_count(tree.nfts[node.lhs].num_of_states())};
                    break;

                case ExecKind::Complement:
                case ExecKind::Identity:
                case ExecKind::Project:
                case ExecKind::Arity1Complement:
                case ExecKind::Arity2Complement:
                case ExecKind::Arity2Project:
                    metrics[node_id] = unary_metrics(node.kind, metrics[node.lhs]);
                    break;

                case ExecKind::Union:
                case ExecKind::Intersect:
                case ExecKind::SyncProduct:
                case ExecKind::Arity1Union:
                case ExecKind::Arity1Intersect:
                case ExecKind::Arity2Union:
                case ExecKind::Arity2Intersect:
                case ExecKind::Arity2SyncProduct:
                    metrics[node_id] = binary_metrics(node.kind, metrics[node.lhs], metrics[node.rhs]);
                    break;
            }
        }

        for (const NodeId node_id : reorderable_nodes) {
            ExecNode& node = output[node_id];
            if (!should_place_left_first(metrics[node.lhs], metrics[node.rhs])) {
                std::swap(node.lhs, node.rhs);
            }
        }
    }

    ExecKind classify_leaf_kind(const NodeKind kind, const uint8_t result_arity) {
        switch (kind) {
            case NodeKind::LeafNfa:
                return ExecKind::LeafNfa;
            case NodeKind::LeafNft:
                return result_arity == 2 ? ExecKind::Arity2LeafNft : ExecKind::LeafNft;
            case NodeKind::Union:
            case NodeKind::Intersect:
            case NodeKind::Complement:
            case NodeKind::Identity:
            case NodeKind::Project:
            case NodeKind::SyncProduct:
                break;
        }

        throw std::logic_error("Unreachable leaf classification branch.");
    }

    ExecKind classify_unary_kind(const NodeKind kind, const uint8_t result_arity, const ExecKind child_kind) {
        switch (kind) {
            case NodeKind::Complement:
                if (result_arity == 1 && is_arity1_exec_kind(child_kind)) {
                    return ExecKind::Arity1Complement;
                }
                return result_arity == 2 ? ExecKind::Arity2Complement : ExecKind::Complement;

            case NodeKind::Identity:
                return ExecKind::Identity;

            case NodeKind::Project:
                return result_arity == 2 ? ExecKind::Arity2Project : ExecKind::Project;

            case NodeKind::LeafNfa:
            case NodeKind::LeafNft:
            case NodeKind::Union:
            case NodeKind::Intersect:
            case NodeKind::SyncProduct:
                break;
        }

        throw std::logic_error("Unreachable unary classification branch.");
    }

    ExecKind classify_binary_kind(
            const NodeKind kind, const uint8_t result_arity, const ExecKind lhs_kind, const ExecKind rhs_kind) {
        switch (kind) {
            case NodeKind::Union:
                if (result_arity == 1 && is_arity1_exec_kind(lhs_kind) && is_arity1_exec_kind(rhs_kind)) {
                    return ExecKind::Arity1Union;
                }
                return result_arity == 2 ? ExecKind::Arity2Union : ExecKind::Union;

            case NodeKind::Intersect:
                if (result_arity == 1 && is_arity1_exec_kind(lhs_kind) && is_arity1_exec_kind(rhs_kind)) {
                    return ExecKind::Arity1Intersect;
                }
                return result_arity == 2 ? ExecKind::Arity2Intersect : ExecKind::Intersect;

            case NodeKind::SyncProduct:
                return result_arity == 2 ? ExecKind::Arity2SyncProduct : ExecKind::SyncProduct;

            case NodeKind::LeafNfa:
            case NodeKind::LeafNft:
            case NodeKind::Complement:
            case NodeKind::Identity:
            case NodeKind::Project:
                break;
        }

        throw std::logic_error("Unreachable binary classification branch.");
    }

    NodeId reconstruct_nodes(
            const SymbolicAutomataTree& tree, const NodeId id, std::vector<ExecNode>& output,
            std::vector<NodeId>& reorderable_nodes, std::vector<std::optional<NodeId>>& rebuilt_ids,
            std::vector<bool>& active_path) {
        if (const std::optional<NodeId> rebuilt_id = rebuilt_ids[id]; rebuilt_id.has_value()) {
            return *rebuilt_id;
        }

        if (active_path[id]) {
            throw std::runtime_error("Cycle detected in the symbolic automata tree");
        }

        const Node& node = tree.nodes[id];
        active_path[id] = true;
        const auto finish = [&](const NodeId new_id) {
            active_path[id] = false;
            rebuilt_ids[id] = new_id;
            return new_id;
        };

        switch (node.kind) {
            case NodeKind::LeafNfa:
            case NodeKind::LeafNft:
                output.push_back(
                        ExecNode{
                                classify_leaf_kind(node.kind, node.result_arity), node.lhs, node.rhs, node.payload,
                                node.result_arity});
                return finish(static_cast<NodeId>(output.size() - 1));

            case NodeKind::Union:
            case NodeKind::Intersect: {
                const NodeId lhs_id =
                        reconstruct_nodes(tree, node.lhs, output, reorderable_nodes, rebuilt_ids, active_path);
                const NodeId rhs_id =
                        reconstruct_nodes(tree, node.rhs, output, reorderable_nodes, rebuilt_ids, active_path);

                output.push_back(
                        ExecNode{
                                classify_binary_kind(
                                        node.kind, node.result_arity, output[lhs_id].kind, output[rhs_id].kind),
                                lhs_id, rhs_id, node.payload, node.result_arity});
                if (is_reorderable_binary_kind(output.back().kind)) {
                    reorderable_nodes.push_back(static_cast<NodeId>(output.size() - 1));
                }
                return finish(static_cast<NodeId>(output.size() - 1));
            }

            case NodeKind::SyncProduct: {
                const NodeId lhs_id =
                        reconstruct_nodes(tree, node.lhs, output, reorderable_nodes, rebuilt_ids, active_path);
                const NodeId rhs_id =
                        reconstruct_nodes(tree, node.rhs, output, reorderable_nodes, rebuilt_ids, active_path);

                output.push_back(
                        ExecNode{
                                classify_binary_kind(
                                        node.kind, node.result_arity, output[lhs_id].kind, output[rhs_id].kind),
                                lhs_id, rhs_id, node.payload, node.result_arity});
                return finish(static_cast<NodeId>(output.size() - 1));
            }

            case NodeKind::Complement: {
                const Node& child_node = tree.nodes[node.lhs];

                switch (child_node.kind) {
                    case NodeKind::Union: {
                        const NodeId lhs_id = reconstruct_nodes(
                                tree, child_node.lhs, output, reorderable_nodes, rebuilt_ids, active_path);
                        const NodeId rhs_id = reconstruct_nodes(
                                tree, child_node.rhs, output, reorderable_nodes, rebuilt_ids, active_path);

                        const NodeId complement_lhs_id = static_cast<NodeId>(output.size());
                        const NodeId complement_rhs_id = complement_lhs_id + 1;

                        output.push_back(
                                ExecNode{
                                        classify_unary_kind(
                                                NodeKind::Complement, node.result_arity, output[lhs_id].kind),
                                        lhs_id, 0, NO_PAYLOAD, node.result_arity});
                        output.push_back(
                                ExecNode{
                                        classify_unary_kind(
                                                NodeKind::Complement, node.result_arity, output[rhs_id].kind),
                                        rhs_id, 0, NO_PAYLOAD, node.result_arity});
                        output.push_back(
                                ExecNode{
                                        classify_binary_kind(
                                                NodeKind::Intersect, node.result_arity, output[complement_lhs_id].kind,
                                                output[complement_rhs_id].kind),
                                        complement_lhs_id, complement_rhs_id, NO_PAYLOAD, node.result_arity});
                        if (is_reorderable_binary_kind(output.back().kind)) {
                            reorderable_nodes.push_back(static_cast<NodeId>(output.size() - 1));
                        }
                        return finish(static_cast<NodeId>(output.size() - 1));
                    }

                    case NodeKind::Intersect: {
                        const NodeId lhs_id = reconstruct_nodes(
                                tree, child_node.lhs, output, reorderable_nodes, rebuilt_ids, active_path);
                        const NodeId rhs_id = reconstruct_nodes(
                                tree, child_node.rhs, output, reorderable_nodes, rebuilt_ids, active_path);

                        const NodeId complement_lhs_id = static_cast<NodeId>(output.size());
                        const NodeId complement_rhs_id = complement_lhs_id + 1;

                        output.push_back(
                                ExecNode{
                                        classify_unary_kind(
                                                NodeKind::Complement, node.result_arity, output[lhs_id].kind),
                                        lhs_id, 0, NO_PAYLOAD, node.result_arity});
                        output.push_back(
                                ExecNode{
                                        classify_unary_kind(
                                                NodeKind::Complement, node.result_arity, output[rhs_id].kind),
                                        rhs_id, 0, NO_PAYLOAD, node.result_arity});
                        output.push_back(
                                ExecNode{
                                        classify_binary_kind(
                                                NodeKind::Union, node.result_arity, output[complement_lhs_id].kind,
                                                output[complement_rhs_id].kind),
                                        complement_lhs_id, complement_rhs_id, NO_PAYLOAD, node.result_arity});
                        if (is_reorderable_binary_kind(output.back().kind)) {
                            reorderable_nodes.push_back(static_cast<NodeId>(output.size() - 1));
                        }
                        return finish(static_cast<NodeId>(output.size() - 1));
                    }

                    case NodeKind::Complement:
                        return finish(reconstruct_nodes(
                                tree, child_node.lhs, output, reorderable_nodes, rebuilt_ids, active_path));

                    case NodeKind::LeafNfa:
                    case NodeKind::LeafNft:
                    case NodeKind::Identity:
                    case NodeKind::Project:
                    case NodeKind::SyncProduct: {
                        const NodeId child_id =
                                reconstruct_nodes(tree, node.lhs, output, reorderable_nodes, rebuilt_ids, active_path);
                        output.push_back(
                                ExecNode{
                                        classify_unary_kind(
                                                NodeKind::Complement, node.result_arity, output[child_id].kind),
                                        child_id, 0, NO_PAYLOAD, node.result_arity});
                        return finish(static_cast<NodeId>(output.size() - 1));
                    }
                }

                throw std::logic_error("Unreachable complement reconstruction branch.");
            }

            case NodeKind::Identity:
            case NodeKind::Project: {
                const NodeId child_id =
                        reconstruct_nodes(tree, node.lhs, output, reorderable_nodes, rebuilt_ids, active_path);
                output.push_back(
                        ExecNode{
                                classify_unary_kind(node.kind, node.result_arity, output[child_id].kind), child_id, 0,
                                node.payload, node.result_arity});
                return finish(static_cast<NodeId>(output.size() - 1));
            }
        }

        throw std::logic_error("Unreachable reconstruction branch.");
    }

} // namespace

NodeId reconstruct_nodes(const SymbolicAutomataTree& tree, const NodeId id, std::vector<ExecNode>& output) {
    std::vector<NodeId> reorderable_nodes{};
    std::vector<std::optional<NodeId>> rebuilt_ids(tree.nodes.size(), std::nullopt);
    std::vector<bool> active_path(tree.nodes.size(), false);
    const NodeId root_id = reconstruct_nodes(tree, id, output, reorderable_nodes, rebuilt_ids, active_path);
    reorder_reconstructed_binary_nodes(tree, output, reorderable_nodes);
    return root_id;
}

} // namespace mata::nft::lazy::detail
