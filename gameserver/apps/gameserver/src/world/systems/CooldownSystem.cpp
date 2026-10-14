#include "CooldownSystem.h"

#include <algorithm>

#include "../components/CombatComponents.h"
#include "../zone/Zone.h"
#include "../zone/ZoneOwnership.h"

namespace gs::game {

void CooldownSystem::Step(Zone& zone, float dt)
{
    AssertZoneOwner(zone, "zone combat cooldowns");

    zone.World().query<AttackCooldown>().each([dt](AttackCooldown& cooldown) {
        cooldown.remaining = std::max(0.0f, cooldown.remaining - dt);
    });
}

} // namespace gs::game
