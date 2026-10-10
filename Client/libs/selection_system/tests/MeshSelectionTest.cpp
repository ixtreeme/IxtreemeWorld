#include "MeshSurfaceQuery.h"
#include "SelectionSystem.h"

#include <array>
#include <cmath>
#include <iostream>

namespace xm = ixtreeme::math;
namespace
{
int checks = 0, failures = 0;
void Check(const char* name, bool value)
{
    ++checks;
    if (!value) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
}
}

int main()
{
    // Two disconnected triangles have a large bounding rectangle and empty space between them.
    const std::array<xm::Vec3, 6> vertices{{{-4, -2, 0}, {-2, -2, 0}, {-3, 2, 0},
        {2, -2, 0}, {4, -2, 0}, {3, 2, 0}}};
    const std::array<std::uint32_t, 6> indices{0, 1, 2, 3, 4, 5};
    auto cast = [&](xm::Ray ray, const xm::Mat4& model, float& distance) {
        return meshquery::RaycastSurface(ray, model, indices,
            [&](std::uint32_t index) { return vertices[index]; }, distance);
    };
    float distance = -1;
    Check("Bounding-box interior without geometry is not selectable",
        !cast({{0, 0, -10}, {0, 0, 1}}, xm::Mat4Identity(), distance));
    Check("Object surface is selectable", cast({{-3, 0, -10}, {0, 0, 1}}, xm::Mat4Identity(), distance));
    Check("Hit distance is the actual surface distance", std::abs(distance - 10) < 0.0001f);
    Check("Two-sided surfaces are selectable from behind",
        cast({{-3, 0, 10}, {0, 0, -1}}, xm::Mat4Identity(), distance));
    Check("Geometry behind the ray is ignored", !cast({{-3, 0, 10}, {0, 0, 1}}, xm::Mat4Identity(), distance));

    // World distance must be comparable across entities, not scaled local-space distance.
    for (float sign : {-1.0f, 1.0f})
        for (float rotation : {0.0f, 0.3f, 1.2f})
        {
            const auto model = xm::TRS({7, 4, 12}, xm::FromEulerRadians({rotation, rotation * .7f, rotation * .4f}),
                {sign * 2.0f, .4f, 3.0f});
            const auto point = xm::TransformPoint(model, {-3, 0, 0});
            const auto direction = xm::Normalize(xm::TransformVector(model, {0, 0, 1}));
            Check("Rotated, scaled and mirrored geometry remains selectable",
                cast({point - direction * 17.0f, direction}, model, distance));
            Check("Nonuniform scale preserves world distance", std::abs(distance - 17.0f) < 0.0001f);
            const auto empty = xm::TransformPoint(model, {0, 0, 0});
            Check("Transform does not fill empty bounding-box space",
                !cast({empty - direction * 17.0f, direction}, model, distance));
        }
    Check("Zero-scale objects do not produce false hits",
        !cast({{-3, 0, -10}, {0, 0, 1}}, xm::Scale({0, 1, 1}), distance));
    const std::array<xm::Vec3, 6> depths{{{-1, -1, 8}, {1, -1, 8}, {0, 1, 8},
        {-1, -1, 3}, {1, -1, 3}, {0, 1, 3}}};
    Check("Nearest triangle wins regardless of index order",
        meshquery::RaycastSurface({{0, 0, 0}, {0, 0, 1}}, xm::Mat4Identity(), indices,
            [&](std::uint32_t index) { return depths[index]; }, distance) && std::abs(distance - 3) < 0.0001f);

    // The CPU query uses exactly the GPU skinning weight normalization and bone transforms.
    const std::array<xm::Mat4, 2> palette{xm::Translation({5, 0, 0}), xm::Translation({0, 4, 0})};
    const auto posed = meshquery::SkinPosition({1, 2, 3}, 128u | (127u << 8), 1u << 8, palette);
    Check("Packed bone weights match GPU normalization", std::abs(posed.x - (1 + 5 * 128.0f / 255)) < 0.00001f &&
        std::abs(posed.y - (2 + 4 * 127.0f / 255)) < 0.00001f);
    Check("Unweighted vertices retain their rest position",
        xm::Distance(meshquery::SkinPosition({1, 2, 3}, 0, 0, palette), {1, 2, 3}) < 0.00001f);
    Check("Single-bone movement moves the clickable surface",
        xm::Distance(meshquery::SkinPosition({1, 2, 3}, 1, 0, palette), {6, 2, 3}) < 0.00001f);

    WorldCamera camera;
    camera.eye = {0, 0, -10};
    camera.target = {0, 0, 0};
    std::vector<MeshSceneEntity> meshes(2);
    meshes[0].id = 1; meshes[0].skinned = true;
    meshes[1].id = 2; meshes[1].skinned = true;
    auto pick = [&] {
        return PickMeshEntity(meshes, camera, 100, 100, 50, 50,
            [](const MeshSceneEntity&) { return std::string{}; },
            [](const std::string&) -> StaticMeshRenderer* { return nullptr; },
            [](const MeshSceneEntity& mesh, WorldVec3, WorldVec3, float& t) {
                t = mesh.id == 1 ? 20.0f : 10.0f; return true;
            });
    };
    Check("Nearest object surface wins", pick() == 2);
    meshes[1].editorHidden = true;
    Check("Hidden objects cannot intercept clicks", pick() == 1);
    meshes[0].effectiveEnabled = false;
    Check("Disabled objects cannot intercept clicks", !pick());
    const SelectedEditorObject selected{SelectedEditorObjectType::MeshEntity, 1};
    Check("Selected meshes have no selection box or proxy circle",
        BuildSelectionOutlineLines(selected, meshes, {}, {}, {}, camera, {}).empty());

    const float origin[3] = {2, 3, 4};
    auto empty = MakeEmptySceneEntity(77, "Dust emitter", origin);
    Check("Empty objects have no geometry, materials or collider",
        !empty.HasMeshGeometry() && empty.materialSlots.empty() && !empty.hasCollider && !empty.hasRigidbody);
    const auto emptyTarget = BuildSceneGizmoTarget({SelectedEditorObjectType::MeshEntity, 77},
        {empty}, {}, {}, {}, {}, [](const MeshSceneEntity&) -> StaticMeshRenderer* { return nullptr; });
    Check("Zero-scale empty object has a movable gizmo at its position",
        emptyTarget.visible && emptyTarget.position[0] == 2 && emptyTarget.position[1] == 3 &&
        emptyTarget.position[2] == 4 && emptyTarget.scale[0] == 0 && emptyTarget.scale[1] == 0 && emptyTarget.scale[2] == 0);
    const float movedPosition[3] = {10, 12, 14}, rotated[3] = {0.2f, 0.7f, 0.1f}, zeroScale[3] = {0, 0, 0};
    Check("Moving and rotating empty objects preserves zero scale",
        ApplySceneGizmoToMesh(empty, movedPosition, rotated, zeroScale, nullptr) &&
        empty.position[0] == 10 && empty.rotation[1] == 0.7f &&
        empty.scale[0] == 0 && empty.scale[1] == 0 && empty.scale[2] == 0);
    empty.meshAssetPath = "builtin://primitive/cube";
    Check("Models retain a safe nonzero minimum scale",
        ApplySceneGizmoToMesh(empty, movedPosition, rotated, zeroScale, nullptr) &&
        empty.scale[0] == 0.001f && empty.scale[1] == 0.001f && empty.scale[2] == 0.001f);

    std::cout << "Mesh selection: " << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
