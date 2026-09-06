#include "InputRouter.h"

#include <utility>

#include "../components/MovementComponents.h"
#include "../components/NetworkComponents.h"
#include "../components/TransformComponents.h"
#include "../distributed/WorldMessageRouter.h"
#include "../zone/Zone.h"
#include "../zone/ZoneOwnership.h"

namespace gs::game {

InputRouter::InputRouter(WorldMessageRouter& router, WakeFn wake)
    : router_(router)
    , wake_(std::move(wake))
{
}

void InputRouter::PostMoveInput(gs::common::SessionId session_id,
                                std::uint32_t sequence,
                                float dir_angle,
                                MoveState state)
{
    {
        std::lock_guard lock(input_mutex_);
        pending_inputs_.push_back(MoveInput{session_id, sequence, dir_angle, state});
    }
    wake_();
}

void InputRouter::PostAttackTarget(gs::common::SessionId session_id, std::uint32_t target_net_id)
{
    {
        std::lock_guard lock(attack_mutex_);
        pending_attacks_.push_back(AttackInput{session_id, target_net_id});
    }
    wake_();
}

void InputRouter::PostToOwner(OwnerMap& owners,
                              gs::common::SessionId session_id,
                              std::function<void(Zone&)> command)
{
    const auto owner_it = owners.find(session_id);
    if (owner_it == owners.end()) {
        return;
    }
    // Routed by stable zone index through the message router: local today,
    // location-aware tomorrow. The scheduler wake stays unconditional --
    // even an unavailable route must not stall the supervisor loop.
    router_.RouteZoneCommand(owner_it->second.zone_index, std::move(command));
    wake_();
}

void InputRouter::DrainMoves(OwnerMap& owners)
{
    std::vector<MoveInput> inputs;
    {
        std::lock_guard lock(input_mutex_);
        inputs.swap(pending_inputs_);
    }

    for (const auto& input : inputs) {
        PostToOwner(owners, input.session_id, [input](Zone& zone) {
            AssertZoneOwner(zone, "zone input command");
            // O(1) session -> net lookup via the zone's reverse index
            // (previously a linear scan per input).
            const auto net_id = zone.NetIdForSession(input.session_id);
            if (!net_id) {
                return;
            }
            const auto entity = zone.FindEntity(*net_id);
            if (!entity.is_valid()) {
                return;
            }
            auto intent = entity.get<MoveIntent>();
            if (input.sequence < intent.last_input_seq) {
                return;
            }
            intent.dir_angle = input.dir_angle;
            intent.state = input.state;
            intent.last_input_seq = input.sequence;
            entity.set<MoveIntent>(intent);
        });
    }
}

void InputRouter::DrainAttacks(OwnerMap& owners, const AttackHandler& handle)
{
    std::vector<AttackInput> inputs;
    {
        std::lock_guard lock(attack_mutex_);
        inputs.swap(pending_attacks_);
    }

    for (const auto& input : inputs) {
        PostToOwner(owners, input.session_id, [handle, input](Zone& zone) {
            AssertZoneOwner(zone, "zone attack command");
            handle(zone, input.session_id, input.target_net_id);
        });
    }
}

} // namespace gs::game
