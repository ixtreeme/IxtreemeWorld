#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "map/MapData.h"

#include "LoadFieldTypes.h"

// Per-zone publication of continuous load work (mirrors the activity-field
// publisher discipline).
//
// HOT PATH (§14): zone systems call CellFor(x, y) once per entity/event and
// bump integer counters on the returned struct. No locks, no allocation, no
// string work, no clock reads, no global lookup: one world->cell index
// computation (the same primitive as the activity field) plus increments.
// The accumulator is thread-confined to whoever holds the zone write guard
// (the worker tick, the supervisor command drain, or a guarded transfer).
//
// PUBLISH (§13): at the end of each tick the zone moves its touched sparse
// deltas into the zone's published buffer under the activity mutex (a
// microsecond section). The supervisor drains those buffers at the field's
// aggregation cadence. A slow supervisor accumulates at most one publish per
// tick per zone; it can never lose or double-count a delta.
namespace gs::game {

class Zone;

// The local rectangle is expanded by this margin so work that legitimately
// belongs to this zone but happens just outside its bounds still maps into a
// real cell: boundary-crossing transitions (migration hysteresis band) and
// transfer destinations. Must stay comfortably below min_zone_size/2.
inline constexpr float kLoadBinMarginMeters = 32.0f;

// Zone-local dense rectangle over the field grid, covering this zone's
// bounds (+ margin). Dense over the RECT (not the world): a zone only pays
// for the cells it can touch. Sparse publication keeps the per-tick copy
// proportional to touched cells, not to the rectangle.
class ZoneLoadBins {
public:
    // Configures the local rectangle for the zone bounds. Called at zone
    // creation / explicit field reconfiguration; never from a hot path.
    void Configure(const LoadFieldMapping& mapping, const mx::map::Rect& zone_bounds)
    {
        mapping_ = mapping;
        enabled_ = mapping.valid;
        touched_.clear();
        cells_.clear();
        stamp_.clear();
        local_x0_ = 0;
        local_y0_ = 0;
        dim_x_ = 0;
        dim_y_ = 0;
        epoch_ = 1;
        if (!enabled_) {
            return;
        }
        const std::uint32_t gx0 = mapping_.CellX(zone_bounds.min_x - kLoadBinMarginMeters);
        const std::uint32_t gx1 = mapping_.CellX(zone_bounds.max_x + kLoadBinMarginMeters);
        const std::uint32_t gy0 = mapping_.CellY(zone_bounds.min_y - kLoadBinMarginMeters);
        const std::uint32_t gy1 = mapping_.CellY(zone_bounds.max_y + kLoadBinMarginMeters);
        local_x0_ = gx0;
        local_y0_ = gy0;
        dim_x_ = gx1 - gx0 + 1;
        dim_y_ = gy1 - gy0 + 1;
        cells_.assign(static_cast<std::size_t>(dim_x_) * dim_y_, LoadCellCounters{});
        stamp_.assign(static_cast<std::size_t>(dim_x_) * dim_y_, 0u);
        touched_.reserve(64);
    }

    void Disable() noexcept
    {
        enabled_ = false;
        touched_.clear();
        cells_.clear();
        stamp_.clear();
        dim_x_ = 0;
        dim_y_ = 0;
    }

    bool Enabled() const noexcept
    {
        return enabled_;
    }
    const LoadFieldMapping& Mapping() const noexcept
    {
        return mapping_;
    }
    std::uint32_t LocalDimX() const noexcept
    {
        return dim_x_;
    }
    std::uint32_t LocalDimY() const noexcept
    {
        return dim_y_;
    }

    // Hot-path accessor: nullptr when the field is disabled or the position
    // cannot map into the local rectangle. Callers bump counters directly:
    //   if (auto* cell = bins.CellFor(x, y)) { cell->sim_work++; }
    LoadCellCounters* CellFor(float x, float y) noexcept
    {
        if (!enabled_) {
            return nullptr;
        }
        const std::uint32_t gx = mapping_.CellX(x);
        const std::uint32_t gy = mapping_.CellY(y);
        if (gx < local_x0_ || gy < local_y0_) {
            return nullptr;
        }
        const std::uint32_t lx = gx - local_x0_;
        const std::uint32_t ly = gy - local_y0_;
        if (lx >= dim_x_ || ly >= dim_y_) {
            return nullptr;
        }
        const std::uint32_t index = ly * dim_x_ + lx;
        if (stamp_[index] != epoch_) {
            stamp_[index] = epoch_;
            touched_.push_back(index);
        }
        return &cells_[index];
    }

    // Single-event convenience wrappers (combat/migration): one index
    // computation, no touched bookkeeping when disabled.
    void NoteCombat(float x, float y) noexcept
    {
        if (auto* cell = CellFor(x, y)) {
            ++cell->combat_events;
        }
    }
    void NoteMigration(float x, float y) noexcept
    {
        if (auto* cell = CellFor(x, y)) {
            ++cell->migration_events;
        }
    }

    // Tick-thread only, under the zone write guard: appends every touched
    // cell's counters to `out` and resets the pending window. Bounded by the
    // number of touched cells, not by the local rectangle size.
    void MovePendingTo(std::vector<LoadBinEntry>& out)
    {
        if (!enabled_ || touched_.empty()) {
            return;
        }
        out.reserve(out.size() + touched_.size());
        for (const std::uint32_t index : touched_) {
            LoadBinEntry entry;
            entry.gx = local_x0_ + index % dim_x_;
            entry.gy = local_y0_ + index / dim_x_;
            entry.counters = cells_[index];
            out.push_back(entry);
            cells_[index] = LoadCellCounters{};
        }
        touched_.clear();
        ++epoch_;
        if (epoch_ == 0) {
            // 32-bit wrap: restart the stamp space so old stamps cannot match.
            std::fill(stamp_.begin(), stamp_.end(), 0u);
            epoch_ = 1;
        }
    }

    void ClearPending() noexcept
    {
        for (const std::uint32_t index : touched_) {
            cells_[index] = LoadCellCounters{};
        }
        touched_.clear();
        ++epoch_;
        if (epoch_ == 0) {
            std::fill(stamp_.begin(), stamp_.end(), 0u);
            epoch_ = 1;
        }
    }

    std::size_t PendingTouched() const noexcept
    {
        return touched_.size();
    }

private:
    bool enabled_ = false;
    LoadFieldMapping mapping_{};
    std::uint32_t local_x0_ = 0; // global cell origin of the local rectangle
    std::uint32_t local_y0_ = 0;
    std::uint32_t dim_x_ = 0;
    std::uint32_t dim_y_ = 0;
    std::vector<LoadCellCounters> cells_; // dense over the local rectangle
    std::vector<std::uint32_t> stamp_;    // per-cell touched epoch (dedupe)
    std::vector<std::uint32_t> touched_;  // sparse indices touched this window
    std::uint32_t epoch_ = 1;
};

// End-of-tick publication: called under the zone write guard, fills the
// zone's published buffer under its activity mutex. Never held across system
// work, never nested, so no lock ordering issues arise.
class LoadFieldPublisher {
public:
    static void Publish(Zone& zone);
};

} // namespace gs::game
