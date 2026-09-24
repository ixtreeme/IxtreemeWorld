#pragma once

#include <cstdio>
#include <cstdlib>
#include <utility>

#include "../world/package/WorldPackageLoader.h"

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

} // namespace gs::bench
