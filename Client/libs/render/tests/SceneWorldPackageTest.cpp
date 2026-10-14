#include "SceneWorldPackage.h"
#include "TerrainRenderer.h"
#include "map/WorldPackage.h"
#include "physics/PhysicsWorld.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <latch>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {
namespace fs = std::filesystem;
namespace map = mx::map;
namespace physics = ixtreeme::physics;
int checks = 0, failures = 0;

void Check(const char* name, bool value)
{
    ++checks;
    if (!value) ++failures;
    std::cout << "SCENE WORLD PACKAGE " << name << ": " << (value ? "PASS" : "FAIL") << '\n';
}

bool Near(double actual, double expected, double tolerance = 0.0002)
{
    return std::isfinite(actual) && std::abs(actual - expected) <= tolerance;
}

class TemporaryWorkspace
{
public:
    TemporaryWorkspace()
    {
        parent_ = fs::weakly_canonical(fs::temp_directory_path());
        root = parent_ / ("ixw_scene_world_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        if (!fs::create_directory(root)) throw std::runtime_error("cannot create unique test workspace");
    }
    ~TemporaryWorkspace()
    {
        if (root.parent_path() == parent_ && root.filename().string().starts_with("ixw_scene_world_test_")) {
            std::error_code ec;
            fs::remove_all(root, ec);
        }
    }
    fs::path root;
private:
    fs::path parent_;
};

std::vector<std::uint8_t> ReadBytes(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void WriteBytes(const fs::path& path, const std::vector<std::uint8_t>& bytes)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream) throw std::runtime_error("cannot write test bytes");
}

SceneData Fixture(bool layers = true)
{
    SceneData scene;
    scene.name = "Asymmetric non-square engine world";
    auto& terrain = scene.terrain;
    terrain.exists = true;
    terrain.cellsX = 6;
    terrain.cellsZ = 4;
    terrain.cellSizeMeters = 2;
    terrain.widthMeters = 12;
    terrain.depthMeters = 8;
    terrain.chunkSizeCells = 3;
    for (std::uint32_t row = 0; row <= terrain.cellsZ; ++row) {
        for (std::uint32_t column = 0; column <= terrain.cellsX; ++column) {
            terrain.heightCmGrid.push_back(-700.0f + 23.0f * column + 71.0f * row);
        }
    }
    terrain.attributes.assign(24, 0);
    terrain.attributes[0] = map::CellSample::kBlocked; // NORTH-west source cell
    terrain.attributes[2 * terrain.cellsX + 4] = map::CellSample::kBlocked;
    if (layers) {
        MeshSceneEntity bridge;
        bridge.id = 10;
        bridge.name = "physical-bridge-top";
        bridge.position[0] = -3;
        bridge.position[1] = 5.5f;
        bridge.position[2] = -1;
        bridge.hasCollider = true;
        bridge.collider.shape = physics::ColliderShape::Box;
        bridge.collider.layer = physics::PhysicsLayer::StaticWorld;
        bridge.collider.size[0] = 2;
        bridge.collider.size[1] = 1;
        bridge.collider.size[2] = 2;
        bridge.layerAuthoring = {true, map::VolumeTagBridge | map::VolumeTagConnector};
        scene.meshEntities.push_back(bridge);
        WaterBody water;
        water.id = 7;
        water.name = "authored-rectangular-water";
        water.bboxMin[0] = 2;
        water.bboxMin[1] = -3;
        water.bboxMax[0] = 5;
        water.bboxMax[1] = 0;
        water.waterLevelY = -1;
        water.maskWidth = 2;
        water.maskHeight = 3;
        water.shapeMask.assign(6, 255);
        scene.waterBodies.push_back(water);
    }
    return scene;
}

SceneWorldExportOptions Options()
{
    return {"asymmetric_engine_world", -5, -3};
}

// Independent physical triangle oracle; it does not call server interpolation
// or the renderer helper. Source NW-to-SE rows use the v10-v01 diagonal.
double IndependentHeight(const TerrainSceneData& terrain, double x, double y)
{
    const double source_x = (x + terrain.widthMeters * 0.5) / terrain.cellSizeMeters;
    const double source_y = (terrain.depthMeters * 0.5 - y) / terrain.cellSizeMeters;
    const auto column = static_cast<std::uint32_t>(std::floor(source_x));
    const auto row = static_cast<std::uint32_t>(std::floor(source_y));
    const double u = source_x - column, v = source_y - row;
    const auto at = [&](std::uint32_t ix, std::uint32_t iy) {
        return static_cast<double>(terrain.heightCmGrid[static_cast<std::size_t>(iy) * (terrain.cellsX + 1) + ix]) * 0.01;
    };
    const double h00 = at(column, row), h10 = at(column + 1, row);
    const double h01 = at(column, row + 1), h11 = at(column + 1, row + 1);
    return u + v <= 1 ? h00 * (1 - u - v) + h10 * u + h01 * v :
        h10 * (1 - v) + h01 * (1 - u) + h11 * (u + v - 1);
}

