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
#include "EditorImGui.h"
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
        target.sceneGizmoEntityType = source.sceneGizmoEntityType;
        target.sceneGizmoEntityId = source.sceneGizmoEntityId;
        std::copy(std::begin(source.sceneGizmoPosition), std::end(source.sceneGizmoPosition), std::begin(target.sceneGizmoPosition));
        std::copy(std::begin(source.sceneGizmoRotation), std::end(source.sceneGizmoRotation), std::begin(target.sceneGizmoRotation));
        std::copy(std::begin(source.sceneGizmoScale), std::end(source.sceneGizmoScale), std::begin(target.sceneGizmoScale));
    }
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
    SkinnedMeshRenderer skinnedMesh;
    bool skinnedMeshOk = false;
    std::string loadedSkinnedMeshPath;
    Tracen("[MAIN] SkinnedMeshRenderer available; no default skinned mesh asset loaded");

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
        if (skinnedMeshOk)
        {
            skinnedMesh.SetMainRenderPass(offscreenScene.GetRenderPass());
            skinnedMesh.RecreatePipeline(device);
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
#endif
    }
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
    auto ensureSkinnedMeshLoaded = [&](const std::string& modelPath) {
        if (modelPath.empty())
            return false;
        if (skinnedMeshOk && loadedSkinnedMeshPath == modelPath)
            return true;
        skinnedMesh.Destroy();
        skinnedMeshOk = skinnedMesh.Create(device, assets, modelPath);
        if (skinnedMeshOk)
        {
            loadedSkinnedMeshPath = modelPath;
            if (offscreenSceneOk)
            {
                skinnedMesh.SetMainRenderPass(offscreenScene.GetRenderPass());
                skinnedMesh.RecreatePipeline(device);
            }
            Tracenf("[MESH-ENTITY] SkinnedMeshRenderer loaded: %s", modelPath.c_str());
        }
        else
        {
            loadedSkinnedMeshPath.clear();
            TraceError("[MESH-ENTITY] Failed to load model: %s", modelPath.c_str());
        }
        return skinnedMeshOk;
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
        if (skinnedMeshOk)
        {
            skinnedMesh.SetMainRenderPass(offscreenScene.GetRenderPass());
            skinnedMesh.RecreatePipeline(device);
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
    auto removeStaticMeshSpatialEntity = [&](std::uint32_t id) {
        staticMeshSelectedLods.erase(id);
        staticMeshLodDispositionStates.erase(id);
        if (staticMeshSpatialIndexed.erase(id) > 0)
            staticMeshSpatialIndex.Remove(id);
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
        [](MeshSceneEntity& mesh) { EnsureMeshEntityMaterialSlots(mesh); }});
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
        if (editorImGui.WantsInputCapture(event) && !sceneViewInputTarget && !editorFlyCameraKey)
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
                        if (skinnedMeshOk)
                            skinnedMesh.SetMainRenderPass(offscreenScene.GetRenderPass());
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
                if (skinnedMeshOk)
                    skinnedMesh.RecreatePipeline(device);
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
        }
