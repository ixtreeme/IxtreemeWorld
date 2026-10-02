// This file is included from EditorImGui.cpp inside the editor-enabled implementation block.
// Keep shared anonymous-namespace helpers in EditorImGui.cpp until this panel group is fully decoupled.

void EditorImGui::RenderAssetBrowserToolbar()
{
    if (UI::IconButton(ICON_FA_ROTATE, "Refresh"))
        RefreshAssetLibrary();
}

void EditorImGui::RenderAssetCreateMenuItems(const std::string& targetSubpath)
{
    const std::string target = AssetLibrary::NormalizeSubpath(targetSubpath);
    if (ImGui::MenuItem("New Folder"))
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
    createItem("New Lua Script", &EditorImGui::CreateLuaScriptAsset);
    createItem("New C++ Script", &EditorImGui::CreateNativeScriptAsset);
    ImGui::Separator();
    createItem("New Material", &EditorImGui::CreatePbrMaterialAsset);
    createItem("New Water Material", static_cast<void (EditorImGui::*)()>(&EditorImGui::CreateWaterMaterialAsset));
    createItem("New Physics Material", &EditorImGui::CreatePhysicsMaterialAsset);
    createItem("New Animator Controller", &EditorImGui::CreateAnimatorControllerAsset);
    ImGui::Separator();
    if (ImGui::MenuItem("Import Asset..."))
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
    ImGui::InvisibleButton("##asset_tile", ImVec2(tileSize, tileSize));
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const bool doubleClicked = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImU32 baseColor = ImGui::ColorConvertFloat4ToU32(hovered
        ? ImVec4(categoryColor.x + 0.08f, categoryColor.y + 0.08f, categoryColor.z + 0.08f, 1.0f)
        : categoryColor);
    drawList->AddRectFilled(previewMin, previewMax, baseColor, 5.0f);
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
            IM_COL32(245, 248, 255, 235),
            icon);
        if (UI::GetEditorFonts().bold)
            ImGui::PopFont();
    }
    drawList->AddRect(previewMin, previewMax,
        selected ? IM_COL32(255, 210, 92, 255) : IM_COL32(55, 60, 70, 255),
        5.0f,
        0,
        selected ? 3.0f : 1.0f);

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
        else if (doubleClicked && entry.category == AssetLibrary::Category::Script)
        {
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

    const std::string shortName = ShortAssetFilename(entry);
    ImGui::TextUnformatted(shortName.c_str());
    const bool labelHovered = ImGui::IsItemHovered();
    if (hovered || labelHovered)
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
        ImGui::TextUnformatted(entry.filename.empty() ? entry.displayName.c_str() : entry.filename.c_str());
        if (!entry.displayName.empty() && entry.displayName != entry.filename)
            ImGui::TextDisabled("Name: %s", entry.displayName.c_str());
        ImGui::TextDisabled("Type: %s", AssetLibrary::CategoryName(entry.category));
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
    if (ImGui::SmallButton("Assets"))
        SelectAssetBrowserFolder("");
    std::filesystem::path current(m_assetSubpath);
    std::string acc;
    for (const auto& part : current)
    {
        const std::string segment = part.generic_string();
        if (segment.empty() || segment == ".")
            continue;
        acc = acc.empty() ? segment : acc + "/" + segment;
        ImGui::SameLine();
        ImGui::TextDisabled(">");
        ImGui::SameLine();
        if (AssetLibrary::NormalizeSubpath(acc) == AssetLibrary::NormalizeSubpath(m_assetSubpath))
            ImGui::TextUnformatted(segment.c_str());
        else if (ImGui::SmallButton(segment.c_str()))
            SelectAssetBrowserFolder(acc);
    }
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
    drawList->AddRectFilled(previewMin, previewMax, hovered ? IM_COL32(88, 96, 112, 255) : IM_COL32(72, 78, 92, 255), 5.0f);
    const char* icon = ICON_FA_FOLDER;
    if (UI::GetEditorFonts().bold)
        ImGui::PushFont(UI::GetEditorFonts().bold);
    const ImVec2 iconSize = ImGui::CalcTextSize(icon);
    drawList->AddText(ImVec2(previewMin.x + (tileSize - iconSize.x) * 0.5f, previewMin.y + (tileSize - iconSize.y) * 0.5f),
        IM_COL32(245, 248, 255, 235),
        icon);
    if (UI::GetEditorFonts().bold)
        ImGui::PopFont();
    drawList->AddRect(previewMin, previewMax,
        selected ? IM_COL32(255, 210, 92, 255) : IM_COL32(55, 60, 70, 255),
        5.0f,
        0,
        selected ? 3.0f : 1.0f);
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
    ImGui::TextUnformatted(FolderDisplayName(subpath).c_str());
    ImGui::PopID();
}