void TestFullExport(const TemporaryWorkspace& workspace)
{
    const SceneData scene = Fixture();
    SceneWorldPackageResult result;
    const auto path = workspace.root / "full_package";
    const bool exported = ExportSceneServerWorld(path, scene, {}, Options(), result);
    Check("full-package-exported-and-published", exported && result.errors.empty() && !result.files.empty());
    if (!exported) {
        for (const auto& error : result.errors) std::cout << "export error: " << error << '\n';
        return;
    }
    Check("authoring-bounds-use-real-negative-origin", result.worldBounds.min_x == -6 && result.worldBounds.min_y == -4 &&
        result.worldBounds.max_x == 6 && result.worldBounds.max_y == 4);
    auto pending = path;
    pending += ".pending";
    Check("successful-export-leaves-no-staging-directory", !fs::exists(pending));
    map::PackageReport report;
    const auto loaded = map::LoadServerWorld(path, map::ValidationDepth::Full, report);
    Check("published-package-passes-full-strict-server-loader", loaded && report.Ok());
    if (!loaded) return;
    const auto& geometry = loaded->terrain.Geometry();
    Check("nonsquare-world-and-partial-edge-chunks-retained", geometry.cells_x == 6 && geometry.cells_y == 4 &&
        geometry.chunks_x == 2 && geometry.chunks_y == 2 && geometry.ChunkCellsY(1) == 1);
    Check("explicit-physical-height-layer-contract", loaded->terrain.Encoding().layer_version == 3 &&
        loaded->terrain.Encoding().int32_samples && loaded->terrain.Encoding().meters_per_unit == 0.0001 &&
        loaded->terrain.Encoding().interpolation == map::HeightInterpolation::TriangleMainDiagonal);
    Check("north-source-row-exported-as-server-north-row", Near(loaded->terrain.Vertex(0, 4).meters, -7) &&
        Near(loaded->terrain.Vertex(0, 0).meters, -4.16) && Near(loaded->terrain.Vertex(6, 4).meters, -5.62));
    Check("blocked-attributes-row-reversed-with-cells-not-vertices", !loaded->terrain.Cell(-5, 3).Walkable() &&
        loaded->terrain.Cell(-5, -3).Walkable() && !loaded->terrain.Cell(3, -1).Walkable());
    Check("explicit-spawn-centre-and-height-retained", loaded->logic.spawns.size() == 1 &&
        loaded->logic.spawns[0].bounds.CenterX() == -5 && loaded->logic.spawns[0].bounds.CenterY() == -3 &&
        Near(loaded->terrain.Height(-5, -3).meters, -4.4));
    Check("layer-metadata-remains-identical", loaded->layered_world &&
        map::EncodeLayeredWorld(*loaded->layered_world) == map::EncodeLayeredWorld(result.layers.world));
    const auto sample = loaded->water.Query(3, -2, geometry, loaded->terrain.Height(3, -2));
    Check("actual-server-water-surface-and-depth", sample.IsWater() && sample.surface_m == -1 &&
        Near(sample.depth_m, -1 - IndependentHeight(scene.terrain, 3, -2)));
    const std::array<std::array<float, 2>, 5> points{{{-5,-3}, {-4.6f,2.2f}, {-0.5f,-0.25f}, {0.25f,1.75f}, {5,-2.25f}}};
    bool all_heights = true;
    for (const auto& point : points) {
        const auto server = loaded->terrain.Height(point[0], point[1]);
        const double independent = IndependentHeight(scene.terrain, point[0], point[1]);
        const float client = SampleTerrainCollisionHeightCm(scene.terrain.heightCmGrid, 7, 5,
            (point[0] + 6) * 100, (4 - point[1]) * 100, 200) * 0.01f;
        all_heights = all_heights && server.Ok() && Near(server.meters, independent) && Near(client, independent);
    }
    Check("asymmetric-independent-client-server-height-oracle", all_heights);
#if defined(IXENGINE_PHYSICS_WITH_JOLT)
    physics::PhysicsWorld physical_world;
    physics::TerrainColliderDesc descriptor;
    descriptor.widthMeters = 12;
    descriptor.depthMeters = 8;
    descriptor.cellSizeMeters = 2;
    descriptor.cellsX = 6;
    descriptor.cellsZ = 4;
    descriptor.heightCmGrid = scene.terrain.heightCmGrid;
    const auto body = physical_world.CreateTerrainCollider(descriptor);
    bool all_rays = body != 0, all_normals = body != 0;
    const double gradient_x = 0.115, gradient_y = -0.355;
    const double normal_length = std::sqrt(1 + gradient_x * gradient_x + gradient_y * gradient_y);
    for (const auto& point : points) {
        const float origin[3] = {point[0], 10, point[1]}, direction[3] = {0,-1,0};
        physics::PhysicsRaycastHit hit;
        const bool hit_body = physical_world.Raycast(origin, direction, 30, hit) && hit.bodyId == body;
        all_rays = all_rays && hit_body && Near(hit.position[1], IndependentHeight(scene.terrain, point[0], point[1]));
        all_normals = all_normals && hit_body && Near(hit.normal[0], -gradient_x / normal_length) &&
            Near(hit.normal[1], 1 / normal_length) && Near(hit.normal[2], -gradient_y / normal_length);
    }
    Check("actual-jolt-height-matches-export-and-independent-oracle", all_rays);
    Check("actual-jolt-slope-normal-matches-asymmetric-analytic-plane", all_normals);
#else
    std::cout << "SCENE WORLD PACKAGE Jolt oracle: NOT_RUN (backend disabled)\n";
#endif
    Check("server-half-open-outer-edge-preserved", !loaded->terrain.Height(6, 0).Ok() && !loaded->terrain.Height(0, 4).Ok());
}

