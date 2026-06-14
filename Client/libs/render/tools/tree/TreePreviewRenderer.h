#pragma once

#include <ixtreemetree/tree_mesh.h>

#include <array>

namespace tree_tool
{
struct TreePreviewStyle
{
    std::array<float, 4> barkColor{0.53f, 0.36f, 0.19f, 0.70f};
    std::array<float, 4> leafColor{0.29f, 0.57f, 0.28f, 0.82f};
};

class TreePreviewRenderer
{
public:
    void ResetView();
    void Render(const ixtreemetree::TreeMesh& mesh, float width, float height, const TreePreviewStyle& style = {});

private:
    float yaw_ = -0.65f;
    float pitch_ = 0.25f;
    float zoom_ = 1.0f;
};
}
