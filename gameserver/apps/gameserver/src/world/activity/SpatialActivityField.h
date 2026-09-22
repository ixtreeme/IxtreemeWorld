#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "map/MapData.h"

#include "ActivityTypes.h"

// World-space Spatial Activity Field (first foundation of the future
// Adaptive Simulation Fabric).
//
// WHAT: a flat, world-indexed cell grid of player influence sources,
// rebuilt ~1Hz on the supervisor thread from per-zone publications and
// consumed via immutable snapshots. Compute topology (zones/processes)
// NEVER appears in the addressing: queries take world coordinates and see
// every authoritative player regardless of zone, region or split state.
// A split turning 1 zone into 4 changes nothing observable here.
//
// THREADING: the supervisor is the sole writer. It aggregates per-zone
// source buffers (each guarded by that zone's activity mutex; no tick
// gating needed) into a fresh grid, then swaps one shared_ptr. Readers
// (zone ticks via ZoneTickContext, scheduler, validator, bench) hold a
// snapshot copy: no locks, no races, always a complete generation.
//
// STALENESS (documented bound, §11-13): per-zone publish runs every zone
// tick (≤50ms old); aggregation runs ~1Hz. Influence therefore lags reality
// by ~1s worst case (≤6m at player run speed). This matches the pre-existing
// LOD evaluation period, so it introduces no new timing class; static
// entities (all deterministic tests) observe exactly zero staleness.
// Despawn/disconnect disappearance is bounded the same way: the owning zone
// republishes without the source on its next tick (or wipes its buffer on
// sleep/retire), and the next aggregation drops it.
namespace gs::game {

class Zone;
class ZoneManager;

struct ActivityCell {
    std::vector<PlayerInfluenceSource> players;
    // Future per-channel aggregates slot in HERE (CombatHeat,
    // SimulationCost, ReplicationCost, ... §23): same cells, same epochs,
    // new fields. No gameplay pointers or flecs handles, ever.
};

struct ActivityGrid {
    ActivityRadii radii;
    ActivityLevel level = ActivityLevel::Local;
    std::uint64_t epoch = 0; // rebuild generation (quiescence-free versioning)
    // False = LOD disabled: queries answer Dormant/none so nothing simulates
    // on stale influence, and validators skip activity checks.
    bool enabled = false;
    float cell_size_m = kActivityCellSizeMeters;
    // Explicit world rectangle: the origin may be non-zero and the extents may
    // differ per axis. Indexing subtracts the origin, so a -50km..+50km world
    // is addressed correctly (the earlier code folded negatives into cell 0).
    WorldBounds bounds{};
    std::uint32_t dim_x = 0; // cells along X
    std::uint32_t dim_y = 0; // cells along Y
    std::vector<ActivityCell> cells; // dim_x*dim_y, row-major (y * dim_x + x)

    std::uint32_t CellDimX() const noexcept
    {
        return dim_x;
    }

    std::uint32_t CellDimY() const noexcept
    {
        return dim_y;
    }

    const ActivityRadii& Radii() const noexcept
    {
        return radii;
    }

    bool Enabled() const noexcept
    {
        return enabled;
    }

    // Stable indexed cell access for validators (row-major order).
    const ActivityCell& CellAt(std::size_t index) const noexcept
    {
        return cells[index];
    }

    std::size_t CellCount() const noexcept
    {
        return cells.size();
    }

    std::size_t NonEmptyCellCount() const noexcept
    {
        std::size_t count = 0;
        for (const auto& cell : cells) {
            if (!cell.players.empty()) {
                ++count;
            }
        }
        return count;
    }

    // TIER CLASSIFICATION (LOD hot path). Box-walks the cells overlapping the
    // low-radius disc and STOPS as soon as any source is inside the Full
    // bubble -- nothing closer could raise the tier any further.
    //
    //   EXACT:       tier, has_influence.
    //   BEST-EFFORT: nearest_sq, cross_zone. When the walk early-outs these
    //                describe SOME source inside the Full bubble, not
    //                necessarily the nearest one. Never use them for
    //                provenance, distance reporting or planner input --
    //                use the Exact query for that.
    InfluenceSample QueryPlayerTierFast(float x, float y, std::uint32_t viewer_zone) const noexcept;

