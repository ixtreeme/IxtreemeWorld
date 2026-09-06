#pragma once

#include <cstdint>

// Authoritative wander AI state. Lives ONLY on the flecs entity.
// NOTE: the per-mob RNG (std::mt19937) intentionally stays OUTSIDE the ECS in
// Zone's mob-rng map: it is a simulation driver, not gameplay state, and must
// not be snapshotted or replicated.
namespace gs::game {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct WanderState {
    enum class Mode : std::uint8_t {
        Idle = 0,
        Moving = 1,
    };

    Mode mode = Mode::Idle;
    Vec2 target;
    float timer = 0.0f;
    Vec2 spawn_center;
    float spawn_radius = 0.0f;
};

} // namespace gs::game
