#pragma once

// Mob wander AI over authoritative flecs state. Behavior unchanged:
// idle countdown -> pick target in spawn circle -> walk -> arrival -> idle.
//
// Simulation LOD: decisions run at the entity's tier rate with dt scaled by
// the tier period (exact timers, no teleports); classification counters run
// every tick so diagnostics stay exact. Null/disabled config = legacy
// every-tick behavior.
namespace gs::game {

class Zone;
class MobPrototypeRegistry;
struct LodConfig;

class AiSystem {
public:
    static void StepWander(Zone& zone,
                           float dt,
                           MobPrototypeRegistry& mob_types,
                           const LodConfig* lod_config);
};

} // namespace gs::game