    // FULL-PRECISION (adaptive planner, diagnostics, validators). Walks every
    // cell overlapping the low-radius disc with no early-out, so nearest_sq is
    // the true minimum and cross_zone names the zone of the actually nearest
    // source. Deterministic; first-minimum wins exact ties. Strictly more work
    // than the Fast query -- keep it off per-entity hot paths.
    //
    //   EXACT: tier, has_influence, nearest_sq, cross_zone.
    InfluenceSample QueryPlayerInfluenceExact(float x, float y, std::uint32_t viewer_zone) const noexcept;

    // Exact predicate: any player source within `radius` of the rect
    // (point-to-rect distance, early-out on first hit). Used for
    // sleep/wake decisions; exact, so never over- or under-sleeps.
    bool HasPlayerWithin(const mx::map::Rect& rect, float radius) const noexcept;

    // Read-only helper for the adaptive boundary-cost estimate: counts player
    // sources whose exact position lies inside `rect` (half-open). The
    // activity field stays an independent system -- this is a separate
    // boundary signal, never folded into a load field channel (§17).
    std::size_t CountPlayerSourcesIn(const WorldBounds& rect) const noexcept;

    // Cell helpers (public: the supervisor builder shares them; pure index
    // math, no state beyond grid dims).
    std::uint32_t ClampedCellX(float x) const noexcept;
    std::uint32_t ClampedCellY(float y) const noexcept;
    void BoxRange(float x, float y, float radius, int& x0, int& x1, int& y0, int& y1) const noexcept;
};

// Per-zone publication (mirrors BorderPublisher discipline): called at the
// end of Zone::Tick under the zone write guard; fills the zone's source
// buffer under its activity mutex (microsecond section, reused storage).
class ActivityPublisher {
public:
    static void Publish(Zone& zone);
};

// Supervisor-side build metrics (plain members: supervisor thread only).
struct ActivityMetricsSnapshot {
    std::uint64_t rebuilds = 0;
    std::uint64_t sources = 0; // player sources in the current generation
    std::uint64_t cells_total = 0;
    std::uint64_t cells_nonempty = 0;
    std::uint64_t rebuild_us_total = 0;
    std::uint64_t epoch = 0;
};

class SpatialActivityField {
public:
    struct Config {
        float cell_size_m = kActivityCellSizeMeters;
        // Defaults to the historic origin-at-zero square world, so existing
        // deployments keep identical coordinates.
        WorldBounds bounds = WorldBounds::FromExtent(100000.0f);
    };

    explicit SpatialActivityField(Config config = Config{});

    SpatialActivityField(const SpatialActivityField&) = delete;
    SpatialActivityField& operator=(const SpatialActivityField&) = delete;

    // Rare: world/config change. Drops the current snapshot (call pre-Start
    // or while idle; not synchronized against in-flight readers beyond the
    // shared_ptr swap, which is always safe).
    void Reconfigure(Config config);

    // Supervisor, ~1Hz, no tick gating needed (copies per-zone buffers under
    // their own mutexes). When !enabled, publishes an empty DISABLED
    // generation so stale influence can never linger.
    std::shared_ptr<const ActivityGrid> Rebuild(const ZoneManager& zones,
                                                const ActivityRadii& radii,
                                                bool enabled);

    // Current immutable generation (never null: starts as an empty disabled
    // grid). Copy the shared_ptr; reads need no further synchronization.
    std::shared_ptr<const ActivityGrid> Snapshot() const;

    ActivityMetricsSnapshot Metrics() const;

private:
    Config config_;
    mutable std::mutex mutex_;
    std::shared_ptr<const ActivityGrid> current_;
    std::uint64_t epoch_ = 0;
    // Supervisor-only: written in Rebuild, read under mutex_.
    std::uint64_t rebuilds_ = 0;
    std::uint64_t last_sources_ = 0;
    std::uint64_t last_cells_nonempty_ = 0;
    std::uint64_t rebuild_us_total_ = 0;
};

} // namespace gs::game
