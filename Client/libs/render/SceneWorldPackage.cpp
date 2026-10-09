#include "SceneWorldPackage.h"

#include "map/WorldPackage.h"
#include "map/WorldPackageWriter.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <system_error>
#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace {
namespace fs = std::filesystem;
namespace map = mx::map;

bool ExistsAny(const fs::path& path, std::error_code& ec)
{
    const auto status = fs::symlink_status(path, ec);
    if (status.type() == fs::file_type::not_found) {
        ec.clear();
        return false;
    }
    return status.type() != fs::file_type::not_found;
}

class OwnedStagingDirectory
{
public:
    explicit OwnedStagingDirectory(fs::path path) : path_(std::move(path)), parent_(path_.parent_path()) {}
    ~OwnedStagingDirectory()
    {
        // Only a directory exclusively created by this operation can be
        // removed. The checked absolute path stays directly under its parent.
        if (owned_ && path_.is_absolute() && path_.parent_path() == parent_ &&
            !path_.filename().empty() && path_.filename() != "." && path_.filename() != "..") {
            std::error_code ec;
            fs::remove_all(path_, ec);
        }
    }
    void Own() noexcept { owned_ = true; }
    void Published() noexcept { owned_ = false; }
private:
    fs::path path_, parent_;
    bool owned_ = false;
};

bool PublishDirectoryNoReplace(const fs::path& source, const fs::path& target, std::string& error)
{
#if defined(_WIN32)
    if (MoveFileExW(source.c_str(), target.c_str(), 0)) return true;
    error = "cannot publish the validated package without replacing an existing target (Windows error " +
        std::to_string(GetLastError()) + ")";
#elif defined(__linux__) && defined(SYS_renameat2)
    constexpr unsigned no_replace = 1; // RENAME_NOREPLACE
    if (::syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, target.c_str(), no_replace) == 0) return true;
    error = "cannot publish the validated package without replacement: " +
        std::error_code(errno, std::generic_category()).message();
#else
    (void)source; (void)target;
    error = "this platform has no supported atomic directory publication without replacement";
#endif
    return false;
}

bool DimensionMatches(float size, std::uint32_t cells, float cell_size)
{
    // This is the same float product used by the engine's authored grid;
    // a relative tolerance could silently accept centimetres of drift.
    return std::isfinite(size) && size > 0 && size == static_cast<float>(cells) * cell_size;
}

} // namespace

