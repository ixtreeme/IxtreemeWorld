#pragma once
#include "TreePreviewRaster.h"
#include "EditorGraphicsBridge.h"
#include <filesystem>
#include <string>
namespace tree_tool
{
struct TreePreviewStyle
{
    std::array<float,4> barkColor{1,1,1,1}, leafColor{1,1,1,1};
    float leafAlphaCutoff=0.5f;
    bool barkTextured=true, leafTextured=true;
    std::filesystem::path barkTexture, leafTexture;
    bool operator==(const TreePreviewStyle&) const = default;
};
class TreePreviewRenderer
{
public:
    ~TreePreviewRenderer();
    void SetTextureProvider(ixeditor::graphics::IEditorTextureProvider* provider);
    void ResetView();
    void ResetView(const ixtreemetree::TreeMesh& mesh);
    void FitToMesh(const ixtreemetree::TreeMesh& mesh);
    void Render(const ixtreemetree::TreeMesh& mesh, float width, float height, const TreePreviewStyle& style = {});
    void InvalidateMesh() { dirty_=true; texturesDirty_=true; }
    const std::string& TextureWarning() const { return textureWarning_; }
private:
    void ReleaseTexture();
    ixeditor::graphics::IEditorTextureProvider* provider_=nullptr;
    ixeditor::graphics::EditorTextureHandle texture_{};
    PreviewView view_{};
    TreePreviewStyle style_{};
    PreviewImage bark_, leaves_;
    std::string textureWarning_;
    bool dirty_=true, hasFit_=false, texturesDirty_=true;
};
}
