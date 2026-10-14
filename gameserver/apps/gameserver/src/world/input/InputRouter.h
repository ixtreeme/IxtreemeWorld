#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "common/Types.h"

#include "../components/MovementComponents.h"
#include "../OwnerMap.h"

// Client-input staging + tick-aligned delivery. Move and attack intents arrive
// on network threads; the network side never touches a flecs world, and
// command payloads carry only stable ids (session, sequence, net_id) -- never
// flecs::entity handles. Delivery goes through WorldMessageRouter, so the same
// call sites work for local, emulated-remote and (future) remote destinations.
//
// Hardening H1 (authoritative 20 Hz): inputs are NOT routed to a zone when
// they arrive. They are staged per session and delivered only to a zone that
// has just been claimed for its next tick, as ONE command applied at the top
// of that tick. Consequences:
//   * input rate never drives the tick rate (the zone ticks on its cadence);
//   * nothing input-related is parked in a zone queue between ticks, so the
//     split/merge/retire "no pending commands" gates are unaffected;
//   * an input always reaches the zone that owns the session when the tick
//     runs -- a migration/transfer between arrival and delivery cannot strand
//     it in the old zone (previously such inputs were silently lost);
//   * moves are latest-state: a newer sequence replaces the staged one, so
//     memory and zone work are bounded by one entry per session regardless of
//     the client's send rate (a per-rendered-frame client or an input flood
//     costs one map upsert per packet on the network thread, nothing more).
// Attacks are events: staged in order per session, bounded per delivery.
// Concrete class, no interface (single implementation).
namespace gs::game {

class Zone;
class WorldMessageRouter;

struct MoveInput {
    gs::common::SessionId session_id = 0;
    std::uint32_t sequence = 0;
    float dir_angle = 0.0f;
    MoveState state = MoveState::Idle;
};

struct AttackInput {
    gs::common::SessionId session_id = 0;
    std::uint32_t target_net_id = 0;
};

class InputRouter {
public:
    using AttackHandler =
        std::function<void(Zone&, gs::common::SessionId attacker, std::uint32_t target_net_id)>;

    // Staged attack events per session between two deliveries (one tick).
    // Excess is dropped and counted: a memory bound on the staging, not a
    // gameplay rule. It must stay well above anything the network edge admits
    // in one tick (the connection policy's attack burst), so it never shapes
    // legitimate traffic -- rate policy lives at the edge, not here.
    static constexpr std::size_t kMaxStagedAttacksPerSession = 64;

    explicit InputRouter(WorldMessageRouter& router);

    // Network side (any thread).
    void PostMoveInput(gs::common::SessionId session_id,
                       std::uint32_t sequence,
                       float dir_angle,
                       MoveState state);
    void PostAttackTarget(gs::common::SessionId session_id, std::uint32_t target_net_id);
    // Drops everything staged for a session (disconnect/despawn).
    void ForgetSession(gs::common::SessionId session_id);

    // Supervisor side, tick-aligned. Precondition: `zone` is claimed for its
    // next tick (TickInProgress set) and not yet handed to a worker, so its
    // session index is stable. Takes the staged input of exactly the sessions
    // resident in the zone and posts one command applying them.
    void DeliverToZone(Zone& zone, std::size_t zone_index, const AttackHandler& handle);

    // Supervisor side (~1 Hz): drops staged input of sessions that own no
    // entity anywhere (never spawned, already gone). Memory backstop only.
    void SweepOrphans(const OwnerMap& owners);

    // Cumulative input-path counters (never reset). A move is "not resident"
    // when the zone executing it no longer owns the session: with tick-aligned
    // delivery this must stay 0.
    struct Stats {
        std::uint64_t moves_posted = 0;
        std::uint64_t moves_coalesced = 0;      // replaced by a newer staged input
        std::uint64_t moves_stale_at_edge = 0;  // arrived with an older sequence
        std::uint64_t moves_routed = 0;         // delivered into a zone command
        std::uint64_t moves_applied = 0;
        std::uint64_t moves_dropped_no_owner = 0; // swept orphans / forgotten sessions
        std::uint64_t moves_dropped_not_resident = 0;
        std::uint64_t moves_dropped_stale_sequence = 0;
        std::uint64_t moves_dropped_invalid = 0; // non-finite heading (H8), never staged
        std::uint64_t attacks_posted = 0;
        std::uint64_t attacks_routed = 0;
        std::uint64_t attacks_dropped_overflow = 0;
        std::uint64_t staged_sessions = 0; // gauge: sessions with staged input
    };
    Stats GetStats() const;

private:
    void ApplyMove(Zone& zone, const MoveInput& input);

    WorldMessageRouter& router_;

    mutable std::mutex input_mutex_;
    std::unordered_map<gs::common::SessionId, MoveInput> staged_moves_;
    std::unordered_map<gs::common::SessionId, std::vector<AttackInput>> staged_attacks_;

    std::atomic<std::uint64_t> moves_posted_{0};
    std::atomic<std::uint64_t> moves_coalesced_{0};
    std::atomic<std::uint64_t> moves_stale_at_edge_{0};
    std::atomic<std::uint64_t> moves_routed_{0};
    std::atomic<std::uint64_t> moves_applied_{0};
    std::atomic<std::uint64_t> moves_dropped_no_owner_{0};
    std::atomic<std::uint64_t> moves_dropped_not_resident_{0};
    std::atomic<std::uint64_t> moves_dropped_stale_sequence_{0};
    std::atomic<std::uint64_t> moves_dropped_invalid_{0};
    std::atomic<std::uint64_t> attacks_posted_{0};
    std::atomic<std::uint64_t> attacks_routed_{0};
    std::atomic<std::uint64_t> attacks_dropped_overflow_{0};
};

} // namespace gs::game
