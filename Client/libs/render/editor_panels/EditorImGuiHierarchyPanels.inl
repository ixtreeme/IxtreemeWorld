// This file is included from EditorImGui.cpp inside the editor-enabled implementation block.
// Keep shared anonymous-namespace helpers in EditorImGui.cpp until this panel group is fully decoupled.

void EditorImGui::RenderHierarchyCreateMenuItems()
{
    const bool canCreate = CanUseEditorTools() && SceneManager::Instance().HasOpenScene();
    if (!SceneManager::Instance().HasOpenScene())
        ImGui::TextDisabled("Open or create a scene first (File menu)");
    else if (!CanUseEditorTools())
        ImGui::TextDisabled("Stop Play to add objects");

    const auto primitive = [&](const char* label, const char* type) {
        if (ImGui::MenuItem(label, nullptr, false, canCreate))
        {
            m_commands.addPrimitiveEntity = true;
            m_commands.primitiveType = type;
            Tracenf("[PRIMITIVE] Create menu queued type=%s", type);
        }
    };
    primitive(ICON_FA_CUBE "  Cube", "cube");
    primitive(ICON_FA_CUBE "  Sphere", "sphere");
    primitive(ICON_FA_CUBE "  Capsule", "capsule");
    ImGui::Separator();
    if (ImGui::MenuItem(ICON_FA_LIGHTBULB "  Point Light", nullptr, false,
            canCreate && m_lightingState.numPointLights < kMaxDynamicPointLights))
        m_commands.addPointLight = true;
    UI::ItemTooltip("Light shining in every direction from a point (a lamp, a torch)");
    if (ImGui::MenuItem(ICON_FA_BULLSEYE "  Spot Light", nullptr, false,
            canCreate && m_lightingState.numSpotLights < kMaxDynamicSpotLights))
        m_commands.addSpotLight = true;
    UI::ItemTooltip("Light shining in a cone (a flashlight, a stage light)");
    ImGui::Separator();
    if (ImGui::MenuItem(ICON_FA_DROPLET "  Water Body", nullptr, false, canCreate))
    {
        m_commands.addWaterBody = true;
        Tracen("[EDITOR-3D-SPAWN] Add water requested");
    }
    if (ImGui::MenuItem(m_terrainState.exists ? ICON_FA_MOUNTAIN "  Replace Terrain..." : ICON_FA_MOUNTAIN "  Terrain...",
            nullptr, false, canCreate))
        OpenCreateTerrainDialog();
    ImGui::Separator();
    ImGui::TextDisabled("Models and prefabs: drag them from the Asset Browser");
}

void EditorImGui::RenderHierarchyToolbar()
{
    // "+": everything that can be added to the scene (the same list as the Create menu).
    if (UI::IconOnlyButton(ICON_FA_PLUS))
        ImGui::OpenPopup("HierarchyCreatePopup");
    UI::ItemTooltip("Add an object to the scene");
    if (ImGui::BeginPopup("HierarchyCreatePopup"))
    {
        RenderHierarchyCreateMenuItems();
        ImGui::EndPopup();
    }
    ImGui::SameLine();

    const bool searching = m_hierarchySearchBuffer[0] != '\0';
    const float clearWidth = searching ? ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x : 0.0f;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - clearWidth);
    const bool changed = ImGui::InputTextWithHint("##hierarchy_search",
        ICON_FA_MAGNIFYING_GLASS "  Search",
        m_hierarchySearchBuffer,
        sizeof(m_hierarchySearchBuffer));
    if (changed)
        Tracenf("[HIERARCHY] Search filter: '%s'", m_hierarchySearchBuffer);

    if (searching)
    {
        ImGui::SameLine();
        if (UI::IconOnlyButton(ICON_FA_XMARK))
        {
            m_hierarchySearchBuffer[0] = '\0';
            Tracen("[HIERARCHY] Search filter cleared");
        }
        UI::ItemTooltip("Clear the search");
    }
}

