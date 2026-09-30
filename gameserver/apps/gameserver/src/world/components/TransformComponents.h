#pragma once

// Authoritative transform state. Lives ONLY on the flecs entity.
// (Moved verbatim out of SimWorld.h; no mirror struct may duplicate these.)
namespace gs::game {

struct Position {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Heading {
    float angle = 0.0f;
};

struct Velocity {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

} // namespace gs::game
