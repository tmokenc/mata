/**
 * @file subsumption.hh
 * @brief Private subsumption and antichain bucketing declarations for mata::nft::lazy::detail.
 */

#pragma once

#include "macrostate_store.hh"

#include <mata/simlib/explicit_lts.hh>

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mata::nft::lazy::detail {

struct SubsumptionContext {
    const std::vector<mata::nfa::Nfa>& nfas;
    const std::vector<mata::nft::Nft>& nfts;
    const std::vector<ExecNode>& nodes;
    const MacroStateStore& macro_store;
};

class SubsumptionEngine {
public:
    explicit SubsumptionEngine(const SubsumptionContext& context);

    void set_nfa_simulation(size_t nfa_index, Simlib::Util::BinaryRelation relation);
    void set_nft_simulation(size_t nft_index, Simlib::Util::BinaryRelation relation);

    AntichainBucketKey root_bucket_key(NodeId root_id, MacroStateId state);

    bool is_subsumed(
            NodeId root_id, MacroStateId state, AntichainBucketKey bucket_key,
            std::unordered_set<MacroStateId>& visited, std::unordered_set<MacroStateId>& queued,
            std::unordered_map<AntichainBucketKey, std::vector<MacroStateId>>& visited_buckets,
            std::unordered_map<AntichainBucketKey, std::vector<MacroStateId>>& queued_buckets) const;

private:
    using State = mata::nfa::State;

    const std::vector<mata::nfa::Nfa>& nfas;
    const std::vector<mata::nft::Nft>& nfts;
    const std::vector<ExecNode>& nodes;
    const MacroStateStore& macro_store;
    std::vector<Simlib::Util::BinaryRelation> precomputed_simulations;

    std::unordered_map<uint64_t, AntichainBucketKey> bucket_key_cache;

    static constexpr uint64_t node_state_key(NodeId node_id, MacroStateId state) noexcept {
        return (static_cast<uint64_t>(node_id) << 32) | static_cast<uint64_t>(state);
    }

    bool subsumed_state(NodeId node_id, MacroStateId state1, MacroStateId state2) const;
    AntichainBucketKey bucket_key(NodeId node_id, MacroStateId state);
};

} // namespace mata::nft::lazy::detail