void TestPrecisionAndNonplanarity(const TemporaryWorkspace& workspace)
{
    auto scene = Fixture(false);
    scene.terrain.attributes.clear();
    scene.terrain.heightCmGrid[0] = -50000.125f;
    scene.terrain.heightCmGrid[1] = -700.003f;
    SceneWorldPackageResult result;
    const auto path = workspace.root / "fractional_depth_package";
    const bool exported = ExportSceneServerWorld(path, scene, {}, Options(), result);
    Check("fractional-cm-and-deep-int32-terrain-export", exported && result.maxHeightErrorMeters <= 0.00005000001);
    map::PackageReport report;
    const auto loaded = exported ? map::LoadServerWorld(path, map::ValidationDepth::Full, report) : std::nullopt;
    Check("large-negative-height-is-not-int16-clamped", loaded && Near(loaded->terrain.Vertex(0, 4).meters, -500.00125));
    Check("fractional-cm-preserved-with-declared-quantization-bound", loaded &&
        Near(loaded->terrain.Vertex(1, 4).meters, static_cast<double>(scene.terrain.heightCmGrid[1]) * 0.01, 0.00006));
    Check("empty-attributes-retain-authored-walkable-default", loaded && loaded->terrain.Cell(-5, 3).Walkable());
    Check("no-layer-sources-produce-no-invented-layer-sidecar", loaded && !loaded->layered_world &&
        !fs::exists(path / "layered_world.mx3d"));
    Check("explicit-dry-world-does-not-mean-unknown-water", loaded && loaded->water.model == map::WaterModel::None);

    scene = Fixture(false);
    scene.terrain.attributes.clear();
    std::fill(scene.terrain.heightCmGrid.begin(), scene.terrain.heightCmGrid.end(), 0.0f);
    scene.terrain.heightCmGrid[1] = 400; // canonical NE of north-west cell
    const auto saddle_path = workspace.root / "physical_nonplanar_package";
    const bool saddle_exported = ExportSceneServerWorld(saddle_path, scene, {}, Options(), result);
    const auto saddle = saddle_exported ? map::LoadServerWorld(saddle_path, map::ValidationDepth::Full, report) : std::nullopt;
    Check("nonplanar-cell-uses-physical-main-diagonal", saddle && Near(saddle->terrain.Height(-5,3).meters, 2) &&
        Near(SampleTerrainCollisionHeightCm(scene.terrain.heightCmGrid, 7, 5, 100,100,200) * 0.01, 2));
    Check("nonplanar-oracle-distinguishes-legacy-bilinear", saddle &&
        !Near(saddle->terrain.Height(-5,3).meters, 1));
#if defined(IXENGINE_PHYSICS_WITH_JOLT)
    physics::PhysicsWorld physical_world;
    physics::TerrainColliderDesc descriptor;
    descriptor.widthMeters = 12;
    descriptor.depthMeters = 8;
    descriptor.cellSizeMeters = 2;
    descriptor.cellsX = 6;
    descriptor.cellsZ = 4;
    descriptor.heightCmGrid = scene.terrain.heightCmGrid;
    const auto body = physical_world.CreateTerrainCollider(descriptor);
    const float origin[3] = {-5,10,3}, direction[3] = {0,-1,0};
    physics::PhysicsRaycastHit hit;
    Check("nonplanar-jolt-raycast-proves-two-metre-height", body != 0 && physical_world.Raycast(origin,direction,30,hit) &&
        hit.bodyId == body && Near(hit.position[1],2));
#endif
}

void Rejected(const char* name, const fs::path& path, const SceneData& scene,
              const SceneWorldExportOptions& options = Options())
{
    SceneWorldPackageResult result;
    const bool exported = ExportSceneServerWorld(path, scene, {}, options, result);
    auto pending = path;
    pending += ".pending";
    Check(name, !exported && !result.errors.empty() && result.files.empty() && !fs::exists(path) && !fs::exists(pending));
}

