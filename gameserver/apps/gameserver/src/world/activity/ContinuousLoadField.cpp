#include "ContinuousLoadField.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "../zone/Zone.h"
#include "../zone/ZoneManager.h"

namespace gs::game {

namespace {

std::uint64_t ElapsedUs(std::chrono::steady_clock::time_point from,
                        std::chrono::steady_clock::time_point to)
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(to - from).count());
}

bool SameWorld(const LoadGrid& lhs, const LoadGrid& rhs) noexcept
{
    return lhs.dim_x == rhs.dim_x && lhs.dim_y == rhs.dim_y &&
           lhs.cell_size_m == rhs.cell_size_m && lhs.bounds.min_x == rhs.bounds.min_x &&
           lhs.bounds.min_y == rhs.bounds.min_y && lhs.bounds.max_x == rhs.bounds.max_x &&
           lhs.bounds.max_y == rhs.bounds.max_y;
}

} // namespace

ContinuousLoadField::ContinuousLoadField(LoadFieldConfig config)
    : config_(config)
{
    auto grid = std::make_shared<LoadGrid>();
    grid->enabled = false;
    grid->cell_size_m = config_.cell_size_m;
    grid->bounds = config_.bounds;
    grid->window_seconds =
        config_.aggregation_hz > 0.0f ? 1.0f / config_.aggregation_hz : 1.0f;
    grid->config = config_;
    current_ = std::move(grid);
}

void ContinuousLoadField::Reconfigure(LoadFieldConfig config)
{
    std::lock_guard lock(mutex_);
    config_ = config;
    auto grid = std::make_shared<LoadGrid>();
    grid->enabled = false;
    grid->cell_size_m = config_.cell_size_m;
    grid->bounds = config_.bounds;
    grid->window_seconds =
        config_.aggregation_hz > 0.0f ? 1.0f / config_.aggregation_hz : 1.0f;
    grid->config = config_;
    current_ = std::move(grid);
}

std::shared_ptr<const LoadGrid> ContinuousLoadField::Rebuild(ZoneManager& zones, double dt_seconds)
{
    drain_scratch_.clear();
    for (std::size_t i = 0; i < zones.ZoneCount(); ++i) {
        Zone& zone = zones.GetZone(i);
        std::lock_guard lock(zone.ActivityMutex());
        auto& published = zone.PublishedLoadBins();
        if (published.empty()) {
            continue;
        }
        // Sparse entries are bounded by touched cells, so this is a handful
        // of pushes per zone per publish window -- never a dense copy.
        drain_scratch_.insert(drain_scratch_.end(), published.begin(), published.end());
        published.clear();
    }
    return RebuildFromEntries(drain_scratch_, dt_seconds);
}