#endif
        {
            const auto ecsUpdateBegin = std::chrono::steady_clock::now();
            runtimeSession->UpdateNetwork();
            runtimeSession->SendMoveInput(movement.DirectionAngle(cameraController.MovementYaw()), RuntimeMoveState::Idle);
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
                    Tracenf("[FRAME-PROFILE] toggles shadow=%s water_reflection=%s asset_library_discovery=%s asset_watcher_poll=%s hierarchy_iteration=%s",
                        debugDisableShadowPass ? "disabled" : "enabled",
                        debugDisableWaterReflectionPass ? "disabled" : "enabled",
                        debugDisableAssetLibraryDiscovery ? "disabled" : "enabled",
                        debugDisableAssetWatcherPoll ? "disabled" : "enabled",
                        debugDisableHierarchyIteration ? "disabled" : "enabled");
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
                if (commands.enterPlayMode)
                {
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
                    cameraController.SetFreeCameraEnabled(true);
                    runtimeSession->Start(SceneManager::Instance().GetCurrentScene());
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
                        if (ext != ".fbx")
                            return false;
                        const std::filesystem::path skeleton =
                            path.parent_path() / (path.stem().string() + "_skeleton.ozz");
                        return std::filesystem::exists(skeleton);
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
                        commands.addComponentType == EditorComponentType::CapsuleCollider)
                    {
                        if (selectedEditorObject.type == SelectedEditorObjectType::MeshEntity)
                        {
                            auto it = std::find_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                                [&](const MeshSceneEntity& mesh) { return mesh.id == selectedEditorObject.id; });
                            if (it != editorMeshEntities.end())
                            {
                                if (commands.addComponentType == EditorComponentType::Rigidbody)
                                {
                                    it->hasRigidbody = true;
                                    it->rigidbody = {};
                                    if (!it->hasCollider)
                                    {
                                        it->hasCollider = true;
                                        it->collider.shape = ixtreeme::physics::ColliderShape::Box;
                                    }
                                    Tracenf("[INSPECTOR-COMP] add entity=%u component=Rigidbody", it->id);
                                    runtimeSession->SetEditorStatus("Added Rigidbody component");
                                }
                                else
                                {
                                    it->hasCollider = true;
                                    if (commands.addComponentType == EditorComponentType::SphereCollider)
                                        it->collider.shape = ixtreeme::physics::ColliderShape::Sphere;
                                    else if (commands.addComponentType == EditorComponentType::CapsuleCollider)
                                        it->collider.shape = ixtreeme::physics::ColliderShape::Capsule;
                                    else
                                        it->collider.shape = ixtreeme::physics::ColliderShape::Box;
                                    Tracenf("[INSPECTOR-COMP] add entity=%u component=%s Collider",
                                        it->id,
                                        ixtreeme::physics::ToString(it->collider.shape));
                                    runtimeSession->SetEditorStatus("Added Collider component");
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
                if (commands.selectedMeshEntityChanged)
                {
                    auto it = std::find_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                        [&](const MeshSceneEntity& mesh) { return mesh.id == commands.selectedMeshEntity.id; });
                    if (it != editorMeshEntities.end())
                    {
                        const bool transformChanged =
                            !std::equal(std::begin(it->position), std::end(it->position), std::begin(commands.selectedMeshEntity.position)) ||
                            !std::equal(std::begin(it->rotation), std::end(it->rotation), std::begin(commands.selectedMeshEntity.rotation)) ||
                            !std::equal(std::begin(it->scale), std::end(it->scale), std::begin(commands.selectedMeshEntity.scale));
                        const bool meshAssetChanged =
                            it->meshAssetId != commands.selectedMeshEntity.meshAssetId ||
                            it->meshAssetPath != commands.selectedMeshEntity.meshAssetPath ||
                            it->skinned != commands.selectedMeshEntity.skinned;
                        ApplyMeshRendererEditorState(*it, commands.selectedMeshEntity);
                        if (transformChanged || meshAssetChanged)
                            syncStaticMeshSpatialEntity(*it);
                        selectedEditorObject = {SelectedEditorObjectType::MeshEntity, it->id};
                        SceneManager::Instance().MarkDirty();
                    }
                }
                if (commands.dumpMaterialState)
                {
                    Tracenf("[MATBIND-DIAG] dump requested meshEntities=%zu", editorMeshEntities.size());
                    for (const MeshSceneEntity& mesh : editorMeshEntities)
                    {
                        const std::string runtimePath = resolveMeshRuntimePath(mesh);
                        if (mesh.skinned)
                        {
                            const bool loaded = ensureSkinnedMeshLoaded(runtimePath);
                            const std::uint32_t submeshCount = loaded
                                ? skinnedMesh.MaterialSlotCount()
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
                        const std::string runtimePath = resolveMeshRuntimePath(*meshIt);
                        if (meshIt->skinned)
                        {
                            if (ensureSkinnedMeshLoaded(runtimePath))
                                meshRendererState.materialSlotCount = std::max<std::uint32_t>(1u, skinnedMesh.MaterialSlotCount());
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
                if (skinnedMeshOk)
                {
                    skinnedMesh.SetLightingState(lightingState);
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

                if (skinnedMeshOk)
                {
                    uint32_t skinSlot = 0;
                    for (const auto& entity : entities)
                    {
                        if (skinSlot >= SkinnedMeshRenderer::MaxSkinSlots())
                            break;
                        skinnedMesh.SkinInstance(device,
                            skinSlot,
                            ToSkinnedMeshMotion(entity.moveState),
                            static_cast<float>(seconds));
                        ++skinSlot;
                    }
                    if (runtimeSession->IsMapEditorOpen())
                    {
                        const size_t editorVisualRenderCount =
                            editorPointLights.size() + editorSpotLights.size() + editorMeshEntities.size();
                        for (size_t visualIndex = 0; visualIndex < editorVisualRenderCount; ++visualIndex)
                        {
                            if (skinSlot >= SkinnedMeshRenderer::MaxSkinSlots())
                                break;
                            skinnedMesh.SkinInstance(device,
                                skinSlot,
                                SkinnedMeshRenderer::MotionState::Idle,
                                static_cast<float>(seconds));
                            ++skinSlot;
                        }
                    }
                }
            }
            else if (runtimeSession->IsLobbyActive() && skinnedMeshOk)
            {
                skinnedMesh.Skin(device, seconds);
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
                        if (!skinnedMeshOk)
                            return;

                        uint32_t skinSlot = 0;
                        for (const auto& entity : entities)
                        {
                            if (skinSlot >= SkinnedMeshRenderer::MaxSkinSlots())
                                break;
                            auto position = ServerMetersToDisplay(entity.position);
                            position.y += skinnedMesh.GroundOffsetY();
                            const std::array<float, 4> tint = entity.visualClassId == 0
                                ? std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f}
                                : (entity.visualClassId == 1
                                      ? std::array<float, 4>{1.35f, 0.55f, 0.55f, 1.0f}
                                      : std::array<float, 4>{0.65f, 0.95f, 1.35f, 1.0f});
                            skinnedMesh.RenderInWorldReflection(device,
                                mirrorCamera,
                                reflectionExtent,
                                reflectionRenderPass,
                                waterLevelY,
                                position,
                                HeadingFromQuantized(entity.heading),
                                skinSlot,
                                tint);
                            ++skinSlot;
                        }

                        if (runtimeSession->IsMapEditorOpen())
                        {
                            for (const auto& light : editorPointLights)
                            {
                                if (editorPlay.state.mode == EditorPlayMode::Edit && light.editorHidden)
                                    continue;
                                if (skinSlot >= SkinnedMeshRenderer::MaxSkinSlots())
                                    break;
                                WorldVec3 position{light.position[0], light.position[1] + skinnedMesh.GroundOffsetY(), light.position[2]};
                                const bool selected =
                                    selectedEditorObject.type == SelectedEditorObjectType::PointLight &&
                                    selectedEditorObject.id == light.id;
                                skinnedMesh.RenderInWorldReflection(device,
                                    mirrorCamera,
                                    reflectionExtent,
                                    reflectionRenderPass,
                                    waterLevelY,
                                    position,
                                    0.0f,
                                    skinSlot,
                                    selected
                                        ? std::array<float, 4>{2.0f, 1.55f, 0.25f, 1.0f}
                                        : std::array<float, 4>{1.6f, 1.05f, 0.35f, 1.0f});
                                ++skinSlot;
                            }
                            for (const auto& light : editorSpotLights)
                            {
                                if (editorPlay.state.mode == EditorPlayMode::Edit && light.editorHidden)
                                    continue;
                                if (skinSlot >= SkinnedMeshRenderer::MaxSkinSlots())
                                    break;
                                WorldVec3 position{light.position[0], light.position[1] + skinnedMesh.GroundOffsetY(), light.position[2]};
                                const bool selected =
                                    selectedEditorObject.type == SelectedEditorObjectType::SpotLight &&
                                    selectedEditorObject.id == light.id;
                                skinnedMesh.RenderInWorldReflection(device,
                                    mirrorCamera,
                                    reflectionExtent,
                                    reflectionRenderPass,
                                    waterLevelY,
                                    position,
                                    light.rotation[1],
                                    skinSlot,
                                    selected
                                        ? std::array<float, 4>{0.35f, 1.7f, 2.0f, 1.0f}
                                        : std::array<float, 4>{0.35f, 1.25f, 1.65f, 1.0f});
                                ++skinSlot;
                            }
                        }
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
                uint32_t skinSlot = 0;
                static bool loggedTerrainAlignment = false;
                for (const auto& entity : entities)
                {
                    auto position = ServerMetersToDisplay(entity.position);
                    const float terrainY = hasSceneTerrain ? terrain.SampleHeight(position) : position.y;
                    const float groundOffsetY = skinnedMeshOk ? skinnedMesh.GroundOffsetY() : 0.0f;
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
                            groundOffsetY);
                        loggedTerrainAlignment = true;
                    }
                    if (skinnedMeshOk && skinSlot < SkinnedMeshRenderer::MaxSkinSlots())
                    {
                        position.y += groundOffsetY;
                        const std::array<float, 4> tint = entity.visualClassId == 0
                            ? std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f}
                            : (entity.visualClassId == 1
                                  ? std::array<float, 4>{1.35f, 0.55f, 0.55f, 1.0f}
                                  : std::array<float, 4>{0.65f, 0.95f, 1.35f, 1.0f});
                        skinnedMesh.RenderInWorld(device,
                            seconds,
                            camera,
                            position,
                            HeadingFromQuantized(entity.heading),
                            skinSlot,
                            tint,
                            renderSize);
                    }
                    plates.push_back(WorldLabelRenderer::Label{
                        position + WorldVec3{0.0f, 2.2f, 0.0f},
                        entity.name,
                        entity.netId == selectedTargetNetId
                            ? std::array<float, 4>{1.0f, 0.86f, 0.32f, 1.0f}
                            : std::array<float, 4>{0.92f, 0.96f, 1.0f, 1.0f},
                        entity.netId == selectedTargetNetId});
                    ++skinSlot;
                }
                if (skinnedMeshOk && runtimeSession->IsMapEditorOpen())
                {
                    for (const auto& light : editorPointLights)
                    {
                        if (editorPlay.state.mode == EditorPlayMode::Edit && light.editorHidden)
                            continue;
                        if (skinSlot >= SkinnedMeshRenderer::MaxSkinSlots())
                            break;
                        WorldVec3 position{light.position[0], light.position[1] + skinnedMesh.GroundOffsetY(), light.position[2]};
                        const bool selected =
                            selectedEditorObject.type == SelectedEditorObjectType::PointLight &&
                            selectedEditorObject.id == light.id;
                        skinnedMesh.RenderInWorld(device,
                            seconds,
                            camera,
                            position,
                            0.0f,
                            skinSlot,
                            selected
                                ? std::array<float, 4>{2.0f, 1.55f, 0.25f, 1.0f}
                                : std::array<float, 4>{1.6f, 1.05f, 0.35f, 1.0f},
                            renderSize);
                        ++skinSlot;
                    }
                    for (const auto& light : editorSpotLights)
                    {
                        if (editorPlay.state.mode == EditorPlayMode::Edit && light.editorHidden)
                            continue;
                        if (skinSlot >= SkinnedMeshRenderer::MaxSkinSlots())
                            break;
                        WorldVec3 position{light.position[0], light.position[1] + skinnedMesh.GroundOffsetY(), light.position[2]};
                        const bool selected =
                            selectedEditorObject.type == SelectedEditorObjectType::SpotLight &&
                            selectedEditorObject.id == light.id;
                        skinnedMesh.RenderInWorld(device,
                            seconds,
                            camera,
                            position,
                            light.rotation[1],
                            skinSlot,
                            selected
                                ? std::array<float, 4>{0.35f, 1.7f, 2.0f, 1.0f}
                                : std::array<float, 4>{0.35f, 1.25f, 1.65f, 1.0f},
                            renderSize);
                        ++skinSlot;
                    }
                }
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
                        const bool selected =
                            selectedEditorObject.type == SelectedEditorObjectType::MeshEntity &&
                            selectedEditorObject.id == mesh.id;
                        const std::string runtimePath = resolveMeshRuntimePath(mesh);
                        if (mesh.skinned)
                        {
                            const std::size_t beforeSlotCount = meshPtr->materialSlots.size();
                            EnsureMeshEntityMaterialSlots(*meshPtr);
                            if (meshPtr->materialSlots.size() != beforeSlotCount)
                            {
                                SceneManager::Instance().MarkDirty();
                            }
                            if (ensureSkinnedMeshLoaded(runtimePath))
                            {
                                const auto lookup = editorMeshEntityLookup.find(mesh.id);
                                const std::uint32_t meshVisualIndex = lookup != editorMeshEntityLookup.end()
                                    ? static_cast<std::uint32_t>(lookup->second)
                                    : 0u;
                                std::uint32_t editorMeshSkinSlot = static_cast<std::uint32_t>(
                                    entities.size() + editorPointLights.size() + editorSpotLights.size()) + meshVisualIndex;
                                if (editorMeshSkinSlot >= SkinnedMeshRenderer::MaxSkinSlots())
                                    editorMeshSkinSlot = 0;
                                skinnedMesh.SkinInstance(device,
                                    editorMeshSkinSlot,
                                    SkinnedMeshRenderer::MotionState::Idle,
                                    static_cast<float>(seconds));
                                skinnedMesh.RenderInWorld(device,
                                    seconds,
                                    camera,
                                    {mesh.position[0], mesh.position[1] + skinnedMesh.GroundOffsetY(), mesh.position[2]},
                                    mesh.rotation[1],
                                    editorMeshSkinSlot,
                                    selected
                                        ? std::array<float, 4>{1.25f, 1.15f, 0.65f, 1.0f}
                                        : std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f},
                                    renderSize);
                                ++frameStaticMeshDrawCalls;
                            }
                            continue;
                        }
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
                                if ((!QuietLogsForLodDiag() && (frameNumber < 3 || (frameNumber % 60u) == 0u)) ||
                                    (QuietLogsForLodDiag() && cullLogChanged))
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
            else if (runtimeSession->IsLobbyActive() && skinnedMeshOk)
            {
                frameSceneRenderCalled = true;
                frameSceneEntityCount = 1;
                device.WriteGpuTimestamp(VulkanDevice::GpuTimestampPoint::SceneOtherBegin);
                skinnedMesh.Render(device, seconds);
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
    if (terrainOk)
        terrain.Destroy();
    if (skinnedMeshOk)
        skinnedMesh.Destroy();
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