std::uint32_t WorldLogicVersion(const fs::path& package)
{
    std::ifstream stream(package / "worldlogic.dat", std::ios::binary);
    std::array<unsigned char, 8> header{};
    stream.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    return stream ? static_cast<std::uint32_t>(header[4]) | static_cast<std::uint32_t>(header[5]) << 8 |
                        static_cast<std::uint32_t>(header[6]) << 16 | static_cast<std::uint32_t>(header[7]) << 24
                  : 0;
}

// 3D-5B: the player spawn region may stand on a generated layer volume.
void TestLayeredSpawn(const TemporaryWorkspace& workspace)
{
    auto scene = Fixture(true);
    // The terrain cell under the bridge deck is blocked: a terrain spawn there
    // is refused, a spawn on the deck above it is valid.
    scene.terrain.attributes[2 * scene.terrain.cellsX + 1] = map::CellSample::kBlocked;
    SceneLayerAuthoringResult preview;
    const bool generated = GenerateSceneLayers(scene, {}, preview);
    const auto bridge = std::find_if(preview.world.volumes.begin(), preview.world.volumes.end(),
        [](const auto& volume) { return (volume.tags & map::VolumeTagBridge) != 0; });
    Check("layered-spawn-fixture-has-baked-bridge", generated && bridge != preview.world.volumes.end() &&
        preview.world.clearance_profile.has_value());
    if (!generated || bridge == preview.world.volumes.end()) return;

    SceneWorldExportOptions options{"layered_spawn_world", -3, -1, bridge->id};
    const auto path = workspace.root / "layered_spawn";
    SceneWorldPackageResult result;
    const bool exported = ExportSceneServerWorld(path, scene, {}, options, result);
    if (!exported)
        for (const auto& error : result.errors) std::cout << "layered spawn export error: " << error << '\n';
    map::PackageReport report;
    const auto loaded = map::LoadServerWorld(path, map::ValidationDepth::Full, report);
    Check("layered-spawn-export-passes-strict-loader", exported && loaded && report.Ok());
    Check("layered-spawn-region-keeps-its-volume", loaded && loaded->logic.spawns.size() == 1 &&
        loaded->logic.spawns[0].volume_id == bridge->id && loaded->logic.spawns[0].bounds.CenterX() == -3.0f &&
        loaded->logic.spawns[0].bounds.CenterY() == -1.0f);
    Check("layered-spawn-writes-worldlogic-v2", WorldLogicVersion(path) == map::kWorldLogicLayeredFileVersion);
    Check("layered-spawn-over-blocked-terrain-cell-is-valid", loaded && !loaded->terrain.Cell(-3, -1).Walkable());
    const auto placed = loaded && loaded->layered_world
        ? map::ResolveLayerActorPlacement(*loaded->layered_world, SceneLayerActorProfile(), bridge->id, -3, -1)
        : map::LayerGroundResult{};
    Check("layered-spawn-places-baked-actor-on-the-deck", placed.Ok() && Near(placed.state.z, 6));

    Rejected("terrain-spawn-on-blocked-cell-still-rejected", workspace.root / "terrain_blocked_spawn", scene,
             SceneWorldExportOptions{"terrain_blocked_spawn", -3, -1, 0});
    Rejected("unknown-spawn-volume-rejected", workspace.root / "unknown_spawn_volume", scene,
             SceneWorldExportOptions{"unknown_spawn_volume", -3, -1, 999});
    Rejected("spawn-outside-its-volume-rejected", workspace.root / "spawn_outside_volume", scene,
             SceneWorldExportOptions{"spawn_outside_volume", -5, -3, bridge->id});
    Rejected("layered-spawn-without-layers-rejected", workspace.root / "spawn_without_layers", Fixture(false),
             SceneWorldExportOptions{"spawn_without_layers", -5, -3, 1});

    auto terrain_scene = Fixture(true);
    const auto terrain_path = workspace.root / "terrain_spawn_v1";
    Check("terrain-spawn-keeps-worldlogic-v1",
        ExportSceneServerWorld(terrain_path, terrain_scene, {}, Options(), result) &&
        WorldLogicVersion(terrain_path) == map::kWorldLogicFileVersion);
}

