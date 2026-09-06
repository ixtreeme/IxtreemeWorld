#pragma once

// Mob wander AI over authoritative flecs state. Behavior unchanged:
// idle countdown -> pick target in spawn circle -> walk -> arrival -> idle.
namespace gs::game {

class Zone;
class MobPrototypeRegistry;

class AiSystem {
public:
    static void StepWander(Zone& zone, float dt, MobPrototypeRegistry& mob_types);
};

} // namespace gs::game
