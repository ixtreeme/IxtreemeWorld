#pragma once

#include <ixtreemetree/tree_mesh.h>

#include <array>
#include <cstdint>
#include <vector>

namespace tree_tool
{
struct TreePreviewStyle
{
    std::array<float, 4> barkColor{0.53f, 0.36f, 0.19f, 0.70f};
    std::array<float, 4> leafColor{0.29f, 0.57f, 0.28f, 0.82f};
    float leafAlphaCutoff = 0.5f;
    bool barkTextured = true;
    bool leafTextured = true;
};

class TreePreviewRenderer
{
public:
    void ResetView();
    void ResetView(const ixtreemetree::TreeMesh& mesh);
    void FitToMesh(const ixtreemetree::TreeMesh& mesh);
    void Render(const ixtreemetree::TreeMesh& mesh, float width, float height, const TreePreviewStyle& style = {});
    // The mesh was generated again: the next Render projects it anew.
    void InvalidateMesh() { ++meshRevision_; }

private:
    // The last view's projected, depth-sorted triangles, drawn again while nothing they depend on
    // changed (the mesh, the view, the canvas, the style). Projecting and sorting a large tree's
    // triangles every frame cost milliseconds.
    struct CachedTriangle
    {
        float a[2] = {};
        float b[2] = {};
        float c[2] = {};
        std::uint32_t fill = 0;
        std::uint32_t line = 0;
        bool outline = false;
    };
    struct CacheKey
    {
        std::uint64_t meshRevision = 0;
        float yaw = 0.0f;
        float pitch = 0.0f;
        float zoom = 0.0f;
        float radius = 0.0f;
        ixtreemetree::Vec3 center{};
        float rect[4] = {};
        TreePreviewStyle style{};
    };
    static bool SameKey(const CacheKey& a, const CacheKey& b);
    std::vector<CachedTriangle> cached_;
    CacheKey cachedKey_{};
    bool cacheValid_ = false;
    std::uint64_t meshRevision_ = 0;

    float yaw_ = -0.65f;
    float pitch_ = 0.25f;
    float zoom_ = 1.0f;
    ixtreemetree::Vec3 center_{0.0f, 2.5f, 0.0f};
    float radius_ = 4.0f;
    bool hasFit_ = false;
};
}
