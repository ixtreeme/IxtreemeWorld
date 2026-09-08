#pragma once

#include <cfloat>
#include <cstddef>
#include <cstdint>

#include "../components/SimulationLod.h"

// Spatial Activity Field: shared vocabulary.
//
// The field answers ONE question in world space: "how much compute
// relevance does anything radiate HERE?" It is DERIVED DATA, never gameplay
// authority: authoritative state stays in flecs (positions, combat, ...);
// the field is disposable and rebuildable at any time. Never treat a field
// sample as authority for gameplay writes.
//
// Channels: plain enum, no virtual provider/consumer forest. Only
// PlayerInfluence is produced today; the enum + per-source channel tag keep
// every later channel (Combat, Replication, Navigation, Event, Predicted
// Movement, ...) a data addition instead of an architecture change.
namespace gs::game {

enum class ActivityChannel : std::uint8_t {
    PlayerInfluence = 0,
    // Future (Adaptive Simulation Fabric): Combat, ReplicationPressure,
    // NavigationCost, EventInfluence, ShipInfluence, PredictedMovement.
    // Each becomes a producer writing tagged sources + consumers reading
    // per-channel queries. No redesign of this header is needed for that.
};

// Which resolution level a snapshot represents. Only Local exists today;
// ZoneScale/RegionScale aggregates slot in as new query levels over the
// same cell storage without touching producers.
enum class ActivityLevel : std::uint8_t {
    Local = 0,
};

// Cell size of the world-space activity grid. Deliberately INDEPENDENT of
// SpatialGrid's cell size (AOI radius): the activity grid trades query box
// visits against aggregation cost, while the spatial index trades AOI
// fanout. Never unify the two concepts.
inline constexpr float kActivityCellSizeMeters = 500.0f;

// Minimal derived datum for cross-zone influence: stable identity +
// world position + origin metadata. NO health/inventory/stats/session —
// none of that is needed to answer "how relevant is HERE?".
struct PlayerInfluenceSource {
    std::uint32_t net_id = 0; // stable cross-zone identity (never reused)
    float x = 0.0f;
    float y = 0.0f;
    // Origin zone: metrics + validator provenance ONLY. The field is indexed
    // by world position, never by zone (compute topology != activity
    // topology); nothing routes on this value.
    std::uint32_t zone_id = 0;
    // Source zone tick at publish. Bounds snapshot staleness for auditing;
    // readers never branch gameplay on it.
    std::uint32_t tick = 0;
};

// Radii snapshot carried by every immutable field generation, so queries
// always agree with the config that built them (config stays authority).
struct ActivityRadii {
    float full_radius_m = 150.0f;
    float reduced_radius_m = 500.0f;
    float low_radius_m = 1500.0f;
};

// Exact tier mapping shared by the field query, the LOD consumer and the
// rebuild validator: single source of truth, strict less-than on squared
// distances (matches LodSystem bubble semantics bit-for-bit, so static
// entities resolve identically through either path).
inline SimulationTier TierForPlayerDistanceSq(float distance_sq, const ActivityRadii& radii) noexcept
{
    const float full_sq = radii.full_radius_m * radii.full_radius_m;
    const float reduced_sq = radii.reduced_radius_m * radii.reduced_radius_m;
    const float low_sq = radii.low_radius_m * radii.low_radius_m;
    return distance_sq < full_sq
               ? SimulationTier::Full
               : distance_sq < reduced_sq ? SimulationTier::Reduced
                                          : distance_sq < low_sq ? SimulationTier::Low
                                                                 : SimulationTier::Dormant;
}

// One field sample: exact nearest-player influence at a world position.
struct InfluenceSample {
    SimulationTier tier = SimulationTier::Dormant;
    float nearest_sq = FLT_MAX;
    // A source exists within low radius.
    bool has_influence = false;
    // The nearest source lives in another zone (joint-contribution
    // semantics: a local source at equal distance does not clear this;
    // first-minimum wins ties deterministically).
    bool cross_zone = false;
};

} // namespace gs::game
