#pragma once

// Movement integration over authoritative flecs state: intent -> velocity ->
// terrain-clamped position, plus warp application and migration marking.
// Reads terrain through TerrainService only; never touches the world runtime.
namespace gs::game {

class Zone;
struct ZoneTickContext;

class MovementSystem {
public:
    static void Step(Zone& zone, float dt, ZoneTickContext& ctx);
};

} // namespace gs::game
