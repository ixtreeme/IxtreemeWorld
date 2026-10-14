#include "RegionDefinition.h"

#include <cmath>
#include <sstream>

namespace gs::game {
namespace {

// Grid line i of n over [min, max): exact at both ends (no accumulated
// rounding), so neighbouring cells share their edge bit for bit.
float GridLine(float min, float max, std::uint32_t i, std::uint32_t n)
{
    if (i == 0) {
        return min;
    }
    if (i == n) {
        return max;
    }
    return static_cast<float>(static_cast<double>(min) +
                              (static_cast<double>(max) - min) * static_cast<double>(i) / static_cast<double>(n));
}

std::string RectText(const mx::map::Rect& r)
{
    std::ostringstream out;
    out << "(" << r.min_x << "," << r.min_y << ")-(" << r.max_x << "," << r.max_y << ")";
    return out.str();
}

} // namespace

std::string RegionName(std::uint32_t ix, std::uint32_t iy, std::uint32_t nx, std::uint32_t ny)
{
    if (nx == 1 && ny == 1) {
        return "World";
    }
    if (nx == 2 && ny == 1) {
        return ix == 0 ? "West" : "East";
    }
    if (nx == 1 && ny == 2) {
        return iy == 0 ? "South" : "North";
    }
    if (nx == 2 && ny == 2) {
        return std::string(iy == 0 ? "South" : "North") + (ix == 0 ? "West" : "East");
    }
    return "R" + std::to_string(ix) + "_" + std::to_string(iy);
}

bool BuildInitialPartition(const WorldBounds& bounds,
                           const PartitionLayout& layout,
                           float min_leaf_m,
                           InitialPartition& out,
                           std::string& error)
{
    out = InitialPartition{};
    if (!bounds.IsValid() || !std::isfinite(bounds.min_x) || !std::isfinite(bounds.max_x) ||
        !std::isfinite(bounds.min_y) || !std::isfinite(bounds.max_y)) {
        error = "invalid world bounds";
        return false;
    }
    const mx::map::Rect world{bounds.min_x, bounds.min_y, bounds.max_x, bounds.max_y};

    if (!layout.explicit_leaves.empty()) {
        // Explicit bootstrap description: one region = the world, tiled
        // exactly by the given leaves (disjoint half-open + full coverage).
        if (layout.explicit_leaves.size() > kMaxInitialLeafZones) {
            error = std::to_string(layout.explicit_leaves.size()) + " explicit initial leaves exceed the bootstrap cap of " +
                    std::to_string(kMaxInitialLeafZones) + " zones";
            return false;
        }
        RegionDefinition region;
        region.id = 1;
        region.name = "World";
        region.bounds = world;
        out.regions.push_back(region);
        double area = 0.0;
        for (std::size_t i = 0; i < layout.explicit_leaves.size(); ++i) {
            const auto& r = layout.explicit_leaves[i];
            if (!(r.min_x < r.max_x) || !(r.min_y < r.max_y) || r.min_x < world.min_x || r.min_y < world.min_y ||
                r.max_x > world.max_x || r.max_y > world.max_y) {
                error = "explicit leaf " + std::to_string(i) + " " + RectText(r) + " is empty or outside the world " +
                        RectText(world);
                return false;
            }
            if (r.max_x - r.min_x < min_leaf_m || r.max_y - r.min_y < min_leaf_m) {
                error = "explicit leaf " + std::to_string(i) + " " + RectText(r) + " is narrower than the " +
                        std::to_string(min_leaf_m) + " m safety floor";
                return false;
            }
            for (std::size_t j = 0; j < i; ++j) {
                const auto& o = layout.explicit_leaves[j];
                if (r.min_x < o.max_x && o.min_x < r.max_x && r.min_y < o.max_y && o.min_y < r.max_y) {
                    error = "explicit leaves " + std::to_string(j) + " and " + std::to_string(i) + " overlap";
                    return false;
                }
            }
            area += (static_cast<double>(r.max_x) - r.min_x) * (static_cast<double>(r.max_y) - r.min_y);
            out.leaves.push_back(InitialLeaf{1, "World." + std::to_string(i), r});
        }
        const double world_area =
            (static_cast<double>(world.max_x) - world.min_x) * (static_cast<double>(world.max_y) - world.min_y);
        if (std::abs(area - world_area) > world_area * 1e-6) {
            error = "explicit leaves cover " + std::to_string(area) + " of " + std::to_string(world_area) +
                    " m2 (gap)";
            return false;
        }
        return true;
    }

    if (layout.regions_x == 0 || layout.regions_y == 0 || layout.leaves_x == 0 || layout.leaves_y == 0 ||
        layout.regions_x > 16 || layout.regions_y > 16 || layout.leaves_x > 64 || layout.leaves_y > 64) {
        error = "partition layout needs 1..16 regions and 1..64 initial leaves per region per axis";
        return false;
    }
    const std::uint32_t total_x = layout.regions_x * layout.leaves_x;
    const std::uint32_t total_y = layout.regions_y * layout.leaves_y;
    // The aggregate first: a config-only property, independent of the world.
    const std::uint64_t total = static_cast<std::uint64_t>(total_x) * total_y;
    if (total > kMaxInitialLeafZones) {
        error = "initial partition " + std::to_string(layout.regions_x) + "x" + std::to_string(layout.regions_y) +
                " regions x " + std::to_string(layout.leaves_x) + "x" + std::to_string(layout.leaves_y) +
                " leaves = " + std::to_string(total) + " initial zones, above the bootstrap cap of " +
                std::to_string(kMaxInitialLeafZones) + " (finer partitions come from runtime splits)";
        return false;
    }
    const float leaf_w = (world.max_x - world.min_x) / static_cast<float>(total_x);
    const float leaf_h = (world.max_y - world.min_y) / static_cast<float>(total_y);
    if (leaf_w < min_leaf_m || leaf_h < min_leaf_m) {
        std::ostringstream message;
        message << "initial leaves would be " << leaf_w << " x " << leaf_h << " m (" << total_x << "x" << total_y
                << " over " << RectText(world) << "), below the " << min_leaf_m << " m safety floor (2x AOI radius)";
        error = message.str();
        return false;
    }
    // Leaf grid lines over the whole world; region edges are every
    // leaves_x-th / leaves_y-th line, so leaves nest inside regions exactly.
    RegionId next_region = 1;
    for (std::uint32_t ry = 0; ry < layout.regions_y; ++ry) {
        for (std::uint32_t rx = 0; rx < layout.regions_x; ++rx) {
            RegionDefinition region;
            region.id = next_region++;
            region.name = RegionName(rx, ry, layout.regions_x, layout.regions_y);
            region.bounds = {GridLine(world.min_x, world.max_x, rx * layout.leaves_x, total_x),
                             GridLine(world.min_y, world.max_y, ry * layout.leaves_y, total_y),
                             GridLine(world.min_x, world.max_x, (rx + 1) * layout.leaves_x, total_x),
                             GridLine(world.min_y, world.max_y, (ry + 1) * layout.leaves_y, total_y)};
            out.regions.push_back(region);
            for (std::uint32_t ly = 0; ly < layout.leaves_y; ++ly) {
                for (std::uint32_t lx = 0; lx < layout.leaves_x; ++lx) {
                    const std::uint32_t gx = rx * layout.leaves_x + lx;
                    const std::uint32_t gy = ry * layout.leaves_y + ly;
                    InitialLeaf leaf;
                    leaf.region = region.id;
                    leaf.name = region.name + "." + std::to_string(lx) + "_" + std::to_string(ly);
                    leaf.bounds = {GridLine(world.min_x, world.max_x, gx, total_x),
                                   GridLine(world.min_y, world.max_y, gy, total_y),
                                   GridLine(world.min_x, world.max_x, gx + 1, total_x),
                                   GridLine(world.min_y, world.max_y, gy + 1, total_y)};
                    out.leaves.push_back(std::move(leaf));
                }
            }
        }
    }
    return true;
}

} // namespace gs::game
