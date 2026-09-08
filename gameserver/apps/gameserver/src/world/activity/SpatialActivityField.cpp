#include "SpatialActivityField.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "common/Logging.h"

#include "../zone/Zone.h"
#include "../zone/ZoneManager.h"
#include "../zone/ZoneOwnership.h"

namespace gs::game {

std::uint32_t ActivityGrid::ClampedCell(float v) const noexcept
{
    if (dim == 0) {
        return 0;
    }
    const int c = static_cast<int>(std::floor(v / cell_size_m));
    if (c < 0) {
        return 0;
    }
    const auto last = static_cast<int>(dim) - 1;
    return static_cast<std::uint32_t>(c > last ? last : c);
}

void ActivityGrid::BoxRange(float x,
                            float y,
                            float radius,
                            int& x0,
                            int& x1,
                            int& y0,
                            int& y1) const noexcept
{
    x0 = static_cast<int>(ClampedCell(x - radius));
    x1 = static_cast<int>(ClampedCell(x + radius));
    y0 = static_cast<int>(ClampedCell(y - radius));
    y1 = static_cast<int>(ClampedCell(y + radius));
}

InfluenceSample ActivityGrid::QueryPlayerInfluence(float x,
                                                   float y,
                                                   std::uint32_t viewer_zone) const noexcept
{
    InfluenceSample out;
    if (!enabled || cells.empty()) {
        return out;
    }
    const float full_sq = radii.full_radius_m * radii.full_radius_m;
    int x0, x1, y0, y1;
    BoxRange(x, y, radii.low_radius_m, x0, x1, y0, y1);
    float best_sq = FLT_MAX;
    std::uint32_t best_zone = 0;
    bool found = false;
    for (int cy = y0; cy <= y1; ++cy) {
        for (int cx = x0; cx <= x1; ++cx) {
            const auto& cell =
                cells[static_cast<std::size_t>(cy) * dim + static_cast<std::uint32_t>(cx)];
            for (const auto& source : cell.players) {
                const float dx = x - source.x;
                const float dy = y - source.y;
                const float d_sq = dx * dx + dy * dy;
                if (d_sq < best_sq) {
                    best_sq = d_sq;
                    best_zone = source.zone_id;
                    found = true;
                    if (best_sq < full_sq) {
                        break; // can't beat Full
                    }
                }
            }
            if (found && best_sq < full_sq) {
                break;
            }
        }
    }
    if (!found) {
        return out;
    }
    out.nearest_sq = best_sq;
    out.cross_zone = (best_zone != viewer_zone);
    out.tier = TierForPlayerDistanceSq(best_sq, radii);
    // Influence means inside the Low bubble; anything beyond is Dormant by
    // tier anyway, but the flag stays exact for sleep/wake-style consumers.
    out.has_influence =
        best_sq < radii.low_radius_m * radii.low_radius_m;
    return out;
}

bool ActivityGrid::HasPlayerWithin(const mx::map::Rect& bounds, float radius) const noexcept
{
    if (!enabled || cells.empty() || !(radius >= 0.0f)) {
        return false;
    }
    const float cx = (bounds.min_x + bounds.max_x) * 0.5f;
    const float cy = (bounds.min_y + bounds.max_y) * 0.5f;
    // The walked box must cover the whole rect expanded by radius (not just
    // a disc around the center): half-extents plus radius on each axis.
    const float span_x = (bounds.max_x - bounds.min_x) * 0.5f + radius;
    const float span_y = (bounds.max_y - bounds.min_y) * 0.5f + radius;
    const int x0 = static_cast<int>(ClampedCell(cx - span_x));
    const int x1 = static_cast<int>(ClampedCell(cx + span_x));
    const int y0 = static_cast<int>(ClampedCell(cy - span_y));
    const int y1 = static_cast<int>(ClampedCell(cy + span_y));
    const float r_sq = radius * radius;
    for (int cyi = y0; cyi <= y1; ++cyi) {
        for (int cxi = x0; cxi <= x1; ++cxi) {
            const auto& cell =
                cells[static_cast<std::size_t>(cyi) * dim + static_cast<std::uint32_t>(cxi)];
            for (const auto& source : cell.players) {
                const float dx = source.x < bounds.min_x   ? bounds.min_x - source.x
                                 : source.x > bounds.max_x ? source.x - bounds.max_x
                                                           : 0.0f;
                const float dy = source.y < bounds.min_y   ? bounds.min_y - source.y
                                 : source.y > bounds.max_y ? source.y - bounds.max_y
                                                           : 0.0f;
                if (dx * dx + dy * dy <= r_sq) {
                    return true;
                }
            }
        }
    }
    return false;
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
    grid->world_extent_m = config_.world_extent_m;
    current_ = std::move(grid);
}

void SpatialActivityField::Reconfigure(Config config)
{
    std::lock_guard lock(mutex_);
    config_ = config;
    auto grid = std::make_shared<ActivityGrid>();
    grid->enabled = false;
    grid->cell_size_m = config_.cell_size_m > 0.0f ? config_.cell_size_m : kActivityCellSizeMeters;
    grid->world_extent_m = config_.world_extent_m;
    current_ = std::move(grid);
}

std::shared_ptr<const ActivityGrid> SpatialActivityField::Rebuild(const ZoneManager& zones,
                                                                  const ActivityRadii& radii,
                                                                  bool enabled)
{
    const auto rebuild_start = std::chrono::steady_clock::now();
    const float cell = config_.cell_size_m > 0.0f ? config_.cell_size_m : kActivityCellSizeMeters;
    const float extent = config_.world_extent_m > 0.0f ? config_.world_extent_m : 1.0f;
    const auto dim = static_cast<std::uint32_t>(std::ceil(extent / cell));
    auto grid = std::make_shared<ActivityGrid>();
    grid->radii = radii;
    grid->enabled = enabled;
    grid->cell_size_m = cell;
    grid->world_extent_m = extent;
    grid->dim = dim > 0 ? dim : 1;
    grid->cells.resize(static_cast<std::size_t>(grid->dim) * grid->dim);
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
                const std::uint32_t cx = grid->ClampedCell(source.x);
                const std::uint32_t cy = grid->ClampedCell(source.y);
                auto& target = grid->cells[static_cast<std::size_t>(cy) * grid->dim + cx];
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
