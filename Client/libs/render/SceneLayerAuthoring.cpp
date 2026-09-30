#include "SceneLayerAuthoring.h"

#include "math/Quaternion.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <unordered_set>
#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {
namespace phys = ixtreeme::physics;
namespace math = ixtreeme::math;

bool Finite(const float* values, std::size_t size)
{
    return std::all_of(values, values + size, [](float value) { return std::isfinite(value); });
}

void AddBoxGeometry(const phys::ColliderComponent& collider,
    const float scale[3],
    std::vector<std::array<float, 3>>& vertices, std::vector<std::uint32_t>& indices)
{
    // Match Jolt's sanitized size and minimum world-space half extents.
    const float x = std::max(0.001f, std::max(0.001f, collider.size[0]) * scale[0] * 0.5f) / scale[0];
    const float y = std::max(0.001f, std::max(0.001f, collider.size[1]) * scale[1] * 0.5f) / scale[1];
    const float z = std::max(0.001f, std::max(0.001f, collider.size[2]) * scale[2] * 0.5f) / scale[2];
    vertices = {{-x,-y,-z}, {x,-y,-z}, {-x,y,-z}, {x,y,-z},
                {-x,-y,z}, {x,-y,z}, {-x,y,z}, {x,y,z}};
    // Outward winding in engine Y-up space, including walls and underside.
    indices = {2,6,7, 2,7,3, 0,1,5, 0,5,4, 0,2,3, 0,3,1,
               4,5,7, 4,7,6, 0,4,6, 0,6,2, 1,3,7, 1,7,5};
}
} // namespace

std::array<float, 3> EngineToLayerCoordinates(const std::array<float, 3>& point) noexcept
{
    return {point[0], point[2], point[1]};
}

std::array<float, 3> LayerToEngineCoordinates(const std::array<float, 3>& point) noexcept
{
    return {point[0], point[2], point[1]};
}

bool BuildLayerCollisionMesh(const MeshSceneEntity& mesh,
    const LayerCollisionGeometryProvider& geometryProvider,
    mx::map::LayerCollisionMesh& output, std::string& error)
{
    output = {};
    error.clear();
    const std::string label = "entity " + std::to_string(mesh.id) + " (" + mesh.name + "): ";
    auto reject = [&](const char* reason) { error = label + reason; return false; };
    if (mesh.id == 0 || mesh.skinned || mesh.hasCharacterController)
        return reject("layer sources must be identified static model instances");
    if (!mesh.hasCollider || !mesh.collider.enabled || mesh.collider.trigger ||
        mesh.collider.layer == phys::PhysicsLayer::NoCollision)
        return reject("an enabled, non-trigger collision source is required");
    // Engine physics promotes Static + gravity to Dynamic when rebuilding bodies.
    if (mesh.hasRigidbody && mesh.rigidbody.enabled &&
        (mesh.rigidbody.bodyType != phys::BodyType::Static || mesh.rigidbody.useGravity))
        return reject("dynamic/kinematic colliders cannot define static world layers");
    if (mesh.layerAuthoring.tags == 0 || (mesh.layerAuthoring.tags & ~mx::map::kKnownVolumeTags) != 0)
        return reject("select valid world tags before generating layers");
    if (!Finite(mesh.position, 3) || !Finite(mesh.rotation, 3) || !Finite(mesh.scale, 3) ||
        !Finite(mesh.collider.center, 3))
        return reject("collision transform contains non-finite values");
    std::vector<std::array<float, 3>> vertices;
    std::vector<std::uint32_t> indices;
    const float scale[3] = {std::max(0.001f, std::abs(mesh.scale[0])),
                           std::max(0.001f, std::abs(mesh.scale[1])),
                           std::max(0.001f, std::abs(mesh.scale[2]))};
    if (mesh.collider.shape == phys::ColliderShape::Box) {
        if (!Finite(mesh.collider.size, 3) || std::any_of(mesh.collider.size, mesh.collider.size + 3,
            [](float size) { return size <= 0; }))
            return reject("box collider dimensions must be finite and positive");
        AddBoxGeometry(mesh.collider, scale, vertices, indices);
    } else if (mesh.collider.shape == phys::ColliderShape::Mesh) {
        if (!geometryProvider || !geometryProvider(mesh, vertices, indices) || vertices.empty() || indices.empty())
            return reject("mesh collision triangles unavailable; export has no bounding-box fallback");
    } else {
        return reject("layer generation currently supports Box and Mesh collision shapes");
    }
    if (indices.size() % 3 != 0)
        return reject("collision index list is not a triangle list");
    if (vertices.size() > mx::map::kMaxLayerGeometryVertices ||
        indices.size() / 3 > mx::map::kMaxLayerGeometryTriangles)
        return reject("collision source exceeds the bounded layer cooking limit");
    const math::Quat rotation = math::FromEulerRadians({mesh.rotation[0], mesh.rotation[1], mesh.rotation[2]});
    // Jolt omits the translated shape wrapper for sub-threshold Box centers;
    // Mesh geometry embeds its scaled center without that threshold.
    const bool applyCenter = mesh.collider.shape == phys::ColliderShape::Mesh ||
        std::abs(mesh.collider.center[0] * scale[0]) > 0.0001f ||
        std::abs(mesh.collider.center[1] * scale[1]) > 0.0001f ||
        std::abs(mesh.collider.center[2] * scale[2]) > 0.0001f;
    output.source_id = mesh.id;
    output.name = mesh.name;
    output.tags = mesh.layerAuthoring.tags;
    output.supports_ground_movement = !mx::map::HasVolumeTag(output.tags, mx::map::VolumeTagWater) &&
        !mx::map::HasVolumeTag(output.tags, mx::map::VolumeTagUnderwater);
    output.vertices.reserve(vertices.size());
    for (const auto& vertex : vertices) {
        const math::Vec3 local{vertex[0] * scale[0] + (applyCenter ? mesh.collider.center[0] * scale[0] : 0),
                               vertex[1] * scale[1] + (applyCenter ? mesh.collider.center[1] * scale[1] : 0),
                               vertex[2] * scale[2] + (applyCenter ? mesh.collider.center[2] * scale[2] : 0)};
        const auto rotated = math::Rotate(rotation, local);
        output.vertices.push_back(EngineToLayerCoordinates(
            {rotated.x + mesh.position[0], rotated.y + mesh.position[1], rotated.z + mesh.position[2]}));
    }
    output.indices = std::move(indices);
    // Swapping Y/Z reverses handedness. Preserve the collider's front faces.
    for (std::size_t i = 0; i < output.indices.size(); i += 3)
        std::swap(output.indices[i + 1], output.indices[i + 2]);
    return true;
}

