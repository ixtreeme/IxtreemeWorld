#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "common/Types.h"

#include "../components/MovementComponents.h"
#include "../distributed/Routing.h"
#include "../OwnerMap.h"

// Client-input fan-out: move + attack intents arrive on network threads and
// are routed to the owning zone as commands. This is pure message passing:
// the network side never touches a flecs world, and command payloads carry
// only stable ids (session, sequence, net_id) -- never flecs::entity handles.
// Delivery goes through WorldMessageRouter, so the same call sites work for
// local, emulated-remote and (future) remote destinations.
// Single writer per queue is NOT assumed; each queue has its own mutex.
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

    InputRouter(WorldMessageRouter& router, WakeFn wake);

    void PostMoveInput(gs::common::SessionId session_id,
                       std::uint32_t sequence,
                       float dir_angle,
                       MoveState state);
    void PostAttackTarget(gs::common::SessionId session_id, std::uint32_t target_net_id);

    // Supervisor side: fan queued inputs out to owning zones.
    void DrainMoves(OwnerMap& owners);
    void DrainAttacks(OwnerMap& owners, const AttackHandler& handle);

private:
    void PostToOwner(OwnerMap& owners,
                      gs::common::SessionId session_id,
                      std::function<void(Zone&)> command);

    WorldMessageRouter& router_;
    WakeFn wake_;

    std::mutex input_mutex_;
    std::vector<MoveInput> pending_inputs_;
    std::mutex attack_mutex_;
    std::vector<AttackInput> pending_attacks_;
};

} // namespace gs::game
