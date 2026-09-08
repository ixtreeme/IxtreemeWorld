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
    float world_extent_m = 0.0f;
    std::uint32_t dim = 0; // cells per side; world is [0, extent]^2
    std::vector<ActivityCell> cells; // dim*dim, row-major (y * dim + x)

    std::uint32_t CellDim() const noexcept
    {
        return dim;
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

    // Exact nearest-player influence at a world position. Box-walks only
    // cells overlapping the low-radius disc, then takes the exact minimum
    // over sources inside (early-out below full radius). Deterministic
    // given identical input; first-minimum wins exact ties.
    InfluenceSample QueryPlayerInfluence(float x, float y, std::uint32_t viewer_zone) const noexcept;

    // Exact predicate: any player source within `radius` of the rect
    // (point-to-rect distance, early-out on first hit). Used for
    // sleep/wake decisions; exact, so never over- or under-sleeps.
    bool HasPlayerWithin(const mx::map::Rect& bounds, float radius) const noexcept;

    // Cell helpers (public: the supervisor builder shares them; pure index
    // math, no state beyond grid dims).
    std::uint32_t ClampedCell(float v) const noexcept;
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
        float world_extent_m = 100000.0f;
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
