#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

// Single home for world-runtime tuning constants. Behavior parity is a
// refactor requirement, so these intentionally keep the exact SimWorld values.
namespace gs::game {

inline constexpr float kDbUnitsPerMeter = 1000.0f;

inline constexpr float kAoiRadiusMeters = 120.0f;
inline constexpr float kAoiRadiusSqMeters = kAoiRadiusMeters * kAoiRadiusMeters;
inline constexpr float kSpatialCellSizeMeters = kAoiRadiusMeters;
inline constexpr std::size_t kAoiEntityCap = 100;

inline constexpr float kMigrationHysteresisMeters = 5.0f;

inline constexpr auto kTickDt = std::chrono::milliseconds(50);
inline constexpr float kTickDtSeconds = 0.05f;

inline constexpr std::uint32_t kFirstMobNetId = 1'000'000;

inline constexpr float kTwoPi = 6.28318530717958647692f;
inline constexpr float kWanderArrivalDistanceMeters = 0.5f;

// Default player runtime stats (previously hardcoded in SimWorld::Spawn).
inline constexpr float kPlayerWalkSpeed = 3.0f;
inline constexpr float kPlayerRunSpeed = 6.0f;
inline constexpr float kPlayerHpMax = 100.0f;
inline constexpr float kPlayerDamage = 10.0f;
inline constexpr float kPlayerDefense = 0.0f;
inline constexpr float kPlayerAttackRange = 2.0f;
inline constexpr float kPlayerAttackCooldown = 1.0f;

} // namespace gs::game
