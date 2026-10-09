#pragma once

#include "TreePreviewRenderer.h"

#include "AssetDatabase.h"

#include <ixtreemetree/ixtreemetree.h>

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

class AssetLibrary;

namespace tree_tool
{
struct TreeMaterialBinding;

// Tools > Tree Generator. Its window is the tree's preview; its settings are in the Inspector, which
// shows them from when the window takes focus (or opens) until something is selected elsewhere.
class TreeGeneratorPanel
{
public:
    explicit TreeGeneratorPanel(std::filesystem::path presetDir = {});

    void SetAssetLibrary(AssetLibrary* assetLibrary);
    void Show();
    bool IsOpen() const { return open_; }
    // The Tree Generator window: the preview only.
    void RenderView();
    // Whether the view took focus or the panel was opened since the last call.
    bool TakeInspectorRequest();
    // The settings, drawn into the Inspector. True when the tree was saved as an asset.
    bool RenderInspector();
    const std::string& Status() const { return status_; }

private:
    void Regenerate();
    void LoadPresets();
    void RenderPresetSelector();
    void ApplyPreset(size_t presetIndex);
    bool RenderGeneral();
    bool RenderBranches();
    bool RenderBranchLevel(int level);
    bool RenderBark();
    bool RenderLeaves();
    void RenderOutput();
    void RenderSavePopup(bool& savedAsset);
    // A material slot: an asset field the user drops a Material on (or picks one from the list).
    bool RenderMaterialField(const char* label, const char* slotName, std::optional<Guid>& material,
                             std::array<float, 4>& previewColor, const char* tooltip);
    TreeMaterialBinding CurrentMaterialBinding();
    TreePreviewStyle CurrentPreviewStyle() const;
    std::filesystem::path InternalRoot() const;

    std::filesystem::path presetDir_;
    AssetLibrary* assetLibrary_ = nullptr;
    std::vector<ixtreemetree::Preset> presets_;
    std::optional<std::string> activePresetName_;
    // The materials the tree is saved with; none: the built-in bark or leaves.
    std::optional<Guid> barkMaterial_;
    std::optional<Guid> leafMaterial_;
    std::array<float, 4> barkMaterialColor_{1.0f, 1.0f, 1.0f, 1.0f};  // what the preview draws them with
    std::array<float, 4> leafMaterialColor_{1.0f, 1.0f, 1.0f, 1.0f};
    bool open_ = false;
    bool inspectorRequested_ = false;
    bool savePopupRequested_ = false;
    bool firstOpenLogged_ = false;
    bool presetsLoaded_ = false;
    int activeBranchLevel_ = 0;
    ixtreemetree::TreeOptions options_;
    ixtreemetree::TreeMesh mesh_;
    double leafCardArea_ = 0.0;  // the leaf cards' area, mesh units squared (Regenerate)
    TreePreviewRenderer preview_;
    char assetName_[96]{};
    std::string status_;
};
}
