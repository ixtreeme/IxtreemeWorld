// This file is included from EditorImGui.cpp inside the editor-enabled implementation block.
// Keep shared anonymous-namespace helpers in EditorImGui.cpp until this panel group is fully decoupled.

namespace
{
constexpr ImU32 kAssetTileColor = IM_COL32(44, 48, 55, 255);
constexpr ImU32 kAssetTileHoveredColor = IM_COL32(56, 61, 70, 255);
constexpr ImU32 kAssetTileBorderColor = IM_COL32(62, 67, 76, 255);
constexpr ImU32 kFolderIconColor = IM_COL32(226, 182, 84, 255);
constexpr float kAssetTileSize = 76.0f;
constexpr float kAssetCellWidth = 124.0f;

// The name under a tile: centred on it, cut with an ellipsis when wider than the cell. Returns
// whether it is hovered.
bool AssetTileLabel(const std::string& text, float tileSize, bool selected)
{
    const float maxWidth = kAssetCellWidth - 6.0f;
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const ImVec2 textSize = ImGui::CalcTextSize(text.c_str());
    const float width = std::min(textSize.x, maxWidth);
    const ImVec2 pos(cursor.x + (tileSize - width) * 0.5f, cursor.y);
    ImGui::SetCursorScreenPos(pos);
    ImGui::Dummy(ImVec2(width, textSize.y));
    const bool hovered = ImGui::IsItemHovered();
    if (selected)
        ImGui::PushStyleColor(ImGuiCol_Text, UI::Theme::AccentHovered);
    ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(),
        pos,
        ImVec2(pos.x + width, pos.y + textSize.y),
        pos.x + width,
        text.c_str(),
        nullptr,
        &textSize);
    if (selected)
        ImGui::PopStyleColor();
    return hovered;
}
}

void EditorImGui::RenderAssetBrowserToolbar()
{
    const bool hasLibrary = m_assetLibrary != nullptr;
    if (!hasLibrary)
        ImGui::BeginDisabled();
    if (UI::IconButton(ICON_FA_PLUS, "Create"))
        ImGui::OpenPopup("AssetBrowserCreatePopup");
    UI::ItemTooltip("Create a folder, script or material in the folder shown");
    if (ImGui::BeginPopup("AssetBrowserCreatePopup"))
    {
        RenderAssetCreateMenuItems(m_assetSubpath);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (UI::IconButton(ICON_FA_FILE_IMPORT, "Import"))
        OpenImportAssetDialog(m_assetSubpath);
    UI::ItemTooltip("Copy files into the folder shown (dropping files from the OS works too)");
    if (!hasLibrary)
        ImGui::EndDisabled();

    // Right: search and refresh; the breadcrumb takes the room between.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float refreshWidth = ImGui::GetFrameHeight();
    const float searchWidth = std::clamp(ImGui::GetContentRegionAvail().x * 0.28f, 140.0f, 280.0f);
    const float rightStart = ImGui::GetWindowContentRegionMax().x - searchWidth - refreshWidth - style.ItemSpacing.x;

    ImGui::SameLine(0.0f, 16.0f);
    RenderAssetBrowserBreadcrumb();

    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 8.0f, rightStart));
    ImGui::SetNextItemWidth(searchWidth);
    ImGui::InputTextWithHint("##asset_search", ICON_FA_MAGNIFYING_GLASS "  Search assets", m_assetSearchBuffer, sizeof(m_assetSearchBuffer));
    UI::ItemTooltip("Find assets by name in the whole project");
    ImGui::SameLine();
    if (UI::IconOnlyButton(ICON_FA_ARROWS_ROTATE))
        RefreshAssetLibrary();
    UI::ItemTooltip("Re-read the asset folder (changes made outside the editor show up by themselves)");
}

void EditorImGui::RenderAssetCreateMenuItems(const std::string& targetSubpath)
{
    const std::string target = AssetLibrary::NormalizeSubpath(targetSubpath);
    if (ImGui::MenuItem(ICON_FA_FOLDER_PLUS "  New Folder"))
    {
        m_assetNewFolderParent = target;
        CopyToBuffer(m_newAssetFolderName, sizeof(m_newAssetFolderName), UniqueFolderName(AssetBrowserPath(target)));
        m_assetOpenNewFolderPopup = true;
    }
    ImGui::Separator();
    // Each creator consumes m_assetCreateTarget (see CreateTargetSubpath).
    const auto createItem = [&](const char* label, void (EditorImGui::*create)()) {
        if (ImGui::MenuItem(label))
        {
            m_assetCreateTarget = target;
            (this->*create)();
            m_assetCreateTarget.reset();
        }
    };
    createItem(ICON_FA_FILE_CODE "  New AngelScript", &EditorImGui::CreateAngelScriptAsset);
    createItem(ICON_FA_FILE_CODE "  New Lua Script", &EditorImGui::CreateLuaScriptAsset);
    ImGui::Separator();
    createItem(ICON_FA_PALETTE "  New Material", &EditorImGui::CreatePbrMaterialAsset);
    createItem(ICON_FA_DROPLET "  New Water Material", static_cast<void (EditorImGui::*)()>(&EditorImGui::CreateWaterMaterialAsset));
    createItem(ICON_FA_CUBES "  New Physics Material", &EditorImGui::CreatePhysicsMaterialAsset);
    createItem(ICON_FA_DIAGRAM_PROJECT "  New Animator Controller", &EditorImGui::CreateAnimatorControllerAsset);
    createItem(ICON_FA_SHAPES "  New Particle Effect", &EditorImGui::CreateParticleEffectAsset);
    ImGui::Separator();
    if (ImGui::MenuItem(ICON_FA_FILE_IMPORT "  Import Asset..."))
        OpenImportAssetDialog(target);
}

void EditorImGui::AcceptAssetBrowserDrop(const std::string& targetSubpath)
{
    if (!ImGui::BeginDragDropTarget())
        return;
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayloadType))
    {
        const std::string assetId(static_cast<const char*>(payload->Data), payload->DataSize);
        MoveAssetEntryToFolder(assetId, targetSubpath);
    }
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetFolderPayloadType))
    {
        const std::string source(static_cast<const char*>(payload->Data), payload->DataSize);
        MoveFolderToFolder(source, targetSubpath);
    }
    ImGui::EndDragDropTarget();
}

