#include "SceneLayerAuthoring.h"
#include "map/WorldPackage.h"
#include "map/WorldPackageWriter.h"
#include "physics/PhysicsWorld.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <latch>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
namespace map = mx::map;
namespace physics = ixtreeme::physics;

int checks = 0;
int failures = 0;

void Check(const std::string& name, bool value)
{
    ++checks;
    if (!value) ++failures;
    std::cout << "SCENE LAYER AUTHORING " << name << ": " << (value ? "PASS" : "FAIL") << '\n';
}

bool Near(float actual, float expected, float tolerance = 0.0001f)
{
    return std::isfinite(actual) && std::abs(actual - expected) <= tolerance;
}

class TemporaryWorkspace
{
public:
    TemporaryWorkspace()
    {
        parent_ = std::filesystem::weakly_canonical(std::filesystem::temp_directory_path());
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        root = parent_ / ("ixw_scene_layer_test_" + std::to_string(suffix));
        if (!std::filesystem::create_directory(root))
            throw std::runtime_error("cannot create unique temporary test directory");
    }

    ~TemporaryWorkspace()
    {
        // Only remove the directory created by this instance, inside the
        // verified temporary parent. Never touch repository fixtures/assets.
        if (root.parent_path() == parent_ && root.filename().string().starts_with("ixw_scene_layer_test_")) {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
        }
    }

    std::filesystem::path root;

private:
    std::filesystem::path parent_;
};

