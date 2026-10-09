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
// Version 4 adds the cooked clearance profile, per-volume clearance grids and
// portal transition proofs (3D-4B). A world without any of them is still
// written as version 3, byte-identical to the previous encoder.
inline constexpr std::uint32_t kLayeredWorldFileVersion = 4;
// Version 5 (3D-5B2) appends proven terrain edges after the portals. It is
// written only when a world has terrain edges; otherwise v4 / v3 as before.
inline constexpr std::uint32_t kLayeredWorldTerrainEdgeFileVersion = 5;
inline constexpr std::uint32_t kLayeredWorldSupportFileVersion = 3;
inline constexpr std::uint32_t kLayeredWorldFileMinVersion = 1;
inline constexpr std::uint32_t kMaxLayeredWorldVolumes = 4096;
inline constexpr std::uint32_t kMaxLayeredWorldPortals = 8192;
inline constexpr std::uint32_t kMaxLayerTerrainEdges = 16384;
inline constexpr std::uint32_t kMaxLayeredWorldNameBytes = 128;
inline constexpr std::uint64_t kMaxLayeredWorldFileBytes = 4ull << 20;
// The collision cooker's existing perpendicular coplanarity bound. A stored
// vertical residual may not exceed this divided by the plane's up-normal.
inline constexpr double kLayerSupportCoplanarToleranceMeters = 0.00001;
// 3D-4B clearance bounds: one volume grid, and all grids together (bits).
inline constexpr std::uint64_t kMaxLayerClearanceCellsPerVolume = 1ull << 22;
inline constexpr std::uint64_t kMaxLayerClearanceTotalCells = 1ull << 24;
// Tolerance for recomputing a stored portal step from the two support planes.
inline constexpr double kLayerPortalStepToleranceMeters = 1e-9;

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

// 3D-4B: the actor class a clearance bake proves free space for, and the
// grid resolution. An upright capsule of total height `actor_height_m` and
// radius `actor_radius_m` stands on a volume's support plane. Collision that
// lies no higher than `floor_contact_m` above the plane is floor contact, not
// an obstruction. Two support surfaces whose heights differ by at most
// `step_height_m` along a shared edge can be proven as a step/ramp portal.
struct LayerClearanceProfile {
    float cell_size_m = 0.25f;
    float actor_radius_m = 0.35f;
    float actor_height_m = 1.8f;
    float step_height_m = 0.35f;
    float floor_contact_m = 0.02f;

    bool Valid() const noexcept;
};

// A cooked, conservative clearance grid over one volume's footprint, cells
// row-major (y outer) from the footprint minimum; the last row/column may be
// partial. A clear cell proves: for EVERY capsule centre inside the cell, the
// profile capsule standing on the volume's support plane intersects no static
// collision triangle (all static colliders and terrain) above floor contact.
// A blocked cell proves nothing (the test is conservative).
struct LayerClearanceGrid {
    std::uint32_t cells_x = 0;
    std::uint32_t cells_y = 0;
    std::vector<std::uint8_t> blocked; // bitset, bit (y * cells_x + x), 1 = blocked

    bool Blocked(std::uint32_t x, std::uint32_t y) const noexcept;
    std::uint64_t BlockedCount() const noexcept;
};

// Proof that two volumes are physically connected: their rectangular support
// footprints touch exactly along an axis-aligned edge, the two support planes
// differ by at most the profile step height along the whole shared segment,
// and a corridor grid (`slots` cells along the segment x `across` cells
// across it; the across band is radius + cell size on both sides of the
// edge, limited to the two footprints) records which capsule-centre cells
// are free above the UPPER ENVELOPE of both planes. Below that envelope only
// the step body (at most `max_step_m` high) can lie: this is the step zone in
// which an upright character controller steps between the two planes.
struct LayerPortalProof {
    std::uint8_t axis = 0; // 0: edge is x = edge (crossing along x); 1: y = edge
    float edge = 0.0f;
    float span_min = 0.0f; // shared segment along the other axis
    float span_max = 0.0f;
    double max_step_m = 0.0; // measured |source - target| plane height over the segment
    std::uint32_t slots = 0;  // cells along the segment
    std::uint32_t across = 0; // cells across the corridor band
    std::vector<std::uint8_t> blocked; // bitset, bit (slot * across + a), 1 = blocked

    bool CellBlocked(std::uint32_t slot, std::uint32_t across_cell) const noexcept;
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
    // 3D-4B; requires a valid ground support and the world's clearance profile.
    std::optional<LayerClearanceGrid> clearance;

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
    // 3D-4B. An authored portal without a proof stays metadata: movement
    // across it still answers TransitionRequired.
    std::optional<LayerPortalProof> proof;

    bool SourceContains(float x, float y, float z) const noexcept;
};

// 3D-5B2: a proven edge between a walkable volume and the terrain around it.
// Along [span_min, span_max] of the volume footprint boundary `edge` (axis 0:
// x = edge, crossing along x; 1: y = edge) with the terrain outside on
// `terrain_side` (0: the edge is the footprint minimum, terrain below it; 1:
// the footprint maximum, terrain above it), the cooked terrain stays within
// the profile step of the support plane (conservatively over every terrain
// quad the edge segment touches; the bound is `max_step_m`). A volume-side
// corridor (radius + cell inward, limited to the footprint) records which
// capsule-centre cells are free above the upper envelope of the plane and
// the local terrain. The record proves the volume side only: the runtime
// still checks the exact terrain height and terrain rules where it crosses.
struct LayerTerrainEdge {
    std::uint32_t id = 0;
    VolumeId volume_id = 0;
    std::uint8_t axis = 0;
    std::uint8_t terrain_side = 0;
    float edge = 0.0f;
    float span_min = 0.0f;
    float span_max = 0.0f;
    double max_step_m = 0.0;
    std::uint32_t slots = 0;  // cells along the segment
    std::uint32_t across = 0; // cells across the volume-side band
    std::vector<std::uint8_t> blocked; // bitset, bit (slot * across + a), 1 = blocked

    bool CellBlocked(std::uint32_t slot, std::uint32_t across_cell) const noexcept;
};

// Cells of a clearance grid / slots of a portal corridor for a length, the
// same double computation for the cooker, the reader and the queries.
std::uint32_t LayerClearanceCellCount(double min, double max, float cell_size) noexcept;

// Across-edge band [across0, across1] of a proven portal's corridor: radius +
// cell size on both sides of the shared edge, limited to the two volumes'
// footprints. Cooker, reader and movement query use this one definition.
void LayerPortalCorridorAcross(const LayerPortalProof& proof,
                               const LayerVolume& a,
                               const LayerVolume& b,
                               const LayerClearanceProfile& profile,
                               double& across0,
                               double& across1) noexcept;

// Volume-side band [across0, across1] of a terrain edge corridor: radius +
// cell size inward from the edge, limited to the volume footprint.
void LayerTerrainEdgeCorridorAcross(const LayerTerrainEdge& edge,
                                    const LayerVolume& volume,
                                    const LayerClearanceProfile& profile,
                                    double& across0,
                                    double& across1) noexcept;

struct LayeredWorld {
    std::vector<LayerVolume> volumes;
    std::vector<LayerPortal> portals;
    // 3D-5B2 proven volume-to-terrain edges (require the clearance profile).
    std::vector<LayerTerrainEdge> terrain_edges;
    // Present exactly when any clearance grid, portal proof or terrain edge is.
    std::optional<LayerClearanceProfile> clearance_profile;

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