std::vector<EditorImGui::ProjectSceneEntry> EditorImGui::QueryProjectScenes() const
{
    std::vector<ProjectSceneEntry> scenes;
    const SceneManager& sceneManager = SceneManager::Instance();
    const std::string activePathKey = CachedComparablePath(sceneManager.GetCurrentScenePath());

    auto appendScene = [&](const std::filesystem::path& path, const std::string& relativePath, bool active) {
        ProjectSceneEntry entry;
        entry.path = path;
        entry.relativePath = relativePath;
        entry.name = path.empty() ? m_sceneRootName : path.stem().string();
        if (entry.name.empty())
            entry.name = "Untitled";
        entry.active = active;
        scenes.push_back(std::move(entry));
    };

    ProjectManager& projects = ProjectManager::Instance();
    if (projects.HasProject())
    {
        for (const std::string& attachedPath : m_attachedScenePaths)
        {
            if (attachedPath.empty())
                continue;
            std::filesystem::path scenePath(attachedPath);
            if (!scenePath.is_absolute())
                scenePath = projects.ProjectRoot() / scenePath;
            const bool active = !activePathKey.empty() && CachedComparablePath(scenePath) == activePathKey;
            appendScene(scenePath, attachedPath, active);
        }

        if (sceneManager.HasOpenScene())
        {
            const bool activeListed = std::any_of(scenes.begin(), scenes.end(), [](const ProjectSceneEntry& scene) {
                return scene.active;
            });
            if (!activeListed)
            {
                const std::filesystem::path activePath(sceneManager.GetCurrentScenePath());
                appendScene(activePath, activePath.empty() ? std::string{} : activePath.generic_string(), true);
            }
        }
    }
    else if (sceneManager.HasOpenScene())
    {
        const std::filesystem::path activePath(sceneManager.GetCurrentScenePath());
        appendScene(activePath, activePath.empty() ? std::string{} : activePath.generic_string(), true);
    }

    std::sort(scenes.begin(), scenes.end(), [](const ProjectSceneEntry& a, const ProjectSceneEntry& b) {
        return ToLowerAscii(a.relativePath.empty() ? a.name : a.relativePath) <
            ToLowerAscii(b.relativePath.empty() ? b.name : b.relativePath);
    });
    return scenes;
}

std::vector<AssetLibrary::Entry> EditorImGui::QuerySceneAssets() const
{
    ValidateAssetBrowserCache();  // a recursive scan of the scenes folder: cached like the browser listings
    if (m_assetBrowserCache.sceneAssets)
        return *m_assetBrowserCache.sceneAssets;
    std::vector<AssetLibrary::Entry>& scenes = m_assetBrowserCache.sceneAssets.emplace();
    ProjectManager& projects = ProjectManager::Instance();
    if (!projects.HasProject())
        return scenes;

    // Scenes live in the project's scenes folder and anywhere in the asset folder.
    std::unordered_set<std::string> seen;
    const auto collectScenes = [&](const std::filesystem::path& scanRoot) {
        std::error_code ec;
        if (!std::filesystem::exists(scanRoot, ec))
            return;
        std::filesystem::recursive_directory_iterator it(
            scanRoot, std::filesystem::directory_options::skip_permission_denied, ec);
        for (const std::filesystem::recursive_directory_iterator end; !ec && it != end; it.increment(ec))
        {
            try
            {
                std::error_code entryEc;
                if (it->is_directory(entryEc))
                {
                    const std::string name = ToLowerAscii(it->path().filename().string());
                    if (name.empty() || name.front() == '.' || name == "build" ||
                        (it.depth() == 0 && AssetLibrary::IsInternalFolder(name)))
                        it.disable_recursion_pending();
                    continue;
                }
                if (!it->is_regular_file(entryEc) || it->path().extension() != ".scene")
                    continue;

                std::filesystem::path absolutePath = std::filesystem::absolute(it->path(), entryEc);
                if (entryEc)
                    absolutePath = it->path();
                if (!seen.insert(ComparablePath(absolutePath)).second)
                    continue;
                const std::filesystem::path relativePath = std::filesystem::relative(absolutePath, projects.ProjectRoot(), entryEc);
                const std::string relative = entryEc ? it->path().generic_string() : relativePath.generic_string();

                AssetLibrary::Entry scene;
                scene.category = AssetLibrary::Category::Scene;
                scene.displayName = absolutePath.stem().string();
                scene.filename = absolutePath.filename().string();
                scene.originalPath = absolutePath.string();
                const std::filesystem::path sceneRelativePath = std::filesystem::relative(absolutePath, scanRoot, entryEc);
                scene.subpath = AssetLibrary::NormalizeSubpath(
                    (entryEc ? std::filesystem::path(relative) : sceneRelativePath).parent_path().generic_string());
                scene.tags = {"scene"};
                scene.id = "scene_";
                for (char ch : relative)
                {
                    const unsigned char uch = static_cast<unsigned char>(ch);
                    scene.id += std::isalnum(uch) ? static_cast<char>(std::tolower(uch)) : '_';
                }
                scenes.push_back(std::move(scene));
            }
            catch (const std::exception&)
            {
                // a name the narrow path API cannot represent: not listed
            }
        }
    };
    collectScenes(projects.ScenesPath());
    collectScenes(projects.AssetRootPath());

    std::sort(scenes.begin(), scenes.end(), [](const AssetLibrary::Entry& a, const AssetLibrary::Entry& b) {
        return ToLowerAscii(a.originalPath) < ToLowerAscii(b.originalPath);
    });
    return scenes;
}