std::vector<std::uint8_t> ReadBytes(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void WriteText(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream stream(path, std::ios::binary);
    stream << text;
    if (!stream) throw std::runtime_error("cannot write temporary scene");
}

MeshSceneEntity Box(std::uint32_t id, const char* name, float floorHeight, std::uint32_t tags)
{
    MeshSceneEntity entity;
    entity.id = id;
    entity.name = name;
    entity.hasCollider = true;
    entity.collider.shape = physics::ColliderShape::Box;
    entity.collider.layer = physics::PhysicsLayer::StaticWorld;
    entity.collider.size[0] = 16;
    entity.collider.size[1] = 1;
    entity.collider.size[2] = 16;
    entity.position[0] = 10;
    entity.position[1] = floorHeight - 0.5f;
    entity.position[2] = 10;
    entity.layerAuthoring = {true, tags};
    return entity;
}

SceneData Fixture()
{
    SceneData scene;
    scene.name = "Collision-authored layered fixture";
    scene.terrain.exists = true;
    scene.terrain.widthMeters = 32;
    scene.terrain.depthMeters = 32;
    scene.terrain.cellsX = 32;
    scene.terrain.cellsZ = 32;
    scene.terrain.cellSizeMeters = 1;
    scene.terrain.chunkSizeCells = 32;
    scene.terrain.heightCmGrid.assign(33u * 33u, 0);
    scene.meshEntities.push_back(Box(10, "ground", 0, map::VolumeTagGround | map::VolumeTagRoad));
    scene.meshEntities.push_back(Box(11, "underpass", -6, map::VolumeTagConnector | map::VolumeTagRoad));
    auto upper = Box(12, "upper-bridge", 6, map::VolumeTagBridge | map::VolumeTagConnector);
    upper.collider.shape = physics::ColliderShape::Mesh;
    upper.position[0] = 6;
    upper.position[1] = 6;
    upper.position[2] = 6;
    scene.meshEntities.push_back(upper);
    auto excluded = Box(13, "excluded-decoration", 0, map::VolumeTagBuilding | map::VolumeTagInterior);
    excluded.layerAuthoring.enabled = false;
    excluded.collider.shape = physics::ColliderShape::Sphere;
    scene.meshEntities.push_back(excluded);
    WaterBody water;
    water.id = 7;
    water.name = "water-surface";
    water.bboxMin[0] = 22;
    water.bboxMin[1] = 22;
    water.bboxMax[0] = 28;
    water.bboxMax[1] = 28;
    water.waterLevelY = 3;
    water.maskWidth = 2;
    water.maskHeight = 2;
    water.shapeMask = {255, 255, 255, 255};
    scene.waterBodies.push_back(water);
    // Match the actual engine/Jolt terrain centered at the world origin.
    for (auto& entity : scene.meshEntities) {
        entity.position[0] -= 16;
        entity.position[2] -= 16;
    }
    for (auto& body : scene.waterBodies) {
        for (std::size_t axis = 0; axis < 2; ++axis) {
            body.bboxMin[axis] -= 16;
            body.bboxMax[axis] -= 16;
        }
    }
    return scene;
}

const LayerCollisionGeometryProvider GeometryProvider = [](
    const MeshSceneEntity& entity, std::vector<std::array<float, 3>>& vertices,
    std::vector<std::uint32_t>& indices) {
    if (entity.id != 12) return false;
    // Actual model collision triangles, upward winding in engine Y-up metres.
    vertices = {{0, 0, 0}, {8, 0, 0}, {8, 0, 8}, {0, 0, 8}};
    indices = {0, 2, 1, 0, 3, 2};
    return true;
};

const map::LayerVolume* Volume(const map::LayeredWorld& world, std::uint32_t tags)
{
    const auto found = std::find_if(world.volumes.begin(), world.volumes.end(),
        [&](const auto& volume) { return volume.tags == tags; });
    return found == world.volumes.end() ? nullptr : &*found;
}

const MeshSceneEntity* Entity(const SceneData& scene, std::uint32_t id)
{
    const auto found = std::find_if(scene.meshEntities.begin(), scene.meshEntities.end(),
        [&](const auto& entity) { return entity.id == id; });
    return found == scene.meshEntities.end() ? nullptr : &*found;
}

void CheckRejected(const char* name, const SceneData& scene,
    const LayerCollisionGeometryProvider& provider = GeometryProvider)
{
    SceneLayerAuthoringResult result;
    Check(name, !GenerateSceneLayers(scene, provider, result) && !result.errors.empty() && result.world.volumes.empty());
}

std::array<float, 3> IndependentlyTransform(const MeshSceneEntity& entity, const std::array<float, 3>& vertex)
{
    float x = (vertex[0] + entity.collider.center[0]) * std::max(0.001f, std::abs(entity.scale[0]));
    float y = (vertex[1] + entity.collider.center[1]) * std::max(0.001f, std::abs(entity.scale[1]));
    float z = (vertex[2] + entity.collider.center[2]) * std::max(0.001f, std::abs(entity.scale[2]));
    const float afterXy = y * std::cos(entity.rotation[0]) - z * std::sin(entity.rotation[0]);
    const float afterXz = y * std::sin(entity.rotation[0]) + z * std::cos(entity.rotation[0]);
    y = afterXy;
    z = afterXz;
    const float afterYx = x * std::cos(entity.rotation[1]) + z * std::sin(entity.rotation[1]);
    const float afterYz = -x * std::sin(entity.rotation[1]) + z * std::cos(entity.rotation[1]);
    x = afterYx;
    z = afterYz;
    const float afterZx = x * std::cos(entity.rotation[2]) - y * std::sin(entity.rotation[2]);
    const float afterZy = x * std::sin(entity.rotation[2]) + y * std::cos(entity.rotation[2]);
    return {afterZx + entity.position[0], z + entity.position[2], afterZy + entity.position[1]};
}

void TestTransforms()
{
    const std::array<float, 3> enginePoint{3.25f, -7.5f, 11.75f};
    const auto canonical = EngineToLayerCoordinates(enginePoint);
    Check("metre-coordinates-y-up-to-z-up", canonical == std::array<float, 3>{3.25f, 11.75f, -7.5f});
    Check("canonical-coordinate-roundtrip", LayerToEngineCoordinates(canonical) == enginePoint);
    auto mesh = Box(12, "transformed-mesh", 0, map::VolumeTagBridge);
    mesh.collider.shape = physics::ColliderShape::Mesh;
    mesh.collider.center[0] = 0.75f;
    mesh.collider.center[1] = -1.25f;
    mesh.collider.center[2] = 2.5f;
    mesh.position[0] = 4;
    mesh.position[1] = 5;
    mesh.position[2] = 6;
    mesh.scale[0] = -2;
    mesh.scale[1] = 3;
    mesh.scale[2] = -0.5f;
    mesh.rotation[0] = 0.2f;
    mesh.rotation[1] = -0.35f;
    mesh.rotation[2] = 0.15f;
    map::LayerCollisionMesh cooked;
    std::string error;
    const bool built = BuildLayerCollisionMesh(mesh, GeometryProvider, cooked, error);
    Check("collider-transform-cooks", built && error.empty() && cooked.vertices.size() == 4);
    const std::array<std::array<float, 3>, 4> local = {{{0, 0, 0}, {8, 0, 0}, {8, 0, 8}, {0, 0, 8}}};
    bool verticesMatch = built && cooked.vertices.size() == local.size();
    for (std::size_t i = 0; verticesMatch && i < local.size(); ++i) {
        const auto expected = IndependentlyTransform(mesh, local[i]);
        for (std::size_t axis = 0; axis < 3; ++axis)
            verticesMatch = verticesMatch && Near(cooked.vertices[i][axis], expected[axis]);
    }
    Check("center-absolute-nonuniform-scale-all-euler-axes", verticesMatch);
    Check("axis-swap-corrects-triangle-winding", built && cooked.indices == std::vector<std::uint32_t>{0, 1, 2, 0, 2, 3});

#if defined(IXENGINE_PHYSICS_WITH_JOLT)
    for (const int scenario : {0, 1, 2, 3}) {
        const bool thin = scenario == 1;
        const bool tinyCenter = scenario == 2;
        const bool mixedCenter = scenario == 3;
        auto box = Box(21, "raycast-oracle", 0, map::VolumeTagGround);
        box.position[1] = 5;
        box.collider.center[0] = 1;
        box.collider.center[1] = 2;
        box.collider.center[2] = -1;
        box.scale[0] = -2;
        box.scale[1] = thin ? 0.001f : 3.0f;
        box.scale[2] = 0.5f;
        box.collider.size[1] = thin ? 0.1f : 2.0f;
        if (tinyCenter || mixedCenter) {
            for (std::size_t axis = 0; axis < 3; ++axis) {
                box.collider.center[axis] = 0.00005f;
                box.scale[axis] = 1;
            }
            if (mixedCenter) box.collider.center[0] = 0.001f;
        }
        physics::PhysicsWorld physicsWorld;
        physics::PhysicsBodyDesc desc;
        desc.bodyType = physics::BodyType::Static;
        desc.rigidbody.bodyType = physics::BodyType::Static;
        desc.rigidbody.useGravity = false;
        desc.collider = box.collider;
        std::copy(std::begin(box.position), std::end(box.position), std::begin(desc.transform.position));
        std::copy(std::begin(box.scale), std::end(box.scale), std::begin(desc.scale));
        const auto body = physicsWorld.CreateBody(desc);
        const float origin[3] = {box.position[0] + std::abs(box.scale[0]) * box.collider.center[0],
                                box.position[1] + 20,
                                box.position[2] + std::abs(box.scale[2]) * box.collider.center[2]};
        const float direction[3] = {0, -1, 0};
        physics::PhysicsRaycastHit hit;
        const bool hitBody = body != 0 && physicsWorld.Raycast(origin, direction, 40, hit) && hit.bodyId == body;
        Check(mixedCenter ? "jolt-mixed-center-box-raycast" : tinyCenter ? "jolt-tiny-center-box-raycast" :
            (thin ? "jolt-thin-box-raycast" : "jolt-transformed-box-raycast"), hitBody);
        const bool boxBuilt = BuildLayerCollisionMesh(box, {}, cooked, error);
        float top = -std::numeric_limits<float>::infinity();
        if (boxBuilt) for (const auto& point : cooked.vertices) top = std::max(top, point[2]);
        Check(mixedCenter ? "jolt-large-component-keeps-entire-center" : tinyCenter ? "jolt-subthreshold-center-matches-cooked-height" :
            (thin ? "jolt-minimum-half-extent-matches-cooked-height" : "jolt-center-scale-matches-cooked-height"),
            boxBuilt && hitBody && Near(top, hit.position[1], (tinyCenter || mixedCenter) ? 0.000001f : 0.0002f));
    }
#else
    std::cout << "SCENE LAYER AUTHORING Jolt collision oracle: NOT_RUN (backend disabled)\n";
#endif
}

void TestGenerationAndServer(const TemporaryWorkspace& workspace)
{
    const SceneData scene = Fixture();
    SceneLayerAuthoringResult result;
    const bool generated = GenerateSceneLayers(scene, GeometryProvider, result);
    Check("scene-collision-and-water-generation", generated && result.errors.empty());
    if (!generated) {
        for (const auto& error : result.errors) std::cout << "generation error: " << error << '\n';
        return;
    }
    Check("four-volumes-from-model-collision-and-water", result.world.volumes.size() == 4);
    Check("only-upward-faces-cooked", result.geometry.triangles_seen == 28 && result.geometry.triangles_walkable == 8);
    Check("actual-centered-engine-terrain-bounds-retained", result.worldBounds.min_x == -16 && result.worldBounds.min_y == -16 &&
        result.worldBounds.max_x == 16 && result.worldBounds.max_y == 16);
    Check("no-inferred-portals", result.world.portals.empty());
    const auto* ground = Volume(result.world, map::VolumeTagGround | map::VolumeTagRoad);
    const auto* underpass = Volume(result.world, map::VolumeTagConnector | map::VolumeTagRoad);
    const auto* upper = Volume(result.world, map::VolumeTagBridge | map::VolumeTagConnector);
    const auto* water = Volume(result.world, map::VolumeTagWater);
    Check("ground-band-derived-from-box-top", ground && Near(ground->min_z, -0.02f) && Near(ground->max_z, 2));
    Check("ground-road-tags-preserved", ground && ground->tags == (map::VolumeTagGround | map::VolumeTagRoad));
    Check("underpass-height-is-negative", underpass && Near(underpass->min_z, -6.02f) && Near(underpass->max_z, -4));
    Check("mesh-upper-height-derived-without-manual-z", upper && Near(upper->min_z, 5.98f) && Near(upper->max_z, 8));
    Check("bridge-connector-tags-and-kind", upper && upper->tags == (map::VolumeTagBridge | map::VolumeTagConnector) &&
        upper->kind == map::VolumeKind::Connector);
    Check("water-band-is-metadata-not-ground-movement", water && Near(water->min_z, 2.98f) && Near(water->max_z, 5) &&
        water->kind == map::VolumeKind::WaterSurface && !water->supports_ground_movement);
    Check("excluded-unsupported-decoration-skipped", Volume(result.world, map::VolumeTagBuilding | map::VolumeTagInterior) == nullptr);
    Check("stacked-volume-identity", ground && underpass && upper &&
        result.world.FindVolume(-6, -6, 0.5f).value_or(0) == ground->id &&
        result.world.FindVolume(-6, -6, -5).value_or(0) == underpass->id &&
        result.world.FindVolume(-6, -6, 7).value_or(0) == upper->id);
    auto reordered = scene;
    std::reverse(reordered.meshEntities.begin(), reordered.meshEntities.end());
    SceneLayerAuthoringResult repeated;
    Check("generation-identity-stable-after-entity-reorder", GenerateSceneLayers(reordered, GeometryProvider, repeated) &&
        map::EncodeLayeredWorld(repeated.world) == map::EncodeLayeredWorld(result.world));
    auto twoWaters = scene;
    auto secondWater = twoWaters.waterBodies.front();
    secondWater.id = 9;
    secondWater.name = "second-water-surface";
    secondWater.bboxMin[1] = -14;
    secondWater.bboxMax[1] = -8;
    twoWaters.waterBodies.push_back(secondWater);
    SceneLayerAuthoringResult waterOrderA;
    SceneLayerAuthoringResult waterOrderB;
    const bool firstWaterOrder = GenerateSceneLayers(twoWaters, GeometryProvider, waterOrderA);
    std::reverse(twoWaters.waterBodies.begin(), twoWaters.waterBodies.end());
    const bool secondWaterOrder = GenerateSceneLayers(twoWaters, GeometryProvider, waterOrderB);
    Check("two-separated-water-surfaces-share-height", firstWaterOrder && secondWaterOrder &&
        waterOrderA.world.volumes.size() == 5 &&
        std::count_if(waterOrderA.world.volumes.begin(), waterOrderA.world.volumes.end(),
            [](const auto& volume) { return volume.kind == map::VolumeKind::WaterSurface; }) == 2);
    Check("water-ids-stable-after-authoring-array-reorder", firstWaterOrder && secondWaterOrder &&
        map::EncodeLayeredWorld(waterOrderA.world) == map::EncodeLayeredWorld(waterOrderB.world));

    std::string error;
    const auto sidecar = workspace.root / "export" / "layered_world.mx3d";
    Check("fresh-sidecar-export", ExportSceneLayerSidecar(sidecar, result, error) && error.empty());
    const auto exported = ReadBytes(sidecar);
    map::LayeredWorld decoded;
    Check("exported-sidecar-decodes-and-validates", map::DecodeLayeredWorld(exported, decoded, error) &&
        decoded.Validate(result.worldBounds, error) && map::EncodeLayeredWorld(decoded) == map::EncodeLayeredWorld(result.world));
    Check("second-export-refuses-existing-file", !ExportSceneLayerSidecar(sidecar, result, error) && !error.empty());
    Check("existing-export-bytes-unchanged", ReadBytes(sidecar) == exported);
    const auto unicodePath = workspace.root /
        std::filesystem::path(u8"r\u00e9tegek_\u00e1rv\u00edzt\u0171r\u0151_\u4e16\u754c.mx3d");
    Check("unicode-native-path-sidecar-export", ExportSceneLayerSidecar(unicodePath, result, error) &&
        error.empty() && ReadBytes(unicodePath) == exported);
    const auto blockedFinal = workspace.root / "blocked-export.mx3d";
    auto existingPending = blockedFinal;
    existingPending += ".pending";
    WriteText(existingPending, "preexisting staging bytes must survive");
    const auto pendingBytes = ReadBytes(existingPending);
    Check("preexisting-pending-file-refuses-export", !ExportSceneLayerSidecar(blockedFinal, result, error) && !error.empty());
    Check("preexisting-pending-file-preserved", ReadBytes(existingPending) == pendingBytes &&
        !std::filesystem::exists(blockedFinal));
    const auto concurrentFinal = workspace.root / "concurrent-export.mx3d";
    std::array<bool, 2> successes{};
    std::array<std::string, 2> threadErrors;
    std::latch start(2);
    auto exportOnce = [&](std::size_t index) {
        start.arrive_and_wait();
        successes[index] = ExportSceneLayerSidecar(concurrentFinal, result, threadErrors[index]);
    };
    std::thread firstExporter(exportOnce, 0);
    std::thread secondExporter(exportOnce, 1);
    firstExporter.join();
    secondExporter.join();
    Check("concurrent-same-target-export-has-one-winner", successes[0] != successes[1] &&
        (successes[0] ? threadErrors[0].empty() && !threadErrors[1].empty() :
                        threadErrors[1].empty() && !threadErrors[0].empty()));
    Check("concurrent-export-publishes-complete-original-bytes", ReadBytes(concurrentFinal) == exported);
    auto concurrentPending = concurrentFinal;
    concurrentPending += ".pending";
    Check("concurrent-export-leaves-no-staging-file", !std::filesystem::exists(concurrentPending));

    map::PackageWriteSpec spec;
    spec.world_id = "scene_layer_authoring_fixture";
    spec.world_name = scene.name;
    spec.size_cells_x = 32;
    spec.size_cells_y = 32;
    spec.chunk_size_cells = 16;
    spec.origin_x = result.worldBounds.min_x;
    spec.origin_y = result.worldBounds.min_y;
    spec.height_raw = [](std::uint32_t, std::uint32_t) { return 0; };
    spec.attributes = [](std::uint32_t, std::uint32_t) { return std::uint16_t{0}; };
    spec.logic.spawns.push_back(map::SpawnRegion{1, 0, map::Rect{2, 2, 4, 4}});
    spec.layered_world = result.world;
    const auto package = workspace.root / "strict_server_package";
    const auto written = map::WritePackage(package, spec);
    Check("strict-v3-package-writer-accepts-engine-metadata", written.ok);
    map::PackageReport report;
    const auto loaded = map::LoadServerWorld(package, map::ValidationDepth::Full, report);
    Check("strict-v3-server-full-validation", loaded && report.Ok() && report.manifest.format_version == 3);
    Check("server-consumes-identical-layer-ids-tags-and-heights", loaded && loaded->layered_world &&
        map::EncodeLayeredWorld(*loaded->layered_world) == exported);
}

void TestPersistence(const TemporaryWorkspace& workspace)
{
    auto& manager = SceneManager::Instance();
    manager.CloseScene();
    manager.NewScene();
    const SceneData scene = Fixture();
    manager.SetCurrentSceneSnapshot(scene);
    const auto path = workspace.root / "authoring.scene";
    Check("actual-scene-manager-save", manager.SaveSceneAs(path.string()));
    manager.CloseScene();
    const bool loaded = manager.LoadScene(path.string());
    Check("actual-scene-manager-load", loaded);
    const auto* ground = Entity(manager.GetCurrentScene(), 10);
    const auto* excluded = Entity(manager.GetCurrentScene(), 13);
    Check("enabled-tag-mask-survives-save-load", loaded && ground && ground->layerAuthoring.enabled &&
        ground->layerAuthoring.tags == (map::VolumeTagGround | map::VolumeTagRoad));
    Check("disabled-tagged-settings-survive-save-load", loaded && excluded && !excluded->layerAuthoring.enabled &&
        excluded->layerAuthoring.tags == (map::VolumeTagBuilding | map::VolumeTagInterior));
    SceneLayerAuthoringResult restored;
    Check("loaded-scene-regenerates-layer-metadata", loaded && GenerateSceneLayers(manager.GetCurrentScene(), GeometryProvider, restored));
    const auto legacy = workspace.root / "legacy.scene";
    WriteText(legacy, R"({"version":1,"metadata":{"name":"legacy"},"terrain":null,"entities":[{"type":"mesh_entity","id":7,"name":"legacy","collider":{"enabled":true,"shape":"box"}}]})");
    manager.CloseScene();
    const bool legacyLoaded = manager.LoadScene(legacy.string());
    const auto* legacyEntity = Entity(manager.GetCurrentScene(), 7);
    Check("legacy-scene-without-layer-settings-loads", legacyLoaded && legacyEntity);
    Check("legacy-scene-defaults-to-opt-out", legacyLoaded && legacyEntity && !legacyEntity->layerAuthoring.enabled &&
        legacyEntity->layerAuthoring.tags == 0);
    const auto malformed = workspace.root / "malformed_tags.scene";
    WriteText(malformed, R"({"version":1,"entities":[{"type":"mesh_entity","id":7,"name":"invalid-tags","collider":{"shape":"box"},"layer_authoring":{"enabled":true,"tags":"ground"}}]})");
    manager.CloseScene();
    const bool malformedLoaded = manager.LoadScene(malformed.string());
    const auto* invalid = Entity(manager.GetCurrentScene(), 7);
    Check("malformed-tag-json-remains-invalid", malformedLoaded && invalid && invalid->layerAuthoring.tags ==
        std::numeric_limits<std::uint32_t>::max());
    SceneLayerAuthoringResult invalidResult;
    Check("malformed-scene-tags-cannot-generate-ground", malformedLoaded &&
        !GenerateSceneLayers(manager.GetCurrentScene(), GeometryProvider, invalidResult) && !invalidResult.errors.empty());
    manager.CloseScene();
}

void TestNegativeControls()
{
    const auto base = Fixture();
    CheckRejected("missing-mesh-geometry-rejected", base, {});
    auto scene = base;
    scene.meshEntities[0].hasRigidbody = true;
    scene.meshEntities[0].rigidbody.bodyType = physics::BodyType::Dynamic;
    CheckRejected("dynamic-collider-rejected", scene);
    scene.meshEntities[0].rigidbody.bodyType = physics::BodyType::Kinematic;
    CheckRejected("kinematic-collider-rejected", scene);
    scene.meshEntities[0].rigidbody.bodyType = physics::BodyType::Static;
    scene.meshEntities[0].rigidbody.useGravity = true;
    CheckRejected("static-with-gravity-auto-promoted-collider-rejected", scene);
    scene = base;
    scene.meshEntities[0].collider.trigger = true;
    CheckRejected("trigger-collider-rejected", scene);
    scene = base;
    scene.meshEntities[0].skinned = true;
    CheckRejected("skinned-collision-source-rejected", scene);
    scene = base;
    scene.meshEntities[0].collider.shape = physics::ColliderShape::Capsule;
    CheckRejected("unsupported-capsule-rejected", scene);
    scene = base;
    scene.meshEntities[0].collider.shape = physics::ColliderShape::ConvexHull;
    CheckRejected("unsupported-convex-hull-rejected", scene);
    scene = base;
    scene.meshEntities[0].layerAuthoring.tags = 1u << 31;
    CheckRejected("unknown-semantic-tags-rejected", scene);
    scene = base;
    scene.meshEntities[0].layerAuthoring.tags = 0;
    CheckRejected("enabled-source-without-semantic-tags-rejected", scene);
    scene = base;
    scene.meshEntities[0].position[0] = 31;
    CheckRejected("out-of-terrain-bounds-rejected", scene);
    scene = base;
    scene.meshEntities = {Box(10, "outside-positive-terrain-half", 0, map::VolumeTagGround)};
    scene.meshEntities[0].position[0] = 22;
    scene.meshEntities[0].position[2] = 8;
    scene.waterBodies.clear();
    CheckRejected("positive-outside-real-terrain-rejected", scene);
    scene = base;
    scene.waterBodies[0].shapeMask = {255};
    CheckRejected("water-mask-size-mismatch-rejected", scene);
    scene = base;
    scene.waterBodies[0].shapeMask.clear();
    CheckRejected("empty-water-mask-rejected", scene);
    scene = base;
    scene.waterBodies[0].maskWidth = 0;
    scene.waterBodies[0].maskHeight = 0;
    CheckRejected("zero-water-mask-dimensions-rejected", scene);
    scene = base;
    scene.waterBodies[0].shapeMask[1] = 0;
    CheckRejected("nonrectangular-water-mask-rejected", scene);
    scene = base;
    scene.waterBodies.push_back(scene.waterBodies.front());
    CheckRejected("duplicate-water-entity-id-rejected", scene);
    scene = base;
    scene.waterBodies[0].id = 0;
    CheckRejected("zero-water-entity-id-rejected", scene);
    scene = base;
    scene.meshEntities[2].position[1] = 1;
    CheckRejected("overlapping-ground-and-bridge-height-bands-rejected", scene);
    scene = base;
    scene.meshEntities[0].position[1] = std::numeric_limits<float>::quiet_NaN();
    CheckRejected("nonfinite-model-transform-rejected", scene);
}

} // namespace

int main()
{
    try {
        const TemporaryWorkspace workspace;
        TestTransforms();
        TestGenerationAndServer(workspace);
        TestPersistence(workspace);
        TestNegativeControls();
    } catch (const std::exception& error) {
        Check("unexpected-test-exception", false);
        std::cout << "test exception: " << error.what() << '\n';
    }
    std::cout << "SCENE LAYER AUTHORING summary: checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