void TestNegativeControls(const TemporaryWorkspace& workspace)
{
    const auto base = Fixture(false);
    auto scene = base;
    scene.terrain.exists = false;
    Rejected("missing-terrain-rejected", workspace.root / "no_terrain", scene);
    scene = base;
    scene.terrain.widthMeters += 1;
    Rejected("dimension-cellsize-mismatch-rejected", workspace.root / "wrong_size", scene);
    scene = base;
    scene.terrain.cellsX = 0;
    Rejected("zero-cells-rejected", workspace.root / "zero_cells", scene);
    scene = base;
    scene.terrain.cellSizeMeters = std::numeric_limits<float>::infinity();
    Rejected("nonfinite-cellsize-rejected", workspace.root / "invalid_cell", scene);
    scene = base;
    scene.terrain.heightCmGrid.pop_back();
    Rejected("missing-height-sample-rejected", workspace.root / "missing_sample", scene);
    scene = base;
    scene.terrain.heightCmGrid.push_back(0);
    Rejected("extra-height-sample-rejected", workspace.root / "extra_sample", scene);
    scene = base;
    scene.terrain.heightCmGrid[0] = std::numeric_limits<float>::quiet_NaN();
    Rejected("nan-height-rejected", workspace.root / "nan_height", scene);
    scene.terrain.heightCmGrid[0] = std::numeric_limits<float>::infinity();
    Rejected("infinite-height-rejected", workspace.root / "infinite_height", scene);
    scene.terrain.heightCmGrid[0] = 1e12f;
    Rejected("out-of-int32-height-range-rejected-without-clamping", workspace.root / "overflow_height", scene);
    scene = base;
    scene.terrain.attributes.pop_back();
    Rejected("attribute-size-mismatch-rejected", workspace.root / "short_attrs", scene);
    scene = base;
    scene.terrain.attributes[1] = 2;
    Rejected("reserved-attribute-bit-rejected", workspace.root / "unknown_attrs", scene);
    scene = base;
    scene.terrain.attributes[3 * scene.terrain.cellsX] = map::CellSample::kBlocked;
    Rejected("authored-blocked-spawn-rejected-without-relocation", workspace.root / "blocked_spawn", scene);
    auto options = Options();
    options.spawnX = 6;
    Rejected("outside-half-open-spawn-rejected", workspace.root / "outside_spawn", base, options);
    options = Options();
    options.spawnZ = std::numeric_limits<float>::quiet_NaN();
    Rejected("nan-spawn-rejected", workspace.root / "nan_spawn", base, options);
    options = Options();
    options.worldId.clear();
    Rejected("missing-world-id-rejected", workspace.root / "empty_id", base, options);
    options = Options();
    options.worldId = "world with spaces";
    Rejected("world-id-with-spaces-rejected",workspace.root / "spaced_id",base,options);
    options.worldId = "world_\xc3\xa1";
    Rejected("non-ascii-world-id-rejected",workspace.root / "nonascii_id",base,options);
    options.worldId.assign(65,'a');
    Rejected("oversized-world-id-rejected",workspace.root / "long_id",base,options);
    scene = Fixture();
    scene.waterBodies[0].shapeMask[0] = 0;
    Rejected("masked-water-never-converted-to-full-rectangle", workspace.root / "masked_water", scene);
    scene = base;
    scene.terrain.cellsX = 5000;
    scene.terrain.cellsZ = 5000;
    scene.terrain.widthMeters = 10000;
    scene.terrain.depthMeters = 10000;
    Rejected("bounded-terrain-vertex-count-before-allocation", workspace.root / "oversized_grid", scene);
    scene = base;
    scene.terrain.chunkSizeCells = map::kMaxChunkSizeCells + 1;
    Rejected("oversized-chunk-pitch-rejected", workspace.root / "oversized_chunk", scene);
}

void TestFractionalSpawnSeam(const TemporaryWorkspace& workspace)
{
    SceneData scene;
    auto& terrain = scene.terrain;
    terrain.exists = true;
    terrain.cellsX = 7;
    terrain.cellsZ = 4;
    terrain.cellSizeMeters = 0.1f;
    terrain.widthMeters = static_cast<float>(terrain.cellsX) * terrain.cellSizeMeters;
    terrain.depthMeters = static_cast<float>(terrain.cellsZ) * terrain.cellSizeMeters;
    terrain.chunkSizeCells = 3;
    terrain.heightCmGrid.assign(40,-100.125f);
    terrain.attributes.assign(28,0);
    for (std::uint32_t row = 0; row < terrain.cellsZ; ++row)
        terrain.attributes[row * terrain.cellsX + 2] = map::CellSample::kBlocked;
    const double seam = -static_cast<double>(terrain.widthMeters * 0.5f) +
        3 * static_cast<double>(terrain.cellSizeMeters);
    SceneWorldExportOptions options{"fractional_cell_seam",0,0};
    options.spawnX = std::nextafter(static_cast<float>(seam),-std::numeric_limits<float>::infinity());
    Rejected("fractional-seam-west-blocked-spawn-rejected",workspace.root / "seam_blocked",scene,options);
    options.spawnX = std::nextafter(static_cast<float>(seam),std::numeric_limits<float>::infinity());
    SceneWorldPackageResult result;
    const auto path = workspace.root / "seam_walkable";
    const bool exported = ExportSceneServerWorld(path,scene,{},options,result);
    map::PackageReport report;
    const auto loaded = exported ? map::LoadServerWorld(path,map::ValidationDepth::Full,report) : std::nullopt;
    std::uint32_t cx = 0, cy = 0;
    Check("fractional-seam-east-spawn-uses-authoritative-cell-membership",loaded &&
        loaded->terrain.CellIndexOf(options.spawnX,options.spawnZ,cx,cy) && cx == 3 &&
        loaded->terrain.Cell(options.spawnX,options.spawnZ).Walkable() &&
        loaded->logic.spawns[0].bounds.CenterX() == options.spawnX);
}