bool EditorImGui::AttachSceneToHierarchy(const AssetLibrary::Entry& entry)
{
    if (entry.category != AssetLibrary::Category::Scene || entry.originalPath.empty())
        return false;
    if (!CanUseEditorTools())
    {
        m_projectStatus = "Stop Play before switching scenes";
        return false;
    }

    ProjectManager& projects = ProjectManager::Instance();
    std::filesystem::path scenePath(entry.originalPath);
    std::string relativePath = entry.originalPath;
    if (projects.HasProject())
    {
        std::error_code ec;
        const std::filesystem::path relative = std::filesystem::relative(scenePath, projects.ProjectRoot(), ec);
        if (!ec)
            relativePath = relative.generic_string();
    }

    auto samePath = [&](const std::string& existing) {
        std::filesystem::path existingPath(existing);
        if (projects.HasProject() && !existingPath.is_absolute())
            existingPath = projects.ProjectRoot() / existingPath;
        return ComparablePath(existingPath) == ComparablePath(scenePath);
    };
    if (std::none_of(m_attachedScenePaths.begin(), m_attachedScenePaths.end(), samePath))
        m_attachedScenePaths.push_back(relativePath);

    if (SceneManager::Instance().LoadScene(scenePath.string()))
    {
        m_projectStatus = "Scene added to Hierarchy: " + relativePath;
        Tracenf("[HIERARCHY] Scene attached: %s", relativePath.c_str());
        return true;
    }

    m_attachedScenePaths.erase(
        std::remove_if(m_attachedScenePaths.begin(), m_attachedScenePaths.end(), samePath),
        m_attachedScenePaths.end());
    m_projectStatus = "Scene attach failed: " + relativePath;
    return false;
}

bool EditorImGui::DetachSceneFromHierarchy(const ProjectSceneEntry& scene)
{
    if (scene.relativePath.empty())
        return false;

    auto matchesScene = [&](const std::string& existing) {
        std::filesystem::path existingPath(existing);
        if (ProjectManager::Instance().HasProject() && !existingPath.is_absolute())
            existingPath = ProjectManager::Instance().ProjectRoot() / existingPath;
        return ComparablePath(existingPath) == ComparablePath(scene.path);
    };
    m_attachedScenePaths.erase(
        std::remove_if(m_attachedScenePaths.begin(), m_attachedScenePaths.end(), matchesScene),
        m_attachedScenePaths.end());

    if (scene.active)
    {
        if (SceneManager::Instance().IsDirty())
        {
            m_attachedScenePaths.push_back(scene.relativePath);
            m_projectStatus = "Save the scene before removing it from Hierarchy.";
            Tracenf("[HIERARCHY] Scene detach blocked by dirty scene: %s", scene.relativePath.c_str());
            return false;
        }
        SceneManager::Instance().CloseScene();
    }

    m_projectStatus = "Scene removed from Hierarchy: " + scene.name;
    Tracenf("[HIERARCHY] Scene detached: %s", scene.relativePath.c_str());
    return true;
}

