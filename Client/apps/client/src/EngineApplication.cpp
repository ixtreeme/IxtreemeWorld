#if defined(_WIN32)
#include <windows.h>
#endif

#include "EngineApplication.h"

#include "EditorSceneRuntime.h"
#include "AssetLibrary.h"
#include "AssetDatabase.h"
#include "AssimpExporter.h"
#include "AssimpImporter.h"
#include "Common.h"
#include "FbxAssetSidecars.h"
#include "WorldLabelRenderer.h"
#include "AssetWatcher.h"
#include "LODSystem.h"
#include "MeshSystem.h"
#include "NativeWindow.h"
#include "AnimationRuntime.h"
#include "AnimatorRuntime.h"
#include "EditorImGui.h"
#include "physics/PhysicsWorld.h"
#if defined(_WIN32)
#include "NativeWindow_Win32.h"
#endif
#if defined(__ANDROID__)
#include "NativeWindow_Android.h"
#endif
#include "OffscreenSceneRenderer.h"
#include "PrefabDocument.h"
#include "ProjectManager.h"
#include "RmlUiLayer.h"
#include "RuntimeSession.h"
#include "RuntimeUiAdapter.h"
#include "SceneManager.h"
#include "SelectionSystem.h"
#include "SelectionOutlineRenderer.h"
#include "SpatialIndex.h"
#include "StaticMeshRenderer.h"
#include "TerrainEditorSystem.h"
#include "TerrainRenderer.h"
#include "ViewportControls.h"
#include "VulkanDevice.h"
#include "SkinnedMeshRenderer.h"
#include "Debug.h"
#include "asset/IAssetReader.h"

#include <flecs.h>

#if defined(_WIN32)
#include "asset/FileAssetReader.h"
#endif
#if defined(__ANDROID__)
#include "asset/AAssetManagerAssetReader.h"
#include <android_native_app_glue.h>
#endif

#include <array>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
namespace xm = ixtreeme::math;
namespace prefab = ixtreeme::prefab;
namespace phys = ixtreeme::physics;

// Builds a render camera from a scene CameraEntity, matching the editor free-fly
// convention (rotation[0]=pitch, rotation[1]=yaw). Used to drive the Game view.
WorldCamera BuildCameraFromEntity(const CameraEntity& cameraEntity, uint32_t width, uint32_t height)
{
    const float aspect = height != 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
    const WorldVec3 eye{cameraEntity.position[0], cameraEntity.position[1], cameraEntity.position[2]};
    const WorldVec3 forward = WorldForwardFromYawPitch(cameraEntity.rotation[1], cameraEntity.rotation[0]);
    WorldCamera camera{};
    camera.eye = eye;
    camera.target = eye + forward;
    camera.nearPlane = cameraEntity.nearPlane;
    camera.farPlane = std::max(cameraEntity.nearPlane + 0.001f, cameraEntity.farPlane);
    const WorldMat4 view = WorldLookAt(camera.eye, camera.target, {0.0f, 1.0f, 0.0f});
    const WorldMat4 projection = WorldPerspective(
        xm::DegreesToRadians(std::clamp(cameraEntity.fovDegrees, 1.0f, 179.0f)),
        aspect,
        camera.nearPlane,
        camera.farPlane);
    camera.viewProjection = WorldMultiply(view, projection);
    return camera;
}

// Per-frame runtime state for a player character (persists across frames in Play mode).
struct CharacterRuntimeState
{
    bool initialized = false;
    WorldVec3 velocity{0.0f, 0.0f, 0.0f};
    bool grounded = false;
    float lookYaw = 0.0f;   // camera/heading yaw (radians)
    float lookPitch = 0.0f; // camera pitch (radians)
    RuntimeMoveState moveState = RuntimeMoveState::Idle; // drives walk/run animation
    float planarSpeed = 0.0f;        // continuous horizontal speed (m/s) — drives the Animator "Speed" param
    bool jumpedThisFrame = false;    // a jump was initiated this frame — drives the "Jump" trigger
};

// Advances one player character (kinematic capsule) for this frame: camera-relative WASD
// move + run, gravity, downward-raycast ground detection + jump, slope limit, and wall
// depenetration via capsule overlap. Writes the resolved transform back to the mesh; the
// existing kinematic sync in stepEditorPhysicsWorld then moves the Jolt body to match.
void UpdateCharacterController(
    phys::PhysicsWorld& world,
    phys::BodyId selfBody,
    MeshSceneEntity& mesh,
    const phys::CharacterControllerComponent& cc,
    CharacterRuntimeState& state,
    const MovementInputState& input,
    bool inputActive,
    float lookDx,
    float lookDy,
    const float worldGravity[3],
    float dt)
{
    if (dt <= 0.0f)
        return;
    dt = std::min(dt, 0.05f); // clamp for stability under frame spikes

    // --- mouse-look (right-drag deltas in pixels) ---
    const float sens = cc.mouseSensitivity;
    state.lookYaw += xm::DegreesToRadians(lookDx * sens);
    state.lookPitch -= xm::DegreesToRadians(lookDy * sens);
    const float pitchLimit = xm::DegreesToRadians(85.0f);
    state.lookPitch = std::clamp(state.lookPitch, -pitchLimit, pitchLimit);

    // --- camera-relative horizontal input ---
    const float yaw = state.lookYaw;
    const float fwdX = std::sin(yaw), fwdZ = std::cos(yaw);
    const float rightX = std::cos(yaw), rightZ = -std::sin(yaw);
    float fb = 0.0f, lr = 0.0f;
    if (inputActive)
    {
        fb = (input.w ? 1.0f : 0.0f) - (input.s ? 1.0f : 0.0f);
        lr = (input.d ? 1.0f : 0.0f) - (input.a ? 1.0f : 0.0f);
    }
    float moveX = fwdX * fb + rightX * lr;
    float moveZ = fwdZ * fb + rightZ * lr;
    const float moveLen = std::sqrt(moveX * moveX + moveZ * moveZ);
    if (moveLen > 0.0001f)
    {
        moveX /= moveLen;
        moveZ /= moveLen;
    }
    else
    {
        moveX = 0.0f;
        moveZ = 0.0f;
    }
    const float speed = (inputActive && input.shift) ? cc.runSpeed : cc.walkSpeed;

    // Animation state from movement intent: Run when sprinting + moving, Walk when moving,
    // else Idle. Drives the skinned mesh's walk/run/idle clip.
    if (moveLen > 0.0001f)
        state.moveState = (inputActive && input.shift) ? RuntimeMoveState::Running : RuntimeMoveState::Walking;
    else
        state.moveState = RuntimeMoveState::Idle;
    state.planarSpeed = (moveLen > 0.0001f) ? speed : 0.0f;
    state.jumpedThisFrame = false;

    // --- gravity / jump (vertical velocity) ---
    const float gY = worldGravity[1] * cc.gravityScale; // negative downward
    const bool jumpPressed = inputActive && input.space;
    if (state.grounded && jumpPressed)
    {
        state.velocity.y = std::sqrt(std::max(0.0f, -2.0f * gY * cc.jumpHeight));
        state.jumpedThisFrame = true;
    }
    else if (state.grounded)
        state.velocity.y = -2.0f; // small stick-down so the ground ray keeps contact
    else
        state.velocity.y += gY * dt;

    // --- integrate position (feet) ---
    float feetX = mesh.position[0] + moveX * speed * dt;
    float feetY = mesh.position[1] + state.velocity.y * dt;
    float feetZ = mesh.position[2] + moveZ * speed * dt;

    const float radius = cc.capsuleRadius;
    const float height = std::max(cc.capsuleHeight, radius * 2.0f + 0.01f);
    const float cylinderHalf = std::max(0.001f, height * 0.5f - radius);

    phys::PhysicsQueryFilter filter{};
    filter.hitTriggers = false;
    filter.ignoreBody = selfBody;

    // --- wall depenetration via capsule overlap (skip walkable floors) ---
    for (int iter = 0; iter < 3; ++iter)
    {
        const float center[3] = {feetX, feetY + height * 0.5f, feetZ};
        const std::vector<phys::PhysicsOverlapHit> hits =
            world.OverlapCapsule(center, cylinderHalf, radius, filter, 16);
        bool pushed = false;
        for (const phys::PhysicsOverlapHit& hit : hits)
        {
            if (hit.bodyId == selfBody || hit.normal[1] > 0.7f || hit.penetrationDepth <= 0.0f)
                continue;
            feetX += hit.normal[0] * hit.penetrationDepth;
            feetZ += hit.normal[2] * hit.penetrationDepth;
            pushed = true;
        }
        if (!pushed)
            break;
    }

    // --- ground detection (downward ray from capsule center) ---
    const float origin[3] = {feetX, feetY + height * 0.5f, feetZ};
    const float down[3] = {0.0f, -1.0f, 0.0f};
    const float maxDist = height * 0.5f + cc.stepHeight + 0.4f;
    phys::PhysicsRaycastHit groundHit{};
    bool grounded = false;
    if (world.Raycast(origin, down, maxDist, filter, groundHit) && groundHit.hit)
    {
        const float groundY = groundHit.position[1];
        const float walkableCos = std::cos(xm::DegreesToRadians(cc.slopeLimitDegrees));
        const float feetToGround = feetY - groundY;
        if (groundHit.normal[1] >= walkableCos &&
            feetToGround <= cc.stepHeight + 0.05f &&
            state.velocity.y <= 0.01f)
        {
            feetY = groundY; // snap onto ground / small step
            state.velocity.y = 0.0f;
            grounded = true;
        }
    }
    state.grounded = grounded;

    mesh.position[0] = feetX;
    mesh.position[1] = feetY;
    mesh.position[2] = feetZ;
    if (moveLen > 0.0001f)
        mesh.rotation[1] = std::atan2(moveX, moveZ); // face travel direction
}

// Builds the Game-view camera transform that follows a player character, per camera mode.
// Copies fov/near/far from the scene's Main Camera and overrides position/rotation.
CameraEntity ComputeCharacterCameraEntity(
    const CameraEntity& base,
    const MeshSceneEntity& mesh,
    const phys::CharacterControllerComponent& cc,
    const CharacterRuntimeState& state)
{
    CameraEntity cam = base;
    const float feetX = mesh.position[0], feetY = mesh.position[1], feetZ = mesh.position[2];
    const float yaw = state.lookYaw;
    if (cc.cameraMode == phys::CameraMode::FirstPerson)
    {
        cam.position[0] = feetX;
        cam.position[1] = feetY + cc.eyeHeight;
        cam.position[2] = feetZ;
        cam.rotation[0] = state.lookPitch;
        cam.rotation[1] = yaw;
        cam.rotation[2] = 0.0f;
    }
    else if (cc.cameraMode == phys::CameraMode::ThirdPerson)
    {
        const WorldVec3 dir = WorldForwardFromYawPitch(yaw, state.lookPitch);
        cam.position[0] = feetX - dir.x * cc.thirdPersonDistance;
        cam.position[1] = feetY + cc.thirdPersonHeight - dir.y * cc.thirdPersonDistance;
        cam.position[2] = feetZ - dir.z * cc.thirdPersonDistance;
        cam.rotation[0] = state.lookPitch;
        cam.rotation[1] = yaw;
        cam.rotation[2] = 0.0f;
    }
    else // TopDown
    {
        const float pitch = -xm::DegreesToRadians(cc.topDownPitchDegrees);
        const WorldVec3 dir = WorldForwardFromYawPitch(yaw, pitch);
        cam.position[0] = feetX - dir.x * cc.topDownHeight;
        cam.position[1] = feetY + 1.0f - dir.y * cc.topDownHeight;
        cam.position[2] = feetZ - dir.z * cc.topDownHeight;
        cam.rotation[0] = pitch;
        cam.rotation[1] = yaw;
        cam.rotation[2] = 0.0f;
    }
    return cam;
}

const char* InputEventTypeName(InputEvent::Type type)
{
    switch (type)
    {
    case InputEvent::MouseMove: return "MouseMove";
    case InputEvent::MouseDown: return "MouseDown";
    case InputEvent::MouseUp: return "MouseUp";
    case InputEvent::MouseWheel: return "MouseWheel";
    case InputEvent::KeyDown: return "KeyDown";
    case InputEvent::KeyUp: return "KeyUp";
    case InputEvent::Char: return "Char";
    case InputEvent::TouchDown: return "TouchDown";
    case InputEvent::TouchMove: return "TouchMove";
    case InputEvent::TouchUp: return "TouchUp";
    default: return "Unknown";
    }
}

const char* SculptDiagToolModeName(MapEditorToolMode mode)
{
    switch (mode)
    {
    case MapEditorToolMode::Heightmap: return "sculpt";
    case MapEditorToolMode::SplatPaint: return "splat";
    case MapEditorToolMode::WaterSculpt: return "water";
    case MapEditorToolMode::None:
    default: return "none";
    }
}

void LogUnhandledInput(const InputEvent& event)
{
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "Input not consumed by gameClient: %s\n",
        InputEventTypeName(event.type));
#if defined(_WIN32)
    OutputDebugStringA(buffer);
#endif
    std::fprintf(stderr, "%s", buffer);
}

void ShowFatal(const char* message)
{
#if defined(_WIN32)
    MessageBoxA(nullptr, message, "IxtreemeEngine", MB_ICONERROR);
#else
    std::fprintf(stderr, "%s\n", message);
#endif
}

float HeadingFromQuantized(std::uint16_t heading)
{
    return (static_cast<float>(heading) / 65535.0f) * xm::TwoPi;
}

struct FrameCpuProfile
{
    double assetLibraryPollMs = 0.0;
    double assetWatcherPollMs = 0.0;
    double ecsSystemsUpdateMs = 0.0;
    double hierarchyIterationMs = 0.0;
    double sceneRenderMs = 0.0;
    double editorUiRenderMs = 0.0;
    double submitPresentMs = 0.0;
    double totalCpuFrameMs = 0.0;
};

struct PhysicsDebugContact
{
    WorldVec3 point{};
    WorldVec3 normal{0.0f, 1.0f, 0.0f};
    std::array<float, 4> color{0.25f, 1.0f, 0.35f, 0.95f};
    float ttlSeconds = 0.25f;
};

struct PhysicsDebugLine
{
    WorldVec3 a{};
    WorldVec3 b{};
    std::array<float, 4> color{1.0f, 0.82f, 0.18f, 0.95f};
    float ttlSeconds = 2.5f;
};

struct PhysicsBodyEntityBinding
{
    std::uint32_t entityId = 0;
    std::string name;
    bool terrain = false;
};

struct PhysicsEntityEvent
{
    phys::PhysicsContactPhase phase = phys::PhysicsContactPhase::Started;
    phys::PhysicsContactKind kind = phys::PhysicsContactKind::Collision;
    std::uint32_t entityA = 0;
    std::uint32_t entityB = 0;
    std::string nameA;
    std::string nameB;
    phys::BodyId bodyA = 0;
    phys::BodyId bodyB = 0;
    WorldVec3 point{};
    WorldVec3 normal{0.0f, 1.0f, 0.0f};
    float penetrationDepth = 0.0f;
};

const char* PhysicsEventKindName(phys::PhysicsContactKind kind)
{
    return kind == phys::PhysicsContactKind::Trigger ? "Trigger" : "Collision";
}

const char* PhysicsEventPhaseName(phys::PhysicsContactPhase phase)
{
    switch (phase)
    {
    case phys::PhysicsContactPhase::Started: return "Enter";
    case phys::PhysicsContactPhase::Stayed: return "Stay";
    case phys::PhysicsContactPhase::Ended: return "Exit";
    default: return "Unknown";
    }
}

double MillisecondsBetween(std::chrono::steady_clock::time_point begin,
                           std::chrono::steady_clock::time_point end)
{
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

std::string EditorDisplayName(const WaterBody& body)
{
    return body.name.empty() ? "Water Body " + std::to_string(body.id) : body.name;
}

std::string EditorDisplayName(const PointLight& light)
{
    return light.name.empty() ? "Point Light " + std::to_string(light.id) : light.name;
}

std::string EditorDisplayName(const SpotLight& light)
{
    return light.name.empty() ? "Spot Light " + std::to_string(light.id) : light.name;
}

std::string EditorDisplayName(const MeshSceneEntity& mesh)
{
    return mesh.name.empty() ? "Mesh Entity " + std::to_string(mesh.id) : mesh.name;
}

std::string EditorDisplayName(const TerrainSceneData& terrain)
{
    return terrain.name.empty() ? "Terrain" : terrain.name;
}

std::string SceneParentTypeName(HierarchyEntityType type)
{
    switch (type)
    {
    case HierarchyEntityType::Terrain: return "terrain";
    case HierarchyEntityType::WaterBody: return "water_body";
    case HierarchyEntityType::PointLight: return "point_light";
    case HierarchyEntityType::SpotLight: return "spot_light";
    case HierarchyEntityType::MeshEntity: return "mesh_entity";
    case HierarchyEntityType::Camera: return "camera";
    case HierarchyEntityType::None:
    default: return {};
    }
}

HierarchyEntityType SceneParentTypeFromName(const std::string& type)
{
    if (type == "terrain")
        return HierarchyEntityType::Terrain;
    if (type == "water_body")
        return HierarchyEntityType::WaterBody;
    if (type == "point_light")
        return HierarchyEntityType::PointLight;
    if (type == "spot_light")
        return HierarchyEntityType::SpotLight;
    if (type == "mesh_entity")
        return HierarchyEntityType::MeshEntity;
    if (type == "camera")
        return HierarchyEntityType::Camera;
    return HierarchyEntityType::None;
}

SceneParentRef MakeSceneParentRef(HierarchyEntityType type, std::uint32_t id)
{
    SceneParentRef parent;
    parent.type = SceneParentTypeName(type);
    parent.id = id;
    return parent.IsValid() ? parent : SceneParentRef{};
}

MeshRendererEditorState BuildMeshRendererEditorState(const std::vector<MeshSceneEntity>& meshes,
                                                     std::uint32_t selectedId)
{
    MeshRendererEditorState state{};
    state.count = static_cast<std::uint32_t>(meshes.size());
    auto it = std::find_if(meshes.begin(), meshes.end(),
        [selectedId](const MeshSceneEntity& mesh) { return selectedId != 0 && mesh.id == selectedId; });
    if (it == meshes.end())
        return state;

    state.selected = true;
    state.id = it->id;
    state.name = EditorDisplayName(*it);
    state.prefabAssetId = it->prefabAssetId;
    state.prefabInstance = it->prefabInstance;
    state.prefabOverrides = it->prefabInstance.assetId.empty() && it->prefabAssetId.empty()
        ? std::vector<std::string>{}
        : std::vector<std::string>{"Transform"};
    state.meshAssetId = it->meshAssetId;
    state.meshAssetPath = it->meshAssetPath;
    state.meshDisplayName = it->meshAssetId.empty() ? it->meshAssetPath : it->meshAssetId;
    std::copy(std::begin(it->position), std::end(it->position), std::begin(state.position));
    std::copy(std::begin(it->rotation), std::end(it->rotation), std::begin(state.rotation));
    std::copy(std::begin(it->scale), std::end(it->scale), std::begin(state.scale));
    state.skinned = it->skinned;
    state.materialSlots = it->materialSlots;
    state.materialOverrides = it->materialOverrides;
    state.editorComponents = it->editorComponents;
    state.lod = it->lod;
    state.hasRigidbody = it->hasRigidbody;
    state.rigidbody = it->rigidbody;
    state.hasCollider = it->hasCollider;
    state.collider = it->collider;
    state.hasFixedJoint = it->hasFixedJoint;
    state.fixedJoint = it->fixedJoint;
    state.hasHingeJoint = it->hasHingeJoint;
    state.hingeJoint = it->hingeJoint;
    state.hasCharacterController = it->hasCharacterController;
    state.characterController = it->characterController;
    state.debugAnimationClipId = it->debugAnimationClipId;
    state.animatorControllerId = it->animatorControllerId;
    state.materialSlotCount = std::max<std::uint32_t>(
        1u,
        std::max(static_cast<std::uint32_t>(state.materialSlots.size()),
            static_cast<std::uint32_t>(state.materialOverrides.size())));
    state.selectedMaterialSlot = std::min(state.selectedMaterialSlot, state.materialSlotCount - 1u);
    return state;
}

void ApplyMeshRendererEditorState(MeshSceneEntity& mesh, const MeshRendererEditorState& state)
{
    mesh.name = state.name;
    mesh.prefabAssetId = state.prefabAssetId;
    mesh.prefabInstance = state.prefabInstance;
    mesh.meshAssetId = state.meshAssetId;
    mesh.meshAssetPath = state.meshAssetPath;
    std::copy(std::begin(state.position), std::end(state.position), std::begin(mesh.position));
    std::copy(std::begin(state.rotation), std::end(state.rotation), std::begin(mesh.rotation));
    std::copy(std::begin(state.scale), std::end(state.scale), std::begin(mesh.scale));
    mesh.skinned = state.skinned;
    mesh.materialSlots = state.materialSlots;
    mesh.materialOverrides = state.materialOverrides;
    mesh.editorComponents = state.editorComponents;
    mesh.lod = state.lod;
    mesh.hasRigidbody = state.hasRigidbody;
    mesh.rigidbody = state.rigidbody;
    mesh.hasCollider = state.hasCollider;
    mesh.collider = state.collider;
    mesh.hasFixedJoint = state.hasFixedJoint;
    mesh.fixedJoint = state.fixedJoint;
    mesh.hasHingeJoint = state.hasHingeJoint;
    mesh.hingeJoint = state.hingeJoint;
    mesh.hasCharacterController = state.hasCharacterController;
    mesh.characterController = state.characterController;
    mesh.debugAnimationClipId = state.debugAnimationClipId;
    mesh.animatorControllerId = state.animatorControllerId;
}

std::filesystem::path ResolveModelAssetPathForMeta(const std::string& meshAssetPath)
{
    if (meshAssetPath.empty())
        return {};
    if (meshAssetPath.rfind("builtin://primitive/", 0) == 0)
        return {};
    std::filesystem::path path(meshAssetPath);
    if (path.is_absolute())
        return path;
    ProjectManager& projects = ProjectManager::Instance();
    if (projects.HasProject())
    {
        const std::string generic = path.generic_string();
        if (generic == "Assets" || generic.rfind("Assets/", 0) == 0)
            path = projects.ProjectRoot() / path;
        else
            path = projects.AssetRootPath() / path;
    }
    else
    {
        path = std::filesystem::current_path() / path;
    }
    std::error_code ec;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
    return ec ? std::filesystem::absolute(path) : canonical;
}

std::uint32_t ResolveModelSubmeshCount(const std::string& meshAssetPath)
{
    if (meshAssetPath.rfind("builtin://primitive/", 0) == 0)
        return 1;
    static std::unordered_map<std::string, std::uint32_t> cachedCounts;
    const std::filesystem::path modelPath = ResolveModelAssetPathForMeta(meshAssetPath);
    if (modelPath.empty())
        return 0;
    const std::string key = modelPath.generic_string();
    if (const auto it = cachedCounts.find(key); it != cachedCounts.end())
        return it->second;

    std::uint32_t count = 0;
    std::string ext = modelPath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (ext == ".fbx")
    {
        AssimpImporter importer;
        const AssimpImporter::ImportResult result = importer.importFile(modelPath);
        if (result.success)
            count = static_cast<std::uint32_t>(result.meshes.size());
    }
    cachedCounts[key] = count;
    return count;
}

std::vector<std::string> LoadDefaultMaterialSlotGuids(const std::string& meshAssetPath, std::uint32_t desiredSlotCount = 0)
{
    std::vector<std::string> slots;
    if (meshAssetPath.rfind("builtin://primitive/", 0) == 0)
    {
        slots.resize(std::max<std::uint32_t>(1u, desiredSlotCount));
        return slots;
    }
    const std::filesystem::path modelPath = ResolveModelAssetPathForMeta(meshAssetPath);
    if (modelPath.empty())
        return slots;

    std::vector<Guid> defaults = AssetDatabase::Instance().loadDefaultMaterials(modelPath);
    if (defaults.empty())
    {
        static std::unordered_set<std::string> warnedLegacyMeta;
        const std::string key = modelPath.generic_string();
        if (warnedLegacyMeta.insert(key).second)
        {
            Tracenf("[MATERIAL-SLOTS] legacy_meta_no_defaultMaterials assetPath=%s",
                key.c_str());
        }

        if (ProjectManager::Instance().HasProject())
        {
            const std::filesystem::path materialFolder =
                ProjectManager::Instance().AssetRootPath() / "materials" / modelPath.stem();
            std::error_code ec;
            std::vector<std::filesystem::path> materialFiles;
            if (std::filesystem::exists(materialFolder, ec))
            {
                for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(materialFolder, ec))
                {
                    if (!entry.is_regular_file(ec) || entry.path().extension() != ".material")
                        continue;
                    materialFiles.push_back(entry.path());
                }
            }
            std::sort(materialFiles.begin(), materialFiles.end());
            for (const std::filesystem::path& materialPath : materialFiles)
                defaults.push_back(AssetDatabase::Instance().getOrCreateGuid(materialPath));
            if (!defaults.empty())
            {
                AssetDatabase::Instance().writeDefaultMaterials(modelPath, defaults);
                Tracenf("[MATERIAL-SLOTS] defaultMaterials_recorded model=%s count=%zu",
                    modelPath.filename().generic_string().c_str(),
                    defaults.size());
            }
        }
    }

    slots.reserve(defaults.size());
    for (const Guid& guid : defaults)
        slots.push_back(guid.toString());
    if (desiredSlotCount > 0)
    {
        if (slots.size() == 1 && desiredSlotCount > 1)
            slots.resize(desiredSlotCount, slots.front());
        else if (slots.size() < desiredSlotCount)
            slots.resize(desiredSlotCount);
        else if (slots.size() > desiredSlotCount)
        {
            Tracenf("[MATERIAL-SLOTS] defaultMaterials_extra_ignored model=%s defaults=%zu submeshes=%u",
                modelPath.filename().generic_string().c_str(),
                slots.size(),
                desiredSlotCount);
            slots.resize(desiredSlotCount);
        }
    }
    return slots;
}

void EnsureMeshEntityMaterialSlots(MeshSceneEntity& mesh)
{
    const std::uint32_t desiredSlotCount = ResolveModelSubmeshCount(mesh.meshAssetPath);
    if (!mesh.materialSlots.empty())
    {
        if (desiredSlotCount > mesh.materialSlots.size())
        {
            if (mesh.materialSlots.size() == 1)
                mesh.materialSlots.resize(desiredSlotCount, mesh.materialSlots.front());
            else
                mesh.materialSlots.resize(desiredSlotCount);
            Tracenf("[MATERIAL-SLOTS] entity_slots_expanded entity=%u count=%zu submeshes=%u",
                mesh.id,
                mesh.materialSlots.size(),
                desiredSlotCount);
        }
        return;
    }
    mesh.materialSlots = LoadDefaultMaterialSlotGuids(mesh.meshAssetPath, desiredSlotCount);
    if (!mesh.materialSlots.empty())
    {
        Tracenf("[MATERIAL-SLOTS] legacy_scene_auto_populated entity=%u from=meta count=%zu",
            mesh.id,
            mesh.materialSlots.size());
    }
}

std::optional<WorldVec3> RaycastTerrainPoint(const TerrainRenderer& terrain,
                                             const WorldCamera& camera,
                                             uint32_t width,
                                             uint32_t height,
                                             int mouseX,
                                             int mouseY)
{
    if (width == 0 || height == 0)
        return std::nullopt;

    const WorldVec3 rayDir = ScreenRayDirection(camera, width, height, mouseX, mouseY);
    constexpr float kStepMeters = 0.5f;
    constexpr float kMaxDistanceMeters = 700.0f;
    float previousT = 0.0f;
    float previousDelta = camera.eye.y - terrain.SampleHeight(camera.eye);
    for (float t = kStepMeters; t <= kMaxDistanceMeters; t += kStepMeters)
    {
        const WorldVec3 p = camera.eye + rayDir * t;
        const float terrainY = terrain.SampleHeight(p);
        const float delta = p.y - terrainY;
        if (delta <= 0.0f && previousDelta > 0.0f)
        {
            const float denom = previousDelta - delta;
            const float lerp = denom > 0.0001f ? previousDelta / denom : 0.0f;
            const float hitT = previousT + (t - previousT) * std::clamp(lerp, 0.0f, 1.0f);
            WorldVec3 hit = camera.eye + rayDir * hitT;
            hit.y = terrain.SampleHeight(hit);
            return hit;
        }
        previousT = t;
        previousDelta = delta;
    }
    return std::nullopt;
}

float SnapValue(float value, float step)
{
    if (step <= 0.0001f)
        return value;
    return xm::Round(value / step) * step;
}

WorldVec3 SnapPoint(WorldVec3 point, float step)
{
    point.x = SnapValue(point.x, step);
    point.y = SnapValue(point.y, step);
    point.z = SnapValue(point.z, step);
    return point;
}

bool ExpandWaterBodyForSculpt(WaterBody& body, const WorldVec3& point, float radiusMeters)
{
    if (body.maskWidth == 0 || body.maskHeight == 0 ||
        body.shapeMask.size() != static_cast<std::size_t>(body.maskWidth) * body.maskHeight)
    {
        return false;
    }

    const float minX = std::min(body.bboxMin[0], body.bboxMax[0]);
    const float maxX = std::max(body.bboxMin[0], body.bboxMax[0]);
    const float minZ = std::min(body.bboxMin[1], body.bboxMax[1]);
    const float maxZ = std::max(body.bboxMin[1], body.bboxMax[1]);
    const float brushRadius = std::max(radiusMeters, 0.001f);
    const float nextMinX = std::min(minX, point.x - brushRadius);
    const float nextMaxX = std::max(maxX, point.x + brushRadius);
    const float nextMinZ = std::min(minZ, point.z - brushRadius);
    const float nextMaxZ = std::max(maxZ, point.z + brushRadius);
    if (xm::Abs(nextMinX - minX) < 0.001f && xm::Abs(nextMaxX - maxX) < 0.001f &&
        xm::Abs(nextMinZ - minZ) < 0.001f && xm::Abs(nextMaxZ - maxZ) < 0.001f)
    {
        return true;
    }

    const float oldSizeX = std::max(maxX - minX, 0.001f);
    const float oldSizeZ = std::max(maxZ - minZ, 0.001f);
    const float cellX = oldSizeX / static_cast<float>(body.maskWidth);
    const float cellZ = oldSizeZ / static_cast<float>(body.maskHeight);
    const float targetCell = std::max(0.1f, std::min(cellX, cellZ));
    const float newSizeX = std::max(nextMaxX - nextMinX, targetCell);
    const float newSizeZ = std::max(nextMaxZ - nextMinZ, targetCell);
    const std::uint32_t newWidth = std::clamp(
        static_cast<std::uint32_t>(xm::Ceil(newSizeX / targetCell)), 8u, 256u);
    const std::uint32_t newHeight = std::clamp(
        static_cast<std::uint32_t>(xm::Ceil(newSizeZ / targetCell)), 8u, 256u);
    std::vector<std::uint8_t> nextMask(static_cast<std::size_t>(newWidth) * newHeight, 0u);
    const std::vector<std::uint8_t> oldMask = body.shapeMask;

    for (std::uint32_t y = 0; y < newHeight; ++y)
    {
        const float worldZ = nextMinZ + (static_cast<float>(y) + 0.5f) / static_cast<float>(newHeight) * newSizeZ;
        if (worldZ < minZ || worldZ > maxZ)
            continue;
        const float oldV = (worldZ - minZ) / oldSizeZ;
        const std::uint32_t oldY = std::min(body.maskHeight - 1u,
            static_cast<std::uint32_t>(std::clamp(oldV, 0.0f, 0.9999f) * static_cast<float>(body.maskHeight)));
        for (std::uint32_t x = 0; x < newWidth; ++x)
        {
            const float worldX = nextMinX + (static_cast<float>(x) + 0.5f) / static_cast<float>(newWidth) * newSizeX;
            if (worldX < minX || worldX > maxX)
                continue;
            const float oldU = (worldX - minX) / oldSizeX;
            const std::uint32_t oldX = std::min(body.maskWidth - 1u,
                static_cast<std::uint32_t>(std::clamp(oldU, 0.0f, 0.9999f) * static_cast<float>(body.maskWidth)));
            nextMask[static_cast<std::size_t>(y) * newWidth + x] =
                oldMask[static_cast<std::size_t>(oldY) * body.maskWidth + oldX];
        }
    }

    body.bboxMin[0] = nextMinX;
    body.bboxMax[0] = nextMaxX;
    body.bboxMin[1] = nextMinZ;
    body.bboxMax[1] = nextMaxZ;
    body.maskWidth = newWidth;
    body.maskHeight = newHeight;
    body.shapeMask = std::move(nextMask);
    return true;
}

std::uint32_t ApplyWaterSculptBrush(WaterBody& body, const WorldVec3& point, float radiusMeters, bool addMode)
{
    if (body.maskWidth == 0 || body.maskHeight == 0 ||
        body.shapeMask.size() != static_cast<std::size_t>(body.maskWidth) * body.maskHeight)
    {
        return 0;
    }

    if (addMode && !ExpandWaterBodyForSculpt(body, point, radiusMeters))
        return 0;

    const float minX = std::min(body.bboxMin[0], body.bboxMax[0]);
    const float maxX = std::max(body.bboxMin[0], body.bboxMax[0]);
    const float minZ = std::min(body.bboxMin[1], body.bboxMax[1]);
    const float maxZ = std::max(body.bboxMin[1], body.bboxMax[1]);
    const float sizeX = std::max(maxX - minX, 0.001f);
    const float sizeZ = std::max(maxZ - minZ, 0.001f);
    if (point.x < minX || point.x > maxX || point.z < minZ || point.z > maxZ)
        return 0;

    const float u = (point.x - minX) / sizeX;
    const float v = (point.z - minZ) / sizeZ;
    const int centerX = static_cast<int>(std::clamp(u, 0.0f, 0.9999f) * static_cast<float>(body.maskWidth));
    const int centerY = static_cast<int>(std::clamp(v, 0.0f, 0.9999f) * static_cast<float>(body.maskHeight));
    const float radiusPxX = (std::max(radiusMeters, 0.001f) / sizeX) * static_cast<float>(body.maskWidth);
    const float radiusPxY = (std::max(radiusMeters, 0.001f) / sizeZ) * static_cast<float>(body.maskHeight);
    const float radiusPx = std::max(1.0f, (radiusPxX + radiusPxY) * 0.5f);
    const int radiusCeil = static_cast<int>(xm::Ceil(radiusPx));
    const int xMin = std::max(0, centerX - radiusCeil);
    const int xMax = std::min(static_cast<int>(body.maskWidth) - 1, centerX + radiusCeil);
    const int yMin = std::max(0, centerY - radiusCeil);
    const int yMax = std::min(static_cast<int>(body.maskHeight) - 1, centerY + radiusCeil);
    const std::uint8_t value = addMode ? 1u : 0u;

    std::uint32_t modified = 0;
    const float radiusSq = radiusPx * radiusPx;
    for (int y = yMin; y <= yMax; ++y)
    {
        for (int x = xMin; x <= xMax; ++x)
        {
            const float dx = static_cast<float>(x) + 0.5f - static_cast<float>(centerX);
            const float dy = static_cast<float>(y) + 0.5f - static_cast<float>(centerY);
            if (dx * dx + dy * dy > radiusSq)
                continue;
            const std::size_t index = static_cast<std::size_t>(y) * body.maskWidth + static_cast<std::size_t>(x);
            if (body.shapeMask[index] == value)
                continue;
            body.shapeMask[index] = value;
            ++modified;
        }
    }
    return modified;
}

SkinnedMeshRenderer::MotionState ToSkinnedMeshMotion(RuntimeMoveState state)
{
    switch (state)
    {
    case RuntimeMoveState::Walking:
        return SkinnedMeshRenderer::MotionState::Walk;
    case RuntimeMoveState::Running:
        return SkinnedMeshRenderer::MotionState::Run;
    default:
        return SkinnedMeshRenderer::MotionState::Idle;
    }
}

std::string ExecutableDirectory()
{
#if defined(_WIN32)
    char path[MAX_PATH]{};
    DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (length == 0 || length == MAX_PATH)
        return ".";

    std::string result(path, length);
    size_t slash = result.find_last_of("\\/");
    return slash == std::string::npos ? "." : result.substr(0, slash);
#else
    return ".";
#endif
}

bool EngineAssetRootLooksValid(const std::filesystem::path& root)
{
    std::error_code ec;
    return std::filesystem::exists(root / "assets" / "shaders" / "composite_ps.spv", ec) &&
        std::filesystem::exists(root / "assets" / "shaders" / "terrain_ps.spv", ec) &&
        std::filesystem::exists(root / "assets" / "shaders" / "rmlui_ps.spv", ec);
}

std::optional<std::filesystem::path> FindClientRootNear(std::filesystem::path start)
{
    std::error_code ec;
    start = std::filesystem::absolute(start, ec);
    if (ec)
        return std::nullopt;
    if (std::filesystem::is_regular_file(start, ec))
        start = start.parent_path();

    for (std::filesystem::path current = start; !current.empty(); current = current.parent_path())
    {
        if (EngineAssetRootLooksValid(current))
            return current;

        const std::filesystem::path clientChild = current / "Client";
        if (EngineAssetRootLooksValid(clientChild))
            return clientChild;

        if (current == current.root_path())
            break;
    }
    return std::nullopt;
}

std::filesystem::path ResolveEngineAssetRoot()
{
    const std::filesystem::path exeDir(ExecutableDirectory());
    std::vector<std::filesystem::path> candidates;

    if (auto clientRoot = FindClientRootNear(exeDir))
        candidates.push_back(*clientRoot);

    std::error_code ec;
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    if (!ec)
    {
        if (auto clientRoot = FindClientRootNear(cwd))
            candidates.push_back(*clientRoot);
    }

    candidates.push_back(exeDir);

    for (const std::filesystem::path& candidate : candidates)
    {
        if (EngineAssetRootLooksValid(candidate))
        {
            Tracenf("[BOOT] engine asset root = %s", candidate.string().c_str());
            return candidate;
        }
    }

    Tracenf("[BOOT] engine asset root fallback = %s (required shaders not found)", exeDir.string().c_str());
    return exeDir;
}

std::optional<std::string> ExtractJsonStringField(const std::string& text, const char* key)
{
    const std::string quotedKey = "\"" + std::string(key) + "\"";
    size_t keyPos = text.find(quotedKey);
    if (keyPos == std::string::npos)
        return std::nullopt;
    size_t colon = text.find(':', keyPos + quotedKey.size());
    if (colon == std::string::npos)
        return std::nullopt;
    size_t valueStart = text.find('"', colon + 1);
    if (valueStart == std::string::npos)
        return std::nullopt;
    ++valueStart;

    std::string value;
    bool escaped = false;
    for (size_t i = valueStart; i < text.size(); ++i)
    {
        const char ch = text[i];
        if (escaped)
        {
            value.push_back(ch);
            escaped = false;
            continue;
        }
        if (ch == '\\')
        {
            escaped = true;
            continue;
        }
        if (ch == '"')
            return value;
        value.push_back(ch);
    }
    return std::nullopt;
}

std::string StartupSceneFromConfig(const client::asset::IAssetReader& assets)
{
    if (auto root = assets.RootPath())
    {
        Tracenf("[BOOT] config probe: %s",
            (*root / "assets" / "app_config.json").string().c_str());
        Tracenf("[BOOT] config probe: %s",
            (*root / "app_config.json").string().c_str());
    }
    std::optional<std::string> configText = assets.ReadText("assets/app_config.json");
    const char* configSource = "assets/app_config.json";
    if (!configText)
    {
        configText = assets.ReadText("app_config.json");
        configSource = "app_config.json";
    }
    if (!configText)
    {
        Tracen("[BOOT] config missing/empty -> fallback = empty runtime");
        return {};
    }

    if (auto root = assets.RootPath())
        Tracenf("[BOOT] config loaded: %s", (*root / configSource).string().c_str());
    else
        Tracenf("[BOOT] config loaded: %s", configSource);

    if (std::optional<std::string> startupScene = ExtractJsonStringField(*configText, "startup_scene");
        startupScene && !startupScene->empty())
    {
        Tracenf("[BOOT] startup_scene = %s", startupScene->c_str());
        return *startupScene;
    }

    Tracen("[BOOT] config missing/empty -> fallback = empty runtime");
    return {};
}

std::filesystem::path ResolveRuntimeScenePath(const client::asset::IAssetReader& assets, const std::string& sceneAssetPath)
{
    std::filesystem::path requested(sceneAssetPath);
    if (requested.is_absolute() && std::filesystem::exists(requested))
        return requested;

    if (auto root = assets.RootPath())
    {
        const std::filesystem::path assetRelative = *root / "assets" / requested;
        if (std::filesystem::exists(assetRelative))
            return assetRelative;
        const std::filesystem::path rootRelative = *root / requested;
        if (std::filesystem::exists(rootRelative))
            return rootRelative;
        return assetRelative;
    }

    return requested;
}

bool LoadRuntimeScene(client::asset::IAssetReader& assets, const std::string& sceneAssetPath)
{
    if (sceneAssetPath.empty())
    {
        Tracen("[BOOT] runtime scene request: <empty>");
        Tracen("[SCENE] no scene loaded (empty runtime startup)");
        return false;
    }
    const std::filesystem::path scenePath = ResolveRuntimeScenePath(assets, sceneAssetPath);
    Tracenf("[BOOT] runtime scene request: %s", sceneAssetPath.c_str());
    Tracenf("[SCENE] load attempt: %s",
        scenePath.string().c_str());
    Tracenf("[SCENE-RUNTIME] Request load: %s -> %s",
        sceneAssetPath.c_str(),
        scenePath.string().c_str());
    return SceneManager::Instance().LoadScene(scenePath.string());
}

phys::PhysicsTransform PhysicsTransformFromMesh(const MeshSceneEntity& mesh)
{
    phys::PhysicsTransform transform{};
    transform.position[0] = mesh.position[0];
    transform.position[1] = mesh.position[1];
    transform.position[2] = mesh.position[2];
    const xm::Quat rotation = xm::FromEulerRadians({mesh.rotation[0], mesh.rotation[1], mesh.rotation[2]});
    transform.rotation[0] = rotation.x;
    transform.rotation[1] = rotation.y;
    transform.rotation[2] = rotation.z;
    transform.rotation[3] = rotation.w;
    return transform;
}

bool ApplyPhysicsTransformToMesh(const phys::PhysicsTransform& transform, MeshSceneEntity& mesh)
{
    constexpr float kPositionEpsilon = 0.0005f;
    constexpr float kRotationEpsilon = 0.0005f;
    const xm::Quat bodyRotation{
        transform.rotation[0],
        transform.rotation[1],
        transform.rotation[2],
        transform.rotation[3]};
    const xm::Vec3 position{transform.position[0], transform.position[1], transform.position[2]};
    const xm::Vec3 rotation = xm::ToEulerRadians(bodyRotation);
    const bool changed =
        xm::Abs(mesh.position[0] - position.x) > kPositionEpsilon ||
        xm::Abs(mesh.position[1] - position.y) > kPositionEpsilon ||
        xm::Abs(mesh.position[2] - position.z) > kPositionEpsilon ||
        xm::Abs(mesh.rotation[0] - rotation.x) > kRotationEpsilon ||
        xm::Abs(mesh.rotation[1] - rotation.y) > kRotationEpsilon ||
        xm::Abs(mesh.rotation[2] - rotation.z) > kRotationEpsilon;
    if (!changed)
        return false;
    mesh.position[0] = position.x;
    mesh.position[1] = position.y;
    mesh.position[2] = position.z;
    mesh.rotation[0] = rotation.x;
    mesh.rotation[1] = rotation.y;
    mesh.rotation[2] = rotation.z;
    return true;
}

WorldVec3 TransformColliderLocalPoint(const MeshSceneEntity& mesh, WorldVec3 local)
{
    xm::Mat4 model = xm::MultiplyRowMajor(
        xm::MultiplyRowMajor(
            xm::MultiplyRowMajor(
                xm::MultiplyRowMajor(xm::Scale({mesh.scale[0], mesh.scale[1], mesh.scale[2]}),
                    xm::RotationX(mesh.rotation[0])),
                xm::RotationYRowMajor(mesh.rotation[1])),
            xm::RotationZ(mesh.rotation[2])),
        xm::Translation({mesh.position[0], mesh.position[1], mesh.position[2]}));
    const xm::Vec3 world = xm::TransformPointRowVector(model.m, {local.x, local.y, local.z});
    return {world.x, world.y, world.z};
}

WorldVec3 ColliderWorldDeltaToLocalDelta(const MeshSceneEntity& mesh, WorldVec3 worldDelta)
{
    xm::Mat4 inverseRotation = xm::MultiplyRowMajor(
        xm::MultiplyRowMajor(
            xm::RotationZRowMajor(-mesh.rotation[2]),
            xm::RotationYRowMajor(-mesh.rotation[1])),
        xm::RotationXRowMajor(-mesh.rotation[0]));
    const xm::Vec3 rotated = xm::TransformVectorRowVector(inverseRotation.m, {worldDelta.x, worldDelta.y, worldDelta.z});
    return {
        rotated.x / std::max(0.001f, mesh.scale[0]),
        rotated.y / std::max(0.001f, mesh.scale[1]),
        rotated.z / std::max(0.001f, mesh.scale[2])};
}

bool ApplySceneGizmoToCollider(MeshSceneEntity& mesh,
                               const float* gizmoPosition,
                               const float* gizmoScale,
                               MapEditorGizmoOperation operation)
{
    if (!mesh.hasCollider)
        return false;

    if (operation == MapEditorGizmoOperation::Scale && gizmoScale)
    {
        phys::ColliderComponent updated = mesh.collider;
        const float invScaleX = 1.0f / std::max(0.001f, mesh.scale[0]);
        const float invScaleY = 1.0f / std::max(0.001f, mesh.scale[1]);
        const float invScaleZ = 1.0f / std::max(0.001f, mesh.scale[2]);
        if (updated.shape == phys::ColliderShape::Sphere)
        {
            const float localDiameter = std::max({
                gizmoScale[0] * invScaleX,
                gizmoScale[1] * invScaleY,
                gizmoScale[2] * invScaleZ,
                0.001f});
            updated.radius = localDiameter * 0.5f;
        }
        else if (updated.shape == phys::ColliderShape::Capsule)
        {
            const float localDiameter = std::max(gizmoScale[0] * invScaleX, gizmoScale[2] * invScaleZ);
            updated.radius = std::max(0.001f, localDiameter * 0.5f);
            updated.height = std::max(updated.radius * 2.0f, gizmoScale[1] * invScaleY);
        }
        else
        {
            updated.size[0] = std::max(0.001f, gizmoScale[0] * invScaleX);
            updated.size[1] = std::max(0.001f, gizmoScale[1] * invScaleY);
            updated.size[2] = std::max(0.001f, gizmoScale[2] * invScaleZ);
        }
        phys::Sanitize(updated);
        const bool changed =
            xm::Abs(updated.size[0] - mesh.collider.size[0]) > 0.0005f ||
            xm::Abs(updated.size[1] - mesh.collider.size[1]) > 0.0005f ||
            xm::Abs(updated.size[2] - mesh.collider.size[2]) > 0.0005f ||
            xm::Abs(updated.radius - mesh.collider.radius) > 0.0005f ||
            xm::Abs(updated.height - mesh.collider.height) > 0.0005f;
        if (!changed)
            return false;
        mesh.collider = updated;
        return true;
    }

    if (!gizmoPosition)
        return false;

    const WorldVec3 previousWorldCenter = TransformColliderLocalPoint(
        mesh,
        {mesh.collider.center[0], mesh.collider.center[1], mesh.collider.center[2]});
    const WorldVec3 nextWorldCenter{gizmoPosition[0], gizmoPosition[1], gizmoPosition[2]};
    const WorldVec3 localDelta = ColliderWorldDeltaToLocalDelta(mesh, nextWorldCenter - previousWorldCenter);
    if (xm::Abs(localDelta.x) < 0.0005f &&
        xm::Abs(localDelta.y) < 0.0005f &&
        xm::Abs(localDelta.z) < 0.0005f)
    {
        return false;
    }

    mesh.collider.center[0] += localDelta.x;
    mesh.collider.center[1] += localDelta.y;
    mesh.collider.center[2] += localDelta.z;
    phys::Sanitize(mesh.collider);
    return true;
}

void AddColliderCircle(std::vector<SelectionOutlineRenderer::Line>& lines,
                       const MeshSceneEntity& mesh,
                       WorldVec3 center,
                       WorldVec3 axisA,
                       WorldVec3 axisB,
                       float radius,
                       const std::array<float, 4>& color,
                       int segments = 32)
{
    WorldVec3 previous{};
    WorldVec3 first{};
    bool hasPrevious = false;
    for (int i = 0; i < segments; ++i)
    {
        const float angle = (static_cast<float>(i) / static_cast<float>(segments)) * xm::TwoPi;
        const WorldVec3 local = center + axisA * (xm::Cos(angle) * radius) + axisB * (xm::Sin(angle) * radius);
        const WorldVec3 point = TransformColliderLocalPoint(mesh, local);
        if (!hasPrevious)
            first = point;
        else
            lines.push_back({previous, point, color});
        previous = point;
        hasPrevious = true;
    }
    if (hasPrevious)
        lines.push_back({previous, first, color});
}

std::array<float, 4> ColliderDebugColor(const MeshSceneEntity& mesh)
{
    if (mesh.hasCollider && mesh.collider.trigger)
        return {1.0f, 0.62f, 0.10f, 0.88f};
    if (mesh.hasRigidbody && mesh.rigidbody.bodyType == phys::BodyType::Dynamic)
        return {0.24f, 0.92f, 0.38f, 0.88f};
    return {0.24f, 0.66f, 1.0f, 0.80f};
}

void AppendColliderLines(
    std::vector<SelectionOutlineRenderer::Line>& lines,
    const MeshSceneEntity& mesh,
    const std::array<float, 4>& color)
{
    if (!mesh.hasCollider || !mesh.collider.enabled)
        return;

    phys::ColliderComponent collider = mesh.collider;
    phys::Sanitize(collider);
    const WorldVec3 center{collider.center[0], collider.center[1], collider.center[2]};
    if (collider.shape == phys::ColliderShape::Box)
    {
        const WorldVec3 half{
            collider.size[0] * 0.5f,
            collider.size[1] * 0.5f,
            collider.size[2] * 0.5f};
        const std::array<WorldVec3, 8> corners{{
            TransformColliderLocalPoint(mesh, center + WorldVec3{-half.x, -half.y, -half.z}),
            TransformColliderLocalPoint(mesh, center + WorldVec3{ half.x, -half.y, -half.z}),
            TransformColliderLocalPoint(mesh, center + WorldVec3{-half.x,  half.y, -half.z}),
            TransformColliderLocalPoint(mesh, center + WorldVec3{ half.x,  half.y, -half.z}),
            TransformColliderLocalPoint(mesh, center + WorldVec3{-half.x, -half.y,  half.z}),
            TransformColliderLocalPoint(mesh, center + WorldVec3{ half.x, -half.y,  half.z}),
            TransformColliderLocalPoint(mesh, center + WorldVec3{-half.x,  half.y,  half.z}),
            TransformColliderLocalPoint(mesh, center + WorldVec3{ half.x,  half.y,  half.z}),
        }};
        constexpr std::array<std::array<int, 2>, 12> edges{{
            {{0, 1}}, {{0, 2}}, {{1, 3}}, {{2, 3}},
            {{4, 5}}, {{4, 6}}, {{5, 7}}, {{6, 7}},
            {{0, 4}}, {{1, 5}}, {{2, 6}}, {{3, 7}},
        }};
        for (const auto& edge : edges)
            lines.push_back({corners[edge[0]], corners[edge[1]], color});
    }
    else if (collider.shape == phys::ColliderShape::Sphere)
    {
        AddColliderCircle(lines, mesh, center, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, collider.radius, color);
        AddColliderCircle(lines, mesh, center, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, collider.radius, color);
        AddColliderCircle(lines, mesh, center, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, collider.radius, color);
    }
    else
    {
        const float halfCylinder = std::max(0.0f, (collider.height - collider.radius * 2.0f) * 0.5f);
        const WorldVec3 top = center + WorldVec3{0.0f, halfCylinder, 0.0f};
        const WorldVec3 bottom = center - WorldVec3{0.0f, halfCylinder, 0.0f};
        AddColliderCircle(lines, mesh, top, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, collider.radius, color);
        AddColliderCircle(lines, mesh, bottom, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, collider.radius, color);
        AddColliderCircle(lines, mesh, center, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, collider.radius, color, 24);
        AddColliderCircle(lines, mesh, center, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, collider.radius, color, 24);
        for (const WorldVec3& offset : {WorldVec3{collider.radius, 0.0f, 0.0f},
                                        WorldVec3{-collider.radius, 0.0f, 0.0f},
                                        WorldVec3{0.0f, 0.0f, collider.radius},
                                        WorldVec3{0.0f, 0.0f, -collider.radius}})
        {
            lines.push_back({
                TransformColliderLocalPoint(mesh, bottom + offset),
                TransformColliderLocalPoint(mesh, top + offset),
                color});
        }
    }
}

std::vector<SelectionOutlineRenderer::Line> BuildPhysicsColliderLines(
    const SelectedEditorObject& selected,
    const std::vector<MeshSceneEntity>& meshes,
    bool showAllColliders)
{
    std::vector<SelectionOutlineRenderer::Line> lines;
    lines.reserve(showAllColliders ? meshes.size() * 36u : 36u);
    if (showAllColliders)
    {
        for (const MeshSceneEntity& mesh : meshes)
            AppendColliderLines(lines, mesh, ColliderDebugColor(mesh));
    }
    else if (selected.type == SelectedEditorObjectType::MeshEntity)
    {
        auto it = std::find_if(meshes.begin(), meshes.end(),
            [&](const MeshSceneEntity& mesh) { return mesh.id == selected.id; });
        if (it != meshes.end())
            AppendColliderLines(lines, *it, {0.95f, 0.82f, 0.18f, 0.95f});
    }
    return lines;
}

void AppendPhysicsBodyCenterLines(
    std::vector<SelectionOutlineRenderer::Line>& lines,
    const MeshSceneEntity& mesh,
    const std::array<float, 4>& color)
{
    if (!mesh.hasRigidbody && !mesh.hasCollider)
        return;

    const WorldVec3 center{mesh.position[0], mesh.position[1], mesh.position[2]};
    constexpr float kSize = 0.22f;
    lines.push_back({center - WorldVec3{kSize, 0.0f, 0.0f}, center + WorldVec3{kSize, 0.0f, 0.0f}, color});
    lines.push_back({center - WorldVec3{0.0f, kSize, 0.0f}, center + WorldVec3{0.0f, kSize, 0.0f}, color});
    lines.push_back({center - WorldVec3{0.0f, 0.0f, kSize}, center + WorldVec3{0.0f, 0.0f, kSize}, color});
}

std::vector<SelectionOutlineRenderer::Line> BuildPhysicsBodyCenterLines(
    const SelectedEditorObject& selected,
    const std::vector<MeshSceneEntity>& meshes,
    bool showAllCenters)
{
    std::vector<SelectionOutlineRenderer::Line> lines;
    if (!showAllCenters)
        return lines;

    lines.reserve(meshes.size() * 3u);
    for (const MeshSceneEntity& mesh : meshes)
    {
        const bool selectedMesh = selected.type == SelectedEditorObjectType::MeshEntity && selected.id == mesh.id;
        AppendPhysicsBodyCenterLines(lines, mesh, selectedMesh
            ? std::array<float, 4>{1.0f, 0.90f, 0.15f, 0.95f}
            : std::array<float, 4>{0.85f, 0.85f, 1.0f, 0.75f});
    }
    return lines;
}

std::vector<SelectionOutlineRenderer::Line> BuildPhysicsContactLines(const std::vector<PhysicsDebugContact>& contacts)
{
    std::vector<SelectionOutlineRenderer::Line> lines;
    lines.reserve(contacts.size() * 4u);
    for (const PhysicsDebugContact& contact : contacts)
    {
        const WorldVec3 p = contact.point;
        constexpr float kCross = 0.08f;
        lines.push_back({p - WorldVec3{kCross, 0.0f, 0.0f}, p + WorldVec3{kCross, 0.0f, 0.0f}, contact.color});
        lines.push_back({p - WorldVec3{0.0f, kCross, 0.0f}, p + WorldVec3{0.0f, kCross, 0.0f}, contact.color});
        lines.push_back({p - WorldVec3{0.0f, 0.0f, kCross}, p + WorldVec3{0.0f, 0.0f, kCross}, contact.color});
        lines.push_back({p, p + contact.normal * 0.55f, contact.color});
    }
    return lines;
}

// Builds a wireframe view frustum (near quad + far quad + connecting edges, plus a small
// roof triangle marking "up") for a scene CameraEntity, so the editor Scene View shows
// what the Game / Main Camera sees. Matches BuildCameraFromEntity's basis convention
// (rotation[0]=pitch, rotation[1]=yaw, no roll); aspect should be the Game view's.
std::vector<SelectionOutlineRenderer::Line> BuildCameraFrustumLines(
    const CameraEntity& cameraEntity,
    std::uint32_t viewWidth,
    std::uint32_t viewHeight,
    const std::array<float, 4>& color)
{
    std::vector<SelectionOutlineRenderer::Line> lines;

    const float aspect = viewHeight != 0
        ? static_cast<float>(viewWidth) / static_cast<float>(viewHeight)
        : 1.0f;
    const float yaw = cameraEntity.rotation[1];
    const WorldVec3 eye{cameraEntity.position[0], cameraEntity.position[1], cameraEntity.position[2]};
    const WorldVec3 forward = WorldForwardFromYawPitch(yaw, cameraEntity.rotation[0]);
    // right is horizontal (no roll); up = forward x right is already unit since forward and
    // right are orthonormal. This matches WorldLookAt(eye, target, {0,1,0}).
    const WorldVec3 right{std::cos(yaw), 0.0f, -std::sin(yaw)};
    const WorldVec3 up{
        forward.y * right.z - forward.z * right.y,
        forward.z * right.x - forward.x * right.z,
        forward.x * right.y - forward.y * right.x};

    const float fovY = xm::DegreesToRadians(std::clamp(cameraEntity.fovDegrees, 1.0f, 179.0f));
    const float tanHalf = std::tan(fovY * 0.5f);
    const float nearDist = std::max(0.001f, cameraEntity.nearPlane);
    const float farDist = std::max(nearDist + 0.001f, cameraEntity.farPlane);

    auto planeCorners = [&](float dist, WorldVec3 out[4]) {
        const float halfH = tanHalf * dist;
        const float halfW = halfH * aspect;
        const WorldVec3 center = eye + forward * dist;
        out[0] = center - right * halfW - up * halfH; // bottom-left
        out[1] = center + right * halfW - up * halfH; // bottom-right
        out[2] = center + right * halfW + up * halfH; // top-right
        out[3] = center - right * halfW + up * halfH; // top-left
    };

    WorldVec3 nearC[4];
    WorldVec3 farC[4];
    planeCorners(nearDist, nearC);
    planeCorners(farDist, farC);

    lines.reserve(12u);
    for (int i = 0; i < 4; ++i)
        lines.push_back({nearC[i], nearC[(i + 1) % 4], color}); // near quad
    for (int i = 0; i < 4; ++i)
        lines.push_back({farC[i], farC[(i + 1) % 4], color});   // far quad
    for (int i = 0; i < 4; ++i)
        lines.push_back({nearC[i], farC[i], color});            // connectors

    return lines;
}

void AppendPhysicsDebugCircle(
    std::vector<PhysicsDebugLine>& lines,
    WorldVec3 center,
    WorldVec3 axisA,
    WorldVec3 axisB,
    float radius,
    const std::array<float, 4>& color)
{
    constexpr int kSegments = 32;
    constexpr float kTau = 6.28318530718f;
    for (int segment = 0; segment < kSegments; ++segment)
    {
        const float a0 = (static_cast<float>(segment) / static_cast<float>(kSegments)) * kTau;
        const float a1 = (static_cast<float>(segment + 1) / static_cast<float>(kSegments)) * kTau;
        PhysicsDebugLine line{};
        line.a = center + axisA * (std::cos(a0) * radius) + axisB * (std::sin(a0) * radius);
        line.b = center + axisA * (std::cos(a1) * radius) + axisB * (std::sin(a1) * radius);
        line.color = color;
        line.ttlSeconds = 2.5f;
        lines.push_back(line);
    }
}

void AppendPhysicsDebugSphere(
    std::vector<PhysicsDebugLine>& lines,
    WorldVec3 center,
    float radius,
    const std::array<float, 4>& color)
{
    AppendPhysicsDebugCircle(lines, center, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, radius, color);
    AppendPhysicsDebugCircle(lines, center, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, radius, color);
    AppendPhysicsDebugCircle(lines, center, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, radius, color);
}

void AppendPhysicsDebugBox(
    std::vector<PhysicsDebugLine>& lines,
    WorldVec3 center,
    WorldVec3 halfExtents,
    const std::array<float, 4>& color)
{
    const WorldVec3 corners[8] = {
        center + WorldVec3{-halfExtents.x, -halfExtents.y, -halfExtents.z},
        center + WorldVec3{ halfExtents.x, -halfExtents.y, -halfExtents.z},
        center + WorldVec3{ halfExtents.x, -halfExtents.y,  halfExtents.z},
        center + WorldVec3{-halfExtents.x, -halfExtents.y,  halfExtents.z},
        center + WorldVec3{-halfExtents.x,  halfExtents.y, -halfExtents.z},
        center + WorldVec3{ halfExtents.x,  halfExtents.y, -halfExtents.z},
        center + WorldVec3{ halfExtents.x,  halfExtents.y,  halfExtents.z},
        center + WorldVec3{-halfExtents.x,  halfExtents.y,  halfExtents.z},
    };
    constexpr int edges[12][2] = {
        {0, 1}, {1, 2}, {2, 3}, {3, 0},
        {4, 5}, {5, 6}, {6, 7}, {7, 4},
        {0, 4}, {1, 5}, {2, 6}, {3, 7},
    };
    for (const auto& edge : edges)
    {
        PhysicsDebugLine line{};
        line.a = corners[edge[0]];
        line.b = corners[edge[1]];
        line.color = color;
        line.ttlSeconds = 2.5f;
        lines.push_back(line);
    }
}

void AppendPhysicsDebugCapsule(
    std::vector<PhysicsDebugLine>& lines,
    WorldVec3 center,
    float halfHeight,
    float radius,
    const std::array<float, 4>& color)
{
    const WorldVec3 top = center + WorldVec3{0.0f, halfHeight, 0.0f};
    const WorldVec3 bottom = center - WorldVec3{0.0f, halfHeight, 0.0f};
    AppendPhysicsDebugCircle(lines, top, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, radius, color);
    AppendPhysicsDebugCircle(lines, bottom, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, radius, color);
    AppendPhysicsDebugCircle(lines, center, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, radius, color);
    AppendPhysicsDebugCircle(lines, center, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, radius, color);
    const WorldVec3 offsets[4] = {
        {radius, 0.0f, 0.0f},
        {-radius, 0.0f, 0.0f},
        {0.0f, 0.0f, radius},
        {0.0f, 0.0f, -radius},
    };
    for (const WorldVec3& offset : offsets)
    {
        PhysicsDebugLine line{};
        line.a = bottom + offset;
        line.b = top + offset;
        line.color = color;
        line.ttlSeconds = 2.5f;
        lines.push_back(line);
    }
}

std::vector<SelectionOutlineRenderer::Line> BuildPhysicsDebugLines(const std::vector<PhysicsDebugLine>& debugLines)
{
    std::vector<SelectionOutlineRenderer::Line> lines;
    lines.reserve(debugLines.size());
    for (const PhysicsDebugLine& line : debugLines)
        lines.push_back({line.a, line.b, line.color});
    return lines;
}

void MergeMapEditorCommands(MapEditorCommands& target, const MapEditorCommands& source)
{
    target.save = target.save || source.save;
    target.reload = target.reload || source.reload;
    target.undo = target.undo || source.undo;
    target.captureGpuFrame = target.captureGpuFrame || source.captureGpuFrame;
    target.dumpFrameProfile = target.dumpFrameProfile || source.dumpFrameProfile;
    if (source.debugPerfTogglesChanged)
    {
        target.debugPerfTogglesChanged = true;
        target.disableShadowPass = source.disableShadowPass;
        target.disableWaterReflectionPass = source.disableWaterReflectionPass;
        target.disableAssetLibraryDiscovery = source.disableAssetLibraryDiscovery;
        target.disableAssetWatcherPoll = source.disableAssetWatcherPoll;
        target.disableHierarchyIteration = source.disableHierarchyIteration;
        target.showPhysicsColliders = source.showPhysicsColliders;
        target.showPhysicsContacts = source.showPhysicsContacts;
        target.showPhysicsBodyCenters = source.showPhysicsBodyCenters;
    }
    if (source.physicsLayerMatrixChanged)
    {
        target.physicsLayerMatrixChanged = true;
        target.physicsLayerMatrix = source.physicsLayerMatrix;
    }
    if (source.physicsRaycastFromCamera)
    {
        target.physicsRaycastFromCamera = true;
        target.physicsRaycastLayerMask = source.physicsRaycastLayerMask;
        target.physicsRaycastHitTriggers = source.physicsRaycastHitTriggers;
        target.physicsRaycastDistance = source.physicsRaycastDistance;
    }
    if (source.physicsOverlapSphereFromCamera)
    {
        target.physicsOverlapSphereFromCamera = true;
        target.physicsOverlapLayerMask = source.physicsOverlapLayerMask;
        target.physicsOverlapHitTriggers = source.physicsOverlapHitTriggers;
        target.physicsOverlapDistance = source.physicsOverlapDistance;
        target.physicsOverlapRadius = source.physicsOverlapRadius;
    }
    if (source.physicsOverlapBoxFromCamera)
    {
        target.physicsOverlapBoxFromCamera = true;
        target.physicsOverlapLayerMask = source.physicsOverlapLayerMask;
        target.physicsOverlapHitTriggers = source.physicsOverlapHitTriggers;
        target.physicsOverlapDistance = source.physicsOverlapDistance;
        std::copy(std::begin(source.physicsOverlapBoxHalfExtents), std::end(source.physicsOverlapBoxHalfExtents), std::begin(target.physicsOverlapBoxHalfExtents));
    }
    if (source.physicsOverlapCapsuleFromCamera)
    {
        target.physicsOverlapCapsuleFromCamera = true;
        target.physicsOverlapLayerMask = source.physicsOverlapLayerMask;
        target.physicsOverlapHitTriggers = source.physicsOverlapHitTriggers;
        target.physicsOverlapDistance = source.physicsOverlapDistance;
        target.physicsOverlapCapsuleRadius = source.physicsOverlapCapsuleRadius;
        target.physicsOverlapCapsuleHeight = source.physicsOverlapCapsuleHeight;
    }
    if (source.physicsSetLinearVelocityForSelected)
    {
        target.physicsSetLinearVelocityForSelected = true;
        target.physicsRuntimeEntityId = source.physicsRuntimeEntityId;
        std::copy(std::begin(source.physicsLinearVelocity), std::end(source.physicsLinearVelocity), std::begin(target.physicsLinearVelocity));
    }
    if (source.physicsApplyForceToSelected)
    {
        target.physicsApplyForceToSelected = true;
        target.physicsRuntimeEntityId = source.physicsRuntimeEntityId;
        std::copy(std::begin(source.physicsForce), std::end(source.physicsForce), std::begin(target.physicsForce));
    }
    if (source.physicsApplyImpulseToSelected)
    {
        target.physicsApplyImpulseToSelected = true;
        target.physicsRuntimeEntityId = source.physicsRuntimeEntityId;
        std::copy(std::begin(source.physicsImpulse), std::end(source.physicsImpulse), std::begin(target.physicsImpulse));
    }
    if (source.physicsApplyAngularImpulseToSelected)
    {
        target.physicsApplyAngularImpulseToSelected = true;
        target.physicsRuntimeEntityId = source.physicsRuntimeEntityId;
        std::copy(std::begin(source.physicsAngularImpulse), std::end(source.physicsAngularImpulse), std::begin(target.physicsAngularImpulse));
    }
    if (source.renderResolutionChanged)
    {
        target.renderResolutionChanged = true;
        target.renderResolutionUseNative = source.renderResolutionUseNative;
        target.renderResolutionWidth = source.renderResolutionWidth;
        target.renderResolutionHeight = source.renderResolutionHeight;
    }
    target.enterPlayMode = target.enterPlayMode || source.enterPlayMode;
    target.exitPlayMode = target.exitPlayMode || source.exitPlayMode;
    target.pausePlayMode = target.pausePlayMode || source.pausePlayMode;
    target.resumePlayMode = target.resumePlayMode || source.resumePlayMode;
    target.addWaterBody = target.addWaterBody || source.addWaterBody;
    if (source.createTerrain)
    {
        target.createTerrain = true;
        target.terrainCreate = source.terrainCreate;
    }
    if (source.addMeshEntity)
    {
        target.addMeshEntity = true;
        target.meshAssetId = source.meshAssetId;
        target.meshDropScreenPositionValid = source.meshDropScreenPositionValid;
        target.meshDropScreenPosition[0] = source.meshDropScreenPosition[0];
        target.meshDropScreenPosition[1] = source.meshDropScreenPosition[1];
    }
    if (source.addPrimitiveEntity)
    {
        target.addPrimitiveEntity = true;
        target.primitiveType = source.primitiveType;
    }
    if (source.addPrefabInstance)
    {
        target.addPrefabInstance = true;
        target.prefabAssetId = source.prefabAssetId;
        target.prefabDropScreenPositionValid = source.prefabDropScreenPositionValid;
        target.prefabDropScreenPosition[0] = source.prefabDropScreenPosition[0];
        target.prefabDropScreenPosition[1] = source.prefabDropScreenPosition[1];
    }
    target.createPrefabFromSelection =
        target.createPrefabFromSelection || source.createPrefabFromSelection;
    target.refreshSelectedPrefabInstance =
        target.refreshSelectedPrefabInstance || source.refreshSelectedPrefabInstance;
    target.refreshAllPrefabInstances =
        target.refreshAllPrefabInstances || source.refreshAllPrefabInstances;
    target.revertSelectedPrefabInstance =
        target.revertSelectedPrefabInstance || source.revertSelectedPrefabInstance;
    target.applySelectedPrefabToAsset =
        target.applySelectedPrefabToAsset || source.applySelectedPrefabToAsset;
    if (source.revertSelectedPrefabOverride)
    {
        target.revertSelectedPrefabOverride = true;
        target.selectedPrefabOverrideName = source.selectedPrefabOverrideName;
    }
    if (source.applySelectedPrefabOverrideToAsset)
    {
        target.applySelectedPrefabOverrideToAsset = true;
        target.selectedPrefabOverrideName = source.selectedPrefabOverrideName;
    }
    target.unpackSelectedPrefabInstance =
        target.unpackSelectedPrefabInstance || source.unpackSelectedPrefabInstance;
    if (source.savePrefabAssetEdit)
    {
        target.savePrefabAssetEdit = true;
        target.editPrefabAssetId = source.editPrefabAssetId;
        target.editPrefabName = source.editPrefabName;
        target.editPrefabEntityNames = source.editPrefabEntityNames;
    }
    if (source.addComponentToSelectedEntity)
    {
        target.addComponentToSelectedEntity = true;
        target.addComponentType = source.addComponentType;
        target.addComponentTypeId = source.addComponentTypeId;
    }
    if (source.removeComponentFromSelectedEntity)
    {
        target.removeComponentFromSelectedEntity = true;
        target.removeComponentTypeId = source.removeComponentTypeId;
    }
    if (source.lodQualityCommitRequested)
    {
        target.lodQualityCommitRequested = true;
        target.lodQualityCommitEntityId = source.lodQualityCommitEntityId;
        target.lodQualityCommitConfig = source.lodQualityCommitConfig;
    }
    if (source.assignMeshAssetToSelectedEntity)
    {
        target.assignMeshAssetToSelectedEntity = true;
        target.assignMeshAssetId = source.assignMeshAssetId;
    }
    target.deleteSelectedWaterBody = target.deleteSelectedWaterBody || source.deleteSelectedWaterBody;
    target.openSelectedWaterMaterialEditor =
        target.openSelectedWaterMaterialEditor || source.openSelectedWaterMaterialEditor;
    if (source.waterMaterialDeleted)
    {
        target.waterMaterialDeleted = true;
        target.deletedWaterMaterialId = source.deletedWaterMaterialId;
    }
    if (source.selectedWaterBodyChanged)
    {
        target.selectedWaterBodyChanged = true;
        target.selectedWaterBody = source.selectedWaterBody;
    }
    target.addPointLight = target.addPointLight || source.addPointLight;
    target.addSpotLight = target.addSpotLight || source.addSpotLight;
    target.deleteSelectedLight = target.deleteSelectedLight || source.deleteSelectedLight;
    if (source.selectedLightChanged)
    {
        target.selectedLightChanged = true;
        target.selectedLight = source.selectedLight;
    }
    target.deleteSelectedMeshEntity = target.deleteSelectedMeshEntity || source.deleteSelectedMeshEntity;
    if (source.selectedMeshEntityChanged)
    {
        target.selectedMeshEntityChanged = true;
        target.selectedMeshEntity = source.selectedMeshEntity;
    }
    if (source.selectedCameraChanged)
    {
        target.selectedCameraChanged = true;
        target.selectedCamera = source.selectedCamera;
    }
    if (source.setMainCameraRequested)
    {
        target.setMainCameraRequested = true;
        target.setMainCameraId = source.setMainCameraId;
    }
    if (source.hierarchySelectEntity)
    {
        target.hierarchySelectEntity = true;
        target.hierarchyEntityType = source.hierarchyEntityType;
        target.hierarchyEntityId = source.hierarchyEntityId;
    }
    if (source.hierarchyFocusEntity)
    {
        target.hierarchyFocusEntity = true;
        target.hierarchyEntityType = source.hierarchyEntityType;
        target.hierarchyEntityId = source.hierarchyEntityId;
    }
    if (source.hierarchyDeleteEntity)
    {
        target.hierarchyDeleteEntity = true;
        target.hierarchyEntityType = source.hierarchyEntityType;
        target.hierarchyEntityId = source.hierarchyEntityId;
    }
    if (source.hierarchyDuplicateEntity)
    {
        target.hierarchyDuplicateEntity = true;
        target.hierarchyEntityType = source.hierarchyEntityType;
        target.hierarchyEntityId = source.hierarchyEntityId;
    }
    if (source.hierarchyRenameEntity)
    {
        target.hierarchyRenameEntity = true;
        target.hierarchyEntityType = source.hierarchyEntityType;
        target.hierarchyEntityId = source.hierarchyEntityId;
        target.hierarchyRenameValue = source.hierarchyRenameValue;
    }
    if (source.hierarchyToggleHidden)
    {
        target.hierarchyToggleHidden = true;
        target.hierarchyEntityType = source.hierarchyEntityType;
        target.hierarchyEntityId = source.hierarchyEntityId;
    }
    if (source.hierarchyReparentEntity)
    {
        target.hierarchyReparentEntity = true;
        target.hierarchyEntityType = source.hierarchyEntityType;
        target.hierarchyEntityId = source.hierarchyEntityId;
        target.hierarchyEntityHandle = source.hierarchyEntityHandle;
        target.hierarchyParentType = source.hierarchyParentType;
        target.hierarchyParentId = source.hierarchyParentId;
    }
    if (source.exportMeshEntityToFbx)
    {
        target.exportMeshEntityToFbx = true;
        target.exportMeshEntityId = source.exportMeshEntityId;
        target.exportFbxOutputPath = source.exportFbxOutputPath;
        target.exportFbxEmbedTextures = source.exportFbxEmbedTextures;
        target.exportFbxMaterials = source.exportFbxMaterials;
        target.exportFbxAnimations = source.exportFbxAnimations;
    }
    if (source.paletteSlotChanged)
    {
        target.paletteSlotChanged = true;
        target.paletteSlot = source.paletteSlot;
        target.paletteAssetId = source.paletteAssetId;
        target.paletteTexturePath = source.paletteTexturePath;
        target.paletteSlotData = source.paletteSlotData;
    }
    if (source.paletteSlotParamsChanged)
    {
        target.paletteSlotParamsChanged = true;
        target.paletteSlot = source.paletteSlot;
        target.paletteSlotData = source.paletteSlotData;
    }
    if (source.terrainTriplanarChanged)
    {
        target.terrainTriplanarChanged = true;
        target.terrainTriplanarEnabled = source.terrainTriplanarEnabled;
        target.terrainTriplanarSharpness = source.terrainTriplanarSharpness;
        target.terrainTriplanarSlopeThreshold = source.terrainTriplanarSlopeThreshold;
        target.terrainTriplanarSlopeTransition = source.terrainTriplanarSlopeTransition;
    }
    if (source.gizmoSettingsChanged)
    {
        target.gizmoSettingsChanged = true;
        target.gizmoOperation = source.gizmoOperation;
        target.gizmoSnapEnabled = source.gizmoSnapEnabled;
        target.gizmoSnapValue = source.gizmoSnapValue;
    }
    if (source.sceneGizmoTransformChanged)
    {
        target.sceneGizmoTransformChanged = true;
        target.sceneGizmoTarget = source.sceneGizmoTarget;
        target.sceneGizmoEntityType = source.sceneGizmoEntityType;
        target.sceneGizmoEntityId = source.sceneGizmoEntityId;
        target.sceneGizmoOperation = source.sceneGizmoOperation;
        std::copy(std::begin(source.sceneGizmoPosition), std::end(source.sceneGizmoPosition), std::begin(target.sceneGizmoPosition));
        std::copy(std::begin(source.sceneGizmoRotation), std::end(source.sceneGizmoRotation), std::begin(target.sceneGizmoRotation));
        std::copy(std::begin(source.sceneGizmoScale), std::end(source.sceneGizmoScale), std::begin(target.sceneGizmoScale));
    }
    // Stage 7: Animator graph edits are a stream — APPEND, never overwrite (the MergeMapEditorCommands
    // silent-drop gotcha). Multiple fragments across a frame all accumulate.
    if (!source.animatorEdits.empty())
        target.animatorEdits.insert(target.animatorEdits.end(),
            source.animatorEdits.begin(), source.animatorEdits.end());
}

// The display-layer (MapEditorTypes.h) mirrors of the ixanim enums must keep identical ordinals,
// since edits cast between them. This is the only place both enums are visible.
static_assert(static_cast<int>(AnimEditConditionOp::Greater) == static_cast<int>(ixanim::ConditionOp::Greater));
static_assert(static_cast<int>(AnimEditConditionOp::IfNot) == static_cast<int>(ixanim::ConditionOp::IfNot));
static_assert(static_cast<int>(AnimEditParamType::Float) == static_cast<int>(ixanim::ParamType::Float));
static_assert(static_cast<int>(AnimEditParamType::Trigger) == static_cast<int>(ixanim::ParamType::Trigger));

// Stage 7: apply a batch of Animator graph edits to the real controller in place. Returns true if a
// runtime re-bind is needed (any behavior-changing edit) — a pure-layout MoveNode must NOT rebind,
// or dragging a node in Play would reset the animation to its default state. The caller saves the
// .controller asset regardless. v1 implements clip-assign + node-drag; later chunks extend the switch.
bool ApplyAnimatorGraphEdits(ixanim::AnimatorController& controller,
                             const std::vector<AnimatorGraphEdit>& edits)
{
    bool needsRebind = false;
    for (const AnimatorGraphEdit& edit : edits)
    {
        switch (edit.type)
        {
        case AnimatorGraphEditType::MoveNode:
            for (ixanim::AnimatorState& state : controller.states)
            {
                if (state.id == edit.stateId)
                {
                    state.graphPos[0] += edit.graphDeltaX;
                    state.graphPos[1] += edit.graphDeltaY;
                    break;
                }
            }
            break;  // cosmetic: no rebind
        case AnimatorGraphEditType::AssignClip:
            for (ixanim::AnimatorState& state : controller.states)
            {
                if (state.id == edit.stateId)
                {
                    state.clipId = edit.text;
                    break;
                }
            }
            needsRebind = true;
            break;
        case AnimatorGraphEditType::RenameState:
            for (ixanim::AnimatorState& state : controller.states)
                if (state.id == edit.stateId) { state.name = edit.text; break; }
            break;  // cosmetic
        case AnimatorGraphEditType::SetStateSpeed:
            for (ixanim::AnimatorState& state : controller.states)
                if (state.id == edit.stateId) { state.speed = edit.floatValue; break; }
            break;  // read live by the runtime
        case AnimatorGraphEditType::SetStateLoop:
            for (ixanim::AnimatorState& state : controller.states)
                if (state.id == edit.stateId) { state.loop = edit.boolValue; break; }
            break;  // read live by the runtime
        case AnimatorGraphEditType::SetDefaultState:
            controller.defaultStateId = edit.stateId;
            break;  // matters at entry; the default-marker updates live, no reset needed
        case AnimatorGraphEditType::AddState:
        {
            std::uint32_t maxId = 0;
            for (const ixanim::AnimatorState& state : controller.states)
                if (state.id != ixanim::kAnyStateId && state.id > maxId)
                    maxId = state.id;
            const bool wasEmpty = controller.states.empty();
            ixanim::AnimatorState ns;
            ns.id = maxId + 1u;
            ns.name = edit.text.empty() ? ("State " + std::to_string(ns.id)) : edit.text;
            ns.graphPos[0] = edit.graphDeltaX;  // AddState reuses the delta fields as an absolute pos
            ns.graphPos[1] = edit.graphDeltaY;
            controller.states.push_back(std::move(ns));
            if (wasEmpty)
            {
                controller.defaultStateId = maxId + 1u;
                needsRebind = true;  // the first state becomes the entry state
            }
            break;
        }
        case AnimatorGraphEditType::DeleteState:
            controller.transitions.erase(
                std::remove_if(controller.transitions.begin(), controller.transitions.end(),
                    [&](const ixanim::AnimatorTransition& t) {
                        return t.fromStateId == edit.stateId || t.toStateId == edit.stateId;
                    }),
                controller.transitions.end());
            controller.states.erase(
                std::remove_if(controller.states.begin(), controller.states.end(),
                    [&](const ixanim::AnimatorState& s) { return s.id == edit.stateId; }),
                controller.states.end());
            if (controller.defaultStateId == edit.stateId)
                controller.defaultStateId = controller.states.empty() ? 0u : controller.states.front().id;
            needsRebind = true;  // the deleted state may have been the current one
            break;
        case AnimatorGraphEditType::CreateTransition:
        {
            ixanim::AnimatorTransition t;
            t.fromStateId = edit.stateId;     // kAnyStateId for an Any-State transition
            t.toStateId = edit.toStateId;
            controller.transitions.push_back(std::move(t));
            break;  // transitions are evaluated live by the runtime — no rebind
        }
        case AnimatorGraphEditType::DeleteTransition:
        {
            for (auto it = controller.transitions.begin(); it != controller.transitions.end(); ++it)
            {
                if (it->fromStateId == edit.stateId && it->toStateId == edit.toStateId)
                {
                    controller.transitions.erase(it);
                    break;  // remove a single matching transition
                }
            }
            break;  // evaluated live — no rebind
        }
        case AnimatorGraphEditType::EditTransition:
            for (ixanim::AnimatorTransition& t : controller.transitions)
            {
                if (t.fromStateId == edit.stateId && t.toStateId == edit.toStateId)
                {
                    t.hasExitTime = edit.hasExitTime;
                    t.exitTime = edit.exitTime;
                    t.duration = edit.duration;
                    t.canTransitionToSelf = edit.canTransitionToSelf;
                    t.conditions.clear();
                    for (const AnimatorGraphCondition& gc : edit.conditions)
                    {
                        ixanim::AnimatorCondition c;
                        c.param = gc.param;
                        c.op = static_cast<ixanim::ConditionOp>(static_cast<int>(gc.op));
                        c.value = gc.value;
                        t.conditions.push_back(std::move(c));
                    }
                    break;  // edit the first matching transition (replace-all semantics)
                }
            }
            break;  // evaluated live by the runtime — no rebind
        case AnimatorGraphEditType::AddParameter:
        {
            ixanim::AnimatorParameter p;
            const std::string base = edit.text.empty() ? std::string("NewParam") : edit.text;
            std::string name = base;
            int suffix = 1;
            auto nameTaken = [&](const std::string& n) {
                for (const ixanim::AnimatorParameter& q : controller.parameters)
                    if (q.name == n) return true;
                return false;
            };
            while (nameTaken(name))
                name = base + std::to_string(suffix++);
            p.name = name;
            p.type = static_cast<ixanim::ParamType>(static_cast<int>(edit.paramType));
            p.defaultValue = edit.floatValue;
            controller.parameters.push_back(std::move(p));
            needsRebind = true;  // the runtime's paramIndex/paramValues are rebuilt at bind
            break;
        }
        case AnimatorGraphEditType::DeleteParameter:
            for (ixanim::AnimatorTransition& t : controller.transitions)
                t.conditions.erase(
                    std::remove_if(t.conditions.begin(), t.conditions.end(),
                        [&](const ixanim::AnimatorCondition& c) { return c.param == edit.text; }),
                    t.conditions.end());
            controller.parameters.erase(
                std::remove_if(controller.parameters.begin(), controller.parameters.end(),
                    [&](const ixanim::AnimatorParameter& p) { return p.name == edit.text; }),
                controller.parameters.end());
            needsRebind = true;
            break;
        case AnimatorGraphEditType::RenameParameter:
            if (!edit.text2.empty() && edit.text != edit.text2)
            {
                // Skip if the target name already exists — duplicate names would collide in the
                // runtime's name-keyed paramIndex (the second silently shadows the first).
                bool targetTaken = false;
                for (const ixanim::AnimatorParameter& p : controller.parameters)
                    if (p.name == edit.text2) { targetTaken = true; break; }
                if (!targetTaken)
                {
                    for (ixanim::AnimatorParameter& p : controller.parameters)
                        if (p.name == edit.text) { p.name = edit.text2; break; }
                    for (ixanim::AnimatorTransition& t : controller.transitions)
                        for (ixanim::AnimatorCondition& c : t.conditions)
                            if (c.param == edit.text) c.param = edit.text2;
                    needsRebind = true;  // paramIndex is keyed by name
                }
            }
            break;
        case AnimatorGraphEditType::SetParameterType:
            for (ixanim::AnimatorParameter& p : controller.parameters)
                if (p.name == edit.text)
                {
                    p.type = static_cast<ixanim::ParamType>(static_cast<int>(edit.paramType));
                    break;
                }
            break;  // runtime stores floats — no rebind
        case AnimatorGraphEditType::SetParameterDefault:
            for (ixanim::AnimatorParameter& p : controller.parameters)
                if (p.name == edit.text) { p.defaultValue = edit.floatValue; break; }
            break;  // takes effect at next bind — no rebind
        // --- Stage 5 blend tree. The runtime reads state.blendTree LIVE each frame and lazily
        //     EnsurePlayback()s any new child clip onto the already-bound target skeleton, so NONE
        //     of these need a rebind (no play-state reset). ---
        case AnimatorGraphEditType::SetStateMotionType:
            for (ixanim::AnimatorState& state : controller.states)
                if (state.id == edit.stateId)
                {
                    if (edit.text2 == "1d") state.blendTree.type = ixanim::BlendTreeType::Blend1D;
                    else if (edit.text2 == "2d") state.blendTree.type = ixanim::BlendTreeType::Blend2D;
                    else state.blendTree.type = ixanim::BlendTreeType::Single;
                    break;
                }
            break;
        case AnimatorGraphEditType::SetStateBlendParam:
            for (ixanim::AnimatorState& state : controller.states)
                if (state.id == edit.stateId) { state.blendTree.blendParam = edit.text; break; }
            break;
        case AnimatorGraphEditType::SetStateBlendParamY:
            for (ixanim::AnimatorState& state : controller.states)
                if (state.id == edit.stateId) { state.blendTree.blendParamY = edit.text; break; }
            break;
        case AnimatorGraphEditType::AddBlendTreeChild:
            for (ixanim::AnimatorState& state : controller.states)
                if (state.id == edit.stateId)
                {
                    ixanim::BlendTreeChild ch;
                    ch.clipId = edit.text;
                    ch.threshold = edit.floatValue;
                    ch.position[0] = edit.vec2Value[0];
                    ch.position[1] = edit.vec2Value[1];
                    // Append in insertion order; the runtime sorts by threshold per-eval, so the
                    // stored order is free to stay stable (keeps the editor's child indices steady).
                    state.blendTree.children.push_back(std::move(ch));
                    break;
                }
            break;
        case AnimatorGraphEditType::DeleteBlendTreeChild:
            for (ixanim::AnimatorState& state : controller.states)
                if (state.id == edit.stateId)
                {
                    auto& kids = state.blendTree.children;
                    if (edit.intValue >= 0 && edit.intValue < static_cast<int>(kids.size()))
                        kids.erase(kids.begin() + edit.intValue);
                    break;
                }
            break;
        case AnimatorGraphEditType::EditBlendTreeChild:
            for (ixanim::AnimatorState& state : controller.states)
                if (state.id == edit.stateId)
                {
                    auto& kids = state.blendTree.children;
                    if (edit.intValue >= 0 && edit.intValue < static_cast<int>(kids.size()))
                    {
                        ixanim::BlendTreeChild& ch = kids[edit.intValue];
                        ch.clipId = edit.text;  // panel always sends the intended clipId (may clear)
                        ch.threshold = edit.floatValue;
                        ch.position[0] = edit.vec2Value[0];
                        ch.position[1] = edit.vec2Value[1];
                        // No re-sort: indices stay stable for the editor; the runtime sorts per-eval.
                    }
                    break;
                }
            break;
        default:
            break;  // later chunks handle the remaining edit types
        }
    }
    return needsRebind;
}

struct EditorPlayRuntime
{
    EditorPlayModeState state;
    EditorPlayMode appliedMode = EditorPlayMode::Edit;
    std::optional<FlyCameraController::Snapshot> editorCameraSnapshot;
    std::string playStartScenePath;
    SceneData playStartSceneSnapshot;
    bool playStartSceneWasOpen = false;
    bool playStartSceneDirty = false;
};

std::unique_ptr<RuntimeSession> CreateRuntimeSession()
{
    Tracen("[RUNTIME] active session = EmptyRuntimeSession");
    return CreateEmptyRuntimeSession();
}

std::unique_ptr<RuntimeUiAdapter> CreateRuntimeUiAdapter(RmlUiLayer& rmlUi)
{
    Tracen("[RUNTIME] active UI adapter = NullRuntimeUiAdapter");
    return CreateNullRuntimeUiAdapter(rmlUi);
}

int RunGame(NativeWindow& window,
            client::asset::IAssetReader& assets)
{
    AssetLibrary::SetFbxSidecarProcessor(&ProcessImportedFbxAsset);

    VulkanDevice device;
    if (!device.Create(window, window.GetWidth(), window.GetHeight()))
    {
        ShowFatal("Failed to create Vulkan device. See debug output/stderr.");
        return 1;
    }
#if defined(IXTREEME_WITH_EDITOR)
    Tracen("[BUILD] Editor: ENABLED");
    Tracen("[BOOT] build = EDITOR");
    Tracen("[BOOT] entry state = editor boot, editor UI available, no startup scene auto-load");
#else
    Tracen("[BUILD] Editor: DISABLED");
    Tracen("[BOOT] build = RELEASE");
    Tracen("[BOOT] entry state = release boot, default runtime, no startup scene");
#endif
    Tracenf("[BOOT] window size = %ux%u", window.GetWidth(), window.GetHeight());
    Tracenf("[LOG-CONFIG] quiet_logs_for_lod_diag = %s", QuietLogsForLodDiag() ? "true" : "false");

    VkExtent2D swapchainSize = device.GetSwapchainExtent();
    VkExtent2D renderSize = swapchainSize;
    bool renderResolutionUseNative = true;
    VkExtent2D requestedRenderResolution{};
    Tracenf("[BOOT] swapchain size = %ux%u", swapchainSize.width, swapchainSize.height);
    Tracenf("[RENDER-RES] initial mode=native size=%ux%u", renderSize.width, renderSize.height);
    Tracenf("[MATH] backend=%s avx2_fma_preferred=%s",
        xm::simd::ActiveBackendName(),
#if defined(IXENGINE_ENABLE_AVX2_FMA)
        "yes"
#else
        "no"
#endif
    );
    std::unique_ptr<RuntimeSession> runtimeSession = CreateRuntimeSession();
    if (!runtimeSession->Create(device, assets, swapchainSize.width, swapchainSize.height))
    {
        ShowFatal("Failed to create runtime session. See debug output/stderr.");
        device.Destroy();
        return 1;
    }

    RmlUiLayer rmlUi;
    if (!rmlUi.Create(device, assets, swapchainSize.width, swapchainSize.height))
    {
        ShowFatal("Failed to create RmlUi layer. See debug output/stderr.");
        runtimeSession->Destroy();
        device.Destroy();
        return 1;
    }
    std::unique_ptr<RuntimeUiAdapter> runtimeUi = CreateRuntimeUiAdapter(rmlUi);
    runtimeUi->SetQuitCallback([&window]() {
        window.RequestClose();
    });
    runtimeUi->BindRuntime(*runtimeSession);
#if defined(IXTREEME_WITH_EDITOR)
    runtimeUi->HideAll();
    Tracen("[STARTUP] Editor build starts in editor-only empty scene state");
#endif

    EditorImGui editorImGui;
#if defined(IXTREEME_WITH_EDITOR)
#if defined(_WIN32)
    NativeWindow_Win32* win32Window = dynamic_cast<NativeWindow_Win32*>(&window);
    if (!win32Window || !editorImGui.Create(device, win32Window->GetHwnd()))
    {
        ShowFatal("Failed to create ImGui editor layer. See debug output/stderr.");
        runtimeSession->Destroy();
        device.Destroy();
        return 1;
    }
    SceneManager::Instance().SetWindowTitleCallback([&window](const std::string& title) {
        window.SetTitle(title);
    });
    win32Window->SetMessageCallback([&editorImGui](HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result)
    {
        return editorImGui.HandleWin32Message(hwnd, message, wParam, lParam, result);
    });
#else
    if (!editorImGui.Create(device, nullptr))
    {
        ShowFatal("Failed to create ImGui editor layer. See debug output/stderr.");
        runtimeSession->Destroy();
        device.Destroy();
        return 1;
    }
    SceneManager::Instance().SetWindowTitleCallback([&window](const std::string& title) {
        window.SetTitle(title);
    });
#endif
    editorImGui.SetMapEditorSettings(runtimeSession->GetMapEditorSettings());
    editorImGui.SetLightingState(runtimeSession->GetLightingState());
    if (auto assetRoot = assets.RootPath())
        editorImGui.SetEngineRoot(*assetRoot);
#else
    Tracen("[SCENE] no scene loaded (default runtime release state)");
#endif
    // Skinned (rigged) character models are cached per resolved model path in
    // `skinnedMeshCache` (declared next to `staticMeshCache` below) so that multiple
    // DISTINCT rigged characters can render in the same frame. (The old single shared
    // `skinnedMesh` instance Destroy/Create-thrashed when two different models were
    // placed, so only the last-loaded one rendered.)
    Tracen("[MAIN] SkinnedMeshRenderer cache initialized; models load on demand per path");

    TerrainRenderer terrain;
    bool terrainOk = terrain.Create(device, assets);
    auto syncTerrainAssetRoots = [&]() {
        std::vector<std::filesystem::path> roots;
        if (ProjectManager::Instance().HasProject())
            roots.push_back(ProjectManager::Instance().ProjectRoot());
        terrain.SetAdditionalAssetRoots(std::move(roots));
    };
    syncTerrainAssetRoots();
    if (!terrainOk)
    {
        Tracenf("[MAIN] TerrainRenderer failed to initialize - terrain will not be available");
        terrain.Destroy();
    }
    else
    {
        runtimeSession->InitializeAssetLibrary("", terrain.GetPaletteSlots());
#if defined(IXTREEME_WITH_EDITOR)
        editorImGui.SetPaletteSlots(runtimeSession->GetPaletteSlots());
        editorImGui.SetWaterMaterials(editorImGui.GetWaterMaterialsSnapshot());
#endif
        if (!terrain.ApplyPaletteSlots(device, runtimeSession->GetPaletteSlots()))
            Tracenf("[MAIN] world palette could not be applied; keeping initial terrain palette");
    }

    WorldLabelRenderer worldLabels;
    bool worldLabelsOk = worldLabels.Create(device, assets);
    if (!worldLabelsOk)
    {
        Tracenf("[MAIN] WorldLabelRenderer failed to initialize - worldLabels will not be available");
        worldLabels.Destroy();
    }

    SelectionOutlineRenderer selectionOutlines;
    bool selectionOutlinesOk = selectionOutlines.Create(device, assets);
    if (!selectionOutlinesOk)
    {
        Tracenf("[MAIN] SelectionOutlineRenderer failed to initialize - selection outlines will not be available");
        selectionOutlines.Destroy();
    }

    OffscreenSceneRenderer offscreenScene;
    bool offscreenSceneOk = offscreenScene.Create(device, assets, renderSize);
    if (offscreenSceneOk)
    {
        // (Skinned models are wired to the offscreen render pass per-entry inside
        // getSkinnedMeshRenderer when each is created; the cache is empty here.)
        if (terrainOk)
        {
            terrain.SetMainRenderPass(offscreenScene.GetRenderPass());
            terrain.SetWaterRefractionInputs(offscreenScene.GetSceneColorSnapshotView(),
                offscreenScene.GetSceneDepthSnapshotView(),
                offscreenScene.GetLinearSampler(),
                offscreenScene.GetExtent());
            terrain.RecreatePipeline(device);
        }
        if (selectionOutlinesOk)
        {
            selectionOutlines.SetMainRenderPass(offscreenScene.GetRenderPass());
            selectionOutlines.RecreatePipeline(device);
        }
#if defined(IXTREEME_WITH_EDITOR)
        editorImGui.SetSceneViewTexture(offscreenScene.GetLinearSampler(),
            offscreenScene.GetSceneColorView(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            offscreenScene.GetExtent());
#endif
    }

    // Second offscreen target for the Game view (rendered from the scene's main camera).
    // Uses a render-pass-compatible target, so the renderers' existing pipelines work as-is.
    OffscreenSceneRenderer gameView;
    bool gameViewOk = gameView.Create(device, assets, renderSize);
#if defined(IXTREEME_WITH_EDITOR)
    if (gameViewOk)
        editorImGui.SetGameViewTexture(gameView.GetLinearSampler(),
            gameView.GetSceneColorView(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            gameView.GetExtent());
#endif
    struct StaticMeshCacheEntry
    {
        enum class State
        {
            Unknown,
            LoadedStatic,
            UnsupportedSkinned,
            Failed
        };

        std::unique_ptr<StaticMeshRenderer> renderer;
        State state = State::Unknown;
    };
    std::unordered_map<std::string, StaticMeshCacheEntry> staticMeshCache;

    // Per-model skinned (rigged character) renderer cache. Keyed by the same resolved
    // runtime path as staticMeshCache, so multiple DISTINCT rigged models render at once.
    // Each entry owns its own SkinnedMeshRenderer (pipelines, buffers, and its own 32-slot
    // skin pool), so per-frame skin slots are allocated PER renderer (no global namespace).
    struct SkinnedMeshCacheEntry
    {
        enum class State { Unknown, Loaded, Failed };
        std::unique_ptr<SkinnedMeshRenderer> renderer;
        State state = State::Unknown;
        // Per-frame skin-slot cursors: Scene view consumes bottom-up, Game view top-down,
        // out of THIS renderer's MaxSkinSlots() pool. Reset lazily once per device frame so a
        // model drawn in both views in one command buffer never clobbers its own poses.
        std::uint32_t sceneSlotCursor = 0;
        std::uint32_t gameSlotCursor = SkinnedMeshRenderer::MaxSkinSlots();
        std::uint64_t cursorsResetFrame = std::numeric_limits<std::uint64_t>::max();
    };
    std::unordered_map<std::string, SkinnedMeshCacheEntry> skinnedMeshCache;
    // Most recent finalized lighting; used to seed an entry created mid-frame so its first
    // frame is lit (matches the per-frame SetLightingState push to all entries).
    LightingState skinnedCacheLighting{};
    // Networked-entity and lobby skinned rendering have no per-entity model path; they are
    // dormant today (nothing ever loaded a model for them). Gate them behind this constant —
    // empty => getSkinnedMeshRenderer returns nullptr => those blocks stay no-ops exactly as
    // before. Populate later (or add a visualClassId->path map) to activate them.
    const std::string kDefaultCharacterModelPath = "";
    // One recorded skinned draw for the Scene view, captured during the SkinInstance pre-pass
    // so the water-reflection and main passes redraw the EXACT (renderer, slot) that was
    // skinned — never re-deriving slots independently.
    struct SkinnedDrawRecord
    {
        SkinnedMeshRenderer* renderer = nullptr;
        std::uint32_t slot = 0;
        WorldVec3 position{};
        float yaw = 0.0f;
        std::array<float, 4> tint{1.0f, 1.0f, 1.0f, 1.0f};
    };

    auto getSkinnedMeshRenderer = [&](const std::string& modelPath) -> SkinnedMeshRenderer* {
        if (modelPath.empty())
            return nullptr;
        auto& entry = skinnedMeshCache[modelPath];
        if (entry.state == SkinnedMeshCacheEntry::State::Loaded)
            return entry.renderer.get();
        if (entry.state == SkinnedMeshCacheEntry::State::Failed)
            return nullptr;
        entry.renderer = std::make_unique<SkinnedMeshRenderer>();
        if (!entry.renderer->Create(device, assets, modelPath))
        {
            entry.renderer.reset();
            entry.state = SkinnedMeshCacheEntry::State::Failed;
            TraceError("[MESH-ENTITY] Failed to load skinned model: %s", modelPath.c_str());
            return nullptr;
        }
        // SkinnedMeshRenderer::Create() internally Destroy()s first (clearing any render pass),
        // so wire the offscreen pass + rebuild the pipeline AFTER Create, not before.
        if (offscreenSceneOk)
        {
            entry.renderer->SetMainRenderPass(offscreenScene.GetRenderPass());
            entry.renderer->RecreatePipeline(device);
        }
        entry.renderer->SetLightingState(skinnedCacheLighting);
        entry.state = SkinnedMeshCacheEntry::State::Loaded;
        Tracenf("[MESH-ENTITY] SkinnedMeshRenderer loaded: %s", modelPath.c_str());
#if defined(IXTREEME_WITH_EDITOR)
        // Auto-generate retargetable .ixclip assets from this model's existing _anim_<i>.ozz
        // sidecars so they appear under the asset browser's "Anim Clips" tab (idempotent).
        editorImGui.EnsureModelAnimationClips(modelPath, entry.renderer->JointNames());
#endif
        return entry.renderer.get();
    };
    auto resolveMeshRuntimePath = [&](const MeshSceneEntity& mesh) {
        if (mesh.meshAssetPath.empty())
            return std::string{};
        if (mesh.meshAssetPath.rfind("builtin://primitive/", 0) == 0)
            return mesh.meshAssetPath;
        const std::filesystem::path stored(mesh.meshAssetPath);
        if (stored.is_absolute())
            return stored.string();
        if (ProjectManager::Instance().HasProject())
        {
            const std::filesystem::path projectPath = ProjectManager::Instance().ProjectRoot() / stored;
            if (std::filesystem::exists(projectPath))
                return projectPath.string();
        }
        if (auto root = assets.RootPath())
        {
            const std::filesystem::path enginePath = *root / stored;
            if (std::filesystem::exists(enginePath))
                return stored.generic_string();
        }
        return mesh.meshAssetPath;
    };
    auto modelHasSkeletalSidecar = [&](const std::string& modelPath) {
        std::filesystem::path path(modelPath);
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (ext != ".fbx")
            return false;
        if (path.is_relative())
        {
            if (ProjectManager::Instance().HasProject())
            {
                const std::filesystem::path projectPath = ProjectManager::Instance().ProjectRoot() / path;
                if (std::filesystem::exists(projectPath))
                    path = projectPath;
            }
            else if (auto root = assets.RootPath())
            {
                const std::filesystem::path enginePath = *root / path;
                if (std::filesystem::exists(enginePath))
                    path = enginePath;
            }
        }
        const std::filesystem::path skeleton = path.parent_path() / (path.stem().string() + "_skeleton.ozz");
        return std::filesystem::exists(skeleton);
    };
    auto getStaticMeshRenderer = [&](const std::string& modelPath) -> StaticMeshRenderer* {
        if (modelPath.empty())
            return nullptr;
        auto& entry = staticMeshCache[modelPath];
        if (entry.state == StaticMeshCacheEntry::State::LoadedStatic)
            return entry.renderer.get();
        if (entry.state == StaticMeshCacheEntry::State::UnsupportedSkinned ||
            entry.state == StaticMeshCacheEntry::State::Failed)
            return nullptr;

        if (modelHasSkeletalSidecar(modelPath))
        {
            entry.state = StaticMeshCacheEntry::State::UnsupportedSkinned;
            Tracenf("[MESH-ENTITY] Skinned FBX detected and skipped for mesh entity static path: %s", modelPath.c_str());
            return nullptr;
        }

        bool isSkinned = false;
        std::string inspectError;
        std::string ext = std::filesystem::path(modelPath).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        const bool builtinPrimitive = modelPath.rfind("builtin://primitive/", 0) == 0;
        if (!builtinPrimitive && ext != ".fbx" && !StaticMeshRenderer::DetectSkinnedGltf(assets, modelPath, isSkinned, &inspectError))
        {
            entry.state = StaticMeshCacheEntry::State::Failed;
            TraceError("[MESH-ENTITY] Static mesh inspect failed: %s (%s)", modelPath.c_str(), inspectError.c_str());
            return nullptr;
        }
        if (isSkinned)
        {
            entry.state = StaticMeshCacheEntry::State::UnsupportedSkinned;
            Tracenf("[MESH-ENTITY] Skinned glTF detected and skipped for mesh entity static path: %s", modelPath.c_str());
            return nullptr;
        }

        entry.renderer = std::make_unique<StaticMeshRenderer>();
        if (offscreenSceneOk)
            entry.renderer->SetMainRenderPass(offscreenScene.GetRenderPass());
        if (!entry.renderer->Create(device, assets, modelPath))
        {
            entry.renderer.reset();
            entry.state = StaticMeshCacheEntry::State::Failed;
            TraceError("[MESH-ENTITY] Static mesh load failed: %s", modelPath.c_str());
            return nullptr;
        }
        entry.state = StaticMeshCacheEntry::State::LoadedStatic;
        Tracenf("[MESH-ENTITY] StaticMeshRenderer loaded: %s", modelPath.c_str());
        return entry.renderer.get();
    };
    auto effectiveRenderExtent = [&]() {
        if (!renderResolutionUseNative &&
            requestedRenderResolution.width > 0 &&
            requestedRenderResolution.height > 0)
        {
            return requestedRenderResolution;
        }
        return device.GetSwapchainExtent();
    };
    auto bindOffscreenSceneTargets = [&]() {
        if (!offscreenSceneOk)
            return;
        renderSize = offscreenScene.GetExtent();
        for (auto& [skinnedPath, skinnedEntry] : skinnedMeshCache)
        {
            (void)skinnedPath;
            if (skinnedEntry.renderer)
            {
                skinnedEntry.renderer->SetMainRenderPass(offscreenScene.GetRenderPass());
                skinnedEntry.renderer->RecreatePipeline(device);
            }
        }
        for (auto& [path, entry] : staticMeshCache)
        {
            (void)path;
            if (entry.renderer)
            {
                entry.renderer->SetMainRenderPass(offscreenScene.GetRenderPass());
                entry.renderer->RecreatePipeline(device);
            }
        }
        if (terrainOk)
        {
            terrain.SetMainRenderPass(offscreenScene.GetRenderPass());
            terrain.SetWaterRefractionInputs(offscreenScene.GetSceneColorSnapshotView(),
                offscreenScene.GetSceneDepthSnapshotView(),
                offscreenScene.GetLinearSampler(),
                offscreenScene.GetExtent());
            terrain.RecreatePipeline(device);
        }
        if (selectionOutlinesOk)
        {
            selectionOutlines.SetMainRenderPass(offscreenScene.GetRenderPass());
            selectionOutlines.RecreatePipeline(device);
        }
#if defined(IXTREEME_WITH_EDITOR)
        editorImGui.SetSceneViewTexture(offscreenScene.GetLinearSampler(),
            offscreenScene.GetSceneColorView(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            offscreenScene.GetExtent());
        if (gameViewOk)
        {
            gameViewOk = gameView.Recreate(device, renderSize);
            editorImGui.SetGameViewTexture(
                gameViewOk ? gameView.GetLinearSampler() : VK_NULL_HANDLE,
                gameViewOk ? gameView.GetSceneColorView() : VK_NULL_HANDLE,
                gameViewOk ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                gameViewOk ? gameView.GetExtent() : VkExtent2D{});
        }
#endif
    };
    auto recreateOffscreenScene = [&](const char* reason) {
        const VkExtent2D targetExtent = effectiveRenderExtent();
        Tracenf("[RENDER-RES] recreate reason=%s mode=%s target=%ux%u swapchain=%ux%u",
            reason ? reason : "unknown",
            renderResolutionUseNative ? "native" : "fixed",
            targetExtent.width,
            targetExtent.height,
            device.GetSwapchainExtent().width,
            device.GetSwapchainExtent().height);
        offscreenSceneOk = offscreenScene.Recreate(device, targetExtent);
        if (offscreenSceneOk)
        {
            bindOffscreenSceneTargets();
            return;
        }
        renderSize = device.GetSwapchainExtent();
#if defined(IXTREEME_WITH_EDITOR)
        editorImGui.SetSceneViewTexture(VK_NULL_HANDLE,
            VK_NULL_HANDLE,
            VK_IMAGE_LAYOUT_UNDEFINED,
            {});
#endif
    };

    runtimeSession->SetQuitCallback([&window]()
    {
        window.RequestClose();
    });

    MovementInputState movement;
    FlyCameraController cameraController;
#if defined(IXTREEME_WITH_EDITOR)
    EditorPlayRuntime editorPlay;
    runtimeSession->SetMapEditorOpen(true);
    if (terrainOk)
        terrain.SetMapEditorOpen(true);
    cameraController.SetFreeCameraEnabled(true);
    runtimeSession->SetEditorStatus("Editor opened at boot");
    Tracen("[BOOT] editor_open forced = 1 (editor build boot)");
    Tracen("[EDITOR-CAMERA] viewport_input_gate enabled");
#endif
    std::vector<WorldRenderEntity> lastPickEntities;
    WorldCamera lastPickCamera{};
    bool hasLastPickCamera = false;
    std::uint32_t selectedTargetNetId = 0;
    std::vector<PointLight> editorPointLights;
    std::vector<SpotLight> editorSpotLights;
    std::vector<MeshSceneEntity> editorMeshEntities;
    std::vector<CameraEntity> editorCameras;
    std::uint32_t nextEditorCameraEntityId = 1;
    std::uint32_t editorMainCameraId = 0;
    std::unordered_map<std::uint32_t, std::size_t> editorMeshEntityLookup;
    SpatialIndex staticMeshSpatialIndex;
    std::unordered_set<std::uint32_t> staticMeshSpatialIndexed;
    std::unordered_map<std::uint32_t, std::uint32_t> staticMeshSelectedLods;
    std::unordered_map<std::uint32_t, LodDispositionState> staticMeshLodDispositionStates;
    std::unordered_map<std::uint32_t, LodCfgLogState> lodCfgLogStates;
    std::unordered_map<std::uint32_t, LodPickLogState> lodPickLogStates;
    std::unordered_set<std::uint32_t> previousCulledMeshLogSet;
    std::array<std::uint32_t, LodConfig::MaxLevels> previousFrameLodSelection{};
    bool previousFrameLodSelectionInitialized = false;
    std::uint32_t previousFrameNonLodEntities = 0;
    bool previousFrameNonLodInitialized = false;
    MPerfMainState previousMperfMain;
    MPerfOverrideState previousMperfOverride;
    MPerfMeshesState previousMperfMeshes;
    InstSummaryState previousInstSummary;
    InstBufferState previousInstBuffer;
    phys::PhysicsWorld editorPhysicsWorld;
    std::unordered_map<std::uint32_t, phys::BodyId> editorPhysicsBodies;
    std::unordered_map<phys::BodyId, PhysicsBodyEntityBinding> editorPhysicsBodyBindings;
    std::vector<PhysicsEntityEvent> editorPhysicsEntityEvents;
    std::vector<PhysicsDebugContact> editorPhysicsDebugContacts;
    std::vector<PhysicsDebugLine> editorPhysicsDebugLines;
    PhysicsLayerMatrix editorPhysicsLayerMatrix{};
    // Player character controllers: per-entity runtime state + accumulated right-drag look.
    std::unordered_map<std::uint32_t, CharacterRuntimeState> editorCharacterStates;
    // Stage-3 temp clip binding: per-entity retarget playback + the currently-bound clip id
    // (to detect changes and rebind). Cleared lazily when an entity's clip is unset.
    std::unordered_map<std::uint32_t, ixanim::ClipPlayback> entityClipPlaybacks;
    std::unordered_map<std::uint32_t, std::string> entityBoundClipId;
    // Stage-4 Animator: per-entity FSM runtime + its (auto-filled) controller copy + the bound
    // controller asset id (to detect reassignment). Takes priority over the debug clip binding.
    std::unordered_map<std::uint32_t, ixanim::AnimatorRuntime> entityAnimators;
    std::unordered_map<std::uint32_t, ixanim::AnimatorController> entityControllers;
    std::unordered_map<std::uint32_t, std::string> entityBoundControllerId;
    double animPrevFrameSeconds = 0.0;
    float editorPlayerLookDx = 0.0f;
    float editorPlayerLookDy = 0.0f;
    bool editorPhysicsWorldActive = false;
    float editorPhysicsAccumulatorSeconds = 0.0f;
    std::uint32_t editorPhysicsStepLogFrames = 0;
    for (std::size_t a = 0; a < static_cast<std::size_t>(phys::PhysicsLayer::Count); ++a)
    {
        for (std::size_t b = 0; b < static_cast<std::size_t>(phys::PhysicsLayer::Count); ++b)
        {
            editorPhysicsLayerMatrix[a][b] = phys::DefaultLayerCollision(
                static_cast<phys::PhysicsLayer>(a),
                static_cast<phys::PhysicsLayer>(b));
        }
    }
    std::size_t previousMeshSubmitDetailInstances = 0;
    std::size_t previousMeshSubmitDetailDrawCalls = 0;
    bool previousMeshSubmitDetailInitialized = false;
    std::vector<WaterBody> editorWaterBodies = terrainOk ? terrain.GetWaterBodies() : std::vector<WaterBody>{};
    bool editorWaterBodiesDirty = false;
#if defined(IXTREEME_WITH_EDITOR)
    std::uint32_t nextEditorLightId = 1;
#endif
    std::uint32_t nextEditorWaterBodyId = 1;
    for (const WaterBody& body : editorWaterBodies)
        nextEditorWaterBodyId = std::max(nextEditorWaterBodyId, body.id + 1u);
    std::uint32_t nextEditorMeshEntityId = 1;
    SelectedEditorObject selectedEditorObject;
    std::unique_ptr<ecs_world_t, void(*)(ecs_world_t*)> editorHierarchyWorld(ecs_init(), [](ecs_world_t* world) {
        if (world)
            ecs_fini(world);
    });
    ecs_entity_t editorSceneRootEntity = ecs_new(editorHierarchyWorld.get());
    ecs_set_name(editorHierarchyWorld.get(), editorSceneRootEntity, "Untitled");
    ecs_entity_t editorNoteComponentEntity = ecs_new(editorHierarchyWorld.get());
    ecs_set_name(editorHierarchyWorld.get(), editorNoteComponentEntity, "EditorNoteComponent");
    std::unordered_map<std::uint64_t, ecs_entity_t> editorHierarchyEntities;
    auto resetEditorHierarchyEntities = [&]() {
        for (const auto& [_, entity] : editorHierarchyEntities)
            ecs_delete(editorHierarchyWorld.get(), entity);
        editorHierarchyEntities.clear();
        selectedEditorObject.flecsEntity = 0;
    };
    auto sceneEntityNameExists = [&](const std::string& name) {
        auto matches = [&](const auto& entity) {
            return entity.name == name;
        };
        return std::any_of(editorWaterBodies.begin(), editorWaterBodies.end(), matches) ||
            std::any_of(editorPointLights.begin(), editorPointLights.end(), matches) ||
            std::any_of(editorSpotLights.begin(), editorSpotLights.end(), matches) ||
            std::any_of(editorMeshEntities.begin(), editorMeshEntities.end(), matches);
    };
    auto makeUniqueSceneEntityName = [&](const std::string& base) {
        if (!sceneEntityNameExists(base))
            return base;
        for (std::uint32_t suffix = 2; suffix < 10000; ++suffix)
        {
            const std::string candidate = base + " " + std::to_string(suffix);
            if (!sceneEntityNameExists(candidate))
                return candidate;
        }
        return base + " " + std::to_string(nextEditorLightId + nextEditorWaterBodyId + nextEditorMeshEntityId);
    };
    EditorGizmoMode editorGizmoMode = EditorGizmoMode::Translate;
    bool editorGizmoSnapEnabled = false;
    float editorGizmoSnapValue = 1.0f;
    bool editorObjectDragActive = false;
    int editorObjectDragLastX = 0;
    int editorObjectDragLastY = 0;
#if defined(IXTREEME_WITH_EDITOR)
    auto rebuildMeshEntityLookup = [&]() {
        editorMeshEntityLookup.clear();
        for (std::size_t i = 0; i < editorMeshEntities.size(); ++i)
            editorMeshEntityLookup[editorMeshEntities[i].id] = i;
    };
    auto findMeshEntityById = [&](std::uint32_t id) -> MeshSceneEntity* {
        auto lookupIt = editorMeshEntityLookup.find(id);
        if (lookupIt == editorMeshEntityLookup.end() || lookupIt->second >= editorMeshEntities.size())
            return nullptr;
        MeshSceneEntity& mesh = editorMeshEntities[lookupIt->second];
        return mesh.id == id ? &mesh : nullptr;
    };
    auto syncStaticMeshSpatialEntity = [&](const MeshSceneEntity& mesh) {
        const std::string runtimePath = resolveMeshRuntimePath(mesh);
        StaticMeshRenderer* renderer = getStaticMeshRenderer(runtimePath);
        if (!renderer || !renderer->IsLoaded())
        {
            if (staticMeshSpatialIndexed.erase(mesh.id) > 0)
                staticMeshSpatialIndex.Remove(mesh.id);
            return false;
        }

        const SpatialIndex::Aabb bounds = StaticMeshWorldAabb(mesh, *renderer);
        if (staticMeshSpatialIndexed.find(mesh.id) == staticMeshSpatialIndexed.end())
        {
            staticMeshSpatialIndex.Insert(mesh.id, bounds);
            staticMeshSpatialIndexed.insert(mesh.id);
        }
        else
        {
            staticMeshSpatialIndex.Update(mesh.id, bounds);
        }
        return true;
    };
    auto fitMeshColliderToBounds = [&](MeshSceneEntity& mesh) {
        if (!mesh.hasCollider)
            return false;
        const std::string runtimePath = resolveMeshRuntimePath(mesh);
        StaticMeshRenderer* renderer = getStaticMeshRenderer(runtimePath);
        if (!renderer || !renderer->IsLoaded())
        {
            Tracenf("[PHYSICS] collider fit skipped entity=%u name=%s reason=mesh-bounds-unavailable",
                mesh.id,
                mesh.name.c_str());
            return false;
        }

        const auto& bmin = renderer->BoundsMin();
        const auto& bmax = renderer->BoundsMax();
        const float sizeX = std::max(0.001f, bmax[0] - bmin[0]);
        const float sizeY = std::max(0.001f, bmax[1] - bmin[1]);
        const float sizeZ = std::max(0.001f, bmax[2] - bmin[2]);
        mesh.collider.center[0] = (bmin[0] + bmax[0]) * 0.5f;
        mesh.collider.center[1] = (bmin[1] + bmax[1]) * 0.5f;
        mesh.collider.center[2] = (bmin[2] + bmax[2]) * 0.5f;
        if (mesh.collider.shape == phys::ColliderShape::Sphere)
        {
            mesh.collider.radius = std::max({sizeX, sizeY, sizeZ}) * 0.5f;
        }
        else if (mesh.collider.shape == phys::ColliderShape::Capsule)
        {
            mesh.collider.radius = std::max(sizeX, sizeZ) * 0.5f;
            mesh.collider.height = std::max(sizeY, mesh.collider.radius * 2.0f);
        }
        else
        {
            mesh.collider.size[0] = sizeX;
            mesh.collider.size[1] = sizeY;
            mesh.collider.size[2] = sizeZ;
        }
        phys::Sanitize(mesh.collider);
        Tracenf("[PHYSICS] collider fit entity=%u name=%s shape=%s center=(%.3f,%.3f,%.3f) size=(%.3f,%.3f,%.3f) radius=%.3f height=%.3f",
            mesh.id,
            mesh.name.c_str(),
            phys::ToString(mesh.collider.shape),
            mesh.collider.center[0],
            mesh.collider.center[1],
            mesh.collider.center[2],
            mesh.collider.size[0],
            mesh.collider.size[1],
            mesh.collider.size[2],
            mesh.collider.radius,
            mesh.collider.height);
        return true;
    };
    auto removeStaticMeshSpatialEntity = [&](std::uint32_t id) {
        staticMeshSelectedLods.erase(id);
        staticMeshLodDispositionStates.erase(id);
        auto physicsIt = editorPhysicsBodies.find(id);
        if (physicsIt != editorPhysicsBodies.end())
        {
            editorPhysicsWorld.DestroyBody(physicsIt->second);
            editorPhysicsBodies.erase(physicsIt);
        }
        if (staticMeshSpatialIndexed.erase(id) > 0)
            staticMeshSpatialIndex.Remove(id);
    };
    auto clearEditorPhysicsWorld = [&]() {
        editorPhysicsBodies.clear();
        editorPhysicsBodyBindings.clear();
        editorPhysicsEntityEvents.clear();
        editorPhysicsDebugContacts.clear();
        editorPhysicsDebugLines.clear();
        editorPhysicsWorld.Destroy();
        editorPhysicsWorldActive = false;
        editorPhysicsAccumulatorSeconds = 0.0f;
        editorPhysicsStepLogFrames = 0;
    };
    auto rebuildEditorPhysicsWorld = [&]() {
        clearEditorPhysicsWorld();
        editorPhysicsWorld.SetCollisionMatrix(editorPhysicsLayerMatrix);
        const PhysicsSceneSettings physicsSettings = SceneManager::Instance().GetCurrentScene().physics;
        editorPhysicsWorld.SetGravity(physicsSettings.gravity);
        if (!editorPhysicsWorld.Create())
        {
            Tracen("[PHYSICS] world create failed");
            return false;
        }
        std::uint32_t createdBodies = 0;
        std::uint32_t dynamicBodies = 0;
        std::uint32_t staticBodies = 0;
        std::uint32_t kinematicBodies = 0;
        std::uint32_t gravityBodies = 0;
        std::uint32_t fixedJoints = 0;
        std::uint32_t hingeJoints = 0;
        auto makeJointPairKey = [](std::uint32_t a, std::uint32_t b) -> std::uint64_t {
            const std::uint32_t lo = std::min(a, b);
            const std::uint32_t hi = std::max(a, b);
            return (static_cast<std::uint64_t>(lo) << 32u) | static_cast<std::uint64_t>(hi);
        };
        std::unordered_set<std::uint64_t> createdJointPairs;
        if (terrainOk && terrain.HasTerrain())
        {
            const TerrainSceneData terrainData = terrain.GetTerrainSceneData();
            phys::TerrainColliderDesc terrainCollider{};
            terrainCollider.widthMeters = terrainData.widthMeters;
            terrainCollider.depthMeters = terrainData.depthMeters;
            terrainCollider.cellSizeMeters = terrainData.cellSizeMeters;
            terrainCollider.cellsX = terrainData.cellsX;
            terrainCollider.cellsZ = terrainData.cellsZ;
            terrainCollider.heightCmGrid = terrainData.heightCmGrid;
            const phys::BodyId terrainBodyId = editorPhysicsWorld.CreateTerrainCollider(terrainCollider);
            if (terrainBodyId != 0)
            {
                editorPhysicsBodyBindings[terrainBodyId] = PhysicsBodyEntityBinding{
                    0u,
                    terrainData.name.empty() ? std::string("Terrain") : terrainData.name,
                    true};
                ++createdBodies;
                ++staticBodies;
            }
        }
        for (const MeshSceneEntity& mesh : editorMeshEntities)
        {
            const bool isCharacter = mesh.hasCharacterController && mesh.characterController.enabled;
            if ((!mesh.hasCollider || !mesh.collider.enabled) && !isCharacter)
                continue;
            phys::PhysicsBodyDesc desc{};
            const bool rigidbodyEnabled = mesh.hasRigidbody && mesh.rigidbody.enabled;
            desc.bodyType = rigidbodyEnabled ? mesh.rigidbody.bodyType : phys::BodyType::Static;
            desc.rigidbody = rigidbodyEnabled ? mesh.rigidbody : phys::RigidbodyComponent{};
            if (!rigidbodyEnabled)
                desc.rigidbody.useGravity = false;
            desc.collider = mesh.collider;
            desc.transform = PhysicsTransformFromMesh(mesh);
            desc.scale[0] = mesh.scale[0];
            desc.scale[1] = mesh.scale[1];
            desc.scale[2] = mesh.scale[2];
            if (isCharacter)
            {
                // Player character: kinematic capsule on the Player layer, driven by the
                // CharacterController. Movement is resolved in UpdateCharacterController; the
                // kinematic sync moves this body to match the mesh each physics step.
                phys::CharacterControllerComponent cc = mesh.characterController;
                phys::Sanitize(cc);
                desc.bodyType = phys::BodyType::Kinematic;
                desc.rigidbody = phys::RigidbodyComponent{};
                desc.rigidbody.enabled = true;
                desc.rigidbody.bodyType = phys::BodyType::Kinematic;
                desc.rigidbody.useGravity = false;
                if (!mesh.hasCollider || mesh.collider.shape != phys::ColliderShape::Capsule)
                {
                    desc.collider = phys::ColliderComponent{};
                    desc.collider.shape = phys::ColliderShape::Capsule;
                    desc.collider.radius = cc.capsuleRadius;
                    desc.collider.height = cc.capsuleHeight;
                    desc.collider.center[1] = cc.capsuleHeight * 0.5f; // seat capsule on feet
                }
                desc.collider.layer = phys::PhysicsLayer::Player;
            }
            if (desc.collider.shape == phys::ColliderShape::Mesh ||
                desc.collider.shape == phys::ColliderShape::ConvexHull)
            {
                StaticMeshRenderer* renderer = getStaticMeshRenderer(resolveMeshRuntimePath(mesh));
                if (renderer && renderer->CopyPhysicsMesh(desc.meshVertices, desc.meshIndices))
                {
                    Tracenf("[PHYSICS] %s collider geometry entity=%u name=%s verts=%zu tris=%zu",
                        phys::ToString(desc.collider.shape),
                        mesh.id,
                        mesh.name.c_str(),
                        desc.meshVertices.size(),
                        desc.meshIndices.size() / 3u);
                }
                else
                {
                    Tracenf("[PHYSICS] %s collider geometry missing entity=%u name=%s fallback=box",
                        phys::ToString(desc.collider.shape),
                        mesh.id,
                        mesh.name.c_str());
                }
            }
            if (rigidbodyEnabled &&
                desc.rigidbody.useGravity &&
                desc.bodyType == phys::BodyType::Static)
            {
                desc.bodyType = phys::BodyType::Dynamic;
                Tracenf("[PHYSICS] body auto-promoted entity=%u name=%s reason=static-rigidbody-with-gravity",
                    mesh.id,
                    mesh.name.c_str());
            }
            bool physicsMaterialApplied = false;
            if (!desc.collider.materialAssetId.empty())
            {
                if (auto physicsMaterial = editorImGui.FindPhysicsMaterial(desc.collider.materialAssetId))
                {
                    desc.collider.friction = physicsMaterial->friction;
                    desc.collider.restitution = physicsMaterial->restitution;
                    desc.collider.frictionCombine = physicsMaterial->frictionCombine;
                    desc.collider.restitutionCombine = physicsMaterial->restitutionCombine;
                    desc.rigidbody.linearDamping = physicsMaterial->linearDamping;
                    desc.rigidbody.angularDamping = physicsMaterial->angularDamping;
                    if (rigidbodyEnabled && desc.bodyType == phys::BodyType::Dynamic)
                        desc.rigidbody.mass = std::max(0.001f, desc.rigidbody.mass * physicsMaterial->density);
                    physicsMaterialApplied = true;
                }
                else
                {
                    Tracenf("[PHYSICS-MAT] missing entity=%u material=%s",
                        mesh.id,
                        desc.collider.materialAssetId.c_str());
                }
            }
            const phys::BodyId bodyId = editorPhysicsWorld.CreateBody(desc);
            if (bodyId == 0)
                continue;
            editorPhysicsBodies[mesh.id] = bodyId;
            editorPhysicsBodyBindings[bodyId] = PhysicsBodyEntityBinding{mesh.id, mesh.name, false};
            ++createdBodies;
            if (desc.bodyType == phys::BodyType::Dynamic)
            {
                ++dynamicBodies;
                if (desc.rigidbody.useGravity)
                    ++gravityBodies;
            }
            else if (desc.bodyType == phys::BodyType::Kinematic)
            {
                ++kinematicBodies;
            }
            else
            {
                ++staticBodies;
            }
            Tracenf("[PHYSICS] body entity=%u name=%s type=%s gravity=%u sleep=%u ccd=%u collider=%s layer=%s material=%s mass=%.3f friction=%.2f bounce=%.2f combine=(%s,%s) scale=(%.3f,%.3f,%.3f)",
                mesh.id,
                mesh.name.c_str(),
                phys::ToString(desc.bodyType),
                desc.rigidbody.useGravity ? 1u : 0u,
                desc.rigidbody.allowSleeping ? 1u : 0u,
                desc.rigidbody.continuousCollision ? 1u : 0u,
                phys::ToString(desc.collider.shape),
                phys::ToString(desc.collider.layer),
                physicsMaterialApplied ? desc.collider.materialAssetId.c_str() : "none",
                desc.rigidbody.mass,
                desc.collider.friction,
                desc.collider.restitution,
                phys::ToString(desc.collider.frictionCombine),
                phys::ToString(desc.collider.restitutionCombine),
                desc.scale[0],
                desc.scale[1],
                desc.scale[2]);
        }
        for (const MeshSceneEntity& mesh : editorMeshEntities)
        {
            if (!mesh.hasFixedJoint || !mesh.fixedJoint.enabled || mesh.fixedJoint.connectedEntityId == 0)
                continue;
            const auto bodyA = editorPhysicsBodies.find(mesh.id);
            const auto bodyB = editorPhysicsBodies.find(mesh.fixedJoint.connectedEntityId);
            if (bodyA == editorPhysicsBodies.end() || bodyB == editorPhysicsBodies.end())
            {
                Tracenf("[PHYSICS-JOINT] fixed skipped entity=%u connected=%u reason=missing-body",
                    mesh.id,
                    mesh.fixedJoint.connectedEntityId);
                continue;
            }
            if (!createdJointPairs.insert(makeJointPairKey(mesh.id, mesh.fixedJoint.connectedEntityId)).second)
            {
                Tracenf("[PHYSICS-JOINT] fixed skipped entity=%u connected=%u reason=duplicate-pair",
                    mesh.id,
                    mesh.fixedJoint.connectedEntityId);
                continue;
            }
            phys::FixedJointDesc jointDesc{};
            jointDesc.bodyA = bodyA->second;
            jointDesc.bodyB = bodyB->second;
            const phys::ConstraintId jointId = editorPhysicsWorld.CreateFixedJoint(jointDesc);
            if (jointId == 0)
            {
                Tracenf("[PHYSICS-JOINT] fixed skipped entity=%u connected=%u reason=create-failed",
                    mesh.id,
                    mesh.fixedJoint.connectedEntityId);
                continue;
            }
            ++fixedJoints;
            Tracenf("[PHYSICS-JOINT] fixed created id=%llu entity=%u connected=%u",
                static_cast<unsigned long long>(jointId),
                mesh.id,
                mesh.fixedJoint.connectedEntityId);
        }
        for (const MeshSceneEntity& mesh : editorMeshEntities)
        {
            if (!mesh.hasHingeJoint || !mesh.hingeJoint.enabled || mesh.hingeJoint.connectedEntityId == 0)
                continue;
            const auto bodyA = editorPhysicsBodies.find(mesh.id);
            const auto bodyB = editorPhysicsBodies.find(mesh.hingeJoint.connectedEntityId);
            if (bodyA == editorPhysicsBodies.end() || bodyB == editorPhysicsBodies.end())
            {
                Tracenf("[PHYSICS-JOINT] hinge skipped entity=%u connected=%u reason=missing-body",
                    mesh.id,
                    mesh.hingeJoint.connectedEntityId);
                continue;
            }
            if (!createdJointPairs.insert(makeJointPairKey(mesh.id, mesh.hingeJoint.connectedEntityId)).second)
            {
                Tracenf("[PHYSICS-JOINT] hinge skipped entity=%u connected=%u reason=duplicate-pair",
                    mesh.id,
                    mesh.hingeJoint.connectedEntityId);
                continue;
            }
            phys::HingeJointDesc jointDesc{};
            jointDesc.bodyA = bodyA->second;
            jointDesc.bodyB = bodyB->second;
            std::copy(std::begin(mesh.hingeJoint.anchor), std::end(mesh.hingeJoint.anchor), std::begin(jointDesc.anchor));
            std::copy(std::begin(mesh.hingeJoint.axis), std::end(mesh.hingeJoint.axis), std::begin(jointDesc.axis));
            jointDesc.limitsEnabled = mesh.hingeJoint.limitsEnabled;
            jointDesc.minAngleRadians = mesh.hingeJoint.minAngleDegrees * 0.017453292519943295f;
            jointDesc.maxAngleRadians = mesh.hingeJoint.maxAngleDegrees * 0.017453292519943295f;
            jointDesc.frictionTorque = mesh.hingeJoint.frictionTorque;
            const phys::ConstraintId jointId = editorPhysicsWorld.CreateHingeJoint(jointDesc);
            if (jointId == 0)
            {
                Tracenf("[PHYSICS-JOINT] hinge skipped entity=%u connected=%u reason=create-failed",
                    mesh.id,
                    mesh.hingeJoint.connectedEntityId);
                continue;
            }
            ++hingeJoints;
            Tracenf("[PHYSICS-JOINT] hinge created id=%llu entity=%u connected=%u anchor=(%.3f,%.3f,%.3f) axis=(%.3f,%.3f,%.3f)",
                static_cast<unsigned long long>(jointId),
                mesh.id,
                mesh.hingeJoint.connectedEntityId,
                mesh.hingeJoint.anchor[0],
                mesh.hingeJoint.anchor[1],
                mesh.hingeJoint.anchor[2],
                mesh.hingeJoint.axis[0],
                mesh.hingeJoint.axis[1],
                mesh.hingeJoint.axis[2]);
        }
        editorPhysicsWorldActive = true;
        editorPhysicsStepLogFrames = 0;
        Tracenf("[PHYSICS] play world rebuilt bodies=%u dynamic=%u static=%u kinematic=%u gravityBodies=%u worldGravity=(%.2f,%.2f,%.2f) fixedDt=%.4f maxSubsteps=%u fixedJoints=%u hingeJoints=%u meshEntities=%zu",
            createdBodies,
            dynamicBodies,
            staticBodies,
            kinematicBodies,
            gravityBodies,
            physicsSettings.gravity[0],
            physicsSettings.gravity[1],
            physicsSettings.gravity[2],
            physicsSettings.fixedDeltaSeconds,
            physicsSettings.maxSubsteps,
            fixedJoints,
            hingeJoints,
            editorMeshEntities.size());
        return true;
    };
    auto stepEditorPhysicsWorld = [&](float deltaSeconds) {
        if (!editorPhysicsWorldActive || editorPlay.state.mode != EditorPlayMode::Play)
            return;
        const PhysicsSceneSettings physicsSettings = SceneManager::Instance().GetCurrentScene().physics;
        const float fixedDeltaSeconds = std::clamp(physicsSettings.fixedDeltaSeconds, 0.001f, 0.1f);
        const std::uint32_t maxSubsteps = std::clamp(physicsSettings.maxSubsteps, 1u, 16u);
        const float frameDeltaSeconds = std::clamp(deltaSeconds, 0.0f, 0.25f);
        editorPhysicsAccumulatorSeconds += frameDeltaSeconds;

        std::uint32_t substeps = 0;
        std::uint32_t movedKinematicBodies = 0;
        std::vector<phys::PhysicsContactEvent> contactEvents;
        while (editorPhysicsAccumulatorSeconds + 0.000001f >= fixedDeltaSeconds &&
               substeps < maxSubsteps)
        {
            for (const auto& [meshId, bodyId] : editorPhysicsBodies)
            {
                MeshSceneEntity* mesh = findMeshEntityById(meshId);
                if (!mesh)
                    continue;
                const bool isCharacter = mesh->hasCharacterController && mesh->characterController.enabled;
                const bool isKinematicRigidbody = mesh->hasRigidbody && mesh->rigidbody.enabled &&
                    mesh->rigidbody.bodyType == phys::BodyType::Kinematic;
                if (!isCharacter && !isKinematicRigidbody)
                    continue;
                if (editorPhysicsWorld.MoveKinematic(bodyId, PhysicsTransformFromMesh(*mesh), fixedDeltaSeconds))
                    ++movedKinematicBodies;
            }
            editorPhysicsWorld.Step(fixedDeltaSeconds);
            const std::vector<phys::PhysicsContactEvent>& stepEvents = editorPhysicsWorld.ContactEvents();
            contactEvents.insert(contactEvents.end(), stepEvents.begin(), stepEvents.end());
            editorPhysicsAccumulatorSeconds -= fixedDeltaSeconds;
            ++substeps;
        }

        const bool droppedSubsteps = editorPhysicsAccumulatorSeconds >= fixedDeltaSeconds;
        if (droppedSubsteps)
            editorPhysicsAccumulatorSeconds = 0.0f;

        std::uint32_t collisionStarted = 0;
        std::uint32_t collisionStayed = 0;
        std::uint32_t collisionEnded = 0;
        std::uint32_t triggerStarted = 0;
        std::uint32_t triggerStayed = 0;
        std::uint32_t triggerEnded = 0;
        for (const phys::PhysicsContactEvent& event : contactEvents)
        {
            const bool trigger = event.kind == phys::PhysicsContactKind::Trigger;
            if (event.phase == phys::PhysicsContactPhase::Started)
                trigger ? ++triggerStarted : ++collisionStarted;
            else if (event.phase == phys::PhysicsContactPhase::Stayed)
                trigger ? ++triggerStayed : ++collisionStayed;
            else
                trigger ? ++triggerEnded : ++collisionEnded;
        }
        std::uint32_t detailedEventLogs = 0;
        for (const phys::PhysicsContactEvent& event : contactEvents)
        {
            const auto bindingA = editorPhysicsBodyBindings.find(event.bodyA);
            const auto bindingB = editorPhysicsBodyBindings.find(event.bodyB);
            PhysicsBodyEntityBinding fallbackA{};
            fallbackA.name = "Unknown";
            PhysicsBodyEntityBinding fallbackB{};
            fallbackB.name = "Unknown";
            const PhysicsBodyEntityBinding& bodyA =
                bindingA == editorPhysicsBodyBindings.end() ? fallbackA : bindingA->second;
            const PhysicsBodyEntityBinding& bodyB =
                bindingB == editorPhysicsBodyBindings.end() ? fallbackB : bindingB->second;

            PhysicsEntityEvent routed{};
            routed.phase = event.phase;
            routed.kind = event.kind;
            routed.entityA = bodyA.entityId;
            routed.entityB = bodyB.entityId;
            routed.nameA = bodyA.name;
            routed.nameB = bodyB.name;
            routed.bodyA = event.bodyA;
            routed.bodyB = event.bodyB;
            routed.point = {event.point[0], event.point[1], event.point[2]};
            routed.normal = {event.normal[0], event.normal[1], event.normal[2]};
            routed.penetrationDepth = event.penetrationDepth;
            editorPhysicsEntityEvents.push_back(std::move(routed));

            if ((event.phase == phys::PhysicsContactPhase::Started ||
                 event.phase == phys::PhysicsContactPhase::Ended) &&
                detailedEventLogs < 8)
            {
                const PhysicsEntityEvent& logged = editorPhysicsEntityEvents.back();
                Tracenf("[PHYSICS-EVENT] %s%s entityA=%u(%s) entityB=%u(%s) bodyA=%llu bodyB=%llu point=(%.2f,%.2f,%.2f) depth=%.3f",
                    PhysicsEventKindName(logged.kind),
                    PhysicsEventPhaseName(logged.phase),
                    logged.entityA,
                    logged.nameA.c_str(),
                    logged.entityB,
                    logged.nameB.c_str(),
                    static_cast<unsigned long long>(logged.bodyA),
                    static_cast<unsigned long long>(logged.bodyB),
                    logged.point.x,
                    logged.point.y,
                    logged.point.z,
                    logged.penetrationDepth);
                ++detailedEventLogs;
            }
        }
        if (editorPhysicsEntityEvents.size() > 512)
        {
            editorPhysicsEntityEvents.erase(
                editorPhysicsEntityEvents.begin(),
                editorPhysicsEntityEvents.begin() + static_cast<std::ptrdiff_t>(editorPhysicsEntityEvents.size() - 512));
        }
        for (const phys::PhysicsContactEvent& event : contactEvents)
        {
            if (event.phase == phys::PhysicsContactPhase::Ended)
                continue;
            PhysicsDebugContact debugContact{};
            debugContact.point = {event.point[0], event.point[1], event.point[2]};
            debugContact.normal = {event.normal[0], event.normal[1], event.normal[2]};
            debugContact.color = event.kind == phys::PhysicsContactKind::Trigger
                ? std::array<float, 4>{1.0f, 0.62f, 0.10f, 0.95f}
                : std::array<float, 4>{0.25f, 1.0f, 0.35f, 0.95f};
            debugContact.ttlSeconds = 0.25f;
            editorPhysicsDebugContacts.push_back(debugContact);
        }
        for (PhysicsDebugContact& contact : editorPhysicsDebugContacts)
            contact.ttlSeconds -= deltaSeconds;
        editorPhysicsDebugContacts.erase(
            std::remove_if(editorPhysicsDebugContacts.begin(), editorPhysicsDebugContacts.end(),
                [](const PhysicsDebugContact& contact) { return contact.ttlSeconds <= 0.0f; }),
            editorPhysicsDebugContacts.end());
        for (PhysicsDebugLine& line : editorPhysicsDebugLines)
            line.ttlSeconds -= deltaSeconds;
        editorPhysicsDebugLines.erase(
            std::remove_if(editorPhysicsDebugLines.begin(), editorPhysicsDebugLines.end(),
                [](const PhysicsDebugLine& line) { return line.ttlSeconds <= 0.0f; }),
            editorPhysicsDebugLines.end());
        if (editorPhysicsDebugContacts.size() > 256)
        {
            editorPhysicsDebugContacts.erase(
                editorPhysicsDebugContacts.begin(),
                editorPhysicsDebugContacts.begin() + static_cast<std::ptrdiff_t>(editorPhysicsDebugContacts.size() - 256));
        }
        if (editorPhysicsDebugLines.size() > 256)
        {
            editorPhysicsDebugLines.erase(
                editorPhysicsDebugLines.begin(),
                editorPhysicsDebugLines.begin() + static_cast<std::ptrdiff_t>(editorPhysicsDebugLines.size() - 256));
        }
        std::uint32_t movedDynamicBodies = 0;
        if (substeps > 0)
        {
            for (const auto& [meshId, bodyId] : editorPhysicsBodies)
            {
                MeshSceneEntity* mesh = findMeshEntityById(meshId);
                if (!mesh || !mesh->hasRigidbody || !mesh->rigidbody.enabled ||
                    mesh->rigidbody.bodyType != phys::BodyType::Dynamic)
                    continue;
                phys::PhysicsTransform transform{};
                if (!editorPhysicsWorld.GetBodyTransform(bodyId, transform))
                    continue;
                if (ApplyPhysicsTransformToMesh(transform, *mesh))
                {
                    syncStaticMeshSpatialEntity(*mesh);
                    ++movedDynamicBodies;
                }
            }
        }
        if (editorPhysicsStepLogFrames < 3)
        {
            const phys::PhysicsWorldStats physicsStats = editorPhysicsWorld.Stats();
            Tracenf("[PHYSICS] step frame=%u bodies=%u meshBodies=%zu movedDynamic=%u movedKinematic=%u frameDt=%.4f fixedDt=%.4f substeps=%u accumulator=%.4f dropped=%u",
                editorPhysicsStepLogFrames,
                physicsStats.bodyCount,
                editorPhysicsBodies.size(),
                movedDynamicBodies,
                movedKinematicBodies,
                deltaSeconds,
                fixedDeltaSeconds,
                substeps,
                editorPhysicsAccumulatorSeconds,
                droppedSubsteps ? 1u : 0u);
            if (!contactEvents.empty())
            {
                Tracenf("[PHYSICS-EVENT] frame=%u collision(start=%u stay=%u end=%u) trigger(start=%u stay=%u end=%u)",
                    editorPhysicsStepLogFrames,
                    collisionStarted,
                    collisionStayed,
                    collisionEnded,
                    triggerStarted,
                    triggerStayed,
                    triggerEnded);
            }
            ++editorPhysicsStepLogFrames;
        }
        else if (collisionStarted + collisionEnded + triggerStarted + triggerEnded > 0)
        {
            Tracenf("[PHYSICS-EVENT] collision(start=%u end=%u) trigger(start=%u end=%u)",
                collisionStarted,
                collisionEnded,
                triggerStarted,
                triggerEnded);
        }
    };
    auto logStaticMeshSpatialBuild = [&]() {
        const SpatialIndex::Aabb& b = staticMeshSpatialIndex.WorldBounds();
        Tracenf("[SPATIAL] built nodes=%u maxDepth=%u objects=%u worldBounds=(%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f)",
            staticMeshSpatialIndex.NodeCount(),
            staticMeshSpatialIndex.MaxDepth(),
            staticMeshSpatialIndex.ObjectCount(),
            b.min.x, b.min.y, b.min.z,
            b.max.x, b.max.y, b.max.z);
    };
    auto rebuildStaticMeshSpatialIndex = [&]() {
        staticMeshSpatialIndex.Clear();
        staticMeshSpatialIndexed.clear();
        rebuildMeshEntityLookup();
        for (const MeshSceneEntity& mesh : editorMeshEntities)
            syncStaticMeshSpatialEntity(mesh);
        logStaticMeshSpatialBuild();
    };
    auto logStaticMeshSpatialMutations = [&]() {
        const SpatialIndex::MutationStats mutations = staticMeshSpatialIndex.ConsumeMutationStats();
        if (mutations.inserts == 0 && mutations.removes == 0 && mutations.updates == 0)
            return;
        if (QuietLogsForLodDiag() && mutations.inserts == 0 && mutations.removes == 0 && mutations.updates == 1)
            return;
        Tracenf("[SPATIAL] mutate insert=%u remove=%u update=%u (this load/edit)",
            mutations.inserts,
            mutations.removes,
            mutations.updates);
    };
    EditorSceneRuntime sceneRuntime(EditorSceneRuntime::Context{
        &editorImGui,
        runtimeSession.get(),
        &terrain,
        &device,
        &editorWaterBodies,
        &editorPointLights,
        &editorSpotLights,
        &editorMeshEntities,
        &editorWaterBodiesDirty,
        terrainOk,
        &nextEditorWaterBodyId,
        &nextEditorLightId,
        &nextEditorMeshEntityId,
        [&selectedEditorObject]() { selectedEditorObject = {}; },
        resetEditorHierarchyEntities,
        rebuildStaticMeshSpatialIndex,
        syncTerrainAssetRoots,
        [](MeshSceneEntity& mesh) { EnsureMeshEntityMaterialSlots(mesh); },
        &editorCameras,
        &nextEditorCameraEntityId,
        &editorMainCameraId,
        [&cameraController]() {
            const FlyCameraController::Snapshot snap = cameraController.SaveSnapshot();
            EditorCameraState state;
            state.eye[0] = static_cast<float>(snap.eye.x);
            state.eye[1] = static_cast<float>(snap.eye.y);
            state.eye[2] = static_cast<float>(snap.eye.z);
            state.yaw = snap.yaw;
            state.pitch = snap.pitch;
            return state;
        },
        [&cameraController](const EditorCameraState& state) {
            FlyCameraController::Snapshot snap;
            snap.eye = WorldVec3{state.eye[0], state.eye[1], state.eye[2]};
            snap.yaw = state.yaw;
            snap.pitch = state.pitch;
            cameraController.RestoreSnapshot(snap);
        }});
    auto buildHierarchyEntities = [&]() {
        std::vector<HierarchySceneEntity> entities;
        std::vector<std::uint64_t> liveKeys;
        selectedEditorObject.flecsEntity = 0;

        const SceneData& scene = SceneManager::Instance().GetCurrentScene();
        const std::filesystem::path scenePath(SceneManager::Instance().GetCurrentScenePath());
        std::string sceneName = scenePath.stem().empty() ? scene.name : scenePath.stem().string();
        if (sceneName.empty())
            sceneName = "Untitled";
        ecs_set_name(editorHierarchyWorld.get(), editorSceneRootEntity, sceneName.c_str());
        auto syncEditorComponentTags = [&](ecs_entity_t entity, const std::vector<EditorAttachedComponent>& components) {
            const bool hasNote = std::any_of(components.begin(), components.end(),
                [](const EditorAttachedComponent& component) { return component.type == "editor.note"; });
            if (hasNote)
                ecs_add_id(editorHierarchyWorld.get(), entity, editorNoteComponentEntity);
            else
                ecs_remove_id(editorHierarchyWorld.get(), entity, editorNoteComponentEntity);
        };
        std::unordered_map<std::uint64_t, SceneParentRef> pendingParents;
        auto prefabAssetIdForHierarchyObject = [&](HierarchyEntityType type, std::uint32_t objectId) -> std::string {
            if (type == HierarchyEntityType::MeshEntity)
            {
                auto it = std::find_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                    [&](const MeshSceneEntity& mesh) { return mesh.id == objectId; });
                if (it == editorMeshEntities.end())
                    return {};
                return !it->prefabInstance.assetId.empty() ? it->prefabInstance.assetId : it->prefabAssetId;
            }
            if (type == HierarchyEntityType::PointLight)
            {
                auto it = std::find_if(editorPointLights.begin(), editorPointLights.end(),
                    [&](const PointLight& light) { return light.id == objectId; });
                if (it == editorPointLights.end())
                    return {};
                return !it->prefabInstance.assetId.empty() ? it->prefabInstance.assetId : it->prefabAssetId;
            }
            if (type == HierarchyEntityType::SpotLight)
            {
                auto it = std::find_if(editorSpotLights.begin(), editorSpotLights.end(),
                    [&](const SpotLight& light) { return light.id == objectId; });
                if (it == editorSpotLights.end())
                    return {};
                return !it->prefabInstance.assetId.empty() ? it->prefabInstance.assetId : it->prefabAssetId;
            }
            return {};
        };
        auto hierarchyParentRefForObject = [&](HierarchyEntityType type, std::uint32_t objectId) -> SceneParentRef {
            if (type == HierarchyEntityType::MeshEntity)
            {
                auto it = std::find_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                    [&](const MeshSceneEntity& mesh) { return mesh.id == objectId; });
                return it == editorMeshEntities.end() ? SceneParentRef{} : it->parent;
            }
            if (type == HierarchyEntityType::PointLight)
            {
                auto it = std::find_if(editorPointLights.begin(), editorPointLights.end(),
                    [&](const PointLight& light) { return light.id == objectId; });
                return it == editorPointLights.end() ? SceneParentRef{} : it->parent;
            }
            if (type == HierarchyEntityType::SpotLight)
            {
                auto it = std::find_if(editorSpotLights.begin(), editorSpotLights.end(),
                    [&](const SpotLight& light) { return light.id == objectId; });
                return it == editorSpotLights.end() ? SceneParentRef{} : it->parent;
            }
            return {};
        };
        std::function<std::pair<HierarchyEntityType, std::uint32_t>(HierarchyEntityType, std::uint32_t)> prefabRootForHierarchyObject;
        prefabRootForHierarchyObject = [&](HierarchyEntityType type, std::uint32_t objectId) {
            const std::string assetId = prefabAssetIdForHierarchyObject(type, objectId);
            if (assetId.empty())
                return std::make_pair(type, objectId);

            HierarchyEntityType currentType = type;
            std::uint32_t currentId = objectId;
            std::unordered_set<std::uint64_t> visited;
            while (true)
            {
                const std::uint64_t currentKey = HierarchyObjectKey(currentType, currentId);
                if (!visited.insert(currentKey).second)
                    break;
                const SceneParentRef parent = hierarchyParentRefForObject(currentType, currentId);
                if (!parent.IsValid())
                    break;
                const HierarchyEntityType parentType = SceneParentTypeFromName(parent.type);
                if (parentType == HierarchyEntityType::None)
                    break;
                if (prefabAssetIdForHierarchyObject(parentType, parent.id) != assetId)
                    break;
                currentType = parentType;
                currentId = parent.id;
            }
            return std::make_pair(currentType, currentId);
        };

        auto ensureEntity = [&](HierarchyEntityType type,
                                std::uint32_t objectId,
                                const std::string& displayName,
                                bool editorHidden,
                                const SceneParentRef& parent = {},
                                bool prefabRoot = false,
                                const std::string& prefabAssetId = {}) {
            const std::uint64_t key = HierarchyObjectKey(type, objectId);
            liveKeys.push_back(key);
            ecs_entity_t entity = 0;
            auto it = editorHierarchyEntities.find(key);
            if (it == editorHierarchyEntities.end())
            {
                entity = ecs_new(editorHierarchyWorld.get());
                ecs_add_pair(editorHierarchyWorld.get(), entity, EcsChildOf, editorSceneRootEntity);
                editorHierarchyEntities[key] = entity;
                Tracenf("[HIERARCHY] Created flecs scene entity: flecs=%llu object=%u type=%d",
                    static_cast<unsigned long long>(entity),
                    objectId,
                    static_cast<int>(type));
            }
            else
            {
                entity = it->second;
            }

            ecs_set_name(editorHierarchyWorld.get(), entity, displayName.c_str());
            pendingParents[key] = parent;

            const bool selected =
                selectedEditorObject.type == ToSelectedObjectType(type) &&
                selectedEditorObject.id == objectId;
            if (selected)
                selectedEditorObject.flecsEntity = static_cast<std::uint64_t>(entity);

            entities.push_back(HierarchySceneEntity{
                static_cast<std::uint64_t>(entity),
                static_cast<std::uint64_t>(editorSceneRootEntity),
                type,
                objectId,
                displayName,
                prefabRoot,
                prefabAssetId,
                editorHidden,
                selected});
        };

        for (const WaterBody& body : editorWaterBodies)
            ensureEntity(HierarchyEntityType::WaterBody, body.id, EditorDisplayName(body), body.editorHidden);
        if (terrainOk && terrain.HasTerrain())
        {
            const TerrainSceneData terrainData = terrain.GetTerrainSceneData();
            ensureEntity(HierarchyEntityType::Terrain, 1u, EditorDisplayName(terrainData), terrainData.editorHidden);
        }
        for (const MeshSceneEntity& mesh : editorMeshEntities)
        {
            const std::string prefabAssetId = prefabAssetIdForHierarchyObject(HierarchyEntityType::MeshEntity, mesh.id);
            const auto prefabRoot = prefabRootForHierarchyObject(HierarchyEntityType::MeshEntity, mesh.id);
            ensureEntity(HierarchyEntityType::MeshEntity,
                mesh.id,
                EditorDisplayName(mesh),
                mesh.editorHidden,
                mesh.parent,
                !prefabAssetId.empty() && prefabRoot.first == HierarchyEntityType::MeshEntity && prefabRoot.second == mesh.id,
                prefabAssetId);
        }
        for (const PointLight& light : editorPointLights)
        {
            const std::string prefabAssetId = prefabAssetIdForHierarchyObject(HierarchyEntityType::PointLight, light.id);
            const auto prefabRoot = prefabRootForHierarchyObject(HierarchyEntityType::PointLight, light.id);
            ensureEntity(HierarchyEntityType::PointLight,
                light.id,
                EditorDisplayName(light),
                light.editorHidden,
                light.parent,
                !prefabAssetId.empty() && prefabRoot.first == HierarchyEntityType::PointLight && prefabRoot.second == light.id,
                prefabAssetId);
        }
        for (const SpotLight& light : editorSpotLights)
        {
            const std::string prefabAssetId = prefabAssetIdForHierarchyObject(HierarchyEntityType::SpotLight, light.id);
            const auto prefabRoot = prefabRootForHierarchyObject(HierarchyEntityType::SpotLight, light.id);
            ensureEntity(HierarchyEntityType::SpotLight,
                light.id,
                EditorDisplayName(light),
                light.editorHidden,
                light.parent,
                !prefabAssetId.empty() && prefabRoot.first == HierarchyEntityType::SpotLight && prefabRoot.second == light.id,
                prefabAssetId);
        }
        for (const CameraEntity& cameraEntity : editorCameras)
        {
            std::string cameraLabel = cameraEntity.name.empty() ? std::string("Camera") : cameraEntity.name;
            if (cameraEntity.id == editorMainCameraId)
                cameraLabel += " (Main)";
            ensureEntity(HierarchyEntityType::Camera,
                cameraEntity.id,
                cameraLabel,
                cameraEntity.editorHidden,
                cameraEntity.parent);
        }

        for (auto it = editorHierarchyEntities.begin(); it != editorHierarchyEntities.end();)
        {
            if (std::find(liveKeys.begin(), liveKeys.end(), it->first) == liveKeys.end())
            {
                ecs_delete(editorHierarchyWorld.get(), it->second);
                it = editorHierarchyEntities.erase(it);
            }
            else
            {
                ++it;
            }
        }

        for (HierarchySceneEntity& entity : entities)
        {
            const std::uint64_t key = HierarchyObjectKey(entity.type, entity.objectId);
            const auto pendingIt = pendingParents.find(key);
            const SceneParentRef& parent = pendingIt == pendingParents.end() ? SceneParentRef{} : pendingIt->second;
            const HierarchyEntityType parentType = SceneParentTypeFromName(parent.type);
            const std::uint64_t parentKey = parent.IsValid() ? HierarchyObjectKey(parentType, parent.id) : 0;
            std::uint64_t parentHandle = static_cast<std::uint64_t>(editorSceneRootEntity);
            if (parentKey != 0)
            {
                auto parentIt = editorHierarchyEntities.find(parentKey);
                if (parentIt != editorHierarchyEntities.end())
                    parentHandle = static_cast<std::uint64_t>(parentIt->second);
            }
            entity.parent = parentHandle;
            ecs_add_pair(editorHierarchyWorld.get(), static_cast<ecs_entity_t>(entity.entity), EcsChildOf, static_cast<ecs_entity_t>(parentHandle));
        }

        for (const MeshSceneEntity& mesh : editorMeshEntities)
        {
            auto meshIt = editorHierarchyEntities.find(HierarchyObjectKey(HierarchyEntityType::MeshEntity, mesh.id));
            if (meshIt != editorHierarchyEntities.end())
                syncEditorComponentTags(meshIt->second, mesh.editorComponents);
        }

        return std::pair<std::string, std::vector<HierarchySceneEntity>>(sceneName, std::move(entities));
    };
    {
        SceneManager& scenes = SceneManager::Instance();
        scenes.CloseScene();
        Tracen("[SCENE] no scene loaded (editor empty state)");
        Tracen("[STARTUP] Editor build: no automatic default.scene load");
    }
#endif
    bool waterSculptStrokeActive = false;
    std::uint32_t waterSculptStrokeBodyId = 0;
    std::uint32_t waterSculptStrokeModifiedCells = 0;
    bool waterSculptMeshRegenPending = false;
#if defined(IXTREEME_WITH_EDITOR)
    bool editorShiftDown = false;
    bool editorLeftMouseHeld = false;
    bool editorRightMouseHeld = false;
    bool editorHasMousePosition = false;
    int editorLastMouseX = 0;
    int editorLastMouseY = 0;
    MovementInputState editorFlyMovement;
#endif

    window.SetInputCallback([&runtimeSession,
                             &runtimeUi,
                             &editorImGui,
                             &movement,
                             &cameraController,
                             &terrain,
                             &terrainOk,
                             &lastPickEntities,
                             &lastPickCamera,
                             &hasLastPickCamera,
                             &selectedTargetNetId,
                             &editorPointLights,
                             &editorSpotLights,
                             &editorMeshEntities,
                             &editorCameras,
                             &editorWaterBodies,
                             &editorWaterBodiesDirty,
                             &selectedEditorObject,
                             &editorGizmoMode,
                             &editorGizmoSnapEnabled,
                             &editorGizmoSnapValue,
                             &editorObjectDragActive,
                             &editorObjectDragLastX,
                             &editorObjectDragLastY,
                             &waterSculptStrokeActive,
                             &waterSculptStrokeBodyId,
                             &waterSculptStrokeModifiedCells,
                             &waterSculptMeshRegenPending,
#if defined(IXTREEME_WITH_EDITOR)
                             &editorPlay,
                             &editorShiftDown,
                             &editorLeftMouseHeld,
                             &editorRightMouseHeld,
                             &editorHasMousePosition,
                             &editorLastMouseX,
                             &editorLastMouseY,
                             &editorPlayerLookDx,
                             &editorPlayerLookDy,
                             &editorFlyMovement,
                             &rebuildMeshEntityLookup,
                             &syncStaticMeshSpatialEntity,
                             &removeStaticMeshSpatialEntity,
                             &resolveMeshRuntimePath,
                             &getStaticMeshRenderer,
#endif
                             &renderSize](const InputEvent& event)
    {
#if defined(IXTREEME_WITH_EDITOR)
        if (event.type == InputEvent::MouseMove ||
            event.type == InputEvent::MouseDown ||
            event.type == InputEvent::MouseUp ||
            event.type == InputEvent::MouseWheel)
        {
            // Accumulate right-drag mouse-look for the player character in Play mode
            // (cross-platform: uses event deltas, no cursor capture). Consumed + reset
            // by the per-frame character update.
            if (event.type == InputEvent::MouseMove && editorRightMouseHeld &&
                editorHasMousePosition && editorPlay.state.mode == EditorPlayMode::Play)
            {
                editorPlayerLookDx += static_cast<float>(event.x - editorLastMouseX);
                editorPlayerLookDy += static_cast<float>(event.y - editorLastMouseY);
            }
            editorHasMousePosition = true;
            editorLastMouseX = event.x;
            editorLastMouseY = event.y;
        }

        if (event.type == InputEvent::MouseDown && event.button == MouseButton_Left)
            editorLeftMouseHeld = true;
        else if (event.type == InputEvent::MouseUp && event.button == MouseButton_Left)
            editorLeftMouseHeld = false;
        else if (event.type == InputEvent::MouseDown && event.button == MouseButton_Right)
            editorRightMouseHeld = true;
        else if (event.type == InputEvent::MouseUp && event.button == MouseButton_Right)
            editorRightMouseHeld = false;

        if (event.type == InputEvent::KeyDown && event.key == Key_Shift)
            editorShiftDown = true;
        else if (event.type == InputEvent::KeyUp && event.key == Key_Shift)
            editorShiftDown = false;

        if (runtimeSession->IsMapEditorOpen() && event.type == InputEvent::KeyDown && event.key == Key_F5)
        {
            if (editorShiftDown)
            {
                if (editorPlay.state.mode != EditorPlayMode::Edit)
                    editorPlay.state.mode = EditorPlayMode::Edit;
            }
            else if (editorPlay.state.mode == EditorPlayMode::Edit)
            {
                if (SceneManager::Instance().HasOpenScene())
                    editorPlay.state.mode = EditorPlayMode::Play;
                else
                    Tracen("[EDIT-PLAY] Play ignored: no open scene");
            }
            else
            {
                editorPlay.state.mode = EditorPlayMode::Edit;
            }
            movement.Clear();
#if defined(IXTREEME_WITH_EDITOR)
            editorFlyMovement.Clear();
#endif
            return;
        }
        if (runtimeSession->IsMapEditorOpen() && event.type == InputEvent::KeyDown && event.key == Key_F6)
        {
            if (editorPlay.state.mode == EditorPlayMode::Play)
                editorPlay.state.mode = EditorPlayMode::PlayPaused;
            else if (editorPlay.state.mode == EditorPlayMode::PlayPaused)
                editorPlay.state.mode = EditorPlayMode::Play;
            movement.Clear();
#if defined(IXTREEME_WITH_EDITOR)
            editorFlyMovement.Clear();
#endif
            return;
        }
#endif
        bool sceneViewInputTarget = false;
        InputEvent viewportEvent = event;
        const auto isEditorFlyCameraKey = [](const InputEvent& input) {
            if (input.type != InputEvent::KeyDown && input.type != InputEvent::KeyUp)
                return false;
            switch (input.key)
            {
            case Key_W:
            case Key_A:
            case Key_S:
            case Key_D:
            case Key_Space:
            case Key_Control:
            case Key_Shift:
                return true;
            default:
                return false;
            }
        };
#if defined(IXTREEME_WITH_EDITOR)
        sceneViewInputTarget = runtimeSession->IsMapEditorOpen() && editorImGui.IsSceneViewInputTarget(event);
        if (runtimeSession->IsMapEditorOpen() && event.type == InputEvent::MouseDown)
            editorImGui.SetSceneViewKeyboardFocus(sceneViewInputTarget);
        if (sceneViewInputTarget)
            viewportEvent = editorImGui.MapInputToSceneView(event);
        if (runtimeSession->IsMapEditorOpen() &&
            sceneViewInputTarget &&
            editorImGui.IsSceneGizmoInputActive() &&
            (event.type == InputEvent::MouseMove ||
             ((event.type == InputEvent::MouseDown || event.type == InputEvent::MouseUp) &&
              event.button == MouseButton_Left)))
        {
            editorObjectDragActive = false;
            return;
        }
#endif
        const bool editorFlyCameraKey =
            runtimeSession->IsMapEditorOpen() &&
            cameraController.IsFreeCameraEnabled() &&
            isEditorFlyCameraKey(event);
        if (editorFlyCameraKey)
        {
            const auto viewportDiag = editorImGui.GetViewportInputDiagnostics();
            const bool lastMouseInsideViewport =
                editorHasMousePosition &&
                viewportDiag.sceneViewRectValid &&
                static_cast<float>(editorLastMouseX) >= viewportDiag.sceneViewMin[0] &&
                static_cast<float>(editorLastMouseX) < viewportDiag.sceneViewMin[0] + viewportDiag.sceneViewSize[0] &&
                static_cast<float>(editorLastMouseY) >= viewportDiag.sceneViewMin[1] &&
                static_cast<float>(editorLastMouseY) < viewportDiag.sceneViewMin[1] + viewportDiag.sceneViewSize[1];
            const bool viewportHovered = lastMouseInsideViewport || viewportDiag.sceneViewHovered;
            const bool wantCaptureKeyboard = editorImGui.IsTextInputActive();
            const bool gateOpen = viewportHovered && !wantCaptureKeyboard;
            editorFlyMovement.ApplyGatedEdge(event, gateOpen);
            return;
        }
        // While a player character is being controlled (Play + editor free-fly handed off),
        // gameplay input must keep flowing even when the cursor is over the Game view panel
        // (ImGui reports WantCaptureMouse there); otherwise mouse-look would clear WASD.
        bool playerInputActive = false;
#if defined(IXTREEME_WITH_EDITOR)
        playerInputActive = editorPlay.state.mode == EditorPlayMode::Play && !cameraController.IsFreeCameraEnabled();
#endif
        if (editorImGui.WantsInputCapture(event) && !sceneViewInputTarget && !editorFlyCameraKey &&
            !playerInputActive)
        {
            movement.Clear();
            return;
        }

        if (runtimeSession->IsInWorld() && event.type == InputEvent::KeyDown && event.key == Key_Escape)
        {
            if (runtimeUi->IsSettingsVisible())
                runtimeUi->HideSettings();
            else
                runtimeUi->ToggleInGameMenu();
            movement.Clear();
            return;
        }
        if (runtimeSession->IsInWorld() && event.type == InputEvent::KeyDown && event.key == Key_I)
        {
            runtimeUi->ToggleInventory();
            movement.Clear();
            return;
        }

        if (runtimeUi->OnInput(event))
        {
            movement.Clear();
            return;
        }

        const bool editorTextInputFocused = runtimeSession->IsMapEditorOpen() && runtimeSession->IsTextInputFocused();
        if (editorTextInputFocused)
            movement.Clear();
        else
            movement.Apply(event);

        if (runtimeSession->IsInWorld() && event.type == InputEvent::MouseDown && event.button == MouseButton_Left &&
            hasLastPickCamera && !runtimeSession->IsMapEditorOpen())
        {
            selectedTargetNetId = PickRenderEntityTarget(lastPickEntities,
                                               lastPickCamera,
                                               renderSize.width,
                                               renderSize.height,
                                               event.x,
                                               event.y);
            Tracenf("[PICK] selected render entity net_id=%u", selectedTargetNetId);
            return;
        }
        if (runtimeSession->IsInWorld() && !runtimeSession->IsMapEditorOpen() &&
            event.type == InputEvent::KeyDown && event.key == Key_F)
        {
            if (selectedTargetNetId != 0)
                runtimeSession->SendAttackTarget(selectedTargetNetId);
            return;
        }
#if defined(IXTREEME_WITH_EDITOR)
        if (event.type == InputEvent::KeyDown && event.key == Key_F2 && runtimeSession->IsMapEditorOpen())
        {
            if (runtimeSession->OnInput(event))
                return;
        }
        if (event.type == InputEvent::KeyDown && event.key == Key_F2 && terrainOk)
        {
            terrain.ToggleWalkabilityDebug();
            if (!terrain.IsWalkabilityDebugEnabled() && runtimeSession->IsMapEditorOpen())
            {
                runtimeSession->ToggleMapEditor();
                terrain.SetMapEditorOpen(false);
                cameraController.SetFreeCameraEnabled(false);
            }
            return;
        }
        if (event.type == InputEvent::KeyDown && event.key == Key_F4 && terrainOk &&
            runtimeSession->IsInWorld())
        {
            runtimeSession->ToggleMapEditor();
            runtimeSession->ClearKeyboardFocus();
            terrain.SetMapEditorOpen(runtimeSession->IsMapEditorOpen());
            cameraController.SetFreeCameraEnabled(runtimeSession->IsMapEditorOpen());
            if (!runtimeSession->IsMapEditorOpen())
            {
                selectedEditorObject = {};
                editorObjectDragActive = false;
                runtimeSession->SetEditorStatus("Editor closed");
            }
            else
            {
                runtimeSession->SetEditorStatus("Editor fly camera active: RMB look, WASD move, Space/Ctrl up/down");
            }
            return;
        }

        if (terrainOk && runtimeSession->IsMapEditorOpen()
#if defined(IXTREEME_WITH_EDITOR)
            && editorPlay.state.mode == EditorPlayMode::Edit
#endif
            )
        {
            if (editorTextInputFocused &&
                (event.type == InputEvent::KeyDown ||
                 event.type == InputEvent::KeyUp ||
                 event.type == InputEvent::Char))
            {
                runtimeSession->OnInput(event);
                if (event.type == InputEvent::KeyDown && event.key == Key_Enter)
                    runtimeSession->ClearKeyboardFocus();
                return;
            }

            if (event.type == InputEvent::KeyDown || event.type == InputEvent::KeyUp)
            {
                if (event.type == InputEvent::KeyDown)
                {
                    if (event.key == Key_W)
                    {
                        editorGizmoMode = EditorGizmoMode::Translate;
                        runtimeSession->SetEditorStatus("Gizmo: translate");
                    }
                    else if (event.key == Key_E)
                    {
                        editorGizmoMode = EditorGizmoMode::Rotate;
                        runtimeSession->SetEditorStatus("Gizmo: rotate");
                    }
                    else if (event.key == Key_R)
                    {
                        editorGizmoMode = EditorGizmoMode::Scale;
                        runtimeSession->SetEditorStatus("Gizmo: scale");
                    }
                    else if (event.key == Key_Delete && selectedEditorObject.type == SelectedEditorObjectType::WaterBody)
                    {
                        editorWaterBodies.erase(std::remove_if(editorWaterBodies.begin(), editorWaterBodies.end(),
                            [&](const WaterBody& body) { return body.id == selectedEditorObject.id; }), editorWaterBodies.end());
                        runtimeSession->SetEditorStatus("Deleted water body #" + std::to_string(selectedEditorObject.id));
                        selectedEditorObject = {};
                        editorWaterBodiesDirty = true;
                        return;
                    }
                    else if (event.key == Key_Delete && selectedEditorObject.type == SelectedEditorObjectType::MeshEntity)
                    {
                        removeStaticMeshSpatialEntity(selectedEditorObject.id);
                        editorMeshEntities.erase(std::remove_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                            [&](const MeshSceneEntity& mesh) { return mesh.id == selectedEditorObject.id; }), editorMeshEntities.end());
                        rebuildMeshEntityLookup();
                        runtimeSession->SetEditorStatus("Deleted mesh entity #" + std::to_string(selectedEditorObject.id));
                        selectedEditorObject = {};
                        SceneManager::Instance().MarkDirty();
                        return;
                    }
                }
                if (terrain.HandleEditorInput(viewportEvent))
                    return;
            }

            bool consumedByEditorUi = false;
            if (event.type == InputEvent::MouseMove ||
                event.type == InputEvent::MouseDown ||
                event.type == InputEvent::MouseUp ||
                event.type == InputEvent::MouseWheel)
            {
                const MapEditorSettings editorSettings = editorImGui.GetMapEditorSettings();
                const bool terrainToolActiveForDiag =
                    editorSettings.toolMode == MapEditorToolMode::Heightmap ||
                    editorSettings.toolMode == MapEditorToolMode::SplatPaint;
                const bool shouldLogSculptDiag =
                    QuietLogsForLodDiag()
                        ? terrainToolActiveForDiag
                        : (terrainToolActiveForDiag ||
                            event.type == InputEvent::MouseDown ||
                            event.type == InputEvent::MouseUp);
                const auto viewportDiag = editorImGui.GetViewportInputDiagnostics();
                auto logSculptGateState = [&](const char* stage, bool brushReached, const char* reason) {
                    if (!shouldLogSculptDiag)
                        return;
                    const bool wantCaptureMouse = editorImGui.WantsInputCapture(event);
                    const bool isLeftMouseDown = event.type == InputEvent::MouseDown && event.button == MouseButton_Left;
                    const bool dropTargetCapturing =
                        viewportDiag.dropTargetActive ||
                        viewportDiag.overlayDropTargetActive;
                    Tracenf("[SCULPT-DIAG] viewport mouseDown=%s drag=%s pos=(%d,%d) sculptModeActive=%s activeTool=%s event=%s stage=%s",
                        isLeftMouseDown ? "yes" : "no",
                        (editorLeftMouseHeld || editorObjectDragActive || viewportDiag.assetDragActive) ? "yes" : "no",
                        event.x,
                        event.y,
                        terrainToolActiveForDiag ? "yes" : "no",
                        SculptDiagToolModeName(editorSettings.toolMode),
                        InputEventTypeName(event.type),
                        stage ? stage : "unknown");
                    Tracenf("[SCULPT-DIAG] gate imgui WantCaptureMouse=%s",
                        wantCaptureMouse ? "yes" : "no");
                    if (viewportDiag.hoveredItemId != 0)
                    {
                        Tracenf("[SCULPT-DIAG] gate hoveredItem=0x%08x dropTargetCapturing=%s dropVisible=%s assetDrag=%s dropHovered=%s overlayHovered=%s activeItem=0x%08x",
                            viewportDiag.hoveredItemId,
                            dropTargetCapturing ? "yes" : "no",
                            viewportDiag.dropTargetVisible ? "yes" : "no",
                            viewportDiag.assetDragActive ? "yes" : "no",
                            viewportDiag.dropTargetHovered ? "yes" : "no",
                            viewportDiag.overlayDropTargetHovered ? "yes" : "no",
                            viewportDiag.activeItemId);
                    }
                    else
                    {
                        Tracenf("[SCULPT-DIAG] gate hoveredItem=none dropTargetCapturing=%s dropVisible=%s assetDrag=%s dropHovered=%s overlayHovered=%s activeItem=0x%08x",
                            dropTargetCapturing ? "yes" : "no",
                            viewportDiag.dropTargetVisible ? "yes" : "no",
                            viewportDiag.assetDragActive ? "yes" : "no",
                            viewportDiag.dropTargetHovered ? "yes" : "no",
                            viewportDiag.overlayDropTargetHovered ? "yes" : "no",
                            viewportDiag.activeItemId);
                    }
                    Tracenf("[SCULPT-DIAG] gate sculptToolSelected=%s",
                        terrainToolActiveForDiag ? "yes" : "no");
                    Tracenf("[SCULPT-DIAG] gate mapLoadedFlag=%s",
                        terrain.IsMapLoadedForDiagnostics() ? "yes" : "no");
                    Tracenf("[SCULPT-DIAG] gate terrainTarget=%p",
                        terrain.HasTerrain() ? static_cast<void*>(&terrain) : nullptr);
                    Tracenf("[SCULPT-DIAG] brush handler reached=%s reason=%s consumedByEditorUi=%s",
                        brushReached ? "yes" : "no",
                        reason ? reason : "n/a",
                        consumedByEditorUi ? "yes" : "no");
                };

                logSculptGateState("before-runtime-ui", false, "pre-runtime-ui");
                consumedByEditorUi = runtimeSession->OnInput(event);
                logSculptGateState("after-runtime-ui", false, consumedByEditorUi ? "runtime-ui-consumed" : "runtime-ui-pass");
                auto selectedWaterBodyIt = [&]() {
                    return std::find_if(editorWaterBodies.begin(), editorWaterBodies.end(),
                        [&](const WaterBody& body) {
                            return selectedEditorObject.type == SelectedEditorObjectType::WaterBody &&
                                body.id == selectedEditorObject.id;
                        });
                };
                auto updateWaterSculptCursor = [&]() -> std::optional<WorldVec3> {
                    if (!editorSettings.waterSculptActive || !hasLastPickCamera ||
                        selectedEditorObject.type != SelectedEditorObjectType::WaterBody)
                    {
                        terrain.SetWaterSculptBrush(false, 0.0f, 0.0f, editorSettings.waterSculptRadiusMeters,
                            editorSettings.waterSculptAdd);
                        return std::nullopt;
                    }
                    auto bodyIt = selectedWaterBodyIt();
                    if (bodyIt == editorWaterBodies.end())
                    {
                        terrain.SetWaterSculptBrush(false, 0.0f, 0.0f, editorSettings.waterSculptRadiusMeters,
                            editorSettings.waterSculptAdd);
                        return std::nullopt;
                    }
                    std::optional<WorldVec3> hit = RaycastTerrainPoint(terrain,
                        lastPickCamera,
                        renderSize.width,
                        renderSize.height,
                        viewportEvent.x,
                        viewportEvent.y);
                    if (!hit)
                    {
                        terrain.SetWaterSculptBrush(false, 0.0f, 0.0f, editorSettings.waterSculptRadiusMeters,
                            editorSettings.waterSculptAdd);
                        return std::nullopt;
                    }
                    terrain.SetWaterSculptBrush(true, hit->x, hit->z, editorSettings.waterSculptRadiusMeters,
                        editorSettings.waterSculptAdd);
                    return hit;
                };
                auto applyWaterSculptAtCursor = [&](const WorldVec3& hit) -> std::uint32_t {
                    auto bodyIt = selectedWaterBodyIt();
                    if (bodyIt == editorWaterBodies.end() || bodyIt->id != waterSculptStrokeBodyId)
                        return 0;
                    return ApplyWaterSculptBrush(*bodyIt, hit, editorSettings.waterSculptRadiusMeters,
                        editorSettings.waterSculptAdd);
                };

                if (waterSculptStrokeActive)
                {
                    if (event.type == InputEvent::MouseMove)
                    {
                        if (std::optional<WorldVec3> hit = updateWaterSculptCursor())
                            waterSculptStrokeModifiedCells += applyWaterSculptAtCursor(*hit);
                        return;
                    }
                    if (event.type == InputEvent::MouseUp && event.button == MouseButton_Left)
                    {
                        if (std::optional<WorldVec3> hit = updateWaterSculptCursor())
                            waterSculptStrokeModifiedCells += applyWaterSculptAtCursor(*hit);
                        waterSculptStrokeActive = false;
                        if (waterSculptStrokeModifiedCells > 0)
                        {
                            editorWaterBodiesDirty = true;
                            waterSculptMeshRegenPending = true;
                            runtimeSession->SetEditorStatus("Water sculpt stroke: " +
                                std::to_string(waterSculptStrokeModifiedCells) + " cells modified");
                        }
                        Tracenf("[WATER-OBJ-5] Brush stroke ended: body_id=%u mode=%s cells_modified=%u",
                            waterSculptStrokeBodyId,
                            editorSettings.waterSculptAdd ? "add" : "remove",
                            waterSculptStrokeModifiedCells);
                        waterSculptStrokeBodyId = 0;
                        waterSculptStrokeModifiedCells = 0;
                        return;
                    }
                }

                if (!consumedByEditorUi && editorSettings.waterSculptActive &&
                    selectedEditorObject.type == SelectedEditorObjectType::WaterBody &&
                    (event.type == InputEvent::MouseMove ||
                     (event.type == InputEvent::MouseDown && event.button == MouseButton_Left)))
                {
                    std::optional<WorldVec3> hit = updateWaterSculptCursor();
                    if (event.type == InputEvent::MouseDown && event.button == MouseButton_Left)
                    {
                        waterSculptStrokeActive = true;
                        waterSculptStrokeBodyId = selectedEditorObject.id;
                        waterSculptStrokeModifiedCells = 0;
                        editorObjectDragActive = false;
                        if (hit)
                            waterSculptStrokeModifiedCells += applyWaterSculptAtCursor(*hit);
                    }
                    return;
                }
                if (!editorSettings.waterSculptActive && !waterSculptStrokeActive)
                    terrain.SetWaterSculptBrush(false, 0.0f, 0.0f, editorSettings.waterSculptRadiusMeters,
                        editorSettings.waterSculptAdd);

                const bool terrainToolActive =
                    editorSettings.toolMode == MapEditorToolMode::Heightmap ||
                    editorSettings.toolMode == MapEditorToolMode::SplatPaint;
                const bool rightMouseButtonEvent =
                    (event.type == InputEvent::MouseDown || event.type == InputEvent::MouseUp) &&
                    event.button == MouseButton_Right;
                const bool cameraRmbInput =
                    rightMouseButtonEvent ||
                    (event.type == InputEvent::MouseMove && editorRightMouseHeld);
                const bool terrainBrushInput =
                    event.type == InputEvent::MouseMove ||
                    ((event.type == InputEvent::MouseDown || event.type == InputEvent::MouseUp) &&
                     event.button == MouseButton_Left);
                if (!consumedByEditorUi && terrainToolActive && !cameraRmbInput && terrainBrushInput)
                {
                    editorObjectDragActive = false;
                    logSculptGateState("before-brush-handler", true, "terrain-tool-route");
                    terrain.HandleEditorInput(viewportEvent);
                    return;
                }
                if (shouldLogSculptDiag &&
                    (event.type == InputEvent::MouseMove ||
                     event.type == InputEvent::MouseDown ||
                     event.type == InputEvent::MouseUp))
                {
                    logSculptGateState("terrain-tool-route-skipped", false,
                        consumedByEditorUi ? "consumed-by-editor-ui" :
                        (!terrainToolActive ? "terrain-tool-inactive" : "event-not-routed"));
                }

                if (event.type == InputEvent::MouseUp)
                {
                    if (event.button == MouseButton_Left)
                        editorObjectDragActive = false;
                    terrain.HandleEditorInput(viewportEvent);
                }
                else if (!consumedByEditorUi)
                {
                    if (event.type == InputEvent::MouseDown)
                        runtimeSession->ClearKeyboardFocus();

                    if (event.type == InputEvent::MouseDown && event.button == MouseButton_Left && hasLastPickCamera)
                    {
                        if (auto pointId = PickDynamicLight(editorPointLights,
                                lastPickCamera,
                                renderSize.width,
                                renderSize.height,
                                viewportEvent.x,
                                viewportEvent.y))
                        {
                            selectedEditorObject = {SelectedEditorObjectType::PointLight, *pointId};
                            editorObjectDragActive = false;
                            editorObjectDragLastX = event.x;
                            editorObjectDragLastY = event.y;
                            runtimeSession->SetEditorStatus("Selected point light #" + std::to_string(*pointId));
                            return;
                        }
                        if (auto spotId = PickDynamicLight(editorSpotLights,
                                lastPickCamera,
                                renderSize.width,
                                renderSize.height,
                                viewportEvent.x,
                                viewportEvent.y))
                        {
                            selectedEditorObject = {SelectedEditorObjectType::SpotLight, *spotId};
                            editorObjectDragActive = false;
                            editorObjectDragLastX = event.x;
                            editorObjectDragLastY = event.y;
                            runtimeSession->SetEditorStatus("Selected spot light #" + std::to_string(*spotId));
                            return;
                        }
                        if (auto cameraId = PickDynamicLight(editorCameras,
                                lastPickCamera,
                                renderSize.width,
                                renderSize.height,
                                viewportEvent.x,
                                viewportEvent.y))
                        {
                            selectedEditorObject = {SelectedEditorObjectType::Camera, *cameraId};
                            editorObjectDragActive = false;
                            editorObjectDragLastX = event.x;
                            editorObjectDragLastY = event.y;
                            runtimeSession->SetEditorStatus("Selected camera #" + std::to_string(*cameraId));
                            return;
                        }
                        if (auto meshId = PickMeshEntity(editorMeshEntities,
                                lastPickCamera,
                                renderSize.width,
                                renderSize.height,
                                viewportEvent.x,
                                viewportEvent.y,
                                resolveMeshRuntimePath,
                                getStaticMeshRenderer))
                        {
                            selectedEditorObject = {SelectedEditorObjectType::MeshEntity, *meshId};
                            editorObjectDragActive = false;
                            editorObjectDragLastX = event.x;
                            editorObjectDragLastY = event.y;
                            runtimeSession->SetEditorStatus("Selected mesh entity #" + std::to_string(*meshId));
                            return;
                        }
                        if (auto waterId = PickWaterBody(editorWaterBodies,
                                lastPickCamera,
                                renderSize.width,
                                renderSize.height,
                                viewportEvent.x,
                                viewportEvent.y))
                        {
                            selectedEditorObject = {SelectedEditorObjectType::WaterBody, *waterId};
                            editorObjectDragActive = false;
                            editorObjectDragLastX = event.x;
                            editorObjectDragLastY = event.y;
                            runtimeSession->SetEditorStatus("Selected water body #" + std::to_string(*waterId));
                            return;
                        }
                        if (terrain.HasTerrain() &&
                            RaycastTerrainPoint(terrain,
                                lastPickCamera,
                                renderSize.width,
                                renderSize.height,
                                viewportEvent.x,
                                viewportEvent.y))
                        {
                            selectedEditorObject = {SelectedEditorObjectType::Terrain, 1u};
                            editorObjectDragActive = false;
                            runtimeSession->SetEditorStatus("Selected terrain");
                            return;
                        }
                    }
                    if (event.type == InputEvent::MouseMove && editorObjectDragActive &&
                        selectedEditorObject.type != SelectedEditorObjectType::None &&
                        selectedEditorObject.type != SelectedEditorObjectType::WaterBody)
                    {
                        const int dx = event.x - editorObjectDragLastX;
                        const int dy = event.y - editorObjectDragLastY;
                        editorObjectDragLastX = event.x;
                        editorObjectDragLastY = event.y;
                        auto moveLight = [&](auto& light) {
                            WorldVec3 position{light.position[0], light.position[1], light.position[2]};
                            const float scale = 0.025f * std::max(1.0f, xm::Distance(position, lastPickCamera.eye));
                            if (editorGizmoMode == EditorGizmoMode::Translate)
                            {
                                position = position +
                                    CameraRight(lastPickCamera) * (static_cast<float>(dx) * scale) +
                                    CameraUp(lastPickCamera) * (static_cast<float>(-dy) * scale);
                                if (editorGizmoSnapEnabled)
                                    position = SnapPoint(position, editorGizmoSnapValue);
                                light.position[0] = position.x;
                                light.position[1] = position.y;
                                light.position[2] = position.z;
                            }
                            else if (editorGizmoMode == EditorGizmoMode::Scale)
                            {
                                const float delta = static_cast<float>(dx - dy) * 0.05f * std::max(1.0f, scale);
                                light.radius = std::clamp(light.radius + delta, 0.5f, 100.0f);
                                if (editorGizmoSnapEnabled)
                                    light.radius = std::clamp(SnapValue(light.radius, editorGizmoSnapValue), 0.5f, 100.0f);
                            }
                            else if constexpr (std::is_same_v<std::decay_t<decltype(light)>, SpotLight>)
                            {
                                if (editorGizmoMode == EditorGizmoMode::Rotate)
                                {
                                    light.rotation[1] += static_cast<float>(dx) * 0.01f;
                                    light.rotation[0] += static_cast<float>(-dy) * 0.01f;
                                }
                            }
                        };
                        if (selectedEditorObject.type == SelectedEditorObjectType::PointLight)
                        {
                            auto it = std::find_if(editorPointLights.begin(), editorPointLights.end(),
                                [&](const PointLight& light) { return light.id == selectedEditorObject.id; });
                            if (it != editorPointLights.end())
                            {
                                moveLight(*it);
                                return;
                            }
                        }
                        if (selectedEditorObject.type == SelectedEditorObjectType::SpotLight)
                        {
                            auto it = std::find_if(editorSpotLights.begin(), editorSpotLights.end(),
                                [&](const SpotLight& light) { return light.id == selectedEditorObject.id; });
                            if (it != editorSpotLights.end())
                            {
                                moveLight(*it);
                                return;
                            }
                        }
                        if (selectedEditorObject.type == SelectedEditorObjectType::MeshEntity)
                        {
                            auto it = std::find_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                                [&](const MeshSceneEntity& mesh) { return mesh.id == selectedEditorObject.id; });
                            if (it != editorMeshEntities.end())
                            {
                                WorldVec3 position{it->position[0], it->position[1], it->position[2]};
                                const float scale = 0.025f * std::max(1.0f, xm::Distance(position, lastPickCamera.eye));
                                if (editorGizmoMode == EditorGizmoMode::Translate)
                                {
                                    position = position +
                                        CameraRight(lastPickCamera) * (static_cast<float>(dx) * scale) +
                                        CameraUp(lastPickCamera) * (static_cast<float>(-dy) * scale);
                                    if (editorGizmoSnapEnabled)
                                        position = SnapPoint(position, editorGizmoSnapValue);
                                    it->position[0] = position.x;
                                    it->position[1] = position.y;
                                    it->position[2] = position.z;
                                }
                                else if (editorGizmoMode == EditorGizmoMode::Scale)
                                {
                                    const float delta = static_cast<float>(dx - dy) * 0.01f * std::max(1.0f, scale);
                                    it->scale[0] = std::max(0.001f, it->scale[0] + delta);
                                    it->scale[1] = std::max(0.001f, it->scale[1] + delta);
                                    it->scale[2] = std::max(0.001f, it->scale[2] + delta);
                                    if (editorGizmoSnapEnabled)
                                    {
                                        it->scale[0] = std::max(0.001f, SnapValue(it->scale[0], editorGizmoSnapValue));
                                        it->scale[1] = std::max(0.001f, SnapValue(it->scale[1], editorGizmoSnapValue));
                                        it->scale[2] = std::max(0.001f, SnapValue(it->scale[2], editorGizmoSnapValue));
                                    }
                                }
                                else if (editorGizmoMode == EditorGizmoMode::Rotate)
                                {
                                    it->rotation[1] += static_cast<float>(dx) * 0.01f;
                                    it->rotation[0] += static_cast<float>(-dy) * 0.01f;
                                }
                                syncStaticMeshSpatialEntity(*it);
                                SceneManager::Instance().MarkDirty();
                                return;
                            }
                        }
                    }
                    if (event.type == InputEvent::MouseMove && editorObjectDragActive &&
                        selectedEditorObject.type == SelectedEditorObjectType::WaterBody)
                    {
                        const int dx = event.x - editorObjectDragLastX;
                        const int dy = event.y - editorObjectDragLastY;
                        editorObjectDragLastX = event.x;
                        editorObjectDragLastY = event.y;
                        auto it = std::find_if(editorWaterBodies.begin(), editorWaterBodies.end(),
                            [&](const WaterBody& body) { return body.id == selectedEditorObject.id; });
                        if (it != editorWaterBodies.end())
                        {
                            WaterBodyEditorState state = BuildWaterBodyEditorState(editorWaterBodies, it->id);
                            const WorldVec3 center{state.center[0], state.center[1], state.center[2]};
                            const float scale = 0.025f * std::max(1.0f, xm::Distance(center, lastPickCamera.eye));
                            if (editorGizmoMode == EditorGizmoMode::Translate)
                            {
                                const WorldVec3 moved = center +
                                    CameraRight(lastPickCamera) * (static_cast<float>(dx) * scale) +
                                    CameraUp(lastPickCamera) * (static_cast<float>(-dy) * scale);
                                const WorldVec3 snapped = editorGizmoSnapEnabled ? SnapPoint(moved, editorGizmoSnapValue) : moved;
                                state.center[0] = snapped.x;
                                state.center[1] = snapped.y;
                                state.center[2] = snapped.z;
                            }
                            else if (editorGizmoMode == EditorGizmoMode::Scale)
                            {
                                const float delta = static_cast<float>(dx - dy) * 0.05f * std::max(1.0f, scale);
                                state.width = std::clamp(state.width + delta, 1.0f, 200.0f);
                                state.depth = std::clamp(state.depth + delta, 1.0f, 200.0f);
                                if (editorGizmoSnapEnabled)
                                {
                                    state.width = std::clamp(SnapValue(state.width, editorGizmoSnapValue), 1.0f, 200.0f);
                                    state.depth = std::clamp(SnapValue(state.depth, editorGizmoSnapValue), 1.0f, 200.0f);
                                }
                            }
                            ApplyWaterBodyEditorStateToBody(*it, state);
                            editorWaterBodiesDirty = true;
                            return;
                        }
                    }
                    terrain.HandleEditorInput(viewportEvent);
                }

                const bool cameraMouse =
                    event.type == InputEvent::MouseMove ||
                    ((event.type == InputEvent::MouseDown || event.type == InputEvent::MouseUp) &&
                     event.button == MouseButton_Right);
                if (!consumedByEditorUi && runtimeSession->IsInWorld() && cameraMouse)
                    cameraController.HandleInput(event);
                return;
            }
        }

        if (terrainOk
#if defined(IXTREEME_WITH_EDITOR)
            && editorPlay.state.mode == EditorPlayMode::Edit
#endif
            && terrain.HandleEditorInput(viewportEvent))
            return;
#endif
        if (runtimeSession->IsInWorld() && cameraController.HandleInput(event))
            return;

        if (!runtimeSession->OnInput(event))
        {
            // TODO: forward unconsumed events to the game/3D scene input path.
            //LogUnhandledInput(event);
        }
    });

#if defined(IXTREEME_WITH_EDITOR)
    std::vector<std::string> pendingDroppedFiles;
    window.SetFileDropCallback([&pendingDroppedFiles](const std::vector<std::string>& paths)
    {
        pendingDroppedFiles.insert(pendingDroppedFiles.end(), paths.begin(), paths.end());
    });
#endif

    const auto startTime = std::chrono::steady_clock::now();
    double previousSeconds = 0.0;
    bool debugDisableShadowPass = false;
    bool debugDisableWaterReflectionPass = false;
#if defined(IXTREEME_WITH_EDITOR)
    EngineStats engineStats{};
    double statsAccumSeconds = 0.0;
    double statsFrameMsAccum = 0.0;
    double statsMinFrameMs = std::numeric_limits<double>::max();
    double statsMaxFrameMs = 0.0;
    std::uint32_t statsFrameCount = 0;
    std::clock_t statsPreviousCpuClock = std::clock();
    double statsPreviousCpuSampleSeconds = 0.0;
    const unsigned int statsHardwareThreads = std::max(1u, std::thread::hardware_concurrency());
    double lastPerfLogSeconds = -1000.0;
    bool debugDisableAssetLibraryDiscovery = false;
    bool debugDisableAssetWatcherPoll = false;
    bool debugDisableHierarchyIteration = false;
    bool debugShowPhysicsColliders = false;
    bool debugShowPhysicsContacts = false;
    bool debugShowPhysicsBodyCenters = false;
    bool dumpFrameProfileRequested = false;
    Tracen("[VISIBILITY-RESPECT] shadow_pass=yes water_reflection_pass=yes main_pass=yes");
#endif
    bool running = true;
    while (running)
    {
        const auto frameCpuStart = std::chrono::steady_clock::now();
        FrameCpuProfile frameProfile{};
        running = window.PumpMessages();

#if defined(IXTREEME_WITH_EDITOR)
        const std::uint64_t assetLibraryDiagFrame = device.GetFrameNumber() + 1u;
        bool assetLibraryDiagFrameActive = false;
        const auto assetDiscoveryBegin = std::chrono::steady_clock::now();
        if (!debugDisableAssetLibraryDiscovery)
        {
            AssetLibrary::BeginMaterialDiscoveryFrame(assetLibraryDiagFrame);
            assetLibraryDiagFrameActive = true;
        }
        frameProfile.assetLibraryPollMs += MillisecondsBetween(assetDiscoveryBegin, std::chrono::steady_clock::now());

        const auto assetWatcherBegin = std::chrono::steady_clock::now();
        bool assetWatcherChanged = false;
        if (!debugDisableAssetWatcherPoll)
            assetWatcherChanged = AssetWatcher::Instance().processPendingEvents();
        frameProfile.assetWatcherPollMs = MillisecondsBetween(assetWatcherBegin, std::chrono::steady_clock::now());

        if (assetWatcherChanged)
        {
            const auto assetRefreshBegin = std::chrono::steady_clock::now();
            if (!debugDisableAssetLibraryDiscovery)
                editorImGui.RefreshAssetLibrary();
            frameProfile.assetLibraryPollMs += MillisecondsBetween(assetRefreshBegin, std::chrono::steady_clock::now());
        }
#endif

        uint32_t width = 0;
        uint32_t height = 0;
        if (window.ConsumeResize(width, height))
        {
            Tracenf("[MAIN] Resize event consumed: %ux%u, calling device.Resize()", width, height);
            if (device.Resize(width, height))
            {
                Tracen("[MAIN] device.Resize() returned true, recreating pipelines");
                if (offscreenSceneOk)
                {
                    offscreenSceneOk = offscreenScene.Recreate(device, effectiveRenderExtent());
                    if (offscreenSceneOk)
                    {
                        renderSize = offscreenScene.GetExtent();
                        for (auto& [skinnedPath, skinnedEntry] : skinnedMeshCache)
                        {
                            (void)skinnedPath;
                            if (skinnedEntry.renderer)
                                skinnedEntry.renderer->SetMainRenderPass(offscreenScene.GetRenderPass());
                        }
                        for (auto& [path, entry] : staticMeshCache)
                        {
                            (void)path;
                            if (entry.renderer)
                                entry.renderer->SetMainRenderPass(offscreenScene.GetRenderPass());
                        }
                        if (terrainOk)
                        {
                            terrain.SetMainRenderPass(offscreenScene.GetRenderPass());
                            terrain.SetWaterRefractionInputs(offscreenScene.GetSceneColorSnapshotView(),
                                offscreenScene.GetSceneDepthSnapshotView(),
                                offscreenScene.GetLinearSampler(),
                                offscreenScene.GetExtent());
                        }
                        if (selectionOutlinesOk)
                            selectionOutlines.SetMainRenderPass(offscreenScene.GetRenderPass());
#if defined(IXTREEME_WITH_EDITOR)
                        editorImGui.SetSceneViewTexture(offscreenScene.GetLinearSampler(),
                            offscreenScene.GetSceneColorView(),
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            offscreenScene.GetExtent());
                        if (gameViewOk)
                        {
                            gameViewOk = gameView.Recreate(device, renderSize);
                            editorImGui.SetGameViewTexture(
                                gameViewOk ? gameView.GetLinearSampler() : VK_NULL_HANDLE,
                                gameViewOk ? gameView.GetSceneColorView() : VK_NULL_HANDLE,
                                gameViewOk ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                                gameViewOk ? gameView.GetExtent() : VkExtent2D{});
                        }
#endif
                    }
#if defined(IXTREEME_WITH_EDITOR)
                    else
                    {
                        editorImGui.SetSceneViewTexture(VK_NULL_HANDLE,
                            VK_NULL_HANDLE,
                            VK_IMAGE_LAYOUT_UNDEFINED,
                            {});
                    }
#endif
                }
                for (auto& [skinnedPath, skinnedEntry] : skinnedMeshCache)
                {
                    (void)skinnedPath;
                    if (skinnedEntry.renderer)
                        skinnedEntry.renderer->RecreatePipeline(device);
                }
                for (auto& [path, entry] : staticMeshCache)
                {
                    (void)path;
                    if (entry.renderer)
                        entry.renderer->RecreatePipeline(device);
                }
                if (terrainOk)
                    terrain.RecreatePipeline(device);
                if (selectionOutlinesOk)
                    selectionOutlines.RecreatePipeline(device);
                if (worldLabelsOk)
                    worldLabels.RecreatePipeline(device);
                runtimeSession->OnRenderPassChanged(device);
                rmlUi.OnRenderPassChanged(device);
#if defined(IXTREEME_WITH_EDITOR)
                editorImGui.OnRenderPassChanged(device);
#endif
                swapchainSize = device.GetSwapchainExtent();
                if (!offscreenSceneOk)
                    renderSize = swapchainSize;
                runtimeSession->Resize(swapchainSize.width, swapchainSize.height);
                rmlUi.Resize(swapchainSize.width, swapchainSize.height);
            }
            else
            {
                Tracen("[MAIN] device.Resize() returned false (unchanged), skipping pipeline recreate");
            }
        }

#if defined(IXTREEME_WITH_EDITOR)
        if (!pendingDroppedFiles.empty())
        {
            std::vector<std::string> dropped;
            dropped.swap(pendingDroppedFiles);
            Tracenf("[ASSET-DROP] queued file drop import: %zu path(s)", dropped.size());
            editorImGui.ImportExternalFiles(dropped, "dragdrop");
        }
#endif

        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - startTime).count();
        const double deltaSeconds = seconds - previousSeconds;
        previousSeconds = seconds;
#if defined(IXTREEME_WITH_EDITOR)
        const double frameMs = std::max(0.0, deltaSeconds * 1000.0);
        statsAccumSeconds += std::max(0.0, deltaSeconds);
        statsFrameMsAccum += frameMs;
        statsMinFrameMs = std::min(statsMinFrameMs, frameMs);
        statsMaxFrameMs = std::max(statsMaxFrameMs, frameMs);
        ++statsFrameCount;
        engineStats.frameMs = frameMs;
        if (statsAccumSeconds >= 0.25 && statsFrameCount > 0)
        {
            engineStats.fps = static_cast<double>(statsFrameCount) / statsAccumSeconds;
            engineStats.averageFrameMs = statsFrameMsAccum / static_cast<double>(statsFrameCount);
            engineStats.minFrameMs = statsMinFrameMs == std::numeric_limits<double>::max() ? 0.0 : statsMinFrameMs;
            engineStats.maxFrameMs = statsMaxFrameMs;
            engineStats.frameBudgetPercent = (engineStats.averageFrameMs / (1000.0 / 60.0)) * 100.0;

            const std::clock_t cpuClock = std::clock();
            const double cpuSeconds = static_cast<double>(cpuClock - statsPreviousCpuClock) / CLOCKS_PER_SEC;
            const double sampleSeconds = std::max(0.0001, seconds - statsPreviousCpuSampleSeconds);
            engineStats.processCpuPercent =
                std::clamp((cpuSeconds / sampleSeconds) * 100.0 / static_cast<double>(statsHardwareThreads), 0.0, 100.0);
            statsPreviousCpuClock = cpuClock;
            statsPreviousCpuSampleSeconds = seconds;

            if (!QuietLogsForLodDiag() || seconds - lastPerfLogSeconds >= 5.0)
            {
                TraceDiagf("[PERF] fps=%.1f frame_ms=%.2f budget60=%.0f%% cpu=%.1f%% swapchain=%ux%u render=%ux%u",
                    engineStats.fps,
                    engineStats.averageFrameMs,
                    engineStats.frameBudgetPercent,
                    engineStats.processCpuPercent,
                    swapchainSize.width,
                    swapchainSize.height,
                    renderSize.width,
                    renderSize.height);
                lastPerfLogSeconds = seconds;
            }

            statsAccumSeconds = 0.0;
            statsFrameMsAccum = 0.0;
            statsMinFrameMs = std::numeric_limits<double>::max();
            statsMaxFrameMs = 0.0;
            statsFrameCount = 0;
        }
#endif
        terrain.SetPerformanceFps(engineStats.fps);
#if defined(IXTREEME_WITH_EDITOR)
        if (runtimeSession->IsMapEditorOpen() && cameraController.IsFreeCameraEnabled())
            cameraController.Update(deltaSeconds, editorFlyMovement);
        else
            cameraController.Update(deltaSeconds, movement);
#else
        cameraController.Update(deltaSeconds, movement);
#endif
#if defined(IXTREEME_WITH_EDITOR)
        if (editorPlay.state.mode == EditorPlayMode::Play)
        {
            editorPlay.state.elapsedSeconds += deltaSeconds;
            ++editorPlay.state.frameCount;
            // Drive player character controllers BEFORE the physics step: each writes its
            // resolved transform to the mesh; the kinematic sync in stepEditorPhysicsWorld
            // then moves the Jolt body to match (so it pushes dynamic objects).
            if (editorPhysicsWorldActive)
            {
                const PhysicsSceneSettings characterPhysics = SceneManager::Instance().GetCurrentScene().physics;
                const bool characterInputActive = !runtimeSession->IsTextInputFocused();
                for (MeshSceneEntity& characterMesh : editorMeshEntities)
                {
                    if (!characterMesh.hasCharacterController || !characterMesh.characterController.enabled)
                        continue;
                    phys::CharacterControllerComponent cc = characterMesh.characterController;
                    phys::Sanitize(cc);
                    CharacterRuntimeState& characterState = editorCharacterStates[characterMesh.id];
                    if (!characterState.initialized)
                    {
                        characterState.lookYaw = characterMesh.rotation[1];
                        characterState.lookPitch = (cc.cameraMode == phys::CameraMode::ThirdPerson)
                            ? -xm::DegreesToRadians(cc.thirdPersonPitchDegrees)
                            : 0.0f;
                        characterState.initialized = true;
                    }
                    phys::BodyId characterBody = 0;
                    if (auto bodyIt = editorPhysicsBodies.find(characterMesh.id); bodyIt != editorPhysicsBodies.end())
                        characterBody = bodyIt->second;
                    UpdateCharacterController(editorPhysicsWorld, characterBody, characterMesh, cc,
                        characterState, movement, characterInputActive,
                        editorPlayerLookDx, editorPlayerLookDy,
                        characterPhysics.gravity, static_cast<float>(deltaSeconds));
                    syncStaticMeshSpatialEntity(characterMesh);
                }
                editorPlayerLookDx = 0.0f;
                editorPlayerLookDy = 0.0f;
            }
            stepEditorPhysicsWorld(static_cast<float>(deltaSeconds));
        }
#endif
        {
            const auto ecsUpdateBegin = std::chrono::steady_clock::now();
            runtimeSession->UpdateNetwork();
            runtimeSession->SendMoveInput(movement.DirectionAngle(cameraController.MovementYaw()), movement.State());
            runtimeSession->Update(seconds);
            rmlUi.Update();
#if defined(IXTREEME_WITH_EDITOR)
            frameProfile.ecsSystemsUpdateMs = MillisecondsBetween(ecsUpdateBegin, std::chrono::steady_clock::now());
#endif
        }

        std::vector<WorldRenderEntity> frameEntities;
        WorldCamera frameCamera{};
        bool hasFrameCamera = false;
        if (runtimeSession->IsInWorld())
        {
            frameEntities = runtimeSession->GetWorldEntities();
            frameCamera = cameraController.BuildCamera(renderSize.width, renderSize.height);
            hasFrameCamera = true;
            lastPickEntities = frameEntities;
            lastPickCamera = frameCamera;
            hasLastPickCamera = true;
            const bool selectedStillVisible = std::any_of(lastPickEntities.begin(),
                lastPickEntities.end(),
                [selectedTargetNetId](const WorldRenderEntity& entity) {
                    return selectedTargetNetId != 0 && entity.netId == selectedTargetNetId;
                });
            if (selectedTargetNetId != 0 && !selectedStillVisible)
                selectedTargetNetId = 0;

            RmlHudData hudData{};
            const WorldRenderEntity* ownEntity = nullptr;
            const WorldRenderEntity* targetEntity = nullptr;
            for (const WorldRenderEntity& entity : frameEntities)
            {
                if (selectedTargetNetId != 0 && entity.netId == selectedTargetNetId)
                    targetEntity = &entity;
            }
            if (!ownEntity && !frameEntities.empty())
                ownEntity = &frameEntities.front();
            if (ownEntity)
            {
                hudData.playerName = ownEntity->name.empty() ? "Player" : ownEntity->name;
                hudData.playerLevel = static_cast<int>(std::max(1u, ownEntity->level));
                hudData.currentHp = ownEntity->hpCurrent;
                hudData.maxHp = ownEntity->hpMax <= 0.0f ? 1.0f : ownEntity->hpMax;
                const WorldVec3 displayPos = ServerMetersToDisplay(ownEntity->position);
                hudData.playerX = displayPos.x;
                hudData.playerZ = displayPos.z;
            }
            hudData.hasTarget = targetEntity != nullptr && targetEntity != ownEntity;
            if (hudData.hasTarget)
            {
                hudData.targetName = targetEntity->name.empty() ? "Target" : targetEntity->name;
                hudData.targetLevel = static_cast<int>(std::max(1u, targetEntity->level));
                hudData.targetCurrentHp = targetEntity->hpCurrent;
                hudData.targetMaxHp = targetEntity->hpMax <= 0.0f ? 1.0f : targetEntity->hpMax;
            }
            runtimeUi->UpdateHud(hudData);

#if defined(IXTREEME_WITH_EDITOR)
            if (terrainOk)
            {
                terrain.SetMapEditorOpen(runtimeSession->IsMapEditorOpen());
                const MapEditorSettings editorSettings = editorImGui.GetMapEditorSettings();
                terrain.SetMapEditorSettings(editorSettings);
                SceneData pendingScene;
                if (SceneManager::Instance().ConsumePendingScene(pendingScene))
                    sceneRuntime.ApplySceneData(pendingScene);
                MapEditorCommands commands = runtimeSession->ConsumeMapEditorCommands();
                MergeMapEditorCommands(commands, editorImGui.ConsumeCommands());
                if (commands.captureGpuFrame)
                    device.RequestGpuFrameCapture();
                if (commands.dumpFrameProfile)
                    dumpFrameProfileRequested = true;
                if (commands.debugPerfTogglesChanged)
                {
                    debugDisableShadowPass = commands.disableShadowPass;
                    debugDisableWaterReflectionPass = commands.disableWaterReflectionPass;
                    debugDisableAssetLibraryDiscovery = commands.disableAssetLibraryDiscovery;
                    debugDisableAssetWatcherPoll = commands.disableAssetWatcherPoll;
                    debugDisableHierarchyIteration = commands.disableHierarchyIteration;
                    debugShowPhysicsColliders = commands.showPhysicsColliders;
                    debugShowPhysicsContacts = commands.showPhysicsContacts;
                    debugShowPhysicsBodyCenters = commands.showPhysicsBodyCenters;
                    Tracenf("[FRAME-PROFILE] toggles shadow=%s water_reflection=%s asset_library_discovery=%s asset_watcher_poll=%s hierarchy_iteration=%s physics_colliders=%s physics_contacts=%s physics_centers=%s",
                        debugDisableShadowPass ? "disabled" : "enabled",
                        debugDisableWaterReflectionPass ? "disabled" : "enabled",
                        debugDisableAssetLibraryDiscovery ? "disabled" : "enabled",
                        debugDisableAssetWatcherPoll ? "disabled" : "enabled",
                        debugDisableHierarchyIteration ? "disabled" : "enabled",
                        debugShowPhysicsColliders ? "shown" : "selected-only",
                        debugShowPhysicsContacts ? "shown" : "hidden",
                        debugShowPhysicsBodyCenters ? "shown" : "hidden");
                }
                if (commands.physicsLayerMatrixChanged)
                {
                    editorPhysicsLayerMatrix = commands.physicsLayerMatrix;
                    Tracen("[PHYSICS-LAYER] matrix applied");
                    runtimeSession->SetEditorStatus("Physics collision matrix updated");
                    if (editorPlay.state.mode == EditorPlayMode::Play && editorPhysicsWorldActive)
                        rebuildEditorPhysicsWorld();
                }
                if (commands.physicsRaycastFromCamera)
                {
                    if (editorPlay.state.mode != EditorPlayMode::Play || !editorPhysicsWorldActive)
                    {
                        runtimeSession->SetEditorStatus("Physics raycast needs Play mode");
                        Tracen("[PHYSICS-QUERY] raycast skipped reason=world-inactive");
                    }
                    else
                    {
                        const WorldVec3 origin = frameCamera.eye;
                        const WorldVec3 direction = WorldCameraForward(frameCamera);
                        phys::PhysicsQueryFilter queryFilter{};
                        queryFilter.layerMask = commands.physicsRaycastLayerMask;
                        queryFilter.hitTriggers = commands.physicsRaycastHitTriggers;
                        phys::PhysicsRaycastHit hit{};
                        const float originRaw[3] = {origin.x, origin.y, origin.z};
                        const float directionRaw[3] = {direction.x, direction.y, direction.z};
                        const bool didHit = editorPhysicsWorld.Raycast(
                            originRaw,
                            directionRaw,
                            commands.physicsRaycastDistance,
                            queryFilter,
                            hit);
                        PhysicsDebugLine rayLine{};
                        rayLine.a = origin;
                        rayLine.b = didHit
                            ? WorldVec3{hit.position[0], hit.position[1], hit.position[2]}
                            : origin + direction * commands.physicsRaycastDistance;
                        rayLine.color = didHit
                            ? std::array<float, 4>{1.0f, 0.86f, 0.15f, 0.95f}
                            : std::array<float, 4>{0.55f, 0.62f, 0.75f, 0.65f};
                        rayLine.ttlSeconds = 2.5f;
                        editorPhysicsDebugLines.push_back(rayLine);
                        if (didHit)
                        {
                            PhysicsDebugContact marker{};
                            marker.point = rayLine.b;
                            marker.normal = {hit.normal[0], hit.normal[1], hit.normal[2]};
                            marker.color = hit.trigger
                                ? std::array<float, 4>{1.0f, 0.62f, 0.10f, 0.95f}
                                : std::array<float, 4>{1.0f, 0.86f, 0.15f, 0.95f};
                            marker.ttlSeconds = 2.5f;
                            editorPhysicsDebugContacts.push_back(marker);

                            std::uint32_t meshEntityId = 0;
                            for (const auto& [candidateMeshId, bodyId] : editorPhysicsBodies)
                            {
                                if (bodyId == hit.bodyId)
                                {
                                    meshEntityId = candidateMeshId;
                                    break;
                                }
                            }
                            Tracenf("[PHYSICS-QUERY] raycast hit body=%llu mesh=%u layer=%s trigger=%u dist=%.3f pos=(%.3f,%.3f,%.3f) normal=(%.3f,%.3f,%.3f)",
                                static_cast<unsigned long long>(hit.bodyId),
                                meshEntityId,
                                phys::ToString(hit.layer),
                                hit.trigger ? 1u : 0u,
                                hit.distance,
                                hit.position[0],
                                hit.position[1],
                                hit.position[2],
                                hit.normal[0],
                                hit.normal[1],
                                hit.normal[2]);
                            runtimeSession->SetEditorStatus("Physics raycast hit");
                        }
                        else
                        {
                            Tracenf("[PHYSICS-QUERY] raycast miss distance=%.3f layerMask=0x%08X hitTriggers=%u",
                                commands.physicsRaycastDistance,
                                commands.physicsRaycastLayerMask,
                                commands.physicsRaycastHitTriggers ? 1u : 0u);
                            runtimeSession->SetEditorStatus("Physics raycast miss");
                        }
                    }
                }
                if (commands.physicsOverlapSphereFromCamera)
                {
                    if (editorPlay.state.mode != EditorPlayMode::Play || !editorPhysicsWorldActive)
                    {
                        runtimeSession->SetEditorStatus("Physics overlap needs Play mode");
                        Tracen("[PHYSICS-QUERY] overlap sphere skipped reason=world-inactive");
                    }
                    else
                    {
                        const WorldVec3 origin = frameCamera.eye;
                        const WorldVec3 direction = WorldCameraForward(frameCamera);
                        const WorldVec3 center = origin + direction * commands.physicsOverlapDistance;
                        phys::PhysicsQueryFilter queryFilter{};
                        queryFilter.layerMask = commands.physicsOverlapLayerMask;
                        queryFilter.hitTriggers = commands.physicsOverlapHitTriggers;
                        const float centerRaw[3] = {center.x, center.y, center.z};
                        const std::vector<phys::PhysicsOverlapHit> hits = editorPhysicsWorld.OverlapSphere(
                            centerRaw,
                            commands.physicsOverlapRadius,
                            queryFilter,
                            64);

                        AppendPhysicsDebugSphere(
                            editorPhysicsDebugLines,
                            center,
                            commands.physicsOverlapRadius,
                            hits.empty()
                                ? std::array<float, 4>{0.55f, 0.62f, 0.75f, 0.65f}
                                : std::array<float, 4>{0.35f, 0.90f, 1.0f, 0.95f});
                        for (const phys::PhysicsOverlapHit& hit : hits)
                        {
                            PhysicsDebugContact marker{};
                            marker.point = {hit.position[0], hit.position[1], hit.position[2]};
                            marker.normal = {hit.normal[0], hit.normal[1], hit.normal[2]};
                            marker.color = hit.trigger
                                ? std::array<float, 4>{1.0f, 0.62f, 0.10f, 0.95f}
                                : std::array<float, 4>{0.35f, 0.90f, 1.0f, 0.95f};
                            marker.ttlSeconds = 2.5f;
                            editorPhysicsDebugContacts.push_back(marker);
                        }

                        std::ostringstream bodyList;
                        std::uint32_t printed = 0;
                        for (const phys::PhysicsOverlapHit& hit : hits)
                        {
                            if (printed >= 8)
                                break;
                            std::uint32_t meshEntityId = 0;
                            for (const auto& [candidateMeshId, bodyId] : editorPhysicsBodies)
                            {
                                if (bodyId == hit.bodyId)
                                {
                                    meshEntityId = candidateMeshId;
                                    break;
                                }
                            }
                            if (printed > 0)
                                bodyList << ",";
                            bodyList << "{body=" << static_cast<unsigned long long>(hit.bodyId)
                                     << " mesh=" << meshEntityId
                                     << " layer=" << phys::ToString(hit.layer)
                                     << " trigger=" << (hit.trigger ? 1 : 0)
                                     << "}";
                            ++printed;
                        }
                        if (hits.size() > printed)
                            bodyList << ",...";

                        Tracenf("[PHYSICS-QUERY] overlap sphere center=(%.3f,%.3f,%.3f) radius=%.3f hits=%zu layerMask=0x%08X hitTriggers=%u bodies=[%s]",
                            center.x,
                            center.y,
                            center.z,
                            commands.physicsOverlapRadius,
                            hits.size(),
                            commands.physicsOverlapLayerMask,
                            commands.physicsOverlapHitTriggers ? 1u : 0u,
                            bodyList.str().c_str());
                        runtimeSession->SetEditorStatus(hits.empty()
                            ? "Physics overlap found 0 bodies"
                            : "Physics overlap found bodies");
                    }
                }
                if (commands.physicsOverlapBoxFromCamera)
                {
                    if (editorPlay.state.mode != EditorPlayMode::Play || !editorPhysicsWorldActive)
                    {
                        runtimeSession->SetEditorStatus("Physics overlap needs Play mode");
                        Tracen("[PHYSICS-QUERY] overlap box skipped reason=world-inactive");
                    }
                    else
                    {
                        const WorldVec3 origin = frameCamera.eye;
                        const WorldVec3 direction = WorldCameraForward(frameCamera);
                        const WorldVec3 center = origin + direction * commands.physicsOverlapDistance;
                        phys::PhysicsQueryFilter queryFilter{};
                        queryFilter.layerMask = commands.physicsOverlapLayerMask;
                        queryFilter.hitTriggers = commands.physicsOverlapHitTriggers;
                        const float centerRaw[3] = {center.x, center.y, center.z};
                        const float halfExtents[3] = {
                            std::max(0.05f, commands.physicsOverlapBoxHalfExtents[0]),
                            std::max(0.05f, commands.physicsOverlapBoxHalfExtents[1]),
                            std::max(0.05f, commands.physicsOverlapBoxHalfExtents[2]),
                        };
                        const std::vector<phys::PhysicsOverlapHit> hits = editorPhysicsWorld.OverlapBox(
                            centerRaw,
                            halfExtents,
                            queryFilter,
                            64);

                        AppendPhysicsDebugBox(
                            editorPhysicsDebugLines,
                            center,
                            {halfExtents[0], halfExtents[1], halfExtents[2]},
                            hits.empty()
                                ? std::array<float, 4>{0.55f, 0.62f, 0.75f, 0.65f}
                                : std::array<float, 4>{0.35f, 0.90f, 1.0f, 0.95f});
                        for (const phys::PhysicsOverlapHit& hit : hits)
                        {
                            PhysicsDebugContact marker{};
                            marker.point = {hit.position[0], hit.position[1], hit.position[2]};
                            marker.normal = {hit.normal[0], hit.normal[1], hit.normal[2]};
                            marker.color = hit.trigger
                                ? std::array<float, 4>{1.0f, 0.62f, 0.10f, 0.95f}
                                : std::array<float, 4>{0.35f, 0.90f, 1.0f, 0.95f};
                            marker.ttlSeconds = 2.5f;
                            editorPhysicsDebugContacts.push_back(marker);
                        }

                        std::ostringstream bodyList;
                        std::uint32_t printed = 0;
                        for (const phys::PhysicsOverlapHit& hit : hits)
                        {
                            if (printed >= 8)
                                break;
                            std::uint32_t meshEntityId = 0;
                            for (const auto& [candidateMeshId, bodyId] : editorPhysicsBodies)
                            {
                                if (bodyId == hit.bodyId)
                                {
                                    meshEntityId = candidateMeshId;
                                    break;
                                }
                            }
                            if (printed > 0)
                                bodyList << ",";
                            bodyList << "{body=" << static_cast<unsigned long long>(hit.bodyId)
                                     << " mesh=" << meshEntityId
                                     << " layer=" << phys::ToString(hit.layer)
                                     << " trigger=" << (hit.trigger ? 1 : 0)
                                     << "}";
                            ++printed;
                        }
                        if (hits.size() > printed)
                            bodyList << ",...";

                        Tracenf("[PHYSICS-QUERY] overlap box center=(%.3f,%.3f,%.3f) half=(%.3f,%.3f,%.3f) hits=%zu layerMask=0x%08X hitTriggers=%u bodies=[%s]",
                            center.x,
                            center.y,
                            center.z,
                            halfExtents[0],
                            halfExtents[1],
                            halfExtents[2],
                            hits.size(),
                            commands.physicsOverlapLayerMask,
                            commands.physicsOverlapHitTriggers ? 1u : 0u,
                            bodyList.str().c_str());
                        runtimeSession->SetEditorStatus(hits.empty()
                            ? "Physics overlap found 0 bodies"
                            : "Physics overlap found bodies");
                    }
                }
                if (commands.physicsOverlapCapsuleFromCamera)
                {
                    if (editorPlay.state.mode != EditorPlayMode::Play || !editorPhysicsWorldActive)
                    {
                        runtimeSession->SetEditorStatus("Physics overlap needs Play mode");
                        Tracen("[PHYSICS-QUERY] overlap capsule skipped reason=world-inactive");
                    }
                    else
                    {
                        const WorldVec3 origin = frameCamera.eye;
                        const WorldVec3 direction = WorldCameraForward(frameCamera);
                        const WorldVec3 center = origin + direction * commands.physicsOverlapDistance;
                        phys::PhysicsQueryFilter queryFilter{};
                        queryFilter.layerMask = commands.physicsOverlapLayerMask;
                        queryFilter.hitTriggers = commands.physicsOverlapHitTriggers;
                        const float centerRaw[3] = {center.x, center.y, center.z};
                        const float radius = std::max(0.05f, commands.physicsOverlapCapsuleRadius);
                        const float totalHeight = std::max(radius * 2.0f, commands.physicsOverlapCapsuleHeight);
                        const float cylinderHalfHeight = std::max(0.0f, (totalHeight - radius * 2.0f) * 0.5f);
                        const std::vector<phys::PhysicsOverlapHit> hits = editorPhysicsWorld.OverlapCapsule(
                            centerRaw,
                            cylinderHalfHeight,
                            radius,
                            queryFilter,
                            64);

                        AppendPhysicsDebugCapsule(
                            editorPhysicsDebugLines,
                            center,
                            cylinderHalfHeight,
                            radius,
                            hits.empty()
                                ? std::array<float, 4>{0.55f, 0.62f, 0.75f, 0.65f}
                                : std::array<float, 4>{0.35f, 0.90f, 1.0f, 0.95f});
                        for (const phys::PhysicsOverlapHit& hit : hits)
                        {
                            PhysicsDebugContact marker{};
                            marker.point = {hit.position[0], hit.position[1], hit.position[2]};
                            marker.normal = {hit.normal[0], hit.normal[1], hit.normal[2]};
                            marker.color = hit.trigger
                                ? std::array<float, 4>{1.0f, 0.62f, 0.10f, 0.95f}
                                : std::array<float, 4>{0.35f, 0.90f, 1.0f, 0.95f};
                            marker.ttlSeconds = 2.5f;
                            editorPhysicsDebugContacts.push_back(marker);
                        }

                        std::ostringstream bodyList;
                        std::uint32_t printed = 0;
                        for (const phys::PhysicsOverlapHit& hit : hits)
                        {
                            if (printed >= 8)
                                break;
                            std::uint32_t meshEntityId = 0;
                            for (const auto& [candidateMeshId, bodyId] : editorPhysicsBodies)
                            {
                                if (bodyId == hit.bodyId)
                                {
                                    meshEntityId = candidateMeshId;
                                    break;
                                }
                            }
                            if (printed > 0)
                                bodyList << ",";
                            bodyList << "{body=" << static_cast<unsigned long long>(hit.bodyId)
                                     << " mesh=" << meshEntityId
                                     << " layer=" << phys::ToString(hit.layer)
                                     << " trigger=" << (hit.trigger ? 1 : 0)
                                     << "}";
                            ++printed;
                        }
                        if (hits.size() > printed)
                            bodyList << ",...";

                        Tracenf("[PHYSICS-QUERY] overlap capsule center=(%.3f,%.3f,%.3f) radius=%.3f height=%.3f hits=%zu layerMask=0x%08X hitTriggers=%u bodies=[%s]",
                            center.x,
                            center.y,
                            center.z,
                            radius,
                            totalHeight,
                            hits.size(),
                            commands.physicsOverlapLayerMask,
                            commands.physicsOverlapHitTriggers ? 1u : 0u,
                            bodyList.str().c_str());
                        runtimeSession->SetEditorStatus(hits.empty()
                            ? "Physics overlap found 0 bodies"
                            : "Physics overlap found bodies");
                    }
                }
                auto runPhysicsBodyCommand = [&](const char* actionName, const float* vector, auto&& apply) {
                    if (editorPlay.state.mode != EditorPlayMode::Play || !editorPhysicsWorldActive)
                    {
                        runtimeSession->SetEditorStatus("Physics control needs Play mode");
                        Tracenf("[PHYSICS-CONTROL] %s skipped reason=world-inactive entity=%u",
                            actionName,
                            commands.physicsRuntimeEntityId);
                        return;
                    }
                    const auto bodyIt = editorPhysicsBodies.find(commands.physicsRuntimeEntityId);
                    if (bodyIt == editorPhysicsBodies.end())
                    {
                        runtimeSession->SetEditorStatus("Selected entity has no physics body");
                        Tracenf("[PHYSICS-CONTROL] %s skipped reason=body-not-found entity=%u",
                            actionName,
                            commands.physicsRuntimeEntityId);
                        return;
                    }
                    const bool applied = apply(bodyIt->second, vector);
                    Tracenf("[PHYSICS-CONTROL] %s entity=%u body=%llu vector=(%.3f,%.3f,%.3f) applied=%u",
                        actionName,
                        commands.physicsRuntimeEntityId,
                        static_cast<unsigned long long>(bodyIt->second),
                        vector[0],
                        vector[1],
                        vector[2],
                        applied ? 1u : 0u);
                    runtimeSession->SetEditorStatus(applied ? "Physics control applied" : "Physics control ignored");
                };
                if (commands.physicsSetLinearVelocityForSelected)
                {
                    runPhysicsBodyCommand("set_velocity", commands.physicsLinearVelocity,
                        [&](phys::BodyId bodyId, const float* value) {
                            return editorPhysicsWorld.SetLinearVelocity(bodyId, value);
                        });
                }
                if (commands.physicsApplyForceToSelected)
                {
                    runPhysicsBodyCommand("add_force", commands.physicsForce,
                        [&](phys::BodyId bodyId, const float* value) {
                            return editorPhysicsWorld.AddForce(bodyId, value);
                        });
                }
                if (commands.physicsApplyImpulseToSelected)
                {
                    runPhysicsBodyCommand("add_impulse", commands.physicsImpulse,
                        [&](phys::BodyId bodyId, const float* value) {
                            return editorPhysicsWorld.AddImpulse(bodyId, value);
                        });
                }
                if (commands.physicsApplyAngularImpulseToSelected)
                {
                    runPhysicsBodyCommand("add_angular_impulse", commands.physicsAngularImpulse,
                        [&](phys::BodyId bodyId, const float* value) {
                            return editorPhysicsWorld.AddAngularImpulse(bodyId, value);
                        });
                }
                if (commands.renderResolutionChanged)
                {
                    renderResolutionUseNative = commands.renderResolutionUseNative;
                    if (renderResolutionUseNative)
                    {
                        requestedRenderResolution = {};
                    }
                    else
                    {
                        requestedRenderResolution.width = std::clamp(commands.renderResolutionWidth, 320u, 7680u);
                        requestedRenderResolution.height = std::clamp(commands.renderResolutionHeight, 180u, 4320u);
                    }
                    recreateOffscreenScene("editor-setting");
                }
                if (commands.gizmoSettingsChanged)
                {
                    editorGizmoMode = commands.gizmoOperation;
                    editorGizmoSnapEnabled = commands.gizmoSnapEnabled;
                    editorGizmoSnapValue = std::max(commands.gizmoSnapValue, 0.001f);
                    Tracenf("[EDITOR-GIZMO] Settings applied: operation=%d snap=%d value=%.2f",
                        static_cast<int>(editorGizmoMode),
                        editorGizmoSnapEnabled ? 1 : 0,
                        editorGizmoSnapValue);
                }
                if (commands.sceneGizmoTransformChanged)
                {
                    bool transformApplied = false;
                    if (commands.sceneGizmoEntityType == HierarchyEntityType::MeshEntity)
                    {
                        auto it = std::find_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                            [&](const MeshSceneEntity& mesh) { return mesh.id == commands.sceneGizmoEntityId; });
                        if (it != editorMeshEntities.end())
                        {
                            if (commands.sceneGizmoTarget == SceneGizmoTargetKind::ColliderCenter)
                            {
                                transformApplied = ApplySceneGizmoToCollider(
                                    *it,
                                    commands.sceneGizmoPosition,
                                    commands.sceneGizmoScale,
                                    commands.sceneGizmoOperation);
                                if (transformApplied)
                                    Tracenf("[PHYSICS] collider gizmo entity=%u op=%d shape=%s center=(%.3f,%.3f,%.3f) size=(%.3f,%.3f,%.3f) radius=%.3f height=%.3f",
                                        it->id,
                                        static_cast<int>(commands.sceneGizmoOperation),
                                        phys::ToString(it->collider.shape),
                                        it->collider.center[0],
                                        it->collider.center[1],
                                        it->collider.center[2],
                                        it->collider.size[0],
                                        it->collider.size[1],
                                        it->collider.size[2],
                                        it->collider.radius,
                                        it->collider.height);
                            }
                            else
                            {
                                StaticMeshRenderer* renderer = it->skinned
                                    ? nullptr
                                    : getStaticMeshRenderer(resolveMeshRuntimePath(*it));
                                transformApplied = ApplySceneGizmoToMesh(
                                    *it,
                                    commands.sceneGizmoPosition,
                                    commands.sceneGizmoRotation,
                                    commands.sceneGizmoScale,
                                    renderer);
                                if (transformApplied)
                                    syncStaticMeshSpatialEntity(*it);
                            }
                        }
                    }
                    else if (commands.sceneGizmoEntityType == HierarchyEntityType::PointLight)
                    {
                        auto it = std::find_if(editorPointLights.begin(), editorPointLights.end(),
                            [&](const PointLight& light) { return light.id == commands.sceneGizmoEntityId; });
                        if (it != editorPointLights.end())
                        {
                            transformApplied = ApplySceneGizmoToPointLight(
                                *it,
                                commands.sceneGizmoPosition,
                                commands.sceneGizmoScale);
                        }
                    }
                    else if (commands.sceneGizmoEntityType == HierarchyEntityType::SpotLight)
                    {
                        auto it = std::find_if(editorSpotLights.begin(), editorSpotLights.end(),
                            [&](const SpotLight& light) { return light.id == commands.sceneGizmoEntityId; });
                        if (it != editorSpotLights.end())
                        {
                            transformApplied = ApplySceneGizmoToSpotLight(
                                *it,
                                commands.sceneGizmoPosition,
                                commands.sceneGizmoRotation,
                                commands.sceneGizmoScale);
                        }
                    }
                    else if (commands.sceneGizmoEntityType == HierarchyEntityType::WaterBody)
                    {
                        auto it = std::find_if(editorWaterBodies.begin(), editorWaterBodies.end(),
                            [&](const WaterBody& body) { return body.id == commands.sceneGizmoEntityId; });
                        if (it != editorWaterBodies.end())
                        {
                            transformApplied = ApplySceneGizmoToWaterBody(
                                *it,
                                commands.sceneGizmoPosition,
                                commands.sceneGizmoScale);
                            editorWaterBodiesDirty = true;
                        }
                    }
                    else if (commands.sceneGizmoEntityType == HierarchyEntityType::Camera)
                    {
                        auto it = std::find_if(editorCameras.begin(), editorCameras.end(),
                            [&](const CameraEntity& camera) { return camera.id == commands.sceneGizmoEntityId; });
                        if (it != editorCameras.end())
                        {
                            transformApplied = ApplySceneGizmoToCamera(
                                *it,
                                commands.sceneGizmoPosition,
                                commands.sceneGizmoRotation);
                        }
                    }
                    if (transformApplied)
                        SceneManager::Instance().MarkDirty();
                }
                auto spawnAtCameraCenter = [&]() {
                    std::optional<WorldVec3> hit = RaycastTerrainPoint(terrain,
                        frameCamera,
                        renderSize.width,
                        renderSize.height,
                        static_cast<int>(renderSize.width / 2u),
                        static_cast<int>(renderSize.height / 2u));
                    if (hit)
                        return *hit;

                    WorldVec3 fallback = frameCamera.eye + CameraForward(frameCamera) * 30.0f;
                    fallback.y = terrain.SampleHeight(fallback);
                    return fallback;
                };
                auto spawnAtScreenPosition = [&](float screenX, float screenY) {
                    const int maxX = renderSize.width > 0 ? static_cast<int>(renderSize.width - 1u) : 0;
                    const int maxY = renderSize.height > 0 ? static_cast<int>(renderSize.height - 1u) : 0;
                    const int mouseX = std::clamp(static_cast<int>(xm::Round(screenX)), 0, maxX);
                    const int mouseY = std::clamp(static_cast<int>(xm::Round(screenY)), 0, maxY);
                    std::optional<WorldVec3> hit = RaycastTerrainPoint(terrain,
                        frameCamera,
                        renderSize.width,
                        renderSize.height,
                        mouseX,
                        mouseY);
                    if (hit)
                        return *hit;

                    WorldVec3 fallback = frameCamera.eye +
                        ScreenRayDirection(frameCamera, renderSize.width, renderSize.height, mouseX, mouseY) * 30.0f;
                    fallback.y = terrain.SampleHeight(fallback);
                    return fallback;
                };
                auto selectHierarchyEntity = [&](HierarchyEntityType type, std::uint32_t id, std::uint64_t flecsEntity = 0) {
                    switch (type)
                    {
                    case HierarchyEntityType::Terrain:
                        selectedEditorObject = {SelectedEditorObjectType::Terrain, id, flecsEntity};
                        runtimeSession->SetEditorStatus("Selected terrain");
                        break;
                    case HierarchyEntityType::WaterBody:
                        selectedEditorObject = {SelectedEditorObjectType::WaterBody, id, flecsEntity};
                        runtimeSession->SetEditorStatus("Selected water body #" + std::to_string(id));
                        break;
                    case HierarchyEntityType::PointLight:
                        selectedEditorObject = {SelectedEditorObjectType::PointLight, id, flecsEntity};
                        runtimeSession->SetEditorStatus("Selected point light #" + std::to_string(id));
                        break;
                    case HierarchyEntityType::SpotLight:
                        selectedEditorObject = {SelectedEditorObjectType::SpotLight, id, flecsEntity};
                        runtimeSession->SetEditorStatus("Selected spot light #" + std::to_string(id));
                        break;
                    case HierarchyEntityType::MeshEntity:
                        selectedEditorObject = {SelectedEditorObjectType::MeshEntity, id, flecsEntity};
                        runtimeSession->SetEditorStatus("Selected mesh entity #" + std::to_string(id));
                        break;
                    case HierarchyEntityType::Camera:
                        selectedEditorObject = {SelectedEditorObjectType::Camera, id, flecsEntity};
                        runtimeSession->SetEditorStatus("Selected camera #" + std::to_string(id));
                        break;
                    default:
                        break;
                    }
                    Tracenf("[HIERARCHY] Selected entity: flecs=%llu id=%u type=%d",
                        static_cast<unsigned long long>(flecsEntity), id, static_cast<int>(type));
                };
                auto focusHierarchyEntity = [&](HierarchyEntityType type, std::uint32_t id) {
                    std::optional<WorldVec3> target;
                    if (type == HierarchyEntityType::Terrain)
                    {
                        target = WorldVec3{0.0f, 0.0f, 0.0f};
                    }
                    else if (type == HierarchyEntityType::WaterBody)
                    {
                        auto it = std::find_if(editorWaterBodies.begin(), editorWaterBodies.end(),
                            [&](const WaterBody& body) { return body.id == id; });
                        if (it != editorWaterBodies.end())
                            target = WaterBodyCenter(*it);
                    }
                    else if (type == HierarchyEntityType::PointLight)
                    {
                        auto it = std::find_if(editorPointLights.begin(), editorPointLights.end(),
                            [&](const PointLight& light) { return light.id == id; });
                        if (it != editorPointLights.end())
                            target = WorldVec3{it->position[0], it->position[1], it->position[2]};
                    }
                    else if (type == HierarchyEntityType::SpotLight)
                    {
                        auto it = std::find_if(editorSpotLights.begin(), editorSpotLights.end(),
                            [&](const SpotLight& light) { return light.id == id; });
                        if (it != editorSpotLights.end())
                            target = WorldVec3{it->position[0], it->position[1], it->position[2]};
                    }
                    else if (type == HierarchyEntityType::MeshEntity)
                    {
                        auto it = std::find_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                            [&](const MeshSceneEntity& mesh) { return mesh.id == id; });
                        if (it != editorMeshEntities.end())
                            target = WorldVec3{it->position[0], it->position[1], it->position[2]};
                    }
                    else if (type == HierarchyEntityType::Camera)
                    {
                        auto it = std::find_if(editorCameras.begin(), editorCameras.end(),
                            [&](const CameraEntity& camera) { return camera.id == id; });
                        if (it != editorCameras.end())
                            target = WorldVec3{it->position[0], it->position[1], it->position[2]};
                    }
                    if (!target)
                        return;
                    cameraController.FocusOn(*target);
                    runtimeSession->SetEditorStatus("Focused camera on entity #" + std::to_string(id));
                    Tracenf("[HIERARCHY] Focused camera on entity: id=%u type=%d", id, static_cast<int>(type));
                };
                auto isHierarchyReparentableType = [](HierarchyEntityType type) {
                    return type == HierarchyEntityType::MeshEntity ||
                        type == HierarchyEntityType::PointLight ||
                        type == HierarchyEntityType::SpotLight;
                };
                auto hierarchyObjectExists = [&](HierarchyEntityType type, std::uint32_t id) {
                    if (type == HierarchyEntityType::MeshEntity)
                        return std::any_of(editorMeshEntities.begin(), editorMeshEntities.end(),
                            [&](const MeshSceneEntity& mesh) { return mesh.id == id; });
                    if (type == HierarchyEntityType::PointLight)
                        return std::any_of(editorPointLights.begin(), editorPointLights.end(),
                            [&](const PointLight& light) { return light.id == id; });
                    if (type == HierarchyEntityType::SpotLight)
                        return std::any_of(editorSpotLights.begin(), editorSpotLights.end(),
                            [&](const SpotLight& light) { return light.id == id; });
                    return false;
                };
                auto hierarchyParentOf = [&](HierarchyEntityType type, std::uint32_t id) {
                    if (type == HierarchyEntityType::MeshEntity)
                    {
                        auto it = std::find_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                            [&](const MeshSceneEntity& mesh) { return mesh.id == id; });
                        return it == editorMeshEntities.end() ? SceneParentRef{} : it->parent;
                    }
                    if (type == HierarchyEntityType::PointLight)
                    {
                        auto it = std::find_if(editorPointLights.begin(), editorPointLights.end(),
                            [&](const PointLight& light) { return light.id == id; });
                        return it == editorPointLights.end() ? SceneParentRef{} : it->parent;
                    }
                    if (type == HierarchyEntityType::SpotLight)
                    {
                        auto it = std::find_if(editorSpotLights.begin(), editorSpotLights.end(),
                            [&](const SpotLight& light) { return light.id == id; });
                        return it == editorSpotLights.end() ? SceneParentRef{} : it->parent;
                    }
                    return SceneParentRef{};
                };
                auto isHierarchyDescendantOf = [&](HierarchyEntityType type,
                                                   std::uint32_t id,
                                                   HierarchyEntityType ancestorType,
                                                   std::uint32_t ancestorId) {
                    for (SceneParentRef parent = hierarchyParentOf(type, id); parent.IsValid();)
                    {
                        const HierarchyEntityType parentType = SceneParentTypeFromName(parent.type);
                        if (parentType == HierarchyEntityType::None)
                            return false;
                        if (parentType == ancestorType && parent.id == ancestorId)
                            return true;
                        parent = hierarchyParentOf(parentType, parent.id);
                    }
                    return false;
                };
                auto setHierarchyParent = [&](HierarchyEntityType type, std::uint32_t id, const SceneParentRef& parent) {
                    if (type == HierarchyEntityType::MeshEntity)
                    {
                        auto it = std::find_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                            [&](const MeshSceneEntity& mesh) { return mesh.id == id; });
                        if (it == editorMeshEntities.end())
                            return false;
                        it->parent = parent;
                        return true;
                    }
                    if (type == HierarchyEntityType::PointLight)
                    {
                        auto it = std::find_if(editorPointLights.begin(), editorPointLights.end(),
                            [&](const PointLight& light) { return light.id == id; });
                        if (it == editorPointLights.end())
                            return false;
                        it->parent = parent;
                        return true;
                    }
                    if (type == HierarchyEntityType::SpotLight)
                    {
                        auto it = std::find_if(editorSpotLights.begin(), editorSpotLights.end(),
                            [&](const SpotLight& light) { return light.id == id; });
                        if (it == editorSpotLights.end())
                            return false;
                        it->parent = parent;
                        return true;
                    }
                    return false;
                };
                auto reparentHierarchyEntity = [&](HierarchyEntityType type,
                                                   std::uint32_t id,
                                                   HierarchyEntityType parentType,
                                                   std::uint32_t parentId) {
                    if (!isHierarchyReparentableType(type) || !hierarchyObjectExists(type, id))
                    {
                        runtimeSession->SetEditorStatus("Cannot parent this hierarchy item");
                        return false;
                    }

                    SceneParentRef parent;
                    if (parentType != HierarchyEntityType::None && parentId != 0)
                    {
                        if (!isHierarchyReparentableType(parentType) || !hierarchyObjectExists(parentType, parentId))
                        {
                            runtimeSession->SetEditorStatus("Parent target is not a scene entity");
                            return false;
                        }
                        if (parentType == type && parentId == id)
                        {
                            runtimeSession->SetEditorStatus("Cannot parent an entity to itself");
                            return false;
                        }
                        if (isHierarchyDescendantOf(parentType, parentId, type, id))
                        {
                            runtimeSession->SetEditorStatus("Cannot create a hierarchy cycle");
                            return false;
                        }
                        parent = MakeSceneParentRef(parentType, parentId);
                    }

                    if (!setHierarchyParent(type, id, parent))
                        return false;
                    SceneManager::Instance().MarkDirty();
                    runtimeSession->SetEditorStatus(parent.IsValid()
                        ? "Hierarchy parent changed"
                        : "Hierarchy entity moved to scene root");
                    Tracenf("[HIERARCHY] Reparented entity: id=%u type=%d parent_id=%u parent_type=%d",
                        id,
                        static_cast<int>(type),
                        parentId,
                        static_cast<int>(parentType));
                    return true;
                };
                auto directHierarchyChildren = [&](HierarchyEntityType parentType, std::uint32_t parentId) {
                    std::vector<std::pair<HierarchyEntityType, std::uint32_t>> children;
                    const std::string parentTypeName = SceneParentTypeName(parentType);
                    if (parentTypeName.empty())
                        return children;
                    for (const MeshSceneEntity& mesh : editorMeshEntities)
                    {
                        if (mesh.parent.type == parentTypeName && mesh.parent.id == parentId)
                            children.push_back({HierarchyEntityType::MeshEntity, mesh.id});
                    }
                    for (const PointLight& light : editorPointLights)
                    {
                        if (light.parent.type == parentTypeName && light.parent.id == parentId)
                            children.push_back({HierarchyEntityType::PointLight, light.id});
                    }
                    for (const SpotLight& light : editorSpotLights)
                    {
                        if (light.parent.type == parentTypeName && light.parent.id == parentId)
                            children.push_back({HierarchyEntityType::SpotLight, light.id});
                    }
                    return children;
                };
                std::function<void(HierarchyEntityType, std::uint32_t)> deleteHierarchyEntity;
                deleteHierarchyEntity = [&](HierarchyEntityType type, std::uint32_t id) {
                    const std::vector<std::pair<HierarchyEntityType, std::uint32_t>> children = directHierarchyChildren(type, id);
                    for (const auto& child : children)
                        deleteHierarchyEntity(child.first, child.second);

                    if (type == HierarchyEntityType::WaterBody)
                    {
                        editorWaterBodies.erase(std::remove_if(editorWaterBodies.begin(), editorWaterBodies.end(),
                            [&](const WaterBody& body) { return body.id == id; }), editorWaterBodies.end());
                        editorWaterBodiesDirty = true;
                    }
                    else if (type == HierarchyEntityType::Terrain)
                    {
                        if (terrainOk)
                            terrain.ClearTerrain(device);
                    }
                    else if (type == HierarchyEntityType::PointLight)
                    {
                        editorPointLights.erase(std::remove_if(editorPointLights.begin(), editorPointLights.end(),
                            [&](const PointLight& light) { return light.id == id; }), editorPointLights.end());
                    }
                    else if (type == HierarchyEntityType::SpotLight)
                    {
                        editorSpotLights.erase(std::remove_if(editorSpotLights.begin(), editorSpotLights.end(),
                            [&](const SpotLight& light) { return light.id == id; }), editorSpotLights.end());
                    }
                    else if (type == HierarchyEntityType::MeshEntity)
                    {
                        removeStaticMeshSpatialEntity(id);
                        editorMeshEntities.erase(std::remove_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                            [&](const MeshSceneEntity& mesh) { return mesh.id == id; }), editorMeshEntities.end());
                        rebuildMeshEntityLookup();
                    }
                    if ((type == HierarchyEntityType::Terrain && selectedEditorObject.type == SelectedEditorObjectType::Terrain) ||
                        (type == HierarchyEntityType::WaterBody && selectedEditorObject.type == SelectedEditorObjectType::WaterBody && selectedEditorObject.id == id) ||
                        (type == HierarchyEntityType::PointLight && selectedEditorObject.type == SelectedEditorObjectType::PointLight && selectedEditorObject.id == id) ||
                        (type == HierarchyEntityType::SpotLight && selectedEditorObject.type == SelectedEditorObjectType::SpotLight && selectedEditorObject.id == id) ||
                        (type == HierarchyEntityType::MeshEntity && selectedEditorObject.type == SelectedEditorObjectType::MeshEntity && selectedEditorObject.id == id))
                    {
                        selectedEditorObject = {};
                    }
                    SceneManager::Instance().MarkDirty();
                    runtimeSession->SetEditorStatus("Deleted hierarchy entity #" + std::to_string(id));
                    Tracenf("[HIERARCHY] Deleted entity: id=%u type=%d", id, static_cast<int>(type));
                };
                auto duplicateHierarchyEntity = [&](HierarchyEntityType type, std::uint32_t id) {
                    if (type == HierarchyEntityType::WaterBody)
                    {
                        auto it = std::find_if(editorWaterBodies.begin(), editorWaterBodies.end(),
                            [&](const WaterBody& body) { return body.id == id; });
                        if (it == editorWaterBodies.end())
                            return;
                        WaterBody copy = *it;
                        copy.id = nextEditorWaterBodyId++;
                        copy.name = makeUniqueSceneEntityName((copy.name.empty() ? "Water Body" : copy.name) + " Copy");
                        copy.bboxMin[0] += 5.0f;
                        copy.bboxMax[0] += 5.0f;
                        copy.editorHidden = false;
                        editorWaterBodies.push_back(copy);
                        selectedEditorObject = {SelectedEditorObjectType::WaterBody, copy.id};
                        editorWaterBodiesDirty = true;
                        SceneManager::Instance().MarkDirty();
                        Tracenf("[HIERARCHY] Duplicated entity: original=%u new=%u", id, copy.id);
                    }
                    else if (type == HierarchyEntityType::Terrain)
                    {
                        runtimeSession->SetEditorStatus("Only one terrain is supported per scene");
                        Tracen("[HIERARCHY] Duplicate ignored for Terrain: one terrain per scene");
                    }
                    else if (type == HierarchyEntityType::PointLight)
                    {
                        if (editorPointLights.size() >= kMaxDynamicPointLights)
                            return;
                        auto it = std::find_if(editorPointLights.begin(), editorPointLights.end(),
                            [&](const PointLight& light) { return light.id == id; });
                        if (it == editorPointLights.end())
                            return;
                        PointLight copy = *it;
                        copy.id = nextEditorLightId++;
                        copy.name = makeUniqueSceneEntityName((copy.name.empty() ? "Point Light" : copy.name) + " Copy");
                        copy.position[0] += 5.0f;
                        copy.editorHidden = false;
                        editorPointLights.push_back(copy);
                        selectedEditorObject = {SelectedEditorObjectType::PointLight, copy.id};
                        SceneManager::Instance().MarkDirty();
                        Tracenf("[HIERARCHY] Duplicated entity: original=%u new=%u", id, copy.id);
                    }
                    else if (type == HierarchyEntityType::SpotLight)
                    {
                        if (editorSpotLights.size() >= kMaxDynamicSpotLights)
                            return;
                        auto it = std::find_if(editorSpotLights.begin(), editorSpotLights.end(),
                            [&](const SpotLight& light) { return light.id == id; });
                        if (it == editorSpotLights.end())
                            return;
                        SpotLight copy = *it;
                        copy.id = nextEditorLightId++;
                        copy.name = makeUniqueSceneEntityName((copy.name.empty() ? "Spot Light" : copy.name) + " Copy");
                        copy.position[0] += 5.0f;
                        copy.editorHidden = false;
                        editorSpotLights.push_back(copy);
                        selectedEditorObject = {SelectedEditorObjectType::SpotLight, copy.id};
                        SceneManager::Instance().MarkDirty();
                        Tracenf("[HIERARCHY] Duplicated entity: original=%u new=%u", id, copy.id);
                    }
                    else if (type == HierarchyEntityType::MeshEntity)
                    {
                        auto it = std::find_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                            [&](const MeshSceneEntity& mesh) { return mesh.id == id; });
                        if (it == editorMeshEntities.end())
                            return;
                        MeshSceneEntity copy = *it;
                        copy.id = nextEditorMeshEntityId++;
                        copy.name = makeUniqueSceneEntityName((copy.name.empty() ? "Mesh Entity" : copy.name) + " Copy");
                        copy.position[0] += 5.0f;
                        copy.editorHidden = false;
                        editorMeshEntities.push_back(copy);
                        editorMeshEntityLookup[copy.id] = editorMeshEntities.size() - 1u;
                        syncStaticMeshSpatialEntity(editorMeshEntities.back());
                        selectedEditorObject = {SelectedEditorObjectType::MeshEntity, copy.id};
                        SceneManager::Instance().MarkDirty();
                        Tracenf("[HIERARCHY] Duplicated entity: original=%u new=%u", id, copy.id);
                    }
                    runtimeSession->SetEditorStatus("Duplicated hierarchy entity #" + std::to_string(id));
                };
                auto renameHierarchyEntity = [&](HierarchyEntityType type, std::uint32_t id, const std::string& name) {
                    if (name.empty())
                        return;
                    if (type == HierarchyEntityType::WaterBody)
                    {
                        auto it = std::find_if(editorWaterBodies.begin(), editorWaterBodies.end(),
                            [&](const WaterBody& body) { return body.id == id; });
                        if (it != editorWaterBodies.end())
                            it->name = name;
                    }
                    else if (type == HierarchyEntityType::Terrain)
                    {
                        if (terrainOk && terrain.HasTerrain())
                        {
                            TerrainSceneData data = terrain.GetTerrainSceneData();
                            data.name = name;
                            terrain.SetTerrainSceneData(data);
                        }
                    }
                    else if (type == HierarchyEntityType::PointLight)
                    {
                        auto it = std::find_if(editorPointLights.begin(), editorPointLights.end(),
                            [&](const PointLight& light) { return light.id == id; });
                        if (it != editorPointLights.end())
                            it->name = name;
                    }
                    else if (type == HierarchyEntityType::SpotLight)
                    {
                        auto it = std::find_if(editorSpotLights.begin(), editorSpotLights.end(),
                            [&](const SpotLight& light) { return light.id == id; });
                        if (it != editorSpotLights.end())
                            it->name = name;
                    }
                    else if (type == HierarchyEntityType::MeshEntity)
                    {
                        auto it = std::find_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                            [&](const MeshSceneEntity& mesh) { return mesh.id == id; });
                        if (it != editorMeshEntities.end())
                            it->name = name;
                    }
                    SceneManager::Instance().MarkDirty();
                    runtimeSession->SetEditorStatus("Renamed hierarchy entity #" + std::to_string(id));
                    Tracenf("[HIERARCHY] Renamed entity: id=%u new_name=%s", id, name.c_str());
                };
                auto toggleHierarchyHidden = [&](HierarchyEntityType type, std::uint32_t id) {
                    bool hidden = false;
                    if (type == HierarchyEntityType::WaterBody)
                    {
                        auto it = std::find_if(editorWaterBodies.begin(), editorWaterBodies.end(),
                            [&](const WaterBody& body) { return body.id == id; });
                        if (it != editorWaterBodies.end())
                        {
                            it->editorHidden = !it->editorHidden;
                            hidden = it->editorHidden;
                            editorWaterBodiesDirty = true;
                        }
                    }
                    else if (type == HierarchyEntityType::Terrain)
                    {
                        if (terrainOk && terrain.HasTerrain())
                        {
                            TerrainSceneData data = terrain.GetTerrainSceneData();
                            data.editorHidden = !data.editorHidden;
                            hidden = data.editorHidden;
                            terrain.SetTerrainSceneData(data);
                        }
                    }
                    else if (type == HierarchyEntityType::PointLight)
                    {
                        auto it = std::find_if(editorPointLights.begin(), editorPointLights.end(),
                            [&](const PointLight& light) { return light.id == id; });
                        if (it != editorPointLights.end())
                        {
                            it->editorHidden = !it->editorHidden;
                            hidden = it->editorHidden;
                        }
                    }
                    else if (type == HierarchyEntityType::SpotLight)
                    {
                        auto it = std::find_if(editorSpotLights.begin(), editorSpotLights.end(),
                            [&](const SpotLight& light) { return light.id == id; });
                        if (it != editorSpotLights.end())
                        {
                            it->editorHidden = !it->editorHidden;
                            hidden = it->editorHidden;
                        }
                    }
                    else if (type == HierarchyEntityType::MeshEntity)
                    {
                        auto it = std::find_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                            [&](const MeshSceneEntity& mesh) { return mesh.id == id; });
                        if (it != editorMeshEntities.end())
                        {
                            it->editorHidden = !it->editorHidden;
                            hidden = it->editorHidden;
                        }
                    }
                    SceneManager::Instance().MarkDirty();
                    runtimeSession->SetEditorStatus(std::string(hidden ? "Hidden" : "Shown") + " hierarchy entity #" + std::to_string(id));
                    Tracenf("[HIERARCHY] Toggled editor-visibility: id=%u hidden=%d", id, hidden ? 1 : 0);
                };
                auto exportMeshEntityToFbx = [&](std::uint32_t id, const MapEditorCommands& exportCommand) {
                    auto it = std::find_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                        [&](const MeshSceneEntity& mesh) { return mesh.id == id; });
                    if (it == editorMeshEntities.end())
                    {
                        runtimeSession->SetEditorStatus("FBX export failed: mesh entity not found");
                        TraceError("[FBX-EXPORT] failed entity=%u error=mesh entity not found", id);
                        return;
                    }

                    const std::filesystem::path sourcePath(resolveMeshRuntimePath(*it));
                    AssimpExporter::ExportOptions options{};
                    options.outputPath = std::filesystem::path(exportCommand.exportFbxOutputPath);
                    options.embedTextures = exportCommand.exportFbxEmbedTextures;
                    options.exportMaterials = exportCommand.exportFbxMaterials;
                    options.exportAnimations = exportCommand.exportFbxAnimations;

                    std::string error;
                    AssimpExporter::SourceAsset source{};
                    source.path = sourcePath;
                    source.nodeName = it->name.empty() ? std::string("MeshEntity_") + std::to_string(id) : it->name;
                    source.transform[0] = it->scale[0];
                    source.transform[5] = it->scale[1];
                    source.transform[10] = it->scale[2];
                    source.transform[12] = it->position[0];
                    source.transform[13] = it->position[1];
                    source.transform[14] = it->position[2];
                    if (!AssimpExporter::ExportAssetsToFbx({source}, options, error))
                    {
                        runtimeSession->SetEditorStatus("FBX export failed: " + error);
                        return;
                    }
                    runtimeSession->SetEditorStatus("FBX exported: " + options.outputPath.filename().string());
                };
                if (commands.exportMeshEntityToFbx)
                    exportMeshEntityToFbx(commands.exportMeshEntityId, commands);
#if defined(IXTREEME_WITH_EDITOR)
                auto applyPendingSelectedMeshEntityChange = [&]() {
                    if (!commands.selectedMeshEntityChanged)
                        return false;
                    auto it = std::find_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                        [&](const MeshSceneEntity& mesh) { return mesh.id == commands.selectedMeshEntity.id; });
                    if (it == editorMeshEntities.end())
                    {
                        commands.selectedMeshEntityChanged = false;
                        commands.fitSelectedColliderToMesh = false;
                        return false;
                    }
                    auto floatsDiffer = [](const float* a, const float* b, std::size_t count, float epsilon = 0.0001f) {
                        for (std::size_t i = 0; i < count; ++i)
                        {
                            if (std::abs(a[i] - b[i]) > epsilon)
                                return true;
                        }
                        return false;
                    };
                    auto boolsDiffer = [](const bool* a, const bool* b, std::size_t count) {
                        for (std::size_t i = 0; i < count; ++i)
                        {
                            if (a[i] != b[i])
                                return true;
                        }
                        return false;
                    };
                    auto rigidbodyDiffers = [&](const phys::RigidbodyComponent& a, const phys::RigidbodyComponent& b) {
                        return a.enabled != b.enabled ||
                            a.bodyType != b.bodyType ||
                            std::abs(a.mass - b.mass) > 0.0001f ||
                            std::abs(a.linearDamping - b.linearDamping) > 0.0001f ||
                            std::abs(a.angularDamping - b.angularDamping) > 0.0001f ||
                            a.useGravity != b.useGravity ||
                            a.allowSleeping != b.allowSleeping ||
                            a.continuousCollision != b.continuousCollision ||
                            boolsDiffer(a.freezePosition, b.freezePosition, 3) ||
                            boolsDiffer(a.freezeRotation, b.freezeRotation, 3);
                    };
                    auto colliderDiffers = [&](const phys::ColliderComponent& a, const phys::ColliderComponent& b) {
                        return a.enabled != b.enabled ||
                            a.trigger != b.trigger ||
                            a.shape != b.shape ||
                            floatsDiffer(a.center, b.center, 3) ||
                            floatsDiffer(a.size, b.size, 3) ||
                            std::abs(a.radius - b.radius) > 0.0001f ||
                            std::abs(a.height - b.height) > 0.0001f ||
                            std::abs(a.friction - b.friction) > 0.0001f ||
                            std::abs(a.restitution - b.restitution) > 0.0001f ||
                            a.layer != b.layer ||
                            a.materialAssetId != b.materialAssetId;
                    };
                    auto fixedJointDiffers = [&](const phys::FixedJointComponent& a, const phys::FixedJointComponent& b) {
                        return a.enabled != b.enabled ||
                            a.connectedEntityId != b.connectedEntityId;
                    };
                    auto hingeJointDiffers = [&](const phys::HingeJointComponent& a, const phys::HingeJointComponent& b) {
                        return a.enabled != b.enabled ||
                            a.connectedEntityId != b.connectedEntityId ||
                            floatsDiffer(a.anchor, b.anchor, 3) ||
                            floatsDiffer(a.axis, b.axis, 3) ||
                            a.limitsEnabled != b.limitsEnabled ||
                            std::abs(a.minAngleDegrees - b.minAngleDegrees) > 0.0001f ||
                            std::abs(a.maxAngleDegrees - b.maxAngleDegrees) > 0.0001f ||
                            std::abs(a.frictionTorque - b.frictionTorque) > 0.0001f;
                    };
                    const bool transformChanged =
                        !std::equal(std::begin(it->position), std::end(it->position), std::begin(commands.selectedMeshEntity.position)) ||
                        !std::equal(std::begin(it->rotation), std::end(it->rotation), std::begin(commands.selectedMeshEntity.rotation)) ||
                        !std::equal(std::begin(it->scale), std::end(it->scale), std::begin(commands.selectedMeshEntity.scale));
                    const bool meshAssetChanged =
                        it->meshAssetId != commands.selectedMeshEntity.meshAssetId ||
                        it->meshAssetPath != commands.selectedMeshEntity.meshAssetPath ||
                        it->skinned != commands.selectedMeshEntity.skinned;
                    bool physicsChanged =
                        transformChanged ||
                        it->hasRigidbody != commands.selectedMeshEntity.hasRigidbody ||
                        it->hasCollider != commands.selectedMeshEntity.hasCollider ||
                        it->hasFixedJoint != commands.selectedMeshEntity.hasFixedJoint ||
                        it->hasHingeJoint != commands.selectedMeshEntity.hasHingeJoint ||
                        (it->hasRigidbody && commands.selectedMeshEntity.hasRigidbody &&
                            rigidbodyDiffers(it->rigidbody, commands.selectedMeshEntity.rigidbody)) ||
                        (it->hasCollider && commands.selectedMeshEntity.hasCollider &&
                            colliderDiffers(it->collider, commands.selectedMeshEntity.collider)) ||
                        (it->hasFixedJoint && commands.selectedMeshEntity.hasFixedJoint &&
                            fixedJointDiffers(it->fixedJoint, commands.selectedMeshEntity.fixedJoint)) ||
                        (it->hasHingeJoint && commands.selectedMeshEntity.hasHingeJoint &&
                            hingeJointDiffers(it->hingeJoint, commands.selectedMeshEntity.hingeJoint));
                    ApplyMeshRendererEditorState(*it, commands.selectedMeshEntity);
                    if (commands.fitSelectedColliderToMesh)
                    {
                        if (fitMeshColliderToBounds(*it))
                        {
                            physicsChanged = true;
                            runtimeSession->SetEditorStatus("Collider fitted to mesh");
                        }
                        commands.fitSelectedColliderToMesh = false;
                    }
                    if (transformChanged || meshAssetChanged)
                        syncStaticMeshSpatialEntity(*it);
                    if (physicsChanged && editorPlay.state.mode == EditorPlayMode::Play && editorPhysicsWorldActive)
                    {
                        rebuildEditorPhysicsWorld();
                        runtimeSession->SetEditorStatus("Physics body rebuilt");
                    }
                    selectedEditorObject = {SelectedEditorObjectType::MeshEntity, it->id};
                    SceneManager::Instance().MarkDirty();
                    commands.selectedMeshEntityChanged = false;
                    commands.fitSelectedColliderToMesh = false;
                    return true;
                };
                if (commands.enterPlayMode)
                {
                    applyPendingSelectedMeshEntityChange();
                    if (SceneManager::Instance().HasOpenScene())
                        editorPlay.state.mode = EditorPlayMode::Play;
                    else
                        Tracen("[EDIT-PLAY] Play ignored: no open scene");
                }
                if (commands.exitPlayMode)
                    editorPlay.state.mode = EditorPlayMode::Edit;
                if (commands.pausePlayMode && editorPlay.state.mode == EditorPlayMode::Play)
                    editorPlay.state.mode = EditorPlayMode::PlayPaused;
                if (commands.resumePlayMode && editorPlay.state.mode == EditorPlayMode::PlayPaused)
                    editorPlay.state.mode = EditorPlayMode::Play;

                if (editorPlay.appliedMode == EditorPlayMode::Edit &&
                    editorPlay.state.mode != EditorPlayMode::Edit)
                {
                    Tracen("[EDIT-PLAY] Entering Play Mode");
                    SceneManager::Instance().SetCurrentSceneSnapshot(sceneRuntime.BuildSceneSnapshot());
                    editorPlay.playStartSceneWasOpen = SceneManager::Instance().HasOpenScene();
                    editorPlay.playStartSceneDirty = SceneManager::Instance().IsDirty();
                    editorPlay.playStartSceneSnapshot = SceneManager::Instance().GetCurrentScene();
                    editorPlay.playStartScenePath = SceneManager::Instance().GetCurrentScenePath();
                    Tracenf("[EDIT-PLAY] Play Mode: starting scene = %s",
                        editorPlay.playStartScenePath.empty() ? "<unsaved>" : editorPlay.playStartScenePath.c_str());
                    editorPlay.editorCameraSnapshot = cameraController.SaveSnapshot();
                    selectedEditorObject = {};
                    editorObjectDragActive = false;
                    waterSculptStrokeActive = false;
                    editorWaterBodiesDirty = true;
                    terrain.SetWaterSculptBrush(false, 0.0f, 0.0f, 0.0f, true);
                    // With a player character present, Play hands WASD/Space to the character
                    // (the editor free-fly would otherwise capture those keys via editorFlyCameraKey).
                    // Without one, keep free-fly so you can fly around the running simulation.
                    const bool hasPlayerCharacter = std::any_of(
                        editorMeshEntities.begin(), editorMeshEntities.end(),
                        [](const MeshSceneEntity& m) { return m.hasCharacterController && m.characterController.enabled; });
                    cameraController.SetFreeCameraEnabled(!hasPlayerCharacter);
                    runtimeSession->Start(SceneManager::Instance().GetCurrentScene());
                    rebuildEditorPhysicsWorld();
                    editorCharacterStates.clear();
                    editorPlayerLookDx = 0.0f;
                    editorPlayerLookDy = 0.0f;
                    editorPlay.state.frameCount = 0;
                    editorPlay.state.elapsedSeconds = 0.0;
                    editorPlay.appliedMode = editorPlay.state.mode;
                    Tracen("[EDIT-PLAY] Default runtime Play mode enabled (no player UI, network, or character)");
                    Tracen("[EDIT-PLAY] Play Mode active");
                }
                else if (editorPlay.appliedMode != EditorPlayMode::Edit &&
                    editorPlay.state.mode == EditorPlayMode::Edit)
                {
                    Tracenf("[EDIT-PLAY] Exiting Play Mode (after %.1fs, %d frames)",
                        editorPlay.state.elapsedSeconds,
                        editorPlay.state.frameCount);
                    runtimeUi->HideAll();
                    runtimeSession->Stop();
                    clearEditorPhysicsWorld();
                    editorCharacterStates.clear();
                    editorWaterBodiesDirty = true;
                    if (editorPlay.playStartSceneWasOpen)
                    {
                        SceneManager::Instance().RestoreSceneSnapshot(
                            editorPlay.playStartSceneSnapshot,
                            editorPlay.playStartScenePath,
                            editorPlay.playStartSceneDirty);
                        Tracenf("[EDIT-PLAY] Restored starting scene: %s",
                            editorPlay.playStartScenePath.empty() ? "<unsaved>" : editorPlay.playStartScenePath.c_str());
                    }
                    editorPlay.playStartScenePath.clear();
                    editorPlay.playStartSceneWasOpen = false;
                    editorPlay.playStartSceneDirty = false;
                    if (editorPlay.editorCameraSnapshot)
                    {
                        cameraController.RestoreSnapshot(*editorPlay.editorCameraSnapshot);
                        editorPlay.editorCameraSnapshot.reset();
                    }
                    cameraController.SetFreeCameraEnabled(true); // restore editor free-fly after Play
                    movement.Clear();
                    editorPlay.state.frameCount = 0;
                    editorPlay.state.elapsedSeconds = 0.0;
                    editorPlay.appliedMode = EditorPlayMode::Edit;
                    Tracen("[EDIT-PLAY] Embedded server stopped");
                    Tracen("[EDIT-PLAY] Snapshot restored");
                    Tracen("[EDIT-PLAY] Edit Mode active");
                }
                else if (editorPlay.appliedMode != editorPlay.state.mode)
                {
                    editorPlay.appliedMode = editorPlay.state.mode;
                    Tracen(editorPlay.state.mode == EditorPlayMode::PlayPaused
                        ? "[EDIT-PLAY] Play paused"
                        : "[EDIT-PLAY] Play resumed");
                }
                if (editorPlay.state.mode != EditorPlayMode::Edit)
                {
                    if (editorPlay.state.mode == EditorPlayMode::Play)
                        runtimeSession->Tick(deltaSeconds);
                    commands.addWaterBody = false;
                    commands.createTerrain = false;
                    commands.addMeshEntity = false;
                    commands.addPrimitiveEntity = false;
                    commands.primitiveType.clear();
                    commands.addPrefabInstance = false;
                    commands.prefabAssetId.clear();
                    commands.prefabDropScreenPositionValid = false;
                    commands.createPrefabFromSelection = false;
                    commands.refreshSelectedPrefabInstance = false;
                    commands.refreshAllPrefabInstances = false;
                    commands.revertSelectedPrefabInstance = false;
                    commands.applySelectedPrefabToAsset = false;
                    commands.revertSelectedPrefabOverride = false;
                    commands.applySelectedPrefabOverrideToAsset = false;
                    commands.selectedPrefabOverrideName.clear();
                    commands.unpackSelectedPrefabInstance = false;
                    commands.savePrefabAssetEdit = false;
                    commands.editPrefabAssetId.clear();
                    commands.editPrefabName.clear();
                    commands.editPrefabEntityNames.clear();
                    commands.addComponentToSelectedEntity = false;
                    commands.addComponentType = EditorComponentType::None;
                    commands.addComponentTypeId.clear();
                    commands.removeComponentFromSelectedEntity = false;
                    commands.removeComponentTypeId.clear();
                    commands.physicsRaycastFromCamera = false;
                    commands.physicsOverlapSphereFromCamera = false;
                    commands.physicsOverlapBoxFromCamera = false;
                    commands.physicsOverlapCapsuleFromCamera = false;
                    commands.physicsSetLinearVelocityForSelected = false;
                    commands.physicsApplyForceToSelected = false;
                    commands.physicsApplyImpulseToSelected = false;
                    commands.physicsApplyAngularImpulseToSelected = false;
                    commands.physicsRuntimeEntityId = 0;
                    commands.lodQualityCommitRequested = false;
                    commands.lodQualityCommitEntityId = 0;
                    commands.assignMeshAssetToSelectedEntity = false;
                    commands.addPointLight = false;
                    commands.addSpotLight = false;
                    commands.deleteSelectedLight = false;
                    commands.deleteSelectedWaterBody = false;
                    commands.deleteSelectedMeshEntity = false;
                    commands.selectedLightChanged = false;
                    commands.selectedWaterBodyChanged = false;
                    commands.selectedMeshEntityChanged = false;
                    commands.hierarchyDeleteEntity = false;
                    commands.hierarchyDuplicateEntity = false;
                    commands.hierarchyRenameEntity = false;
                    commands.hierarchyToggleHidden = false;
                    commands.hierarchyReparentEntity = false;
                    commands.hierarchyParentType = HierarchyEntityType::None;
                    commands.hierarchyParentId = 0;
                    commands.paletteSlotChanged = false;
                    commands.save = false;
                    commands.reload = false;
                    commands.undo = false;
                }
#endif
                if (commands.hierarchySelectEntity)
                    selectHierarchyEntity(commands.hierarchyEntityType, commands.hierarchyEntityId, commands.hierarchyEntityHandle);
                if (commands.hierarchyFocusEntity)
                    focusHierarchyEntity(commands.hierarchyEntityType, commands.hierarchyEntityId);
                if (commands.hierarchyDeleteEntity)
                    deleteHierarchyEntity(commands.hierarchyEntityType, commands.hierarchyEntityId);
                if (commands.hierarchyDuplicateEntity)
                    duplicateHierarchyEntity(commands.hierarchyEntityType, commands.hierarchyEntityId);
                if (commands.hierarchyRenameEntity)
                    renameHierarchyEntity(commands.hierarchyEntityType, commands.hierarchyEntityId, commands.hierarchyRenameValue);
                if (commands.hierarchyToggleHidden)
                    toggleHierarchyHidden(commands.hierarchyEntityType, commands.hierarchyEntityId);
                if (commands.hierarchyReparentEntity)
                    reparentHierarchyEntity(commands.hierarchyEntityType,
                        commands.hierarchyEntityId,
                        commands.hierarchyParentType,
                        commands.hierarchyParentId);
                auto selectedEntityPosition = [&]() {
                    if (selectedEditorObject.type == SelectedEditorObjectType::Terrain)
                    {
                        return WorldVec3{0.0f, 0.0f, 0.0f};
                    }
                    if (selectedEditorObject.type == SelectedEditorObjectType::WaterBody)
                    {
                        auto it = std::find_if(editorWaterBodies.begin(), editorWaterBodies.end(),
                            [&](const WaterBody& body) { return body.id == selectedEditorObject.id; });
                        if (it != editorWaterBodies.end())
                            return WaterBodyCenter(*it);
                    }
                    else if (selectedEditorObject.type == SelectedEditorObjectType::PointLight)
                    {
                        auto it = std::find_if(editorPointLights.begin(), editorPointLights.end(),
                            [&](const PointLight& light) { return light.id == selectedEditorObject.id; });
                        if (it != editorPointLights.end())
                            return WorldVec3{it->position[0], it->position[1], it->position[2]};
                    }
                    else if (selectedEditorObject.type == SelectedEditorObjectType::SpotLight)
                    {
                        auto it = std::find_if(editorSpotLights.begin(), editorSpotLights.end(),
                            [&](const SpotLight& light) { return light.id == selectedEditorObject.id; });
                        if (it != editorSpotLights.end())
                            return WorldVec3{it->position[0], it->position[1], it->position[2]};
                    }
                    else if (selectedEditorObject.type == SelectedEditorObjectType::MeshEntity)
                    {
                        auto it = std::find_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                            [&](const MeshSceneEntity& mesh) { return mesh.id == selectedEditorObject.id; });
                        if (it != editorMeshEntities.end())
                            return WorldVec3{it->position[0], it->position[1], it->position[2]};
                    }
                    return spawnAtCameraCenter();
                };
                auto resolveModelAsset = [&](const std::string& assetId) -> std::optional<AssetLibrary::Entry> {
                    if (assetId.empty())
                        return std::nullopt;
                    std::string error;
                    if (ProjectManager::Instance().HasProject())
                    {
                        AssetLibrary projectAssets(ProjectManager::Instance().ProjectRoot(),
                            ProjectManager::Instance().AssetRootPath());
                        if (projectAssets.Initialize())
                        {
                            auto entry = projectAssets.FindById(assetId);
                            if (entry && entry->category == AssetLibrary::Category::Model)
                            {
                                entry->originalPath = projectAssets.AssetRelativePath(*entry);
                                return entry;
                            }
                        }
                    }
                    if (auto root = assets.RootPath())
                    {
                        AssetLibrary engineAssets(*root);
                        if (engineAssets.Initialize())
                        {
                            auto entry = engineAssets.FindById(assetId);
                            if (entry && entry->category == AssetLibrary::Category::Model)
                            {
                                entry->originalPath = engineAssets.AssetRelativePath(*entry);
                                return entry;
                            }
                        }
                    }
                    return std::nullopt;
                };
                auto createMeshEntityAt = [&](const std::string& assetId, WorldVec3 spawn) {
                    auto entry = resolveModelAsset(assetId);
                    MeshSceneEntity mesh{};
                    mesh.id = nextEditorMeshEntityId++;
                    mesh.meshAssetId = assetId;
                    mesh.meshAssetPath = entry ? entry->originalPath : assetId;
                    auto primitiveDisplayName = [](const std::string& path) {
                        if (path == "builtin://primitive/cube")
                            return std::string("Cube");
                        if (path == "builtin://primitive/sphere")
                            return std::string("Sphere");
                        if (path == "builtin://primitive/capsule")
                            return std::string("Capsule");
                        return std::string{};
                    };
                    const std::string primitiveName = primitiveDisplayName(assetId);
                    const std::string baseName = !primitiveName.empty()
                        ? primitiveName
                        : (entry
                            ? (entry->displayName.empty() ? std::filesystem::path(entry->filename).stem().string() : entry->displayName)
                            : (assetId.empty() ? std::string("Mesh Entity") : assetId));
                    mesh.name = makeUniqueSceneEntityName(baseName);
                    mesh.position[0] = spawn.x;
                    mesh.position[1] = spawn.y;
                    mesh.position[2] = spawn.z;
                    auto hasSkeletalSidecar = [&](const std::string& modelPath) {
                        std::filesystem::path path(modelPath);
                        if (path.is_relative() && ProjectManager::Instance().HasProject())
                            path = ProjectManager::Instance().ProjectRoot() / path;
                        std::string ext = path.extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
                            return static_cast<char>(std::tolower(c));
                        });
                        if (ext == ".fbx")
                        {
                            const std::filesystem::path skeleton =
                                path.parent_path() / (path.stem().string() + "_skeleton.ozz");
                            return std::filesystem::exists(skeleton);
                        }
                        // glTF/GLB: a rigged model (has a skin) is skinned. Without this, a
                        // .glb character was treated as static and silently dropped by the
                        // static path (which refuses skinned glTF), so it never rendered.
                        if (ext == ".glb" || ext == ".gltf")
                        {
                            bool isSkinned = false;
                            std::string detectError;
                            return StaticMeshRenderer::DetectSkinnedGltf(assets, path.string(), isSkinned, &detectError) && isSkinned;
                        }
                        return false;
                    };
                    mesh.skinned = hasSkeletalSidecar(mesh.meshAssetPath);
                    mesh.materialSlots = LoadDefaultMaterialSlotGuids(mesh.meshAssetPath, ResolveModelSubmeshCount(mesh.meshAssetPath));
                    editorMeshEntities.push_back(mesh);
                    editorMeshEntityLookup[mesh.id] = editorMeshEntities.size() - 1u;
                    syncStaticMeshSpatialEntity(editorMeshEntities.back());
                    selectedEditorObject = {SelectedEditorObjectType::MeshEntity, mesh.id};
                    editorGizmoMode = EditorGizmoMode::Translate;
                    SceneManager::Instance().MarkDirty();
                    runtimeSession->SetEditorStatus("Mesh entity spawned: " + mesh.name);
                    Tracenf("[MESH-ENTITY] Spawned: id=%u asset_id=%s path=%s position=(%.2f,%.2f,%.2f)",
                        mesh.id,
                        mesh.meshAssetId.c_str(),
                        mesh.meshAssetPath.c_str(),
                        spawn.x,
                        spawn.y,
                        spawn.z);
                };
                auto resolvePrefabAssetPath = [&](const std::string& assetId)
                    -> std::optional<std::pair<AssetLibrary::Entry, std::filesystem::path>> {
                    if (assetId.empty())
                        return std::nullopt;
                    if (ProjectManager::Instance().HasProject())
                    {
                        AssetLibrary projectAssets(ProjectManager::Instance().ProjectRoot(),
                            ProjectManager::Instance().AssetRootPath());
                        if (projectAssets.Initialize())
                        {
                            auto entry = projectAssets.FindById(assetId);
                            if (entry && entry->category == AssetLibrary::Category::Prefab)
                                return std::make_pair(*entry, projectAssets.AbsolutePath(*entry));
                        }
                    }
                    if (auto root = assets.RootPath())
                    {
                        AssetLibrary engineAssets(*root);
                        if (engineAssets.Initialize())
                        {
                            auto entry = engineAssets.FindById(assetId);
                            if (entry && entry->category == AssetLibrary::Category::Prefab)
                                return std::make_pair(*entry, engineAssets.AbsolutePath(*entry));
                        }
                    }
                    return std::nullopt;
                };
                auto loadPrefabDocument = [&](const std::string& assetId)
                    -> std::optional<std::pair<prefab::PrefabDocument, std::filesystem::path>> {
                    const auto resolved = resolvePrefabAssetPath(assetId);
                    if (!resolved)
                    {
                        TraceError("[PREFAB] load failed: asset not found id=%s", assetId.c_str());
                        return std::nullopt;
                    }
                    std::ifstream file(resolved->second, std::ios::binary);
                    if (!file)
                    {
                        TraceError("[PREFAB] load failed: unreadable path=%s", resolved->second.string().c_str());
                        return std::nullopt;
                    }
                    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                    prefab::PrefabDocument prefabData = prefab::ParseDocument(text, resolved->first.displayName);
                    if (prefabData.entities.empty())
                    {
                        TraceError("[PREFAB] load failed: unsupported file=%s", resolved->second.string().c_str());
                        return std::nullopt;
                    }
                    return std::make_pair(std::move(prefabData), resolved->second);
                };
                auto selectedPrefabRoot = [&]() -> std::optional<std::pair<HierarchyEntityType, std::uint32_t>> {
                    if (selectedEditorObject.type == SelectedEditorObjectType::MeshEntity)
                        return std::make_pair(HierarchyEntityType::MeshEntity, selectedEditorObject.id);
                    if (selectedEditorObject.type == SelectedEditorObjectType::PointLight)
                        return std::make_pair(HierarchyEntityType::PointLight, selectedEditorObject.id);
                    if (selectedEditorObject.type == SelectedEditorObjectType::SpotLight)
                        return std::make_pair(HierarchyEntityType::SpotLight, selectedEditorObject.id);
                    return std::nullopt;
                };
                auto buildPrefabDocumentFromRoot = [&](HierarchyEntityType rootType,
                                                       std::uint32_t rootId,
                                                       std::string prefabName) -> std::optional<prefab::PrefabDocument> {
                    prefab::PrefabDocument document;
                    struct PendingPrefabEntity
                    {
                        HierarchyEntityType type = HierarchyEntityType::None;
                        std::uint32_t id = 0;
                        std::uint32_t parentLocalId = 0;
                    };
                    std::vector<PendingPrefabEntity> pending;
                    pending.push_back({rootType, rootId, 0});

                    std::unordered_map<std::uint64_t, std::uint32_t> localIds;
                    auto enqueueChildren = [&](HierarchyEntityType parentType, std::uint32_t parentId, std::uint32_t parentLocalId) {
                        const std::string parentTypeName = SceneParentTypeName(parentType);
                        for (const MeshSceneEntity& mesh : editorMeshEntities)
                        {
                            if (mesh.parent.type == parentTypeName && mesh.parent.id == parentId)
                                pending.push_back({HierarchyEntityType::MeshEntity, mesh.id, parentLocalId});
                        }
                        for (const PointLight& light : editorPointLights)
                        {
                            if (light.parent.type == parentTypeName && light.parent.id == parentId)
                                pending.push_back({HierarchyEntityType::PointLight, light.id, parentLocalId});
                        }
                        for (const SpotLight& light : editorSpotLights)
                        {
                            if (light.parent.type == parentTypeName && light.parent.id == parentId)
                                pending.push_back({HierarchyEntityType::SpotLight, light.id, parentLocalId});
                        }
                    };

                    for (std::size_t i = 0; i < pending.size(); ++i)
                    {
                        const PendingPrefabEntity item = pending[i];
                        const std::uint64_t key = HierarchyObjectKey(item.type, item.id);
                        if (localIds.find(key) != localIds.end())
                            continue;
                        const std::uint32_t localId = static_cast<std::uint32_t>(localIds.size() + 1u);
                        localIds[key] = localId;

                        prefab::PrefabEntity entity;
                        entity.localId = localId;
                        entity.parentLocalId = item.parentLocalId;
                        if (item.type == HierarchyEntityType::MeshEntity)
                        {
                            auto it = std::find_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                                [&](const MeshSceneEntity& mesh) { return mesh.id == item.id; });
                            if (it == editorMeshEntities.end())
                                continue;
                            MeshSceneEntity mesh = *it;
                            if (localId == 1)
                            {
                                mesh.prefabAssetId.clear();
                                mesh.prefabInstance = {};
                            }
                            mesh.parent = {};
                            entity.kind = prefab::PrefabTemplate::Kind::Mesh;
                            entity.name = EditorDisplayName(*it);
                            entity.mesh = std::move(mesh);
                            if (localId == 1)
                                prefabName = entity.name;
                        }
                        else if (item.type == HierarchyEntityType::PointLight)
                        {
                            auto it = std::find_if(editorPointLights.begin(), editorPointLights.end(),
                                [&](const PointLight& light) { return light.id == item.id; });
                            if (it == editorPointLights.end())
                                continue;
                            PointLight light = *it;
                            if (localId == 1)
                            {
                                light.prefabAssetId.clear();
                                light.prefabInstance = {};
                            }
                            light.parent = {};
                            entity.kind = prefab::PrefabTemplate::Kind::PointLight;
                            entity.name = EditorDisplayName(*it);
                            entity.point = light;
                            if (localId == 1)
                                prefabName = entity.name;
                        }
                        else if (item.type == HierarchyEntityType::SpotLight)
                        {
                            auto it = std::find_if(editorSpotLights.begin(), editorSpotLights.end(),
                                [&](const SpotLight& light) { return light.id == item.id; });
                            if (it == editorSpotLights.end())
                                continue;
                            SpotLight light = *it;
                            if (localId == 1)
                            {
                                light.prefabAssetId.clear();
                                light.prefabInstance = {};
                            }
                            light.parent = {};
                            entity.kind = prefab::PrefabTemplate::Kind::SpotLight;
                            entity.name = EditorDisplayName(*it);
                            entity.spot = light;
                            if (localId == 1)
                                prefabName = entity.name;
                        }
                        if (entity.kind == prefab::PrefabTemplate::Kind::Unsupported)
                            continue;
                        document.entities.push_back(std::move(entity));
                        enqueueChildren(item.type, item.id, localId);
                    }

                    if (document.entities.empty())
                        return std::nullopt;
                    document.name = prefabName.empty() ? "Prefab" : prefabName;
                    return document;
                };
                auto createPrefabFromSelection = [&]() {
                    if (!ProjectManager::Instance().HasProject())
                    {
                        runtimeSession->SetEditorStatus("Open or create a project before creating prefabs");
                        return false;
                    }
                    const auto root = selectedPrefabRoot();
                    if (!root)
                    {
                        runtimeSession->SetEditorStatus("Select a mesh or light before creating a prefab");
                        return false;
                    }
                    auto document = buildPrefabDocumentFromRoot(root->first, root->second, "Prefab");
                    if (!document)
                    {
                        runtimeSession->SetEditorStatus("Select a mesh or light before creating a prefab");
                        return false;
                    }
                    const std::string prefabName = document->name.empty() ? "Prefab" : document->name;

                    std::error_code ec;
                    std::filesystem::path tempPath = std::filesystem::temp_directory_path(ec);
                    if (ec)
                        tempPath = ProjectManager::Instance().ProjectRoot();
                    std::string fileStem = prefabName;
                    for (char& ch : fileStem)
                    {
                        const unsigned char c = static_cast<unsigned char>(ch);
                        if (!std::isalnum(c) && ch != '_' && ch != '-')
                            ch = '_';
                    }
                    if (fileStem.empty())
                        fileStem = "Prefab";
                    tempPath /= fileStem + ".ixprefab";
                    {
                        std::ofstream file(tempPath, std::ios::binary);
                        if (!file)
                        {
                            runtimeSession->SetEditorStatus("Prefab create failed: temp write");
                            return false;
                        }
                        std::ostringstream json;
                        prefab::WriteDocument(json, *document);
                        const std::string text = json.str();
                        file.write(text.data(), static_cast<std::streamsize>(text.size()));
                    }

                    AssetLibrary projectAssets(ProjectManager::Instance().ProjectRoot(),
                        ProjectManager::Instance().AssetRootPath());
                    if (!projectAssets.Initialize())
                    {
                        std::filesystem::remove(tempPath, ec);
                        runtimeSession->SetEditorStatus("Prefab create failed: asset library");
                        return false;
                    }
                    AssetLibrary::ImportOptions options;
                    options.displayName = prefabName;
                    options.tags = {"prefab"};
                    AssetLibrary::Entry entry;
                    std::string error;
                    if (!projectAssets.Import(AssetLibrary::Category::Prefab, tempPath, options, entry, error))
                    {
                        std::filesystem::remove(tempPath, ec);
                        runtimeSession->SetEditorStatus("Prefab create failed: " + error);
                        return false;
                    }
                    std::filesystem::remove(tempPath, ec);
                    AssetDatabase::Instance().runtimeAdd(projectAssets.AbsolutePath(entry));
                    editorImGui.RefreshAssetLibrary();
                    runtimeSession->SetEditorStatus("Prefab created: " + entry.displayName);
                    Tracenf("[PREFAB] created asset_id=%s name=%s file=%s",
                        entry.id.c_str(),
                        entry.displayName.c_str(),
                        projectAssets.AssetRelativePath(entry).c_str());
                    return true;
                };
                auto writePrefabTemplateToPath = [&](const std::filesystem::path& path,
                                                     const std::string& prefabName,
                                                     auto&& writeEntity) {
                    std::filesystem::path tempPath = path;
                    tempPath += ".tmp";
                    {
                        std::ofstream file(tempPath, std::ios::binary | std::ios::trunc);
                        if (!file)
                            return false;
                        file << "{\n";
                        file << "  \"version\": 1,\n";
                        file << "  \"name\": \"" << ixtreeme::common::EscapeJson(prefabName) << "\",\n";
                        writeEntity(file, prefabName);
                        file << "}\n";
                    }
                    std::error_code ec;
                    std::filesystem::rename(tempPath, path, ec);
                    if (ec)
                    {
                        std::filesystem::remove(path, ec);
                        ec.clear();
                        std::filesystem::rename(tempPath, path, ec);
                    }
                    if (ec)
                    {
                        std::filesystem::remove(tempPath, ec);
                        return false;
                    }
                    return true;
                };
                auto writePrefabDocumentToPath = [&](const std::filesystem::path& path,
                                                     const prefab::PrefabDocument& document) {
                    std::filesystem::path tempPath = path;
                    tempPath += ".tmp";
                    {
                        std::ofstream file(tempPath, std::ios::binary | std::ios::trunc);
                        if (!file)
                            return false;
                        prefab::WriteDocument(file, document);
                    }
                    std::error_code ec;
                    std::filesystem::rename(tempPath, path, ec);
                    if (ec)
                    {
                        std::filesystem::remove(path, ec);
                        ec.clear();
                        std::filesystem::rename(tempPath, path, ec);
                    }
                    if (ec)
                    {
                        std::filesystem::remove(tempPath, ec);
                        return false;
                    }
                    return true;
                };
                auto writePrefabDependencies = [&](const std::filesystem::path& path,
                                                   const prefab::PrefabDocument& document) {
                    std::vector<Guid> dependencies;
                    auto addGuidText = [&](const std::string& guidText) {
                        if (const std::optional<Guid> guid = Guid::fromString(guidText))
                            dependencies.push_back(*guid);
                    };
                    for (const prefab::PrefabEntity& entity : document.entities)
                    {
                        if (entity.kind == prefab::PrefabTemplate::Kind::Mesh)
                        {
                            addGuidText(entity.mesh.meshAssetId);
                            addGuidText(entity.mesh.prefabAssetId);
                            if (!entity.mesh.prefabInstance.assetId.empty())
                                addGuidText(entity.mesh.prefabInstance.assetId);
                            for (const std::string& material : entity.mesh.materialSlots)
                                addGuidText(material);
                        }
                        else if (entity.kind == prefab::PrefabTemplate::Kind::PointLight)
                        {
                            addGuidText(entity.point.prefabAssetId);
                            if (!entity.point.prefabInstance.assetId.empty())
                                addGuidText(entity.point.prefabInstance.assetId);
                        }
                        else if (entity.kind == prefab::PrefabTemplate::Kind::SpotLight)
                        {
                            addGuidText(entity.spot.prefabAssetId);
                            if (!entity.spot.prefabInstance.assetId.empty())
                                addGuidText(entity.spot.prefabInstance.assetId);
                        }
                    }
                    return AssetDatabase::Instance().writeDependencies(path, dependencies);
                };
                auto savePrefabAssetEdit = [&](const std::string& assetId,
                                               const std::string& prefabName,
                                               const std::vector<PrefabAssetEntityNameEdit>& entityNames) {
                    if (assetId.empty())
                        return false;
                    auto loaded = loadPrefabDocument(assetId);
                    if (!loaded)
                        return false;

                    prefab::PrefabDocument& document = loaded->first;
                    if (!prefabName.empty())
                        document.name = prefabName;

                    std::unordered_set<std::uint32_t> editedLocalIds;
                    for (const PrefabAssetEntityNameEdit& edit : entityNames)
                    {
                        if (edit.localId != 0)
                            editedLocalIds.insert(edit.localId);
                    }
                    document.entities.erase(
                        std::remove_if(document.entities.begin(),
                            document.entities.end(),
                            [&](const prefab::PrefabEntity& entity) {
                                return entity.localId != 0 && !editedLocalIds.contains(entity.localId);
                            }),
                        document.entities.end());

                    auto kindForEdit = [](const PrefabAssetEntityNameEdit& edit) {
                        if (edit.type == "mesh_entity")
                            return prefab::PrefabTemplate::Kind::Mesh;
                        if (edit.type == "dynamic_light" && edit.lightType == "point")
                            return prefab::PrefabTemplate::Kind::PointLight;
                        if (edit.type == "dynamic_light" && edit.lightType == "spot")
                            return prefab::PrefabTemplate::Kind::SpotLight;
                        return prefab::PrefabTemplate::Kind::Unsupported;
                    };

                    for (const PrefabAssetEntityNameEdit& edit : entityNames)
                    {
                        if (edit.localId == 0 || edit.name.empty())
                            continue;
                        const prefab::PrefabTemplate::Kind editKind = kindForEdit(edit);
                        if (editKind == prefab::PrefabTemplate::Kind::Unsupported)
                            continue;
                        auto entityIt = std::find_if(document.entities.begin(), document.entities.end(),
                            [&](const prefab::PrefabEntity& entity) {
                                return entity.localId == edit.localId;
                            });
                        if (entityIt == document.entities.end())
                        {
                            prefab::PrefabEntity newEntity{};
                            newEntity.localId = edit.localId;
                            newEntity.kind = editKind;
                            if (editKind == prefab::PrefabTemplate::Kind::Mesh)
                            {
                                newEntity.mesh.meshAssetId = edit.meshAssetId;
                                newEntity.mesh.meshAssetPath = edit.meshAssetPath;
                                newEntity.mesh.prefabAssetId = edit.prefabAssetId;
                                newEntity.mesh.prefabInstance = MakePrefabInstanceState(edit.prefabAssetId);
                            }
                            document.entities.push_back(std::move(newEntity));
                            entityIt = std::prev(document.entities.end());
                        }
                        entityIt->kind = editKind;

                        entityIt->parentLocalId = edit.parentLocalId;
                        entityIt->name = edit.name;
                        if (entityIt->kind == prefab::PrefabTemplate::Kind::Mesh)
                        {
                            entityIt->mesh.name = edit.name;
                            entityIt->mesh.meshAssetId = edit.meshAssetId.empty() ? entityIt->mesh.meshAssetId : edit.meshAssetId;
                            entityIt->mesh.meshAssetPath = edit.meshAssetPath.empty() ? entityIt->mesh.meshAssetPath : edit.meshAssetPath;
                            entityIt->mesh.prefabAssetId = edit.prefabAssetId;
                            if (!edit.prefabAssetId.empty())
                            {
                                entityIt->mesh.prefabInstance = MakePrefabInstanceState(edit.prefabAssetId);
                                entityIt->mesh.prefabInstance.localId =
                                    entityIt->mesh.prefabInstance.localId == 0 ? 1u : entityIt->mesh.prefabInstance.localId;
                            }
                            entityIt->mesh.materialSlots = edit.materialSlots;
                            if (edit.transformValid)
                            {
                                std::copy(std::begin(edit.position), std::end(edit.position), std::begin(entityIt->mesh.position));
                                std::copy(std::begin(edit.rotation), std::end(edit.rotation), std::begin(entityIt->mesh.rotation));
                                std::copy(std::begin(edit.scale), std::end(edit.scale), std::begin(entityIt->mesh.scale));
                            }
                        }
                        else if (entityIt->kind == prefab::PrefabTemplate::Kind::PointLight)
                        {
                            entityIt->point.name = edit.name;
                            if (entityIt->point.radius <= 0.0f)
                                entityIt->point.radius = 10.0f;
                            if (edit.transformValid)
                                std::copy(std::begin(edit.position), std::end(edit.position), std::begin(entityIt->point.position));
                            if (edit.lightValid)
                            {
                                entityIt->point.r = edit.color[0];
                                entityIt->point.g = edit.color[1];
                                entityIt->point.b = edit.color[2];
                                entityIt->point.intensity = edit.intensity;
                                entityIt->point.radius = edit.radius;
                                entityIt->point.enabled = edit.enabled;
                            }
                        }
                        else if (entityIt->kind == prefab::PrefabTemplate::Kind::SpotLight)
                        {
                            entityIt->spot.name = edit.name;
                            if (entityIt->spot.radius <= 0.0f)
                                entityIt->spot.radius = 20.0f;
                            if (edit.transformValid)
                            {
                                std::copy(std::begin(edit.position), std::end(edit.position), std::begin(entityIt->spot.position));
                                std::copy(std::begin(edit.rotation), std::end(edit.rotation), std::begin(entityIt->spot.rotation));
                            }
                            if (edit.lightValid)
                            {
                                entityIt->spot.r = edit.color[0];
                                entityIt->spot.g = edit.color[1];
                                entityIt->spot.b = edit.color[2];
                                entityIt->spot.intensity = edit.intensity;
                                entityIt->spot.radius = edit.radius;
                                entityIt->spot.innerConeDegrees = edit.innerConeDegrees;
                                entityIt->spot.outerConeDegrees = std::max(edit.outerConeDegrees, edit.innerConeDegrees);
                                entityIt->spot.enabled = edit.enabled;
                            }
                        }
                    }

                    if (!writePrefabDocumentToPath(loaded->second, document))
                    {
                        TraceError("[PREFAB] asset edit save failed: path=%s", loaded->second.string().c_str());
                        return false;
                    }
                    AssetDatabase::Instance().runtimeAdd(loaded->second);
                    writePrefabDependencies(loaded->second, document);
                    editorImGui.RefreshAssetLibrary();
                    runtimeSession->SetEditorStatus("Prefab asset saved: " + document.name);
                    Tracenf("[PREFAB] asset edit saved asset=%s entities=%zu",
                        assetId.c_str(),
                        document.entities.size());
                    return true;
                };
                auto applySelectedPrefabToAsset = [&]() {
                    const auto root = selectedPrefabRoot();
                    if (!root)
                        return false;

                    std::string assetId;
                    std::string fallbackName;
                    if (root->first == HierarchyEntityType::MeshEntity)
                    {
                        auto it = std::find_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                            [&](const MeshSceneEntity& mesh) { return mesh.id == root->second; });
                        if (it == editorMeshEntities.end())
                            return false;
                        assetId = !it->prefabInstance.assetId.empty() ? it->prefabInstance.assetId : it->prefabAssetId;
                        fallbackName = EditorDisplayName(*it);
                    }
                    else if (root->first == HierarchyEntityType::PointLight)
                    {
                        auto it = std::find_if(editorPointLights.begin(), editorPointLights.end(),
                            [&](const PointLight& light) { return light.id == root->second; });
                        if (it == editorPointLights.end())
                            return false;
                        assetId = !it->prefabInstance.assetId.empty() ? it->prefabInstance.assetId : it->prefabAssetId;
                        fallbackName = EditorDisplayName(*it);
                    }
                    else if (root->first == HierarchyEntityType::SpotLight)
                    {
                        auto it = std::find_if(editorSpotLights.begin(), editorSpotLights.end(),
                            [&](const SpotLight& light) { return light.id == root->second; });
                        if (it == editorSpotLights.end())
                            return false;
                        assetId = !it->prefabInstance.assetId.empty() ? it->prefabInstance.assetId : it->prefabAssetId;
                        fallbackName = EditorDisplayName(*it);
                    }

                    const auto resolved = resolvePrefabAssetPath(assetId);
                    if (!resolved)
                        return false;
                    const std::string prefabName = resolved->first.displayName.empty() ? fallbackName : resolved->first.displayName;
                    auto document = buildPrefabDocumentFromRoot(root->first, root->second, prefabName);
                    if (!document)
                        return false;
                    document->name = prefabName;
                    if (!writePrefabDocumentToPath(resolved->second, *document))
                    {
                        TraceError("[PREFAB] apply failed: write path=%s", resolved->second.string().c_str());
                        return false;
                    }
                    AssetDatabase::Instance().runtimeAdd(resolved->second);
                    writePrefabDependencies(resolved->second, *document);
                    editorImGui.RefreshAssetLibrary();
                    runtimeSession->SetEditorStatus("Applied instance subtree to prefab: " + prefabName);
                    Tracenf("[PREFAB] applied instance subtree root=%u root_type=%d prefab=%s entities=%zu",
                        root->second,
                        static_cast<int>(root->first),
                        assetId.c_str(),
                        document->entities.size());
                    return true;
                };
                auto instantiatePrefabAt = [&](const std::string& assetId, WorldVec3 spawn) {
                    const auto loaded = loadPrefabDocument(assetId);
                    if (!loaded)
                    {
                        runtimeSession->SetEditorStatus("Prefab not found: " + assetId);
                        return false;
                    }
                    const prefab::PrefabDocument& prefabData = loaded->first;
                    if (prefabData.entities.empty())
                    {
                        runtimeSession->SetEditorStatus("Unsupported prefab entity");
                        TraceError("[PREFAB] instantiate failed: unsupported file=%s", loaded->second.string().c_str());
                        return false;
                    }

                    WorldVec3 rootPosition{0.0f, 0.0f, 0.0f};
                    const prefab::PrefabEntity& rootEntity = prefabData.entities.front();
                    if (rootEntity.kind == prefab::PrefabTemplate::Kind::Mesh)
                        rootPosition = {rootEntity.mesh.position[0], rootEntity.mesh.position[1], rootEntity.mesh.position[2]};
                    else if (rootEntity.kind == prefab::PrefabTemplate::Kind::PointLight)
                        rootPosition = {rootEntity.point.position[0], rootEntity.point.position[1], rootEntity.point.position[2]};
                    else if (rootEntity.kind == prefab::PrefabTemplate::Kind::SpotLight)
                        rootPosition = {rootEntity.spot.position[0], rootEntity.spot.position[1], rootEntity.spot.position[2]};

                    std::unordered_map<std::uint32_t, SceneParentRef> instantiatedRefs;
                    SelectedEditorObject firstCreated{};
                    std::size_t createdCount = 0;
                    auto offsetPosition = [&](float* position) {
                        position[0] = spawn.x + (position[0] - rootPosition.x);
                        position[1] = spawn.y + (position[1] - rootPosition.y);
                        position[2] = spawn.z + (position[2] - rootPosition.z);
                    };
                    auto assignPrefabLink = [&](auto& object, const prefab::PrefabEntity& prefabEntity) {
                        const std::string nestedAssetId = !object.prefabInstance.assetId.empty()
                            ? object.prefabInstance.assetId
                            : object.prefabAssetId;
                        if (!nestedAssetId.empty() && nestedAssetId != assetId)
                        {
                            object.prefabAssetId = nestedAssetId;
                            if (object.prefabInstance.assetId.empty())
                                object.prefabInstance = MakePrefabInstanceState(nestedAssetId);
                            object.prefabInstance.assetId = nestedAssetId;
                            object.prefabInstance.linked = true;
                            object.prefabInstance.localId = object.prefabInstance.localId == 0 ? 1u : object.prefabInstance.localId;
                            return;
                        }

                        object.prefabAssetId = assetId;
                        object.prefabInstance = MakePrefabInstanceState(assetId);
                        object.prefabInstance.localId = prefabEntity.localId == 0 ? 1u : prefabEntity.localId;
                    };

                    for (const prefab::PrefabEntity& prefabEntity : prefabData.entities)
                    {
                        const std::uint32_t prefabLocalRefId = prefabEntity.localId == 0 ? 1u : prefabEntity.localId;
                        SceneParentRef parent;
                        if (prefabEntity.parentLocalId != 0)
                        {
                            auto parentIt = instantiatedRefs.find(prefabEntity.parentLocalId);
                            if (parentIt != instantiatedRefs.end())
                                parent = parentIt->second;
                        }

                        if (prefabEntity.kind == prefab::PrefabTemplate::Kind::Mesh)
                        {
                            MeshSceneEntity mesh = prefabEntity.mesh;
                            mesh.id = nextEditorMeshEntityId++;
                            mesh.name = makeUniqueSceneEntityName(prefabEntity.name.empty() ? "Prefab Mesh" : prefabEntity.name);
                            assignPrefabLink(mesh, prefabEntity);
                            mesh.parent = parent;
                            offsetPosition(mesh.position);
                            if (mesh.materialSlots.empty())
                                mesh.materialSlots = LoadDefaultMaterialSlotGuids(mesh.meshAssetPath, ResolveModelSubmeshCount(mesh.meshAssetPath));
                            editorMeshEntities.push_back(mesh);
                            editorMeshEntityLookup[mesh.id] = editorMeshEntities.size() - 1u;
                            syncStaticMeshSpatialEntity(editorMeshEntities.back());
                            instantiatedRefs[prefabLocalRefId] = MakeSceneParentRef(HierarchyEntityType::MeshEntity, mesh.id);
                            if (createdCount == 0)
                                firstCreated = {SelectedEditorObjectType::MeshEntity, mesh.id};
                            ++createdCount;
                        }
                        else if (prefabEntity.kind == prefab::PrefabTemplate::Kind::PointLight)
                        {
                            if (editorPointLights.size() >= kMaxDynamicPointLights)
                                continue;
                            PointLight light = prefabEntity.point;
                            light.id = nextEditorLightId++;
                            light.name = makeUniqueSceneEntityName(prefabEntity.name.empty() ? "Point Light" : prefabEntity.name);
                            assignPrefabLink(light, prefabEntity);
                            light.parent = parent;
                            offsetPosition(light.position);
                            editorPointLights.push_back(light);
                            instantiatedRefs[prefabLocalRefId] = MakeSceneParentRef(HierarchyEntityType::PointLight, light.id);
                            if (createdCount == 0)
                                firstCreated = {SelectedEditorObjectType::PointLight, light.id};
                            ++createdCount;
                        }
                        else if (prefabEntity.kind == prefab::PrefabTemplate::Kind::SpotLight)
                        {
                            if (editorSpotLights.size() >= kMaxDynamicSpotLights)
                                continue;
                            SpotLight light = prefabEntity.spot;
                            light.id = nextEditorLightId++;
                            light.name = makeUniqueSceneEntityName(prefabEntity.name.empty() ? "Spot Light" : prefabEntity.name);
                            assignPrefabLink(light, prefabEntity);
                            light.parent = parent;
                            offsetPosition(light.position);
                            editorSpotLights.push_back(light);
                            instantiatedRefs[prefabLocalRefId] = MakeSceneParentRef(HierarchyEntityType::SpotLight, light.id);
                            if (createdCount == 0)
                                firstCreated = {SelectedEditorObjectType::SpotLight, light.id};
                            ++createdCount;
                        }
                    }
                    if (createdCount == 0)
                    {
                        runtimeSession->SetEditorStatus("Prefab instantiate failed: no supported entities");
                        return false;
                    }
                    selectedEditorObject = firstCreated;
                    editorGizmoMode = EditorGizmoMode::Translate;
                    SceneManager::Instance().MarkDirty();
                    runtimeSession->SetEditorStatus("Prefab instantiated: " + prefabData.name);
                    Tracenf("[PREFAB] instantiated prefab=%s entities=%zu position=(%.2f,%.2f,%.2f)",
                        assetId.c_str(), createdCount, spawn.x, spawn.y, spawn.z);
                    return true;
                };
                auto refreshMeshPrefabInstance = [&](MeshSceneEntity& mesh, bool preserveInstanceOverrides = true) {
                    const std::string sourcePrefabAssetId =
                        !mesh.prefabInstance.assetId.empty() ? mesh.prefabInstance.assetId : mesh.prefabAssetId;
                    if (sourcePrefabAssetId.empty())
                        return false;
                    const auto loaded = loadPrefabDocument(sourcePrefabAssetId);
                    const std::uint32_t localId = mesh.prefabInstance.localId == 0 ? 1u : mesh.prefabInstance.localId;
                    const prefab::PrefabEntity* prefabEntity = nullptr;
                    if (loaded)
                    {
                        auto entityIt = std::find_if(loaded->first.entities.begin(), loaded->first.entities.end(),
                            [&](const prefab::PrefabEntity& entity) {
                                return entity.localId == localId && entity.kind == prefab::PrefabTemplate::Kind::Mesh;
                            });
                        if (entityIt != loaded->first.entities.end())
                            prefabEntity = &*entityIt;
                    }
                    if (!prefabEntity)
                    {
                        TraceError("[PREFAB] refresh failed: mesh instance id=%u prefab=%s", mesh.id, sourcePrefabAssetId.c_str());
                        return false;
                    }

                    const std::uint32_t id = mesh.id;
                    const std::string name = preserveInstanceOverrides
                        ? mesh.name
                        : (prefabEntity->name.empty() ? prefabEntity->mesh.name : prefabEntity->name);
                    const std::string prefabAssetId = sourcePrefabAssetId;
                    PrefabInstanceState prefabInstance = mesh.prefabInstance;
                    if (prefabInstance.assetId.empty())
                        prefabInstance = MakePrefabInstanceState(prefabAssetId);
                    const bool editorHidden = mesh.editorHidden;
                    const SceneParentRef parent = mesh.parent;
                    const auto materialOverrides = preserveInstanceOverrides
                        ? mesh.materialOverrides
                        : std::vector<MeshSceneEntity::MaterialOverride>{};
                    const auto editorComponents = preserveInstanceOverrides
                        ? mesh.editorComponents
                        : std::vector<EditorAttachedComponent>{};
                    const LodComponent lod = preserveInstanceOverrides ? mesh.lod : prefabEntity->mesh.lod;
                    float position[3] = {mesh.position[0], mesh.position[1], mesh.position[2]};
                    float rotation[3] = {mesh.rotation[0], mesh.rotation[1], mesh.rotation[2]};
                    float scale[3] = {mesh.scale[0], mesh.scale[1], mesh.scale[2]};

                    mesh.meshAssetId = prefabEntity->mesh.meshAssetId;
                    mesh.meshAssetPath = prefabEntity->mesh.meshAssetPath;
                    mesh.skinned = prefabEntity->mesh.skinned;
                    mesh.materialSlots = prefabEntity->mesh.materialSlots;
                    if (mesh.materialSlots.empty())
                        mesh.materialSlots = LoadDefaultMaterialSlotGuids(mesh.meshAssetPath, ResolveModelSubmeshCount(mesh.meshAssetPath));
                    mesh.materialOverrides = materialOverrides;
                    mesh.editorComponents = editorComponents;
                    mesh.lod = lod;
                    mesh.id = id;
                    mesh.name = name;
                    mesh.prefabAssetId = prefabAssetId;
                    mesh.prefabInstance = prefabInstance;
                    mesh.prefabInstance.assetId = prefabAssetId;
                    mesh.prefabInstance.linked = true;
                    mesh.prefabInstance.localId = localId;
                    mesh.parent = parent;
                    mesh.editorHidden = editorHidden;
                    if (preserveInstanceOverrides)
                    {
                        std::copy(std::begin(position), std::end(position), std::begin(mesh.position));
                        std::copy(std::begin(rotation), std::end(rotation), std::begin(mesh.rotation));
                        std::copy(std::begin(scale), std::end(scale), std::begin(mesh.scale));
                    }
                    syncStaticMeshSpatialEntity(mesh);
                    Tracenf("[PREFAB] refreshed instance id=%u prefab=%s type=mesh preserveOverrides=%d",
                        mesh.id,
                        mesh.prefabAssetId.c_str(),
                        preserveInstanceOverrides ? 1 : 0);
                    return true;
                };
                auto refreshPointPrefabInstance = [&](PointLight& light, bool preserveInstanceOverrides = true) {
                    const std::string sourcePrefabAssetId =
                        !light.prefabInstance.assetId.empty() ? light.prefabInstance.assetId : light.prefabAssetId;
                    if (sourcePrefabAssetId.empty())
                        return false;
                    const auto loaded = loadPrefabDocument(sourcePrefabAssetId);
                    const std::uint32_t localId = light.prefabInstance.localId == 0 ? 1u : light.prefabInstance.localId;
                    const prefab::PrefabEntity* prefabEntity = nullptr;
                    if (loaded)
                    {
                        auto entityIt = std::find_if(loaded->first.entities.begin(), loaded->first.entities.end(),
                            [&](const prefab::PrefabEntity& entity) {
                                return entity.localId == localId && entity.kind == prefab::PrefabTemplate::Kind::PointLight;
                            });
                        if (entityIt != loaded->first.entities.end())
                            prefabEntity = &*entityIt;
                    }
                    if (!prefabEntity)
                    {
                        TraceError("[PREFAB] refresh failed: point_light id=%u prefab=%s", light.id, sourcePrefabAssetId.c_str());
                        return false;
                    }
                    const std::uint32_t id = light.id;
                    const std::string name = preserveInstanceOverrides
                        ? light.name
                        : (prefabEntity->name.empty() ? prefabEntity->point.name : prefabEntity->name);
                    const std::string prefabAssetId = sourcePrefabAssetId;
                    PrefabInstanceState prefabInstance = light.prefabInstance;
                    if (prefabInstance.assetId.empty())
                        prefabInstance = MakePrefabInstanceState(prefabAssetId);
                    const bool editorHidden = light.editorHidden;
                    const SceneParentRef parent = light.parent;
                    const float position[3] = {light.position[0], light.position[1], light.position[2]};
                    light = prefabEntity->point;
                    light.id = id;
                    light.name = name;
                    light.prefabAssetId = prefabAssetId;
                    light.prefabInstance = prefabInstance;
                    light.prefabInstance.assetId = prefabAssetId;
                    light.prefabInstance.linked = true;
                    light.prefabInstance.localId = localId;
                    light.parent = parent;
                    light.editorHidden = editorHidden;
                    if (preserveInstanceOverrides)
                        std::copy(std::begin(position), std::end(position), std::begin(light.position));
                    Tracenf("[PREFAB] refreshed instance id=%u prefab=%s type=point_light preserveOverrides=%d",
                        light.id,
                        light.prefabAssetId.c_str(),
                        preserveInstanceOverrides ? 1 : 0);
                    return true;
                };
                auto refreshSpotPrefabInstance = [&](SpotLight& light, bool preserveInstanceOverrides = true) {
                    const std::string sourcePrefabAssetId =
                        !light.prefabInstance.assetId.empty() ? light.prefabInstance.assetId : light.prefabAssetId;
                    if (sourcePrefabAssetId.empty())
                        return false;
                    const auto loaded = loadPrefabDocument(sourcePrefabAssetId);
                    const std::uint32_t localId = light.prefabInstance.localId == 0 ? 1u : light.prefabInstance.localId;
                    const prefab::PrefabEntity* prefabEntity = nullptr;
                    if (loaded)
                    {
                        auto entityIt = std::find_if(loaded->first.entities.begin(), loaded->first.entities.end(),
                            [&](const prefab::PrefabEntity& entity) {
                                return entity.localId == localId && entity.kind == prefab::PrefabTemplate::Kind::SpotLight;
                            });
                        if (entityIt != loaded->first.entities.end())
                            prefabEntity = &*entityIt;
                    }
                    if (!prefabEntity)
                    {
                        TraceError("[PREFAB] refresh failed: spot_light id=%u prefab=%s", light.id, sourcePrefabAssetId.c_str());
                        return false;
                    }
                    const std::uint32_t id = light.id;
                    const std::string name = preserveInstanceOverrides
                        ? light.name
                        : (prefabEntity->name.empty() ? prefabEntity->spot.name : prefabEntity->name);
                    const std::string prefabAssetId = sourcePrefabAssetId;
                    PrefabInstanceState prefabInstance = light.prefabInstance;
                    if (prefabInstance.assetId.empty())
                        prefabInstance = MakePrefabInstanceState(prefabAssetId);
                    const bool editorHidden = light.editorHidden;
                    const SceneParentRef parent = light.parent;
                    const float position[3] = {light.position[0], light.position[1], light.position[2]};
                    const float rotation[3] = {light.rotation[0], light.rotation[1], light.rotation[2]};
                    light = prefabEntity->spot;
                    light.id = id;
                    light.name = name;
                    light.prefabAssetId = prefabAssetId;
                    light.prefabInstance = prefabInstance;
                    light.prefabInstance.assetId = prefabAssetId;
                    light.prefabInstance.linked = true;
                    light.prefabInstance.localId = localId;
                    light.parent = parent;
                    light.editorHidden = editorHidden;
                    if (preserveInstanceOverrides)
                    {
                        std::copy(std::begin(position), std::end(position), std::begin(light.position));
                        std::copy(std::begin(rotation), std::end(rotation), std::begin(light.rotation));
                    }
                    Tracenf("[PREFAB] refreshed instance id=%u prefab=%s type=spot_light preserveOverrides=%d",
                        light.id,
                        light.prefabAssetId.c_str(),
                        preserveInstanceOverrides ? 1 : 0);
                    return true;
                };
                auto refreshSelectedPrefabInstance = [&]() {
                    bool changed = false;
                    if (selectedEditorObject.type == SelectedEditorObjectType::MeshEntity)
                    {
                        auto it = std::find_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                            [&](const MeshSceneEntity& mesh) { return mesh.id == selectedEditorObject.id; });
                        changed = it != editorMeshEntities.end() && refreshMeshPrefabInstance(*it);
                    }
                    else if (selectedEditorObject.type == SelectedEditorObjectType::PointLight)
                    {
                        auto it = std::find_if(editorPointLights.begin(), editorPointLights.end(),
                            [&](const PointLight& light) { return light.id == selectedEditorObject.id; });
                        changed = it != editorPointLights.end() && refreshPointPrefabInstance(*it);
                    }
                    else if (selectedEditorObject.type == SelectedEditorObjectType::SpotLight)
                    {
                        auto it = std::find_if(editorSpotLights.begin(), editorSpotLights.end(),
                            [&](const SpotLight& light) { return light.id == selectedEditorObject.id; });
                        changed = it != editorSpotLights.end() && refreshSpotPrefabInstance(*it);
                    }
                    if (changed)
                    {
                        SceneManager::Instance().MarkDirty();
                        runtimeSession->SetEditorStatus("Prefab instance refreshed");
                    }
                    else
                    {
                        runtimeSession->SetEditorStatus("Selected entity is not a refreshable prefab instance");
                    }
                    return changed;
                };
                auto revertSelectedPrefabInstance = [&]() {
                    bool changed = false;
                    if (selectedEditorObject.type == SelectedEditorObjectType::MeshEntity)
                    {
                        auto it = std::find_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                            [&](const MeshSceneEntity& mesh) { return mesh.id == selectedEditorObject.id; });
                        changed = it != editorMeshEntities.end() && refreshMeshPrefabInstance(*it, false);
                    }
                    else if (selectedEditorObject.type == SelectedEditorObjectType::PointLight)
                    {
                        auto it = std::find_if(editorPointLights.begin(), editorPointLights.end(),
                            [&](const PointLight& light) { return light.id == selectedEditorObject.id; });
                        changed = it != editorPointLights.end() && refreshPointPrefabInstance(*it, false);
                    }
                    else if (selectedEditorObject.type == SelectedEditorObjectType::SpotLight)
                    {
                        auto it = std::find_if(editorSpotLights.begin(), editorSpotLights.end(),
                            [&](const SpotLight& light) { return light.id == selectedEditorObject.id; });
                        changed = it != editorSpotLights.end() && refreshSpotPrefabInstance(*it, false);
                    }
                    if (changed)
                    {
                        SceneManager::Instance().MarkDirty();
                        runtimeSession->SetEditorStatus("Prefab overrides reverted");
                        Tracen("[PREFAB] reverted selected instance overrides");
                    }
                    else
                    {
                        runtimeSession->SetEditorStatus("Selected entity is not a revertable prefab instance");
                    }
                    return changed;
                };
                auto applySelectedPrefabOverrideToAsset = [&](const std::string& overrideName) {
                    if (overrideName.empty())
                        return false;

                    std::string assetId;
                    std::uint32_t localId = 1;
                    prefab::PrefabTemplate::Kind kind = prefab::PrefabTemplate::Kind::Unsupported;
                    MeshSceneEntity* selectedMesh = nullptr;
                    PointLight* selectedPoint = nullptr;
                    SpotLight* selectedSpot = nullptr;

                    if (selectedEditorObject.type == SelectedEditorObjectType::MeshEntity)
                    {
                        auto it = std::find_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                            [&](const MeshSceneEntity& mesh) { return mesh.id == selectedEditorObject.id; });
                        if (it == editorMeshEntities.end())
                            return false;
                        selectedMesh = &*it;
                        assetId = !it->prefabInstance.assetId.empty() ? it->prefabInstance.assetId : it->prefabAssetId;
                        localId = it->prefabInstance.localId == 0 ? 1u : it->prefabInstance.localId;
                        kind = prefab::PrefabTemplate::Kind::Mesh;
                    }
                    else if (selectedEditorObject.type == SelectedEditorObjectType::PointLight)
                    {
                        auto it = std::find_if(editorPointLights.begin(), editorPointLights.end(),
                            [&](const PointLight& light) { return light.id == selectedEditorObject.id; });
                        if (it == editorPointLights.end())
                            return false;
                        selectedPoint = &*it;
                        assetId = !it->prefabInstance.assetId.empty() ? it->prefabInstance.assetId : it->prefabAssetId;
                        localId = it->prefabInstance.localId == 0 ? 1u : it->prefabInstance.localId;
                        kind = prefab::PrefabTemplate::Kind::PointLight;
                    }
                    else if (selectedEditorObject.type == SelectedEditorObjectType::SpotLight)
                    {
                        auto it = std::find_if(editorSpotLights.begin(), editorSpotLights.end(),
                            [&](const SpotLight& light) { return light.id == selectedEditorObject.id; });
                        if (it == editorSpotLights.end())
                            return false;
                        selectedSpot = &*it;
                        assetId = !it->prefabInstance.assetId.empty() ? it->prefabInstance.assetId : it->prefabAssetId;
                        localId = it->prefabInstance.localId == 0 ? 1u : it->prefabInstance.localId;
                        kind = prefab::PrefabTemplate::Kind::SpotLight;
                    }

                    if (assetId.empty())
                        return false;
                    auto loaded = loadPrefabDocument(assetId);
                    if (!loaded)
                        return false;
                    prefab::PrefabDocument& document = loaded->first;
                    auto entityIt = std::find_if(document.entities.begin(), document.entities.end(),
                        [&](const prefab::PrefabEntity& entity) {
                            return entity.localId == localId && entity.kind == kind;
                        });
                    if (entityIt == document.entities.end())
                        return false;

                    bool changed = false;
                    if (selectedMesh && kind == prefab::PrefabTemplate::Kind::Mesh)
                    {
                        MeshSceneEntity& target = entityIt->mesh;
                        if (overrideName == "Name")
                        {
                            entityIt->name = selectedMesh->name;
                            target.name = selectedMesh->name;
                            changed = true;
                        }
                        else if (overrideName == "Transform")
                        {
                            std::copy(std::begin(selectedMesh->position), std::end(selectedMesh->position), std::begin(target.position));
                            std::copy(std::begin(selectedMesh->rotation), std::end(selectedMesh->rotation), std::begin(target.rotation));
                            std::copy(std::begin(selectedMesh->scale), std::end(selectedMesh->scale), std::begin(target.scale));
                            changed = true;
                        }
                        else if (overrideName == "Position")
                        {
                            std::copy(std::begin(selectedMesh->position), std::end(selectedMesh->position), std::begin(target.position));
                            changed = true;
                        }
                        else if (overrideName == "Rotation")
                        {
                            std::copy(std::begin(selectedMesh->rotation), std::end(selectedMesh->rotation), std::begin(target.rotation));
                            changed = true;
                        }
                        else if (overrideName == "Scale")
                        {
                            std::copy(std::begin(selectedMesh->scale), std::end(selectedMesh->scale), std::begin(target.scale));
                            changed = true;
                        }
                        else if (overrideName == "Mesh asset")
                        {
                            target.meshAssetId = selectedMesh->meshAssetId;
                            target.meshAssetPath = selectedMesh->meshAssetPath;
                            target.skinned = selectedMesh->skinned;
                            changed = true;
                        }
                        else if (overrideName == "Material slots")
                        {
                            target.materialSlots = selectedMesh->materialSlots;
                            changed = true;
                        }
                        else if (overrideName == "Material overrides")
                        {
                            target.materialOverrides = selectedMesh->materialOverrides;
                            changed = true;
                        }
                    }
                    else if (selectedPoint && kind == prefab::PrefabTemplate::Kind::PointLight)
                    {
                        PointLight& target = entityIt->point;
                        if (overrideName == "Name")
                        {
                            entityIt->name = EditorDisplayName(*selectedPoint);
                            target.name = selectedPoint->name;
                            changed = true;
                        }
                        else if (overrideName == "Transform" || overrideName == "Position")
                        {
                            std::copy(std::begin(selectedPoint->position), std::end(selectedPoint->position), std::begin(target.position));
                            changed = true;
                        }
                        else if (overrideName == "Color")
                        {
                            target.r = selectedPoint->r;
                            target.g = selectedPoint->g;
                            target.b = selectedPoint->b;
                            changed = true;
                        }
                        else if (overrideName == "Intensity")
                        {
                            target.intensity = selectedPoint->intensity;
                            changed = true;
                        }
                        else if (overrideName == "Radius")
                        {
                            target.radius = selectedPoint->radius;
                            changed = true;
                        }
                        else if (overrideName == "Enabled")
                        {
                            target.enabled = selectedPoint->enabled;
                            changed = true;
                        }
                    }
                    else if (selectedSpot && kind == prefab::PrefabTemplate::Kind::SpotLight)
                    {
                        SpotLight& target = entityIt->spot;
                        if (overrideName == "Name")
                        {
                            entityIt->name = EditorDisplayName(*selectedSpot);
                            target.name = selectedSpot->name;
                            changed = true;
                        }
                        else if (overrideName == "Transform")
                        {
                            std::copy(std::begin(selectedSpot->position), std::end(selectedSpot->position), std::begin(target.position));
                            std::copy(std::begin(selectedSpot->rotation), std::end(selectedSpot->rotation), std::begin(target.rotation));
                            changed = true;
                        }
                        else if (overrideName == "Position")
                        {
                            std::copy(std::begin(selectedSpot->position), std::end(selectedSpot->position), std::begin(target.position));
                            changed = true;
                        }
                        else if (overrideName == "Rotation")
                        {
                            std::copy(std::begin(selectedSpot->rotation), std::end(selectedSpot->rotation), std::begin(target.rotation));
                            changed = true;
                        }
                        else if (overrideName == "Color")
                        {
                            target.r = selectedSpot->r;
                            target.g = selectedSpot->g;
                            target.b = selectedSpot->b;
                            changed = true;
                        }
                        else if (overrideName == "Intensity")
                        {
                            target.intensity = selectedSpot->intensity;
                            changed = true;
                        }
                        else if (overrideName == "Radius")
                        {
                            target.radius = selectedSpot->radius;
                            changed = true;
                        }
                        else if (overrideName == "Cone")
                        {
                            target.innerConeDegrees = selectedSpot->innerConeDegrees;
                            target.outerConeDegrees = selectedSpot->outerConeDegrees;
                            changed = true;
                        }
                        else if (overrideName == "Enabled")
                        {
                            target.enabled = selectedSpot->enabled;
                            changed = true;
                        }
                    }

                    if (!changed)
                        return false;
                    if (!writePrefabDocumentToPath(loaded->second, document))
                    {
                        TraceError("[PREFAB] apply override failed: write path=%s", loaded->second.string().c_str());
                        return false;
                    }
                    AssetDatabase::Instance().runtimeAdd(loaded->second);
                    writePrefabDependencies(loaded->second, document);
                    editorImGui.RefreshAssetLibrary();
                    runtimeSession->SetEditorStatus("Applied prefab override: " + overrideName);
                    Tracenf("[PREFAB] applied override=%s prefab=%s local_id=%u",
                        overrideName.c_str(),
                        assetId.c_str(),
                        localId);
                    return true;
                };
                auto revertSelectedPrefabOverride = [&](const std::string& overrideName) {
                    if (overrideName.empty())
                        return false;

                    auto loadedMeshSource = [&](const MeshSceneEntity& mesh) -> std::optional<prefab::PrefabEntity> {
                        const std::string assetId = !mesh.prefabInstance.assetId.empty() ? mesh.prefabInstance.assetId : mesh.prefabAssetId;
                        const std::uint32_t localId = mesh.prefabInstance.localId == 0 ? 1u : mesh.prefabInstance.localId;
                        const auto loaded = loadPrefabDocument(assetId);
                        if (!loaded)
                            return std::nullopt;
                        auto entityIt = std::find_if(loaded->first.entities.begin(), loaded->first.entities.end(),
                            [&](const prefab::PrefabEntity& entity) {
                                return entity.localId == localId && entity.kind == prefab::PrefabTemplate::Kind::Mesh;
                            });
                        return entityIt == loaded->first.entities.end()
                            ? std::optional<prefab::PrefabEntity>{}
                            : std::optional<prefab::PrefabEntity>{*entityIt};
                    };
                    auto loadedPointSource = [&](const PointLight& light) -> std::optional<prefab::PrefabEntity> {
                        const std::string assetId = !light.prefabInstance.assetId.empty() ? light.prefabInstance.assetId : light.prefabAssetId;
                        const std::uint32_t localId = light.prefabInstance.localId == 0 ? 1u : light.prefabInstance.localId;
                        const auto loaded = loadPrefabDocument(assetId);
                        if (!loaded)
                            return std::nullopt;
                        auto entityIt = std::find_if(loaded->first.entities.begin(), loaded->first.entities.end(),
                            [&](const prefab::PrefabEntity& entity) {
                                return entity.localId == localId && entity.kind == prefab::PrefabTemplate::Kind::PointLight;
                            });
                        return entityIt == loaded->first.entities.end()
                            ? std::optional<prefab::PrefabEntity>{}
                            : std::optional<prefab::PrefabEntity>{*entityIt};
                    };
                    auto loadedSpotSource = [&](const SpotLight& light) -> std::optional<prefab::PrefabEntity> {
                        const std::string assetId = !light.prefabInstance.assetId.empty() ? light.prefabInstance.assetId : light.prefabAssetId;
                        const std::uint32_t localId = light.prefabInstance.localId == 0 ? 1u : light.prefabInstance.localId;
                        const auto loaded = loadPrefabDocument(assetId);
                        if (!loaded)
                            return std::nullopt;
                        auto entityIt = std::find_if(loaded->first.entities.begin(), loaded->first.entities.end(),
                            [&](const prefab::PrefabEntity& entity) {
                                return entity.localId == localId && entity.kind == prefab::PrefabTemplate::Kind::SpotLight;
                            });
                        return entityIt == loaded->first.entities.end()
                            ? std::optional<prefab::PrefabEntity>{}
                            : std::optional<prefab::PrefabEntity>{*entityIt};
                    };

                    bool changed = false;
                    if (selectedEditorObject.type == SelectedEditorObjectType::MeshEntity)
                    {
                        auto it = std::find_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                            [&](const MeshSceneEntity& mesh) { return mesh.id == selectedEditorObject.id; });
                        if (it == editorMeshEntities.end())
                            return false;
                        const auto loaded = loadedMeshSource(*it);
                        if (!loaded)
                            return false;
                        const MeshSceneEntity& source = loaded->mesh;
                        if (overrideName == "Name")
                        {
                            it->name = loaded->name.empty() ? source.name : loaded->name;
                            changed = true;
                        }
                        else if (overrideName == "Transform")
                        {
                            std::copy(std::begin(source.position), std::end(source.position), std::begin(it->position));
                            std::copy(std::begin(source.rotation), std::end(source.rotation), std::begin(it->rotation));
                            std::copy(std::begin(source.scale), std::end(source.scale), std::begin(it->scale));
                            syncStaticMeshSpatialEntity(*it);
                            changed = true;
                        }
                        else if (overrideName == "Position")
                        {
                            std::copy(std::begin(source.position), std::end(source.position), std::begin(it->position));
                            syncStaticMeshSpatialEntity(*it);
                            changed = true;
                        }
                        else if (overrideName == "Rotation")
                        {
                            std::copy(std::begin(source.rotation), std::end(source.rotation), std::begin(it->rotation));
                            syncStaticMeshSpatialEntity(*it);
                            changed = true;
                        }
                        else if (overrideName == "Scale")
                        {
                            std::copy(std::begin(source.scale), std::end(source.scale), std::begin(it->scale));
                            syncStaticMeshSpatialEntity(*it);
                            changed = true;
                        }
                        else if (overrideName == "Mesh asset")
                        {
                            it->meshAssetId = source.meshAssetId;
                            it->meshAssetPath = source.meshAssetPath;
                            it->skinned = source.skinned;
                            changed = true;
                        }
                        else if (overrideName == "Material slots")
                        {
                            it->materialSlots = source.materialSlots.empty()
                                ? LoadDefaultMaterialSlotGuids(source.meshAssetPath, ResolveModelSubmeshCount(source.meshAssetPath))
                                : source.materialSlots;
                            changed = true;
                        }
                        else if (overrideName == "Material overrides")
                        {
                            it->materialOverrides = source.materialOverrides;
                            changed = true;
                        }
                    }
                    else if (selectedEditorObject.type == SelectedEditorObjectType::PointLight)
                    {
                        auto it = std::find_if(editorPointLights.begin(), editorPointLights.end(),
                            [&](const PointLight& light) { return light.id == selectedEditorObject.id; });
                        if (it == editorPointLights.end())
                            return false;
                        const auto loaded = loadedPointSource(*it);
                        if (!loaded)
                            return false;
                        const PointLight& source = loaded->point;
                        if (overrideName == "Name")
                        {
                            it->name = loaded->name.empty() ? source.name : loaded->name;
                            changed = true;
                        }
                        else if (overrideName == "Transform" || overrideName == "Position")
                        {
                            std::copy(std::begin(source.position), std::end(source.position), std::begin(it->position));
                            changed = true;
                        }
                        else if (overrideName == "Color")
                        {
                            it->r = source.r;
                            it->g = source.g;
                            it->b = source.b;
                            changed = true;
                        }
                        else if (overrideName == "Intensity")
                        {
                            it->intensity = source.intensity;
                            changed = true;
                        }
                        else if (overrideName == "Radius")
                        {
                            it->radius = source.radius;
                            changed = true;
                        }
                        else if (overrideName == "Enabled")
                        {
                            it->enabled = source.enabled;
                            changed = true;
                        }
                    }
                    else if (selectedEditorObject.type == SelectedEditorObjectType::SpotLight)
                    {
                        auto it = std::find_if(editorSpotLights.begin(), editorSpotLights.end(),
                            [&](const SpotLight& light) { return light.id == selectedEditorObject.id; });
                        if (it == editorSpotLights.end())
                            return false;
                        const auto loaded = loadedSpotSource(*it);
                        if (!loaded)
                            return false;
                        const SpotLight& source = loaded->spot;
                        if (overrideName == "Name")
                        {
                            it->name = loaded->name.empty() ? source.name : loaded->name;
                            changed = true;
                        }
                        else if (overrideName == "Transform")
                        {
                            std::copy(std::begin(source.position), std::end(source.position), std::begin(it->position));
                            std::copy(std::begin(source.rotation), std::end(source.rotation), std::begin(it->rotation));
                            changed = true;
                        }
                        else if (overrideName == "Position")
                        {
                            std::copy(std::begin(source.position), std::end(source.position), std::begin(it->position));
                            changed = true;
                        }
                        else if (overrideName == "Rotation")
                        {
                            std::copy(std::begin(source.rotation), std::end(source.rotation), std::begin(it->rotation));
                            changed = true;
                        }
                        else if (overrideName == "Color")
                        {
                            it->r = source.r;
                            it->g = source.g;
                            it->b = source.b;
                            changed = true;
                        }
                        else if (overrideName == "Intensity")
                        {
                            it->intensity = source.intensity;
                            changed = true;
                        }
                        else if (overrideName == "Radius")
                        {
                            it->radius = source.radius;
                            changed = true;
                        }
                        else if (overrideName == "Cone")
                        {
                            it->innerConeDegrees = source.innerConeDegrees;
                            it->outerConeDegrees = source.outerConeDegrees;
                            changed = true;
                        }
                        else if (overrideName == "Enabled")
                        {
                            it->enabled = source.enabled;
                            changed = true;
                        }
                    }

                    if (changed)
                    {
                        SceneManager::Instance().MarkDirty();
                        runtimeSession->SetEditorStatus("Reverted prefab override: " + overrideName);
                        Tracenf("[PREFAB] reverted override=%s", overrideName.c_str());
                    }
                    return changed;
                };
                auto refreshAllPrefabInstances = [&]() {
                    std::size_t refreshed = 0;
                    for (MeshSceneEntity& mesh : editorMeshEntities)
                    {
                        if (refreshMeshPrefabInstance(mesh))
                            ++refreshed;
                    }
                    for (PointLight& light : editorPointLights)
                    {
                        if (refreshPointPrefabInstance(light))
                            ++refreshed;
                    }
                    for (SpotLight& light : editorSpotLights)
                    {
                        if (refreshSpotPrefabInstance(light))
                            ++refreshed;
                    }
                    if (refreshed > 0)
                    {
                        SceneManager::Instance().MarkDirty();
                        runtimeSession->SetEditorStatus("Prefab instances refreshed: " + std::to_string(refreshed));
                    }
                    Tracenf("[PREFAB] refresh_all refreshed=%zu", refreshed);
                    return refreshed;
                };
                auto refreshPrefabInstancesForAsset = [&](const std::string& assetId) {
                    if (assetId.empty())
                        return std::size_t{0};

                    std::size_t refreshed = 0;
                    auto matchesAsset = [&](const auto& object) {
                        const std::string objectAssetId =
                            !object.prefabInstance.assetId.empty() ? object.prefabInstance.assetId : object.prefabAssetId;
                        return objectAssetId == assetId;
                    };
                    for (MeshSceneEntity& mesh : editorMeshEntities)
                    {
                        if (matchesAsset(mesh) && refreshMeshPrefabInstance(mesh))
                            ++refreshed;
                    }
                    for (PointLight& light : editorPointLights)
                    {
                        if (matchesAsset(light) && refreshPointPrefabInstance(light))
                            ++refreshed;
                    }
                    for (SpotLight& light : editorSpotLights)
                    {
                        if (matchesAsset(light) && refreshSpotPrefabInstance(light))
                            ++refreshed;
                    }
                    if (refreshed > 0)
                    {
                        SceneManager::Instance().MarkDirty();
                        runtimeSession->SetEditorStatus("Prefab asset saved; instances refreshed: " + std::to_string(refreshed));
                    }
                    Tracenf("[PREFAB] refresh_asset asset=%s refreshed=%zu", assetId.c_str(), refreshed);
                    return refreshed;
                };
                auto unpackSelectedPrefabInstance = [&]() {
                    const auto root = selectedPrefabRoot();
                    if (!root)
                        return false;

                    std::size_t unpacked = 0;
                    std::string firstPrefab;
                    auto clearPrefabLink = [&](HierarchyEntityType type, std::uint32_t id) {
                        if (type == HierarchyEntityType::MeshEntity)
                        {
                            auto it = std::find_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                                [&](const MeshSceneEntity& mesh) { return mesh.id == id; });
                            if (it == editorMeshEntities.end() || (it->prefabAssetId.empty() && it->prefabInstance.assetId.empty()))
                                return false;
                            if (firstPrefab.empty())
                                firstPrefab = !it->prefabInstance.assetId.empty() ? it->prefabInstance.assetId : it->prefabAssetId;
                            it->prefabAssetId.clear();
                            it->prefabInstance = {};
                            ++unpacked;
                            return true;
                        }
                        if (type == HierarchyEntityType::PointLight)
                        {
                            auto it = std::find_if(editorPointLights.begin(), editorPointLights.end(),
                                [&](const PointLight& light) { return light.id == id; });
                            if (it == editorPointLights.end() || (it->prefabAssetId.empty() && it->prefabInstance.assetId.empty()))
                                return false;
                            if (firstPrefab.empty())
                                firstPrefab = !it->prefabInstance.assetId.empty() ? it->prefabInstance.assetId : it->prefabAssetId;
                            it->prefabAssetId.clear();
                            it->prefabInstance = {};
                            ++unpacked;
                            return true;
                        }
                        if (type == HierarchyEntityType::SpotLight)
                        {
                            auto it = std::find_if(editorSpotLights.begin(), editorSpotLights.end(),
                                [&](const SpotLight& light) { return light.id == id; });
                            if (it == editorSpotLights.end() || (it->prefabAssetId.empty() && it->prefabInstance.assetId.empty()))
                                return false;
                            if (firstPrefab.empty())
                                firstPrefab = !it->prefabInstance.assetId.empty() ? it->prefabInstance.assetId : it->prefabAssetId;
                            it->prefabAssetId.clear();
                            it->prefabInstance = {};
                            ++unpacked;
                            return true;
                        }
                        return false;
                    };

                    std::function<void(HierarchyEntityType, std::uint32_t)> unpackSubtree;
                    unpackSubtree = [&](HierarchyEntityType type, std::uint32_t id) {
                        clearPrefabLink(type, id);
                        const auto children = directHierarchyChildren(type, id);
                        for (const auto& child : children)
                            unpackSubtree(child.first, child.second);
                    };
                    unpackSubtree(root->first, root->second);
                    if (unpacked == 0)
                        return false;

                    SceneManager::Instance().MarkDirty();
                    runtimeSession->SetEditorStatus("Prefab instance subtree unpacked");
                    Tracenf("[PREFAB] unpacked instance subtree root=%u root_type=%d prefab=%s entities=%zu",
                        root->second,
                        static_cast<int>(root->first),
                        firstPrefab.c_str(),
                        unpacked);
                    return true;
                };
                auto addEditorNoteComponentToSelectedMesh = [&]() {
                    if (selectedEditorObject.type != SelectedEditorObjectType::MeshEntity)
                        return false;
                    auto it = std::find_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                        [&](const MeshSceneEntity& mesh) { return mesh.id == selectedEditorObject.id; });
                    if (it == editorMeshEntities.end())
                        return false;
                    const auto existing = std::find_if(it->editorComponents.begin(), it->editorComponents.end(),
                        [](const EditorAttachedComponent& component) { return component.type == "editor.note"; });
                    if (existing != it->editorComponents.end())
                        return false;
                    EditorAttachedComponent component{};
                    component.type = "editor.note";
                    component.displayName = "Note";
                    component.category = "Editor";
                    component.note = "New note";
                    it->editorComponents.push_back(component);
                    if (selectedEditorObject.flecsEntity != 0)
                        ecs_add_id(editorHierarchyWorld.get(), static_cast<ecs_entity_t>(selectedEditorObject.flecsEntity), editorNoteComponentEntity);
                    SceneManager::Instance().MarkDirty();
                    Tracenf("[INSPECTOR-COMP] add entity=%u component=Note", it->id);
                    return true;
                };
                auto addLodComponentToSelectedMesh = [&]() {
                    if (selectedEditorObject.type != SelectedEditorObjectType::MeshEntity)
                        return false;
                    auto it = std::find_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                        [&](const MeshSceneEntity& mesh) { return mesh.id == selectedEditorObject.id; });
                    if (it == editorMeshEntities.end())
                        return false;
                    const auto existing = std::find_if(it->editorComponents.begin(), it->editorComponents.end(),
                        [](const EditorAttachedComponent& component) { return component.type == "rendering.lod"; });
                    if (existing == it->editorComponents.end())
                        it->editorComponents.push_back({"rendering.lod", "LOD Group", "Rendering", {}});
                    const std::optional<LodConfig> assetDefault = editorImGui.FindModelLodDefault(it->meshAssetId);
                    it->lod.enabled = true;
                    it->lod.overrideAssetDefault = false;
                    it->lod.config = assetDefault.value_or(LodConfig{});
                    SceneManager::Instance().MarkDirty();
                    Tracenf("[INSPECTOR-COMP] add entity=%u component=LOD Group", it->id);
                    if (LodLogsEnabled())
                    {
                        Tracenf("[LOD] component added entity=%u asset=%s source=%s",
                            it->id,
                            it->meshAssetId.empty() ? it->meshAssetPath.c_str() : it->meshAssetId.c_str(),
                            assetDefault ? "assetDefault" : "engineDefault");
                    }
                    return true;
                };
                auto removeEditorComponentFromSelectedMesh = [&](const std::string& componentType) {
                    if (selectedEditorObject.type != SelectedEditorObjectType::MeshEntity)
                        return false;
                    auto it = std::find_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                        [&](const MeshSceneEntity& mesh) { return mesh.id == selectedEditorObject.id; });
                    if (it == editorMeshEntities.end())
                        return false;
                    if (componentType == "physics.rigidbody")
                    {
                        if (!it->hasRigidbody)
                            return false;
                        it->hasRigidbody = false;
                        it->rigidbody = {};
                        SceneManager::Instance().MarkDirty();
                        Tracenf("[INSPECTOR-COMP] remove entity=%u component=Rigidbody", it->id);
                        return true;
                    }
                    if (componentType == "physics.collider" ||
                        componentType == "physics.box_collider" ||
                        componentType == "physics.sphere_collider" ||
                        componentType == "physics.capsule_collider")
                    {
                        if (!it->hasCollider)
                            return false;
                        it->hasCollider = false;
                        it->collider = {};
                        SceneManager::Instance().MarkDirty();
                        Tracenf("[INSPECTOR-COMP] remove entity=%u component=Collider", it->id);
                        return true;
                    }
                    if (componentType == "physics.fixed_joint")
                    {
                        if (!it->hasFixedJoint)
                            return false;
                        it->hasFixedJoint = false;
                        it->fixedJoint = {};
                        SceneManager::Instance().MarkDirty();
                        Tracenf("[INSPECTOR-COMP] remove entity=%u component=Fixed Joint", it->id);
                        return true;
                    }
                    if (componentType == "physics.hinge_joint")
                    {
                        if (!it->hasHingeJoint)
                            return false;
                        it->hasHingeJoint = false;
                        it->hingeJoint = {};
                        SceneManager::Instance().MarkDirty();
                        Tracenf("[INSPECTOR-COMP] remove entity=%u component=Hinge Joint", it->id);
                        return true;
                    }
                    if (componentType == "physics.character_controller")
                    {
                        if (!it->hasCharacterController)
                            return false;
                        it->hasCharacterController = false;
                        it->characterController = {};
                        SceneManager::Instance().MarkDirty();
                        Tracenf("[INSPECTOR-COMP] remove entity=%u component=Character Controller", it->id);
                        return true;
                    }
                    const std::size_t oldSize = it->editorComponents.size();
                    it->editorComponents.erase(std::remove_if(it->editorComponents.begin(), it->editorComponents.end(),
                        [&](const EditorAttachedComponent& component) { return component.type == componentType; }),
                        it->editorComponents.end());
                    if (it->editorComponents.size() == oldSize)
                        return false;
                    if (componentType == "editor.note" && selectedEditorObject.flecsEntity != 0)
                        ecs_remove_id(editorHierarchyWorld.get(), static_cast<ecs_entity_t>(selectedEditorObject.flecsEntity), editorNoteComponentEntity);
                    if (componentType == "rendering.lod")
                        it->lod = {};
                    SceneManager::Instance().MarkDirty();
                    Tracenf("[INSPECTOR-COMP] remove entity=%u component=%s",
                        it->id,
                        componentType == "editor.note" ? "Note" : (componentType == "rendering.lod" ? "LOD Group" : componentType.c_str()));
                    return true;
                };
                if (commands.addComponentToSelectedEntity)
                {
                    if (commands.addComponentTypeId == "editor.note")
                    {
                        if (addEditorNoteComponentToSelectedMesh())
                            runtimeSession->SetEditorStatus("Added Note component");
                    }
                    else if (commands.addComponentTypeId == "rendering.lod")
                    {
                        if (addLodComponentToSelectedMesh())
                            runtimeSession->SetEditorStatus("Added LOD Group component");
                    }
                    else if (commands.addComponentType == EditorComponentType::Rigidbody ||
                        commands.addComponentType == EditorComponentType::BoxCollider ||
                        commands.addComponentType == EditorComponentType::SphereCollider ||
                        commands.addComponentType == EditorComponentType::CapsuleCollider ||
                        commands.addComponentType == EditorComponentType::TriggerBox ||
                        commands.addComponentType == EditorComponentType::TriggerSphere ||
                        commands.addComponentType == EditorComponentType::TriggerCapsule ||
                        commands.addComponentType == EditorComponentType::FixedJoint ||
                        commands.addComponentType == EditorComponentType::HingeJoint ||
                        commands.addComponentType == EditorComponentType::CharacterController)
                    {
                        if (selectedEditorObject.type == SelectedEditorObjectType::MeshEntity)
                        {
                            auto it = std::find_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                                [&](const MeshSceneEntity& mesh) { return mesh.id == selectedEditorObject.id; });
                            if (it != editorMeshEntities.end())
                            {
                                if (commands.addComponentType == EditorComponentType::FixedJoint)
                                {
                                    it->hasFixedJoint = true;
                                    it->fixedJoint = {};
                                    if (!it->hasRigidbody)
                                    {
                                        it->hasRigidbody = true;
                                        it->rigidbody = {};
                                    }
                                    if (!it->hasCollider)
                                    {
                                        it->hasCollider = true;
                                        it->collider.shape = ixtreeme::physics::ColliderShape::Box;
                                        fitMeshColliderToBounds(*it);
                                    }
                                    Tracenf("[INSPECTOR-COMP] add entity=%u component=Fixed Joint", it->id);
                                    runtimeSession->SetEditorStatus("Added Fixed Joint component");
                                }
                                else if (commands.addComponentType == EditorComponentType::HingeJoint)
                                {
                                    it->hasHingeJoint = true;
                                    it->hingeJoint = {};
                                    it->hingeJoint.anchor[0] = it->position[0];
                                    it->hingeJoint.anchor[1] = it->position[1];
                                    it->hingeJoint.anchor[2] = it->position[2];
                                    if (!it->hasRigidbody)
                                    {
                                        it->hasRigidbody = true;
                                        it->rigidbody = {};
                                    }
                                    if (!it->hasCollider)
                                    {
                                        it->hasCollider = true;
                                        it->collider.shape = ixtreeme::physics::ColliderShape::Box;
                                        fitMeshColliderToBounds(*it);
                                    }
                                    Tracenf("[INSPECTOR-COMP] add entity=%u component=Hinge Joint", it->id);
                                    runtimeSession->SetEditorStatus("Added Hinge Joint component");
                                }
                                else if (commands.addComponentType == EditorComponentType::Rigidbody)
                                {
                                    it->hasRigidbody = true;
                                    it->rigidbody = {};
                                    if (!it->hasCollider)
                                    {
                                        it->hasCollider = true;
                                        it->collider.shape = ixtreeme::physics::ColliderShape::Box;
                                        fitMeshColliderToBounds(*it);
                                    }
                                    Tracenf("[INSPECTOR-COMP] add entity=%u component=Rigidbody", it->id);
                                    runtimeSession->SetEditorStatus("Added Rigidbody component");
                                }
                                else if (commands.addComponentType == EditorComponentType::CharacterController)
                                {
                                    it->hasCharacterController = true;
                                    it->characterController = {};
                                    Tracenf("[INSPECTOR-COMP] add entity=%u component=Character Controller", it->id);
                                    runtimeSession->SetEditorStatus("Added Character Controller component");
                                }
                                else
                                {
                                    it->hasCollider = true;
                                    const bool isTriggerPreset =
                                        commands.addComponentType == EditorComponentType::TriggerBox ||
                                        commands.addComponentType == EditorComponentType::TriggerSphere ||
                                        commands.addComponentType == EditorComponentType::TriggerCapsule;
                                    if (commands.addComponentType == EditorComponentType::SphereCollider ||
                                        commands.addComponentType == EditorComponentType::TriggerSphere)
                                    {
                                        it->collider.shape = ixtreeme::physics::ColliderShape::Sphere;
                                    }
                                    else if (commands.addComponentType == EditorComponentType::CapsuleCollider ||
                                        commands.addComponentType == EditorComponentType::TriggerCapsule)
                                    {
                                        it->collider.shape = ixtreeme::physics::ColliderShape::Capsule;
                                    }
                                    else
                                    {
                                        it->collider.shape = ixtreeme::physics::ColliderShape::Box;
                                    }
                                    if (isTriggerPreset)
                                    {
                                        it->collider.trigger = true;
                                        it->collider.layer = ixtreeme::physics::PhysicsLayer::Trigger;
                                    }
                                    fitMeshColliderToBounds(*it);
                                    const char* colliderShapeName = ixtreeme::physics::ToString(it->collider.shape);
                                    Tracenf("[INSPECTOR-COMP] add entity=%u component=%s%s_collider",
                                        it->id,
                                        isTriggerPreset ? "trigger_" : "",
                                        colliderShapeName);
                                    runtimeSession->SetEditorStatus(isTriggerPreset ? "Added Trigger Collider component" : "Added Collider component");
                                }
                                SceneManager::Instance().MarkDirty();
                            }
                        }
                    }
                    else
                    {
                    const WorldVec3 spawn = selectedEntityPosition();
                    if (commands.addComponentType == EditorComponentType::WaterBody &&
                        selectedEditorObject.type != SelectedEditorObjectType::WaterBody)
                    {
                        if (!terrainOk || !terrain.HasTerrain())
                        {
                            runtimeSession->SetEditorStatus("Create a terrain before adding water");
                        }
                        else
                        {
                        WaterBody body{};
                        body.id = nextEditorWaterBodyId++;
                        body.name = makeUniqueSceneEntityName("Water Body");
                        body.materialId = "watermat_Default_Water";
                        body.waterLevelY = spawn.y;
                        body.bboxMin[0] = spawn.x - 5.0f;
                        body.bboxMax[0] = spawn.x + 5.0f;
                        body.bboxMin[1] = spawn.z - 5.0f;
                        body.bboxMax[1] = spawn.z + 5.0f;
                        RegenerateCircularWaterMask(body);
                        editorWaterBodies.push_back(body);
                        selectedEditorObject = {SelectedEditorObjectType::WaterBody, body.id};
                        editorGizmoMode = EditorGizmoMode::Translate;
                        editorWaterBodiesDirty = true;
                        SceneManager::Instance().MarkDirty();
                        runtimeSession->SetEditorStatus("Added Water Body component");
                        }
                    }
                    else if (commands.addComponentType == EditorComponentType::PointLight &&
                        selectedEditorObject.type != SelectedEditorObjectType::PointLight)
                    {
                        if (editorPointLights.size() >= kMaxDynamicPointLights)
                        {
                            runtimeSession->SetEditorStatus("Maximum point lights reached (16)");
                        }
                        else
                        {
                            PointLight light{};
                            light.id = nextEditorLightId++;
                            light.name = makeUniqueSceneEntityName("Point Light");
                            light.position[0] = spawn.x;
                            light.position[1] = spawn.y + 1.8f;
                            light.position[2] = spawn.z;
                            editorPointLights.push_back(light);
                            selectedEditorObject = {SelectedEditorObjectType::PointLight, light.id};
                            editorGizmoMode = EditorGizmoMode::Translate;
                            SceneManager::Instance().MarkDirty();
                            runtimeSession->SetEditorStatus("Added Point Light component");
                        }
                    }
                    else if (commands.addComponentType == EditorComponentType::SpotLight &&
                        selectedEditorObject.type != SelectedEditorObjectType::SpotLight)
                    {
                        if (editorSpotLights.size() >= kMaxDynamicSpotLights)
                        {
                            runtimeSession->SetEditorStatus("Maximum spot lights reached (16)");
                        }
                        else
                        {
                            SpotLight light{};
                            light.id = nextEditorLightId++;
                            light.name = makeUniqueSceneEntityName("Spot Light");
                            light.position[0] = spawn.x;
                            light.position[1] = spawn.y + 4.0f;
                            light.position[2] = spawn.z;
                            light.rotation[0] = -1.5708f;
                            editorSpotLights.push_back(light);
                            selectedEditorObject = {SelectedEditorObjectType::SpotLight, light.id};
                            editorGizmoMode = EditorGizmoMode::Translate;
                            SceneManager::Instance().MarkDirty();
                            runtimeSession->SetEditorStatus("Added Spot Light component");
                        }
                    }
                    else if (commands.addComponentType == EditorComponentType::MeshRenderer &&
                        selectedEditorObject.type != SelectedEditorObjectType::MeshEntity)
                    {
                        createMeshEntityAt(commands.assignMeshAssetId.empty() ? commands.meshAssetId : commands.assignMeshAssetId, spawn);
                    }
                    }
                }
                if (commands.removeComponentFromSelectedEntity)
                {
                    if (commands.removeComponentTypeId == "builtin.transform")
                    {
                        Tracen("[INSPECTOR-COMP] remove ignored component=Transform reason=not-removable");
                    }
                    else if (removeEditorComponentFromSelectedMesh(commands.removeComponentTypeId))
                    {
                        runtimeSession->SetEditorStatus("Removed component");
                    }
                }
                if (commands.createPrefabFromSelection)
                    createPrefabFromSelection();
                if (commands.refreshSelectedPrefabInstance)
                    refreshSelectedPrefabInstance();
                if (commands.refreshAllPrefabInstances)
                    refreshAllPrefabInstances();
                if (commands.revertSelectedPrefabInstance)
                    revertSelectedPrefabInstance();
                if (commands.revertSelectedPrefabOverride && !revertSelectedPrefabOverride(commands.selectedPrefabOverrideName))
                    runtimeSession->SetEditorStatus("Prefab override cannot be reverted: " + commands.selectedPrefabOverrideName);
                if (commands.applySelectedPrefabOverrideToAsset && !applySelectedPrefabOverrideToAsset(commands.selectedPrefabOverrideName))
                    runtimeSession->SetEditorStatus("Prefab override cannot be applied: " + commands.selectedPrefabOverrideName);
                if (commands.applySelectedPrefabToAsset && !applySelectedPrefabToAsset())
                    runtimeSession->SetEditorStatus("Selected entity is not an applicable prefab instance");
                if (commands.unpackSelectedPrefabInstance && !unpackSelectedPrefabInstance())
                    runtimeSession->SetEditorStatus("Selected entity is not a prefab instance");
                if (commands.savePrefabAssetEdit)
                {
                    if (savePrefabAssetEdit(commands.editPrefabAssetId, commands.editPrefabName, commands.editPrefabEntityNames))
                        refreshPrefabInstancesForAsset(commands.editPrefabAssetId);
                    else
                        runtimeSession->SetEditorStatus("Prefab asset save failed");
                }
                if (commands.addPrefabInstance)
                {
                    const WorldVec3 spawn = commands.prefabDropScreenPositionValid
                        ? spawnAtScreenPosition(commands.prefabDropScreenPosition[0], commands.prefabDropScreenPosition[1])
                        : spawnAtCameraCenter();
                    instantiatePrefabAt(commands.prefabAssetId, spawn);
                }
                if (commands.addMeshEntity)
                {
                    const WorldVec3 spawn = commands.meshDropScreenPositionValid
                        ? spawnAtScreenPosition(commands.meshDropScreenPosition[0], commands.meshDropScreenPosition[1])
                        : spawnAtCameraCenter();
                    if (commands.meshDropScreenPositionValid)
                    {
                        Tracenf("[DND] drop -> spawn at raycast pos=(%.2f,%.2f,%.2f)",
                            spawn.x,
                            spawn.y,
                            spawn.z);
                    }
                    createMeshEntityAt(commands.meshAssetId, spawn);
                }
                if (commands.addPrimitiveEntity)
                {
                    const std::string primitivePath = "builtin://primitive/" + commands.primitiveType;
                    const WorldVec3 spawn = spawnAtCameraCenter();
                    createMeshEntityAt(primitivePath, spawn);
                    runtimeSession->SetEditorStatus("Primitive created: " + commands.primitiveType);
                    Tracenf("[PRIMITIVE] create requested type=%s path=%s position=(%.2f,%.2f,%.2f)",
                        commands.primitiveType.c_str(),
                        primitivePath.c_str(),
                        spawn.x,
                        spawn.y,
                        spawn.z);
                }
                if (commands.createTerrain && terrainOk)
                {
                    TerrainSceneData next = NormalizeTerrainCreateRequest(commands.terrainCreate);
                    if (next.name.empty())
                        next.name = makeUniqueSceneEntityName("Terrain");
                    if (terrain.CreateFlatTerrain(device, next))
                    {
                        editorWaterBodies.clear();
                        editorWaterBodiesDirty = true;
                        selectedEditorObject = {SelectedEditorObjectType::Terrain, 1u};
                        editorGizmoMode = EditorGizmoMode::Translate;
                        SceneManager::Instance().MarkDirty();
                        runtimeSession->SetEditorStatus("Terrain created");
                    }
                    else
                    {
                        runtimeSession->SetEditorStatus("Terrain creation failed");
                    }
                }
                if (commands.addWaterBody)
                {
                    if (!terrainOk || !terrain.HasTerrain())
                    {
                        runtimeSession->SetEditorStatus("Create a terrain before adding water");
                    }
                    else
                    {
                    const WorldVec3 spawn = spawnAtCameraCenter();
                    WaterBody body{};
                    body.id = nextEditorWaterBodyId++;
                    body.name = makeUniqueSceneEntityName("Water Body");
                    body.materialId = "watermat_Default_Water";
                    body.waterLevelY = spawn.y;
                    body.bboxMin[0] = spawn.x - 5.0f;
                    body.bboxMax[0] = spawn.x + 5.0f;
                    body.bboxMin[1] = spawn.z - 5.0f;
                    body.bboxMax[1] = spawn.z + 5.0f;
                    RegenerateCircularWaterMask(body);
                    editorWaterBodies.push_back(body);
                    selectedEditorObject = {SelectedEditorObjectType::WaterBody, body.id};
                    editorGizmoMode = EditorGizmoMode::Translate;
                    editorWaterBodiesDirty = true;
                    SceneManager::Instance().MarkDirty();
                    runtimeSession->SetEditorStatus("Water body spawned: id=" + std::to_string(body.id) +
                        " name=" + body.name);
                    Tracenf("[EDITOR-3D-SPAWN] Spawn at cursor: type=water position=(%.2f,%.2f,%.2f)",
                        spawn.x, spawn.y, spawn.z);
                    }
                }
                if (commands.addPointLight)
                {
                    if (editorPointLights.size() >= kMaxDynamicPointLights)
                    {
                        runtimeSession->SetEditorStatus("Maximum point lights reached (16)");
                    }
                    else
                    {
                        PointLight light{};
                        light.id = nextEditorLightId++;
                        light.name = makeUniqueSceneEntityName("Point Light");
                        const WorldVec3 spawn = spawnAtCameraCenter();
                        light.position[0] = spawn.x;
                        light.position[1] = spawn.y + 1.8f;
                        light.position[2] = spawn.z;
                        editorPointLights.push_back(light);
                        selectedEditorObject = {SelectedEditorObjectType::PointLight, light.id};
                        editorGizmoMode = EditorGizmoMode::Translate;
                        SceneManager::Instance().MarkDirty();
                        runtimeSession->SetEditorStatus("Added point light #" + std::to_string(light.id));
                        Tracenf("[EDITOR-3D-SPAWN] Spawn at cursor: type=point_light position=(%.2f,%.2f,%.2f)",
                            spawn.x, spawn.y, spawn.z);
                    }
                }
                if (commands.addSpotLight)
                {
                    if (editorSpotLights.size() >= kMaxDynamicSpotLights)
                    {
                        runtimeSession->SetEditorStatus("Maximum spot lights reached (16)");
                    }
                    else
                    {
                        SpotLight light{};
                        light.id = nextEditorLightId++;
                        light.name = makeUniqueSceneEntityName("Spot Light");
                        const WorldVec3 spawn = spawnAtCameraCenter();
                        light.position[0] = spawn.x;
                        light.position[1] = spawn.y + 4.0f;
                        light.position[2] = spawn.z;
                        light.rotation[0] = -1.5708f;
                        editorSpotLights.push_back(light);
                        selectedEditorObject = {SelectedEditorObjectType::SpotLight, light.id};
                        editorGizmoMode = EditorGizmoMode::Translate;
                        SceneManager::Instance().MarkDirty();
                        runtimeSession->SetEditorStatus("Added spot light #" + std::to_string(light.id));
                        Tracenf("[EDITOR-3D-SPAWN] Spawn at cursor: type=spot_light position=(%.2f,%.2f,%.2f)",
                            spawn.x, spawn.y, spawn.z);
                    }
                }
                if (commands.selectedLightChanged)
                {
                    if (commands.selectedLight.type == DynamicLightType::Point)
                    {
                        auto it = std::find_if(editorPointLights.begin(), editorPointLights.end(),
                            [&](const PointLight& light) { return light.id == commands.selectedLight.point.id; });
                        if (it != editorPointLights.end())
                        {
                            *it = commands.selectedLight.point;
                            selectedEditorObject = {SelectedEditorObjectType::PointLight, it->id};
                            SceneManager::Instance().MarkDirty();
                        }
                    }
                    else if (commands.selectedLight.type == DynamicLightType::Spot)
                    {
                        auto it = std::find_if(editorSpotLights.begin(), editorSpotLights.end(),
                            [&](const SpotLight& light) { return light.id == commands.selectedLight.spot.id; });
                        if (it != editorSpotLights.end())
                        {
                            SpotLight spot = commands.selectedLight.spot;
                            spot.outerConeDegrees = std::clamp(spot.outerConeDegrees, 1.0f, 90.0f);
                            spot.innerConeDegrees = std::clamp(spot.innerConeDegrees, 1.0f, spot.outerConeDegrees);
                            *it = spot;
                            selectedEditorObject = {SelectedEditorObjectType::SpotLight, it->id};
                            SceneManager::Instance().MarkDirty();
                        }
                    }
                }
                if (commands.selectedCameraChanged)
                {
                    auto it = std::find_if(editorCameras.begin(), editorCameras.end(),
                        [&](const CameraEntity& camera) { return camera.id == commands.selectedCamera.id; });
                    if (it != editorCameras.end())
                    {
                        *it = commands.selectedCamera;
                        selectedEditorObject = {SelectedEditorObjectType::Camera, it->id};
                        SceneManager::Instance().MarkDirty();
                    }
                }
                if (commands.setMainCameraRequested)
                {
                    auto it = std::find_if(editorCameras.begin(), editorCameras.end(),
                        [&](const CameraEntity& camera) { return camera.id == commands.setMainCameraId; });
                    if (it != editorCameras.end())
                    {
                        editorMainCameraId = it->id;
                        SceneManager::Instance().MarkDirty();
                        runtimeSession->SetEditorStatus("Set Main Camera #" + std::to_string(it->id));
                    }
                }
                if (commands.selectedMeshEntityChanged)
                    applyPendingSelectedMeshEntityChange();
                if (commands.dumpMaterialState)
                {
                    Tracenf("[MATBIND-DIAG] dump requested meshEntities=%zu", editorMeshEntities.size());
                    for (const MeshSceneEntity& mesh : editorMeshEntities)
                    {
                        const std::string runtimePath = resolveMeshRuntimePath(mesh);
                        if (mesh.skinned)
                        {
                            SkinnedMeshRenderer* skinnedRenderer = getSkinnedMeshRenderer(runtimePath);
                            const bool loaded = skinnedRenderer != nullptr;
                            const std::uint32_t submeshCount = loaded
                                ? skinnedRenderer->MaterialSlotCount()
                                : std::max<std::uint32_t>(1u, ResolveModelSubmeshCount(mesh.meshAssetPath));
                            std::vector<std::string> slots = mesh.materialSlots;
                            if (slots.empty())
                                slots = LoadDefaultMaterialSlotGuids(mesh.meshAssetPath, submeshCount);
                            if (slots.size() < submeshCount)
                                slots.resize(submeshCount);

                            Tracenf("[SKELETAL-DIAG] === entity name=%s entityId=%u path=%s ===",
                                mesh.name.c_str(),
                                mesh.id,
                                runtimePath.c_str());
                            Tracenf("[SKELETAL-DIAG]   submeshCount=%u materialsVectorSize=%zu skinnedRendererLoaded=%s",
                                submeshCount,
                                slots.size(),
                                loaded ? "yes" : "no");
                            const std::string emptySlotGuid;
                            for (std::uint32_t slot = 0; slot < submeshCount; ++slot)
                            {
                                const std::string& guidText = slot < slots.size() ? slots[slot] : emptySlotGuid;
                                std::string resolvedMaterial = "(empty)";
                                if (!guidText.empty())
                                {
                                    if (const std::optional<Guid> guid = Guid::fromString(guidText))
                                    {
                                        if (const std::optional<std::filesystem::path> path =
                                                AssetDatabase::Instance().resolveGuid(*guid))
                                            resolvedMaterial = path->generic_string();
                                        else
                                            resolvedMaterial = "(missing material)";
                                    }
                                    else
                                    {
                                        resolvedMaterial = "(invalid material guid)";
                                    }
                                }
                                Tracenf("[SKELETAL-DIAG]   submesh=%u slotGuid=%s resolvedMaterial=%s usedPipeline=skeletal_lit_opaque boneMatricesUploaded=%s drawCallIssued=%s",
                                    slot,
                                    guidText.empty() ? "(empty)" : guidText.c_str(),
                                    resolvedMaterial.c_str(),
                                    loaded ? "yes" : "no",
                                    loaded && slot < submeshCount ? "yes" : "no");
                            }
                            continue;
                        }
                        StaticMeshRenderer* renderer = getStaticMeshRenderer(runtimePath);
                        if (!renderer)
                        {
                            Tracenf("[MATBIND-DIAG] === entity name=%s entityId=%u renderer=NULL path=%s ===",
                                mesh.name.c_str(),
                                mesh.id,
                                runtimePath.c_str());
                            continue;
                        }
                        StaticMeshRenderer::Instance instance{};
                        instance.entityId = mesh.id;
                        instance.position = {mesh.position[0], mesh.position[1], mesh.position[2]};
                        instance.rotation[0] = mesh.rotation[0];
                        instance.rotation[1] = mesh.rotation[1];
                        instance.rotation[2] = mesh.rotation[2];
                        instance.scale[0] = mesh.scale[0];
                        instance.scale[1] = mesh.scale[1];
                        instance.scale[2] = mesh.scale[2];
                        instance.materialSlots = mesh.materialSlots;
                        instance.materialOverrides = mesh.materialOverrides;
                        renderer->DumpMaterialState(mesh.name.c_str(), instance);
                    }
                }
                if (commands.lodQualityCommitRequested)
                {
                    MeshSceneEntity* mesh = findMeshEntityById(commands.lodQualityCommitEntityId);
                    if (mesh && mesh->lod.enabled)
                    {
                        LodConfig qualityConfig = commands.lodQualityCommitConfig;
                        qualityConfig.levelCount = std::clamp(qualityConfig.levelCount, 1u, LodConfig::MaxLevels);
                        qualityConfig.targetRatios[0] = 1.0f;
                        qualityConfig.distances[0] = 0.0f;
                        const std::uint64_t qualityHash = HashLodConfig(qualityConfig);
                        if (StaticMeshRenderer* renderer = getStaticMeshRenderer(resolveMeshRuntimePath(*mesh)))
                            renderer->RequestLodQualityBuild(qualityConfig, qualityHash, mesh->id);
                    }
                }
                if (commands.deleteSelectedLight)
                {
                    if (selectedEditorObject.type == SelectedEditorObjectType::PointLight)
                    {
                        editorPointLights.erase(std::remove_if(editorPointLights.begin(), editorPointLights.end(),
                            [&](const PointLight& light) { return light.id == selectedEditorObject.id; }), editorPointLights.end());
                        runtimeSession->SetEditorStatus("Deleted point light #" + std::to_string(selectedEditorObject.id));
                        selectedEditorObject = {};
                        SceneManager::Instance().MarkDirty();
                    }
                    else if (selectedEditorObject.type == SelectedEditorObjectType::SpotLight)
                    {
                        editorSpotLights.erase(std::remove_if(editorSpotLights.begin(), editorSpotLights.end(),
                            [&](const SpotLight& light) { return light.id == selectedEditorObject.id; }), editorSpotLights.end());
                        runtimeSession->SetEditorStatus("Deleted spot light #" + std::to_string(selectedEditorObject.id));
                        selectedEditorObject = {};
                        SceneManager::Instance().MarkDirty();
                    }
                }
                if (commands.deleteSelectedMeshEntity &&
                    selectedEditorObject.type == SelectedEditorObjectType::MeshEntity)
                {
                    removeStaticMeshSpatialEntity(selectedEditorObject.id);
                    editorMeshEntities.erase(std::remove_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                        [&](const MeshSceneEntity& mesh) { return mesh.id == selectedEditorObject.id; }), editorMeshEntities.end());
                    rebuildMeshEntityLookup();
                    runtimeSession->SetEditorStatus("Mesh entity deleted: id=" + std::to_string(selectedEditorObject.id));
                    selectedEditorObject = {};
                    SceneManager::Instance().MarkDirty();
                }
                if (commands.selectedWaterBodyChanged)
                {
                    auto it = std::find_if(editorWaterBodies.begin(), editorWaterBodies.end(),
                        [&](const WaterBody& body) { return body.id == commands.selectedWaterBody.id; });
                    if (it != editorWaterBodies.end())
                    {
                        ApplyWaterBodyEditorStateToBody(*it, commands.selectedWaterBody);
                        selectedEditorObject = {SelectedEditorObjectType::WaterBody, it->id};
                        editorWaterBodiesDirty = true;
                        SceneManager::Instance().MarkDirty();
                    }
                }
                if (commands.openSelectedWaterMaterialEditor)
                {
                    const std::string materialId = commands.selectedWaterBody.materialId.empty()
                        ? std::string("watermat_Default_Water")
                        : commands.selectedWaterBody.materialId;
                    if (editorImGui.OpenWaterMaterialEditor(materialId))
                        runtimeSession->SetEditorStatus("Editing water material: " + materialId);
                }
                if (commands.waterMaterialDeleted)
                {
                    for (WaterBody& body : editorWaterBodies)
                    {
                        if (body.materialId == commands.deletedWaterMaterialId)
                        {
                            body.materialId = "watermat_Default_Water";
                            editorWaterBodiesDirty = true;
                            SceneManager::Instance().MarkDirty();
                        }
                    }
                    if (selectedEditorObject.type == SelectedEditorObjectType::WaterBody)
                    {
                        auto selectedIt = std::find_if(editorWaterBodies.begin(), editorWaterBodies.end(),
                            [&](const WaterBody& body) { return body.id == selectedEditorObject.id; });
                        if (selectedIt != editorWaterBodies.end() &&
                            selectedIt->materialId == "watermat_Default_Water")
                        {
                            runtimeSession->SetEditorStatus("Deleted material replaced with default on selected water body");
                        }
                    }
                }
                if (commands.deleteSelectedWaterBody &&
                    selectedEditorObject.type == SelectedEditorObjectType::WaterBody)
                {
                    editorWaterBodies.erase(std::remove_if(editorWaterBodies.begin(), editorWaterBodies.end(),
                        [&](const WaterBody& body) { return body.id == selectedEditorObject.id; }), editorWaterBodies.end());
                    runtimeSession->SetEditorStatus("Water body deleted: id=" + std::to_string(selectedEditorObject.id));
                    selectedEditorObject = {};
                    editorWaterBodiesDirty = true;
                    SceneManager::Instance().MarkDirty();
                }

                logStaticMeshSpatialMutations();

                auto floatDiffers = [](float a, float b, float epsilon = 0.0005f) {
                    return std::fabs(a - b) > epsilon;
                };
                auto floatArrayDiffers = [&](const float* a, const float* b, std::size_t count, float epsilon = 0.0005f) {
                    for (std::size_t i = 0; i < count; ++i)
                    {
                        if (floatDiffers(a[i], b[i], epsilon))
                            return true;
                    }
                    return false;
                };
                auto prefabAssetIdFor = [](const auto& entity) -> std::string {
                    return !entity.prefabInstance.assetId.empty() ? entity.prefabInstance.assetId : entity.prefabAssetId;
                };
                auto prefabLocalIdFor = [](const auto& entity) {
                    return entity.prefabInstance.localId == 0 ? 1u : entity.prefabInstance.localId;
                };
                auto findPrefabEntityForInstance = [&](const std::string& assetId,
                                                       std::uint32_t localId,
                                                       prefab::PrefabTemplate::Kind kind) -> std::optional<prefab::PrefabEntity> {
                    const auto loaded = loadPrefabDocument(assetId);
                    if (!loaded)
                        return std::nullopt;
                    auto entityIt = std::find_if(loaded->first.entities.begin(), loaded->first.entities.end(),
                        [&](const prefab::PrefabEntity& entity) {
                            return entity.localId == localId && entity.kind == kind;
                        });
                    if (entityIt == loaded->first.entities.end())
                        return std::nullopt;
                    return *entityIt;
                };
                auto meshPrefabOverrides = [&](const MeshSceneEntity& mesh) {
                    std::vector<std::string> overrides;
                    const std::string assetId = prefabAssetIdFor(mesh);
                    if (assetId.empty())
                        return overrides;
                    const auto loaded = findPrefabEntityForInstance(assetId, prefabLocalIdFor(mesh), prefab::PrefabTemplate::Kind::Mesh);
                    if (!loaded)
                    {
                        overrides.push_back("Prefab asset missing");
                        return overrides;
                    }
                    const MeshSceneEntity& source = loaded->mesh;
                    const std::string sourceName = loaded->name.empty() ? source.name : loaded->name;
                    if (mesh.name != sourceName)
                        overrides.push_back("Name");
                    if (floatArrayDiffers(mesh.position, source.position, 3))
                        overrides.push_back("Position");
                    if (floatArrayDiffers(mesh.rotation, source.rotation, 3))
                        overrides.push_back("Rotation");
                    if (floatArrayDiffers(mesh.scale, source.scale, 3))
                        overrides.push_back("Scale");
                    if (mesh.meshAssetId != source.meshAssetId || mesh.meshAssetPath != source.meshAssetPath || mesh.skinned != source.skinned)
                        overrides.push_back("Mesh asset");
                    if (!source.materialSlots.empty() && mesh.materialSlots != source.materialSlots)
                        overrides.push_back("Material slots");
                    if (!mesh.materialOverrides.empty())
                        overrides.push_back("Material overrides");
                    return overrides;
                };
                auto pointPrefabOverrides = [&](const PointLight& light) {
                    std::vector<std::string> overrides;
                    const std::string assetId = prefabAssetIdFor(light);
                    if (assetId.empty())
                        return overrides;
                    const auto loaded = findPrefabEntityForInstance(assetId, prefabLocalIdFor(light), prefab::PrefabTemplate::Kind::PointLight);
                    if (!loaded)
                    {
                        overrides.push_back("Prefab asset missing");
                        return overrides;
                    }
                    const PointLight& source = loaded->point;
                    const std::string sourceName = loaded->name.empty() ? EditorDisplayName(source) : loaded->name;
                    if (EditorDisplayName(light) != sourceName)
                        overrides.push_back("Name");
                    if (floatArrayDiffers(light.position, source.position, 3))
                        overrides.push_back("Position");
                    if (floatDiffers(light.r, source.r) || floatDiffers(light.g, source.g) || floatDiffers(light.b, source.b))
                        overrides.push_back("Color");
                    if (floatDiffers(light.intensity, source.intensity))
                        overrides.push_back("Intensity");
                    if (floatDiffers(light.radius, source.radius))
                        overrides.push_back("Radius");
                    if (light.enabled != source.enabled)
                        overrides.push_back("Enabled");
                    return overrides;
                };
                auto spotPrefabOverrides = [&](const SpotLight& light) {
                    std::vector<std::string> overrides;
                    const std::string assetId = prefabAssetIdFor(light);
                    if (assetId.empty())
                        return overrides;
                    const auto loaded = findPrefabEntityForInstance(assetId, prefabLocalIdFor(light), prefab::PrefabTemplate::Kind::SpotLight);
                    if (!loaded)
                    {
                        overrides.push_back("Prefab asset missing");
                        return overrides;
                    }
                    const SpotLight& source = loaded->spot;
                    const std::string sourceName = loaded->name.empty() ? EditorDisplayName(source) : loaded->name;
                    if (EditorDisplayName(light) != sourceName)
                        overrides.push_back("Name");
                    if (floatArrayDiffers(light.position, source.position, 3))
                        overrides.push_back("Position");
                    if (floatArrayDiffers(light.rotation, source.rotation, 3))
                        overrides.push_back("Rotation");
                    if (floatDiffers(light.r, source.r) || floatDiffers(light.g, source.g) || floatDiffers(light.b, source.b))
                        overrides.push_back("Color");
                    if (floatDiffers(light.intensity, source.intensity))
                        overrides.push_back("Intensity");
                    if (floatDiffers(light.radius, source.radius))
                        overrides.push_back("Radius");
                    if (floatDiffers(light.innerConeDegrees, source.innerConeDegrees) ||
                        floatDiffers(light.outerConeDegrees, source.outerConeDegrees))
                    {
                        overrides.push_back("Cone");
                    }
                    if (light.enabled != source.enabled)
                        overrides.push_back("Enabled");
                    return overrides;
                };

                DynamicLightEditorState dynamicLightState{};
                dynamicLightState.pointCount = static_cast<std::uint32_t>(std::min<std::size_t>(editorPointLights.size(), kMaxDynamicPointLights));
                dynamicLightState.spotCount = static_cast<std::uint32_t>(std::min<std::size_t>(editorSpotLights.size(), kMaxDynamicSpotLights));
                if (selectedEditorObject.type == SelectedEditorObjectType::PointLight)
                {
                    auto it = std::find_if(editorPointLights.begin(), editorPointLights.end(),
                        [&](const PointLight& light) { return light.id == selectedEditorObject.id; });
                    if (it != editorPointLights.end())
                    {
                        dynamicLightState.type = DynamicLightType::Point;
                        dynamicLightState.id = it->id;
                        dynamicLightState.point = *it;
                        dynamicLightState.prefabOverrides = pointPrefabOverrides(*it);
                    }
                }
                else if (selectedEditorObject.type == SelectedEditorObjectType::SpotLight)
                {
                    auto it = std::find_if(editorSpotLights.begin(), editorSpotLights.end(),
                        [&](const SpotLight& light) { return light.id == selectedEditorObject.id; });
                    if (it != editorSpotLights.end())
                    {
                        dynamicLightState.type = DynamicLightType::Spot;
                        dynamicLightState.id = it->id;
                        dynamicLightState.spot = *it;
                        dynamicLightState.prefabOverrides = spotPrefabOverrides(*it);
                    }
                }
                editorImGui.SetDynamicLightEditorState(dynamicLightState);
                CameraEditorState cameraEditorState{};
                cameraEditorState.cameraCount = static_cast<std::uint32_t>(editorCameras.size());
                if (selectedEditorObject.type == SelectedEditorObjectType::Camera)
                {
                    auto it = std::find_if(editorCameras.begin(), editorCameras.end(),
                        [&](const CameraEntity& camera) { return camera.id == selectedEditorObject.id; });
                    if (it != editorCameras.end())
                    {
                        cameraEditorState.selected = true;
                        cameraEditorState.isMain = (it->id == editorMainCameraId);
                        cameraEditorState.camera = *it;
                    }
                }
                editorImGui.SetCameraEditorState(cameraEditorState);
                WaterBodyEditorState waterBodyState = BuildWaterBodyEditorState(editorWaterBodies,
                    selectedEditorObject.type == SelectedEditorObjectType::WaterBody ? selectedEditorObject.id : 0u);
                runtimeSession->SetWaterBodyEditorState(waterBodyState);
                editorImGui.SetWaterBodyEditorState(waterBodyState);
                MeshRendererEditorState meshRendererState = BuildMeshRendererEditorState(editorMeshEntities,
                    selectedEditorObject.type == SelectedEditorObjectType::MeshEntity ? selectedEditorObject.id : 0u);
                if (meshRendererState.selected)
                {
                    if (auto entry = resolveModelAsset(meshRendererState.meshAssetId))
                        meshRendererState.meshDisplayName = entry->displayName.empty() ? entry->filename : entry->displayName;
                    auto meshIt = std::find_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                        [&](const MeshSceneEntity& mesh) { return mesh.id == meshRendererState.id; });
                    if (meshIt != editorMeshEntities.end())
                    {
                        meshRendererState.prefabOverrides = meshPrefabOverrides(*meshIt);
                        const auto physicsBodyIt = editorPhysicsBodies.find(meshRendererState.id);
                        if (editorPlay.state.mode == EditorPlayMode::Play &&
                            editorPhysicsWorldActive &&
                            physicsBodyIt != editorPhysicsBodies.end())
                        {
                            meshRendererState.physicsRuntimeValid = true;
                            meshRendererState.physicsRuntimeBodyId = physicsBodyIt->second;
                            meshRendererState.physicsRuntimeActive = editorPhysicsWorld.IsBodyActive(physicsBodyIt->second);
                            editorPhysicsWorld.GetLinearVelocity(
                                physicsBodyIt->second,
                                meshRendererState.physicsRuntimeLinearVelocity);
                            editorPhysicsWorld.GetAngularVelocity(
                                physicsBodyIt->second,
                                meshRendererState.physicsRuntimeAngularVelocity);
                        }
                        const std::string runtimePath = resolveMeshRuntimePath(*meshIt);
                        if (meshIt->skinned)
                        {
                            if (SkinnedMeshRenderer* skinnedRenderer = getSkinnedMeshRenderer(runtimePath))
                                meshRendererState.materialSlotCount = std::max<std::uint32_t>(1u, skinnedRenderer->MaterialSlotCount());
                            else
                                meshRendererState.materialSlotCount = std::max<std::uint32_t>(
                                    1u,
                                    ResolveModelSubmeshCount(meshIt->meshAssetPath));
                            if (meshRendererState.materialSlots.empty())
                                meshRendererState.materialSlots = LoadDefaultMaterialSlotGuids(
                                    meshIt->meshAssetPath,
                                    meshRendererState.materialSlotCount);
                            else if (meshRendererState.materialSlots.size() < meshRendererState.materialSlotCount)
                            {
                                if (meshRendererState.materialSlots.size() == 1)
                                    meshRendererState.materialSlots.resize(
                                        meshRendererState.materialSlotCount,
                                        meshRendererState.materialSlots.front());
                                else
                                    meshRendererState.materialSlots.resize(meshRendererState.materialSlotCount);
                            }
                        }
                        else if (StaticMeshRenderer* renderer = getStaticMeshRenderer(runtimePath))
                        {
                            meshRendererState.materialSlotCount = std::max<std::uint32_t>(1u, renderer->MaterialSlotCount());
                        }
                    }
                    meshRendererState.selectedMaterialSlot = std::min(meshRendererState.selectedMaterialSlot,
                        meshRendererState.materialSlotCount > 0 ? meshRendererState.materialSlotCount - 1u : 0u);
                }
                editorImGui.SetMeshRendererEditorState(meshRendererState);

                // Stage-6 Animator graph snapshot for the dedicated Animator window (read-only).
                // Flatten the selected entity's bound AnimatorController (auto-filled clip ids and
                // all) into display nodes/edges plus synthetic Entry / Any-State nodes. The runtime
                // map is populated one frame later by the skinned pre-pass, so a freshly-selected
                // entity shows its graph on the next frame — invisible to the eye.
                {
                    // Stage 7: apply any pending graph edits from the Animator panel to the selected
                    // entity's controller, persist the .controller, and clear the bound-controller id
                    // so the skinned pre-pass (later this frame) reloads the saved file and re-binds —
                    // the edit takes effect immediately.
                    if (!commands.animatorEdits.empty() &&
                        selectedEditorObject.type == SelectedEditorObjectType::MeshEntity)
                    {
                        auto editIt = entityControllers.find(selectedEditorObject.id);
                        if (editIt != entityControllers.end())
                        {
                            const bool needsRebind =
                                ApplyAnimatorGraphEdits(editIt->second, commands.animatorEdits);
                            const std::string ctrlPath =
                                editorImGui.AnimatorControllerFilePath(editIt->second.id);
                            if (!ctrlPath.empty())
                            {
                                std::ofstream ctrlOut(ctrlPath, std::ios::binary | std::ios::trunc);
                                ctrlOut << ixanim::ControllerToJson(editIt->second);
                            }
                            // Only behavior-changing edits force a re-bind (which resets play state);
                            // a cosmetic node move persists in-memory + on disk without rebinding.
                            if (needsRebind)
                                entityBoundControllerId[selectedEditorObject.id].clear();
                        }
                    }

                    AnimatorGraphEditorState animatorGraphState;
                    if (selectedEditorObject.type == SelectedEditorObjectType::MeshEntity)
                    {
                        auto ctrlIt = entityControllers.find(selectedEditorObject.id);
                        if (ctrlIt != entityControllers.end())
                        {
                            const ixanim::AnimatorController& ctrl = ctrlIt->second;
                            animatorGraphState.hasController = true;
                            animatorGraphState.controllerId = ctrl.id;
                            animatorGraphState.controllerDisplayName =
                                ctrl.displayName.empty() ? ctrl.id : ctrl.displayName;
                            animatorGraphState.defaultStateId = ctrl.defaultStateId;

                            bool hasAnyState = false;
                            for (const ixanim::AnimatorTransition& t : ctrl.transitions)
                                if (t.fromStateId == ixanim::kAnyStateId) { hasAnyState = true; break; }

                            // State nodes (track the default state's position to anchor Entry).
                            float defaultX = 120.0f, defaultY = 80.0f;
                            for (const ixanim::AnimatorState& s : ctrl.states)
                            {
                                AnimatorGraphNode node;
                                node.id = s.id;
                                node.name = s.name;
                                node.clipLabel = s.clipId.empty() ? "(no clip)" : s.clipId;
                                node.clipId = s.clipId;
                                node.speed = s.speed;
                                node.loop = s.loop;
                                // Stage 5: blend tree (display copy for the inspector + node label).
                                node.blendTreeType = static_cast<int>(s.blendTree.type);
                                node.blendParam = s.blendTree.blendParam;
                                node.blendParamY = s.blendTree.blendParamY;
                                for (const ixanim::BlendTreeChild& bc : s.blendTree.children)
                                {
                                    AnimatorGraphBlendChild gbc;
                                    gbc.clipId = bc.clipId;
                                    gbc.threshold = bc.threshold;
                                    gbc.pos[0] = bc.position[0];
                                    gbc.pos[1] = bc.position[1];
                                    node.blendChildren.push_back(std::move(gbc));
                                }
                                if (s.blendTree.type == ixanim::BlendTreeType::Blend1D)
                                    node.clipLabel = "1D: " + (s.blendTree.blendParam.empty() ? std::string("(param)") : s.blendTree.blendParam)
                                        + " (" + std::to_string(s.blendTree.children.size()) + ")";
                                else if (s.blendTree.type == ixanim::BlendTreeType::Blend2D)
                                    node.clipLabel = "2D (" + std::to_string(s.blendTree.children.size()) + ")";
                                node.graphPos[0] = s.graphPos[0];
                                node.graphPos[1] = s.graphPos[1];
                                node.isDefault = (s.id == ctrl.defaultStateId);
                                if (s.id == ctrl.defaultStateId)
                                {
                                    defaultX = s.graphPos[0];
                                    defaultY = s.graphPos[1];
                                }
                                animatorGraphState.nodes.push_back(std::move(node));
                            }

                            // Synthetic Entry node (id 0) left of the default state, + its edge.
                            {
                                AnimatorGraphNode entry;
                                entry.id = 0u;
                                entry.name = "Entry";
                                entry.isEntry = true;
                                entry.graphPos[0] = defaultX - 200.0f;
                                entry.graphPos[1] = defaultY;
                                animatorGraphState.nodes.push_back(std::move(entry));

                                AnimatorGraphEdge entryEdge;
                                entryEdge.fromStateId = 0u;
                                entryEdge.toStateId = ctrl.defaultStateId;
                                animatorGraphState.edges.push_back(entryEdge);
                            }

                            // Synthetic Any-State node (id kAnyStateId) above the states; its edges
                            // already carry fromStateId == kAnyStateId, so they resolve to it.
                            if (hasAnyState)
                            {
                                float minX = 120.0f, minY = 80.0f;
                                bool first = true;
                                for (const ixanim::AnimatorState& s : ctrl.states)
                                {
                                    minX = first ? s.graphPos[0] : std::min(minX, s.graphPos[0]);
                                    minY = first ? s.graphPos[1] : std::min(minY, s.graphPos[1]);
                                    first = false;
                                }
                                AnimatorGraphNode anyNode;
                                anyNode.id = ixanim::kAnyStateId;
                                anyNode.name = "Any State";
                                anyNode.isAnyState = true;
                                anyNode.graphPos[0] = minX;
                                anyNode.graphPos[1] = minY - 120.0f;
                                animatorGraphState.nodes.push_back(std::move(anyNode));
                            }

                            // Transition edges (Any-State ones flagged for the cyan style).
                            for (const ixanim::AnimatorTransition& t : ctrl.transitions)
                            {
                                AnimatorGraphEdge edge;
                                edge.fromStateId = t.fromStateId;
                                edge.toStateId = t.toStateId;
                                edge.isAnyState = (t.fromStateId == ixanim::kAnyStateId);
                                edge.conditionCount = static_cast<int>(t.conditions.size());
                                edge.hasExitTime = t.hasExitTime;
                                edge.exitTime = t.exitTime;
                                edge.duration = t.duration;
                                edge.canTransitionToSelf = t.canTransitionToSelf;
                                for (const ixanim::AnimatorCondition& c : t.conditions)
                                {
                                    AnimatorGraphCondition gc;
                                    gc.param = c.param;
                                    gc.op = static_cast<AnimEditConditionOp>(static_cast<int>(c.op));
                                    gc.value = c.value;
                                    edge.conditions.push_back(std::move(gc));
                                }
                                animatorGraphState.edges.push_back(std::move(edge));
                            }

                            // Parameters (for the Stage-7 parameter panel + transition editor).
                            for (const ixanim::AnimatorParameter& p : ctrl.parameters)
                            {
                                AnimatorGraphParameter gp;
                                gp.name = p.name;
                                gp.type = static_cast<AnimEditParamType>(static_cast<int>(p.type));
                                gp.defaultValue = p.defaultValue;
                                animatorGraphState.parameters.push_back(std::move(gp));
                            }

                            // Live highlight: whenever the runtime is bound (idles in Edit, drives in
                            // Play), mark the active state + in-flight transition so the panel tints them.
                            if (auto rtIt = entityAnimators.find(selectedEditorObject.id);
                                rtIt != entityAnimators.end() && rtIt->second.bound)
                            {
                                const ixanim::AnimatorRuntime& rt = rtIt->second;
                                animatorGraphState.activeStateId = rt.currentStateId;
                                for (AnimatorGraphNode& n : animatorGraphState.nodes)
                                    n.isActive = (n.id == rt.currentStateId);
                                for (AnimatorGraphEdge& e : animatorGraphState.edges)
                                    e.active = rt.inTransition &&
                                               e.fromStateId == rt.currentStateId &&
                                               e.toStateId == rt.transitionToStateId;
                            }

                            // Node positions stay in raw controller coordinates; the Animator panel
                            // centers the view ONCE (via pan) when the controller first appears, so
                            // editing a node never makes the rest of the graph drift.
                        }
                    }
                    editorImGui.SetAnimatorGraphEditorState(animatorGraphState);
                }

                std::vector<PhysicsEventEditorState> physicsEventStates;
                physicsEventStates.reserve(editorPhysicsEntityEvents.size());
                for (const PhysicsEntityEvent& event : editorPhysicsEntityEvents)
                {
                    PhysicsEventEditorState state{};
                    state.phase = PhysicsEventPhaseName(event.phase);
                    state.kind = PhysicsEventKindName(event.kind);
                    state.entityA = event.entityA;
                    state.entityB = event.entityB;
                    state.nameA = event.nameA;
                    state.nameB = event.nameB;
                    state.bodyA = event.bodyA;
                    state.bodyB = event.bodyB;
                    state.point[0] = event.point.x;
                    state.point[1] = event.point.y;
                    state.point[2] = event.point.z;
                    state.normal[0] = event.normal.x;
                    state.normal[1] = event.normal.y;
                    state.normal[2] = event.normal.z;
                    state.penetrationDepth = event.penetrationDepth;
                    physicsEventStates.push_back(std::move(state));
                }
                editorImGui.SetPhysicsEvents(std::move(physicsEventStates));
                TerrainEditorState terrainState{};
                if (terrainOk && terrain.HasTerrain())
                {
                    const TerrainSceneData terrainData = terrain.GetTerrainSceneData();
                    terrainState = BuildTerrainEditorState(
                        terrainData,
                        selectedEditorObject.type == SelectedEditorObjectType::Terrain);
                }
                editorImGui.SetTerrainEditorState(terrainState);
                const auto hierarchyBegin = std::chrono::steady_clock::now();
                if (!debugDisableHierarchyIteration)
                {
                    auto hierarchyState = buildHierarchyEntities();
                    editorImGui.SetHierarchySceneState(
                        static_cast<std::uint64_t>(editorSceneRootEntity),
                        std::move(hierarchyState.first),
                        std::move(hierarchyState.second));
                }
                frameProfile.hierarchyIterationMs = MillisecondsBetween(hierarchyBegin, std::chrono::steady_clock::now());

                LightingState lightingState = editorImGui.GetLightingState();
                const bool editorHideEntities = editorPlay.state.mode == EditorPlayMode::Edit;
                lightingState.numPointLights = 0;
                for (const PointLight& light : editorPointLights)
                {
                    if (editorHideEntities && light.editorHidden)
                        continue;
                    if (lightingState.numPointLights >= kMaxDynamicPointLights)
                        break;
                    lightingState.pointLights[lightingState.numPointLights++] = light;
                }
                lightingState.numSpotLights = 0;
                for (const SpotLight& light : editorSpotLights)
                {
                    if (editorHideEntities && light.editorHidden)
                        continue;
                    if (lightingState.numSpotLights >= kMaxDynamicSpotLights)
                        break;
                    lightingState.spotLights[lightingState.numSpotLights++] = light;
                }
                editorImGui.SetLightingState(lightingState);
                terrain.SetLightingState(lightingState);
                std::unordered_map<std::string, std::uint32_t> waterMaterialUsageMap;
                for (const WaterBody& body : editorWaterBodies)
                {
                    if (!body.materialId.empty())
                        ++waterMaterialUsageMap[body.materialId];
                }
                std::vector<std::pair<std::string, std::uint32_t>> waterMaterialUsageCounts;
                waterMaterialUsageCounts.reserve(waterMaterialUsageMap.size());
                for (const auto& usage : waterMaterialUsageMap)
                    waterMaterialUsageCounts.push_back(usage);
                editorImGui.SetWaterMaterialUsageCounts(std::move(waterMaterialUsageCounts));

                const auto waterMaterials = editorImGui.GetWaterMaterialsSnapshot();
                editorImGui.SetWaterMaterials(waterMaterials);
                terrain.SetWaterMaterials(waterMaterials);
                if (editorWaterBodiesDirty)
                {
                    std::vector<WaterBody> terrainWaterBodies = editorWaterBodies;
                    const bool hasHiddenWaterBody = editorHideEntities && std::any_of(terrainWaterBodies.begin(),
                        terrainWaterBodies.end(),
                        [](const WaterBody& body) { return body.editorHidden; });
                    if (hasHiddenWaterBody)
                    {
                        terrainWaterBodies.erase(std::remove_if(terrainWaterBodies.begin(),
                            terrainWaterBodies.end(),
                            [](const WaterBody& body) { return body.editorHidden; }), terrainWaterBodies.end());
                        terrain.SetWaterBodies(device, terrainWaterBodies);
                    }
                    else
                    {
                        terrain.SetWaterBodies(device, terrainWaterBodies);
                        editorWaterBodies = terrain.GetWaterBodies();
                    }
                    if (waterSculptMeshRegenPending &&
                        selectedEditorObject.type == SelectedEditorObjectType::WaterBody)
                    {
                        auto bodyIt = std::find_if(editorWaterBodies.begin(), editorWaterBodies.end(),
                            [&](const WaterBody& body) { return body.id == selectedEditorObject.id; });
                        if (bodyIt != editorWaterBodies.end())
                        {
                            const std::uint32_t activeCells = static_cast<std::uint32_t>(
                                std::count_if(bodyIt->shapeMask.begin(), bodyIt->shapeMask.end(),
                                    [](std::uint8_t value) { return value != 0; }));
                            Tracenf("[WATER-OBJ-5] Mesh regenerated: body_id=%u cells_active=%u mask=%ux%u",
                                bodyIt->id,
                                activeCells,
                                bodyIt->maskWidth,
                                bodyIt->maskHeight);
                        }
                    }
                    waterSculptMeshRegenPending = false;
                    editorWaterBodiesDirty = false;
                }
                terrain.SetSelectedWaterBodyHighlight(device, 0u);
                // Push the finalized lighting to every cached skinned model, and stash it so a
                // model loaded later this frame is seeded lit at create time (getSkinnedMeshRenderer).
                skinnedCacheLighting = lightingState;
                for (auto& [skinnedPath, skinnedEntry] : skinnedMeshCache)
                {
                    (void)skinnedPath;
                    if (skinnedEntry.renderer)
                        skinnedEntry.renderer->SetLightingState(lightingState);
                }
                if (commands.paletteSlotChanged)
                {
                    syncTerrainAssetRoots();
                    if (!terrain.ApplyPaletteSlotChange(device, commands.paletteSlotData))
                    {
                        Tracenf("[MAIN] failed to apply terrain palette slot %u", commands.paletteSlot);
                    }
                    else
                    {
                        editorImGui.SetPaletteSlots(terrain.GetPaletteSlots());
                        SceneManager::Instance().MarkDirty();
                    }
                }
                if (commands.paletteSlotParamsChanged)
                {
                    if (!terrain.ApplyPaletteSlotParams(commands.paletteSlotData))
                    {
                        Tracenf("[MAIN] failed to apply terrain material params for layer %u", commands.paletteSlot);
                    }
                    else
                    {
                        editorImGui.SetPaletteSlots(terrain.GetPaletteSlots());
                        SceneManager::Instance().MarkDirty();
                    }
                }
                if (commands.terrainTriplanarChanged)
                {
                    if (!terrain.SetTriplanarSettings(commands.terrainTriplanarEnabled,
                            commands.terrainTriplanarSharpness,
                            commands.terrainTriplanarSlopeThreshold,
                            commands.terrainTriplanarSlopeTransition))
                    {
                        Tracen("[MAIN] failed to apply terrain triplanar settings");
                    }
                    else
                    {
                        SceneManager::Instance().MarkDirty();
                    }
                }
                if (commands.save)
                {
                    terrain.RequestEditorSave();
                    SceneManager::Instance().MarkDirty();
                }
                if (commands.reload)
                {
                    terrain.RequestEditorReload();
                    selectedEditorObject = {};
                }
                if (commands.undo)
                    terrain.RequestEditorUndo();
                terrain.UpdateEditor(device, deltaSeconds, frameCamera, renderSize.width, renderSize.height);
                if (commands.reload)
                {
                    editorWaterBodies = terrain.GetWaterBodies();
                    editorWaterBodiesDirty = false;
                    for (const WaterBody& body : editorWaterBodies)
                        nextEditorWaterBodyId = std::max(nextEditorWaterBodyId, body.id + 1u);
                }
                SceneManager::Instance().SetCurrentSceneSnapshot(sceneRuntime.BuildSceneSnapshot());
            }
#endif
        }

        device.BeginFrame();
        if (device.IsFrameActive())
        {
            const uint64_t frameNumber = device.GetFrameNumber();
            // Per-frame skin-slot allocation over the per-model skinned cache. Each entry owns
            // its own MaxSkinSlots() pool; Scene view (+ water reflection) consumes bottom-up,
            // Game view top-down, both reset lazily once per device frame, so a model drawn in
            // BOTH views in one command buffer never overwrites its own poses. UINT32_MAX = skip.
            auto resetSkinnedCursorsIfNewFrame = [&](SkinnedMeshCacheEntry& e) {
                if (e.cursorsResetFrame != frameNumber)
                {
                    e.sceneSlotCursor = 0;
                    e.gameSlotCursor = SkinnedMeshRenderer::MaxSkinSlots();
                    e.cursorsResetFrame = frameNumber;
                }
            };
            auto allocSceneSkinSlot = [&](SkinnedMeshCacheEntry& e) -> std::uint32_t {
                resetSkinnedCursorsIfNewFrame(e);
                if (e.sceneSlotCursor >= e.gameSlotCursor)
                    return std::numeric_limits<std::uint32_t>::max();
                return e.sceneSlotCursor++;
            };
            auto allocGameSkinSlot = [&](SkinnedMeshCacheEntry& e) -> std::uint32_t {
                resetSkinnedCursorsIfNewFrame(e);
                if (e.gameSlotCursor == 0 || e.gameSlotCursor <= e.sceneSlotCursor)
                    return std::numeric_limits<std::uint32_t>::max();
                return --e.gameSlotCursor;
            };
            // Skinned draws recorded by the pre-pass (which issues the compute SkinInstance
            // BEFORE any render pass begins — a compute dispatch + barrier inside a render pass
            // is illegal). The render passes only replay these as RenderInWorld draws:
            //   sceneSkinnedDraws       — networked entities (Scene/offscreen + water reflection)
            //   sceneEditorSkinnedDraws — editor mesh entities, Scene-view (bottom-up) slots
            //   gameEditorSkinnedDraws  — editor mesh entities, Game-view (top-down) slots
            std::vector<SkinnedDrawRecord> sceneSkinnedDraws;
            std::vector<SkinnedDrawRecord> sceneEditorSkinnedDraws;
            std::vector<SkinnedDrawRecord> gameEditorSkinnedDraws;
            terrain.ResetFrameDrawStats();
            bool frameRmlUiRenderCalled = false;
            bool frameImGuiRenderCalled = false;
            bool frameSceneRenderCalled = false;
            size_t frameSceneEntityCount = 0;
            size_t frameStaticMeshEntityCount = 0;
            size_t frameStaticMeshSubmitted = 0;
            size_t frameStaticMeshDrawCalls = 0;
            size_t frameStaticMeshTriangles = 0;
            size_t frameStaticMeshUniformUpdates = 0;
            size_t frameStaticMeshOverrideActiveDraws = 0;
            size_t frameStaticMeshFrustumCulled = 0;
            SpatialIndex::QueryStats frameStaticMeshSpatialStats{};
            size_t frameStaticMeshBatches = 0;
            size_t frameStaticMeshMaxBatchSize = 0;
            size_t frameStaticMeshInstanceBufferBytes = 0;
            bool frameStaticMeshInstanceBufferRebuilt = false;
#if defined(IXTREEME_WITH_EDITOR)
            editorImGui.SetEditorPlayModeState(editorPlay.state);
            editorImGui.BeginFrame(runtimeSession->IsMapEditorOpen());
#endif
            const bool isInWorld = runtimeSession->IsInWorld();
            const bool hasSceneTerrain = terrainOk && terrain.HasTerrain();
            std::vector<WorldRenderEntity> entities;
            WorldCamera camera{};
            if (isInWorld)
            {
                entities = frameEntities;
                frameSceneEntityCount = entities.size();
                if (runtimeSession->IsMapEditorOpen())
                    frameSceneEntityCount += editorMeshEntities.size();
                camera = hasFrameCamera ? frameCamera : cameraController.BuildCamera(renderSize.width, renderSize.height);

                // Networked entities (dormant until kDefaultCharacterModelPath is set) skin into
                // the default character renderer's own slots and record their draws so the water
                // reflection and main passes redraw the exact (renderer, slot). Editor mesh
                // entities are skinned later in their own dedicated pass (per-model cache).
                if (SkinnedMeshRenderer* defaultSkinned = getSkinnedMeshRenderer(kDefaultCharacterModelPath))
                {
                    SkinnedMeshCacheEntry& defaultEntry = skinnedMeshCache[kDefaultCharacterModelPath];
                    for (const auto& entity : entities)
                    {
                        const std::uint32_t slot = allocSceneSkinSlot(defaultEntry);
                        if (slot == std::numeric_limits<std::uint32_t>::max())
                            break;
                        defaultSkinned->SkinInstance(device,
                            slot,
                            ToSkinnedMeshMotion(entity.moveState),
                            static_cast<float>(seconds));
                        WorldVec3 position = ServerMetersToDisplay(entity.position);
                        position.y += defaultSkinned->GroundOffsetY();
                        const std::array<float, 4> tint = entity.visualClassId == 0
                            ? std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f}
                            : (entity.visualClassId == 1
                                  ? std::array<float, 4>{1.35f, 0.55f, 0.55f, 1.0f}
                                  : std::array<float, 4>{0.65f, 0.95f, 1.35f, 1.0f});
                        sceneSkinnedDraws.push_back(SkinnedDrawRecord{
                            defaultSkinned, slot, position, HeadingFromQuantized(entity.heading), tint});
                    }
                }

                // Editor mesh entities (characters) are skinned HERE in the pre-pass — the compute
                // dispatch + barrier inside SkinInstance must be recorded OUTSIDE any render pass.
                // We allocate a Scene-view slot (and, when the Game view is visible, a separate
                // top-of-range Game slot) per entity, run the compute skin into each, and record
                // the (renderer, slot) so the offscreen and game passes only issue draws later.
                if (runtimeSession->IsMapEditorOpen())
                {
                    bool willRenderGameView = false;
#if defined(IXTREEME_WITH_EDITOR)
                    willRenderGameView = gameViewOk && editorImGui.IsGameViewVisible();
#endif
                    // Frame delta for clip-playback clocks (clamped; 0 on the first frame).
                    const float animDeltaSeconds = animPrevFrameSeconds > 0.0
                        ? static_cast<float>(std::clamp(seconds - animPrevFrameSeconds, 0.0, 0.25))
                        : 0.0f;
                    animPrevFrameSeconds = seconds;
                    for (MeshSceneEntity& skinnedEntity : editorMeshEntities)
                    {
                        if (editorPlay.state.mode == EditorPlayMode::Edit && skinnedEntity.editorHidden)
                            continue;
                        const std::string skinnedRuntimePath = resolveMeshRuntimePath(skinnedEntity);
                        // Self-heal: an entity flagged static whose model is actually a rigged
                        // glTF/FBX (static path reports UnsupportedSkinned) is promoted to skinned.
                        if (!skinnedEntity.skinned)
                        {
                            auto skinnedCacheIt = staticMeshCache.find(skinnedRuntimePath);
                            if (skinnedCacheIt != staticMeshCache.end() &&
                                skinnedCacheIt->second.state == StaticMeshCacheEntry::State::UnsupportedSkinned)
                            {
                                skinnedEntity.skinned = true;
                                SceneManager::Instance().MarkDirty();
                            }
                        }
                        if (!skinnedEntity.skinned)
                            continue;
                        const std::size_t beforeSlotCount = skinnedEntity.materialSlots.size();
                        EnsureMeshEntityMaterialSlots(skinnedEntity);
                        if (skinnedEntity.materialSlots.size() != beforeSlotCount)
                            SceneManager::Instance().MarkDirty();
                        SkinnedMeshRenderer* skinnedRenderer = getSkinnedMeshRenderer(skinnedRuntimePath);
                        if (!skinnedRenderer)
                            continue;
                        SkinnedMeshCacheEntry& skinnedEntry = skinnedMeshCache[skinnedRuntimePath];
                        // Player characters animate per their movement state (walk/run/idle);
                        // others stay idle. editorCharacterStates only has an entry while in Play.
                        SkinnedMeshRenderer::MotionState editorMeshMotion = SkinnedMeshRenderer::MotionState::Idle;
                        if (auto csIt = editorCharacterStates.find(skinnedEntity.id); csIt != editorCharacterStates.end())
                            editorMeshMotion = ToSkinnedMeshMotion(csIt->second.moveState);
                        const bool skinnedSelected =
                            selectedEditorObject.type == SelectedEditorObjectType::MeshEntity &&
                            selectedEditorObject.id == skinnedEntity.id;
                        const WorldVec3 skinnedPosition{skinnedEntity.position[0],
                            skinnedEntity.position[1] + skinnedRenderer->GroundOffsetY(),
                            skinnedEntity.position[2]};
                        const float skinnedYaw = skinnedEntity.rotation[1];

                        // Stage-4 Animator: if an AnimatorController is assigned, evaluate the FSM
                        // and use its pose (highest priority — over the debug clip and MotionState).
                        ozz::span<const ozz::math::SoaTransform> animatorPose;
#if defined(IXTREEME_WITH_EDITOR)
                        if (!skinnedEntity.animatorControllerId.empty())
                        {
                            const std::string& ctrlId = skinnedEntity.animatorControllerId;
                            std::string& boundCtrlId = entityBoundControllerId[skinnedEntity.id];
                            const auto existingAnim = entityAnimators.find(skinnedEntity.id);
                            const bool needBind = boundCtrlId != ctrlId ||
                                existingAnim == entityAnimators.end() || !existingAnim->second.bound;
                            if (needBind)
                            {
                                boundCtrlId = ctrlId;
                                const std::string ctrlPath = editorImGui.AnimatorControllerFilePath(ctrlId);
                                ixanim::AnimatorController controller;
                                bool ok = false;
                                if (!ctrlPath.empty())
                                {
                                    std::ifstream ctrlFile(ctrlPath, std::ios::binary);
                                    const std::string ctrlText((std::istreambuf_iterator<char>(ctrlFile)),
                                        std::istreambuf_iterator<char>());
                                    ok = ixanim::ControllerFromJson(ctrlText, controller);
                                }
                                if (ok)
                                {
                                    // Auto-fill empty state clips with this character's own
                                    // <stem>_anim_<i> clips (by state order) so a generic locomotion
                                    // controller drives the character with no per-state editing.
                                    const std::string stem = std::filesystem::path(skinnedRuntimePath).stem().string();
                                    for (std::size_t si = 0; si < controller.states.size(); ++si)
                                    {
                                        if (!controller.states[si].clipId.empty())
                                            continue;
                                        const std::string foundClip = editorImGui.FindAnimationClipIdByDisplayName(
                                            stem + "_anim_" + std::to_string(si));
                                        if (!foundClip.empty())
                                            controller.states[si].clipId = foundClip;
                                    }
                                    entityControllers[skinnedEntity.id] = std::move(controller);
                                    ixanim::BindAnimator(entityAnimators[skinnedEntity.id],
                                        &entityControllers[skinnedEntity.id],
                                        skinnedRenderer->JointNames(),
                                        static_cast<int>(skinnedRenderer->NumJoints()),
                                        static_cast<int>(skinnedRenderer->NumSoaJoints()));
                                }
                                else
                                {
                                    entityAnimators.erase(skinnedEntity.id);
                                    entityControllers.erase(skinnedEntity.id);
                                }
                            }
                            if (auto animIt = entityAnimators.find(skinnedEntity.id);
                                animIt != entityAnimators.end() && animIt->second.bound)
                            {
                                ixanim::AnimatorRuntime& animator = animIt->second;
                                if (auto csIt = editorCharacterStates.find(skinnedEntity.id);
                                    csIt != editorCharacterStates.end())
                                {
                                    ixanim::SetFloat(animator, "Speed", csIt->second.planarSpeed);
                                    ixanim::SetBool(animator, "IsGrounded", csIt->second.grounded);
                                    if (csIt->second.jumpedThisFrame)
                                        ixanim::SetTrigger(animator, "Jump");
                                }
                                animatorPose = ixanim::EvaluateAnimator(animator, animDeltaSeconds,
                                    skinnedRenderer->RestPoseLocals(),
                                    [&](const std::string& clipId) { return editorImGui.AnimationClipFilePath(clipId); });
                            }
                        }
                        else
                        {
                            entityAnimators.erase(skinnedEntity.id);
                            entityControllers.erase(skinnedEntity.id);
                            entityBoundControllerId.erase(skinnedEntity.id);
                        }
#endif

                        // Stage-3 temp binding: if a debug clip is assigned, (re)bind it retargeted
                        // onto this character's skeleton and sample it ONCE this frame. The slots
                        // below then skin from that pose (SkinInstanceFromPose) instead of the
                        // built-in MotionState clip. Sampling here (pre-pass) keeps Scene + Game in
                        // lockstep on the same pose and advances the clock exactly once per frame.
                        ozz::span<const ozz::math::SoaTransform> retargetedPose;
#if defined(IXTREEME_WITH_EDITOR)
                        {
                            const std::string& clipId = skinnedEntity.debugAnimationClipId;
                            if (clipId.empty())
                            {
                                entityClipPlaybacks.erase(skinnedEntity.id);
                                entityBoundClipId.erase(skinnedEntity.id);
                            }
                            else
                            {
                                std::string& boundId = entityBoundClipId[skinnedEntity.id];
                                if (boundId != clipId ||
                                    entityClipPlaybacks.find(skinnedEntity.id) == entityClipPlaybacks.end())
                                {
                                    boundId = clipId;
                                    ixanim::ClipPlayback playback;
                                    const std::string ixclipPath = editorImGui.AnimationClipFilePath(clipId);
                                    std::shared_ptr<ixanim::ClipAsset> clipAsset =
                                        ixclipPath.empty() ? nullptr : ixanim::LoadClipAsset(ixclipPath);
                                    if (clipAsset && ixanim::BindClip(playback, clipAsset, skinnedRenderer->JointNames(),
                                            static_cast<int>(skinnedRenderer->NumJoints()),
                                            static_cast<int>(skinnedRenderer->NumSoaJoints())))
                                        entityClipPlaybacks[skinnedEntity.id] = std::move(playback);
                                    else
                                        entityClipPlaybacks.erase(skinnedEntity.id);
                                }
                                if (auto pbIt = entityClipPlaybacks.find(skinnedEntity.id);
                                    pbIt != entityClipPlaybacks.end() && pbIt->second.ready)
                                {
                                    retargetedPose = ixanim::SampleAndRetarget(pbIt->second,
                                        skinnedRenderer->RestPoseLocals(), animDeltaSeconds);
                                }
                            }
                        }
#endif

                        const std::uint32_t sceneSlot = allocSceneSkinSlot(skinnedEntry);
                        if (sceneSlot != std::numeric_limits<std::uint32_t>::max())
                        {
                            if (animatorPose.size() != 0)
                                skinnedRenderer->SkinInstanceFromPose(device, sceneSlot, animatorPose);
                            else if (retargetedPose.size() != 0)
                                skinnedRenderer->SkinInstanceFromPose(device, sceneSlot, retargetedPose);
                            else
                                skinnedRenderer->SkinInstance(device, sceneSlot, editorMeshMotion, static_cast<float>(seconds));
                            sceneEditorSkinnedDraws.push_back(SkinnedDrawRecord{skinnedRenderer, sceneSlot, skinnedPosition, skinnedYaw,
                                skinnedSelected ? std::array<float, 4>{1.25f, 1.15f, 0.65f, 1.0f}
                                                : std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f}});
                        }
                        else
                        {
                            static bool loggedSceneSkinPoolFull = false;
                            if (!loggedSceneSkinPoolFull)
                            {
                                TraceError("[MESH-ENTITY] Scene skin-slot pool exhausted for model %s (>%u instances/frame); extra characters dropped",
                                    skinnedRuntimePath.c_str(), SkinnedMeshRenderer::MaxSkinSlots());
                                loggedSceneSkinPoolFull = true;
                            }
                        }

                        if (willRenderGameView)
                        {
                            const std::uint32_t gameSlot = allocGameSkinSlot(skinnedEntry);
                            if (gameSlot != std::numeric_limits<std::uint32_t>::max())
                            {
                                if (animatorPose.size() != 0)
                                    skinnedRenderer->SkinInstanceFromPose(device, gameSlot, animatorPose);
                                else if (retargetedPose.size() != 0)
                                    skinnedRenderer->SkinInstanceFromPose(device, gameSlot, retargetedPose);
                                else
                                    skinnedRenderer->SkinInstance(device, gameSlot, editorMeshMotion, static_cast<float>(seconds));
                                gameEditorSkinnedDraws.push_back(SkinnedDrawRecord{skinnedRenderer, gameSlot, skinnedPosition, skinnedYaw,
                                    std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f}});
                            }
                            else
                            {
                                static bool loggedGameSkinPoolFull = false;
                                if (!loggedGameSkinPoolFull)
                                {
                                    TraceError("[MESH-ENTITY] Game skin-slot pool exhausted for model %s (>%u instances/frame across Scene+Game); extra characters dropped",
                                        skinnedRuntimePath.c_str(), SkinnedMeshRenderer::MaxSkinSlots());
                                    loggedGameSkinPoolFull = true;
                                }
                            }
                        }
                    }
                }
            }
            else if (runtimeSession->IsLobbyActive())
            {
                if (SkinnedMeshRenderer* lobbySkinned = getSkinnedMeshRenderer(kDefaultCharacterModelPath))
                    lobbySkinned->Skin(device, seconds);
            }

            const auto sceneRenderBegin = std::chrono::steady_clock::now();
            if (isInWorld && hasSceneTerrain && hasFrameCamera && !debugDisableShadowPass)
            {
                device.WriteGpuTimestamp(VulkanDevice::GpuTimestampPoint::ShadowPassBegin);
                terrain.RenderSunShadowMap(device, frameCamera);
                device.WriteGpuTimestamp(VulkanDevice::GpuTimestampPoint::ShadowPassEnd);
            }
            if (isInWorld && hasSceneTerrain && hasFrameCamera && !debugDisableWaterReflectionPass)
            {
                device.WriteGpuTimestamp(VulkanDevice::GpuTimestampPoint::WaterReflectionBegin);
                terrain.RenderWaterReflection(device,
                    frameCamera,
                    seconds,
                    [&](const WorldCamera& mirrorCamera,
                        VkExtent2D reflectionExtent,
                        VkRenderPass reflectionRenderPass,
                        float waterLevelY)
                    {
                        // Redraw the exact (renderer, slot) recorded by the Scene pre-pass — the
                        // output buffers are already skinned, so no fresh slot allocation here.
                        for (const SkinnedDrawRecord& rec : sceneSkinnedDraws)
                        {
                            if (!rec.renderer)
                                continue;
                            rec.renderer->RenderInWorldReflection(device,
                                mirrorCamera,
                                reflectionExtent,
                                reflectionRenderPass,
                                waterLevelY,
                                rec.position,
                                rec.yaw,
                                rec.slot,
                                rec.tint);
                        }

                        // (Editor lights are not drawn as the skinned character model in the
                        // water reflection either — removed old debug visualization.)
                    });
                device.WriteGpuTimestamp(VulkanDevice::GpuTimestampPoint::WaterReflectionEnd);
            }

            const bool useOffscreenScene = offscreenSceneOk && device.IsFrameActive();
            if (useOffscreenScene)
                offscreenScene.BeginMainPass(device);
            else
                device.BeginSwapchainRenderPass("direct");

            std::vector<WorldLabelRenderer::Label> plates;
            if (isInWorld)
            {
                if (hasSceneTerrain)
                {
                    frameSceneRenderCalled = true;
                    device.WriteGpuTimestamp(VulkanDevice::GpuTimestampPoint::TerrainMainBegin);
                    terrain.Render(device, camera, renderSize);
                    device.WriteGpuTimestamp(VulkanDevice::GpuTimestampPoint::TerrainMainEnd);
                }
                device.WriteGpuTimestamp(VulkanDevice::GpuTimestampPoint::SceneOtherBegin);

                plates.reserve(entities.size());
                // Draw the networked skinned entities recorded by the pre-pass (dormant until
                // kDefaultCharacterModelPath is configured) — exact (renderer, slot) reuse.
                for (const SkinnedDrawRecord& rec : sceneSkinnedDraws)
                {
                    if (!rec.renderer)
                        continue;
                    rec.renderer->RenderInWorld(device,
                        seconds,
                        camera,
                        rec.position,
                        rec.yaw,
                        rec.slot,
                        rec.tint,
                        renderSize);
                }
                // Name plates over each networked entity. The label sits a fixed height above
                // the model's grounded origin (the default character's ground offset, if any).
                SkinnedMeshRenderer* plateSkinned = getSkinnedMeshRenderer(kDefaultCharacterModelPath);
                const float plateGroundOffsetY = plateSkinned ? plateSkinned->GroundOffsetY() : 0.0f;
                static bool loggedTerrainAlignment = false;
                for (const auto& entity : entities)
                {
                    auto position = ServerMetersToDisplay(entity.position);
                    const float terrainY = hasSceneTerrain ? terrain.SampleHeight(position) : position.y;
                    if (!loggedTerrainAlignment && hasSceneTerrain)
                    {
                        Tracenf("[WORLD] terrain align: net_id=%u server=(%.3f,%.3f,%.3f) displayY=%.3f terrainY=%.3f delta=%.3f modelGroundOffset=%.3f",
                            entity.netId,
                            entity.position.x,
                            entity.position.y,
                            entity.position.z,
                            position.y,
                            terrainY,
                            position.y - terrainY,
                            plateGroundOffsetY);
                        loggedTerrainAlignment = true;
                    }
                    position.y += plateGroundOffsetY;
                    plates.push_back(WorldLabelRenderer::Label{
                        position + WorldVec3{0.0f, 2.2f, 0.0f},
                        entity.name,
                        entity.netId == selectedTargetNetId
                            ? std::array<float, 4>{1.0f, 0.86f, 0.32f, 1.0f}
                            : std::array<float, 4>{0.92f, 0.96f, 1.0f, 1.0f},
                        entity.netId == selectedTargetNetId});
                }
                // (Editor point/spot lights are visualized by their wireframe gizmos via
                // BuildEditorLightShapeLines — they are NOT drawn as the skinned character
                // model. The old debug code that rendered lights/transforms as a character
                // mesh was removed.)
                if (runtimeSession->IsMapEditorOpen())
                {
                    std::unordered_map<StaticMeshLodBatchKey, StaticMeshLodBatch, StaticMeshLodBatchKeyHash> staticMeshBatches;
                    std::array<std::uint32_t, LodConfig::MaxLevels> frameLodSelection{};
                    std::uint32_t frameLodActiveInstances = 0;
                    std::uint32_t frameNonLodEntities = 0;
                    const std::vector<std::uint32_t> spatialCandidates =
                        staticMeshSpatialIndex.QueryFrustum(SpatialFrustumFromCamera(camera), &frameStaticMeshSpatialStats);
                    frameStaticMeshEntityCount = editorMeshEntities.size();
                    frameStaticMeshFrustumCulled =
                        frameStaticMeshSpatialStats.totalObjects > frameStaticMeshSpatialStats.candidates
                            ? static_cast<std::size_t>(frameStaticMeshSpatialStats.totalObjects - frameStaticMeshSpatialStats.candidates)
                            : 0u;
                    // Skinned mesh entities (characters) were already compute-skinned in the
                    // pre-pass (before this render pass began); here we only issue their Scene-view
                    // draws from the recorded (renderer, slot). Per-model cache + per-renderer slots
                    // mean multiple DIFFERENT characters draw at once.
                    for (const SkinnedDrawRecord& rec : sceneEditorSkinnedDraws)
                    {
                        if (!rec.renderer)
                            continue;
                        rec.renderer->RenderInWorld(device,
                            seconds,
                            camera,
                            rec.position,
                            rec.yaw,
                            rec.slot,
                            rec.tint,
                            renderSize);
                        ++frameStaticMeshDrawCalls;
                    }
                    auto logLodDisposition = [&](const StaticMeshLodBatch::LodDispositionRecord& record) {
                        const char* disposition = "DRAWN";
                        if (record.fullResFallback && record.submitted)
                            disposition = "DRAWN";
                        else if (record.selectedLevel >= record.levelCount)
                            disposition = "LEVEL_OUT_OF_RANGE";
                        else if (!record.bufferValid)
                            disposition = "INVALID_BUFFER";
                        else if (record.selectedIndices == 0)
                            disposition = "EMPTY_BUFFER";
                        else if (record.culled)
                            disposition = "CULLED";
                        else if (!record.submitted)
                            disposition = "NOT_SUBMITTED";

                        LodDispositionState& state = staticMeshLodDispositionStates[record.entityId];
                        const bool changed =
                            state.selectedLevel != record.selectedLevel ||
                            state.bufferValid != record.bufferValid ||
                            state.culled != record.culled ||
                            state.submitted != record.submitted ||
                            state.fullResFallback != record.fullResFallback ||
                            state.disposition != disposition;
                        const bool heartbeat = (frameNumber % 60u) == 0u;
                        if (!changed && !heartbeat)
                            return;

                        if (LodLogsEnabled())
                        {
                            Tracenf("[LOD-DISP] entity=%u dist=%.3f prevLevel=%u -> level=%u levelCount=%u cfg.distances=[%.2f,%.2f,%.2f,%.2f] cfg.ratios=[%.5f,%.5f,%.5f,%.5f] override=%u bufferValid=%s bufferSource=%s selVerts=%zu selIndices=%zu bbox.min=(%.3f,%.3f,%.3f) bbox.max=(%.3f,%.3f,%.3f) bboxSource=%s culled=%s cullReason=%s submitted=%s drawIndexCount=%u disposition=%s",
                                record.entityId,
                                record.distance,
                                record.previousLevel,
                                record.selectedLevel,
                                record.levelCount,
                                record.configSnapshot.distances[0],
                                record.configSnapshot.distances[1],
                                record.configSnapshot.distances[2],
                                record.configSnapshot.distances[3],
                                record.configSnapshot.targetRatios[0],
                                record.configSnapshot.targetRatios[1],
                                record.configSnapshot.targetRatios[2],
                                record.configSnapshot.targetRatios[3],
                                record.overrideEnabled ? 1u : 0u,
                                record.bufferValid ? "y" : "n",
                                record.bufferSource,
                                record.selectedVertices,
                                record.selectedIndices,
                                record.bounds.min.x, record.bounds.min.y, record.bounds.min.z,
                                record.bounds.max.x, record.bounds.max.y, record.bounds.max.z,
                                record.bboxSource,
                                record.culled ? "y" : "n",
                                record.cullReason,
                                record.submitted ? "y" : "n",
                                record.drawIndexCount,
                                disposition);
                        }

                        state.selectedLevel = record.selectedLevel;
                        state.bufferValid = record.bufferValid;
                        state.culled = record.culled;
                        state.submitted = record.submitted;
                        state.fullResFallback = record.fullResFallback;
                        state.disposition = disposition;
                    };
                    auto lodBufferSourceForDiag = [](const StaticMeshRenderer::LodDiagnostics& diag) -> const char* {
                        if (diag.pendingUpload)
                            return "pending";
                        if (std::strcmp(diag.source, "cache") == 0)
                            return "cache";
                        if (std::strcmp(diag.source, "preview") == 0 ||
                            std::strcmp(diag.source, "commit") == 0 ||
                            std::strcmp(diag.source, "fullres") == 0)
                        {
                            return "generated";
                        }
                        return "none";
                    };
                    std::unordered_set<std::uint32_t> currentCulledMeshLogSet;
                    for (std::uint32_t meshId : spatialCandidates)
                    {
                        MeshSceneEntity* meshPtr = findMeshEntityById(meshId);
                        if (!meshPtr)
                            continue;
                        const MeshSceneEntity& mesh = *meshPtr;
                        if (editorPlay.state.mode == EditorPlayMode::Edit && mesh.editorHidden)
                            continue;
                        const std::string runtimePath = resolveMeshRuntimePath(mesh);
                        // Skinned mesh entities are handled in the dedicated skinned pass above
                        // (they are not tracked in the static spatial index), so skip them here.
                        if (mesh.skinned)
                            continue;
                        if (StaticMeshRenderer* renderer = getStaticMeshRenderer(runtimePath))
                        {
                            if (meshPtr->materialSlots.empty())
                            {
                                EnsureMeshEntityMaterialSlots(*meshPtr);
                                if (!meshPtr->materialSlots.empty())
                                    SceneManager::Instance().MarkDirty();
                            }
                            const SpatialIndex::Aabb worldBounds = StaticMeshWorldAabb(mesh, *renderer);
                            if (renderer->IsLoaded() &&
                                WorldAabbOutsideCameraFrustum(camera, SpatialAabbCorners(worldBounds)))
                            {
                                ++frameStaticMeshFrustumCulled;
                                currentCulledMeshLogSet.insert(mesh.id);
                                if (mesh.lod.enabled)
                                {
                                    LodConfig cullLodConfig{};
                                    const std::optional<LodConfig> assetDefault = editorImGui.FindModelLodDefault(mesh.meshAssetId);
                                    cullLodConfig = (!mesh.lod.overrideAssetDefault && assetDefault)
                                        ? *assetDefault
                                        : mesh.lod.config;
                                    cullLodConfig.levelCount = std::clamp(cullLodConfig.levelCount, 1u, LodConfig::MaxLevels);
                                    cullLodConfig.targetRatios[0] = 1.0f;
                                    cullLodConfig.distances[0] = 0.0f;
                                    const std::uint64_t cullConfigHash = HashLodConfig(cullLodConfig);
                                    const float cullDistance = DistanceToAabb(camera.eye, worldBounds);
                                    const std::uint32_t previousLevel = staticMeshSelectedLods.count(mesh.id) > 0
                                        ? staticMeshSelectedLods[mesh.id]
                                        : 0u;
                                    const std::uint32_t cullLevel = SelectLodLevel(cullLodConfig, cullDistance, previousLevel);
                                    const StaticMeshRenderer::LodDiagnostics lodDiag =
                                        renderer->GetLodDiagnostics(cullLevel == 0 ? 0 : cullConfigHash);
                                    StaticMeshLodBatch::LodDispositionRecord record{};
                                    record.entityId = mesh.id;
                                    record.distance = cullDistance;
                                    record.previousLevel = previousLevel;
                                    record.selectedLevel = cullLevel;
                                    record.levelCount = lodDiag.levelCount;
                                    record.configSnapshot = cullLodConfig;
                                    record.overrideEnabled = mesh.lod.overrideAssetDefault;
                                    record.bufferValid = cullLevel < lodDiag.levelCount && lodDiag.bufferValid;
                                    record.bufferSource = lodBufferSourceForDiag(lodDiag);
                                    record.selectedVertices = lodDiag.vertexCount;
                                    record.selectedIndices = cullLevel < lodDiag.levelIndices.size() ? lodDiag.levelIndices[cullLevel] : 0u;
                                    record.bounds = worldBounds;
                                    record.bboxSource =
                                        (worldBounds.min.x == worldBounds.max.x ||
                                            worldBounds.min.y == worldBounds.max.y ||
                                            worldBounds.min.z == worldBounds.max.z)
                                        ? "degenerate"
                                        : "entity";
                                    record.culled = true;
                                    record.cullReason = "frustum";
                                    record.submitted = false;
                                    record.drawIndexCount = 0;
                                    logLodDisposition(record);
                                }
                                const bool cullLogChanged = previousCulledMeshLogSet.find(mesh.id) == previousCulledMeshLogSet.end();
                                if (cullLogChanged)
                                {
                                    Tracenf("[MESH-CULL] culled id=%u name=%s path=%s",
                                        mesh.id,
                                        mesh.name.c_str(),
                                        runtimePath.c_str());
                                }
                                continue;
                            }
                            renderer->SetLightingState(runtimeSession->GetLightingState());
                            StaticMeshRenderer::Instance instance{};
                            instance.entityId = mesh.id;
                            instance.position = {mesh.position[0], mesh.position[1], mesh.position[2]};
                            instance.rotation[0] = mesh.rotation[0];
                            instance.rotation[1] = mesh.rotation[1];
                            instance.rotation[2] = mesh.rotation[2];
                            instance.scale[0] = mesh.scale[0];
                            instance.scale[1] = mesh.scale[1];
                            instance.scale[2] = mesh.scale[2];
                            instance.tint = {1.0f, 1.0f, 1.0f, 1.0f};
                            instance.selectedForOutline = false;
                            instance.materialSlots = mesh.materialSlots;
                            instance.materialOverrides = mesh.materialOverrides;
                            LodConfig effectiveLodConfig{};
                            std::uint64_t lodConfigHash = 0;
                            std::uint32_t lodLevel = 0;
                            bool lodDispositionActive = false;
                            StaticMeshLodBatch::LodDispositionRecord lodDispositionRecord{};
                            if (mesh.lod.enabled)
                            {
                                ++frameLodActiveInstances;
                                const std::optional<LodConfig> assetDefault = editorImGui.FindModelLodDefault(mesh.meshAssetId);
                                effectiveLodConfig = (!mesh.lod.overrideAssetDefault && assetDefault)
                                    ? *assetDefault
                                    : mesh.lod.config;
                                effectiveLodConfig.levelCount = std::clamp(effectiveLodConfig.levelCount, 1u, LodConfig::MaxLevels);
                                effectiveLodConfig.targetRatios[0] = 1.0f;
                                effectiveLodConfig.distances[0] = 0.0f;
                                lodConfigHash = HashLodConfig(effectiveLodConfig);
                                const float distance = DistanceToAabb(camera.eye, worldBounds);
                                const std::uint32_t previousLevel = staticMeshSelectedLods.count(mesh.id) > 0
                                    ? staticMeshSelectedLods[mesh.id]
                                    : 0u;
                                lodLevel = SelectLodLevel(effectiveLodConfig, distance, previousLevel);
                                staticMeshSelectedLods[mesh.id] = lodLevel;
                                if (lodLevel < frameLodSelection.size())
                                    ++frameLodSelection[lodLevel];
                                const StaticMeshRenderer::LodDiagnostics lodDiag =
                                    renderer->GetLodDiagnostics(lodLevel == 0 ? 0 : lodConfigHash);
                                const std::size_t selectedTris =
                                    lodLevel < lodDiag.levelTris.size() ? lodDiag.levelTris[lodLevel] : 0u;
                                const bool selectedBufferValid =
                                    lodLevel < lodDiag.levelCount && lodDiag.bufferValid;
                                lodDispositionActive = true;
                                lodDispositionRecord.entityId = mesh.id;
                                lodDispositionRecord.distance = distance;
                                lodDispositionRecord.previousLevel = previousLevel;
                                lodDispositionRecord.selectedLevel = lodLevel;
                                lodDispositionRecord.levelCount = lodDiag.levelCount;
                                lodDispositionRecord.configSnapshot = effectiveLodConfig;
                                lodDispositionRecord.overrideEnabled = mesh.lod.overrideAssetDefault;
                                lodDispositionRecord.bufferValid = selectedBufferValid;
                                lodDispositionRecord.bufferSource = lodBufferSourceForDiag(lodDiag);
                                lodDispositionRecord.selectedVertices = lodDiag.vertexCount;
                                lodDispositionRecord.selectedIndices = lodLevel < lodDiag.levelIndices.size() ? lodDiag.levelIndices[lodLevel] : 0u;
                                lodDispositionRecord.bounds = worldBounds;
                                lodDispositionRecord.bboxSource =
                                    (worldBounds.min.x == worldBounds.max.x ||
                                        worldBounds.min.y == worldBounds.max.y ||
                                        worldBounds.min.z == worldBounds.max.z)
                                    ? "degenerate"
                                    : "entity";
                                lodDispositionRecord.culled = false;
                                lodDispositionRecord.cullReason = "none";
                                std::array<float, LodConfig::MaxLevels> cfgRatios{};
                                std::array<float, LodConfig::MaxLevels> cfgDistances{};
                                for (std::size_t i = 0; i < LodConfig::MaxLevels; ++i)
                                {
                                    cfgRatios[i] = effectiveLodConfig.targetRatios[i];
                                    cfgDistances[i] = effectiveLodConfig.distances[i];
                                }
                                LodCfgLogState& cfgLogState = lodCfgLogStates[mesh.id];
                                const bool cfgChanged =
                                    !cfgLogState.initialized ||
                                    cfgLogState.levelCount != effectiveLodConfig.levelCount ||
                                    cfgLogState.ratios != cfgRatios ||
                                    cfgLogState.distances != cfgDistances ||
                                    cfgLogState.overrideEnabled != mesh.lod.overrideAssetDefault;
                                if (LodLogsEnabled() && (!QuietLogsForLodDiag() || cfgChanged))
                                {
                                    Tracenf("[LOD-CFG] entity=%u levelCount=%u ratios=[%.5f,%.5f,%.5f,%.5f] distances=[%.2f,%.2f,%.2f,%.2f] override=%u",
                                        mesh.id,
                                        effectiveLodConfig.levelCount,
                                        effectiveLodConfig.targetRatios[0],
                                        effectiveLodConfig.targetRatios[1],
                                        effectiveLodConfig.targetRatios[2],
                                        effectiveLodConfig.targetRatios[3],
                                        effectiveLodConfig.distances[0],
                                        effectiveLodConfig.distances[1],
                                        effectiveLodConfig.distances[2],
                                        effectiveLodConfig.distances[3],
                                        mesh.lod.overrideAssetDefault ? 1u : 0u);
                                }
                                cfgLogState.levelCount = effectiveLodConfig.levelCount;
                                cfgLogState.ratios = cfgRatios;
                                cfgLogState.distances = cfgDistances;
                                cfgLogState.overrideEnabled = mesh.lod.overrideAssetDefault;
                                cfgLogState.initialized = true;

                                std::array<std::size_t, LodConfig::MaxLevels> pickLevelTris{};
                                for (std::size_t i = 0; i < LodConfig::MaxLevels; ++i)
                                    pickLevelTris[i] = lodDiag.levelTris[i];
                                LodPickLogState& pickLogState = lodPickLogStates[mesh.id];
                                const bool pickCritical =
                                    lodLevel > 0 &&
                                    (lodLevel >= lodDiag.levelCount ||
                                        !selectedBufferValid ||
                                        selectedTris == 0);
                                const bool pickChanged =
                                    !pickLogState.initialized ||
                                    pickLogState.selectedLevel != lodLevel ||
                                    pickLogState.levelCount != lodDiag.levelCount ||
                                    pickLogState.levelTris != pickLevelTris ||
                                    pickLogState.selectedTris != selectedTris ||
                                    pickLogState.selectedBufferValid != selectedBufferValid ||
                                    pickLogState.source != lodDiag.source;
                                if (LodLogsEnabled() && (!QuietLogsForLodDiag() || pickChanged || pickCritical))
                                {
                                    Tracenf("[LOD-PICK] entity=%u dist=%.3f selectedLevel=%u levelCount=%u levelTris=[%zu,%zu,%zu,%zu] selectedTris=%zu selectedBufferValid=%s source=%s",
                                        mesh.id,
                                        distance,
                                        lodLevel,
                                        lodDiag.levelCount,
                                        lodDiag.levelTris[0],
                                        lodDiag.levelTris[1],
                                        lodDiag.levelTris[2],
                                        lodDiag.levelTris[3],
                                        selectedTris,
                                        selectedBufferValid ? "y" : "n",
                                        lodDiag.source);
                                }
                                pickLogState.selectedLevel = lodLevel;
                                pickLogState.levelCount = lodDiag.levelCount;
                                pickLogState.levelTris = pickLevelTris;
                                pickLogState.selectedTris = selectedTris;
                                pickLogState.selectedBufferValid = selectedBufferValid;
                                pickLogState.source = lodDiag.source;
                                pickLogState.initialized = true;
                                if (lodLevel > 0)
                                {
                                    const char* emptyReason = nullptr;
                                    if (!lodDiag.bufferKnown || !lodDiag.bufferValid)
                                        emptyReason = "buffer-null";
                                    else if (lodLevel >= lodDiag.levelCount)
                                        emptyReason = "level-out-of-range";
                                    else if (selectedTris == 0)
                                        emptyReason = std::strcmp(lodDiag.source, "preview") == 0 ? "preview-empty" : "zero-tris";
                                    if (emptyReason)
                                    {
                                        const char* fallbackReason = "buffer-not-ready";
                                        if (std::strcmp(emptyReason, "level-out-of-range") == 0)
                                            fallbackReason = "no-levels";
                                        else if (std::strcmp(emptyReason, "zero-tris") == 0 ||
                                            std::strcmp(emptyReason, "preview-empty") == 0)
                                            fallbackReason = "zero-tris";
                                        if (LodLogsEnabled())
                                        {
                                            Tracenf("[LOD-PICK] EMPTY entity=%u reason=%s", mesh.id, emptyReason);
                                            Tracenf("[LOD-PICK] FALLBACK entity=%u reason=%s drawing=full-res",
                                                mesh.id,
                                                fallbackReason);
                                        }
                                    }
                                }
                            }
                            else
                            {
                                ++frameNonLodEntities;
                                staticMeshSelectedLods.erase(mesh.id);
                            }
                            StaticMeshLodBatchKey key{renderer, lodConfigHash, lodLevel};
                            StaticMeshLodBatch& batch = staticMeshBatches[key];
                            batch.config = effectiveLodConfig;
                            batch.instances.push_back(std::move(instance));
                            if (lodDispositionActive)
                                batch.lodDispositionRecords.push_back(lodDispositionRecord);
                        }
                    }
                    for (auto& [key, batch] : staticMeshBatches)
                    {
                        StaticMeshRenderer* renderer = key.renderer;
                        std::vector<StaticMeshRenderer::Instance>& instances = batch.instances;
                        if (!renderer || instances.empty())
                            continue;
                        if (key.configHash != 0)
                            renderer->RenderLodBatchInWorld(device, seconds, camera, instances, batch.config, key.configHash, key.lodLevel, renderSize);
                        else
                            renderer->RenderBatchInWorld(device, seconds, camera, instances, renderSize);
                        const std::uint32_t submittedDrawCalls = renderer->LastSubmittedDrawCalls();
                        const std::uint32_t submittedInstances = renderer->LastSubmittedInstances();
                        const std::uint32_t submittedIndexCount = renderer->LastSubmittedIndexCount();
                        for (StaticMeshLodBatch::LodDispositionRecord& record : batch.lodDispositionRecords)
                        {
                            record.submitted = submittedDrawCalls > 0 && submittedInstances > 0;
                            record.fullResFallback = renderer->LastUsedFullResFallback();
                            record.drawIndexCount = record.submitted ? submittedIndexCount : 0u;
                            logLodDisposition(record);
                        }
                        if (submittedDrawCalls == 0 || submittedInstances == 0)
                            continue;
                        frameStaticMeshBatches += submittedDrawCalls;
                        frameStaticMeshMaxBatchSize = std::max(frameStaticMeshMaxBatchSize, instances.size());
                        frameStaticMeshSubmitted += submittedInstances;
                        frameStaticMeshDrawCalls += submittedDrawCalls;
                        frameStaticMeshTriangles += (renderer->LastUsedFullResFallback()
                            ? renderer->TriangleCount()
                            : renderer->TriangleCountForLod(key.configHash, key.lodLevel)) * submittedInstances;
                        frameStaticMeshUniformUpdates += renderer->LastMaterialUniformUpdates();
                        frameStaticMeshOverrideActiveDraws += renderer->LastOverrideActiveDraws();
                        frameStaticMeshInstanceBufferBytes += renderer->LastInstanceBufferBytes();
                        frameStaticMeshInstanceBufferRebuilt = frameStaticMeshInstanceBufferRebuilt || renderer->LastInstanceBufferRebuilt();
                        const bool meshSubmitDetailChanged =
                            !previousMeshSubmitDetailInitialized ||
                            previousMeshSubmitDetailInstances != submittedInstances ||
                            previousMeshSubmitDetailDrawCalls != submittedDrawCalls;
                        const bool shouldLogMeshSubmitDetail =
                            LodLogsEnabled() &&
                            ((!QuietLogsForLodDiag() && (frameNumber < 3 || (frameNumber % 60u) == 0u)) ||
                                (QuietLogsForLodDiag() && meshSubmitDetailChanged));
                        if (shouldLogMeshSubmitDetail)
                        {
                            const auto& bmin = renderer->BoundsMin();
                            const auto& bmax = renderer->BoundsMax();
                            Tracenf("[MESH] Static instanced submit detail: instances=%u drawcalls=%u verts=%zu indices=%zu bbox_min=(%.3f,%.3f,%.3f) bbox_max=(%.3f,%.3f,%.3f)",
                                submittedInstances,
                                submittedDrawCalls,
                                renderer->VertexCount(),
                                renderer->IndexCount(),
                                bmin[0], bmin[1], bmin[2],
                                bmax[0], bmax[1], bmax[2]);
                        }
                        previousMeshSubmitDetailInstances = submittedInstances;
                        previousMeshSubmitDetailDrawCalls = submittedDrawCalls;
                        previousMeshSubmitDetailInitialized = true;
                    }
                    const bool lodSelectionChanged =
                        !previousFrameLodSelectionInitialized ||
                        previousFrameLodSelection != frameLodSelection;
                    const bool nonLodChanged =
                        !previousFrameNonLodInitialized ||
                        previousFrameNonLodEntities != frameNonLodEntities;
                    if (LodLogsEnabled() &&
                        ((!QuietLogsForLodDiag() && (frameNumber < 3 || (frameNumber % 60u) == 0u)) ||
                        (QuietLogsForLodDiag() && (lodSelectionChanged || nonLodChanged))))
                    {
                        Tracenf("[LOD] selection lod0=%u lod1=%u lod2=%u lod3=%u (LOD-active instances)",
                            frameLodSelection[0],
                            frameLodSelection[1],
                            frameLodSelection[2],
                            frameLodSelection[3]);
                        Tracenf("[LOD] non-lod entities=%u (always full-res)", frameNonLodEntities);
                    }
                    previousFrameLodSelection = frameLodSelection;
                    previousFrameLodSelectionInitialized = true;
                    previousFrameNonLodEntities = frameNonLodEntities;
                    previousFrameNonLodInitialized = true;
                    previousCulledMeshLogSet = std::move(currentCulledMeshLogSet);
                }
                if (selectionOutlinesOk)
                {
                    std::vector<SelectionOutlineRenderer::Line> selectionLines =
                        BuildEditorLightShapeLines(editorPointLights, editorSpotLights, selectedEditorObject);
                    std::vector<SelectionOutlineRenderer::Line> selectedObjectLines =
                        BuildSelectionOutlineLines(
                            selectedEditorObject,
                            editorMeshEntities,
                            editorPointLights,
                            editorSpotLights,
                            editorWaterBodies,
                            camera,
                            [&](const MeshSceneEntity& mesh) {
                                return getStaticMeshRenderer(resolveMeshRuntimePath(mesh));
                            });
                    selectionLines.insert(selectionLines.end(),
                        selectedObjectLines.begin(),
                        selectedObjectLines.end());
                    std::vector<SelectionOutlineRenderer::Line> selectedColliderLines =
                        BuildPhysicsColliderLines(selectedEditorObject, editorMeshEntities, debugShowPhysicsColliders);
                    selectionLines.insert(selectionLines.end(),
                        selectedColliderLines.begin(),
                        selectedColliderLines.end());
                    std::vector<SelectionOutlineRenderer::Line> physicsCenterLines =
                        BuildPhysicsBodyCenterLines(selectedEditorObject, editorMeshEntities, debugShowPhysicsBodyCenters);
                    selectionLines.insert(selectionLines.end(),
                        physicsCenterLines.begin(),
                        physicsCenterLines.end());
                    if (debugShowPhysicsContacts)
                    {
                        std::vector<SelectionOutlineRenderer::Line> physicsContactLines =
                            BuildPhysicsContactLines(editorPhysicsDebugContacts);
                        selectionLines.insert(selectionLines.end(),
                            physicsContactLines.begin(),
                            physicsContactLines.end());
                    }
                    if (!editorPhysicsDebugLines.empty())
                    {
                        std::vector<SelectionOutlineRenderer::Line> physicsQueryLines =
                            BuildPhysicsDebugLines(editorPhysicsDebugLines);
                        selectionLines.insert(selectionLines.end(),
                            physicsQueryLines.begin(),
                            physicsQueryLines.end());
                    }
                    // Draw the selected camera's view frustum so the Scene View shows what the
                    // Game / Main Camera sees. Uses the Game view's aspect for an accurate cone.
                    if (selectedEditorObject.type == SelectedEditorObjectType::Camera)
                    {
                        for (const CameraEntity& cameraEntity : editorCameras)
                        {
                            if (cameraEntity.id != selectedEditorObject.id)
                                continue;
                            const VkExtent2D frustumExtent = gameViewOk ? gameView.GetExtent() : renderSize;
                            std::vector<SelectionOutlineRenderer::Line> frustumLines =
                                BuildCameraFrustumLines(cameraEntity,
                                    frustumExtent.width, frustumExtent.height,
                                    {0.30f, 0.85f, 1.0f, 0.9f});
                            selectionLines.insert(selectionLines.end(),
                                frustumLines.begin(),
                                frustumLines.end());
                            break;
                        }
                    }
                    selectionOutlines.Render(device, camera, selectionLines, renderSize);
                }
                if (!useOffscreenScene && hasSceneTerrain)
                {
                    terrain.RenderWater(device, camera, seconds, renderSize);
                }
                if (!useOffscreenScene && worldLabelsOk)
                    worldLabels.Render(device, camera, plates);
                device.WriteGpuTimestamp(VulkanDevice::GpuTimestampPoint::SceneOtherEnd);
            }
            else if (SkinnedMeshRenderer* lobbySkinned = runtimeSession->IsLobbyActive()
                         ? getSkinnedMeshRenderer(kDefaultCharacterModelPath)
                         : nullptr)
            {
                frameSceneRenderCalled = true;
                frameSceneEntityCount = 1;
                device.WriteGpuTimestamp(VulkanDevice::GpuTimestampPoint::SceneOtherBegin);
                lobbySkinned->Render(device, seconds);
                device.WriteGpuTimestamp(VulkanDevice::GpuTimestampPoint::SceneOtherEnd);
            }

            if (useOffscreenScene)
            {
                offscreenScene.EndMainPass(device);
                if (isInWorld && hasSceneTerrain)
                {
                    offscreenScene.SnapshotScene(device);
                    terrain.SetWaterRefractionInputs(offscreenScene.GetSceneColorSnapshotView(),
                        offscreenScene.GetSceneDepthSnapshotView(),
                        offscreenScene.GetLinearSampler(),
                        offscreenScene.GetExtent());
                    offscreenScene.BeginMainPass(device, false);
                    terrain.RenderWater(device, camera, seconds, renderSize);
                    offscreenScene.EndMainPass(device);
                }
#if defined(IXTREEME_WITH_EDITOR)
                // --- Game view: render the scene from the main camera into the second offscreen target.
                // Reuses this frame's shadow map (light-space, view-independent) and water reflection.
                // Only render the Game view when its panel is actually visible (active dock
                // tab / not collapsed). Skipping it when hidden avoids a full second scene
                // render every frame — the biggest editor perf win.
                if (gameViewOk && runtimeSession->IsMapEditorOpen() && editorImGui.IsGameViewVisible())
                {
                    const CameraEntity* mainCameraEntity = nullptr;
                    for (const CameraEntity& cameraEntity : editorCameras)
                    {
                        if (cameraEntity.id == editorMainCameraId)
                        {
                            mainCameraEntity = &cameraEntity;
                            break;
                        }
                    }
                    if (!mainCameraEntity && !editorCameras.empty())
                        mainCameraEntity = &editorCameras.front();
                    if (mainCameraEntity)
                    {
                        const VkExtent2D gameExtent = gameView.GetExtent();
                        // In Play, the first active player character drives the Game camera
                        // (follow/first-person/top-down per its CameraMode). Otherwise the
                        // scene's static Main Camera is used.
                        CameraEntity gameCameraEntity = *mainCameraEntity;
                        if (editorPlay.state.mode == EditorPlayMode::Play)
                        {
                            for (const MeshSceneEntity& characterMesh : editorMeshEntities)
                            {
                                if (!characterMesh.hasCharacterController || !characterMesh.characterController.enabled)
                                    continue;
                                auto stateIt = editorCharacterStates.find(characterMesh.id);
                                if (stateIt == editorCharacterStates.end() || !stateIt->second.initialized)
                                    continue;
                                phys::CharacterControllerComponent cc = characterMesh.characterController;
                                phys::Sanitize(cc);
                                gameCameraEntity = ComputeCharacterCameraEntity(
                                    *mainCameraEntity, characterMesh, cc, stateIt->second);
                                break;
                            }
                        }
                        const WorldCamera gameCamera =
                            BuildCameraFromEntity(gameCameraEntity, gameExtent.width, gameExtent.height);
                        gameView.BeginMainPass(device);
                        // Terrain is drawn from the project Main Camera using the secondary
                        // camera-uniform path (viewIndex=1). That path has its own per-frame
                        // uniform buffer + descriptor set, so this draw no longer clobbers the
                        // Scene View's free-fly terrain (viewIndex=0, recorded earlier this
                        // frame into the same command buffer). Static meshes already use
                        // per-draw uniforms and are safe.
                        // Water uses the secondary per-water-body uniform/descriptor path
                        // (viewIndex=1) so it can be drawn from the Main Camera without
                        // clobbering the Scene View's water; reflection/refraction reuse this
                        // frame's Scene-view textures (acceptable; per-view RTs are a refinement).
                        if (hasSceneTerrain)
                        {
                            terrain.Render(device, gameCamera, gameExtent, /*viewIndex=*/1);
                        }
                        if (isInWorld)
                        {
                            // Batch + frustum-cull the Game view meshes (mirroring the Scene
                            // view) instead of one un-batched RenderInWorld per entity. This
                            // collapses N per-entity draws into one instanced draw per renderer
                            // and skips off-screen meshes — the main per-mesh cost of the Game view.
                            std::unordered_map<StaticMeshRenderer*, std::vector<StaticMeshRenderer::Instance>> gameMeshBatches;
                            const std::vector<std::uint32_t> gameMeshCandidates =
                                staticMeshSpatialIndex.QueryFrustum(SpatialFrustumFromCamera(gameCamera));
                            for (std::uint32_t candidateId : gameMeshCandidates)
                            {
                                const MeshSceneEntity* mesh = findMeshEntityById(candidateId);
                                if (!mesh || mesh->editorHidden || mesh->skinned)
                                    continue;
                                StaticMeshRenderer* renderer = getStaticMeshRenderer(resolveMeshRuntimePath(*mesh));
                                if (!renderer || !renderer->IsLoaded())
                                    continue;
                                StaticMeshRenderer::Instance instance{};
                                instance.entityId = mesh->id;
                                instance.position = {mesh->position[0], mesh->position[1], mesh->position[2]};
                                instance.rotation[0] = mesh->rotation[0];
                                instance.rotation[1] = mesh->rotation[1];
                                instance.rotation[2] = mesh->rotation[2];
                                instance.scale[0] = mesh->scale[0];
                                instance.scale[1] = mesh->scale[1];
                                instance.scale[2] = mesh->scale[2];
                                instance.materialSlots = mesh->materialSlots;
                                instance.materialOverrides = mesh->materialOverrides;
                                gameMeshBatches[renderer].push_back(std::move(instance));
                            }
                            for (auto& [gameRenderer, gameInstances] : gameMeshBatches)
                                gameRenderer->RenderBatchInWorld(device, seconds, gameCamera, gameInstances, gameExtent);

                            // Skinned meshes (incl. the player character) were compute-skinned in
                            // the pre-pass into their own top-of-range Game slots; here we only
                            // issue the Game-view draws from the recorded (renderer, slot).
                            for (const SkinnedDrawRecord& rec : gameEditorSkinnedDraws)
                            {
                                if (!rec.renderer)
                                    continue;
                                rec.renderer->RenderInWorld(device, seconds, gameCamera,
                                    rec.position, rec.yaw, rec.slot, rec.tint, gameExtent);
                            }
                        }
                        if (hasSceneTerrain)
                        {
                            terrain.RenderWater(device, gameCamera, seconds, gameExtent, /*viewIndex=*/1);
                        }
                        gameView.EndMainPass(device);
                    }
                }
#endif
                device.BeginSwapchainRenderPass("composite");
                device.WriteGpuTimestamp(VulkanDevice::GpuTimestampPoint::CompositeBegin);
                offscreenScene.RenderComposite(device);
                device.WriteGpuTimestamp(VulkanDevice::GpuTimestampPoint::CompositeEnd);
                if (isInWorld && worldLabelsOk)
                    worldLabels.Render(device, camera, plates);
            }
            frameProfile.sceneRenderMs = MillisecondsBetween(sceneRenderBegin, std::chrono::steady_clock::now());
            const auto editorUiBegin = std::chrono::steady_clock::now();
            frameRmlUiRenderCalled = true;
            device.WriteGpuTimestamp(VulkanDevice::GpuTimestampPoint::RmlUiBegin);
            rmlUi.Render(device);
            device.WriteGpuTimestamp(VulkanDevice::GpuTimestampPoint::RmlUiEnd);
#if defined(IXTREEME_WITH_EDITOR)
            frameImGuiRenderCalled = true;
            engineStats.swapchainWidth = swapchainSize.width;
            engineStats.swapchainHeight = swapchainSize.height;
            engineStats.renderWidth = renderSize.width;
            engineStats.renderHeight = renderSize.height;
            engineStats.frameNumber = frameNumber;
            engineStats.sceneEntityCount = frameSceneEntityCount;
            engineStats.staticMeshSubmitted = frameStaticMeshSubmitted;
            engineStats.staticMeshDrawCalls = frameStaticMeshDrawCalls;
            editorImGui.ClearSceneViewGizmo();
            editorImGui.SetSceneViewSelectionOutline({});
            if (runtimeSession->IsMapEditorOpen() && editorPlay.state.mode == EditorPlayMode::Edit)
            {
                const SceneGizmoTarget gizmoTarget = BuildSceneGizmoTarget(
                    selectedEditorObject,
                    editorMeshEntities,
                    editorPointLights,
                    editorSpotLights,
                    editorWaterBodies,
                    editorCameras,
                    [&](const MeshSceneEntity& mesh) {
                        return getStaticMeshRenderer(resolveMeshRuntimePath(mesh));
                    });
                if (gizmoTarget.visible)
                {
                    editorImGui.SetSceneViewGizmo(gizmoTarget.type,
                        gizmoTarget.id,
                        camera,
                        gizmoTarget.position,
                        gizmoTarget.rotation,
                        gizmoTarget.scale,
                        editorGizmoMode,
                        editorGizmoSnapEnabled,
                        editorGizmoSnapValue);
                }
            }
            editorImGui.SetEngineStats(engineStats);
            device.WriteGpuTimestamp(VulkanDevice::GpuTimestampPoint::ImGuiBegin);
            editorImGui.Render(device);
            device.WriteGpuTimestamp(VulkanDevice::GpuTimestampPoint::ImGuiEnd);
#else
            frameImGuiRenderCalled = false;
#endif
            frameProfile.editorUiRenderMs = MillisecondsBetween(editorUiBegin, std::chrono::steady_clock::now());
            const bool frameHeartbeatLog = QuietLogsForLodDiag()
                ? ((frameNumber % 60u) == 0u)
                : (frameNumber < 3 || (frameNumber % 60u) == 0u);
            if (frameHeartbeatLog)
            {
                if (!QuietLogsForLodDiag())
                {
                    TraceDiagf("[FRAME] static_mesh entities=%zu submitted=%zu drawcalls=%zu",
                        frameStaticMeshEntityCount,
                        frameStaticMeshSubmitted,
                        frameStaticMeshDrawCalls);
                }
                const bool mperfMainChanged =
                    !previousMperfMain.initialized ||
                    previousMperfMain.drawcalls != frameStaticMeshDrawCalls ||
                    previousMperfMain.tris != frameStaticMeshTriangles;
                if (!QuietLogsForLodDiag() || mperfMainChanged)
                {
                    TraceDiagf("[MPERF] pass=main drawcalls=%zu tris=%zu",
                        frameStaticMeshDrawCalls,
                        frameStaticMeshTriangles);
                }
                previousMperfMain.drawcalls = frameStaticMeshDrawCalls;
                previousMperfMain.tris = frameStaticMeshTriangles;
                previousMperfMain.initialized = true;

                if (!QuietLogsForLodDiag())
                {
                    for (int cascade = 0; cascade < 4; ++cascade)
                        TraceDiagf("[MPERF] pass=shadow-cascade%d drawcalls=0 tris=0", cascade);
                    TraceDiag("[MPERF] pass=water-reflection drawcalls=0 tris=0");
                }

                const bool mperfOverrideChanged =
                    !previousMperfOverride.initialized ||
                    previousMperfOverride.uniformUpdates != frameStaticMeshUniformUpdates ||
                    previousMperfOverride.activeOverrideDraws != frameStaticMeshOverrideActiveDraws;
                if (!QuietLogsForLodDiag() || mperfOverrideChanged)
                {
                    TraceDiagf("[MPERF] mmat overrideUpdatesThisFrame=%zu activeOverrideDraws=%zu mode=every-frame descriptorAllocPerDraw=no bufferMapPerDraw=yes queueWaitPerDraw=no",
                        frameStaticMeshUniformUpdates,
                        frameStaticMeshOverrideActiveDraws);
                }
                previousMperfOverride.uniformUpdates = frameStaticMeshUniformUpdates;
                previousMperfOverride.activeOverrideDraws = frameStaticMeshOverrideActiveDraws;
                previousMperfOverride.initialized = true;

                const bool mperfMeshesChanged =
                    !previousMperfMeshes.initialized ||
                    previousMperfMeshes.total != frameStaticMeshEntityCount ||
                    previousMperfMeshes.culled != frameStaticMeshFrustumCulled ||
                    previousMperfMeshes.drawn != frameStaticMeshSubmitted;
                if (!QuietLogsForLodDiag() || mperfMeshesChanged)
                {
                    TraceDiagf("[MPERF] meshes total=%zu frustumCulled=%zu drawn=%zu cullEnabled=yes",
                        frameStaticMeshEntityCount,
                        frameStaticMeshFrustumCulled,
                        frameStaticMeshSubmitted);
                }
                previousMperfMeshes.total = frameStaticMeshEntityCount;
                previousMperfMeshes.culled = frameStaticMeshFrustumCulled;
                previousMperfMeshes.drawn = frameStaticMeshSubmitted;
                previousMperfMeshes.initialized = true;

                const bool instSummaryChanged =
                    !previousInstSummary.initialized ||
                    previousInstSummary.batches != frameStaticMeshBatches ||
                    previousInstSummary.draws != frameStaticMeshDrawCalls ||
                    previousInstSummary.instances != frameStaticMeshSubmitted ||
                    previousInstSummary.maxBatch != frameStaticMeshMaxBatchSize;
                if (!QuietLogsForLodDiag() || instSummaryChanged)
                {
                    TraceDiagf("[INST] batches=%zu instancedDraws=%zu instancesTotal=%zu maxBatchSize=%zu",
                        frameStaticMeshBatches,
                        frameStaticMeshDrawCalls,
                        frameStaticMeshSubmitted,
                        frameStaticMeshMaxBatchSize);
                }
                previousInstSummary.batches = frameStaticMeshBatches;
                previousInstSummary.draws = frameStaticMeshDrawCalls;
                previousInstSummary.instances = frameStaticMeshSubmitted;
                previousInstSummary.maxBatch = frameStaticMeshMaxBatchSize;
                previousInstSummary.initialized = true;

                const bool instBufferChanged =
                    !previousInstBuffer.initialized ||
                    previousInstBuffer.bytes != frameStaticMeshInstanceBufferBytes ||
                    frameStaticMeshInstanceBufferRebuilt;
                if (!QuietLogsForLodDiag() || instBufferChanged)
                {
                    TraceDiagf("[INST] instanceBufferBytes=%zu rebuiltThisFrame=%s",
                        frameStaticMeshInstanceBufferBytes,
                        frameStaticMeshInstanceBufferRebuilt ? "yes" : "no");
                }
                previousInstBuffer.bytes = frameStaticMeshInstanceBufferBytes;
                previousInstBuffer.initialized = true;

                if (!QuietLogsForLodDiag())
                {
                    TraceDiagf("[SPATIAL] frustumQuery nodesVisited=%u candidates=%u total=%u",
                        frameStaticMeshSpatialStats.nodesVisited,
                        frameStaticMeshSpatialStats.candidates,
                        frameStaticMeshSpatialStats.totalObjects);
                    const VkExtent2D mperfExtent = useOffscreenScene ? offscreenScene.GetExtent() : renderSize;
                    TraceDiagf("[MPERF] offscreen=%ux%u halfResTestFps=n/a boundHint=unknown",
                        mperfExtent.width,
                        mperfExtent.height);
                }
                TraceDiagf("[FRAME] summary frame=%llu imgui_render called=%s rmlui_render called=%s scene_render called=%s entity_count=%zu clear_color=(0.04,0.05,0.09,1.00) in_world=%d lobby=%d editor_open=%d swapchain=%ux%u",
                    static_cast<unsigned long long>(frameNumber),
                    frameImGuiRenderCalled ? "yes" : "no",
                    frameRmlUiRenderCalled ? "yes" : "no",
                    frameSceneRenderCalled ? "yes" : "no",
                    frameSceneEntityCount,
                    isInWorld ? 1 : 0,
                    runtimeSession->IsLobbyActive() ? 1 : 0,
                    runtimeSession->IsMapEditorOpen() ? 1 : 0,
                    renderSize.width,
                    renderSize.height);
            }
        }
        else
        {
            static uint32_t inactiveFrameLogs = 0;
            if (inactiveFrameLogs < 3)
            {
                ++inactiveFrameLogs;
                TraceDiagf("[FRAME] inactive: imgui_render called=no rmlui_render called=no scene_render called=no clear_color=(0.04,0.05,0.09,1.00) swapchain=%ux%u",
                    renderSize.width,
                    renderSize.height);
            }
        }
#if defined(IXTREEME_WITH_EDITOR)
        if (assetLibraryDiagFrameActive)
        {
            const auto assetDiscoveryEndBegin = std::chrono::steady_clock::now();
            AssetLibrary::EndMaterialDiscoveryFrame(assetLibraryDiagFrame);
            frameProfile.assetLibraryPollMs += MillisecondsBetween(assetDiscoveryEndBegin, std::chrono::steady_clock::now());
        }
#endif
        const auto submitPresentBegin = std::chrono::steady_clock::now();
        device.EndFrame();
        frameProfile.submitPresentMs = MillisecondsBetween(submitPresentBegin, std::chrono::steady_clock::now());
        frameProfile.totalCpuFrameMs = MillisecondsBetween(frameCpuStart, std::chrono::steady_clock::now());
        // Carry this frame's CPU profile + present mode into engineStats so next frame's
        // Performance panel shows the cost breakdown (works in Release, no debug-logs build).
        engineStats.presentUncapped = device.IsPresentUncapped();
        engineStats.cpuTotalMs = frameProfile.totalCpuFrameMs;
        engineStats.cpuSceneRenderMs = frameProfile.sceneRenderMs;
        engineStats.cpuEditorUiMs = frameProfile.editorUiRenderMs;
        engineStats.cpuSubmitPresentMs = frameProfile.submitPresentMs;
        engineStats.cpuEcsUpdateMs = frameProfile.ecsSystemsUpdateMs;
        engineStats.cpuAssetWatcherMs = frameProfile.assetWatcherPollMs;
#if defined(IXTREEME_WITH_EDITOR)
        if (dumpFrameProfileRequested)
        {
            dumpFrameProfileRequested = false;
            TraceDiagf("[FRAME-PROFILE] frame=%llu",
                static_cast<unsigned long long>(device.GetFrameNumber()));
            TraceDiagf("[FRAME-PROFILE]   asset_library_poll = %.3f ms",
                frameProfile.assetLibraryPollMs);
            TraceDiagf("[FRAME-PROFILE]   asset_watcher_poll = %.3f ms",
                frameProfile.assetWatcherPollMs);
            TraceDiagf("[FRAME-PROFILE]   ecs_systems_update = %.3f ms",
                frameProfile.ecsSystemsUpdateMs);
            TraceDiagf("[FRAME-PROFILE]   hierarchy_iteration = %.3f ms",
                frameProfile.hierarchyIterationMs);
            TraceDiagf("[FRAME-PROFILE]   scene_render = %.3f ms",
                frameProfile.sceneRenderMs);
            TraceDiagf("[FRAME-PROFILE]   editor_ui_render = %.3f ms",
                frameProfile.editorUiRenderMs);
            TraceDiagf("[FRAME-PROFILE]   submit_present = %.3f ms",
                frameProfile.submitPresentMs);
            TraceDiagf("[FRAME-PROFILE]   total_cpu_frame = %.3f ms",
                frameProfile.totalCpuFrameMs);
            TraceDiagf("[FRAME-PROFILE]   reported_fps = %.1f",
                engineStats.fps);
        }
#endif
#if defined(IXTREEME_DEBUG_LOGS)
        {
            VulkanDevice::GpuTimestampResults gpuTiming{};
            VulkanDevice::CpuFrameTimingResults cpuTiming{};
            if (device.ConsumeGpuFrameCaptureResults(gpuTiming, cpuTiming))
            {
                const TerrainRenderer::FrameDrawStats terrainDrawStats = terrain.GetFrameDrawStats();
                auto elapsedMs = [&](VulkanDevice::GpuTimestampPoint begin, VulkanDevice::GpuTimestampPoint end) -> double {
                    const uint32_t beginIndex = static_cast<uint32_t>(begin);
                    const uint32_t endIndex = static_cast<uint32_t>(end);
                    if (beginIndex >= VulkanDevice::GpuTimestampPointCount ||
                        endIndex >= VulkanDevice::GpuTimestampPointCount ||
                        !gpuTiming.pointValid[beginIndex] ||
                        !gpuTiming.pointValid[endIndex])
                    {
                        return 0.0;
                    }
                    return std::max(0.0, gpuTiming.pointMs[endIndex] - gpuTiming.pointMs[beginIndex]);
                };
                const double shadowCascadeMs[4] = {
                    elapsedMs(VulkanDevice::GpuTimestampPoint::ShadowCascade0Begin, VulkanDevice::GpuTimestampPoint::ShadowCascade0End),
                    elapsedMs(VulkanDevice::GpuTimestampPoint::ShadowCascade1Begin, VulkanDevice::GpuTimestampPoint::ShadowCascade1End),
                    elapsedMs(VulkanDevice::GpuTimestampPoint::ShadowCascade2Begin, VulkanDevice::GpuTimestampPoint::ShadowCascade2End),
                    elapsedMs(VulkanDevice::GpuTimestampPoint::ShadowCascade3Begin, VulkanDevice::GpuTimestampPoint::ShadowCascade3End),
                };
                const double shadowTotalMs = elapsedMs(VulkanDevice::GpuTimestampPoint::ShadowPassBegin, VulkanDevice::GpuTimestampPoint::ShadowPassEnd);
                const double waterReflectionMs = elapsedMs(VulkanDevice::GpuTimestampPoint::WaterReflectionBegin, VulkanDevice::GpuTimestampPoint::WaterReflectionEnd);
                const double terrainMainMs = elapsedMs(VulkanDevice::GpuTimestampPoint::TerrainMainBegin, VulkanDevice::GpuTimestampPoint::TerrainMainEnd);
                const double sceneOtherMs = elapsedMs(VulkanDevice::GpuTimestampPoint::SceneOtherBegin, VulkanDevice::GpuTimestampPoint::SceneOtherEnd);
                const double compositeMs = elapsedMs(VulkanDevice::GpuTimestampPoint::CompositeBegin, VulkanDevice::GpuTimestampPoint::CompositeEnd);
                const double rmluiMs = elapsedMs(VulkanDevice::GpuTimestampPoint::RmlUiBegin, VulkanDevice::GpuTimestampPoint::RmlUiEnd);
                const double imguiMs = elapsedMs(VulkanDevice::GpuTimestampPoint::ImGuiBegin, VulkanDevice::GpuTimestampPoint::ImGuiEnd);
                const double totalGpuMs = elapsedMs(VulkanDevice::GpuTimestampPoint::FrameBegin, VulkanDevice::GpuTimestampPoint::FrameEnd);
                const TerrainRenderer::PassDrawStats& terrainMain = terrainDrawStats.terrainMain;
                const TerrainRenderer::PassDrawStats& reflection = terrainDrawStats.waterReflection;
                TraceDiagf("[GPU-TIME] frame=%llu",
                    static_cast<unsigned long long>(gpuTiming.frameNumber));
                TraceDiagf("[GPU-TIME]   shadow_pass_total = %.3f ms", shadowTotalMs);
                for (uint32_t cascade = 0; cascade < 4; ++cascade)
                    TraceDiagf("[GPU-TIME]     shadow_cascade_%u = %.3f ms", cascade, shadowCascadeMs[cascade]);
                TraceDiagf("[GPU-TIME]   water_reflection_pass = %.3f ms", waterReflectionMs);
                TraceDiagf("[GPU-TIME]   terrain_main_render = %.3f ms chunks_drawn=%u draw_calls=%u",
                    terrainMainMs,
                    terrainMain.chunksDrawn,
                    terrainMain.drawCalls);
                TraceDiagf("[GPU-TIME]   scene_render_other = %.3f ms", sceneOtherMs);
                TraceDiagf("[GPU-TIME]   imgui_render = %.3f ms", imguiMs);
                TraceDiagf("[GPU-TIME]   rmlui_render = %.3f ms", rmluiMs);
                TraceDiagf("[GPU-TIME]   tonemapping_composite = %.3f ms", compositeMs);
                TraceDiagf("[GPU-TIME]   total_gpu_time = %.3f ms", totalGpuMs);

                TraceDiagf("[DRAW-CALL] frame=%llu",
                    static_cast<unsigned long long>(gpuTiming.frameNumber));
                for (uint32_t cascade = 0; cascade < 4; ++cascade)
                {
                    const TerrainRenderer::PassDrawStats& stats = terrainDrawStats.shadowCascades[cascade];
                    TraceDiagf("[DRAW-CALL]   shadow_cascade_%u_draw_calls = %u chunks_drawn = %u chunks_culled = %u%s",
                        cascade,
                        stats.drawCalls,
                        stats.chunksDrawn,
                        stats.chunksCulled,
                        stats.skipped ? " skipped" : "");
                }
                if (reflection.skipped)
                {
                    TraceDiag("[DRAW-CALL]   water_reflection_draw_calls = skipped");
                }
                else
                {
                    TraceDiagf("[DRAW-CALL]   water_reflection_draw_calls = %u chunks_drawn = %u chunks_culled = %u",
                        reflection.drawCalls,
                        reflection.chunksDrawn,
                        reflection.chunksCulled);
                }
                TraceDiagf("[DRAW-CALL]   terrain_main_draw_calls = %u chunks_drawn = %u chunks_culled = %u%s",
                    terrainMain.drawCalls,
                    terrainMain.chunksDrawn,
                    terrainMain.chunksCulled,
                    terrainMain.skipped ? " skipped" : "");

                TraceDiagf("[CPU-TIME] frame=%llu",
                    static_cast<unsigned long long>(cpuTiming.frameNumber));
                TraceDiagf("[CPU-TIME]   acquire_image_ms = %.3f", cpuTiming.acquireImageMs);
                TraceDiagf("[CPU-TIME]   wait_for_fences_ms = %.3f", cpuTiming.waitForFencesMs);
                TraceDiagf("[CPU-TIME]   render_loop_cpu_work_ms = %.3f", cpuTiming.renderLoopCpuWorkMs);
                TraceDiagf("[CPU-TIME]   submit_ms = %.3f", cpuTiming.submitMs);
                TraceDiagf("[CPU-TIME]   present_ms = %.3f", cpuTiming.presentMs);
                TraceDiagf("[CPU-TIME]   total_cpu_frame_ms = %.3f", cpuTiming.totalCpuFrameMs);
            }
        }
#endif
    }

    device.WaitIdle();
    if (worldLabelsOk)
        worldLabels.Destroy();
    if (selectionOutlinesOk)
        selectionOutlines.Destroy();
    for (auto& [path, entry] : staticMeshCache)
    {
        (void)path;
        if (entry.renderer)
            entry.renderer->Destroy();
    }
    if (offscreenSceneOk)
        offscreenScene.Destroy();
        gameView.Destroy();
    if (terrainOk)
        terrain.Destroy();
    for (auto& [skinnedPath, skinnedEntry] : skinnedMeshCache)
    {
        (void)skinnedPath;
        if (skinnedEntry.renderer)
            skinnedEntry.renderer->Destroy();
    }
    skinnedMeshCache.clear();
#if defined(IXTREEME_WITH_EDITOR)
    editorImGui.Destroy();
#if defined(_WIN32)
    if (NativeWindow_Win32* cleanupWin32Window = dynamic_cast<NativeWindow_Win32*>(&window))
        cleanupWin32Window->SetMessageCallback({});
#endif
#endif
    rmlUi.Destroy();
    runtimeSession->Destroy();
    device.Destroy();
    return 0;
}
}

std::filesystem::path ResolveIxtreemeEngineAssetRoot()
{
    return ResolveEngineAssetRoot();
}

int RunIxtreemeEngine(NativeWindow& window,
                      client::asset::IAssetReader& assets)
{
    return RunGame(window, assets);
}