void TestFractionalPitchOracle(const TemporaryWorkspace& workspace)
{
    SceneData scene;
    auto& terrain = scene.terrain;
    terrain.exists = true;
    terrain.cellsX = 13;
    terrain.cellsZ = 5;
    terrain.cellSizeMeters = 0.1f;
    terrain.widthMeters = static_cast<float>(terrain.cellsX) * terrain.cellSizeMeters;
    terrain.depthMeters = static_cast<float>(terrain.cellsZ) * terrain.cellSizeMeters;
    terrain.chunkSizeCells = 4;
    for (std::uint32_t y = 0; y <= terrain.cellsZ; ++y) {
        for (std::uint32_t x = 0; x <= terrain.cellsX; ++x) {
            terrain.heightCmGrid.push_back(-250.125f + 7.003f * x + 11.007f * y);
        }
    }
    SceneWorldPackageResult result;
    const auto path = workspace.root / "fractional_pitch_13x5";
    const bool exported = ExportSceneServerWorld(path,scene,{},SceneWorldExportOptions{"fractional_pitch_world",0,0},result);
    map::PackageReport report;
    const auto loaded = exported ? map::LoadServerWorld(path,map::ValidationDepth::Full,report) : std::nullopt;
    Check("fractional-thirteen-by-five-cell-grid-export",loaded && report.Ok() &&
        result.maxHeightErrorMeters <= 0.00005000001);
    Check("fractional-grid-position-drift-is-separate-and-measured",exported &&
        result.maxGridPositionErrorMeters > 0 && result.maxGridPositionErrorMeters < 0.000001);
    if (!loaded) return;
    const std::array<std::array<float,2>,6> points{{{-0.6f,0.21f}, {-0.45f,-0.13f}, {-0.21f,0.04f},
                                                {0.07f,-0.04f}, {0.38f,0.12f}, {0.60f,-0.21f}}};
    double maximum_client_error = 0, maximum_server_error = 0;
    bool client_server_ok = true;
    for (const auto& point : points) {
        const double expected = IndependentHeight(terrain,point[0],point[1]);
        const float client = SampleTerrainCollisionHeightCm(terrain.heightCmGrid,14,6,
            (point[0]+terrain.widthMeters*0.5f)*100,(terrain.depthMeters*0.5f-point[1])*100,
            terrain.cellSizeMeters*100)*0.01f;
        const auto server = loaded->terrain.Height(point[0],point[1]);
        maximum_client_error = std::max(maximum_client_error,std::abs(client-expected));
        maximum_server_error = std::max(maximum_server_error,std::abs(server.meters-expected));
        client_server_ok = client_server_ok && server.Ok() && Near(client,expected,0.00006) && Near(server.meters,expected,0.00006);
    }
    Check("fractional-pitch-client-server-independent-height-parity",client_server_ok);
    std::cout << "SCENE WORLD PACKAGE fractional pitch measured max metres: vertex-quantization=" <<
        result.maxHeightErrorMeters << " client-vs-independent=" << maximum_client_error <<
        " server-vs-independent=" << maximum_server_error << " grid-position-drift=" <<
        result.maxGridPositionErrorMeters << '\n';
#if defined(IXENGINE_PHYSICS_WITH_JOLT)
    physics::PhysicsWorld world;
    physics::TerrainColliderDesc descriptor;
    descriptor.widthMeters = terrain.widthMeters;
    descriptor.depthMeters = terrain.depthMeters;
    descriptor.cellSizeMeters = terrain.cellSizeMeters;
    descriptor.cellsX = terrain.cellsX;
    descriptor.cellsZ = terrain.cellsZ;
    descriptor.heightCmGrid = terrain.heightCmGrid;
    const auto body = world.CreateTerrainCollider(descriptor);
    bool all_rays = body != 0;
    double maximum_jolt_error = 0, maximum_server_jolt_error = 0;
    for (const auto& point : points) {
        const float origin[3] = {point[0],10,point[1]}, direction[3] = {0,-1,0};
        physics::PhysicsRaycastHit hit;
        const bool hit_body = world.Raycast(origin,direction,30,hit) && hit.bodyId == body;
        const double expected = IndependentHeight(terrain,point[0],point[1]);
        if (hit_body) {
            maximum_jolt_error = std::max(maximum_jolt_error,std::abs(hit.position[1]-expected));
            maximum_server_jolt_error = std::max(maximum_server_jolt_error,
                std::abs(static_cast<double>(loaded->terrain.Height(point[0],point[1]).meters)-hit.position[1]));
        }
        all_rays = all_rays && hit_body && Near(hit.position[1],expected,0.00006) &&
            Near(hit.position[1],loaded->terrain.Height(point[0],point[1]).meters,0.00006);
    }
    Check("fractional-pitch-actual-jolt-client-server-parity",all_rays);
    std::cout << "SCENE WORLD PACKAGE fractional pitch measured max metres: jolt-vs-independent=" <<
        maximum_jolt_error << " server-vs-jolt=" << maximum_server_jolt_error << '\n';
    // A legacy descriptor with inconsistent extent intentionally keeps its
    // historical width/cell-count pitch; do not silently reinterpret it.
    physics::PhysicsWorld legacy_world;
    descriptor.widthMeters = 2.6f;
    const auto legacy_body = legacy_world.CreateTerrainCollider(descriptor);
    const float legacy_origin[3] = {-1.2f,10,0.21f}, direction[3] = {0,-1,0};
    physics::PhysicsRaycastHit legacy_hit;
    const double legacy_source_x = 0.5;
    const double legacy_source_y = (0.25-0.21) / 0.1;
    const double legacy_expected = (-250.125 + 7.003*legacy_source_x + 11.007*legacy_source_y)*0.01;
    Check("legacy-inconsistent-descriptor-keeps-extent-divided-pitch",legacy_body != 0 &&
        legacy_world.Raycast(legacy_origin,direction,30,legacy_hit) && legacy_hit.bodyId == legacy_body &&
        Near(legacy_hit.position[1],legacy_expected,0.00006));
#endif
}

