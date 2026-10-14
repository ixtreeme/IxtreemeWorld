#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "LoadFieldTypes.h"

// Continuous Multi-Channel Load Field (Adaptive Simulation Fabric, phase 1).
//
// WHAT: a world-indexed, multi-channel map of ACTUAL server work. Work is
// attributed to world positions at the call sites (zone tick, supervisor
// transfers), accumulated in per-zone sparse bins, published batched, then
// aggregated by the supervisor into an immutable generation. The partition
// tree is a CONSUMER: split/merge never changes what the field measures (§25).
//
// PUBLISH MODEL (§13): zone-local integer counters -> local spatial
// accumulation -> batched sparse publish -> supervisor aggregation ->
// immutable generation. No worker ever locks the field; no global map lookup
// or allocation happens on the hot path.
//
// THREADING: supervisor is the sole writer (Rebuild/RebuildFromEntries);
// readers copy a shared_ptr to an immutable generation. Same discipline as
// SpatialActivityField, without coupling the two fields.
namespace gs::game {

class ZoneManager;

// One immutable load generation. Dense rectangular storage over WorldBounds
// (§34): the 100km world at 500m is 40k cells; a dense grid keeps indexing
// branch-free and the memory cost is a documented, benchmarked number (see
// docs/adaptive-simulation-fabric.md). The L1 layer is a plain 2D sum
// aggregation of L0 (a seam, not a hierarchy framework, §36).
struct LoadGrid {
    bool enabled = false;
    std::uint64_t epoch = 0; // rebuild generation (quiescence-free versioning)
    float cell_size_m = 0.0f;
    WorldBounds bounds{};
    std::uint32_t dim_x = 0; // cells along X
    std::uint32_t dim_y = 0; // cells along Y
    std::vector<LoadCell> cells; // dim_x*dim_y, row-major (y * dim_x + x)

    // Coarse L1 seam: l1_ratio x l1_ratio L0 cells per L1 cell.
    bool l1_enabled = false;
    float l1_cell_size_m = 0.0f;
    std::uint32_t l1_dim_x = 0;
    std::uint32_t l1_dim_y = 0;
    std::vector<LoadCell> l1_cells;

    // Window diagnostics (§12): exactly the integers this generation consumed.
    LoadTotals totals;
    std::uint64_t active_cells = 0;
    std::uint64_t l1_active_cells = 0;
    float peak_normalized = 0.0f; // max single-channel normalized (fast EMA)
    float peak_composite = 0.0f;  // max composite (fast EMA)
    float window_seconds = 1.0f;
    // Config snapshot the generation was built with, so every query agrees
    // with the budgets/weights that produced it (config stays authority).
    LoadFieldConfig config{};

    std::uint32_t ClampedCellX(float x) const noexcept
    {
        return ClampedAxisCellFor(x, bounds.min_x, cell_size_m, dim_x);
    }
    std::uint32_t ClampedCellY(float y) const noexcept
    {
        return ClampedAxisCellFor(y, bounds.min_y, cell_size_m, dim_y);
    }

    // Null when the field is disabled or the grid is empty: callers must
    // treat "no sample" as zero load, never as a default hot cell.
    const LoadCell* CellAt(float x, float y) const noexcept
    {
        if (!enabled || cells.empty() || dim_x == 0) {
            return nullptr;
        }
        return &cells[static_cast<std::size_t>(ClampedCellY(y)) * dim_x + ClampedCellX(x)];
    }

    const LoadCell* L1CellAt(float x, float y) const noexcept
    {
        if (!enabled || !l1_enabled || l1_cells.empty() || l1_dim_x == 0) {
            return nullptr;
        }
        const std::uint32_t cx = ClampedAxisCellFor(x, bounds.min_x, l1_cell_size_m, l1_dim_x);
        const std::uint32_t cy = ClampedAxisCellFor(y, bounds.min_y, l1_cell_size_m, l1_dim_y);
        return &l1_cells[static_cast<std::size_t>(cy) * l1_dim_x + cx];
    }

    const LoadCell& CellByIndex(std::size_t index) const noexcept
    {
        return cells[index];
    }

