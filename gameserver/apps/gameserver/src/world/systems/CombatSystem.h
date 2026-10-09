#pragma once

#include <cstdint>

#include "common/Types.h"

// Melee combat over authoritative flecs state. PvP is rejected, ghosts can
// never be hit, damage is max(1, dmg - defense). Deaths schedule a respawn
// through the tick context and clean up ghosts, bindings and RNG drivers.
// Returns what happened so the caller can maintain global counters.
namespace gs::game {

class Zone;
struct ZoneTickContext;

class CombatSystem {
public:
    struct AttackResult {
        bool attacked = false;
        bool killed = false;
    };

    static AttackResult ProcessAttack(Zone& zone,
                                      gs::common::SessionId attacker_session_id,
                                      std::uint32_t target_net_id,
                                      ZoneTickContext& ctx);
};

} // namespace gs::game
