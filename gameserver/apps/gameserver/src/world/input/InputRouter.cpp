#include "InputRouter.h"

#include <iterator>
#include <utility>

#include "../components/MovementComponents.h"
#include "../components/NetworkComponents.h"
#include "../components/TransformComponents.h"
#include "../distributed/WorldMessageRouter.h"
#include "../zone/Zone.h"
#include "../zone/ZoneOwnership.h"

namespace gs::game {

InputRouter::InputRouter(WorldMessageRouter& router)
    : router_(router)
{
}

void InputRouter::PostMoveInput(gs::common::SessionId session_id,
                                std::uint32_t sequence,
                                float dir_angle,
                                MoveState state)
{
    const MoveInput input{session_id, sequence, dir_angle, state};
    {
        std::lock_guard lock(input_mutex_);
        const auto [it, inserted] = staged_moves_.try_emplace(session_id, input);
        if (!inserted) {
            // Latest-state: only a newer (or equal, for idempotent resends)
            // sequence replaces the staged input.
            if (sequence >= it->second.sequence) {
                it->second = input;
                moves_coalesced_.fetch_add(1, std::memory_order_relaxed);
            } else {
                moves_stale_at_edge_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    moves_posted_.fetch_add(1, std::memory_order_relaxed);
}

void InputRouter::PostAttackTarget(gs::common::SessionId session_id, std::uint32_t target_net_id)
{
    {
        std::lock_guard lock(input_mutex_);
        auto& staged = staged_attacks_[session_id];
        if (staged.size() >= kMaxStagedAttacksPerSession) {
            attacks_dropped_overflow_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        staged.push_back(AttackInput{session_id, target_net_id});
    }
    attacks_posted_.fetch_add(1, std::memory_order_relaxed);
}

void InputRouter::ForgetSession(gs::common::SessionId session_id)
{
    std::lock_guard lock(input_mutex_);
    if (staged_moves_.erase(session_id) > 0) {
        moves_dropped_no_owner_.fetch_add(1, std::memory_order_relaxed);
    }
    staged_attacks_.erase(session_id);
}

void InputRouter::DeliverToZone(Zone& zone, std::size_t zone_index, const AttackHandler& handle)
{
    const auto& sessions = zone.NetBySession();
    if (sessions.empty()) {
        return;
    }
    std::vector<MoveInput> moves;
    std::vector<AttackInput> attacks;
    {
        std::lock_guard lock(input_mutex_);
        if (staged_moves_.empty() && staged_attacks_.empty()) {
            return;
        }
        // Only the sessions resident in this zone are taken.
        for (const auto& [session_id, net_id] : sessions) {
            (void)net_id;
            if (const auto move = staged_moves_.find(session_id); move != staged_moves_.end()) {
                moves.push_back(move->second);
                staged_moves_.erase(move);
            }
            if (const auto staged = staged_attacks_.find(session_id);
                staged != staged_attacks_.end()) {
                attacks.insert(attacks.end(), staged->second.begin(), staged->second.end());
                staged_attacks_.erase(staged);
            }
        }
    }
    if (moves.empty() && attacks.empty()) {
        return;
    }
    moves_routed_.fetch_add(moves.size(), std::memory_order_relaxed);
    attacks_routed_.fetch_add(attacks.size(), std::memory_order_relaxed);
    // One command per zone per tick; moves before attacks (the order the
    // separate drains used to produce).
    router_.RouteZoneCommand(
        zone_index,
        [this, moves = std::move(moves), attacks = std::move(attacks), handle](Zone& target) {
            AssertZoneOwner(target, "zone input command");
            for (const auto& input : moves) {
                ApplyMove(target, input);
            }
            for (const auto& attack : attacks) {
                handle(target, attack.session_id, attack.target_net_id);
            }
        });
}

void InputRouter::SweepOrphans(const OwnerMap& owners)
{
    std::lock_guard lock(input_mutex_);
    for (auto it = staged_moves_.begin(); it != staged_moves_.end();) {
        if (!owners.contains(it->first)) {
            moves_dropped_no_owner_.fetch_add(1, std::memory_order_relaxed);
            it = staged_moves_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = staged_attacks_.begin(); it != staged_attacks_.end();) {
        it = owners.contains(it->first) ? std::next(it) : staged_attacks_.erase(it);
    }
}

void InputRouter::ApplyMove(Zone& zone, const MoveInput& input)
{
    // O(1) session -> net lookup via the zone's reverse index.
    const auto net_id = zone.NetIdForSession(input.session_id);
    if (!net_id) {
        moves_dropped_not_resident_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto entity = zone.FindEntity(*net_id);
    if (!entity.is_valid()) {
        moves_dropped_not_resident_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto intent = entity.get<MoveIntent>();
    if (input.sequence < intent.last_input_seq) {
        moves_dropped_stale_sequence_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    moves_applied_.fetch_add(1, std::memory_order_relaxed);
    intent.dir_angle = input.dir_angle;
    intent.state = input.state;
    intent.last_input_seq = input.sequence;
    entity.set<MoveIntent>(intent);
    // Phase 5A: move_state is a published border field; an intent change
    // without displacement (e.g. stopping) must still refresh the entity's
    // border snapshot.
    zone.MarkEntityDirty(entity);
}

InputRouter::Stats InputRouter::GetStats() const
{
    Stats stats;
    stats.moves_posted = moves_posted_.load(std::memory_order_relaxed);
    stats.moves_coalesced = moves_coalesced_.load(std::memory_order_relaxed);
    stats.moves_stale_at_edge = moves_stale_at_edge_.load(std::memory_order_relaxed);
    stats.moves_routed = moves_routed_.load(std::memory_order_relaxed);
    stats.moves_applied = moves_applied_.load(std::memory_order_relaxed);
    stats.moves_dropped_no_owner = moves_dropped_no_owner_.load(std::memory_order_relaxed);
    stats.moves_dropped_not_resident = moves_dropped_not_resident_.load(std::memory_order_relaxed);
    stats.moves_dropped_stale_sequence =
        moves_dropped_stale_sequence_.load(std::memory_order_relaxed);
    stats.attacks_posted = attacks_posted_.load(std::memory_order_relaxed);
    stats.attacks_routed = attacks_routed_.load(std::memory_order_relaxed);
    stats.attacks_dropped_overflow = attacks_dropped_overflow_.load(std::memory_order_relaxed);
    {
        std::lock_guard lock(input_mutex_);
        stats.staged_sessions = staged_moves_.size();
    }
    return stats;
}

} // namespace gs::game
