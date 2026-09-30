#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "map/MapData.h"

namespace mx::map {

// 3D-0/3D-1 contract types. Legacy maps keep an implicit layer id of zero;
// these types are opt-in and do not change the existing heightfield format.
using LayerId = std::uint32_t;
using VolumeId = std::uint32_t;

inline constexpr LayerId kLegacyLayerId = 0;

enum class VolumeKind : std::uint8_t {
    Ground = 0,
    Interior = 1,
    WaterSurface = 2,
    Underwater = 3,
    Connector = 4,
};

const char* ToString(VolumeKind kind) noexcept;

struct LayerVolume {
    VolumeId id = 0;
    LayerId layer_id = kLegacyLayerId;
    std::string name;
    Rect bounds;
    float min_z = 0.0f;
    float max_z = 0.0f;
    VolumeKind kind = VolumeKind::Ground;
    bool supports_ground_movement = true;

    bool Contains(float x, float y, float z) const noexcept;
};

// A portal connects two explicit volume-local footprints. It is the only
// contract-level way to cross layers; merely changing z is not a transition.
struct LayerPortal {
    std::uint32_t id = 0;
    VolumeId source_volume = 0;
    VolumeId target_volume = 0;
    Rect source_bounds;
    Rect target_bounds;
    float source_min_z = 0.0f;
    float source_max_z = 0.0f;
    float target_min_z = 0.0f;
    float target_max_z = 0.0f;
    bool bidirectional = true;

    bool SourceContains(float x, float y, float z) const noexcept;
};

struct LayeredWorld {
    std::vector<LayerVolume> volumes;
    std::vector<LayerPortal> portals;

    // An empty contract is valid and means legacy single-layer behaviour.
    // When volumes are present, all 3D AABBs must be finite, inside the
    // supplied horizontal world bounds, non-degenerate and pairwise disjoint
    // in 3D. XY overlap is therefore allowed for stacked floors when their
    // z intervals do not overlap.
    bool Validate(const Rect& world_bounds, std::string& error) const;

    // Deterministic lookup after Validate(): at most one volume can contain a
    // point. Returns nullopt for legacy/no-volume or an uncovered point.
    std::optional<VolumeId> FindVolume(float x, float y, float z) const noexcept;

    // Checks the explicit source and target footprints of a portal. This does
    // not perform collision or movement; those remain runtime responsibilities.
    bool CanTraverse(const LayerPortal& portal,
                    float source_x,
                    float source_y,
                    float source_z,
                    float target_x,
                    float target_y,
                    float target_z) const noexcept;
};

} // namespace mx::map