    std::size_t CellCount() const noexcept
    {
        return cells.size();
    }
    std::size_t L1CellCount() const noexcept
    {
        return l1_cells.size();
    }

    // Normalized view of one timescale; raw channel breakdown stays available
    // through CellAt(). Never adds channels raw (§15).
    NormalizedLoad NormalizedLoadAt(float x, float y, LoadTimescale scale) const noexcept
    {
        const LoadCell* cell = CellAt(x, y);
        if (cell == nullptr) {
            return NormalizedLoad{};
        }
        return NormalizeLoad(LoadChannelsFor(*cell, scale), config, window_seconds);
    }

    float CompositeAt(float x, float y, LoadTimescale scale) const noexcept
    {
        return NormalizedLoadAt(x, y, scale).composite;
    }
};

// --- read-only aggregation seam (Adaptive Partition Scoring input) ----------
//
// The partition scorer NEVER walks the grid itself: it asks for the load of a
// world-space rect at one timescale. Deterministic, allocation-free,
// O(cells overlapping the rect). Half-open rect semantics: a cell is counted
// exactly once for a rect whose edges fall on cell boundaries (the scorer
// snaps its candidate cuts to cell boundaries for exactly this reason).
struct LoadAggregate {
    bool valid = false;
    WorldBounds rect{};
    std::uint64_t cells = 0;        // cells overlapping the rect
    std::uint64_t active_cells = 0; // cells with any nonzero smoothed load
    LoadChannels raw;               // summed raw channel values (chosen scale)
    float normalized[kLoadChannelCount] = {}; // summed per-cell normalized channels
    float composite_sum = 0.0f;     // sum of per-cell normalized composite
    float composite_peak = 0.0f;    // max per-cell normalized composite
    float peak_x = 0.0f;            // world position of the peak cell center
    float peak_y = 0.0f;
    float centroid_x = 0.0f;        // composite-weighted centroid
    float centroid_y = 0.0f;
    float weight_sum = 0.0f;        // total composite weight behind the centroid
};

LoadAggregate AggregateLoad(const LoadGrid& grid,
                            const WorldBounds& rect,
                            LoadTimescale scale) noexcept;

// Connected hot cells: 4-connectivity flood fill over cells whose chosen
// timescale composite >= threshold, clipped to `rect`. Bounded by
// max_hotspots (largest load first), deterministic ordering. `scratch` is a
// caller-owned buffer reused across calls (control-plane only; never hot).
struct LoadHotspot {
    WorldBounds bounds{}; // cell-aligned bounding box of the component
    float load = 0.0f;    // sum of per-cell composite over the component
    float peak = 0.0f;    // max per-cell composite
    float center_x = 0.0f;
    float center_y = 0.0f;
    std::uint32_t cells = 0;
};

std::vector<LoadHotspot> DetectLoadHotspots(const LoadGrid& grid,
                                            const WorldBounds& rect,
                                            LoadTimescale scale,
                                            float threshold,
                                            std::size_t max_hotspots,
                                            std::vector<std::uint8_t>& scratch);

// L1 = sum of the L0 cells in each l1_ratio x l1_ratio block. Because the
// EMA is linear, summing already-smoothed L0 values yields a correctly
// smoothed L1 without a second set of filters. Deterministic and O(cells).
inline void RebuildL1(LoadGrid& grid) noexcept
{
    grid.l1_enabled = grid.config.l1_enabled;
    grid.l1_cell_size_m = 0.0f;
    grid.l1_dim_x = 0;
    grid.l1_dim_y = 0;
    grid.l1_cells.clear();
    grid.l1_active_cells = 0;
    if (!grid.l1_enabled || grid.dim_x == 0 || grid.dim_y == 0) {
        return;
    }
    const std::uint32_t ratio = grid.config.l1_ratio;
    grid.l1_cell_size_m = grid.cell_size_m * static_cast<float>(ratio);
    grid.l1_dim_x = (grid.dim_x + ratio - 1) / ratio;
    grid.l1_dim_y = (grid.dim_y + ratio - 1) / ratio;
    grid.l1_cells.assign(static_cast<std::size_t>(grid.l1_dim_x) * grid.l1_dim_y, LoadCell{});
    for (std::uint32_t y = 0; y < grid.dim_y; ++y) {
        for (std::uint32_t x = 0; x < grid.dim_x; ++x) {
            auto& dst = grid.l1_cells[static_cast<std::size_t>(y / ratio) * grid.l1_dim_x +
                                      (x / ratio)];
            const auto& src = grid.cells[static_cast<std::size_t>(y) * grid.dim_x + x];
            dst.current += src.current;
            dst.fast += src.fast;
            dst.slow += src.slow;
        }
    }
    for (auto& cell : grid.l1_cells) {
        cell.predicted = cell.slow; // §37 seam
        if (IsLoadCellActive(cell)) {
            ++grid.l1_active_cells;
        }
    }
}

// Supervisor-side build metrics (plain snapshot for diagnostics/bench).
struct LoadFieldMetricsSnapshot {
    std::uint64_t rebuilds = 0;
    std::uint64_t epoch = 0;
    std::uint64_t cells_total = 0;
    std::uint64_t cells_active = 0;
    std::uint64_t l1_cells_total = 0;
    std::uint64_t l1_cells_active = 0;
    std::uint64_t rebuild_us_total = 0;
    std::uint64_t last_rebuild_us = 0;
    std::uint64_t drained_entries = 0;
    LoadTotals last_totals;
    float peak_normalized = 0.0f;
    float peak_composite = 0.0f;
};

class ContinuousLoadField {
public:
    explicit ContinuousLoadField(LoadFieldConfig config = LoadFieldConfig{});

