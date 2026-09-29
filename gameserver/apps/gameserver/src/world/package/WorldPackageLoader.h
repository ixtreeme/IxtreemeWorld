#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "map/WorldPackage.h"

#include "../spawn/SpawnLoader.h"

// MAP-1: turns a world package into a validated, fully built world before
// any runtime object exists. The package is checked by the shared strict
// loader (mx::map::LoadServerWorld); the server adds its own startup data
// rules (mob type registry present, every spawn line names a known type).
// Nothing here falls back: a failure is a structured report, the caller
// refuses to start.
namespace gs::game {

struct LoadedWorld {
    mx::map::ServerTerrain terrain; // per-chunk heights + attributes only
    // Eager: every chunk published. Streaming: only the startup set; the
    // runtime's terrain streamer loads the rest through `chunk_source`.
    mx::map::ResidencyMode residency = mx::map::ResidencyMode::Eager;
    std::shared_ptr<const mx::map::ChunkSource> chunk_source;
    std::vector<std::uint32_t> startup_chunks;
    mx::map::ServerWater water; // declared water capability (MAP-3)
    mx::map::WorldLogic logic;
    std::vector<MobSpawnPoint> spawn_points;
    std::string mob_types_config; // absolute path, loaded and checked
    std::size_t mob_type_count = 0;
    mx::map::PackageReport report; // what was loaded (startup summary)
};

struct WorldLoadRequest {
    std::filesystem::path package_root;
    std::filesystem::path mob_types_config;
    mx::map::ValidationDepth depth = mx::map::ValidationDepth::Startup;
    mx::map::ResidencyMode residency = mx::map::ResidencyMode::Eager;
    mx::map::WarpPolicy warp_policy = mx::map::WarpPolicy::Strict;
};

// nullopt <=> report has an Error issue. `report` always describes the attempt.
std::optional<LoadedWorld> LoadWorldPackage(const WorldLoadRequest& request, mx::map::PackageReport& report);

// Every issue to the log (ERROR / WARN / INFO), one line each.
void LogPackageReport(const mx::map::PackageReport& report);

} // namespace gs::game
