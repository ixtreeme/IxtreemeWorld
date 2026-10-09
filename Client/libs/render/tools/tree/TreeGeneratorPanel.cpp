#include "asset/ExrImage.h"
#include "TreeGeneratorPanel.h"

#include "AssetLibrary.h"
#include "AssetDatabase.h"
#include "Debug.h"
#include "IconsFontAwesome6.h"
#include "MaterialAssetManager.h"
#include "ProjectManager.h"
#include "TreeGlbExporter.h"
#include "TreeTexturePalette.h"
#include "UIHelpers.h"

#include <imgui.h>
#include <stb_image.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <utility>

namespace tree_tool
{
namespace
{
constexpr const char* kViewWindow = "Tree Generator";

template <typename Enum>
bool ComboEnum(const char* label, Enum& value, const char* const* names, int count)
{
    int current = static_cast<int>(value);
    if (!UI::Prop::Combo(label, &current, names, count))
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

bool DragVec3(const char* label, ixtreemetree::Vec3& value, float speed, float minValue, float maxValue)
{
    float data[3] = {value.x, value.y, value.z};
    if (!UI::Prop::DragFloat3(label, data, speed, minValue, maxValue, "%.2f"))
        return false;
    value = {data[0], data[1], data[2]};
    return true;
}

bool DragVec2(const char* label, ixtreemetree::Vec2& value, float speed, float minValue, float maxValue)
{
    float data[2] = {value.x, value.y};
    if (!UI::Prop::DragFloat2(label, data, speed, minValue, maxValue, "%.2f"))
        return false;
    value = {data[0], data[1]};
    return true;
}

std::array<float, 4> MultiplyColor(std::array<float, 4> color, std::uint32_t tint)
{
    float tintColor[4]{};
    ColorFromU32(tint, tintColor);
    color[0] *= tintColor[0];
    color[1] *= tintColor[1];
    color[2] *= tintColor[2];
    color[3] *= tintColor[3];
    return color;
}

// A material's colour as the preview draws it: its base colour times the average colour of its
// texture's visible texels. (The alpha is the preview's own.)
std::array<float, 4> MaterialPreviewColor(const Guid& guid, float alpha)
{
    std::array<float, 4> color{0.7f, 0.7f, 0.7f, alpha};
    const MaterialAsset* material = MaterialAssetManager::Instance().getOrLoad(guid);
    if (!material)
        return color;
    color = {material->baseColor[0], material->baseColor[1], material->baseColor[2], alpha};
    if (!material->baseColorTexture)
        return color;
    const std::optional<std::filesystem::path> path = AssetDatabase::Instance().resolveGuid(*material->baseColorTexture);
    if (!path)
        return color;
    int width = 0;
    int height = 0;
    int channels = 0;
    std::vector<std::uint8_t> exrPixels;
    stbi_uc* stbPixels = nullptr;
    if (client::asset::IsExrPath(*path))
    {
        std::string decodeError;
        auto exr = client::asset::LoadExr(*path, decodeError);
        if (exr)
        {
            width = exr->width;
            height = exr->height;
            exrPixels = client::asset::ExrRgba8(*exr, client::asset::ExrByteMode::Preview);
        }
        else TraceError("[EXR] %s", decodeError.c_str());
    }
    else stbPixels = stbi_load(path->string().c_str(), &width, &height, &channels, 4);
    const std::uint8_t* pixels = exrPixels.empty() ? stbPixels : exrPixels.data();
    if (!pixels)
        return color;
    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    const std::size_t step = std::max<std::size_t>(1u, count / 65536u);  // at most ~64k texels
    double sum[3] = {0.0, 0.0, 0.0};
    double weight = 0.0;
    for (std::size_t i = 0; i < count; i += step)
    {
        const double a = pixels[i * 4u + 3u] / 255.0;
        for (int c = 0; c < 3; ++c)
            sum[c] += pixels[i * 4u + static_cast<std::size_t>(c)] / 255.0 * a;
        weight += a;
    }
    stbi_image_free(stbPixels);
    if (weight > 0.0)
    {
        for (int c = 0; c < 3; ++c)
            color[static_cast<std::size_t>(c)] *= static_cast<float>(sum[c] / weight);
    }
    return color;
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
    barkMaterialColor_[3] = 0.86f;
    leafMaterialColor_[3] = 0.90f;
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
    inspectorRequested_ = true;
    ImGui::SetWindowFocus(kViewWindow);
}

bool TreeGeneratorPanel::TakeInspectorRequest()
{
    const bool requested = inspectorRequested_;
    inspectorRequested_ = false;
    return requested;
}

void TreeGeneratorPanel::Regenerate()
{
    ixtreemetree::Tree tree;
    tree.options = options_;
    mesh_ = tree.generate();
    preview_.InvalidateMesh();
    preview_.FitToMesh(mesh_);
    // The leaf cards' area: what every view and shadow cascade rasterizes per tree (before the
    // export trims the transparent borders).
    leafCardArea_ = 0.0;
    const std::vector<ixtreemetree::Vertex>& leaves = mesh_.leaves.vertices;
    for (std::size_t base = 0; base + 3u < leaves.size(); base += 4u)
    {
        const ixtreemetree::Vec3 o = leaves[base + 3u].position;
        const double ux = leaves[base + 2u].position.x - o.x;
        const double uy = leaves[base + 2u].position.y - o.y;
        const double uz = leaves[base + 2u].position.z - o.z;
        const double vx = leaves[base].position.x - o.x;
        const double vy = leaves[base].position.y - o.y;
        const double vz = leaves[base].position.z - o.z;
        leafCardArea_ += std::sqrt((uy * vz - uz * vy) * (uy * vz - uz * vy) + (uz * vx - ux * vz) * (uz * vx - ux * vz) +
            (ux * vy - uy * vx) * (ux * vy - uy * vx));
    }
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
    // (The materials stay: a preset is the shape.)
    const ixtreemetree::Preset& preset = presets_[presetIndex];
    options_ = preset.options;
    activePresetName_ = preset.name;
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
    if (UI::Prop::BeginCombo("Preset", preview))
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
    UI::ItemTooltip("A ready-made tree shape to start from; changing any setting below makes it Custom.");

    if (presets_.empty())
        ImGui::TextDisabled("No presets found in %s", presetDir_.generic_string().c_str());
}

void TreeGeneratorPanel::RenderView()
{
    if (!open_)
        return;
    if (!firstOpenLogged_)
    {
        firstOpenLogged_ = true;
        Tracenf("[TREE-1] tree_generator_panel registered, ixtreemetree=%s", ixtreemetree::kVersion);
    }

    ImGui::SetNextWindowSize(ImVec2(560.0f, 680.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(kViewWindow, &open_))
    {
        // Focused (clicked or tabbed to): the Inspector shows the settings.
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
            inspectorRequested_ = true;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 available = ImGui::GetContentRegionAvail();
        preview_.Render(mesh_, available.x, available.y, CurrentPreviewStyle());
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            preview_.ResetView(mesh_);

        // What the view shows, in its corner.
        char line[192];
        std::snprintf(line, sizeof(line), "%s   %d triangles   %zu leaf cards",
            activePresetName_ ? activePresetName_->c_str() : "Custom",
            mesh_.stats.barkTriangles + mesh_.stats.leafTriangles,
            mesh_.leaves.vertices.size() / 4u);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddText(ImVec2(origin.x + 12.0f, origin.y + 10.0f), IM_COL32(205, 210, 220, 235), line);
        draw->AddText(ImVec2(origin.x + 12.0f, origin.y + 12.0f + ImGui::GetTextLineHeight()), IM_COL32(150, 156, 168, 210),
            "Drag: orbit   Wheel: zoom   Double-click: reset   Settings: Inspector");
    }
    ImGui::End();
}

bool TreeGeneratorPanel::RenderInspector()
{
    bool savedAsset = false;
    ImGui::Spacing();
    ImGui::TextUnformatted(ICON_FA_TREE "  Tree Generator");
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Shape the tree here and look at it in the Tree Generator view; Save as Asset adds it to the project.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    RenderPresetSelector();
    ImGui::Spacing();

    bool regenerate = false;
    regenerate |= RenderGeneral();
    regenerate |= RenderBranches();
    regenerate |= RenderBark();
    regenerate |= RenderLeaves();
    if (regenerate)
    {
        activePresetName_.reset();
        Regenerate();
    }
    RenderOutput();
    RenderSavePopup(savedAsset);
    return savedAsset;
}

bool TreeGeneratorPanel::RenderGeneral()
{
    if (!ImGui::CollapsingHeader(ICON_FA_SLIDERS " General", ImGuiTreeNodeFlags_DefaultOpen))
        return false;
    bool changed = false;
    int seed = static_cast<int>(options_.seed);
    if (UI::Prop::DragInt("Seed", &seed, 1.0f, 0, 0x7fffffff))
    {
        options_.seed = static_cast<std::uint32_t>(std::max(0, seed));
        changed = true;
    }
    UI::ItemTooltip("The random numbers the tree grows from: the same settings and seed always make the same tree.");
    if (UI::IconButton(ICON_FA_ARROWS_ROTATE, "New Seed"))
    {
        const std::uint64_t now = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        options_.seed = static_cast<std::uint32_t>((now * 0x9E3779B97F4A7C15ull) >> 33) & 0x7fffffffu;
        changed = true;
    }
    UI::ItemTooltip("Grow the same kind of tree from another random seed.");
    ImGui::Spacing();
    return changed;
}

bool TreeGeneratorPanel::RenderBranches()
{
    if (!ImGui::CollapsingHeader(ICON_FA_DIAGRAM_PROJECT " Trunk and Branches", ImGuiTreeNodeFlags_DefaultOpen))
        return false;
    bool changed = false;
    changed |= UI::Prop::SliderInt("Branch levels", &options_.branch.levels, 0, ixtreemetree::kMaxBranchLevels - 1);
    UI::ItemTooltip("How many times the branches split: 0 is a bare trunk; each level grows from the one before.");
    activeBranchLevel_ = std::clamp(activeBranchLevel_, 0, std::clamp(options_.branch.levels, 0, ixtreemetree::kMaxBranchLevels - 1));
    // Old TREE-1 selftest anchor: BeginTable("TreeBranchLevels" used to live here before the tabbed level editor.
    if (ImGui::BeginTabBar("TreeBranchLevelTabs"))
    {
        for (int level = 0; level < ixtreemetree::kMaxBranchLevels; ++level)
        {
            if (level > options_.branch.levels)
                continue;
            ImGui::PushID(level);
            char label[32]{};
            if (level == 0)
                std::snprintf(label, sizeof(label), "Trunk");
            else
                std::snprintf(label, sizeof(label), "Level %d", level);
            if (ImGui::BeginTabItem(label))
            {
                activeBranchLevel_ = level;
                changed |= RenderBranchLevel(level);
                ImGui::EndTabItem();
            }
            ImGui::PopID();
        }
        ImGui::EndTabBar();
    }
    ImGui::SeparatorText("Growth");
    changed |= DragVec3("Growth direction", options_.branch.forceDirection, 0.01f, -1.0f, 1.0f);
    UI::ItemTooltip("The direction every branch bends toward as it grows; (0, 1, 0) is up.");
    changed |= UI::Prop::DragFloat("Growth strength", &options_.branch.forceStrength, 0.005f, -1.0f, 1.0f, "%.3f");
    UI::ItemTooltip("How strongly the branches bend toward the growth direction; negative bends them away.");
    ImGui::Spacing();
    return changed;
}

bool TreeGeneratorPanel::RenderBranchLevel(int level)
{
    bool changed = false;
    ImGui::Spacing();
    if (level > 0)
    {
        changed |= UI::Prop::DragInt("Count per parent", &options_.branch.children[level], 0.1f, 0, 120);
        UI::ItemTooltip("How many of these branches grow from each branch of the level before.");
        changed |= UI::Prop::DragFloat("Start on parent", &options_.branch.start[level], 0.01f, 0.0f, 0.95f, "%.2f");
        UI::ItemTooltip("Where along the parent branch they begin: 0 at its base, 1 at its tip.");
        changed |= UI::Prop::DragFloat("Angle from parent", &options_.branch.angle[level], 0.5f, 0.0f, 180.0f, "%.1f deg");
        UI::ItemTooltip("How far they lean away from the parent branch.");
    }
    changed |= UI::Prop::DragFloat("Length", &options_.branch.length[level], 0.05f, 0.1f, 100.0f, "%.2f m");
    UI::ItemTooltip(level == 0 ? "The trunk's length." : "These branches' length.");
    changed |= UI::Prop::DragFloat("Radius", &options_.branch.radius[level], 0.01f, 0.01f, 5.0f, "%.3f m");
    UI::ItemTooltip("The radius at the base.");
    changed |= UI::Prop::DragFloat("Taper", &options_.branch.taper[level], 0.01f, 0.0f, 1.0f, "%.2f");
    UI::ItemTooltip("The radius at the tip, as a share of the radius at the base.");
    changed |= UI::Prop::DragFloat("Gnarliness", &options_.branch.gnarliness[level], 0.01f, -3.0f, 3.0f, "%.2f");
    UI::ItemTooltip("How much the branch wanders as it grows.");
    changed |= UI::Prop::DragFloat("Twist", &options_.branch.twist[level], 0.5f, -180.0f, 180.0f, "%.1f deg");
    UI::ItemTooltip("How far the bark turns around the branch from one ring to the next.");
    changed |= UI::Prop::DragInt("Length segments", &options_.branch.sections[level], 0.1f, 1, 32);
    UI::ItemTooltip("Rings along the branch: more bend more smoothly, and cost triangles.");
    changed |= UI::Prop::DragInt("Radial segments", &options_.branch.segments[level], 0.1f, 3, 32);
    UI::ItemTooltip("Sides around the branch: more look rounder, and cost triangles.");
    return changed;
}

bool TreeGeneratorPanel::RenderMaterialField(const char* label,
                                             const char* slotName,
                                             std::optional<Guid>& material,
                                             std::array<float, 4>& previewColor,
                                             const char* tooltip)
{
    ImGui::PushID(slotName);
    std::string name = "None (built-in)";
    if (material)
    {
        const std::optional<std::filesystem::path> path = AssetDatabase::Instance().resolveGuid(*material);
        name = path ? path->stem().string() : std::string("Missing material");
    }
    const auto guidOf = [&](const AssetLibrary::Entry& entry) {
        std::filesystem::path path =
            entry.originalPath.empty() ? assetLibrary_->AbsolutePath(entry) : std::filesystem::path(entry.originalPath);
        if (path.is_relative())
            path = assetLibrary_->AbsolutePath(entry);
        return AssetDatabase::Instance().getOrCreateGuid(path);
    };
    std::optional<Guid> picked;
    bool cleared = false;
    if (UI::AssetField(label, ICON_FA_PALETTE, name, tooltip))
        ImGui::OpenPopup("PickMaterial");
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_ID"))
        {
            const std::string droppedId(static_cast<const char*>(payload->Data), static_cast<std::size_t>(payload->DataSize));
            const std::optional<AssetLibrary::Entry> entry =
                assetLibrary_ ? assetLibrary_->FindById(droppedId) : std::optional<AssetLibrary::Entry>{};
            if (entry && entry->category == AssetLibrary::Category::Material)
                picked = guidOf(*entry);
            else
                status_ = std::string(slotName) + " material: drop a Material asset";
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopup("PickMaterial"))
    {
        if (ImGui::Selectable("None (built-in)", !material))
            cleared = true;
        if (assetLibrary_)
        {
            // (With its folder: every saved tree has a "bark" and a "leaves".)
            for (const AssetLibrary::Entry& entry : assetLibrary_->EntriesFor(AssetLibrary::Category::Material))
            {
                const bool current = material && !entry.guid.empty() && entry.guid == material->toString();
                if (ImGui::Selectable((entry.displayName + "##" + entry.id).c_str(), current))
                    picked = guidOf(entry);
                if (!entry.subpath.empty())
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", entry.subpath.c_str());
                }
            }
        }
        ImGui::EndPopup();
    }
    bool changed = false;
    if (picked)
    {
        material = *picked;
        previewColor = MaterialPreviewColor(*picked, previewColor[3]);
        const std::optional<std::filesystem::path> path = AssetDatabase::Instance().resolveGuid(*picked);
        status_ = std::string(slotName) + " material: " + (path ? path->stem().string() : std::string("?"));
        Tracenf("[TREE-3] material slot=%s guid=%s", slotName, picked->toString().c_str());
        changed = true;
    }
    else if (cleared && material)
    {
        material.reset();
        status_ = std::string(slotName) + " material: built-in";
        changed = true;
    }
    ImGui::PopID();
    return changed;
}

bool TreeGeneratorPanel::RenderBark()
{
    if (!ImGui::CollapsingHeader(ICON_FA_TREE " Bark", ImGuiTreeNodeFlags_DefaultOpen))
        return false;
    RenderMaterialField("Material", "bark", barkMaterial_, barkMaterialColor_,
        "The bark's material. Drop a Material from the Asset Browser here, or click to pick one; "
        "with none, the built-in bark below is used.");
    ImGui::BeginDisabled(barkMaterial_.has_value());
    static const char* const barkTypes[] = {"Oak", "Birch", "Pine", "Willow", "Ash"};
    ComboEnum("Built-in bark", options_.bark.type, barkTypes, 5);
    ImGui::EndDisabled();
    bool changed = DragVec2("Texture repeat", options_.bark.textureScale, 0.02f, 0.1f, 20.0f);
    UI::ItemTooltip("How many times the bark texture wraps around each branch (x) and repeats along it (y).");
    ImGui::Spacing();
    return changed;
}

bool TreeGeneratorPanel::RenderLeaves()
{
    if (!ImGui::CollapsingHeader(ICON_FA_LAYER_GROUP " Leaves", ImGuiTreeNodeFlags_DefaultOpen))
        return false;
    RenderMaterialField("Material", "leaves", leafMaterial_, leafMaterialColor_,
        "The leaves' material: alpha-masked, its texture holding the leaf images (see Atlas columns and rows). "
        "Drop a Material from the Asset Browser here, or click to pick one; with none, the built-in leaves below are used.");
    const MaterialAsset* material = leafMaterial_ ? MaterialAssetManager::Instance().getOrLoad(*leafMaterial_) : nullptr;
    if (material && material->alphaMode != MaterialAsset::AlphaMode::Mask)
        UI::StatusWarning("This material is not alpha-masked: the leaf cards will draw as solid squares.");
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Saving fits each leaf card to the visible part of the leaf texture (an alpha-masked one): "
                        "after changing the leaf material's texture or alpha mode, save the tree again.");
    ImGui::PopTextWrapPos();
    ImGui::BeginDisabled(leafMaterial_.has_value());
    static const char* const leafTypes[] = {"Oak", "Ash", "Pine", "Willow", "Birch"};
    ComboEnum("Built-in leaves", options_.leaves.type, leafTypes, 5);
    ImGui::EndDisabled();

    bool changed = false;
    changed |= UI::Prop::SliderInt("Atlas columns", &options_.leaves.atlasGridX, 1, 8);
    UI::ItemTooltip("How many leaf images the texture holds across (the built-in textures: 2 by 2); each card shows one.");
    changed |= UI::Prop::SliderInt("Atlas rows", &options_.leaves.atlasGridY, 1, 8);
    UI::ItemTooltip("How many leaf images the texture holds down.");
    changed |= UI::Prop::DragInt("Clusters per twig", &options_.leaves.count, 0.1f, 0, 120);
    UI::ItemTooltip("Leaf clusters along each last-level branch.");
    changed |= UI::Prop::SliderInt("Cards per cluster", &options_.leaves.cardsPerCluster, 1, 7);
    UI::ItemTooltip("Leaf cards in a cluster, turned around it (2: a crossed pair). Every card is drawn in every view "
                    "and shadow: fewer cards make a faster forest.");
    changed |= UI::Prop::DragFloat("Start on twig", &options_.leaves.start, 0.01f, 0.0f, 0.95f, "%.2f");
    UI::ItemTooltip("Where along each twig the clusters begin: 0 at its base, 1 at its tip.");
    changed |= UI::Prop::DragFloat("Size", &options_.leaves.size, 0.01f, 0.02f, 8.0f, "%.2f m");
    UI::ItemTooltip("A leaf card's width and height.");
    changed |= UI::Prop::DragFloat("Size variance", &options_.leaves.sizeVariance, 0.01f, 0.0f, 1.0f, "%.2f");
    UI::ItemTooltip("How much the card sizes differ: 0.3 is up to 30% smaller or larger.");
    changed |= UI::Prop::DragFloat("Tilt", &options_.leaves.angle, 0.5f, 0.0f, 90.0f, "%.1f deg");
    UI::ItemTooltip("How far the cards lean out of their cluster.");
    if (material)
    {
        UI::Prop::Text("Alpha cutoff", "%.2f (the material's)", material->alphaCutoff);
    }
    else
    {
        UI::Prop::DragFloat("Alpha cutoff", &options_.leaves.alphaTest, 0.01f, 0.0f, 1.0f, "%.2f");
        UI::ItemTooltip("Texels less opaque than this are cut out of the leaf cards.");
    }
    ImGui::Spacing();
    return changed;
}

void TreeGeneratorPanel::RenderOutput()
{
    if (!ImGui::CollapsingHeader(ICON_FA_FLOPPY_DISK " Output", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    UI::Prop::Text("Triangles", "%d (bark %d, leaves %d)",
        mesh_.stats.barkTriangles + mesh_.stats.leafTriangles,
        mesh_.stats.barkTriangles,
        mesh_.stats.leafTriangles);
    UI::Prop::Text("Leaf cards", "%zu", mesh_.leaves.vertices.size() / 4u);
    UI::Prop::Text("Leaf area", "%.0f m2", leafCardArea_);
    UI::ItemTooltip("The leaf cards' area per tree: every view and shadow cascade draws all of it. Fewer or smaller "
                    "leaves make the cheapest forest; saving trims the texture's transparent borders away.");
    UI::Prop::Text("Height", "%.1f m", mesh_.bboxMax.y - mesh_.bboxMin.y);
    UI::Prop::Text("Generated in", "%.2f ms", mesh_.stats.generationMs);
    ImGui::Checkbox("Bake distant-tree impostor", &bakeImpostor_);
    UI::ItemTooltip("Save an atlas of the tree for distant views. The game uses lit billboards beyond the chosen distance.");
    if (bakeImpostor_)
    {
        UI::Prop::SliderFloat("Impostor distance", &impostorDistance_, 50.0f, 500.0f, "%.0f m");
        UI::Prop::SliderFloat("Transition range", &impostorTransition_, 5.0f, 100.0f, "%.0f m");
        const char* resolutions[] = {"128", "256", "512"};
        int resolution = impostorResolution_ == 128 ? 0 : impostorResolution_ == 512 ? 2 : 1;
        if (UI::Prop::Combo("Atlas view size", &resolution, resolutions, 3)) impostorResolution_ = 128 << resolution;
        UI::ItemTooltip("8 directions and 3 elevations. Larger images use more texture memory and take longer to bake.");
    }
    ImGui::Spacing();
    if (UI::IconButton(ICON_FA_CAMERA, "Reset View"))
        preview_.ResetView(mesh_);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, UI::Theme::Accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, UI::Theme::AccentHovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, UI::Theme::AccentActive);
    if (UI::IconButton(ICON_FA_FLOPPY_DISK, "Save as Asset..."))
        savePopupRequested_ = true;
    ImGui::PopStyleColor(3);
    if (!status_.empty())
        ImGui::TextDisabled("%s", status_.c_str());
}

TreeMaterialBinding TreeGeneratorPanel::CurrentMaterialBinding()
{
    TreeTexturePalette::Instance().EnsureLoaded(InternalRoot());
    const TreePaletteTexture& bark = TreeTexturePalette::Instance().Bark(options_.bark.type);
    const TreePaletteTexture& leaf = TreeTexturePalette::Instance().Leaf(options_.leaves.type);
    TreeMaterialBinding binding{};
    binding.barkMaterial = barkMaterial_;
    binding.leafMaterial = leafMaterial_;
    binding.barkBaseColorTexturePath = bark.path;
    binding.leafBaseColorTexturePath = leaf.path;
    binding.leafAlphaCutoff = options_.leaves.alphaTest;
    binding.impostor.enabled = bakeImpostor_;
    binding.impostor.distance = impostorDistance_;
    binding.impostor.transition = impostorTransition_;
    binding.impostor.resolution = impostorResolution_;
    return binding;
}

TreePreviewStyle TreeGeneratorPanel::CurrentPreviewStyle() const
{
    const TreePaletteTexture& bark = TreeTexturePalette::Instance().Bark(options_.bark.type);
    const TreePaletteTexture& leaf = TreeTexturePalette::Instance().Leaf(options_.leaves.type);
    TreePreviewStyle style{};
    style.barkColor = barkMaterial_ ? barkMaterialColor_ : MultiplyColor(bark.previewColor, options_.bark.tint);
    style.leafColor = leafMaterial_ ? leafMaterialColor_ : MultiplyColor(leaf.previewColor, options_.leaves.tint);
    style.leafAlphaCutoff = options_.leaves.alphaTest;
    style.barkTextured = options_.bark.textured;
    style.leafTextured = true;
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
                if (result.impostorBaked) status_ += " (distant impostor ready)";
                else if (!result.impostorWarning.empty()) status_ += " (impostor unavailable: " + result.impostorWarning + ")";
                if (result.leafAreaReduction > 0.0f)
                {
                    char trimmed[64];
                    std::snprintf(trimmed, sizeof(trimmed), " (leaf area -%.0f%%)", result.leafAreaReduction * 100.0f);
                    status_ += trimmed;
                }
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