std::shared_ptr<const LoadGrid> ContinuousLoadField::RebuildFromEntries(
    const std::vector<LoadBinEntry>& entries,
    double dt_seconds)
{
    const auto start = std::chrono::steady_clock::now();
    LoadFieldConfig config;
    std::shared_ptr<const LoadGrid> previous;
    {
        std::lock_guard lock(mutex_);
        config = config_;
        previous = current_;
    }

    const auto mapping = LoadFieldMapping::FromConfig(config);
    auto grid = std::make_shared<LoadGrid>();
    grid->enabled = config.enabled;
    grid->cell_size_m = mapping.cell_size_m;
    grid->bounds = mapping.bounds;
    grid->window_seconds = 1.0f / config.aggregation_hz;
    grid->config = config;
    if (!config.enabled) {
        // Disabled field: an empty generation with no storage and no work.
        // Consumers see Enabled()==false; queries answer zero. Any pending
        // published deltas were already drained by Rebuild() and are dropped.
        std::lock_guard lock(mutex_);
        ++epoch_;
        grid->epoch = epoch_;
        current_ = grid;
        ++rebuilds_;
        last_rebuild_us_ = ElapsedUs(start, std::chrono::steady_clock::now());
        rebuild_us_total_ += last_rebuild_us_;
        drained_entries_ = entries.size();
        last_totals_ = LoadTotals{};
        peak_normalized_ = 0.0f;
        peak_composite_ = 0.0f;
        return grid;
    }
    grid->dim_x = mapping.dim_x;
    grid->dim_y = mapping.dim_y;
    grid->cells.assign(static_cast<std::size_t>(grid->dim_x) * grid->dim_y, LoadCell{});

    // dt is clamped to a sane band: a stalled supervisor must not turn the
    // EMA into a single jump, and dt <= 0 (same-pass rebuild) must not stall
    // it either.
    const float dt = static_cast<float>(std::clamp(dt_seconds, 0.001, 60.0));

    // Carry the smoothed state across generations when the geometry is
    // unchanged. `current` is deliberately NOT carried: it always describes
    // exactly one window. A geometry change (cell size/bounds/dims) starts
    // fresh -- the old generation's smoothed values are not reinterpretable.
    const bool carry = previous && previous->enabled && SameWorld(*previous, *grid);
    if (carry) {
        for (std::size_t i = 0; i < grid->cells.size(); ++i) {
            grid->cells[i].fast = previous->cells[i].fast;
            grid->cells[i].slow = previous->cells[i].slow;
        }
        grid->totals.windows = previous->totals.windows + 1;
    } else {
        grid->totals.windows = 1;
    }

    if (config.enabled) {
        for (const auto& entry : entries) {
            if (entry.gx >= grid->dim_x || entry.gy >= grid->dim_y) {
                continue; // defensive: zone bins clamp into the world by construction
            }
            const std::size_t index =
                static_cast<std::size_t>(entry.gy) * grid->dim_x + entry.gx;
            grid->cells[index].current += ChannelsFromCounters(entry.counters);
            grid->totals.Add(entry.counters);
        }
    }

    // One pass over the dense grid: decay/rise + active count + peaks. Cells
    // with no history and no new work are skipped (the common sparse case).
    for (auto& cell : grid->cells) {
        if (!IsLoadCellActive(cell)) {
            continue;
        }
        const LoadChannels window = cell.current;
        AdvanceLoadCell(cell, window, config, dt);
        ++grid->active_cells;
        const NormalizedLoad normalized = NormalizeLoad(cell.fast, config, grid->window_seconds);
        for (std::size_t channel = 0; channel < kLoadChannelCount; ++channel) {
            grid->peak_normalized = std::max(grid->peak_normalized, normalized.values[channel]);
        }
        grid->peak_composite = std::max(grid->peak_composite, normalized.composite);
    }

    if (config.l1_enabled) {
        RebuildL1(*grid);
    }

    {
        std::lock_guard lock(mutex_);
        ++epoch_;
        grid->epoch = epoch_;
        current_ = grid;
        ++rebuilds_;
        last_rebuild_us_ = ElapsedUs(start, std::chrono::steady_clock::now());
        rebuild_us_total_ += last_rebuild_us_;
        drained_entries_ = entries.size();
        last_totals_ = grid->totals;
        peak_normalized_ = grid->peak_normalized;
        peak_composite_ = grid->peak_composite;
    }
    return grid;
}

std::shared_ptr<const LoadGrid> ContinuousLoadField::Snapshot() const
{
    std::lock_guard lock(mutex_);
    return current_;
}

LoadFieldMetricsSnapshot ContinuousLoadField::Metrics() const
{
    std::lock_guard lock(mutex_);
    LoadFieldMetricsSnapshot snapshot;
    snapshot.rebuilds = rebuilds_;
    snapshot.epoch = epoch_;
    snapshot.cells_total = current_ ? current_->CellCount() : 0;
    snapshot.cells_active = current_ ? current_->active_cells : 0;
    snapshot.l1_cells_total = current_ ? current_->L1CellCount() : 0;
    snapshot.l1_cells_active = current_ ? current_->l1_active_cells : 0;
    snapshot.rebuild_us_total = rebuild_us_total_;
    snapshot.last_rebuild_us = last_rebuild_us_;
    snapshot.drained_entries = drained_entries_;
    snapshot.last_totals = last_totals_;
    snapshot.peak_normalized = peak_normalized_;
    snapshot.peak_composite = peak_composite_;
    return snapshot;
}

const LoadFieldConfig& ContinuousLoadField::GetConfig() const
{
    std::lock_guard lock(mutex_);
    return config_;
}