void EditorImGui::RenderProjectSceneNode(const ProjectSceneEntry& scene)
{
    const bool activeHasMatchingEntity =
        scene.active && m_hierarchySearchBuffer[0] != '\0' && HierarchySubtreePassesSearch(m_sceneRootEntity);
    if (m_hierarchySearchBuffer[0] != '\0' && !HierarchyPassesSearch(scene.name) && !activeHasMatchingEntity)
        return;

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::PushID(scene.relativePath.empty() ? scene.name.c_str() : scene.relativePath.c_str());

    if (!scene.active)
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.62f, 0.62f, 1.0f));

    bool hasChildren = scene.active && !m_hierarchyEntities.empty();
    ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_OpenOnArrow |
        ImGuiTreeNodeFlags_SpanFullWidth |
        (scene.active ? ImGuiTreeNodeFlags_DefaultOpen : 0);
    if (!hasChildren)
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

    const std::string label = std::string(ICON_FA_GLOBE) + "  " + scene.name + "##" + scene.relativePath;
    const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
    if (ImGui::IsItemHovered() && !scene.relativePath.empty())
        ImGui::SetTooltip("%s", scene.relativePath.c_str());
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen() && !scene.active && !scene.path.empty() && CanUseEditorTools())
    {
        if (SceneManager::Instance().LoadScene(scene.path.string()))
            m_projectStatus = "Scene loaded: " + scene.relativePath;
    }
    if (ImGui::BeginPopupContextItem("##scene_context"))
    {
        if (scene.active)
        {
            RenderHierarchyCreateMenuItems();
            ImGui::Separator();
        }
        if (ImGui::MenuItem(ICON_FA_XMARK "  Remove From Hierarchy", nullptr, false, !scene.relativePath.empty()))
            DetachSceneFromHierarchy(scene);
        ImGui::EndPopup();
    }

    if (scene.active && ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayloadType))
        {
            const std::string assetId(static_cast<const char*>(payload->Data), payload->DataSize);
            const auto entry = m_assetLibrary ? m_assetLibrary->FindById(assetId) : std::optional<AssetLibrary::Entry>{};
            if (entry && entry->category == AssetLibrary::Category::Model)
            {
                m_commands.addMeshEntity = true;
                m_commands.meshAssetId = entry->id;
                m_assetStatus = "Mesh entity queued: " + entry->displayName;
                Tracenf("[MESH-ENTITY] Hierarchy drop queued: asset_id=%s", entry->id.c_str());
            }
            else if (entry && entry->category == AssetLibrary::Category::Prefab)
            {
                m_commands.addPrefabInstance = true;
                m_commands.prefabAssetId = entry->id;
                m_assetStatus = "Prefab instance queued: " + entry->displayName;
                Tracenf("[PREFAB] Hierarchy drop queued: asset_id=%s", entry->id.c_str());
            }
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kHierarchyEntityPayloadType))
        {
            if (payload->DataSize == sizeof(HierarchyEntityDragPayload))
            {
                const auto* dropped = static_cast<const HierarchyEntityDragPayload*>(payload->Data);
                m_commands.hierarchyReparentEntity = true;
                m_commands.hierarchyEntityType = static_cast<HierarchyEntityType>(dropped->type);
                m_commands.hierarchyEntityId = dropped->id;
                m_commands.hierarchyEntityHandle = dropped->entity;
                m_commands.hierarchyParentType = HierarchyEntityType::None;
                m_commands.hierarchyParentId = 0;
                m_projectStatus = "Entity moved to scene root";
                Tracenf("[HIERARCHY] Reparent requested: object=%u type=%d parent=root",
                    dropped->id,
                    dropped->type);
            }
        }
        ImGui::EndDragDropTarget();
    }

    if (!scene.active)
        ImGui::PopStyleColor();

    if (!scene.active)
        UI::ItemTooltip("Click to open this scene");

    if (hasChildren && open)
    {
        for (const HierarchySceneEntity& entity : m_hierarchyEntities)
        {
            if (entity.parent == m_sceneRootEntity || entity.parent == 0)
                RenderHierarchyEntityNode(entity.entity);
        }
        ImGui::TreePop();
    }

    ImGui::PopID();
}

