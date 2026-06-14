#include "TreePreviewRenderer.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace tree_tool
{
namespace
{
ImU32 Color(const std::array<float, 4>& value)
{
    const auto byte = [](float v) {
        return static_cast<int>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return IM_COL32(byte(value[0]), byte(value[1]), byte(value[2]), byte(value[3]));
}

ImVec2 Project(const ixtreemetree::Vec3& p,
               const ixtreemetree::TreeMesh& mesh,
               ImVec2 origin,
               ImVec2 size,
               float yaw,
               float pitch,
               float zoom)
{
    const float cy = std::cos(yaw);
    const float sy = std::sin(yaw);
    const float cp = std::cos(pitch);
    const float sp = std::sin(pitch);
    const float x = p.x * cy - p.z * sy;
    const float z = p.x * sy + p.z * cy;
    const float y = p.y * cp - z * sp;
    const float treeHeight = std::max(0.1f, mesh.bboxMax.y - mesh.bboxMin.y);
    const float scale = (std::min(size.x, size.y) * 0.78f * zoom) / treeHeight;
    return {origin.x + size.x * 0.5f + x * scale, origin.y + size.y * 0.78f - y * scale};
}
}

void TreePreviewRenderer::ResetView()
{
    yaw_ = -0.65f;
    pitch_ = 0.25f;
    zoom_ = 1.0f;
}

void TreePreviewRenderer::Render(const ixtreemetree::TreeMesh& mesh, float width, float height, const TreePreviewStyle& style)
{
    const ImVec2 imageSize(std::max(128.0f, width), std::max(160.0f, height));
    ImGui::InvisibleButton("##TreePreviewCanvas", imageSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const ImVec2 size(max.x - min.x, max.y - min.y);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, max, IM_COL32(28, 32, 40, 255), 6.0f);
    draw->AddRect(min, max, IM_COL32(90, 100, 120, 255), 6.0f);

    if (hovered)
    {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
            zoom_ = std::clamp(zoom_ + wheel * 0.08f, 0.45f, 2.4f);
    }
    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
    {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        yaw_ += delta.x * 0.01f;
        pitch_ = std::clamp(pitch_ + delta.y * 0.008f, -1.0f, 1.0f);
    }

    const ImU32 barkColor = Color(style.barkColor);
    const ImU32 leafColor = Color(style.leafColor);

    for (std::size_t i = 0; i + 1u < mesh.bark.indices.size(); i += 6u)
    {
        const auto& a = mesh.bark.vertices[mesh.bark.indices[i + 0u]].position;
        const auto& b = mesh.bark.vertices[mesh.bark.indices[i + 1u]].position;
        draw->AddLine(Project(a, mesh, min, size, yaw_, pitch_, zoom_),
            Project(b, mesh, min, size, yaw_, pitch_, zoom_),
            barkColor,
            1.0f);
    }

    const std::size_t leafStep = std::max<std::size_t>(1u, mesh.leaves.vertices.size() / 300u);
    for (std::size_t i = 0; i < mesh.leaves.vertices.size(); i += leafStep)
    {
        const ImVec2 p = Project(mesh.leaves.vertices[i].position, mesh, min, size, yaw_, pitch_, zoom_);
        draw->AddCircleFilled(p, 2.2f, leafColor);
    }

    if (mesh.bark.vertices.empty())
        draw->AddText(ImVec2(min.x + 16.0f, min.y + 16.0f), IM_COL32(200, 205, 214, 255), "No preview mesh");
}
}
