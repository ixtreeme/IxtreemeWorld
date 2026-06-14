#pragma once

#include "TreePreviewRenderer.h"

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

class TreeGeneratorPanel
{
public:
    explicit TreeGeneratorPanel(std::filesystem::path presetDir = {});

    void SetAssetLibrary(AssetLibrary* assetLibrary);
    void Show();
    bool IsOpen() const { return open_; }
    bool Render();
    const std::string& Status() const { return status_; }

private:
    void Regenerate();
    void LoadPresets();
    void RenderPresetSelector();
    void ApplyPreset(size_t presetIndex);
    bool RenderParameters();
    bool RenderGeneral();
    bool RenderBark();
    bool RenderBranch();
    bool RenderLeaves();
    void RenderSavePopup(bool& savedAsset);
    bool RenderTextureOverrideSlot(const char* label,
                                   const char* slotName,
                                   std::optional<std::string>& assetId,
                                   std::optional<std::filesystem::path>& assetPath,
                                   const std::filesystem::path& defaultPath,
                                   const std::array<float, 4>& previewColor);
    TreeMaterialBinding CurrentMaterialBinding();
    TreePreviewStyle CurrentPreviewStyle() const;
    std::filesystem::path InternalRoot() const;

    std::filesystem::path presetDir_;
    AssetLibrary* assetLibrary_ = nullptr;
    std::vector<ixtreemetree::Preset> presets_;
    std::optional<std::string> activePresetName_;
    std::optional<std::string> barkTextureOverrideAssetId_;
    std::optional<std::string> leafTextureOverrideAssetId_;
    std::optional<std::filesystem::path> barkTextureOverridePath_;
    std::optional<std::filesystem::path> leafTextureOverridePath_;
    bool open_ = false;
    bool savePopupRequested_ = false;
    bool firstOpenLogged_ = false;
    bool presetsLoaded_ = false;
    ixtreemetree::TreeOptions options_;
    ixtreemetree::TreeMesh mesh_;
    TreePreviewRenderer preview_;
    char assetName_[96]{};
    std::string status_;
};
}