void EditorImGui::RenderAssetTile(const AssetLibrary::Entry& entry, float tileSize)
{
    ImGui::PushID(entry.id.c_str());
    const bool selected = entry.id == m_selectedAssetId;
    const ImVec4 categoryColor = AssetCategoryColor(entry.category);
    AssetPreviewTexture* preview = GetAssetPreviewTexture(entry);

    const ImVec2 previewMin = ImGui::GetCursorScreenPos();
    const ImVec2 previewMax(previewMin.x + tileSize, previewMin.y + tileSize);
    if (entry.category == AssetLibrary::Category::Model)
    {
        const bool expanded = m_expandedModelAssets.contains(entry.id);
        ImGui::SetCursorScreenPos(ImVec2(previewMin.x - 23.0f, previewMin.y + (tileSize - ImGui::GetFrameHeight()) * 0.5f));
        if (ImGui::ArrowButton("##model_contents", expanded ? ImGuiDir_Down : ImGuiDir_Right))
        {
            if (expanded) m_expandedModelAssets.erase(entry.id);
            else m_expandedModelAssets.insert(entry.id);
        }
        if (ImGui::IsItemHovered())
        {
            const auto& contents = QueryModelContents(entry);
            ImGui::BeginTooltip();
            ImGui::Text("%zu materials/textures", contents.assets.size());
            if (!contents.missingTextures.empty())
                ImGui::TextColored(UI::Theme::Warning, "%zu missing textures", contents.missingTextures.size());
            ImGui::EndTooltip();
        }
        ImGui::SetCursorScreenPos(previewMin);
    }
    ImGui::InvisibleButton("##asset_tile", ImVec2(tileSize, tileSize));
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const bool doubleClicked = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    // A dark tile with the type's icon in the type's color (or the preview image when there is one).
    drawList->AddRectFilled(previewMin, previewMax, hovered ? kAssetTileHoveredColor : kAssetTileColor, 5.0f);
    void* previewTextureId =
        (preview && preview->handle.IsValid() && m_textureProvider)
        ? m_textureProvider->GetPreviewTexture(preview->handle)
        : nullptr;
    if (previewTextureId)
    {
        drawList->AddImage(
            reinterpret_cast<ImTextureID>(previewTextureId),
            previewMin,
            previewMax,
            ImVec2(0.0f, 0.0f),
            ImVec2(1.0f, 1.0f),
            IM_COL32_WHITE);
        drawList->AddRectFilled(
            ImVec2(previewMin.x, previewMax.y - 18.0f),
            previewMax,
            IM_COL32(10, 12, 16, 145),
            0.0f);
        drawList->AddText(ImVec2(previewMin.x + 6.0f, previewMax.y - 16.0f),
            IM_COL32(235, 238, 244, 235),
            AssetLibrary::TextureRoleBadge(entry.textureRole));
    }
    else
    {
        const char* icon = AssetCategoryIcon(entry.category);
        if (UI::GetEditorFonts().bold)
            ImGui::PushFont(UI::GetEditorFonts().bold);
        const ImVec2 iconSize = ImGui::CalcTextSize(icon);
        drawList->AddText(
            ImVec2(previewMin.x + (tileSize - iconSize.x) * 0.5f, previewMin.y + (tileSize - iconSize.y) * 0.5f),
            ImGui::ColorConvertFloat4ToU32(categoryColor),
            icon);
        if (UI::GetEditorFonts().bold)
            ImGui::PopFont();
    }
    drawList->AddRect(previewMin, previewMax,
        selected ? ImGui::ColorConvertFloat4ToU32(UI::Theme::AccentHovered) : kAssetTileBorderColor,
        5.0f,
        0,
        selected ? 2.5f : 1.0f);

    if (clicked)
    {
        m_selectedAssetId = entry.id;
        m_assetInspectorSelectionActive = true;
        m_assetStatus = "Selected: " + entry.displayName;
        if (doubleClicked && entry.category == AssetLibrary::Category::WaterMaterial)
        {
            OpenWaterMaterialEditor(entry.id);
        }
        else if (doubleClicked && entry.category == AssetLibrary::Category::Material)
        {
            OpenPbrMaterialEditor(entry.id);
        }
        else if (doubleClicked && entry.category == AssetLibrary::Category::Model)
        {
            m_commands.addMeshEntity = true;
            m_commands.meshAssetId = entry.id;
            m_assetStatus = "Mesh entity queued: " + entry.displayName;
            Tracenf("[MESH-ENTITY] Asset browser model spawn queued: asset_id=%s", entry.id.c_str());
        }
        else if (doubleClicked && entry.category == AssetLibrary::Category::Scene)
        {
            if (AttachSceneToHierarchy(entry))
                m_assetStatus = "Scene added to Hierarchy: " + entry.displayName;
        }
        else if (doubleClicked && entry.category == AssetLibrary::Category::Prefab)
        {
            m_commands.addPrefabInstance = true;
            m_commands.prefabAssetId = entry.id;
            m_assetStatus = "Prefab instance queued: " + entry.displayName;
            Tracenf("[PREFAB] Asset browser spawn queued: asset_id=%s", entry.id.c_str());
        }
        else if (doubleClicked && entry.category == AssetLibrary::Category::Audio)
        {
            m_commands.previewAudioClipId = entry.id;
            m_assetStatus = "Preview: " + entry.displayName;
        }
        else if (doubleClicked && (entry.category == AssetLibrary::Category::Script ||
                                      entry.category == AssetLibrary::Category::UiDocument))
        {
            // Scripts and UI documents are text: edit them in the OS's editor for that file type.
            std::string err;
            const std::filesystem::path path = m_assetLibrary->AbsolutePath(entry);
            if (platform::OpenInDefaultApp(path, &err))
                m_assetStatus = "Opened: " + entry.displayName;
            else
                m_assetStatus = "Open failed: " + err;
        }
    }

    if (ImGui::BeginDragDropSource())
    {
        ImGui::SetDragDropPayload(kAssetPayloadType, entry.id.data(), entry.id.size());
        ImGui::Text("%s", entry.displayName.c_str());
        ImGui::TextDisabled("%s", AssetLibrary::CategoryName(entry.category));
        if (m_loggedDragAssetId != entry.id)
        {
            m_loggedDragAssetId = entry.id;
            Tracenf("[EDITOR-IMGUI-3] Drag started: asset_id=%s type=%s",
                entry.id.c_str(),
                AssetLibrary::CategoryName(entry.category));
        }
        ImGui::EndDragDropSource();
    }

    if (ImGui::BeginPopupContextItem("AssetTileContext"))
    {
        ImGui::TextDisabled("%s", entry.displayName.c_str());
        if (ImGui::MenuItem("Rename"))
            BeginAssetRename(entry);
        if (ImGui::MenuItem("Delete"))
        {
            const std::filesystem::path path = entry.originalPath.empty() && m_assetLibrary
                ? m_assetLibrary->AbsolutePath(entry)
                : std::filesystem::path(entry.originalPath);
            m_assetDeletePath = path.generic_string();
            m_assetDeleteIsFolder = false;
            m_assetOpenDeletePopup = true;
        }
        if (entry.category == AssetLibrary::Category::Scene)
        {
            ImGui::Separator();
            if (ImGui::MenuItem("Add to Hierarchy"))
                AttachSceneToHierarchy(entry);
        }
        else if (entry.category == AssetLibrary::Category::Prefab)
        {
            ImGui::Separator();
            if (ImGui::MenuItem("Instantiate Prefab"))
            {
                m_commands.addPrefabInstance = true;
                m_commands.prefabAssetId = entry.id;
                m_assetStatus = "Prefab instance queued: " + entry.displayName;
                Tracenf("[PREFAB] Context instantiate queued: asset_id=%s", entry.id.c_str());
            }
        }
        else if (entry.category == AssetLibrary::Category::Audio)
        {
            ImGui::Separator();
            if (ImGui::MenuItem("Play Preview"))
            {
                m_commands.previewAudioClipId = entry.id;
                m_assetStatus = "Preview: " + entry.displayName;
            }
        }
        else if (entry.category == AssetLibrary::Category::Model)
        {
            ImGui::Separator();
            if (ImGui::MenuItem("Export to FBX..."))
                OpenFbxExportDialogForAsset(entry);
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("Create Here"))
        {
            RenderAssetCreateMenuItems(entry.subpath);
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }

    const bool labelHovered = AssetTileLabel(entry.filename.empty() ? entry.displayName : entry.filename, tileSize, selected);
    if (hovered || labelHovered)
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
        ImGui::TextUnformatted(entry.filename.empty() ? entry.displayName.c_str() : entry.filename.c_str());
        if (!entry.displayName.empty() && entry.displayName != entry.filename)
            ImGui::TextDisabled("Name: %s", entry.displayName.c_str());
        ImGui::TextDisabled("Type: %s", AssetLibrary::CategoryName(entry.category));
        if (entry.category == AssetLibrary::Category::Model)
        {
            const auto& contents = QueryModelContents(entry);
            ImGui::TextDisabled("Contents: %zu materials/textures (use the arrow to expand)", contents.assets.size());
            for (const auto& missing : contents.missingTextures)
                ImGui::TextColored(UI::Theme::Warning, "Missing texture: %s", missing.c_str());
        }
        if (!entry.subpath.empty())
            ImGui::TextDisabled("Folder: %s", entry.subpath.c_str());
        if (entry.category == AssetLibrary::Category::Texture)
        {
            ImGui::TextDisabled("Role: %s", AssetLibrary::TextureRoleName(entry.textureRole));
            if (entry.resolutionWidth > 0 && entry.resolutionHeight > 0)
                ImGui::TextDisabled("Size: %ux%u", entry.resolutionWidth, entry.resolutionHeight);
        }
        if (!entry.thumbnail.empty())
            ImGui::TextDisabled("Preview: %s", entry.thumbnail.c_str());
        if (!entry.tags.empty())
            ImGui::TextDisabled("Tags: %s", AssetLibrary::TagsToCsv(entry.tags).c_str());
        if (!entry.originalPath.empty())
            ImGui::TextDisabled("Source: %s", entry.originalPath.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    ImGui::PopID();
}

void EditorImGui::RenderAssetBrowserFolderTreeNode(const std::string& subpath)
{
    const std::string normalized = AssetLibrary::NormalizeSubpath(subpath);
    ImGui::PushID(normalized.empty() ? "__assets_root__" : normalized.c_str());
    const std::vector<std::string> children = QueryFilesystemChildFolders(normalized);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (children.empty())
        flags |= ImGuiTreeNodeFlags_Leaf;
    if (AssetLibrary::NormalizeSubpath(m_assetSubpath) == normalized)
        flags |= ImGuiTreeNodeFlags_Selected;
    const std::string label = normalized.empty() ? (std::string(ICON_FA_FOLDER_OPEN) + " Assets")
        : (std::string(ICON_FA_FOLDER) + " " + FolderDisplayName(normalized));
    const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        SelectAssetBrowserFolder(normalized);

    if (!normalized.empty() && ImGui::BeginDragDropSource())
    {
        ImGui::SetDragDropPayload(kAssetFolderPayloadType, normalized.data(), normalized.size());
        ImGui::Text("%s", FolderDisplayName(normalized).c_str());
        ImGui::EndDragDropSource();
    }

    AcceptAssetBrowserDrop(normalized);

    if (ImGui::BeginPopupContextItem("FolderTreeContext"))
    {
        if (!normalized.empty() && ImGui::MenuItem("Rename"))
            BeginFolderRename(normalized);
        if (!normalized.empty() && ImGui::MenuItem("Delete"))
        {
            m_assetDeletePath = AssetBrowserPath(normalized).generic_string();
            m_assetDeleteIsFolder = true;
            m_assetOpenDeletePopup = true;
        }
        if (!normalized.empty())
            ImGui::Separator();
        RenderAssetCreateMenuItems(normalized);
        ImGui::EndPopup();
    }

    if (open)
    {
        for (const std::string& child : children)
            RenderAssetBrowserFolderTreeNode(child);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void EditorImGui::RenderAssetBrowserFolderTree()
{
    RenderAssetBrowserFolderTreeNode("");
}

void EditorImGui::RenderAssetBrowserBreadcrumb()
{
    // Assets > folder > folder: every part but the last one opens that folder.
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, ImGui::GetStyle().FramePadding.y));
    const bool atRoot = AssetLibrary::NormalizeSubpath(m_assetSubpath).empty();
    if (atRoot)
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(ICON_FA_FOLDER_OPEN "  Assets");
    }
    else if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Assets"))
    {
        SelectAssetBrowserFolder("");
    }
    AcceptAssetBrowserDrop("");
    std::filesystem::path current(m_assetSubpath);
    std::string acc;
    for (const auto& part : current)
    {
        const std::string segment = part.generic_string();
        if (segment.empty() || segment == ".")
            continue;
        acc = acc.empty() ? segment : acc + "/" + segment;
        ImGui::SameLine(0.0f, 2.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled(ICON_FA_CHEVRON_RIGHT);
        ImGui::SameLine(0.0f, 2.0f);
        ImGui::PushID(acc.c_str());
        if (AssetLibrary::NormalizeSubpath(acc) == AssetLibrary::NormalizeSubpath(m_assetSubpath))
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(segment.c_str());
        }
        else if (ImGui::Button(segment.c_str()))
        {
            SelectAssetBrowserFolder(acc);
        }
        AcceptAssetBrowserDrop(acc);
        ImGui::PopID();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void EditorImGui::RenderAssetBrowserFolderTile(const std::string& subpath, float tileSize)
{
    ImGui::PushID(subpath.c_str());
    const bool selected = m_selectedAssetId == ("folder:" + subpath);
    const ImVec2 previewMin = ImGui::GetCursorScreenPos();
    const ImVec2 previewMax(previewMin.x + tileSize, previewMin.y + tileSize);
    ImGui::InvisibleButton("##folder_tile", ImVec2(tileSize, tileSize));
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const bool doubleClicked = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(previewMin, previewMax, hovered ? kAssetTileHoveredColor : kAssetTileColor, 5.0f);
    const char* icon = ICON_FA_FOLDER;
    if (UI::GetEditorFonts().bold)
        ImGui::PushFont(UI::GetEditorFonts().bold);
    const ImVec2 iconSize = ImGui::CalcTextSize(icon);
    drawList->AddText(ImVec2(previewMin.x + (tileSize - iconSize.x) * 0.5f, previewMin.y + (tileSize - iconSize.y) * 0.5f),
        kFolderIconColor,
        icon);
    if (UI::GetEditorFonts().bold)
        ImGui::PopFont();
    drawList->AddRect(previewMin, previewMax,
        selected ? ImGui::ColorConvertFloat4ToU32(UI::Theme::AccentHovered) : kAssetTileBorderColor,
        5.0f,
        0,
        selected ? 2.5f : 1.0f);
    if (clicked)
    {
        m_selectedAssetId = "folder:" + subpath;
        m_assetInspectorSelectionActive = false;
        if (doubleClicked)
            SelectAssetBrowserFolder(subpath);
    }
    if (ImGui::BeginDragDropSource())
    {
        ImGui::SetDragDropPayload(kAssetFolderPayloadType, subpath.data(), subpath.size());
        ImGui::Text("%s", FolderDisplayName(subpath).c_str());
        ImGui::EndDragDropSource();
    }
    AcceptAssetBrowserDrop(subpath);
    if (ImGui::BeginPopupContextItem("FolderTileContext"))
    {
        if (ImGui::MenuItem("Rename"))
            BeginFolderRename(subpath);
        if (ImGui::MenuItem("Delete"))
        {
            m_assetDeletePath = AssetBrowserPath(subpath).generic_string();
            m_assetDeleteIsFolder = true;
            m_assetOpenDeletePopup = true;
        }
        ImGui::Separator();
        RenderAssetCreateMenuItems(subpath);
        ImGui::EndPopup();
    }
    const bool labelHovered = AssetTileLabel(FolderDisplayName(subpath), tileSize, selected);
    if (hovered || labelHovered)
        ImGui::SetTooltip("%s\nDouble-click to open", FolderDisplayName(subpath).c_str());
    ImGui::PopID();
}

void EditorImGui::RenderAssetBrowserContent()
{
    // With a search, every matching asset of the project; otherwise the folder shown.
    const bool searching = m_assetSearchBuffer[0] != '\0';
    std::vector<std::string> folders;
    std::vector<AssetLibrary::Entry> assets;
    const std::vector<std::string>* folderView = &folders;
    const std::vector<AssetLibrary::Entry>* assetView = &assets;
    if (searching)
    {
        for (const AssetLibrary::Entry& entry : m_assetLibrary->Entries())
        {
            if (ContainsCaseInsensitive(entry.displayName, m_assetSearchBuffer) ||
                ContainsCaseInsensitive(entry.filename, m_assetSearchBuffer))
                assets.push_back(entry);
        }
        std::sort(assets.begin(), assets.end(), [](const AssetLibrary::Entry& a, const AssetLibrary::Entry& b) {
            return ToLowerAscii(a.displayName) < ToLowerAscii(b.displayName);
        });
        ImGui::TextDisabled("%zu asset%s found for \"%s\"", assets.size(), assets.size() == 1 ? "" : "s", m_assetSearchBuffer);
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_XMARK "  Clear search"))
            m_assetSearchBuffer[0] = '\0';
        ImGui::Spacing();
    }
    else
    {
        folderView = &QueryFilesystemChildFolders(m_assetSubpath);
        assetView = &QueryFilesystemAssetsInFolder(m_assetSubpath);
    }

    const float panelWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const int columns = std::max(1, static_cast<int>(panelWidth / (kAssetCellWidth + ImGui::GetStyle().ItemSpacing.x)));
    if (ImGui::BeginTable("AssetBrowserGrid", columns, ImGuiTableFlags_SizingFixedSame | ImGuiTableFlags_NoSavedSettings))
    {
        for (int i = 0; i < columns; ++i)
            ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthFixed, kAssetCellWidth);
        const auto beginCell = []() {
            ImGui::TableNextColumn();
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (kAssetCellWidth - kAssetTileSize) * 0.5f);
            ImGui::BeginGroup();
        };
        for (const std::string& folder : *folderView)
        {
            beginCell();
            RenderAssetBrowserFolderTile(folder, kAssetTileSize);
            ImGui::EndGroup();
        }
        for (const AssetLibrary::Entry& entry : *assetView)
        {
            beginCell();
            RenderAssetTile(entry, kAssetTileSize);
            ImGui::EndGroup();
            if (entry.category == AssetLibrary::Category::Model && m_expandedModelAssets.contains(entry.id))
            {
                const auto& contents = QueryModelContents(entry);
                ImGui::PushID(entry.id.c_str());
                for (const auto& child : contents.assets)
                {
                    beginCell();
                    const ImVec2 childMin = ImGui::GetCursorScreenPos();
                    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(childMin.x - 5.0f, childMin.y - 3.0f),
                        ImVec2(childMin.x + kAssetTileSize + 5.0f, childMin.y + kAssetTileSize + 24.0f),
                        IM_COL32(45, 65, 78, 120), 5.0f);
                    RenderAssetTile(child, kAssetTileSize);
                    ImGui::EndGroup();
                }
                if (contents.assets.empty())
                {
                    beginCell();
                    ImGui::TextDisabled(contents.missingTextures.empty() ? "No materials\nor textures" : "Missing textures\nSee model tooltip");
                    ImGui::EndGroup();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    if (folderView->empty() && assetView->empty())
    {
        ImGui::Spacing();
        ImGui::TextDisabled(searching ? "Nothing matches the search."
                                      : "This folder is empty. Drop files here from the OS, or use Create / Import above.");
    }

    // The empty area under the tiles takes drops too (into the folder shown) and opens the create menu.
    const ImVec2 remaining = ImGui::GetContentRegionAvail();
    if (!searching && remaining.x > 1.0f && remaining.y > 1.0f)
    {
        ImGui::InvisibleButton("##asset_content_drop", remaining);
        AcceptAssetBrowserDrop(m_assetSubpath);
        ImGui::OpenPopupOnItemClick("AssetBrowserContentContext", ImGuiPopupFlags_MouseButtonRight);
    }

    if (ImGui::BeginPopupContextWindow("AssetBrowserContentContext", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
    {
        RenderAssetCreateMenuItems(m_assetSubpath);
        ImGui::EndPopup();
    }
}

void EditorImGui::RenderAssetBrowserOperationPopups()
{
    if (m_assetOpenNewFolderPopup)
    {
        ImGui::OpenPopup("NewAssetFolderUnity");
        m_assetOpenNewFolderPopup = false;
    }
    if (m_assetOpenRenamePopup)
    {
        ImGui::OpenPopup("RenameAssetBrowserItem");
        m_assetOpenRenamePopup = false;
    }
    if (m_assetOpenDeletePopup)
    {
        ImGui::OpenPopup("DeleteAssetBrowserItem");
        m_assetOpenDeletePopup = false;
    }
    if (m_assetOpenImportPopup)
    {
        ImGui::OpenPopup("ImportAssetIntoFolder");
        m_assetOpenImportPopup = false;
    }

    if (ImGui::BeginPopupModal("NewAssetFolderUnity", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::InputText("Name", m_newAssetFolderName, sizeof(m_newAssetFolderName));
        if (ImGui::Button("Create") || ImGui::IsKeyPressed(ImGuiKey_Enter))
        {
            CreateFilesystemFolder(m_assetNewFolderParent, m_newAssetFolderName);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("RenameAssetBrowserItem", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::InputText("Name", m_assetRenameBuffer, sizeof(m_assetRenameBuffer));
        if (ImGui::Button("Rename") || ImGui::IsKeyPressed(ImGuiKey_Enter))
        {
            RenameFilesystemSelection();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("DeleteAssetBrowserItem", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        const std::filesystem::path path(m_assetDeletePath);
        ImGui::Text("Move '%s' to recycle bin?", path.filename().string().c_str());
        if (ImGui::Button("Yes"))
        {
            DeleteFilesystemSelection();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("No") || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("ImportAssetIntoFolder", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        const std::filesystem::path target = AssetBrowserPath(m_assetImportTargetSubpath);
        ImGui::TextDisabled("Target: %s", target.generic_string().c_str());
        ImGui::TextWrapped("Choose a supported source file, or drag files from Explorer onto the editor window.");
        ImGui::InputText("Source File", m_assetImportPathBuffer, sizeof(m_assetImportPathBuffer));
        ImGui::TextDisabled("Supported: png jpg jpeg tga bmp dds ktx hdr exr glb gltf fbx obj material anim ozz wav ogg mp3 flac");

        ImGui::Separator();
        ImGui::InputText("Browse Path", m_assetImportBrowserPathBuffer, sizeof(m_assetImportBrowserPathBuffer));
        ImGui::SameLine();
        if (ImGui::Button("Go"))
        {
            std::error_code ec;
            std::filesystem::path requested(m_assetImportBrowserPathBuffer);
            requested = std::filesystem::absolute(requested, ec);
            if (!ec && std::filesystem::exists(requested, ec))
            {
                m_assetImportBrowserPath = std::filesystem::is_directory(requested, ec) ? requested : requested.parent_path();
                CopyToBuffer(m_assetImportBrowserPathBuffer, sizeof(m_assetImportBrowserPathBuffer), m_assetImportBrowserPath.string());
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Up"))
        {
            if (m_assetImportBrowserPath.has_parent_path())
                m_assetImportBrowserPath = m_assetImportBrowserPath.parent_path();
            CopyToBuffer(m_assetImportBrowserPathBuffer, sizeof(m_assetImportBrowserPathBuffer), m_assetImportBrowserPath.string());
        }
        ImGui::InputText("Filter", m_assetImportFilterBuffer, sizeof(m_assetImportFilterBuffer));

        std::vector<std::filesystem::path> folders;
        std::vector<std::filesystem::path> files;
        std::error_code ec;
        if (std::filesystem::exists(m_assetImportBrowserPath, ec) && std::filesystem::is_directory(m_assetImportBrowserPath, ec))
        {
            for (const auto& entry : std::filesystem::directory_iterator(m_assetImportBrowserPath, std::filesystem::directory_options::skip_permission_denied, ec))
            {
                if (ec)
                    break;
                std::error_code itemEc;
                if (entry.is_directory(itemEc))
                {
                    folders.push_back(entry.path());
                }
                else if (entry.is_regular_file(itemEc) && IsSupportedImportFile(entry.path()) &&
                    ContainsCaseInsensitive(entry.path().filename().string(), m_assetImportFilterBuffer))
                {
                    files.push_back(entry.path());
                }
            }
        }
        std::sort(folders.begin(), folders.end(), [](const auto& a, const auto& b) {
            return ToLowerAscii(a.filename().string()) < ToLowerAscii(b.filename().string());
        });
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
            return ToLowerAscii(a.filename().string()) < ToLowerAscii(b.filename().string());
        });

        bool importedFromBrowser = false;
        if (ImGui::BeginChild("ImportAssetBrowserList", ImVec2(660.0f, 260.0f), true, ImGuiWindowFlags_HorizontalScrollbar))
        {
            for (const std::filesystem::path& folder : folders)
            {
                const std::string label = std::string(ICON_FA_FOLDER) + " " + folder.filename().string();
                if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) &&
                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    m_assetImportBrowserPath = folder;
                    CopyToBuffer(m_assetImportBrowserPathBuffer, sizeof(m_assetImportBrowserPathBuffer), m_assetImportBrowserPath.string());
                }
            }
            for (const std::filesystem::path& file : files)
            {
                const std::string label = std::string(ICON_FA_FILE) + " " + file.filename().string();
                const bool selected = ComparablePath(file) == ComparablePath(std::filesystem::path(m_assetImportPathBuffer));
                if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick))
                {
                    CopyToBuffer(m_assetImportPathBuffer, sizeof(m_assetImportPathBuffer), file.string());
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    {
                        ImportAssetFromPath(file, m_assetImportTargetSubpath, "context_menu");
                        importedFromBrowser = true;
                    }
                }
            }
        }
        ImGui::EndChild();
        if (importedFromBrowser)
        {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }

        if (ImGui::Button("Import") || ImGui::IsKeyPressed(ImGuiKey_Enter))
        {
            ImportAssetFromPath(std::filesystem::path(m_assetImportPathBuffer), m_assetImportTargetSubpath, "context_menu");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void EditorImGui::RenderCreatePbrMaterialPopup()
{
    if (m_assetOpenCreateMaterialPopup)
    {
        ImGui::OpenPopup("CreatePbrMaterialWithShadingMode");
        m_assetOpenCreateMaterialPopup = false;
    }

    if (!ImGui::BeginPopupModal("CreatePbrMaterialWithShadingMode", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::TextUnformatted("Create New Material");
    ImGui::InputText("Name", m_createMaterialName, sizeof(m_createMaterialName));
    ImGui::Separator();
    ImGui::TextUnformatted("Shading Mode");
    ImGui::RadioButton("Lit", &m_createMaterialShadingMode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Unlit", &m_createMaterialShadingMode, 1);
    ImGui::TextDisabled("Lit uses scene lighting. Unlit ignores lighting and uses base color/emissive.");

    const bool canCreate = m_assetLibrary && m_createMaterialName[0] != '\0' && m_createMaterialShadingMode >= 0;
    if (!canCreate)
        ImGui::BeginDisabled();
    if (ImGui::Button("Create") || (canCreate && ImGui::IsKeyPressed(ImGuiKey_Enter)))
    {
        AssetLibrary::ImportOptions options{};
        options.displayName = m_createMaterialName[0] != '\0' ? m_createMaterialName : "material";
        options.subpath = m_createMaterialTargetSubpath;
        options.tags = {"material"};
        AssetLibrary::MaterialData material{};
        material.shadingMode = m_createMaterialShadingMode == 1 ? "unlit" : "lit";

        AssetLibrary::Entry entry{};
        std::string error;
        if (!m_assetLibrary->CreateMaterial(options, material, entry, error))
        {
            m_assetStatus = "Material create failed: " + error;
        }
        else
        {
            RevealCreatedAsset(entry);
            m_assetStatus = "Material created: " + entry.displayName;
            Tracenf("[MATERIAL] created path=%s shadingMode=%s",
                m_assetLibrary->AbsolutePath(entry).generic_string().c_str(),
                material.shadingMode == "unlit" ? "Unlit" : "Lit");
            OpenPbrMaterialEditor(entry.id);
            ImGui::CloseCurrentPopup();
        }
    }
    if (!canCreate)
        ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void EditorImGui::OpenFbxExportDialogForAsset(const AssetLibrary::Entry& entry)
{
    m_fbxExportSource = FbxExportSource::Asset;
    m_fbxExportAssetId = entry.id;
    m_fbxExportMeshEntityId = 0;
    m_fbxExportEmbedTextures = true;
    m_fbxExportMaterials = true;
    m_fbxExportAnimations = true;
    CopyToBuffer(m_fbxExportPathBuffer,
        sizeof(m_fbxExportPathBuffer),
        DefaultFbxExportPath(entry.displayName.empty() ? entry.filename : entry.displayName).string());
    m_fbxExportPopupOpen = true;
}

void EditorImGui::OpenFbxExportDialogForMeshEntity(const HierarchySceneEntity& entity)
{
    m_fbxExportSource = FbxExportSource::MeshEntity;
    m_fbxExportAssetId.clear();
    m_fbxExportMeshEntityId = entity.objectId;
    m_fbxExportEmbedTextures = true;
    m_fbxExportMaterials = true;
    m_fbxExportAnimations = true;
    CopyToBuffer(m_fbxExportPathBuffer,
        sizeof(m_fbxExportPathBuffer),
        DefaultFbxExportPath(entity.name).string());
    m_fbxExportPopupOpen = true;
}

void EditorImGui::ExecuteFbxAssetExport()
{
    if (!m_assetLibrary)
        return;

    const auto entry = m_assetLibrary->FindById(m_fbxExportAssetId);
    if (!entry || entry->category != AssetLibrary::Category::Model)
    {
        m_assetStatus = "FBX export failed: model asset not found";
        return;
    }

    const std::filesystem::path source = entry->originalPath.empty()
        ? m_assetLibrary->AbsolutePath(*entry)
        : std::filesystem::path(entry->originalPath);
    AssimpExporter::ExportOptions options{};
    options.outputPath = std::filesystem::path(m_fbxExportPathBuffer);
    options.embedTextures = m_fbxExportEmbedTextures;
    options.exportMaterials = m_fbxExportMaterials;
    options.exportAnimations = m_fbxExportAnimations;

    std::string error;
    if (!AssimpExporter::ExportAssetToFbx(source, options, error))
    {
        m_assetStatus = "FBX export failed: " + error;
        return;
    }
    m_assetStatus = "FBX exported: " + options.outputPath.filename().string();
}

void EditorImGui::RenderFbxExportPopup()
{
    if (m_fbxExportPopupOpen)
    {
        ImGui::OpenPopup("ExportToFbx");
        m_fbxExportPopupOpen = false;
    }

    if (!ImGui::BeginPopupModal("ExportToFbx", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::TextUnformatted(m_fbxExportSource == FbxExportSource::MeshEntity
        ? "Export Selection to FBX"
        : "Export Asset to FBX");
    ImGui::InputText("File", m_fbxExportPathBuffer, sizeof(m_fbxExportPathBuffer));
    ImGui::Checkbox("Embed textures in FBX", &m_fbxExportEmbedTextures);
    ImGui::Checkbox("Export materials", &m_fbxExportMaterials);
    ImGui::Checkbox("Export animations", &m_fbxExportAnimations);
    ImGui::TextDisabled("FBX only. Source asset data is preserved through Assimp where supported.");

    const bool canExport = m_fbxExportPathBuffer[0] != '\0' && m_fbxExportSource != FbxExportSource::None;
    if (!canExport)
        ImGui::BeginDisabled();
    if (ImGui::Button("Export") || (canExport && ImGui::IsKeyPressed(ImGuiKey_Enter)))
    {
        if (m_fbxExportSource == FbxExportSource::Asset)
        {
            ExecuteFbxAssetExport();
        }
        else if (m_fbxExportSource == FbxExportSource::MeshEntity)
        {
            m_commands.exportMeshEntityToFbx = true;
            m_commands.exportMeshEntityId = m_fbxExportMeshEntityId;
            m_commands.exportFbxOutputPath = m_fbxExportPathBuffer;
            m_commands.exportFbxEmbedTextures = m_fbxExportEmbedTextures;
            m_commands.exportFbxMaterials = m_fbxExportMaterials;
            m_commands.exportFbxAnimations = m_fbxExportAnimations;
        }
        m_fbxExportSource = FbxExportSource::None;
        ImGui::CloseCurrentPopup();
    }
    if (!canExport)
        ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        m_fbxExportSource = FbxExportSource::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void EditorImGui::RenderAssetBrowser()
{
    if (!m_editorModeActive || !m_assetBrowserPanelOpen)
        return;

    if (ImGui::Begin(EditorWindow::AssetBrowser, &m_assetBrowserPanelOpen))
    {
        if (!m_assetLibrary)
        {
            ImGui::TextDisabled("Asset library is not initialized.");
            ImGui::End();
            return;
        }

        RenderAssetBrowserToolbar();
        ImGui::Separator();

        const float browserPanelHeight = std::max(140.0f, ImGui::GetContentRegionAvail().y);
        if (ImGui::BeginTable("AssetBrowserLayout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV))
        {
            ImGui::TableSetupColumn("Folders", ImGuiTableColumnFlags_WidthFixed, 210.0f);
            ImGui::TableSetupColumn("Content", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            if (ImGui::BeginChild("AssetFolderTreeScroll", ImVec2(0.0f, browserPanelHeight), false,
                    ImGuiWindowFlags_HorizontalScrollbar))
            {
                RenderAssetBrowserFolderTree();
            }
            ImGui::EndChild();

            ImGui::TableSetColumnIndex(1);
            if (ImGui::BeginChild("AssetContentScroll", ImVec2(0.0f, browserPanelHeight), false,
                    ImGuiWindowFlags_HorizontalScrollbar))
            {
                RenderAssetBrowserContent();
            }
            ImGui::EndChild();

            ImGui::EndTable();
        }
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
        {
            if (ImGui::IsKeyPressed(ImGuiKey_F2) && !m_selectedAssetId.empty())
            {
                if (m_selectedAssetId.rfind("folder:", 0) == 0)
                    BeginFolderRename(m_selectedAssetId.substr(7));
                else if (auto entry = m_assetLibrary->FindById(m_selectedAssetId))
                    BeginAssetRename(*entry);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !m_selectedAssetId.empty())
            {
                if (m_selectedAssetId.rfind("folder:", 0) == 0)
                {
                    const std::string folder = m_selectedAssetId.substr(7);
                    m_assetDeletePath = AssetBrowserPath(folder).generic_string();
                    m_assetDeleteIsFolder = true;
                    m_assetOpenDeletePopup = true;
                }
                else if (auto entry = m_assetLibrary->FindById(m_selectedAssetId))
                {
                    const std::filesystem::path path = entry->originalPath.empty() ? m_assetLibrary->AbsolutePath(*entry) : std::filesystem::path(entry->originalPath);
                    m_assetDeletePath = path.generic_string();
                    m_assetDeleteIsFolder = false;
                    m_assetOpenDeletePopup = true;
                }
            }
        }
        RenderAssetBrowserOperationPopups();
        if (!m_assetBrowserLogged)
        {
            m_assetBrowserLogged = true;
            const auto folders = QueryFilesystemChildFolders(m_assetSubpath);
            const auto assets = QueryFilesystemAssetsInFolder(m_assetSubpath);
            Tracenf("[EDITOR-IMGUI-3] Asset Browser rendered, current_folder=Assets/%s subfolders=%zu files=%zu",
                m_assetSubpath.c_str(),
                folders.size(),
                assets.size());
        }
    }
    ImGui::End();
}

void EditorImGui::RenderScriptsPanel()
{
    if (!m_editorModeActive || !m_scriptsPanelOpen)
        return;
    DockBesideIfUnplaced(EditorWindow::Scripts, EditorWindow::AssetBrowser);
    if (!ImGui::Begin(EditorWindow::Scripts, &m_scriptsPanelOpen))
    {
        ImGui::End();
        return;
    }

    const bool hasProject = ProjectManager::Instance().HasProject();

    auto openExternal = [this](const std::filesystem::path& p) {
        std::string err;
        if (platform::OpenInDefaultApp(p, &err))
            m_assetStatus = "Opened: " + p.filename().string();
        else
            m_assetStatus = "Open failed: " + err;
    };
    // Cached per asset-library revision: the panel renders every frame and EntriesFor copies the
    // whole Script category.
    const auto scriptAssetsWithExtension = [this](const char* extension) -> const std::vector<AssetLibrary::Entry>& {
        return CachedScriptAssets(extension);
    };
    const auto renderScriptList = [&](const std::vector<AssetLibrary::Entry>& scripts, const char* dragLabel) {
        for (const AssetLibrary::Entry& e : scripts)
        {
            ImGui::PushID(e.id.c_str());
            if (ImGui::Selectable((ICON_FA_FILE " " + e.displayName).c_str(), false,
                    ImGuiSelectableFlags_AllowDoubleClick) &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                openExternal(m_assetLibrary->AbsolutePath(e));
            }
            // Drag onto an entity row in the Hierarchy to attach it (Unity-style).
            if (ImGui::BeginDragDropSource())
            {
                ImGui::SetDragDropPayload(kAssetPayloadType, e.id.data(), e.id.size());
                ImGui::Text("%s", e.displayName.c_str());
                ImGui::TextDisabled("%s", dragLabel);
                ImGui::EndDragDropSource();
            }
            ImGui::PopID();
        }
    };

    // --- AngelScript (.as assets): a project scripting language (hot-reloaded in Play, like Lua) ---
    ImGui::SeparatorText("AngelScript (.as assets)");
    if (m_assetLibrary && ImGui::Button(ICON_FA_PLUS " New AngelScript"))
        CreateAngelScriptAsset();
    if (m_assetLibrary)
    {
        const std::vector<AssetLibrary::Entry>& scripts = scriptAssetsWithExtension(".as");
        if (scripts.empty())
            ImGui::TextDisabled("No .as scripts. Drop an .as into the asset browser or click New AngelScript.");
        renderScriptList(scripts, "AngelScript script");
    }
    if (!hasProject)
        ImGui::TextDisabled("No project open.");

    // --- Lua script assets ---
    ImGui::SeparatorText("Lua scripts (.lua assets)");
    if (m_assetLibrary && ImGui::Button(ICON_FA_PLUS " New Lua Script"))
        CreateLuaScriptAsset();
    if (m_assetLibrary)
    {
        const std::vector<AssetLibrary::Entry>& luaScripts = scriptAssetsWithExtension(".lua");
        if (luaScripts.empty())
            ImGui::TextDisabled("No .lua scripts. Drop a .lua into the asset browser.");
        renderScriptList(luaScripts, "Lua script");
    }

    // --- LEGACY native C++ sources: shown only while the project actually has .cpp Script assets ---
    if (hasProject && ProjectHasNativeScriptSources())
    {
        ImGui::SeparatorText("Legacy C++ source");
        ImGui::TextDisabled("C++ is no longer a project scripting language (use AngelScript or Lua).");
        const bool canBuild = m_playModeState.mode == EditorPlayMode::Edit && !IsBuildRunning();
        if (!canBuild)
            ImGui::BeginDisabled();
        if (ImGui::Button(IsBuildRunning() ? ICON_FA_HAMMER "  Compiling..." : ICON_FA_HAMMER "  Compile Legacy C++ Scripts"))
            m_commands.buildGameScripts = true;
        if (!canBuild)
            ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Checkbox("Compile on save", &m_autoBuildOnSave);

        const std::filesystem::path scriptsDir = ProjectScriptSourceDir();
        // The .cpp Script assets (anywhere in the asset folder): cached with the browser listings.
        ValidateAssetBrowserCache();
        if (!m_assetBrowserCache.nativeScriptSources && m_assetLibrary)
        {
            auto& sources = m_assetBrowserCache.nativeScriptSources.emplace();
            for (const AssetLibrary::Entry& e : m_assetLibrary->EntriesFor(AssetLibrary::Category::Script))
            {
                const std::filesystem::path p = m_assetLibrary->AbsolutePath(e);
                if (IsNativeScriptSource(scriptsDir, p))
                    sources.emplace_back(p, e.subpath.empty() ? e.filename : e.subpath + "/" + e.filename);
            }
            std::sort(sources.begin(), sources.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
        }
        if (!m_assetBrowserCache.nativeScriptSources)
            m_assetBrowserCache.nativeScriptSources.emplace();
        for (const auto& [path, rel] : *m_assetBrowserCache.nativeScriptSources)
        {
            ImGui::PushID(rel.c_str());
            if (ImGui::Selectable((ICON_FA_FILE " " + rel).c_str(), false,
                    ImGuiSelectableFlags_AllowDoubleClick) &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                openExternal(path);
            }
            ImGui::PopID();
        }
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Drag a script onto an entity in the Hierarchy to attach it; double-click to open.");
    ImGui::End();
}

