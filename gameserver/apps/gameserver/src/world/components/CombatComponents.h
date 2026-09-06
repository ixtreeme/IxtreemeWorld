#pragma once

// Authoritative combat state. Lives ONLY on the flecs entity.
namespace gs::game {

struct Hp {
    float current = 1.0f;
    float max = 1.0f;
};

struct CombatStats {
    float damage = 1.0f;
    float defense = 0.0f;
    float attack_range = 2.0f;
    float attack_cooldown = 1.0f;
};

struct AttackCooldown {
    float remaining = 0.0f;
};

} // namespace gs::game