void EditorImGui::RenderHierarchyContextMenu(const HierarchySceneEntity& entity)
{
    ImGui::TextUnformatted(entity.name.c_str());
    ImGui::Separator();

    if (ImGui::MenuItem(ICON_FA_BULLSEYE "  Frame in Scene View", "F"))
        QueueHierarchyFocus(entity);
    if (ImGui::MenuItem(ICON_FA_COPY "  Duplicate"))
    {
        m_commands.hierarchyDuplicateEntity = true;
        m_commands.hierarchyEntityType = entity.type;
        m_commands.hierarchyEntityId = entity.objectId;
        m_commands.hierarchyEntityHandle = entity.entity;
        Tracenf("[HIERARCHY] Duplicate requested: flecs=%llu object=%u type=%d",
            static_cast<unsigned long long>(entity.entity),
            entity.objectId,
            static_cast<int>(entity.type));
    }
    if (ImGui::MenuItem(ICON_FA_PEN "  Rename"))
        StartHierarchyRename(entity);
    if (entity.type == HierarchyEntityType::MeshEntity ||
        entity.type == HierarchyEntityType::PointLight ||
        entity.type == HierarchyEntityType::SpotLight)
    {
        if (ImGui::MenuItem(ICON_FA_LAYER_GROUP "  Create Prefab"))
        {
            QueueHierarchySelection(entity);
            m_commands.createPrefabFromSelection = true;
            Tracenf("[PREFAB] Create requested: flecs=%llu object=%u type=%d",
                static_cast<unsigned long long>(entity.entity),
                entity.objectId,
                static_cast<int>(entity.type));
        }
        if (ImGui::MenuItem(ICON_FA_ROTATE "  Revert Prefab Overrides"))
        {
            QueueHierarchySelection(entity);
            m_commands.revertSelectedPrefabInstance = true;
        }
        if (ImGui::MenuItem(ICON_FA_FLOPPY_DISK "  Apply to Prefab"))
        {
            QueueHierarchySelection(entity);
            m_commands.applySelectedPrefabToAsset = true;
        }
        if (ImGui::MenuItem(ICON_FA_ROTATE "  Refresh All Prefab Instances"))
            m_commands.refreshAllPrefabInstances = true;
        if (ImGui::MenuItem("Unpack Prefab Instance"))
        {
            QueueHierarchySelection(entity);
            m_commands.unpackSelectedPrefabInstance = true;
        }
    }
    if (entity.parent != 0 && entity.parent != m_sceneRootEntity &&
        (entity.type == HierarchyEntityType::MeshEntity ||
            entity.type == HierarchyEntityType::PointLight ||
            entity.type == HierarchyEntityType::SpotLight))
    {
        if (ImGui::MenuItem("Detach From Parent"))
        {
            m_commands.hierarchyReparentEntity = true;
            m_commands.hierarchyEntityType = entity.type;
            m_commands.hierarchyEntityId = entity.objectId;
            m_commands.hierarchyEntityHandle = entity.entity;
            m_commands.hierarchyParentType = HierarchyEntityType::None;
            m_commands.hierarchyParentId = 0;
            Tracenf("[HIERARCHY] Detach requested: flecs=%llu object=%u type=%d",
                static_cast<unsigned long long>(entity.entity),
                entity.objectId,
                static_cast<int>(entity.type));
        }
    }

    if (entity.type == HierarchyEntityType::MeshEntity)
    {
        ImGui::Separator();
        if (ImGui::MenuItem("Export Selection to FBX..."))
            OpenFbxExportDialogForMeshEntity(entity);
    }

    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.30f, 0.30f, 1.0f));
    if (ImGui::MenuItem(ICON_FA_TRASH "  Delete", "Del"))
    {
        m_commands.hierarchyDeleteEntity = true;
        m_commands.hierarchyEntityType = entity.type;
        m_commands.hierarchyEntityId = entity.objectId;
        m_commands.hierarchyEntityHandle = entity.entity;
        Tracenf("[HIERARCHY] Delete requested: flecs=%llu object=%u type=%d",
            static_cast<unsigned long long>(entity.entity),
            entity.objectId,
            static_cast<int>(entity.type));
    }
    ImGui::PopStyleColor();
}