bool ValidateLoadFieldGrid(const LoadGrid& grid, std::string& out_error)
{
    auto fail = [&out_error](const char* message) {
        out_error = message;
        return false;
    };

    if (!grid.enabled) {
        // A disabled generation carries no storage and no work by contract.
        if (!grid.cells.empty() || grid.dim_x != 0 || grid.dim_y != 0) {
            return fail("disabled grid must not carry cell storage");
        }
        out_error.clear();
        return true;
    }
    if (grid.dim_x == 0 || grid.dim_y == 0) {
        return fail("grid has zero dimensions");
    }
    if (grid.cells.size() != static_cast<std::size_t>(grid.dim_x) * grid.dim_y) {
        return fail("cell storage does not match dim_x*dim_y");
    }
    if (!(grid.cell_size_m > 0.0f) || !grid.bounds.IsValid()) {
        return fail("grid geometry is invalid");
    }
    if (!(grid.window_seconds > 0.0f)) {
        return fail("window_seconds is invalid");
    }

    LoadChannels current_sum;
    std::uint64_t active = 0;
    for (const auto& cell : grid.cells) {
        if (!cell.current.IsFiniteNonNegative() || !cell.fast.IsFiniteNonNegative() ||
            !cell.slow.IsFiniteNonNegative() || !cell.predicted.IsFiniteNonNegative()) {
            return fail("cell contains a non-finite or negative channel value");
        }
        for (std::size_t i = 0; i < kLoadChannelCount; ++i) {
            if (cell.predicted.values[i] != cell.slow.values[i]) {
                return fail("predicted != slow (seam invariant broken)");
            }
        }
        current_sum += cell.current;
        if (IsLoadCellActive(cell)) {
            ++active;
        }
        const NormalizedLoad normalized =
            NormalizeLoad(cell.slow, grid.config, grid.window_seconds);
        for (std::size_t i = 0; i < kLoadChannelCount; ++i) {
            if (!(normalized.values[i] >= 0.0f) || normalized.values[i] > 1.0f) {
                return fail("normalized channel outside [0,1]");
            }
        }
        if (!(normalized.composite >= 0.0f) || normalized.composite > 1.0f) {
            return fail("composite outside [0,1]");
        }
    }
    if (active != grid.active_cells) {
        return fail("active cell count does not match the grid");
    }

    // Raw totals must equal the sum of the per-cell current values. The only
    // float tolerance needed is the summation order difference.
    auto close = [](float lhs, float rhs) {
        const float scale = std::max(1.0f, std::max(std::abs(lhs), std::abs(rhs)));
        return std::abs(lhs - rhs) <= 1e-3f * scale;
    };
    const float expected_sim = static_cast<float>(grid.totals.sim_work);
    const float expected_repl = static_cast<float>(grid.totals.repl_bytes);
    const float expected_aoi = static_cast<float>(grid.totals.aoi_queries) +
                               static_cast<float>(grid.totals.aoi_candidates);
    const float expected_combat = static_cast<float>(grid.totals.combat_events);
    const float expected_migration = static_cast<float>(grid.totals.migration_events);
    if (!close(current_sum[LoadChannel::Simulation], expected_sim) ||
        !close(current_sum[LoadChannel::Replication], expected_repl) ||
        !close(current_sum[LoadChannel::AOI], expected_aoi) ||
        !close(current_sum[LoadChannel::Combat], expected_combat) ||
        !close(current_sum[LoadChannel::Migration], expected_migration)) {
        return fail("raw totals do not match the sum of the cells");
    }

    // L1 must be an exact block sum of the L0 generation.
    if (grid.l1_enabled) {
        if (grid.l1_dim_x != (grid.dim_x + grid.config.l1_ratio - 1) / grid.config.l1_ratio ||
            grid.l1_dim_y != (grid.dim_y + grid.config.l1_ratio - 1) / grid.config.l1_ratio) {
            return fail("L1 dimensions do not match the L0 grid");
        }
        if (grid.l1_cells.size() !=
            static_cast<std::size_t>(grid.l1_dim_x) * grid.l1_dim_y) {
            return fail("L1 storage does not match its dimensions");
        }
        std::vector<LoadCell> expected(
            static_cast<std::size_t>(grid.l1_dim_x) * grid.l1_dim_y, LoadCell{});
        for (std::uint32_t y = 0; y < grid.dim_y; ++y) {
            for (std::uint32_t x = 0; x < grid.dim_x; ++x) {
                auto& dst = expected[static_cast<std::size_t>(y / grid.config.l1_ratio) *
                                         grid.l1_dim_x +
                                     (x / grid.config.l1_ratio)];
                const auto& src = grid.cells[static_cast<std::size_t>(y) * grid.dim_x + x];
                dst.current += src.current;
                dst.fast += src.fast;
                dst.slow += src.slow;
            }
        }
        std::uint64_t l1_active = 0;
        for (std::size_t i = 0; i < grid.l1_cells.size(); ++i) {
            const auto& cell = grid.l1_cells[i];
            for (std::size_t channel = 0; channel < kLoadChannelCount; ++channel) {
                if (cell.current.values[channel] != expected[i].current.values[channel] ||
                    cell.fast.values[channel] != expected[i].fast.values[channel] ||
                    cell.slow.values[channel] != expected[i].slow.values[channel] ||
                    cell.predicted.values[channel] != cell.slow.values[channel]) {
                    return fail("L1 aggregation is not an exact L0 block sum");
                }
            }
            if (IsLoadCellActive(cell)) {
                ++l1_active;
            }
        }
        if (l1_active != grid.l1_active_cells) {
            return fail("L1 active cell count does not match");
        }
    }

    out_error.clear();
    return true;
}

} // namespace gs::game