bool ExportSceneServerWorld(const std::filesystem::path& path,
    const SceneData& scene,
    const LayerCollisionGeometryProvider& geometryProvider,
    const SceneWorldExportOptions& options,
    SceneWorldPackageResult& result)
{
    result = {};
    const auto fail = [&](const std::string& message) {
        result.errors.push_back(message);
        return false;
    };
    try {
        if (path.empty() || path.filename().empty() || path.filename() == "." || path.filename() == "..")
            return fail("select a new named directory for the server world package");
        if (options.worldId.empty() || options.worldId.size() > 64 ||
            std::any_of(options.worldId.begin(), options.worldId.end(), [](unsigned char c) {
                return !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-');
            })) return fail("world id must contain one to 64 ASCII letters, digits, underscore, dot or dash");
        const auto& terrain = scene.terrain;
        if (!terrain.exists) return fail("a real authored terrain is required; no synthetic terrain fallback");
        if (terrain.cellsX == 0 || terrain.cellsZ == 0 || terrain.cellsX > map::kMaxWorldSizeCells ||
            terrain.cellsZ > map::kMaxWorldSizeCells || !std::isfinite(terrain.cellSizeMeters) ||
            terrain.cellSizeMeters <= 0 || terrain.chunkSizeCells == 0 || terrain.chunkSizeCells > map::kMaxChunkSizeCells)
            return fail("terrain cells, cell size and chunk size must satisfy the bounded server format");
        if (!DimensionMatches(terrain.widthMeters, terrain.cellsX, terrain.cellSizeMeters) ||
            !DimensionMatches(terrain.depthMeters, terrain.cellsZ, terrain.cellSizeMeters))
            return fail("terrain width/depth must match the authored cell grid and cell size");
        const auto sample_count = (static_cast<std::uint64_t>(terrain.cellsX) + 1) *
                                  (static_cast<std::uint64_t>(terrain.cellsZ) + 1);
        const auto cell_count = static_cast<std::uint64_t>(terrain.cellsX) * terrain.cellsZ;
        const auto chunk_count = ((static_cast<std::uint64_t>(terrain.cellsX) + terrain.chunkSizeCells - 1) / terrain.chunkSizeCells) *
                                 ((static_cast<std::uint64_t>(terrain.cellsZ) + terrain.chunkSizeCells - 1) / terrain.chunkSizeCells);
        if (sample_count > kMaxSceneExportTerrainVertices || chunk_count > map::kMaxChunkCount ||
            terrain.heightCmGrid.size() != sample_count)
            return fail("terrain requires its exact bounded height grid; missing samples are not ground at zero");
        if (!terrain.attributes.empty() && terrain.attributes.size() != cell_count)
            return fail("terrain attributes must be empty or contain exactly one record per source cell");
        if (std::any_of(terrain.attributes.begin(), terrain.attributes.end(), [](std::uint16_t bits) {
            return (bits & map::kAttributeReservedMask) != 0;
        })) return fail("terrain attributes use unsupported reserved bits; only blocked bit 0 is defined");
        const float half_width = terrain.widthMeters * 0.5f, half_depth = terrain.depthMeters * 0.5f;
        result.worldBounds = {-half_width, -half_depth, half_width, half_depth};
        if (half_width > map::kMaxWorldCoordinate || half_depth > map::kMaxWorldCoordinate)
            return fail("terrain world bounds exceed the server coordinate limit");
        for (std::uint32_t sample = 0; sample <= terrain.cellsX; ++sample) {
            const double server_position = -static_cast<double>(half_width) +
                static_cast<double>(sample) * terrain.cellSizeMeters;
            const float physical_position = -half_width + static_cast<float>(sample) * terrain.cellSizeMeters;
            result.maxGridPositionErrorMeters = std::max(result.maxGridPositionErrorMeters,
                std::abs(server_position - physical_position));
        }
        for (std::uint32_t sample = 0; sample <= terrain.cellsZ; ++sample) {
            const double server_position = -static_cast<double>(half_depth) +
                static_cast<double>(sample) * terrain.cellSizeMeters;
            const float physical_position = half_depth - static_cast<float>(terrain.cellsZ - sample) * terrain.cellSizeMeters;
            result.maxGridPositionErrorMeters = std::max(result.maxGridPositionErrorMeters,
                std::abs(server_position - physical_position));
        }
        if (!std::isfinite(options.spawnX) || !std::isfinite(options.spawnZ) ||
            !result.worldBounds.ContainsHalfOpen(options.spawnX, options.spawnZ))
            return fail("the explicit spawn must be a finite point inside the authored terrain");
        // Cell membership (including fractional-grid seam correction) belongs
        // to ServerTerrain. Full strict validation below decides walkability;
        // no duplicate approximate floor/division can move the authored spawn.
        const float spawn_half_size = std::min({terrain.cellSizeMeters * 0.25f,
            (options.spawnX + half_width) * 0.5f, (half_width - options.spawnX) * 0.5f,
            (options.spawnZ + half_depth) * 0.5f, (half_depth - options.spawnZ) * 0.5f});
        const map::Rect spawn_bounds{options.spawnX - spawn_half_size, options.spawnZ - spawn_half_size,
                                     options.spawnX + spawn_half_size, options.spawnZ + spawn_half_size};
        if (!(spawn_half_size > 0 && spawn_bounds.min_x < spawn_bounds.max_x && spawn_bounds.min_y < spawn_bounds.max_y) ||
            spawn_bounds.CenterX() != options.spawnX || spawn_bounds.CenterY() != options.spawnZ)
            return fail("spawn needs a representable centred region inset from the outer terrain edge");

        std::vector<std::int32_t> heights;
        heights.reserve(terrain.heightCmGrid.size());
        for (const float centimetres : terrain.heightCmGrid) {
            if (!std::isfinite(centimetres)) return fail("terrain height grid contains non-finite values");
            const double raw = std::round(static_cast<double>(centimetres) * 100.0);
            if (raw < std::numeric_limits<std::int32_t>::min() || raw > std::numeric_limits<std::int32_t>::max())
                return fail("terrain height exceeds the explicit int32 range at 0.0001 metre precision; values are never clamped");
            heights.push_back(static_cast<std::int32_t>(raw));
            result.maxHeightErrorMeters = std::max(result.maxHeightErrorMeters,
                std::abs(raw * 0.0001 - static_cast<double>(centimetres) * 0.01));
        }
        const bool have_layers = !scene.waterBodies.empty() || std::any_of(scene.meshEntities.begin(), scene.meshEntities.end(),
            [](const MeshSceneEntity& entity) { return entity.layerAuthoring.enabled; });
        if (have_layers && !GenerateSceneLayers(scene, geometryProvider, result.layers)) {
            result.errors = result.layers.errors;
            return false;
        }
        if (options.spawnVolumeId != 0 && (!have_layers || !result.layers.world.clearance_profile))
            return fail("a layered spawn volume needs generated layers with a clearance bake");

        map::PackageWriteSpec spec;
        spec.world_id = options.worldId;
        spec.world_name = scene.name;
        spec.size_cells_x = terrain.cellsX;
        spec.size_cells_y = terrain.cellsZ;
        spec.origin_x = -static_cast<double>(half_width);
        spec.origin_y = -static_cast<double>(half_depth);
        spec.cell_size_m = terrain.cellSizeMeters;
        spec.chunk_size_cells = terrain.chunkSizeCells;
        spec.height_encoding = map::HeightEncoding{3, true, 0.0001, 0.0, map::HeightInterpolation::TriangleMainDiagonal};
        spec.height_raw = [&](std::uint32_t x, std::uint32_t y) {
            return heights[static_cast<std::size_t>(terrain.cellsZ - y) * (terrain.cellsX + 1u) + x];
        };
        spec.attributes = [&](std::uint32_t x, std::uint32_t y) {
            return terrain.attributes.empty() ? std::uint16_t{0} :
                terrain.attributes[static_cast<std::size_t>(terrain.cellsZ - 1u - y) * terrain.cellsX + x];
        };
        spec.logic.spawns.push_back(map::SpawnRegion{1, 0, spawn_bounds, options.spawnVolumeId});
        spec.water_model = scene.waterBodies.empty() ? map::WaterModel::None : map::WaterModel::Bodies;
        for (const auto& water : scene.waterBodies) {
            spec.water_bodies.push_back({water.id,
                map::Rect{water.bboxMin[0], water.bboxMin[1], water.bboxMax[0], water.bboxMax[1]}, water.waterLevelY});
        }
        if (have_layers) spec.layered_world = result.layers.world;

        std::error_code ec;
        const auto final_path = fs::absolute(path, ec).lexically_normal();
        if (ec || !final_path.is_absolute() || final_path.filename().empty())
            return fail("cannot resolve the requested package directory");
        auto staging_path = final_path;
        staging_path += ".pending";
        if (ExistsAny(final_path, ec) || ec || ExistsAny(staging_path, ec) || ec)
            return fail("refusing to replace an existing final or staging package path");
        fs::create_directories(final_path.parent_path(), ec);
        if (ec) return fail("cannot create the package parent directory: " + ec.message());
        OwnedStagingDirectory staging(staging_path);
        if (!fs::create_directory(staging_path, ec) || ec)
            return fail("cannot exclusively create a fresh staging package directory");
        staging.Own();
        const auto written = map::WritePackage(staging_path, spec);
        if (!written.ok) return fail("server package write failed: " + written.error);
        map::PackageReport report;
        map::LoadOptions load_options;
        load_options.depth = map::ValidationDepth::Full;
        load_options.residency = map::ResidencyMode::Eager;
        load_options.warp_policy = map::WarpPolicy::Strict;
        const auto loaded = map::LoadServerWorld(staging_path, load_options, report);
        if (!loaded || !report.Ok()) {
            if (const auto* issue = report.FirstError()) return fail("full strict package validation failed: " + issue->Format());
            return fail("full strict package validation failed without a usable world");
        }
        if (options.spawnVolumeId != 0) {
            // 3D-5B: the exact authored point on the chosen volume, for the
            // package's own baked actor (the loader already proved it too).
            const auto& layered = loaded->layered_world;
            if (!layered || !layered->clearance_profile ||
                !map::ResolveLayerActorPlacement(*layered,
                    map::LayerActorProfile{layered->clearance_profile->actor_radius_m,
                                           layered->clearance_profile->actor_height_m},
                    options.spawnVolumeId, options.spawnX, options.spawnZ).Ok())
                return fail("validated server package cannot place the baked actor at the exact authored layered spawn");
        } else if (!loaded->terrain.Cell(options.spawnX, options.spawnZ).Walkable() ||
            !loaded->terrain.Height(options.spawnX, options.spawnZ).Ok())
            return fail("validated server package cannot support the exact authored spawn");
        std::string publication_error;
        if (!PublishDirectoryNoReplace(staging_path, final_path, publication_error)) return fail(publication_error);
        staging.Published();
        result.files = written.files;
        return true;
    } catch (const std::exception& exception) {
        return fail("server world export failed: " + std::string(exception.what()));
    }
}
