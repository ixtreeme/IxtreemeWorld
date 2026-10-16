#pragma once

#include <cstdint>

// Content area of a player: the most specific area it stands in, as
// AreaSystem resolved it after the tick's movement. Authoritative, lives on
// the flecs entity and travels in transfers, so a zone border crossed inside
// one area raises no transition. The resolved point lets the validator tell
// a stale area apart from a position the zone has not ticked since.
namespace gs::game {

struct AreaPresence {
    std::uint32_t area = 0; // mx::map::AreaId; 0 = no area
    bool known = false;     // false until the first resolution
    float x = 0.0f;         // where `area` was resolved
    float y = 0.0f;
    std::uint32_t volume_id = 0;
};

} // namespace gs::game
