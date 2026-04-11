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

    bool is_subsumed(
            NodeId root_id, MacroStateId state, std::unordered_set<MacroStateId>& visited,
            std::unordered_set<MacroStateId>& queued) const;

private:
    using State = mata::nfa::State;

    const std::vector<mata::nfa::Nfa>& nfas;
    const std::vector<mata::nft::Nft>& nfts;
    const std::vector<ExecNode>& nodes;
    const MacroStateStore& macro_store;
    std::vector<Simlib::Util::BinaryRelation> precomputed_simulations;

    bool subsumed_state(NodeId node_id, MacroStateId state1, MacroStateId state2) const;
};

} // namespace mata::nft::lazy::detail
