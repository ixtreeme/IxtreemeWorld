#pragma once

#include <cstdio>
#include <cstdlib>
#include <utility>

#include "../world/package/WorldPackageLoader.h"
#include "../world/partition/RegionDefinition.h"

// MAP-1: benches that used the old implicit map-root constructor now load
// the checked-in test map through the same strict package loader as the
// production server. The paths are bench-only compile definitions
// (IXTREEME_TEST_MAP_ROOT / IXTREEME_DEFAULT_MOB_TYPES_CONFIG); the
// gameserver target has no compile-time world path at all.
namespace gs::bench {

inline gs::game::LoadedWorld LoadBenchTestWorld()
{
    mx::map::PackageReport report;
    gs::game::WorldLoadRequest request;
    // This immutable v2 test_zone contains an acyclic warp chain. Explicit
    // compatibility for this benchmark fixture; server default stays strict.
    request.warp_policy = mx::map::WarpPolicy::Legacy;
    static const bool logged_policy = [] {
        std::printf("BENCH test_zone fixture-policy=explicit-legacy (immutable checked-in chain)\n");
        return true;
    }();
    (void)logged_policy;
    request.package_root = IXTREEME_TEST_MAP_ROOT;
    request.mob_types_config = IXTREEME_DEFAULT_MOB_TYPES_CONFIG;
    auto world = gs::game::LoadWorldPackage(request, report);
    if (!world) {
        std::fprintf(stderr, "FATAL bench test map %s rejected:\n", IXTREEME_TEST_MAP_ROOT);
        for (const auto& issue : report.issues) {
            std::fprintf(stderr, "  %s\n", issue.Format().c_str());
        }
        std::fflush(stderr);
        std::abort();
    }
    return std::move(*world);
}

// Explicit bootstrap description for the test map benches (MAP-2): the three
// initial leaves the older WorldBench scenarios were written against (zone ids
// 1, 2, 3 in this order). Given by the bench -- not derived from the map's
// areas, which are metadata since MAP-2.
inline gs::game::PartitionLayout LegacyTestMapLayout()
{
    gs::game::PartitionLayout layout;
    layout.explicit_leaves = {
        mx::map::Rect{0.0f, 0.0f, 500.0f, 500.0f},     // id 1 (south-west)
        mx::map::Rect{0.0f, 500.0f, 500.0f, 1000.0f},  // id 2 (north-west)
        mx::map::Rect{500.0f, 0.0f, 1000.0f, 1000.0f}, // id 3 (east half)
    };
    return layout;
}

} // namespace gs::bench