void TestPublication(const TemporaryWorkspace& workspace)
{
    const auto scene = Fixture(false);
    const std::vector<std::uint8_t> sentinel{'k','e','e','p'};
    SceneWorldPackageResult result;
    const auto existing = workspace.root / "existing_partial_package";
    fs::create_directory(existing);
    WriteBytes(existing / "worldlogic.dat", sentinel);
    Check("existing-partial-target-without-manifest-refused", !ExportSceneServerWorld(existing,scene,{},Options(),result) &&
        ReadBytes(existing / "worldlogic.dat") == sentinel && !fs::exists(existing / "map.manifest"));
    const auto pending_final = workspace.root / "preexisting_stage_target";
    auto pending = pending_final;
    pending += ".pending";
    fs::create_directory(pending);
    WriteBytes(pending / "owned-by-another-export.txt", sentinel);
    Check("preexisting-staging-directory-preserved", !ExportSceneServerWorld(pending_final,scene,{},Options(),result) &&
        ReadBytes(pending / "owned-by-another-export.txt") == sentinel && !fs::exists(pending_final));
    const auto existing_file = workspace.root / "existing_target_file";
    WriteBytes(existing_file,sentinel);
    Check("existing-target-file-preserved", !ExportSceneServerWorld(existing_file,scene,{},Options(),result) &&
        ReadBytes(existing_file) == sentinel);
    const auto unicode = workspace.root / fs::path(u8"világ_árvíztűrő_世界");
    const bool unicode_exported = ExportSceneServerWorld(unicode,scene,{},Options(),result);
    Check("unicode-package-directory-export", unicode_exported && fs::exists(unicode / "map.manifest"));
    if (!unicode_exported) {
        for (const auto& error : result.errors) std::cout << "Unicode export diagnostic: " << error << '\n';
    }
    const auto first_manifest = ReadBytes(unicode / "map.manifest");
    const bool second_exported = ExportSceneServerWorld(unicode,scene,{},Options(),result);
    Check("second-export-preserves-complete-existing-package", unicode_exported && !first_manifest.empty() &&
        !second_exported && !result.errors.empty() && ReadBytes(unicode / "map.manifest") == first_manifest);
    const auto concurrent = workspace.root / "concurrent_package";
    std::array<bool,2> successes{};
    std::array<SceneWorldPackageResult,2> results;
    std::latch start(2);
    const auto once = [&](std::size_t index) {
        start.arrive_and_wait();
        successes[index] = ExportSceneServerWorld(concurrent,scene,{},Options(),results[index]);
    };
    std::thread first(once,0), second(once,1);
    first.join();
    second.join();
    Check("concurrent-whole-package-export-has-one-winner", successes[0] != successes[1]);
    map::PackageReport report;
    Check("concurrent-winner-publishes-a-complete-strict-package", map::LoadServerWorld(concurrent,map::ValidationDepth::Full,report) && report.Ok());
    auto concurrent_pending = concurrent;
    concurrent_pending += ".pending";
    Check("concurrent-export-removes-owned-staging", !fs::exists(concurrent_pending));
    auto invalid_water_scene = Fixture();
    invalid_water_scene.waterBodies[0].waterLevelY = 100001;
    const auto invalid_water = workspace.root / "strict_rejected_water";
    Rejected("postwrite-strict-rejection-cleans-only-owned-staging",invalid_water,invalid_water_scene);
}

