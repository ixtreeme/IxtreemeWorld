#pragma once

#include <cstdint>

// Authoritative movement state. Lives ONLY on the flecs entity.
namespace gs::game {

enum class MoveState : std::uint8_t {
    Idle = 0,
    Walking = 1,
    Running = 2,
};

struct MoveIntent {
    float dir_angle = 0.0f;
    MoveState state = MoveState::Idle;
    std::uint32_t last_input_seq = 0;
};

struct MoveSpeed {
    float walk = 3.0f;
    float run = 6.0f;
};

} // namespace gs::game
