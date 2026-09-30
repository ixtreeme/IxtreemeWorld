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
inline constexpr std::uint32_t kLayeredWorldFileMagic = 0x4433584d; // "MX3D" little-endian
inline constexpr std::uint32_t kLayeredWorldFileVersion = 3;
inline constexpr std::uint32_t kLayeredWorldFileMinVersion = 1;
inline constexpr std::uint32_t kMaxLayeredWorldVolumes = 4096;
inline constexpr std::uint32_t kMaxLayeredWorldPortals = 8192;
inline constexpr std::uint32_t kMaxLayeredWorldNameBytes = 128;
inline constexpr std::uint64_t kMaxLayeredWorldFileBytes = 4ull << 20;
// The collision cooker's existing perpendicular coplanarity bound. A stored
// vertical residual may not exceed this divided by the plane's up-normal.
inline constexpr double kLayerSupportCoplanarToleranceMeters = 0.00001;

enum class VolumeKind : std::uint8_t {
    Ground = 0,
    Interior = 1,
    WaterSurface = 2,
    Underwater = 3,
    Connector = 4,
};

// Semantic authoring tags. VolumeKind describes the broad structural type;
// tags carry the information used by automatic layer generation and later
// collision/AOI policy. Tags are a bitmask so one volume can be, for example,
// both Building and Interior, or Bridge and Connector.
enum VolumeTag : std::uint32_t {
    VolumeTagNone = 0,
    VolumeTagGround = 1u << 0,
    VolumeTagBuilding = 1u << 1,
    VolumeTagBridge = 1u << 2,
    VolumeTagWater = 1u << 3,
    VolumeTagUnderwater = 1u << 4,
    VolumeTagDungeon = 1u << 5,
    VolumeTagInterior = 1u << 6,
    VolumeTagConnector = 1u << 7,
    VolumeTagRoad = 1u << 8,
    VolumeTagStairs = 1u << 9,
    VolumeTagLift = 1u << 10,
    VolumeTagDock = 1u << 11,
};

inline constexpr std::uint32_t kKnownVolumeTags = VolumeTagGround | VolumeTagBuilding | VolumeTagBridge |
                                                   VolumeTagWater | VolumeTagUnderwater | VolumeTagDungeon |
                                                   VolumeTagInterior | VolumeTagConnector | VolumeTagRoad |
                                                   VolumeTagStairs | VolumeTagLift | VolumeTagDock;

inline constexpr bool HasVolumeTag(std::uint32_t tags, VolumeTag tag) noexcept
{
    return (tags & static_cast<std::uint32_t>(tag)) != 0;
}

const char* ToString(VolumeKind kind) noexcept;

// A proven rectangular collision surface, not a solid collider or a
// clearance guarantee. Coordinates and slopes use canonical server metres.
// Source/component identify the deterministic collision bake, not a live
// entity. Legacy v1/v2 records have no support plane.
struct LayerSupportPlane {
    std::uint32_t source_id = 0;
    std::uint32_t component_id = 0;
    double anchor_x = 0.0;
    double anchor_y = 0.0;
    double anchor_z = 0.0;
    double slope_x = 0.0;
    double slope_y = 0.0;
    double max_height_error_m = 0.0;

    double Height(double x, double y) const noexcept;
};

struct LayerVolume {
    VolumeId id = 0;
    LayerId layer_id = kLegacyLayerId;
    std::string name;
    Rect bounds;
    float min_z = 0.0f;
    float max_z = 0.0f;
    VolumeKind kind = VolumeKind::Ground;
    bool supports_ground_movement = true;
    std::uint32_t tags = VolumeTagNone;
    std::optional<LayerSupportPlane> ground_support;

    bool Contains(float x, float y, float z) const noexcept;
    bool AllowsGroundMovement() const noexcept;
    // Includes finite fields, nonzero bake identifiers, the cooker-derived
    // residual bound and corner heights +/- deviation in the half-open band.
    bool HasValidGroundSupport() const noexcept;
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

// Versioned package sidecar (`layered_world.mx3d`). Decode checks the binary
// envelope and bounded records; callers must still call Validate() against
// the package's horizontal world bounds before using the result.
std::vector<std::uint8_t> EncodeLayeredWorld(const LayeredWorld& world);
bool DecodeLayeredWorld(const std::vector<std::uint8_t>& bytes,
                        LayeredWorld& world,
                        std::string& error);

} // namespace mx::map
