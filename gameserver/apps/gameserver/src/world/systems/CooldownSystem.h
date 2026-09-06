#pragma once

// Attack-cooldown countdown over authoritative flecs state.
namespace gs::game {

class Zone;

class CooldownSystem {
public:
    static void Step(Zone& zone, float dt);
};

} // namespace gs::game
