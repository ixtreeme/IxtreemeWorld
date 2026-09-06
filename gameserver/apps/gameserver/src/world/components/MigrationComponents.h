#pragma once

#include <cstdint>

// Cross-zone handoff marker. Set by the owning zone's movement step when an
// entity has left the zone bounds; consumed by the supervisor, which performs
// the actual ownership transfer. Never written cross-zone directly.
namespace gs::game {

struct MigrateTo {
    std::uint32_t target_zone = 0;
};

} // namespace gs::game
