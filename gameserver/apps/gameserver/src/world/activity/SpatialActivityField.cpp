#include "SpatialActivityField.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "common/Logging.h"

#include "../zone/Zone.h"
#include "../zone/ZoneManager.h"
#include "../zone/ZoneOwnership.h"

namespace gs::game {

std::uint32_t ActivityGrid::ClampedCellX(float x) const noexcept
{
    return ClampedAxisCellFor(x, bounds.min_x, cell_size_m, dim_x);
}

std::uint32_t ActivityGrid::ClampedCellY(float y) const noexcept
{
    return ClampedAxisCellFor(y, bounds.min_y, cell_size_m, dim_y);
}

void ActivityGrid::BoxRange(float x,
                            float y,
                            float radius,
                            int& x0,
                            int& x1,
                            int& y0,
                            int& y1) const noexcept
{
    x0 = static_cast<int>(ClampedCellX(x - radius));
    x1 = static_cast<int>(ClampedCellX(x + radius));
    y0 = static_cast<int>(ClampedCellY(y - radius));
    y1 = static_cast<int>(ClampedCellY(y + radius));
}

namespace {

// One walk, two semantics.
//
// EarlyOutOnFull == true reproduces the historic fast path EXACTLY: stop the
// source scan and the row scan once a source inside the Full bubble is seen,
// while the outer row loop keeps going -- as it always did. Tier
// classification, and the cross-zone diagnostic counters the LOD loop derives
// from the sample, therefore stay bit-identical to the pre-split behavior.
//
// EarlyOutOnFull == false skips nothing, so best_sq is the true minimum and
// best_zone belongs to the actually nearest source.
template <bool EarlyOutOnFull>
InfluenceSample QueryInfluenceImpl(const ActivityGrid& grid,
                                   float x,
                                   float y,
                                   std::uint32_t viewer_zone) noexcept
{
    InfluenceSample out;
    if (!grid.enabled || grid.cells.empty()) {
        return out;
    }
    const float full_sq = grid.radii.full_radius_m * grid.radii.full_radius_m;
    int x0, x1, y0, y1;
    grid.BoxRange(x, y, grid.radii.low_radius_m, x0, x1, y0, y1);
    float best_sq = FLT_MAX;
    std::uint32_t best_zone = 0;
    bool found = false;
    for (int cy = y0; cy <= y1; ++cy) {
        for (int cx = x0; cx <= x1; ++cx) {
            const auto& cell = grid.cells[static_cast<std::size_t>(cy) * grid.dim_x +
                                          static_cast<std::uint32_t>(cx)];
            for (const auto& source : cell.players) {
                const float dx = x - source.x;
                const float dy = y - source.y;
                const float d_sq = dx * dx + dy * dy;
                if (d_sq < best_sq) {
                    best_sq = d_sq;
                    best_zone = source.zone_id;
                    found = true;
                    if constexpr (EarlyOutOnFull) {
                        if (best_sq < full_sq) {
                            break; // tier cannot improve past Full
                        }
                    }
                }
            }
            if constexpr (EarlyOutOnFull) {
                if (found && best_sq < full_sq) {
                    break;
                }
            }
        }
    }
    if (!found) {
        return out;
    }
    out.nearest_sq = best_sq;
    out.cross_zone = (best_zone != viewer_zone);
    out.tier = TierForPlayerDistanceSq(best_sq, grid.radii);
    // Influence means inside the Low bubble; anything beyond is Dormant by
    // tier anyway. The flag is exact in BOTH variants: an early-out only ever
    // triggers strictly inside Full, which is strictly inside Low.
    out.has_influence = best_sq < grid.radii.low_radius_m * grid.radii.low_radius_m;
    return out;
}

} // namespace

InfluenceSample ActivityGrid::QueryPlayerTierFast(float x,
                                                  float y,
                                                  std::uint32_t viewer_zone) const noexcept
{
    return QueryInfluenceImpl<true>(*this, x, y, viewer_zone);
}

InfluenceSample ActivityGrid::QueryPlayerInfluenceExact(float x,
                                                        float y,
                                                        std::uint32_t viewer_zone) const noexcept
{
    return QueryInfluenceImpl<false>(*this, x, y, viewer_zone);
}

// `rect` (not `bounds`): the grid now carries a WorldBounds member called
// bounds, and a same-named parameter of a different type would shadow it.
bool ActivityGrid::HasPlayerWithin(const mx::map::Rect& rect, float radius) const noexcept
{
    if (!enabled || cells.empty() || !(radius >= 0.0f)) {
        return false;
    }
    const float cx = (rect.min_x + rect.max_x) * 0.5f;
    const float cy = (rect.min_y + rect.max_y) * 0.5f;
    // The walked box must cover the whole rect expanded by radius (not just
    // a disc around the center): half-extents plus radius on each axis.
    const float span_x = (rect.max_x - rect.min_x) * 0.5f + radius;
    const float span_y = (rect.max_y - rect.min_y) * 0.5f + radius;
    const int x0 = static_cast<int>(ClampedCellX(cx - span_x));
    const int x1 = static_cast<int>(ClampedCellX(cx + span_x));
    const int y0 = static_cast<int>(ClampedCellY(cy - span_y));
    const int y1 = static_cast<int>(ClampedCellY(cy + span_y));
    const float r_sq = radius * radius;
    for (int cyi = y0; cyi <= y1; ++cyi) {
        for (int cxi = x0; cxi <= x1; ++cxi) {
            const auto& cell =
                cells[static_cast<std::size_t>(cyi) * dim_x + static_cast<std::uint32_t>(cxi)];
            for (const auto& source : cell.players) {
                const float dx = source.x < rect.min_x   ? rect.min_x - source.x
                                 : source.x > rect.max_x ? source.x - rect.max_x
                                                         : 0.0f;
                const float dy = source.y < rect.min_y   ? rect.min_y - source.y
                                 : source.y > rect.max_y ? source.y - rect.max_y
                                                         : 0.0f;
                if (dx * dx + dy * dy <= r_sq) {
                    return true;
                }
            }
        }
    }
    return false;
}

std::size_t ActivityGrid::CountPlayerSourcesIn(const WorldBounds& rect) const noexcept
{
    if (!enabled || cells.empty() || !rect.IsValid()) {
        return 0;
    }
    if (rect.max_x <= bounds.min_x || rect.min_x >= bounds.max_x || rect.max_y <= bounds.min_y ||
        rect.min_y >= bounds.max_y) {
        return 0;
    }
    const std::uint32_t x0 = ClampedCellX(rect.min_x);
    const std::uint32_t x1 = ClampedCellX(std::nextafter(rect.max_x, rect.min_x));
    const std::uint32_t y0 = ClampedCellY(rect.min_y);
    const std::uint32_t y1 = ClampedCellY(std::nextafter(rect.max_y, rect.min_y));
    std::size_t count = 0;
    for (std::uint32_t cy = y0; cy <= y1; ++cy) {
        for (std::uint32_t cx = x0; cx <= x1; ++cx) {
            const auto& cell =
                cells[static_cast<std::size_t>(cy) * dim_x + cx];
            for (const auto& source : cell.players) {
                if (source.x >= rect.min_x && source.x < rect.max_x && source.y >= rect.min_y &&
                    source.y < rect.max_y) {
                    ++count;
                }
            }
        }
    }
    return count;
}

void ActivityPublisher::Publish(Zone& zone)
{
    AssertZoneOwner(zone, "zone activity publish");
    std::lock_guard lock(zone.ActivityMutex());
    auto& out = zone.ActivitySources();
    out.clear();
    out.reserve(zone.Players().size());
    const std::uint32_t tick = zone.TickIndex();
    const auto zone_id = zone.Id();
    for (const auto& [net_id, binding] : zone.Players()) {
        (void)binding;
        const auto player = zone.FindEntity(net_id);
        if (!player.is_valid() || !player.has<Position>()) {
            continue;
        }
        const auto pos = player.get<Position>();
        out.push_back(PlayerInfluenceSource{net_id, pos.x, pos.y, zone_id, tick});
    }
}

SpatialActivityField::SpatialActivityField(Config config)
    : config_(config)
{
    auto grid = std::make_shared<ActivityGrid>();
    grid->enabled = false;
    grid->cell_size_m = config_.cell_size_m > 0.0f ? config_.cell_size_m : kActivityCellSizeMeters;
    grid->bounds = config_.bounds;
    current_ = std::move(grid);
}

void SpatialActivityField::Reconfigure(Config config)
{
    std::lock_guard lock(mutex_);
    config_ = config;
    auto grid = std::make_shared<ActivityGrid>();
    grid->enabled = false;
    grid->cell_size_m = config_.cell_size_m > 0.0f ? config_.cell_size_m : kActivityCellSizeMeters;
    grid->bounds = config_.bounds;
    current_ = std::move(grid);
}

std::shared_ptr<const ActivityGrid> SpatialActivityField::Rebuild(const ZoneManager& zones,
                                                                  const ActivityRadii& radii,
                                                                  bool enabled)
{
    const auto rebuild_start = std::chrono::steady_clock::now();
    const float cell = config_.cell_size_m > 0.0f ? config_.cell_size_m : kActivityCellSizeMeters;
    WorldBounds bounds = config_.bounds;
    if (!bounds.IsValid()) {
        // Degenerate/unset world: keep a 1x1 cell grid so every query is a
        // safe no-op instead of dividing by a zero extent.
        bounds = WorldBounds::FromExtent(1.0f);
    }
    const auto dim_x = static_cast<std::uint32_t>(std::ceil(bounds.ExtentX() / cell));
    const auto dim_y = static_cast<std::uint32_t>(std::ceil(bounds.ExtentY() / cell));
    auto grid = std::make_shared<ActivityGrid>();
    grid->radii = radii;
    grid->enabled = enabled;
    grid->cell_size_m = cell;
    grid->bounds = bounds;
    grid->dim_x = dim_x > 0 ? dim_x : 1;
    grid->dim_y = dim_y > 0 ? dim_y : 1;
    grid->cells.resize(static_cast<std::size_t>(grid->dim_x) * grid->dim_y);
    grid->epoch = epoch_ + 1;

    std::uint64_t sources = 0;
    std::uint64_t nonempty = 0;
    if (enabled) {
        for (std::size_t i = 0; i < zones.ZoneCount(); ++i) {
            const Zone& zone = zones.GetZone(i);
            if (!zone.SimulationEnabled()) {
                continue;
            }
            std::lock_guard lock(zone.ActivityMutex());
            for (const auto& source : zone.ActivitySources()) {
                const std::uint32_t cx = grid->ClampedCellX(source.x);
                const std::uint32_t cy = grid->ClampedCellY(source.y);
                auto& target = grid->cells[static_cast<std::size_t>(cy) * grid->dim_x + cx];
                if (target.players.empty()) {
                    ++nonempty;
                }
                target.players.push_back(source);
                ++sources;
            }
        }
    }
    {
        std::lock_guard lock(mutex_);
        ++epoch_;
        grid->epoch = epoch_;
        current_ = grid;
        ++rebuilds_;
        last_sources_ = sources;
        last_cells_nonempty_ = nonempty;
        rebuild_us_total_ += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                  rebuild_start)
                .count());
    }
    return grid;
}

std::shared_ptr<const ActivityGrid> SpatialActivityField::Snapshot() const
{
    std::lock_guard lock(mutex_);
    return current_;
}

ActivityMetricsSnapshot SpatialActivityField::Metrics() const
{
    std::lock_guard lock(mutex_);
    ActivityMetricsSnapshot snapshot;
    snapshot.rebuilds = rebuilds_;
    snapshot.sources = last_sources_;
    snapshot.cells_total = current_ ? current_->CellCount() : 0;
    snapshot.cells_nonempty = last_cells_nonempty_;
    snapshot.rebuild_us_total = rebuild_us_total_;
    snapshot.epoch = epoch_;
    return snapshot;
}

} // namespace gs::game