bool GenerateSceneLayers(const SceneData& scene,
    const LayerCollisionGeometryProvider& geometryProvider,
    SceneLayerAuthoringResult& result)
{
    result = {};
    std::vector<mx::map::LayerCollisionMesh> meshes;
    std::unordered_set<std::uint32_t> ids;
    std::size_t vertexCount = 0, triangleCount = 0;
    auto appendMesh = [&](mx::map::LayerCollisionMesh mesh) {
        if (meshes.size() >= mx::map::kMaxLayeredWorldVolumes ||
            mesh.vertices.size() > mx::map::kMaxLayerGeometryVertices - vertexCount ||
            mesh.indices.size() / 3 > mx::map::kMaxLayerGeometryTriangles - triangleCount) {
            result.errors.push_back("scene collision exceeds the bounded layer cooking limit");
            return false;
        }
        vertexCount += mesh.vertices.size();
        triangleCount += mesh.indices.size() / 3;
        meshes.push_back(std::move(mesh));
        return true;
    };
    for (const auto& entity : scene.meshEntities) {
        if (!entity.layerAuthoring.enabled) continue;
        mx::map::LayerCollisionMesh mesh;
        std::string error;
        if (!ids.insert(entity.id).second)
            result.errors.push_back("duplicate layer source entity id " + std::to_string(entity.id));
        else if (!BuildLayerCollisionMesh(entity, geometryProvider, mesh, error))
            result.errors.push_back(error);
        else
            if (!appendMesh(std::move(mesh))) return false;
    }
    // Water has an explicit surface and height already. A non-rectangular
    // shape mask cannot be flattened to a rectangle without inventing water.
    std::uint32_t waterSource = 1;
    std::vector<const WaterBody*> orderedWater;
    std::unordered_set<std::uint32_t> waterIds;
    for (const auto& water : scene.waterBodies) {
        if (water.id == 0 || !waterIds.insert(water.id).second)
            result.errors.push_back("water sources require unique nonzero entity ids");
        orderedWater.push_back(&water);
    }
    std::sort(orderedWater.begin(), orderedWater.end(), [](const auto* lhs, const auto* rhs) {
        return lhs->id < rhs->id;
    });
    for (const auto* waterPointer : orderedWater) {
        const auto& water = *waterPointer;
        while (ids.contains(waterSource) && waterSource != std::numeric_limits<std::uint32_t>::max())
            ++waterSource;
        if (ids.contains(waterSource)) {
            result.errors.push_back("no available water source identity");
            break;
        }
        if (water.shapeMask.empty() || water.maskWidth == 0 || water.maskHeight == 0 ||
            static_cast<std::uint64_t>(water.maskWidth) * water.maskHeight != water.shapeMask.size() ||
            std::any_of(water.shapeMask.begin(), water.shapeMask.end(), [](std::uint8_t cell) { return cell == 0; })) {
            result.errors.push_back("water " + std::to_string(water.id) + ": a complete rectangular surface mask is required; other shapes need partitioning");
            continue;
        }
        if (!Finite(water.bboxMin, 2) || !Finite(water.bboxMax, 2) || !std::isfinite(water.waterLevelY) ||
            water.bboxMin[0] >= water.bboxMax[0] || water.bboxMin[1] >= water.bboxMax[1]) {
            result.errors.push_back("water " + std::to_string(water.id) + ": invalid surface bounds");
            continue;
        }
        mx::map::LayerCollisionMesh mesh;
        mesh.source_id = waterSource;
        ids.insert(waterSource);
        mesh.name = water.name;
        mesh.tags = mx::map::VolumeTagWater;
        mesh.supports_ground_movement = false;
        mesh.vertices = {{water.bboxMin[0], water.bboxMin[1], water.waterLevelY},
                         {water.bboxMax[0], water.bboxMin[1], water.waterLevelY},
                         {water.bboxMax[0], water.bboxMax[1], water.waterLevelY},
                         {water.bboxMin[0], water.bboxMax[1], water.waterLevelY}};
        mesh.indices = {0,1,2, 0,2,3};
        if (!appendMesh(std::move(mesh))) return false;
    }
    if (!result.errors.empty()) return false;
    std::vector<mx::map::LayerSourceSurface> surfaces;
    if (!mx::map::ExtractLayerSourceSurfaces(meshes, {}, surfaces, result.geometry)) {
        result.errors = result.geometry.errors;
        return false;
    }
    if (scene.terrain.exists) {
        // Engine rendering and Jolt both center terrain on the world origin.
        const float halfWidth = scene.terrain.widthMeters * 0.5f;
        const float halfDepth = scene.terrain.depthMeters * 0.5f;
        result.worldBounds = {-halfWidth, -halfDepth, halfWidth, halfDepth};
    } else {
        result.worldBounds = surfaces.front().bounds;
        for (const auto& surface : surfaces) {
            result.worldBounds.min_x = std::min(result.worldBounds.min_x, surface.bounds.min_x);
            result.worldBounds.min_y = std::min(result.worldBounds.min_y, surface.bounds.min_y);
            result.worldBounds.max_x = std::max(result.worldBounds.max_x, surface.bounds.max_x);
            result.worldBounds.max_y = std::max(result.worldBounds.max_y, surface.bounds.max_y);
        }
    }
    mx::map::LayerGenerationOptions options;
    options.world_bounds = result.worldBounds;
    options.require_exact_footprints = true;
    if (!mx::map::GenerateLayeredWorld(surfaces, options, result.world, result.generation)) {
        result.errors = result.generation.errors;
        return false;
    }
    return true;
}

