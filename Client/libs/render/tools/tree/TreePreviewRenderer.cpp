#include "TreePreviewRenderer.h"

#include "math/IXMath.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace tree_tool
{
namespace
{
namespace xm = ixtreeme::math;

ImU32 Color(const std::array<float, 4>& value)
{
    const auto byte = [](float v) {
        return static_cast<int>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return IM_COL32(byte(value[0]), byte(value[1]), byte(value[2]), byte(value[3]));
}

struct ProjectedPoint
{
    ImVec2 screen;
    float depth = 0.0f;
};

struct PreviewTriangle
{
    ImVec2 a;
    ImVec2 b;
    ImVec2 c;
    float depth = 0.0f;
    ImU32 fill = 0;
    ImU32 line = 0;
    bool outline = false;
};

xm::Vec3 ToMath(const ixtreemetree::Vec3& v)
{
    return {v.x, v.y, v.z};
}

ixtreemetree::Vec3 ToTree(xm::Vec3 v)
{
    return {v.x, v.y, v.z};
}

ixtreemetree::Vec3 Sub(const ixtreemetree::Vec3& a, const ixtreemetree::Vec3& b)
{
    return ToTree(ToMath(a) - ToMath(b));
}

ixtreemetree::Vec3 Cross(const ixtreemetree::Vec3& a, const ixtreemetree::Vec3& b)
{
    return ToTree(xm::Cross(ToMath(a), ToMath(b)));
}

ixtreemetree::Vec3 Normalize(ixtreemetree::Vec3 v)
{
    return ToTree(xm::SafeNormalize(ToMath(v), {0.0f, 1.0f, 0.0f}));
}

float Dot(const ixtreemetree::Vec3& a, const ixtreemetree::Vec3& b)
{
    return xm::Dot(ToMath(a), ToMath(b));
}

std::array<float, 4> ShadeColor(std::array<float, 4> color, float shade)
{
    color[0] = std::clamp(color[0] * shade, 0.0f, 1.0f);
    color[1] = std::clamp(color[1] * shade, 0.0f, 1.0f);
    color[2] = std::clamp(color[2] * shade, 0.0f, 1.0f);
    return color;
}

ProjectedPoint Project(const ixtreemetree::Vec3& p,
                       const ixtreemetree::Vec3& center,
                       float radius,
                       ImVec2 origin,
                       ImVec2 size,
                       float yaw,
                       float pitch,
                       float zoom)
{
    const float cy = xm::Cos(yaw);
    const float sy = xm::Sin(yaw);
    const float cp = xm::Cos(pitch);
    const float sp = xm::Sin(pitch);
    const ixtreemetree::Vec3 local = Sub(p, center);
    const float x = local.x * cy - local.z * sy;
    const float z = local.x * sy + local.z * cy;
    const float y = local.y * cp - z * sp;
    const float depth = local.y * sp + z * cp;
    const float scale = (std::min(size.x, size.y) * 0.45f * zoom) / std::max(radius, 0.1f);
    return {{origin.x + size.x * 0.5f + x * scale, origin.y + size.y * 0.52f - y * scale}, depth};
}

void PushSurfaceTriangles(std::vector<PreviewTriangle>& triangles,
                          const std::vector<ixtreemetree::Vertex>& vertices,
                          const std::vector<std::uint32_t>& indices,
                          const ixtreemetree::Vec3& center,
                          float radius,
                          ImVec2 origin,
                          ImVec2 size,
                          float yaw,
                          float pitch,
                          float zoom,
                          const std::array<float, 4>& baseColor,
                          bool textured,
                          bool leafSurface)
{
    const ixtreemetree::Vec3 lightDir = Normalize({-0.35f, 0.85f, 0.45f});
    for (std::size_t i = 0; i + 2u < indices.size(); i += 3u)
    {
        const ixtreemetree::Vertex& va = vertices[indices[i + 0u]];
        const ixtreemetree::Vertex& vb = vertices[indices[i + 1u]];
        const ixtreemetree::Vertex& vc = vertices[indices[i + 2u]];
        const ProjectedPoint a = Project(va.position, center, radius, origin, size, yaw, pitch, zoom);
        const ProjectedPoint b = Project(vb.position, center, radius, origin, size, yaw, pitch, zoom);
        const ProjectedPoint c = Project(vc.position, center, radius, origin, size, yaw, pitch, zoom);
        const ixtreemetree::Vec3 faceNormal = Normalize(Cross(Sub(vb.position, va.position), Sub(vc.position, va.position)));
        float shade = 0.34f + std::max(0.0f, Dot(faceNormal, lightDir)) * 0.66f;
        if (textured)
        {
            const float uv = (va.uv.x + vb.uv.x + vc.uv.x + va.uv.y + vb.uv.y + vc.uv.y) * 8.0f;
            shade *= 0.88f + xm::Sin(uv) * 0.08f;
        }
        std::array<float, 4> fillColor = ShadeColor(baseColor, shade);
        if (leafSurface)
        {
            const float centerU = (va.uv.x + vb.uv.x + vc.uv.x) / 3.0f;
            const float centerV = (va.uv.y + vb.uv.y + vc.uv.y) / 3.0f;
            const float maskHint = 1.0f - std::max(xm::Abs(centerU - 0.5f), xm::Abs(centerV - 0.5f)) * 1.6f;
            fillColor[3] = std::clamp(fillColor[3] * (0.72f + maskHint * 0.22f), 0.24f, 0.92f);
        }
        PreviewTriangle tri{};
        tri.a = a.screen;
        tri.b = b.screen;
        tri.c = c.screen;
        tri.depth = (a.depth + b.depth + c.depth) / 3.0f;
        tri.fill = Color(fillColor);
        tri.line = leafSurface ? IM_COL32(25, 42, 24, 70) : IM_COL32(38, 27, 19, 70);
        tri.outline = leafSurface;
        triangles.push_back(tri);
    }
}
}

void TreePreviewRenderer::ResetView()
{
    yaw_ = -0.65f;
    pitch_ = 0.25f;
    zoom_ = 1.0f;
}

void TreePreviewRenderer::ResetView(const ixtreemetree::TreeMesh& mesh)
{
    ResetView();
    FitToMesh(mesh);
}

void TreePreviewRenderer::FitToMesh(const ixtreemetree::TreeMesh& mesh)
{
    center_ = {
        (mesh.bboxMin.x + mesh.bboxMax.x) * 0.5f,
        (mesh.bboxMin.y + mesh.bboxMax.y) * 0.5f,
        (mesh.bboxMin.z + mesh.bboxMax.z) * 0.5f,
    };
    const float dx = mesh.bboxMax.x - mesh.bboxMin.x;
    const float dy = mesh.bboxMax.y - mesh.bboxMin.y;
    const float dz = mesh.bboxMax.z - mesh.bboxMin.z;
    radius_ = std::max(0.6f, xm::Sqrt(dx * dx + dy * dy + dz * dz) * 0.5f);
    zoom_ = 1.0f;
    hasFit_ = true;
}

void TreePreviewRenderer::Render(const ixtreemetree::TreeMesh& mesh, float width, float height, const TreePreviewStyle& style)
{
    const ImVec2 imageSize(std::max(128.0f, width), std::max(160.0f, height));
    if (!hasFit_ && !mesh.bark.vertices.empty())
        FitToMesh(mesh);
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

    std::vector<PreviewTriangle> triangles;
    triangles.reserve(mesh.bark.indices.size() / 3u + mesh.leaves.indices.size() / 3u);
    PushSurfaceTriangles(triangles,
        mesh.bark.vertices,
        mesh.bark.indices,
        center_,
        radius_,
        min,
        size,
        yaw_,
        pitch_,
        zoom_,
        style.barkColor,
        style.barkTextured,
        false);
    std::array<float, 4> leafColor = style.leafColor;
    leafColor[3] = std::clamp(leafColor[3] * (1.0f - style.leafAlphaCutoff * 0.22f), 0.25f, 0.9f);
    PushSurfaceTriangles(triangles,
        mesh.leaves.vertices,
        mesh.leaves.indices,
        center_,
        radius_,
        min,
        size,
        yaw_,
        pitch_,
        zoom_,
        leafColor,
        style.leafTextured,
        true);
    std::sort(triangles.begin(), triangles.end(), [](const PreviewTriangle& a, const PreviewTriangle& b) {
        return a.depth > b.depth;
    });
    for (const PreviewTriangle& tri : triangles)
    {
        draw->AddTriangleFilled(tri.a, tri.b, tri.c, tri.fill);
        if (tri.outline)
            draw->AddTriangle(tri.a, tri.b, tri.c, tri.line, 0.7f);
    }

    if (mesh.bark.vertices.empty())
        draw->AddText(ImVec2(min.x + 16.0f, min.y + 16.0f), IM_COL32(200, 205, 214, 255), "No preview mesh");
}
}
