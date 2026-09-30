#include "SceneLayerGround.h"
#include "SceneWorldPackage.h"
#include "math/Quaternion.h"
#include "map/WorldPackage.h"
#include "physics/PhysicsWorld.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace {
namespace fs = std::filesystem;
namespace map = mx::map;
namespace physics = ixtreeme::physics;
int checks = 0, failures = 0;

void Check(const char* name, bool value)
{
    ++checks;
    if (!value) ++failures;
    std::cout << "SCENE LAYER GROUND " << name << ": " << (value ? "PASS" : "FAIL") << '\n';
}

bool Near(double actual, double expected, double tolerance = 0.0001)
{
    return std::isfinite(actual) && std::abs(actual - expected) <= tolerance;
}

bool SameState(const map::LayerGroundState& lhs, const map::LayerGroundState& rhs)
{
    return lhs.volume_id == rhs.volume_id && lhs.layer_id == rhs.layer_id &&
        lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
}

class TemporaryWorkspace
{
public:
    TemporaryWorkspace()
    {
        parent_ = fs::weakly_canonical(fs::temp_directory_path());
        root = parent_ / ("ixw_scene_ground_test_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        if (!fs::create_directory(root)) throw std::runtime_error("cannot create unique test workspace");
    }
    ~TemporaryWorkspace()
    {
        if (root.parent_path() == parent_ && root.filename().string().starts_with("ixw_scene_ground_test_")) {
            std::error_code ec;
            fs::remove_all(root, ec);
        }
    }
    fs::path root;
private:
    fs::path parent_;
};

SceneData EmptyFixture()
{
    SceneData scene;
    scene.name = "Explicit physical support, no inferred nearest floor";
    auto& terrain = scene.terrain;
    terrain.exists = true;
    terrain.widthMeters = terrain.depthMeters = 32;
    terrain.cellsX = terrain.cellsZ = 32;
    terrain.cellSizeMeters = 1;
    terrain.chunkSizeCells = 8;
    terrain.heightCmGrid.assign(33 * 33, -1000);
    terrain.attributes.assign(32 * 32, 0);
    return scene;
}

MeshSceneEntity Box(std::uint32_t id, const char* name, float height)
{
    MeshSceneEntity entity;
    entity.id = id;
    entity.name = name;
    entity.position[0] = -6;
    entity.position[1] = height - 0.5f;
    entity.position[2] = 6;
    entity.hasCollider = true;
    entity.collider.shape = physics::ColliderShape::Box;
    entity.collider.layer = physics::PhysicsLayer::StaticWorld;
    entity.collider.size[0] = entity.collider.size[2] = 8;
    entity.collider.size[1] = 1;
    entity.layerAuthoring = {true, map::VolumeTagGround};
    return entity;
}

const LayerCollisionGeometryProvider GeometryProvider = [](
    const MeshSceneEntity& entity, std::vector<std::array<float, 3>>& vertices,
    std::vector<std::uint32_t>& indices) {
    if (entity.id == 12) {
        vertices = {{0,0,0}, {8,0,0}, {8,0,8}, {0,0,8}};
        indices = {0,2,1, 0,3,2};
    } else if (entity.id == 20) {
        // Two disconnected exact components belonging to the same real mesh.
        vertices = {{2,2,-10}, {6,2,-10}, {6,2,-6}, {2,2,-6},
                    {8,4,-10}, {12,4,-10}, {12,4,-6}, {8,4,-6}};
        indices = {0,2,1, 0,3,2, 4,6,5, 4,7,6};
    } else if (entity.id == 30) {
        // Ascending and descending planes share every AABB/height-band bound.
        const bool descending = entity.name == "descending-ramp";
        const float left = descending ? 2.0f : 0.0f;
        const float right = descending ? 0.0f : 2.0f;
        vertices = {{0,left,0}, {8,right,0}, {8,right,8}, {0,left,8}};
        indices = {0,2,1, 0,3,2};
    } else return false;
    return true;
};

SceneData StackedFixture()
{
    auto scene = EmptyFixture();
    scene.meshEntities.push_back(Box(10,"ground",0));
    scene.meshEntities.push_back(Box(11,"negative-underpass",-6));
    auto bridge = Box(12,"upper-mesh-bridge",6);
    bridge.collider.shape = physics::ColliderShape::Mesh;
    bridge.position[0] = -10;
    bridge.position[1] = 6;
    bridge.position[2] = 2;
    bridge.layerAuthoring.tags = map::VolumeTagBridge | map::VolumeTagConnector;
    scene.meshEntities.push_back(bridge);
    auto mixed = Box(20,"mixed-mesh-components",0);
    mixed.collider.shape = physics::ColliderShape::Mesh;
    for (auto& coordinate : mixed.position) coordinate = 0;
    mixed.layerAuthoring.tags = map::VolumeTagRoad;
    scene.meshEntities.push_back(mixed);
    WaterBody water;
    water.id = 7;
    water.name = "water-without-ground-support";
    water.bboxMin[0] = water.bboxMin[1] = 4;
    water.bboxMax[0] = water.bboxMax[1] = 8;
    water.waterLevelY = -2;
    water.maskWidth = water.maskHeight = 2;
    water.shapeMask.assign(4,255);
    scene.waterBodies.push_back(water);
    return scene;
}

const map::LayerVolume* SourceVolume(const map::LayeredWorld& world,
    std::uint32_t source, std::uint32_t component = 0)
{
    const auto found = std::find_if(world.volumes.begin(), world.volumes.end(), [&](const auto& volume) {
        return volume.ground_support && volume.ground_support->source_id == source &&
            (component == 0 || volume.ground_support->component_id == component);
    });
    return found == world.volumes.end() ? nullptr : &*found;
}

const map::LayerVolume* WaterVolume(const map::LayeredWorld& world)
{
    const auto found = std::find_if(world.volumes.begin(),world.volumes.end(),[](const auto& volume) {
        return map::HasVolumeTag(volume.tags,map::VolumeTagWater);
    });
    return found == world.volumes.end() ? nullptr : &*found;
}

physics::BodyId CreatePhysicalBody(physics::PhysicsWorld& world, const MeshSceneEntity& entity)
{
    physics::PhysicsBodyDesc desc;
    desc.bodyType = desc.rigidbody.bodyType = physics::BodyType::Static;
    desc.rigidbody.useGravity = false;
    desc.collider = entity.collider;
    std::copy(std::begin(entity.position),std::end(entity.position),std::begin(desc.transform.position));
    std::copy(std::begin(entity.scale),std::end(entity.scale),std::begin(desc.scale));
    const auto rotation = ixtreeme::math::FromEulerRadians({entity.rotation[0],entity.rotation[1],entity.rotation[2]});
    desc.transform.rotation[0] = rotation.x;
    desc.transform.rotation[1] = rotation.y;
    desc.transform.rotation[2] = rotation.z;
    desc.transform.rotation[3] = rotation.w;
    if (entity.collider.shape == physics::ColliderShape::Mesh &&
        !GeometryProvider(entity,desc.meshVertices,desc.meshIndices)) return 0;
    return world.CreateBody(desc);
}

void CheckPhysicalOracle(const char* heightName, const char* normalName,
    const MeshSceneEntity& entity, const map::LayeredWorld& world, map::VolumeId volume,
    const std::vector<std::array<float, 2>>& points, double expectedSlopeX = 0, double expectedSlopeZ = 0)
{
#if defined(IXENGINE_PHYSICS_WITH_JOLT)
    physics::PhysicsWorld physicsWorld;
    const auto body = CreatePhysicalBody(physicsWorld,entity);
    bool heights = body != 0, normals = body != 0;
    const double normalLength = std::sqrt(1 + expectedSlopeX * expectedSlopeX + expectedSlopeZ * expectedSlopeZ);
    for (const auto& point : points) {
        const auto query = map::ResolveLayerGroundPlacement(world,volume,point[0],point[1]);
        const float origin[3] = {point[0],30,point[1]}, down[3] = {0,-1,0};
        physics::PhysicsRaycastHit hit;
        const bool ray = physicsWorld.Raycast(origin,down,60,hit) && hit.bodyId == body;
        heights = heights && query.Ok() && ray && Near(hit.position[1],query.state.z);
        normals = normals && ray && Near(hit.normal[0],-expectedSlopeX / normalLength) &&
            Near(hit.normal[1],1 / normalLength) && Near(hit.normal[2],-expectedSlopeZ / normalLength);
    }
    Check(heightName,heights);
    Check(normalName,normals);
#else
    (void)heightName; (void)normalName; (void)entity; (void)world; (void)volume;
    (void)points; (void)expectedSlopeX; (void)expectedSlopeZ;
    std::cout << "SCENE LAYER GROUND actual Jolt oracle: NOT_RUN (backend disabled)\n";
#endif
}

void TestStackedAndAdapter()
{
    const auto scene = StackedFixture();
    SceneLayerAuthoringResult result;
    const bool generated = GenerateSceneLayers(scene,GeometryProvider,result);
    Check("scene-cooks-six-exact-volumes-without-manual-height-bands",generated && result.world.volumes.size() == 6);
    if (!generated) {
        for (const auto& error : result.errors) std::cout << "generation error: " << error << '\n';
        return;
    }
    const auto* ground = SourceVolume(result.world,10);
    const auto* negative = SourceVolume(result.world,11);
    const auto* upper = SourceVolume(result.world,12);
    const auto* mixed1 = SourceVolume(result.world,20,1);
    const auto* mixed2 = SourceVolume(result.world,20,2);
    const auto* water = WaterVolume(result.world);
    Check("original-box-mesh-source-and-component-identities-retained",ground && negative && upper && mixed1 && mixed2 &&
        mixed1->id != mixed2->id && mixed1->ground_support->component_id != mixed2->ground_support->component_id);
    Check("water-retains-semantic-tags-but-has-no-floor-plane",water && !water->ground_support && !water->supports_ground_movement);
    if (!ground || !negative || !upper || !mixed1 || !mixed2 || !water) return;
    const auto lower = map::ResolveLayerGroundPlacement(result.world,ground->id,-8,4);
    const auto below = map::ResolveLayerGroundPlacement(result.world,negative->id,-8,4);
    const auto higher = map::ResolveLayerGroundPlacement(result.world,upper->id,-8,4);
    Check("explicit-volume-resolves-stacked-floors-at-identical-xy",lower.Ok() && below.Ok() && higher.Ok() &&
        Near(lower.state.z,0) && Near(below.state.z,-6) && Near(higher.state.z,6));
    const auto mixedGround1 = map::ResolveLayerGroundPlacement(result.world,mixed1->id,4,-8);
    const auto mixedGround2 = map::ResolveLayerGroundPlacement(result.world,mixed2->id,10,-8);
    Check("disconnected-mesh-components-resolve-their-own-surfaces",mixedGround1.Ok() && mixedGround2.Ok() &&
        Near(mixedGround1.state.z,2) && Near(mixedGround2.state.z,4));

    SceneLayerGround adapter(result.world);
    Check("new-adapter-is-unplaced-and-move-fails-closed",!adapter.HasState() &&
        adapter.Move(ground->id,-8,4).status == map::GroundSupportStatus::InvalidState && !adapter.HasState());
    const auto placed = adapter.Place(upper->id,-8,4);
    Check("canonical-negative-x-positive-engine-z-no-legacy-sign-flip",placed.Ok() && adapter.HasState() &&
        SameState(placed.state,adapter.State()) && adapter.State().x == -8 && adapter.State().y == 4 && adapter.State().z == 6 &&
        adapter.EnginePosition() == std::array<double,3>{-8,6,4});
    Check("canonical-coordinate-helper-roundtrip-preserves-sign",LayerToEngineCoordinates(EngineToLayerCoordinates({-8,6,4})) ==
        std::array<float,3>{-8,6,4});
    const auto moved = adapter.Move(upper->id,-4,8);
    Check("same-volume-move-commits-queried-height-and-engine-pose",moved.Ok() &&
        SameState(moved.state,adapter.State()) && adapter.EnginePosition() == std::array<double,3>{-4,6,8});
    const auto state = adapter.State();
    const auto enginePosition = adapter.EnginePosition();
    const auto outside = adapter.Move(upper->id,-2,8);
    Check("half-open-volume-boundary-rejects-without-state-mutation",outside.status == map::GroundSupportStatus::OutsideVolume &&
        SameState(outside.state,state) && SameState(adapter.State(),state) && adapter.EnginePosition() == enginePosition);
    const auto failedPlace = adapter.Place(9999,-4,8);
    Check("failed-placement-preserves-existing-canonical-and-engine-state",failedPlace.status == map::GroundSupportStatus::UnknownVolume &&
        SameState(failedPlace.state,state) && SameState(adapter.State(),state) && adapter.EnginePosition() == enginePosition);
    const auto invalidMove = adapter.Move(upper->id,std::numeric_limits<double>::quiet_NaN(),8);
    Check("nonfinite-target-fails-transactionally",invalidMove.status == map::GroundSupportStatus::InvalidState &&
        SameState(invalidMove.state,state) && SameState(adapter.State(),state) && adapter.EnginePosition() == enginePosition);
    const auto cross = adapter.Move(ground->id,-4,8);
    Check("same-xy-cross-volume-move-requires-separate-transition",cross.status == map::GroundSupportStatus::TransitionRequired &&
        SameState(cross.state,state) && SameState(adapter.State(),state) && adapter.EnginePosition() == enginePosition);
    Check("temporary-world-constructor-is-disabled",!std::is_constructible_v<SceneLayerGround,map::LayeredWorld&&> &&
        !std::is_constructible_v<SceneLayerGround,const map::LayeredWorld&&>);

    // Both instances remain bound to distinct immutable worlds. Re-bake/load
    // requires an explicit new placement, even if structural ids happen to match.
    auto revisedWorld = result.world;
    for (auto& volume : revisedWorld.volumes) if (volume.id == upper->id) {
        volume.min_z += 8;
        volume.max_z += 8;
        volume.ground_support->anchor_z += 8;
    }
    SceneLayerGround revised(revisedWorld);
    Check("new-world-instance-never-inherits-old-ground-state",!revised.HasState() &&
        revised.Move(upper->id,-4,8).status == map::GroundSupportStatus::InvalidState);
    Check("new-world-requires-place-and-old-world-remains-independent",revised.Place(upper->id,-4,8).Ok() &&
        revised.EnginePosition() == std::array<double,3>{-4,14,8} && adapter.EnginePosition() == enginePosition);

    CheckPhysicalOracle("ground-box-height-agrees-with-actual-jolt","ground-box-normal-agrees-with-actual-jolt",
        scene.meshEntities[0],result.world,ground->id,{{-8,4},{-6,6},{-4,8}});
    CheckPhysicalOracle("negative-box-height-agrees-with-actual-jolt","negative-box-normal-agrees-with-actual-jolt",
        scene.meshEntities[1],result.world,negative->id,{{-8,4},{-6,6}});
    CheckPhysicalOracle("upper-mesh-height-agrees-with-actual-jolt","upper-mesh-normal-agrees-with-actual-jolt",
        scene.meshEntities[2],result.world,upper->id,{{-8,4},{-6,6},{-4,8}});
    CheckPhysicalOracle("mixed-component-height-agrees-with-actual-jolt","mixed-component-normal-agrees-with-actual-jolt",
        scene.meshEntities[3],result.world,mixed1->id,{{3,-9},{5,-7}});
}

void TestNegativeControls()
{
    SceneLayerAuthoringResult baked;
    if (!GenerateSceneLayers(StackedFixture(),GeometryProvider,baked)) {
        Check("negative-controls-require-valid-bake",false);
        return;
    }
    const auto* ground = SourceVolume(baked.world,10);
    const auto* negative = SourceVolume(baked.world,11);
    const auto* upper = SourceVolume(baked.world,12);
    const auto* water = WaterVolume(baked.world);
    if (!ground || !negative || !upper || !water) { Check("negative-control-source-identities",false); return; }
    const auto placed = map::ResolveLayerGroundPlacement(baked.world,ground->id,-6,6);
    auto fake = placed.state;
    fake.z = 0.5; // Still inside occupancy AABB, but not on its physical plane.
    const auto fakeHeight = map::ResolveLayerGroundMove(baked.world,fake,ground->id,-5,7);
    Check("occupancy-band-does-not-authorize-fabricated-current-height",fakeHeight.status == map::GroundSupportStatus::InvalidState &&
        SameState(fakeHeight.state,fake));
    fake = placed.state;
    fake.layer_id = 9999;
    const auto fakeLayer = map::ResolveLayerGroundMove(baked.world,fake,ground->id,-5,7);
    Check("fabricated-current-layer-is-rejected-without-snapping",fakeLayer.status == map::GroundSupportStatus::InvalidState &&
        SameState(fakeLayer.state,fake));
    fake = placed.state;
    fake.volume_id = negative->id;
    fake.layer_id = negative->layer_id;
    const auto fakeSource = map::ResolveLayerGroundMove(baked.world,fake,negative->id,-5,7);
    Check("changing-source-volume-id-cannot-rebind-current-height",fakeSource.status == map::GroundSupportStatus::InvalidState &&
        SameState(fakeSource.state,fake));
    fake = placed.state;
    fake.x = -1;
    const auto fakePoint = map::ResolveLayerGroundMove(baked.world,fake,ground->id,-5,7);
    Check("current-point-outside-source-footprint-rejected-verbatim",fakePoint.status == map::GroundSupportStatus::OutsideVolume &&
        SameState(fakePoint.state,fake));
    Check("unknown-volume-never-selects-nearest-floor",map::ResolveLayerGroundPlacement(baked.world,9999,-6,6).status ==
        map::GroundSupportStatus::UnknownVolume);
    const map::LayeredWorld emptyWorld;
    Check("legacy-empty-world-has-no-invented-support",map::ResolveLayerGroundPlacement(emptyWorld,ground->id,-6,6).status ==
        map::GroundSupportStatus::NotAvailable);
    auto legacy = baked.world;
    for (auto& volume : legacy.volumes) volume.ground_support.reset();
    Check("legacy-occupancy-band-is-support-unknown",map::ResolveLayerGroundPlacement(legacy,ground->id,-6,6).status ==
        map::GroundSupportStatus::NotAvailable);
    Check("ordinary-water-cannot-be-ground",map::ResolveLayerGroundPlacement(baked.world,water->id,6,6).status ==
        map::GroundSupportStatus::UnsupportedMovement);
    auto maliciousWater = baked.world;
    for (auto& volume : maliciousWater.volumes) if (volume.id == water->id) volume.supports_ground_movement = true;
    Check("water-flag-true-cannot-override-semantic-policy",map::ResolveLayerGroundPlacement(maliciousWater,water->id,6,6).status ==
        map::GroundSupportStatus::UnsupportedMovement);
    auto badPlane = baked.world;
    for (auto& volume : badPlane.volumes) if (volume.id == ground->id) volume.ground_support->source_id = 0;
    std::string error;
    Check("invalid-plane-source-identity-fails-validation-and-query",!badPlane.Validate(baked.worldBounds,error) &&
        map::ResolveLayerGroundPlacement(badPlane,ground->id,-6,6).status == map::GroundSupportStatus::InvalidState);

    auto portals = baked.world;
    map::LayerPortal portal;
    portal.id = 1;
    portal.source_volume = ground->id;
    portal.target_volume = upper->id;
    portal.source_bounds = portal.target_bounds = {-7,5,-5,7};
    portal.source_min_z = 0;
    portal.source_max_z = 0.1f;
    portal.target_min_z = 6;
    portal.target_max_z = 6.1f;
    portals.portals.push_back(portal);
    Check("portal-negative-control-uses-valid-traversable-contract",portals.Validate(baked.worldBounds,error) &&
        portals.CanTraverse(portal,-6,6,0,-6,6,6));
    const auto portalCross = map::ResolveLayerGroundMove(portals,placed.state,upper->id,-6,6);
    Check("valid-portal-is-not-automatic-ground-move-permission",portalCross.status == map::GroundSupportStatus::TransitionRequired &&
        SameState(portalCross.state,placed.state));
}

void TestRampAndTransformedBox()
{
    std::array<SceneLayerAuthoringResult,2> rampResults;
    std::array<SceneData,2> rampScenes;
    for (std::size_t index = 0; index < 2; ++index) {
        auto& scene = rampScenes[index];
        scene = EmptyFixture();
        auto ramp = Box(30,index == 0 ? "ascending-ramp" : "descending-ramp",0);
        ramp.collider.shape = physics::ColliderShape::Mesh;
        ramp.position[0] = -10;
        ramp.position[1] = 4;
        ramp.position[2] = 2;
        ramp.layerAuthoring.tags = map::VolumeTagRoad | map::VolumeTagConnector;
        scene.meshEntities.push_back(ramp);
    }
    const bool ascending = GenerateSceneLayers(rampScenes[0],GeometryProvider,rampResults[0]);
    const bool descending = GenerateSceneLayers(rampScenes[1],GeometryProvider,rampResults[1]);
    Check("opposing-ramp-collision-scenes-cook",ascending && descending && rampResults[0].world.volumes.size() == 1 &&
        rampResults[1].world.volumes.size() == 1);
    if (ascending && descending) {
        const auto& a = rampResults[0].world.volumes[0];
        const auto& b = rampResults[1].world.volumes[0];
        Check("same-aabb-and-band-control-has-distinct-proven-planes",a.bounds.min_x == b.bounds.min_x &&
            a.bounds.max_x == b.bounds.max_x && a.bounds.min_y == b.bounds.min_y && a.bounds.max_y == b.bounds.max_y &&
            a.min_z == b.min_z && a.max_z == b.max_z && a.ground_support && b.ground_support &&
            a.ground_support->slope_x != b.ground_support->slope_x);
        SceneLayerGround up(rampResults[0].world), down(rampResults[1].world);
        const auto upPlace = up.Place(a.id,-8,4), downPlace = down.Place(b.id,-8,4);
        Check("support-height-cannot-be-derived-from-aabb-midpoint",upPlace.Ok() && downPlace.Ok() &&
            Near(upPlace.state.z,4.5) && Near(downPlace.state.z,5.5) && !Near(upPlace.state.z,downPlace.state.z));
        const auto moved = up.Move(a.id,-4,8);
        Check("ramp-move-follows-analytic-plane-not-old-height",moved.Ok() && Near(moved.state.z,5.5) &&
            up.EnginePosition() == std::array<double,3>{-4,5.5,8});
        CheckPhysicalOracle("ascending-ramp-actual-jolt-height","ascending-ramp-analytic-jolt-normal",
            rampScenes[0].meshEntities[0],rampResults[0].world,a.id,{{-8,4},{-6,6},{-4,8}},0.25);
        CheckPhysicalOracle("descending-ramp-actual-jolt-height","descending-ramp-analytic-jolt-normal",
            rampScenes[1].meshEntities[0],rampResults[1].world,b.id,{{-8,4},{-6,6},{-4,8}},-0.25);
    }
    auto boxScene = EmptyFixture();
    auto box = Box(40,"transformed-static-box",0);
    box.position[1] = 1;
    box.collider.size[0] = 2;
    box.collider.size[1] = 1;
    box.collider.size[2] = 4;
    box.collider.center[0] = 0.5f;
    box.collider.center[1] = 1;
    box.collider.center[2] = -0.25f;
    box.scale[0] = -2;
    box.scale[1] = 3;
    box.scale[2] = 0.5f;
    boxScene.meshEntities.push_back(box);
    SceneLayerAuthoringResult boxResult;
    const bool boxGenerated = GenerateSceneLayers(boxScene,GeometryProvider,boxResult);
    Check("box-center-absolute-nonuniform-scale-support-cooks",boxGenerated && boxResult.world.volumes.size() == 1);
    if (boxGenerated) {
        const auto& volume = boxResult.world.volumes[0];
        const auto height = map::ResolveLayerGroundPlacement(boxResult.world,volume.id,-5,6);
        Check("transformed-box-height-agrees-with-independent-analytic-surface",height.Ok() && Near(height.state.z,5.5) &&
            volume.bounds.min_x == -7 && volume.bounds.max_x == -3 &&
            volume.bounds.min_y == 4.875f && volume.bounds.max_y == 6.875f);
        CheckPhysicalOracle("transformed-box-support-agrees-with-actual-jolt","transformed-box-normal-agrees-with-actual-jolt",
            box,boxResult.world,volume.id,{{-6,5.5f},{-5,6},{-4,6.5f}});
    }
}

void TestBakeIdentitySafety()
{
    SceneLayerAuthoringResult baked;
    const bool generated = GenerateSceneLayers(StackedFixture(),GeometryProvider,baked);
    Check("world-identity-controls-require-valid-bake",generated);
    if (!generated) return;
    SceneLayerGround adapter(baked.world);
    const auto identical = baked.world;
    Check("world-identity-matches-self-and-encodable-copy",adapter.MatchesWorld(baked.world) && adapter.MatchesWorld(identical));
    auto edited = baked.world;
    edited.volumes.front().ground_support->anchor_z += 0.25;
    Check("edited-physical-plane-invalidates-world-identity",!adapter.MatchesWorld(edited));

    auto invalidWorld = baked.world;
    invalidWorld.volumes.front().name.assign(map::kMaxLayeredWorldNameBytes + 1,'a');
    auto otherInvalid = invalidWorld;
    otherInvalid.volumes.front().name.back() = 'b';
    otherInvalid.volumes.front().ground_support->anchor_z += 0.25;
    std::string error;
    Check("long-name-control-has-valid-geometry-but-two-empty-codec-results",
        invalidWorld.Validate(baked.worldBounds,error) && otherInvalid.Validate(baked.worldBounds,error) &&
        map::EncodeLayeredWorld(invalidWorld).empty() && map::EncodeLayeredWorld(otherInvalid).empty());
    SceneLayerGround invalidAdapter(invalidWorld);
    Check("two-empty-encodings-never-match-stale-bake",!invalidAdapter.MatchesWorld(invalidWorld) &&
        !invalidAdapter.MatchesWorld(otherInvalid));
    Check("failed-encoding-never-matches-either-valid-world-direction",!adapter.MatchesWorld(invalidWorld) &&
        !invalidAdapter.MatchesWorld(baked.world));
    const auto* ground = SourceVolume(baked.world,10);
    if (!ground) { Check("world-identity-controls-require-ground",false); return; }
    const auto placed = adapter.Place(ground->id,-6,6);
    const auto pose = adapter.EnginePosition();
    Check("world-identity-mismatch-cannot-mutate-grounded-pose",placed.Ok() && !adapter.MatchesWorld(otherInvalid) &&
        SameState(adapter.State(),placed.state) && adapter.EnginePosition() == pose);
}

void TestStrictPackageRoundtrip(const TemporaryWorkspace& workspace)
{
    const auto scene = StackedFixture();
    SceneWorldPackageResult result;
    const auto path = workspace.root / "exact_scene_support_package";
    const bool exported = ExportSceneServerWorld(path,scene,GeometryProvider,{"exact_ground_support",-14,-14},result);
    Check("actual-scene-export-publishes-full-strict-support-package",exported && result.errors.empty() &&
        result.layers.world.volumes.size() == 6);
    if (!exported) {
        for (const auto& error : result.errors) std::cout << "export error: " << error << '\n';
        return;
    }
    std::ifstream file(path / map::kLayeredWorldFile,std::ios::binary);
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
    const std::uint32_t version = bytes.size() >= 8 ? static_cast<std::uint32_t>(bytes[4]) |
        static_cast<std::uint32_t>(bytes[5]) << 8 | static_cast<std::uint32_t>(bytes[6]) << 16 |
        static_cast<std::uint32_t>(bytes[7]) << 24 : 0;
    // 3D-4B: scene bakes carry the clearance proof, so the sidecar is v4.
    Check("scene-support-sidecar-uses-explicit-mx3d-v4-with-clearance",version == 4 &&
        version == map::kLayeredWorldFileVersion && result.layers.world.clearance_profile.has_value());
    map::LoadOptions options;
    options.depth = map::ValidationDepth::Full;
    options.residency = map::ResidencyMode::Eager;
    options.warp_policy = map::WarpPolicy::Strict;
    map::PackageReport report;
    const auto loaded = map::LoadServerWorld(path,options,report);
    Check("independent-server-loader-accepts-full-eager-strict-package",loaded && report.Ok() && loaded->layered_world);
    if (!loaded || !loaded->layered_world) return;
    Check("support-plane-source-component-and-volume-ids-roundtrip-exactly",map::EncodeLayeredWorld(*loaded->layered_world) ==
        map::EncodeLayeredWorld(result.layers.world));
    const auto* upper = SourceVolume(*loaded->layered_world,12);
    const auto* below = SourceVolume(*loaded->layered_world,11);
    Check("loaded-package-retains-upper-and-negative-support-identities",upper && below);
    if (!upper || !below) return;
    SceneLayerGround adapter(*loaded->layered_world);
    const auto upperPlace = adapter.Place(upper->id,-8,4);
    Check("loaded-scene-world-query-matches-authoring-and-analytic-upper-floor",upperPlace.Ok() && Near(upperPlace.state.z,6) &&
        adapter.EnginePosition() == std::array<double,3>{-8,6,4} && Near(loaded->terrain.Height(-8,4).meters,-10));
    const auto negativePlace = adapter.Place(below->id,-8,4);
    Check("loaded-negative-floor-stays-explicit-above-independent-terrain",negativePlace.Ok() &&
        adapter.EnginePosition() == std::array<double,3>{-8,-6,4});
    CheckPhysicalOracle("strict-loaded-upper-floor-actual-jolt-oracle","strict-loaded-upper-floor-actual-jolt-normal",
        scene.meshEntities[2],*loaded->layered_world,upper->id,{{-8,4},{-4,8}});
}

// Read-only verification of a real editor export. The package loader is the
// independent consumer; these queries prove loaded support semantics, not
// physical clearance or a Jolt comparison without the original scene assets.
// 3D-4B checks on a loaded v4 package: the editor's actor profile, a cooked
// grid per supported volume, blocked cells refusing the actor where support
// alone still answers, and every proven portal crossable straight across.
void ValidateExternalClearance(const map::LayeredWorld& world)
{
    const auto expected = SceneLayerClearanceProfile();
    const bool profile = world.clearance_profile &&
        world.clearance_profile->cell_size_m == expected.cell_size_m &&
        world.clearance_profile->actor_radius_m == expected.actor_radius_m &&
        world.clearance_profile->actor_height_m == expected.actor_height_m &&
        world.clearance_profile->step_height_m == expected.step_height_m &&
        world.clearance_profile->floor_contact_m == expected.floor_contact_m;
    Check("external-v4-carries-editor-actor-profile", profile);
    if (!profile) return;
    const double cell = world.clearance_profile->cell_size_m;
    std::size_t graded = 0, blockedCells = 0, blockedRefused = 0;
    for (const auto& volume : world.volumes) {
        if (!volume.ground_support || !volume.AllowsGroundMovement()) continue;
        if (volume.clearance) ++graded;
        if (!volume.clearance || blockedRefused > 0) continue;
        for (std::uint32_t j = 0; j < volume.clearance->cells_y && blockedRefused == 0; ++j)
            for (std::uint32_t i = 0; i < volume.clearance->cells_x && blockedRefused == 0; ++i) {
                if (!volume.clearance->Blocked(i, j)) continue;
                ++blockedCells;
                const double x = volume.bounds.min_x + (i + 0.5) * cell;
                const double z = volume.bounds.min_y + (j + 0.5) * cell;
                SceneLayerGround support(world), actor(world, SceneLayerActorProfile());
                if (support.Place(volume.id, x, z).Ok() &&
                    actor.Place(volume.id, x, z).status == map::GroundSupportStatus::Blocked)
                    ++blockedRefused;
            }
    }
    const auto walkable = static_cast<std::size_t>(std::count_if(world.volumes.begin(), world.volumes.end(),
        [](const auto& volume) { return volume.ground_support && volume.AllowsGroundMovement(); }));
    Check("external-every-supported-volume-has-clearance", graded == walkable && graded > 0);
    Check("external-blocked-cell-refuses-actor-where-support-answers", blockedCells == 0 || blockedRefused > 0);

    std::size_t proven = 0, crossed = 0;
    for (const auto& portal : world.portals) {
        if (!portal.proof) continue;
        ++proven;
        const auto& proof = *portal.proof;
        const map::LayerVolume* a = nullptr;
        const map::LayerVolume* b = nullptr;
        for (const auto& volume : world.volumes) {
            if (volume.id == portal.source_volume) a = &volume;
            if (volume.id == portal.target_volume) b = &volume;
        }
        if (!a || !b) continue;
        const double aMax = proof.axis == 0 ? a->bounds.max_x : a->bounds.max_y;
        const double sideA = aMax <= proof.edge ? -1.0 : 1.0;
        bool ok = false;
        for (std::uint32_t k = 0; k < proof.slots && !ok; ++k) {
            // Middle slot first, then alternating outwards.
            const std::int64_t middle = proof.slots / 2;
            const std::int64_t slot = middle + ((k % 2) ? -1 : 1) * static_cast<std::int64_t>((k + 1) / 2);
            if (slot < 0 || slot >= static_cast<std::int64_t>(proof.slots)) continue;
            const double along = std::min(static_cast<double>(proof.span_min) + (slot + 0.5) * cell,
                                          static_cast<double>(proof.span_max) - 0.5 * cell);
            const double from = proof.edge + sideA * 0.3, to = proof.edge - sideA * 0.3;
            SceneLayerGround walker(world, SceneLayerActorProfile());
            const auto start = proof.axis == 0 ? walker.Place(a->id, from, along) : walker.Place(a->id, along, from);
            if (!start.Ok()) continue;
            const auto step = proof.axis == 0 ? walker.Move(b->id, to, along) : walker.Move(b->id, along, to);
            ok = step.Ok() && step.portal_id == portal.id && walker.State().volume_id == b->id;
        }
        if (ok) ++crossed;
    }
    Check("external-every-proven-portal-crossable-by-baked-actor", crossed == proven);
    std::cout << "EXTERNAL CLEARANCE summary: graded=" << graded << " walkable=" << walkable <<
        " proven_portals=" << proven << " crossed=" << crossed << " blocked_refused=" << blockedRefused << '\n';
}

void ValidateExternalPackage(const fs::path& path)
{
    map::LoadOptions options;
    options.depth = map::ValidationDepth::Full;
    options.residency = map::ResidencyMode::Eager;
    options.warp_policy = map::WarpPolicy::Strict;
    map::PackageReport report;
    const auto loaded = map::LoadServerWorld(path,options,report);
    Check("external-package-full-eager-strict-load",loaded && report.Ok() && loaded->layered_world);
    if (!loaded || !loaded->layered_world) {
        for (const auto& issue : report.issues) std::cout << issue.Format() << '\n';
        return;
    }
    std::ifstream file(path / map::kLayeredWorldFile,std::ios::binary);
    std::array<unsigned char,8> header{};
    file.read(reinterpret_cast<char*>(header.data()),static_cast<std::streamsize>(header.size()));
    const auto version = static_cast<std::uint32_t>(header[4]) |
        static_cast<std::uint32_t>(header[5]) << 8 | static_cast<std::uint32_t>(header[6]) << 16 |
        static_cast<std::uint32_t>(header[7]) << 24;
    Check("external-editor-support-sidecar-is-mx3d-v3-or-v4",file && (version == 3 || version == 4));
    const auto& world = *loaded->layered_world;
    SceneLayerGround adapter(world);
    std::size_t supported = 0, unavailable = 0, unsupported = 0;
    std::cout << std::setprecision(17);
    for (const auto& volume : world.volumes) {
        const double x = (static_cast<double>(volume.bounds.min_x) + volume.bounds.max_x) * 0.5;
        const double z = (static_cast<double>(volume.bounds.min_y) + volume.bounds.max_y) * 0.5;
        const auto previous = adapter.State();
        const auto enginePosition = adapter.EnginePosition();
        const auto placed = adapter.Place(volume.id,x,z);
        if (!volume.AllowsGroundMovement()) {
            ++unsupported;
            Check("external-water-or-unsupported-volume-explicit-rejection",
                placed.status == map::GroundSupportStatus::UnsupportedMovement &&
                SameState(placed.state,previous) && SameState(adapter.State(),previous) &&
                adapter.EnginePosition() == enginePosition);
            std::cout << "EXTERNAL SUPPORT volume=" << volume.id << " layer=" << volume.layer_id <<
                " kind=" << map::ToString(volume.kind) << " status=" << map::ToString(placed.status) <<
                " supports_ground_flag=" << volume.supports_ground_movement << '\n';
        } else if (!volume.ground_support) {
            ++unavailable;
            Check("external-plane-unknown-volume-remains-support-unavailable",
                placed.status == map::GroundSupportStatus::NotAvailable && SameState(adapter.State(),previous) &&
                adapter.EnginePosition() == enginePosition);
            std::cout << "EXTERNAL SUPPORT volume=" << volume.id << " layer=" << volume.layer_id <<
                " status=" << map::ToString(placed.status) << " source=unknown component=unknown\n";
        } else {
            ++supported;
            const auto& plane = *volume.ground_support;
            const double expectedHeight = plane.anchor_z + plane.slope_x * (x - plane.anchor_x) +
                plane.slope_y * (z - plane.anchor_y);
            Check("external-loaded-support-centre-placement",placed.Ok() && volume.HasValidGroundSupport() &&
                placed.state.volume_id == volume.id && placed.state.layer_id == volume.layer_id &&
                placed.state.x == x && placed.state.y == z && placed.state.z == expectedHeight &&
                placed.max_height_error_m == plane.max_height_error_m &&
                adapter.EnginePosition() == std::array<double,3>{x,expectedHeight,z});
            const double moveX = x + (static_cast<double>(volume.bounds.max_x) - volume.bounds.min_x) * 0.125;
            const double moveZ = z + (static_cast<double>(volume.bounds.max_y) - volume.bounds.min_y) * 0.125;
            const double moveHeight = plane.anchor_z + plane.slope_x * (moveX - plane.anchor_x) +
                plane.slope_y * (moveZ - plane.anchor_y);
            const auto moved = adapter.Move(volume.id,moveX,moveZ);
            Check("external-loaded-support-same-volume-move",moved.Ok() &&
                moved.state.volume_id == volume.id && moved.state.layer_id == volume.layer_id &&
                moved.state.x == moveX && moved.state.y == moveZ && moved.state.z == moveHeight &&
                SameState(moved.state,adapter.State()) &&
                adapter.EnginePosition() == std::array<double,3>{moveX,moveHeight,moveZ});
            std::cout << "EXTERNAL SUPPORT volume=" << volume.id << " layer=" << volume.layer_id <<
                " source=" << plane.source_id << " component=" << plane.component_id <<
                " centre_x=" << x << " centre_z=" << z << " height_m=" << placed.state.z <<
                " max_height_error_m=" << plane.max_height_error_m << " move_height_m=" << moved.state.z <<
                " place=" << map::ToString(placed.status) << " move=" << map::ToString(moved.status) << '\n';
        }
    }
    Check("external-v3-package-has-proven-ground-support",supported > 0);
    const auto previous = adapter.State();
    const auto unknown = adapter.Place(0,0,0);
    Check("external-unknown-volume-explicit-rejection",unknown.status == map::GroundSupportStatus::UnknownVolume &&
        SameState(unknown.state,previous) && SameState(adapter.State(),previous));
    std::cout << "EXTERNAL SUPPORT summary: volumes=" << world.volumes.size() << " supported=" << supported <<
        " unavailable=" << unavailable << " unsupported=" << unsupported << " MX3D=" << version << '\n';
    if (version == 4) ValidateExternalClearance(world);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 1 && (argc != 3 || std::string(argv[1]) != "--validate-package")) {
        std::cerr << "usage: SceneLayerGroundTest [--validate-package <path>]\n";
        return 2;
    }
    try {
        if (argc == 3) ValidateExternalPackage(fs::path(argv[2]));
        else {
            const TemporaryWorkspace workspace;
            TestStackedAndAdapter();
            TestNegativeControls();
            TestBakeIdentitySafety();
            TestRampAndTransformedBox();
            TestStrictPackageRoundtrip(workspace);
        }
    } catch (const std::exception& error) {
        Check("unexpected-test-exception",false);
        std::cout << "test exception: " << error.what() << '\n';
    }
    std::cout << "SCENE LAYER GROUND summary: checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