bool ExportSceneLayerSidecar(const std::filesystem::path& path,
    const SceneLayerAuthoringResult& result, std::string& error)
{
    error.clear();
    if (!result.errors.empty() || result.world.volumes.empty() || !result.world.Validate(result.worldBounds, error)) {
        if (error.empty()) error = "generate valid layer metadata before export";
        return false;
    }
    const auto bytes = mx::map::EncodeLayeredWorld(result.world);
    mx::map::LayeredWorld verified;
    if (bytes.empty() || !mx::map::DecodeLayeredWorld(bytes, verified, error) ||
        !verified.Validate(result.worldBounds, error)) {
        if (error.empty()) error = "layer metadata encoding failed";
        return false;
    }
    std::error_code ec;
    auto temporary = path;
    temporary += ".pending";
    if (std::filesystem::exists(path, ec) || ec || std::filesystem::exists(temporary, ec) || ec) {
        error = "refusing to overwrite existing layer export";
        return false;
    }
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) { error = "cannot create export directory: " + ec.message(); return false; }
#if defined(_WIN32)
    // Native Unicode path and exclusive creation: concurrent exports cannot
    // truncate a staging file, and MoveFileEx without REPLACE preserves final.
    const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { error = "cannot exclusively create layer export"; return false; }
    DWORD written = 0;
    const bool wrote = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
        written == bytes.size();
    const bool closed = CloseHandle(file) != 0;
    if (!wrote || !closed) {
        DeleteFileW(temporary.c_str());
        error = "failed writing layer export";
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), 0)) {
        DeleteFileW(temporary.c_str());
        error = "failed publishing layer export without replacement";
        return false;
    }
#else
    const int file = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (file < 0) { error = "cannot exclusively create layer export"; return false; }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto written = ::write(file, bytes.data() + offset, bytes.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) break;
        offset += static_cast<std::size_t>(written);
    }
    const bool closed = ::close(file) == 0;
    if (offset != bytes.size() || !closed || ::link(temporary.c_str(), path.c_str()) != 0) {
        ::unlink(temporary.c_str());
        error = "failed writing/publishing layer export without replacement";
        return false;
    }
    ::unlink(temporary.c_str());
#endif
    return true;
}