void EditorImGui::RenderHierarchyEntityNode(std::uint64_t entityHandle)
{
    const HierarchySceneEntity* entity = FindHierarchyEntity(entityHandle);
    if (!entity)
        return;
    if (m_hierarchySearchBuffer[0] != '\0' && !HierarchySubtreePassesSearch(entityHandle))
        return;

    std::vector<std::uint64_t> children;
    for (const HierarchySceneEntity& candidate : m_hierarchyEntities)
    {
        if (candidate.parent == entityHandle)
            children.push_back(candidate.entity);
    }

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::PushID(static_cast<int>(entity->entity & 0xffffffffu));

    if (entity->editorHidden)
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.60f, 0.60f, 0.60f, 1.0f));

    const bool selected = entity->selected || m_selectedHierarchyEntity == entity->entity;
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth;
    if (children.empty())
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (selected)
        flags |= ImGuiTreeNodeFlags_Selected;

    const char* icon = entity->prefabRoot ? ICON_FA_LAYER_GROUP : ICON_FA_CUBE;
    if (entity->prefabRoot)
        icon = ICON_FA_LAYER_GROUP;
    else if (entity->type == HierarchyEntityType::Terrain)
        icon = ICON_FA_MOUNTAIN;
    else if (entity->type == HierarchyEntityType::WaterBody)
        icon = ICON_FA_DROPLET;
    else if (entity->type == HierarchyEntityType::PointLight)
        icon = ICON_FA_LIGHTBULB;
    else if (entity->type == HierarchyEntityType::SpotLight)
        icon = ICON_FA_BULLSEYE;
    else if (entity->type == HierarchyEntityType::MeshEntity)
        icon = ICON_FA_CUBE;
    else if (entity->type == HierarchyEntityType::Camera)
        icon = ICON_FA_VIDEO;

    bool open = false;
    if (m_hierarchyRenamingEntity == entity->entity)
    {
        ImGui::Indent(ImGui::GetTreeNodeToLabelSpacing());
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SetKeyboardFocusHere();
        if (ImGui::InputText("##rename",
                m_hierarchyRenameBuffer,
                sizeof(m_hierarchyRenameBuffer),
                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
        {
            m_commands.hierarchyRenameEntity = true;
            m_commands.hierarchyEntityType = entity->type;
            m_commands.hierarchyEntityId = entity->objectId;
            m_commands.hierarchyEntityHandle = entity->entity;
            m_commands.hierarchyRenameValue = m_hierarchyRenameBuffer;
            m_hierarchyRenamingEntity = 0;
            Tracenf("[HIERARCHY] Rename requested: flecs=%llu new_name=%s",
                static_cast<unsigned long long>(entity->entity),
                m_hierarchyRenameBuffer);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            m_hierarchyRenamingEntity = 0;
        ImGui::Unindent(ImGui::GetTreeNodeToLabelSpacing());
    }
    else
    {
        const std::string displayName = entity->prefabRoot ? ("Prefab: " + entity->name) : entity->name;
        const std::string label = std::string(icon) + "  " + displayName + "##" + std::to_string(entity->entity);
        open = ImGui::TreeNodeEx(label.c_str(), flags);
        if (entity->prefabRoot && ImGui::IsItemHovered())
            ImGui::SetTooltip("Prefab instance\nAsset: %s", entity->prefabAssetId.c_str());
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        {
            QueueHierarchySelection(*entity);
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                QueueHierarchyFocus(*entity);
        }
        if (selected)
            ImGui::SetScrollHereY(0.5f);
    }

    if (m_hierarchyRenamingEntity != entity->entity)
    {
        const bool draggable =
            entity->type == HierarchyEntityType::MeshEntity ||
            entity->type == HierarchyEntityType::PointLight ||
            entity->type == HierarchyEntityType::SpotLight;
        if (draggable && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
        {
            const HierarchyEntityDragPayload payload{
                static_cast<int>(entity->type),
                entity->objectId,
                entity->entity};
            ImGui::SetDragDropPayload(kHierarchyEntityPayloadType, &payload, sizeof(payload));
            if (entity->prefabRoot)
                ImGui::Text("%s %s", ICON_FA_LAYER_GROUP, entity->name.c_str());
            else
                ImGui::TextUnformatted(entity->name.c_str());
            ImGui::EndDragDropSource();
        }

        const bool validParentTarget =
            entity->type == HierarchyEntityType::MeshEntity ||
            entity->type == HierarchyEntityType::PointLight ||
            entity->type == HierarchyEntityType::SpotLight;
        if (validParentTarget && ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kHierarchyEntityPayloadType))
            {
                if (payload->DataSize == sizeof(HierarchyEntityDragPayload))
                {
                    const auto* dropped = static_cast<const HierarchyEntityDragPayload*>(payload->Data);
                    if (!(dropped->id == entity->objectId && dropped->type == static_cast<int>(entity->type)))
                    {
                        m_commands.hierarchyReparentEntity = true;
                        m_commands.hierarchyEntityType = static_cast<HierarchyEntityType>(dropped->type);
                        m_commands.hierarchyEntityId = dropped->id;
                        m_commands.hierarchyEntityHandle = dropped->entity;
                        m_commands.hierarchyParentType = entity->type;
                        m_commands.hierarchyParentId = entity->objectId;
                        m_projectStatus = "Entity parent changed: " + entity->name;
                        Tracenf("[HIERARCHY] Reparent requested: object=%u type=%d parent=%u parent_type=%d",
                            dropped->id,
                            dropped->type,
                            entity->objectId,
                            static_cast<int>(entity->type));
                    }
                }
            }
            // Drag a Script asset onto a mesh row -> attach a Script component. The asset's file type
            // selects the backend: .as -> AngelScript, .lua -> Lua (both by asset id, hot-reloaded in
            // Play). Legacy .cpp assets are not attachable (C++ is no longer a project language).
            else if (entity->type == HierarchyEntityType::MeshEntity && m_assetLibrary)
            {
                if (const ImGuiPayload* assetPayload = ImGui::AcceptDragDropPayload(kAssetPayloadType))
                {
                    const std::string assetId(static_cast<const char*>(assetPayload->Data),
                        static_cast<size_t>(assetPayload->DataSize));
                    if (auto e = m_assetLibrary->FindById(assetId);
                        e && e->category == AssetLibrary::Category::Script)
                    {
                        const std::string extension = std::filesystem::path(e->filename).extension().string();
                        if (extension == ".as")
                        {
                            m_commands.attachScriptToEntity = true;
                            m_commands.attachScriptEntityId = entity->objectId;
                            m_commands.attachScriptBackend = ixscript::ScriptBackendType::AngelScript;
                            m_commands.attachScriptAssetId = assetId;
                            m_commands.attachScriptClassName.clear();
                            m_projectStatus = "Attached AngelScript to " + entity->name;
                        }
                        else if (extension == ".lua")
                        {
                            m_commands.attachScriptToEntity = true;
                            m_commands.attachScriptEntityId = entity->objectId;
                            m_commands.attachScriptBackend = ixscript::ScriptBackendType::Lua;
                            m_commands.attachScriptAssetId = assetId;
                            m_commands.attachScriptClassName.clear();
                            m_projectStatus = "Attached Lua script to " + entity->name;
                        }
                        else
                        {
                            m_projectStatus = "C++ scripts are not attachable; use AngelScript or Lua";
                        }
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
    }

    if (entity->editorHidden)
        ImGui::PopStyleColor();

    if (ImGui::BeginPopupContextItem("##hierarchy_context"))
    {
        RenderHierarchyContextMenu(*entity);
        ImGui::EndPopup();
    }

    ImGui::TableSetColumnIndex(1);
    const ImVec4 eyeColor = entity->editorHidden ? ImVec4(0.48f, 0.48f, 0.48f, 1.0f) : ImVec4(0.88f, 0.88f, 0.88f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, eyeColor);
    if (ImGui::SmallButton(entity->editorHidden ? ICON_FA_EYE_SLASH : ICON_FA_EYE))
    {
        m_commands.hierarchyToggleHidden = true;
        m_commands.hierarchyEntityType = entity->type;
        m_commands.hierarchyEntityId = entity->objectId;
        m_commands.hierarchyEntityHandle = entity->entity;
        Tracenf("[HIERARCHY] Toggle visibility requested: flecs=%llu object=%u type=%d",
            static_cast<unsigned long long>(entity->entity),
            entity->objectId,
            static_cast<int>(entity->type));
    }
    ImGui::PopStyleColor();

    if (!children.empty() && open)
    {
        for (std::uint64_t child : children)
            RenderHierarchyEntityNode(child);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void EditorImGui::RenderHierarchyPanel()
{
    if (!m_hierarchyPanelOpen)
        return;
    if (ImGui::Begin(EditorWindow::Hierarchy, &m_hierarchyPanelOpen))
    {
        RenderHierarchyToolbar();
        ImGui::Spacing();

        const std::vector<ProjectSceneEntry> projectScenes = QueryProjectScenes();
        if (projectScenes.empty())
        {
            ImGui::Spacing();
            ImGui::TextDisabled("No scene is open");
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled(ProjectManager::Instance().HasProject()
                    ? "Create a scene with File > New Scene, or open one with File > Open Scene."
                    : "Open a project (File > Open Project) or create a new one.");
            ImGui::PopTextWrapPos();
        }
        else if (ImGui::BeginTable("HierarchyEntityTree", 2, ImGuiTableFlags_SizingStretchProp))
        {
            // The selected row in the accent color, so the selection is obvious at a glance.
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.17f, 0.31f, 0.52f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.21f, 0.24f, 0.29f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.21f, 0.37f, 0.60f, 1.0f));
            // Name, and the eye that hides an object in the editor only (it still exists in Play).
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Visible", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight() + 4.0f);

            if (ProjectManager::Instance().HasProject())
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                const ProjectData& project = ProjectManager::Instance().CurrentProject();
                const std::string rootLabel = std::string(ICON_FA_FOLDER_OPEN "  ") + project.name;
                const bool projectOpen = ImGui::TreeNodeEx(rootLabel.c_str(),
                    ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth);
                UI::ItemTooltip("The project. Drop a scene from the Asset Browser here to open it.");
                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayloadType))
                    {
                        const std::string assetId(static_cast<const char*>(payload->Data), payload->DataSize);
                        const std::vector<AssetLibrary::Entry> sceneAssets = QuerySceneAssets();
                        const auto sceneIt = std::find_if(sceneAssets.begin(), sceneAssets.end(), [&assetId](const AssetLibrary::Entry& entry) {
                            return entry.id == assetId;
                        });
                        if (sceneIt != sceneAssets.end())
                            AttachSceneToHierarchy(*sceneIt);
                    }
                    ImGui::EndDragDropTarget();
                }
                if (projectOpen)
                {
                    for (const ProjectSceneEntry& scene : projectScenes)
                        RenderProjectSceneNode(scene);
                    ImGui::TreePop();
                }
            }
            else
            {
                for (const ProjectSceneEntry& scene : projectScenes)
                    RenderProjectSceneNode(scene);
            }
            ImGui::PopStyleColor(3);
            ImGui::EndTable();

            if (m_hierarchyEntities.empty() && m_hierarchySearchBuffer[0] == '\0')
            {
                ImGui::Spacing();
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextDisabled("The scene is empty. Add objects with + above, or drag models from the Asset Browser.");
                ImGui::PopTextWrapPos();
            }

            if (!m_logHierarchyRendered)
            {
                m_logHierarchyRendered = true;
                Tracenf("[HIERARCHY] Project scene tree rendered, root=%llu scenes=%zu entities=%zu",
                    static_cast<unsigned long long>(m_sceneRootEntity),
                    projectScenes.size(),
                    m_hierarchyEntities.size());
            }
        }
        // Right-click on empty space: the same create list as + and the Create menu.
        if (ImGui::BeginPopupContextWindow("HierarchyCreateContext",
                ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
        {
            RenderHierarchyCreateMenuItems();
            ImGui::EndPopup();
        }
    }
    ImGui::End();
}

void EditorImGui::MarkSelectedWaterBodyChanged()
{
    if (!m_waterBodyState.selected)
        return;
    m_waterBodyState.center[1] = m_waterBodyState.config.waterLevelY;
    SceneManager::Instance().MarkDirty();
    m_commands.selectedWaterBodyChanged = true;
    m_commands.selectedWaterBody = m_waterBodyState;
}

void EditorImGui::MarkSelectedLightChanged()
{
    if (m_dynamicLightState.type != DynamicLightType::Point &&
        m_dynamicLightState.type != DynamicLightType::Spot)
    {
        return;
    }
    SceneManager::Instance().MarkDirty();
    m_commands.selectedLightChanged = true;
    m_commands.selectedLight = m_dynamicLightState;
}

void EditorImGui::MarkSelectedCameraChanged()
{
    if (!m_cameraEditorState.selected)
        return;
    SceneManager::Instance().MarkDirty();
    m_commands.selectedCameraChanged = true;
    m_commands.selectedCamera = m_cameraEditorState.camera;
}

void EditorImGui::MarkSelectedMeshRendererChanged()
{
    if (!m_meshRendererState.selected)
        return;
    SceneManager::Instance().MarkDirty();
    m_commands.selectedMeshEntityChanged = true;
    m_commands.selectedMeshEntity = m_meshRendererState;
}

