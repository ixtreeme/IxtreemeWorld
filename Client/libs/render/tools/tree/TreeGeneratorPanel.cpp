#include "TreeGeneratorPanel.h"

#include "AssetLibrary.h"
#include "AssetDatabase.h"
#include "Debug.h"
#include "ProjectManager.h"
#include "TreeGlbExporter.h"
#include "TreeTexturePalette.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <utility>

namespace tree_tool
{
namespace
{
template <typename Enum>
bool ComboEnum(const char* label, Enum& value, const char* const* names, int count)
{
    int current = static_cast<int>(value);
    if (!ImGui::Combo(label, &current, names, count))
        return false;
    value = static_cast<Enum>(std::clamp(current, 0, count - 1));
    return true;
}

void ColorFromU32(std::uint32_t color, float out[4])
{
    out[0] = static_cast<float>((color >> 24) & 0xFFu) / 255.0f;
    out[1] = static_cast<float>((color >> 16) & 0xFFu) / 255.0f;
    out[2] = static_cast<float>((color >> 8) & 0xFFu) / 255.0f;
    out[3] = static_cast<float>(color & 0xFFu) / 255.0f;
}

std::uint32_t ColorToU32(const float in[4])
{
    const auto byte = [](float v) {
        return static_cast<std::uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return (byte(in[0]) << 24) | (byte(in[1]) << 16) | (byte(in[2]) << 8) | byte(in[3]);
}

bool DragVec3(const char* label, ixtreemetree::Vec3& value, float speed, float minValue, float maxValue)
{
    float data[3] = {value.x, value.y, value.z};
    if (!ImGui::DragFloat3(label, data, speed, minValue, maxValue))
        return false;
    value = {data[0], data[1], data[2]};
    return true;
}

bool DragVec2(const char* label, ixtreemetree::Vec2& value, float speed, float minValue, float maxValue)
{
    float data[2] = {value.x, value.y};
    if (!ImGui::DragFloat2(label, data, speed, minValue, maxValue))
        return false;
    value = {data[0], data[1]};
    return true;
}

std::filesystem::path DefaultPresetDir()
{
#if defined(IXTREEME_TREE_PRESET_DIR)
    return std::filesystem::path(IXTREEME_TREE_PRESET_DIR);
#else
    return std::filesystem::current_path() / "assets" / "internal" / "tree_presets";
#endif
}
}

TreeGeneratorPanel::TreeGeneratorPanel(std::filesystem::path presetDir)
    : presetDir_(presetDir.empty() ? DefaultPresetDir() : std::move(presetDir))
{
    options_ = ixtreemetree::defaultTreeOptions();
    TreeTexturePalette::Instance().EnsureLoaded(InternalRoot());
    std::snprintf(assetName_, sizeof(assetName_), "tree_%u", options_.seed);
    LoadPresets();
    Regenerate();
}

void TreeGeneratorPanel::SetAssetLibrary(AssetLibrary* assetLibrary)
{
    assetLibrary_ = assetLibrary;
}

void TreeGeneratorPanel::Show()
{
    open_ = true;
}

void TreeGeneratorPanel::Regenerate()
{
    ixtreemetree::Tree tree;
    tree.options = options_;
    mesh_ = tree.generate();
    status_ = "Generated tree";
}

void TreeGeneratorPanel::LoadPresets()
{
    presetsLoaded_ = true;
    std::vector<std::string> errors;
    presets_ = ixtreemetree::loadAllPresets(presetDir_, &errors);
    for (const std::string& error : errors)
        TraceError("[TREE-2] preset_load_failed: %s", error.c_str());
    Tracenf("[TREE-2] loaded %zu presets from %s",
        presets_.size(),
        presetDir_.generic_string().c_str());
}

void TreeGeneratorPanel::ApplyPreset(size_t presetIndex)
{
    if (presetIndex >= presets_.size())
        return;
    const ixtreemetree::Preset& preset = presets_[presetIndex];
    options_ = preset.options;
    activePresetName_ = preset.name;
    barkTextureOverrideAssetId_.reset();
    leafTextureOverrideAssetId_.reset();
    barkTextureOverridePath_.reset();
    leafTextureOverridePath_.reset();
    Regenerate();
    status_ = "Preset: " + preset.name;
}

void TreeGeneratorPanel::RenderPresetSelector()
{
    if (!presetsLoaded_)
        LoadPresets();

    int currentIndex = 0;
    if (activePresetName_)
    {
        for (size_t i = 0; i < presets_.size(); ++i)
        {
            if (presets_[i].name == *activePresetName_)
            {
                currentIndex = static_cast<int>(i + 1);
                break;
            }
        }
    }

    const char* preview = currentIndex == 0 ? "Custom" : presets_[static_cast<size_t>(currentIndex - 1)].name.c_str();
    if (ImGui::BeginCombo("Preset", preview))
    {
        const bool customSelected = currentIndex == 0;
        if (ImGui::Selectable("Custom", customSelected))
            activePresetName_.reset();
        if (customSelected)
            ImGui::SetItemDefaultFocus();

        for (size_t i = 0; i < presets_.size(); ++i)
        {
            const bool selected = currentIndex == static_cast<int>(i + 1);
            if (ImGui::Selectable(presets_[i].name.c_str(), selected))
                ApplyPreset(i);
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    if (presets_.empty())
        ImGui::TextDisabled("No presets found in %s", presetDir_.generic_string().c_str());
}

bool TreeGeneratorPanel::Render()
{
    if (!open_)
        return false;
    if (!firstOpenLogged_)
    {
        firstOpenLogged_ = true;
        Tracenf("[TREE-1] tree_generator_panel registered, ixtreemetree=%s", ixtreemetree::kVersion);
    }

    bool savedAsset = false;
    if (ImGui::Begin("Tree Generator", &open_))
    {
        RenderPresetSelector();
        ImGui::Separator();
        if (ImGui::BeginTable("TreeGeneratorLayout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV))
        {
            ImGui::TableSetupColumn("Parameters", ImGuiTableColumnFlags_WidthStretch, 0.55f);
            ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthStretch, 0.45f);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (RenderParameters())
            {
                activePresetName_.reset();
                Regenerate();
            }
            ImGui::TableSetColumnIndex(1);
            const float previewWidth = ImGui::GetContentRegionAvail().x;
            preview_.Render(mesh_, previewWidth, 420.0f, CurrentPreviewStyle());
            if (ImGui::Button("Reset View"))
                preview_.ResetView();
            ImGui::SameLine();
            if (ImGui::Button("Regenerate"))
                Regenerate();
            ImGui::SameLine();
            if (ImGui::Button("Save as Asset..."))
                savePopupRequested_ = true;

            ImGui::Separator();
            ImGui::Text("Triangles: %d", mesh_.stats.barkTriangles + mesh_.stats.leafTriangles);
            ImGui::Text("Bark: %d  Leaves: %d", mesh_.stats.barkTriangles, mesh_.stats.leafTriangles);
            ImGui::Text("Generation: %.3f ms", mesh_.stats.generationMs);
            ImGui::Text("Bounds: (%.2f %.2f %.2f) - (%.2f %.2f %.2f)",
                mesh_.bboxMin.x, mesh_.bboxMin.y, mesh_.bboxMin.z,
                mesh_.bboxMax.x, mesh_.bboxMax.y, mesh_.bboxMax.z);
            if (!status_.empty())
                ImGui::TextDisabled("%s", status_.c_str());
            ImGui::EndTable();
        }
    }
    ImGui::End();

    RenderSavePopup(savedAsset);
    return savedAsset;
}

bool TreeGeneratorPanel::RenderParameters()
{
    bool changed = false;
    changed |= RenderGeneral();
    changed |= RenderBark();
    changed |= RenderBranch();
    changed |= RenderLeaves();
    return changed;
}

bool TreeGeneratorPanel::RenderGeneral()
{
    if (!ImGui::CollapsingHeader("General", ImGuiTreeNodeFlags_DefaultOpen))
        return false;
    bool changed = false;
    int seed = static_cast<int>(options_.seed);
    if (ImGui::DragInt("Seed", &seed, 1.0f, 0, 0x7fffffff))
    {
        options_.seed = static_cast<std::uint32_t>(std::max(0, seed));
        changed = true;
    }
    static const char* const treeTypes[] = {"Deciduous", "Evergreen"};
    changed |= ComboEnum("Type", options_.type, treeTypes, 2);
    return changed;
}

bool TreeGeneratorPanel::RenderBark()
{
    if (!ImGui::CollapsingHeader("Bark", ImGuiTreeNodeFlags_DefaultOpen))
        return false;
    bool changed = false;
    static const char* const barkTypes[] = {"Oak", "Birch", "Pine", "Willow", "Ash"};
    changed |= ComboEnum("Bark Type", options_.bark.type, barkTypes, 5);
    float color[4]{};
    ColorFromU32(options_.bark.tint, color);
    if (ImGui::ColorEdit4("Bark Tint", color))
    {
        options_.bark.tint = ColorToU32(color);
        changed = true;
    }
    changed |= ImGui::Checkbox("Flat Shading", &options_.bark.flatShading);
    changed |= ImGui::Checkbox("Textured", &options_.bark.textured);
    changed |= DragVec2("Texture Scale", options_.bark.textureScale, 0.02f, 0.1f, 20.0f);
    const auto& palette = TreeTexturePalette::Instance().Bark(options_.bark.type);
    changed |= RenderTextureOverrideSlot("Texture Override", "bark",
        barkTextureOverrideAssetId_,
        barkTextureOverridePath_,
        palette.path,
        palette.previewColor);
    return changed;
}

bool TreeGeneratorPanel::RenderBranch()
{
    if (!ImGui::CollapsingHeader("Branch", ImGuiTreeNodeFlags_DefaultOpen))
        return false;
    bool changed = false;
    changed |= ImGui::SliderInt("Levels", &options_.branch.levels, 0, ixtreemetree::kMaxBranchLevels - 1);
    if (ImGui::BeginTable("TreeBranchLevels", 10, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
    {
        const char* headers[] = {"Level", "Angle", "Children", "Gnarl", "Length", "Radius", "Sections", "Segments", "Start", "Taper/Twist"};
        for (const char* header : headers)
            ImGui::TableSetupColumn(header);
        ImGui::TableHeadersRow();
        for (int level = 0; level < ixtreemetree::kMaxBranchLevels; ++level)
        {
            ImGui::PushID(level);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("%d", level);
            ImGui::TableSetColumnIndex(1); ImGui::SetNextItemWidth(74.0f); changed |= ImGui::DragFloat("##angle", &options_.branch.angle[level], 0.5f, 0.0f, 180.0f);
            ImGui::TableSetColumnIndex(2); ImGui::SetNextItemWidth(64.0f); changed |= ImGui::DragInt("##children", &options_.branch.children[level], 0.1f, 0, 120);
            ImGui::TableSetColumnIndex(3); ImGui::SetNextItemWidth(74.0f); changed |= ImGui::DragFloat("##gnarl", &options_.branch.gnarliness[level], 0.01f, -3.0f, 3.0f);
            ImGui::TableSetColumnIndex(4); ImGui::SetNextItemWidth(74.0f); changed |= ImGui::DragFloat("##length", &options_.branch.length[level], 0.05f, 0.1f, 100.0f);
            ImGui::TableSetColumnIndex(5); ImGui::SetNextItemWidth(74.0f); changed |= ImGui::DragFloat("##radius", &options_.branch.radius[level], 0.01f, 0.01f, 5.0f);
            ImGui::TableSetColumnIndex(6); ImGui::SetNextItemWidth(64.0f); changed |= ImGui::DragInt("##sections", &options_.branch.sections[level], 0.1f, 1, 32);
            ImGui::TableSetColumnIndex(7); ImGui::SetNextItemWidth(64.0f); changed |= ImGui::DragInt("##segments", &options_.branch.segments[level], 0.1f, 3, 32);
            ImGui::TableSetColumnIndex(8); ImGui::SetNextItemWidth(74.0f); changed |= ImGui::DragFloat("##start", &options_.branch.start[level], 0.01f, 0.0f, 0.95f);
            ImGui::TableSetColumnIndex(9);
            ImGui::SetNextItemWidth(58.0f); changed |= ImGui::DragFloat("##taper", &options_.branch.taper[level], 0.01f, 0.0f, 1.0f);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(58.0f); changed |= ImGui::DragFloat("##twist", &options_.branch.twist[level], 0.5f, -180.0f, 180.0f);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    changed |= DragVec3("Force Direction", options_.branch.forceDirection, 0.01f, -1.0f, 1.0f);
    changed |= ImGui::DragFloat("Force Strength", &options_.branch.forceStrength, 0.005f, -1.0f, 1.0f);
    return changed;
}

bool TreeGeneratorPanel::RenderLeaves()
{
    if (!ImGui::CollapsingHeader("Leaves", ImGuiTreeNodeFlags_DefaultOpen))
        return false;
    bool changed = false;
    static const char* const leafTypes[] = {"Oak", "Ash", "Pine", "Willow", "Birch"};
    static const char* const billboardTypes[] = {"Single", "Double"};
    changed |= ComboEnum("Leaf Type", options_.leaves.type, leafTypes, 5);
    changed |= ComboEnum("Billboard", options_.leaves.billboard, billboardTypes, 2);
    changed |= ImGui::DragFloat("Leaf Angle", &options_.leaves.angle, 0.5f, 0.0f, 90.0f);
    changed |= ImGui::DragInt("Leaf Count", &options_.leaves.count, 0.1f, 1, 120);
    changed |= ImGui::DragFloat("Leaf Start", &options_.leaves.start, 0.01f, 0.0f, 0.95f);
    changed |= ImGui::DragFloat("Leaf Size", &options_.leaves.size, 0.01f, 0.02f, 8.0f);
    changed |= ImGui::DragFloat("Size Variance", &options_.leaves.sizeVariance, 0.01f, 0.0f, 1.0f);
    float color[4]{};
    ColorFromU32(options_.leaves.tint, color);
    if (ImGui::ColorEdit4("Leaf Tint", color))
    {
        options_.leaves.tint = ColorToU32(color);
        changed = true;
    }
    changed |= ImGui::DragFloat("Alpha Test", &options_.leaves.alphaTest, 0.01f, 0.0f, 1.0f);
    const auto& palette = TreeTexturePalette::Instance().Leaf(options_.leaves.type);
    changed |= RenderTextureOverrideSlot("Texture Override", "leaves",
        leafTextureOverrideAssetId_,
        leafTextureOverridePath_,
        palette.path,
        palette.previewColor);
    return changed;
}

bool TreeGeneratorPanel::RenderTextureOverrideSlot(const char* label,
                                                   const char* slotName,
                                                   std::optional<std::string>& assetId,
                                                   std::optional<std::filesystem::path>& assetPath,
                                                   const std::filesystem::path& defaultPath,
                                                   const std::array<float, 4>& previewColor)
{
    bool changed = false;
    ImGui::SeparatorText(label);
    ImGui::PushID(slotName);
    const ImVec2 slotSize(96.0f, 74.0f);
    ImGui::ColorButton("##preview",
        ImVec4(previewColor[0], previewColor[1], previewColor[2], previewColor[3]),
        ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
        slotSize);
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_ID"))
        {
            const std::string droppedId(static_cast<const char*>(payload->Data),
                static_cast<std::size_t>(payload->DataSize));
            if (assetLibrary_)
            {
                const std::optional<AssetLibrary::Entry> entry = assetLibrary_->FindById(droppedId);
                if (entry && entry->category == AssetLibrary::Category::Texture)
                {
                    assetId = entry->id;
                    assetPath = assetLibrary_->AbsolutePath(*entry);
                    AssetDatabase::Instance().getOrCreateGuid(*assetPath);
                    status_ = std::string(slotName) + " texture override: " + entry->displayName;
                    Tracenf("[TREE-3] texture_override slot=%s asset_id=%s path=%s",
                        slotName,
                        entry->id.c_str(),
                        assetPath->generic_string().c_str());
                    changed = true;
                }
                else
                {
                    status_ = "Texture override accepts Texture assets only";
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    const std::filesystem::path shownPath = assetPath.value_or(defaultPath);
    ImGui::TextUnformatted(assetPath ? "Override" : "Palette default");
    ImGui::TextDisabled("%s", shownPath.filename().string().c_str());
    ImGui::TextDisabled("Drop Texture here");
    if (assetPath)
    {
        if (ImGui::Button("Clear"))
        {
            assetId.reset();
            assetPath.reset();
            status_ = std::string(slotName) + " texture override cleared";
            changed = true;
        }
    }
    ImGui::EndGroup();
    ImGui::PopID();
    return changed;
}

TreeMaterialBinding TreeGeneratorPanel::CurrentMaterialBinding()
{
    TreeTexturePalette::Instance().EnsureLoaded(InternalRoot());
    const TreePaletteTexture& bark = TreeTexturePalette::Instance().Bark(options_.bark.type);
    const TreePaletteTexture& leaf = TreeTexturePalette::Instance().Leaf(options_.leaves.type);
    TreeMaterialBinding binding{};
    binding.barkBaseColorTexturePath = barkTextureOverridePath_.value_or(bark.path);
    binding.leafBaseColorTexturePath = leafTextureOverridePath_.value_or(leaf.path);
    binding.leafAlphaCutoff = options_.leaves.alphaTest;
    return binding;
}

TreePreviewStyle TreeGeneratorPanel::CurrentPreviewStyle() const
{
    const TreePaletteTexture& bark = TreeTexturePalette::Instance().Bark(options_.bark.type);
    const TreePaletteTexture& leaf = TreeTexturePalette::Instance().Leaf(options_.leaves.type);
    TreePreviewStyle style{};
    style.barkColor = barkTextureOverridePath_ ? std::array<float, 4>{0.72f, 0.62f, 0.46f, 0.82f} : bark.previewColor;
    style.leafColor = leafTextureOverridePath_ ? std::array<float, 4>{0.60f, 0.72f, 0.44f, 0.86f} : leaf.previewColor;
    return style;
}

std::filesystem::path TreeGeneratorPanel::InternalRoot() const
{
    return presetDir_.empty()
        ? (std::filesystem::current_path() / "assets" / "internal")
        : presetDir_.parent_path();
}

void TreeGeneratorPanel::RenderSavePopup(bool& savedAsset)
{
    if (savePopupRequested_)
    {
        savePopupRequested_ = false;
        std::snprintf(assetName_, sizeof(assetName_), "tree_%u", options_.seed);
        ImGui::OpenPopup("Save Tree Asset");
    }
    if (!ImGui::BeginPopupModal("Save Tree Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::InputText("Asset Name", assetName_, sizeof(assetName_));
    ProjectManager& projects = ProjectManager::Instance();
    if (!projects.HasProject())
        ImGui::TextDisabled("Open or create a project before saving.");

    if (ImGui::Button("Save", ImVec2(120.0f, 0.0f)))
    {
        if (!projects.HasProject())
        {
            status_ = "Save failed: no open project";
        }
        else
        {
            TreeExportResult result = TreeGlbExporter::SaveAsAsset(mesh_, projects.ProjectRoot(), assetName_, CurrentMaterialBinding());
            if (result.ok)
            {
                status_ = "Saved: " + result.modelPath.filename().string();
                savedAsset = true;
                ImGui::CloseCurrentPopup();
            }
            else
            {
                status_ = "Save failed: " + result.error;
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
        ImGui::CloseCurrentPopup();
    if (!status_.empty())
        ImGui::TextDisabled("%s", status_.c_str());
    ImGui::EndPopup();
}
}
