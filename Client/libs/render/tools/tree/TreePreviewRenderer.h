#pragma once

#include <ixtreemetree/tree_mesh.h>

#include <array>

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

private:
    float yaw_ = -0.65f;
    float pitch_ = 0.25f;
    float zoom_ = 1.0f;
    ixtreemetree::Vec3 center_{0.0f, 2.5f, 0.0f};
    float radius_ = 4.0f;
    bool hasFit_ = false;
};
}
