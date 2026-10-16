#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "map/MapData.h"

// Nested content areas (areaDefs + areaIds layers, docs/map-data-format.md
// section 15). An area is a content region -- the successor of a Metin-style
// map -- and areas nest: a region holds a town, the town an arena. The terrain
// raster names the most specific area of every cell; that cell belongs to the
// area and to each of its ancestors. Rules left at their inherit value take
// the parent's effective value, and a top-level area inherits the world
// defaults below.
namespace mx::map {

inline constexpr AreaId kMaxAreaId = 65535;     // the raster stores uint16 ids
inline constexpr std::size_t kMaxAreaDepth = 16; // levels, a top-level area is 1
inline constexpr std::size_t kMaxAreaTextBytes = 128;

enum class PvpMode : std::uint8_t {
    Inherit = 0,
    Peaceful = 1, // no player may attack another
    DuelOnly = 2, // only agreed duels
    Free = 3,     // open player combat
};
const char* ToString(PvpMode mode) noexcept;

// AreaRules::forbid / allow bits. The server enforces each one when the
// system it restricts exists; until then the bit is carried and reported.
namespace area_flag {
inline constexpr std::uint32_t kNoMount = 1u << 0;
inline constexpr std::uint32_t kNoPet = 1u << 1;
inline constexpr std::uint32_t kNoTeleportOut = 1u << 2;
inline constexpr std::uint32_t kNoPartyInvite = 1u << 3;
inline constexpr std::uint32_t kKnown = kNoMount | kNoPet | kNoTeleportOut | kNoPartyInvite;
} // namespace area_flag

struct AreaPoint {
    float x = 0.0f;
    float y = 0.0f;
    std::uint32_t volume_id = 0; // 0 = on the terrain
};

// As authored: every field may defer to the parent.
struct AreaRules {
    PvpMode pvp = PvpMode::Inherit;
    std::uint16_t level_min = 0; // both 0 = inherit, else 1 <= min <= max
    std::uint16_t level_max = 0;
    std::uint32_t forbid = 0;    // forbidden here and below
    std::uint32_t allow = 0;     // allowed again (cancels an inherited forbid)
    std::optional<AreaPoint> respawn;
    std::uint32_t music_id = 0;  // 0 = inherit
    std::uint32_t ambient_id = 0;
};

struct AreaDef {
    AreaId id = 0;        // 1 .. kMaxAreaId
    AreaId parent_id = 0; // 0 = top level
    std::string name;     // authoring / diagnostic name
    std::string name_key; // localization key shown to players
    AreaRules rules;
    std::vector<std::uint32_t> volumes; // layered volumes that belong to this area
};

// After inheritance: every field is concrete. The defaults are the world's.
struct EffectiveAreaRules {
    PvpMode pvp = PvpMode::Peaceful;
    std::uint16_t level_min = 0; // 0 / 0 = no recommended range
    std::uint16_t level_max = 0;
    std::uint32_t forbidden = 0;
    std::optional<AreaPoint> respawn;
    std::uint32_t music_id = 0;
    std::uint32_t ambient_id = 0;
};

class AreaTable {
public:
    AreaTable() = default;
    // `defs` must be valid (the package loader checks it): ids in
    // 1..kMaxAreaId and unique, every parent exists, no cycle, depth at most
    // kMaxAreaDepth, every volume in at most one area.
    explicit AreaTable(std::vector<AreaDef> defs);

    bool Empty() const noexcept
    {
        return defs_.empty();
    }
    const std::vector<AreaDef>& Defs() const noexcept
    {
        return defs_;
    }
    const AreaDef* Find(AreaId id) const noexcept;
    bool Contains(AreaId id) const noexcept
    {
        return Find(id) != nullptr;
    }
    // Rules of `id` after inheritance; 0 or an unknown id answers the world
    // defaults.
    const EffectiveAreaRules& Rules(AreaId id) const noexcept;
    static const EffectiveAreaRules& WorldDefaults() noexcept;
    AreaId Parent(AreaId id) const noexcept;
    // 1 for a top-level area, 0 for 0 or an unknown id.
    std::size_t Depth(AreaId id) const noexcept;
    // `id` is `ancestor` or lies below it. Nothing lies within area 0.
    bool IsWithin(AreaId id, AreaId ancestor) const noexcept;
    // Deepest area both lie within (0 = none in common).
    AreaId CommonAncestor(AreaId a, AreaId b) const noexcept;
    // The area a whole layered volume belongs to (0 = none).
    AreaId VolumeArea(std::uint32_t volume_id) const noexcept;

private:
    std::vector<AreaDef> defs_;
    std::vector<std::uint16_t> slot_;            // id -> defs_ index + 1 (0 = absent)
    std::vector<EffectiveAreaRules> effective_; // parallel to defs_
    std::vector<std::uint8_t> depth_;           // parallel to defs_
    std::unordered_map<std::uint32_t, AreaId> volume_area_;
};

} // namespace mx::map
