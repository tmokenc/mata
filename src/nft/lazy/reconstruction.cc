/**
 * @file reconstruction.cc
 * @brief Private symbolic-tree reconstruction for mata::nft::lazy::detail.
 */

#include "reconstruction.hh"

#include <optional>
#include <stdexcept>
#include <vector>

namespace mata::nft::lazy::detail {

namespace {

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

ExecKind classify_binary_kind(const NodeKind kind, const uint8_t result_arity, const ExecKind lhs_kind, const ExecKind rhs_kind) {
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
        const std::vector<Node>& original_nodes, const NodeId id, std::vector<ExecNode>& output,
        std::vector<std::optional<NodeId>>& rebuilt_ids, std::vector<bool>& active_path) {
    if (const std::optional<NodeId> rebuilt_id = rebuilt_ids[id]; rebuilt_id.has_value()) {
        return *rebuilt_id;
    }

    if (active_path[id]) {
        throw std::runtime_error("Cycle detected in the symbolic automata tree");
    }

    const Node& node = original_nodes[id];
    active_path[id] = true;
    const auto finish = [&](const NodeId new_id) {
        active_path[id] = false;
        rebuilt_ids[id] = new_id;
        return new_id;
    };

    switch (node.kind) {
        case NodeKind::LeafNfa:
        case NodeKind::LeafNft:
            output.push_back(ExecNode{classify_leaf_kind(node.kind, node.result_arity), node.lhs, node.rhs, node.payload, node.result_arity});
            return finish(static_cast<NodeId>(output.size() - 1));

        case NodeKind::Union:
        case NodeKind::Intersect:
        case NodeKind::SyncProduct: {
            const NodeId lhs_id = reconstruct_nodes(original_nodes, node.lhs, output, rebuilt_ids, active_path);
            const NodeId rhs_id = reconstruct_nodes(original_nodes, node.rhs, output, rebuilt_ids, active_path);

            output.push_back(ExecNode{
                    classify_binary_kind(node.kind, node.result_arity, output[lhs_id].kind, output[rhs_id].kind), lhs_id,
                    rhs_id, node.payload, node.result_arity});
            return finish(static_cast<NodeId>(output.size() - 1));
        }

        case NodeKind::Complement: {
            const Node& child_node = original_nodes[node.lhs];

            switch (child_node.kind) {
                case NodeKind::Union: {
                    const NodeId lhs_id =
                            reconstruct_nodes(original_nodes, child_node.lhs, output, rebuilt_ids, active_path);
                    const NodeId rhs_id =
                            reconstruct_nodes(original_nodes, child_node.rhs, output, rebuilt_ids, active_path);

                    const NodeId complement_lhs_id = static_cast<NodeId>(output.size());
                    const NodeId complement_rhs_id = complement_lhs_id + 1;

                    output.push_back(ExecNode{
                            classify_unary_kind(NodeKind::Complement, node.result_arity, output[lhs_id].kind), lhs_id, 0,
                            NO_PAYLOAD, node.result_arity});
                    output.push_back(ExecNode{
                            classify_unary_kind(NodeKind::Complement, node.result_arity, output[rhs_id].kind), rhs_id, 0,
                            NO_PAYLOAD, node.result_arity});
                    output.push_back(ExecNode{
                            classify_binary_kind(
                                    NodeKind::Intersect, node.result_arity, output[complement_lhs_id].kind,
                                    output[complement_rhs_id].kind),
                            complement_lhs_id, complement_rhs_id, NO_PAYLOAD, node.result_arity});
                    return finish(static_cast<NodeId>(output.size() - 1));
                }

                case NodeKind::Intersect: {
                    const NodeId lhs_id =
                            reconstruct_nodes(original_nodes, child_node.lhs, output, rebuilt_ids, active_path);
                    const NodeId rhs_id =
                            reconstruct_nodes(original_nodes, child_node.rhs, output, rebuilt_ids, active_path);

                    const NodeId complement_lhs_id = static_cast<NodeId>(output.size());
                    const NodeId complement_rhs_id = complement_lhs_id + 1;

                    output.push_back(ExecNode{
                            classify_unary_kind(NodeKind::Complement, node.result_arity, output[lhs_id].kind), lhs_id, 0,
                            NO_PAYLOAD, node.result_arity});
                    output.push_back(ExecNode{
                            classify_unary_kind(NodeKind::Complement, node.result_arity, output[rhs_id].kind), rhs_id, 0,
                            NO_PAYLOAD, node.result_arity});
                    output.push_back(ExecNode{
                            classify_binary_kind(
                                    NodeKind::Union, node.result_arity, output[complement_lhs_id].kind,
                                    output[complement_rhs_id].kind),
                            complement_lhs_id, complement_rhs_id, NO_PAYLOAD, node.result_arity});
                    return finish(static_cast<NodeId>(output.size() - 1));
                }

                case NodeKind::Complement:
                    return finish(reconstruct_nodes(original_nodes, child_node.lhs, output, rebuilt_ids, active_path));

                case NodeKind::LeafNfa:
                case NodeKind::LeafNft:
                case NodeKind::Identity:
                case NodeKind::Project:
                case NodeKind::SyncProduct: {
                    const NodeId child_id = reconstruct_nodes(original_nodes, node.lhs, output, rebuilt_ids, active_path);
                    output.push_back(ExecNode{
                            classify_unary_kind(NodeKind::Complement, node.result_arity, output[child_id].kind), child_id,
                            0, NO_PAYLOAD, node.result_arity});
                    return finish(static_cast<NodeId>(output.size() - 1));
                }
            }

            throw std::logic_error("Unreachable complement reconstruction branch.");
        }

        case NodeKind::Identity:
        case NodeKind::Project: {
            const NodeId child_id = reconstruct_nodes(original_nodes, node.lhs, output, rebuilt_ids, active_path);
            output.push_back(ExecNode{
                    classify_unary_kind(node.kind, node.result_arity, output[child_id].kind), child_id, 0, node.payload,
                    node.result_arity});
            return finish(static_cast<NodeId>(output.size() - 1));
        }
    }

    throw std::logic_error("Unreachable reconstruction branch.");
}

} // namespace

NodeId reconstruct_nodes(const std::vector<Node>& original_nodes, const NodeId id, std::vector<ExecNode>& output) {
    std::vector<std::optional<NodeId>> rebuilt_ids(original_nodes.size(), std::nullopt);
    std::vector<bool> active_path(original_nodes.size(), false);
    return reconstruct_nodes(original_nodes, id, output, rebuilt_ids, active_path);
}

} // namespace mata::nft::lazy::detail