void EditorImGui::RenderAssetBrowserContent()
{
    RenderAssetBrowserBreadcrumb();
    ImGui::Separator();
    const std::vector<std::string> folders = QueryFilesystemChildFolders(m_assetSubpath);
    const std::vector<AssetLibrary::Entry> assets = QueryFilesystemAssetsInFolder(m_assetSubpath);
    ImGui::Text("%zu assets, %zu folders | Assets%s%s",
        assets.size(),
        folders.size(),
        m_assetSubpath.empty() ? "" : "/",
        m_assetSubpath.c_str());
    ImGui::Separator();

    const float tileSize = 76.0f;
    const float cellWidth = 128.0f;
    const float panelWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const int columns = std::max(1, static_cast<int>(panelWidth / (cellWidth + ImGui::GetStyle().ItemSpacing.x)));
    if (ImGui::BeginTable("AssetBrowserUnityGrid", columns, ImGuiTableFlags_SizingFixedSame | ImGuiTableFlags_NoSavedSettings))
    {
        for (int i = 0; i < columns; ++i)
            ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthFixed, cellWidth);
        for (const std::string& folder : folders)
        {
            ImGui::TableNextColumn();
            ImGui::BeginGroup();
            RenderAssetBrowserFolderTile(folder, tileSize);
            ImGui::EndGroup();
        }
        for (const AssetLibrary::Entry& entry : assets)
        {
            ImGui::TableNextColumn();
            ImGui::BeginGroup();
            RenderAssetTile(entry, tileSize);
            ImGui::EndGroup();
        }
        ImGui::EndTable();
    }
    if (folders.empty() && assets.empty())
        ImGui::TextDisabled("Empty folder.");

    // The empty area under the tiles takes drops too (into the folder shown) and opens the create menu.
    const ImVec2 remaining = ImGui::GetContentRegionAvail();
    if (remaining.x > 1.0f && remaining.y > 1.0f)
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
        ImGui::TextDisabled("Supported: png jpg jpeg tga bmp dds ktx glb gltf fbx obj material anim ozz wav ogg mp3 flac");

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
    if (!m_editorModeActive)
        return;

    if (ImGui::Begin("Asset Browser"))
    {
        if (!m_assetLibrary)
        {
            ImGui::TextDisabled("Asset library is not initialized.");
            ImGui::End();
            return;
        }

        RenderAssetBrowserToolbar();
        ImGui::Separator();

        if (!m_assetStatus.empty())
        {
            ImGui::TextDisabled("%s", m_assetStatus.c_str());
            ImGui::Separator();
        }

        const float browserPanelHeight = std::max(140.0f, ImGui::GetContentRegionAvail().y);
        if (ImGui::BeginTable("AssetBrowserLayout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV))
        {
            ImGui::TableSetupColumn("Folder Tree", ImGuiTableColumnFlags_WidthFixed, 230.0f);
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
    if (!m_editorModeActive)
        return;
    DockBesideIfUnplaced("Scripts", "Asset Browser");
    if (!ImGui::Begin("Scripts"))
    {
        ImGui::End();
        return;
    }

    const bool hasProject = ProjectManager::Instance().HasProject();

    // Build the project's native C++ game scripts (same action as the toolbar Build button).
    const bool canBuild = hasProject && m_playModeState.mode == EditorPlayMode::Edit && !IsBuildRunning();
    if (!canBuild)
        ImGui::BeginDisabled();
    if (ImGui::Button(IsBuildRunning() ? ICON_FA_HAMMER " Building..." : ICON_FA_HAMMER " Build Game Scripts"))
        m_commands.buildGameScripts = true;
    if (!canBuild)
        ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("compiles Assets/scripts -> Binaries + reloads");
    ImGui::Checkbox("Auto-build on save", &m_autoBuildOnSave);
    ImGui::SameLine();
    ImGui::TextDisabled("(.cpp save -> auto Build in Edit; .lua hot-reloads live in Play)");
    ImGui::Separator();

    auto openExternal = [this](const std::filesystem::path& p) {
        std::string err;
        if (platform::OpenInDefaultApp(p, &err))
            m_assetStatus = "Opened: " + p.filename().string();
        else
            m_assetStatus = "Open failed: " + err;
    };

    // --- Native C++ source (<AssetRoot>/scripts/*.cpp,*.h; <ProjectRoot>/Scripts only holds the build) ---
    ImGui::SeparatorText("C++ source (Assets/scripts/)");
    if (hasProject && ImGui::Button(ICON_FA_PLUS " New C++ Script"))
    {
        CopyToBuffer(m_newCppScriptName, sizeof(m_newCppScriptName), std::string("MyScript"));
        m_openNewCppScriptPopup = true;
    }
    if (m_openNewCppScriptPopup)
    {
        ImGui::OpenPopup("NewCppScript");
        m_openNewCppScriptPopup = false;
    }
    if (ImGui::BeginPopupModal("NewCppScript", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Class name (the file is named after it):");
        ImGui::InputText("##cppname", m_newCppScriptName, sizeof(m_newCppScriptName));
        if (ImGui::Button("Create") && m_newCppScriptName[0] != '\0')
        {
            CreateNativeScriptFile(m_newCppScriptName);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (!hasProject)
    {
        ImGui::TextDisabled("No project open.");
    }
    else
    {
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
        const bool anySource = !m_assetBrowserCache.nativeScriptSources->empty();
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
        if (!anySource)
            ImGui::TextDisabled("No C++ scripts yet — use \"New C++ Script\" or click Build to scaffold Game.cpp.");
    }

    // --- Lua script assets ---
    ImGui::SeparatorText("Lua scripts (.lua assets)");
    if (m_assetLibrary && ImGui::Button(ICON_FA_PLUS " New Lua Script"))
        CreateLuaScriptAsset();
    if (m_assetLibrary)
    {
        // Script assets are both languages; the .cpp ones are listed above under C++ source.
        std::vector<AssetLibrary::Entry> luaScripts =
            m_assetLibrary->EntriesFor(AssetLibrary::Category::Script);
        luaScripts.erase(std::remove_if(luaScripts.begin(), luaScripts.end(),
            [](const AssetLibrary::Entry& entry) {
                std::string ext = std::filesystem::path(entry.filename).extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return ext != ".lua";
            }),
            luaScripts.end());
        if (luaScripts.empty())
            ImGui::TextDisabled("No .lua scripts. Drop a .lua into the asset browser.");
        for (const AssetLibrary::Entry& e : luaScripts)
        {
            ImGui::PushID(e.id.c_str());
            if (ImGui::Selectable((ICON_FA_FILE " " + e.displayName).c_str(), false,
                    ImGuiSelectableFlags_AllowDoubleClick) &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                openExternal(m_assetLibrary->AbsolutePath(e));
            }
            // Drag a .lua onto an entity row in the hierarchy to attach it (Unity-style).
            if (ImGui::BeginDragDropSource())
            {
                ImGui::SetDragDropPayload(kAssetPayloadType, e.id.data(), e.id.size());
                ImGui::Text("%s", e.displayName.c_str());
                ImGui::TextDisabled("Lua script");
                ImGui::EndDragDropSource();
            }
            ImGui::PopID();
        }
    }

    // --- Registered native C++ classes (drag onto an entity to attach a Native Script component) ---
    ImGui::SeparatorText("Registered C++ classes");
    {
        const std::vector<std::string> classes = ixscript::NativeBackend::RegisteredNames();
        if (classes.empty())
            ImGui::TextDisabled("No native classes — Build your C++ scripts first.");
        for (const std::string& cls : classes)
        {
            ImGui::PushID(cls.c_str());
            ImGui::Selectable((ICON_FA_FILE " " + cls).c_str());
            if (ImGui::BeginDragDropSource())
            {
                ImGui::SetDragDropPayload(kNativeClassPayloadType, cls.data(), cls.size());
                ImGui::Text("%s", cls.c_str());
                ImGui::TextDisabled("C++ class");
                ImGui::EndDragDropSource();
            }
            ImGui::PopID();
        }
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Drag a script onto an entity in the Hierarchy to attach it; double-click to open.");
    ImGui::End();
}