void TestExactScenePersistence(const TemporaryWorkspace& workspace)
{
    auto scene = Fixture(false);
    scene.terrain.heightCmGrid[0] = -50000.125f;
    scene.terrain.heightCmGrid[1] = -700.003f;
    auto& manager = SceneManager::Instance();
    manager.CloseScene();
    manager.NewScene();
    manager.SetCurrentSceneSnapshot(scene);
    const auto path = workspace.root / "precise_authoring.scene";
    const bool saved = manager.SaveSceneAs(path.string());
    Check("real-scene-manager-saves-exact-height-sidecar", saved);
    if (!saved) return;
    manager.CloseScene();
    const bool loaded = manager.LoadScene(path.string());
    Check("real-scene-manager-reloads-fractional-and-large-height-data", loaded &&
        manager.GetCurrentScene().terrain.heightCmGrid == scene.terrain.heightCmGrid);
    Check("real-scene-manager-reloads-nonsquare-known-cell-attributes", loaded &&
        manager.GetCurrentScene().terrain.attributes == scene.terrain.attributes &&
        manager.GetCurrentScene().terrain.cellsX == 6 && manager.GetCurrentScene().terrain.cellsZ == 4);
    const auto exact = workspace.root / "precise_authoring_terrain_exact.height";
    const auto exact_bytes = ReadBytes(exact);
    Check("declared-exact-height-sidecar-is-present", exact_bytes.size() == 36 + 35 * sizeof(float));
    manager.CloseScene();
    fs::rename(exact, workspace.root / "saved_exact_height.bin");
    Check("declared-missing-exact-height-sidecar-fails-without-rounded-fallback", !manager.LoadScene(path.string()));
    WriteBytes(exact, std::vector<std::uint8_t>{'b','a','d'});
    Check("declared-truncated-exact-height-sidecar-fails", !manager.LoadScene(path.string()));
    auto malformed = exact_bytes;
    if (malformed.size() > 35) {
        malformed[28] = 34; // little-endian u64 sample count, actual expected 35
        WriteBytes(exact,malformed);
        Check("exact-height-sidecar-count-mismatch-fails", !manager.LoadScene(path.string()));
        malformed = exact_bytes;
        const float nan = std::numeric_limits<float>::quiet_NaN();
        std::memcpy(malformed.data() + 36,&nan,sizeof(nan));
        WriteBytes(exact,malformed);
        Check("exact-height-sidecar-nonfinite-sample-fails", !manager.LoadScene(path.string()));
        malformed = exact_bytes;
        malformed[16] = 7; // cellsX mismatch, preserve source JSON/chunks metadata
        WriteBytes(exact,malformed);
        Check("exact-height-sidecar-dimension-mismatch-fails", !manager.LoadScene(path.string()));
    }
    WriteBytes(exact,exact_bytes);
    Check("restored-exact-height-sidecar-reloads-original-data", manager.LoadScene(path.string()) &&
        manager.GetCurrentScene().terrain.heightCmGrid == scene.terrain.heightCmGrid);
    manager.CloseScene();
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--validate-package") {
        map::PackageReport report;
        const auto loaded = map::LoadServerWorld(fs::path(argv[2]),map::ValidationDepth::Full,report);
        Check("external-package-full-strict-validation",loaded && report.Ok());
        for (const auto& issue : report.issues) std::cout << issue.Format() << '\n';
        if (loaded) std::cout << "world=" << report.manifest.world_id << " origin=" << report.manifest.origin_x << ',' <<
            report.manifest.origin_y << " size=" << report.manifest.size_cells_x << 'x' << report.manifest.size_cells_y <<
            " heightLayer=" << loaded->terrain.Encoding().layer_version << " chunks=" << report.chunks_decoded << '\n';
        return failures == 0 ? 0 : 1;
    }
    try {
        const TemporaryWorkspace workspace;
        TestFullExport(workspace);
        TestPrecisionAndNonplanarity(workspace);
        TestNegativeControls(workspace);
        TestLayeredSpawn(workspace);
        TestFractionalSpawnSeam(workspace);
        TestFractionalPitchOracle(workspace);
        TestPublication(workspace);
        TestExactScenePersistence(workspace);
    } catch (const std::exception& exception) {
        Check("unexpected-test-exception",false);
        std::cout << "test exception: " << exception.what() << '\n';
    }
    std::cout << "SCENE WORLD PACKAGE summary: checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