    ContinuousLoadField(const ContinuousLoadField&) = delete;
    ContinuousLoadField& operator=(const ContinuousLoadField&) = delete;

    // Rare: world/config change. Drops the current generation (call pre-Start
    // or while idle; the shared_ptr swap is always safe for readers).
    void Reconfigure(LoadFieldConfig config);

    // Supervisor, at the configured cadence: drains every zone's published
    // sparse bins (under that zone's activity mutex; no tick gating needed)
    // and builds a fresh immutable generation. dt_seconds is the real elapsed
    // time since the previous rebuild, so smoothing stays cadence independent.
    std::shared_ptr<const LoadGrid> Rebuild(ZoneManager& zones, double dt_seconds);

    // Core builder over raw published entries. Rebuild(zones, dt) is the
    // production caller; the entry-list form is also the bench seam used to
    // benchmark resolutions without a running world (no fake runtime data:
    // the caller supplies the same entries a zone would publish).
    std::shared_ptr<const LoadGrid> RebuildFromEntries(const std::vector<LoadBinEntry>& entries,
                                                       double dt_seconds);

    // Current immutable generation (never null; starts as a disabled empty
    // grid). Copy the shared_ptr; reads need no further synchronization.
    std::shared_ptr<const LoadGrid> Snapshot() const;

    LoadFieldMetricsSnapshot Metrics() const;

    const LoadFieldConfig& GetConfig() const;

private:
    mutable std::mutex mutex_;
    LoadFieldConfig config_;
    std::shared_ptr<const LoadGrid> current_;
    std::uint64_t epoch_ = 0;
    std::uint64_t rebuilds_ = 0;
    std::uint64_t rebuild_us_total_ = 0;
    std::uint64_t last_rebuild_us_ = 0;
    std::uint64_t drained_entries_ = 0;
    LoadTotals last_totals_;
    float peak_normalized_ = 0.0f;
    float peak_composite_ = 0.0f;
    std::vector<LoadBinEntry> drain_scratch_; // supervisor only
};

// Field self-consistency audit (debug/test only; never on the hot path).
// Verifies the invariants a consumer may rely on: dimensions vs storage,
// finite/non-negative channels, predicted == slow (§37 seam), active-cell
// count, raw totals == sum of cells, normalized/composite within [0,1], and
// L1 == exact block sums. This audits the FIELD, not the world: raw work
// events have already been consumed, so there is no brute-force reference
// (that role is played by the bench scenarios).
bool ValidateLoadFieldGrid(const LoadGrid& grid, std::string& out_error);

} // namespace gs::game
