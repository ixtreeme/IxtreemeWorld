// This file is included from EditorImGui.cpp inside the editor-enabled implementation block.
// Keep shared anonymous-namespace helpers in EditorImGui.cpp until this panel group is fully decoupled.

void EditorImGui::RenderHierarchyToolbar()
{
    ImGui::PushItemWidth(-1.0f);
    const bool changed = ImGui::InputTextWithHint("##hierarchy_search",
        ICON_FA_MAGNIFYING_GLASS " Search entities/scenes...",
        m_hierarchySearchBuffer,
        sizeof(m_hierarchySearchBuffer));
    ImGui::PopItemWidth();
    if (changed)
        Tracenf("[HIERARCHY] Search filter: '%s'", m_hierarchySearchBuffer);

    if (m_hierarchySearchBuffer[0] != '\0')
    {
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_XMARK))
        {
            m_hierarchySearchBuffer[0] = '\0';
            Tracen("[HIERARCHY] Search filter cleared");
        }
    }
}

std::vector<EditorImGui::ProjectSceneEntry> EditorImGui::QueryProjectScenes() const
{
    std::vector<ProjectSceneEntry> scenes;
    const SceneManager& sceneManager = SceneManager::Instance();
    const std::string activePathKey = ComparablePath(sceneManager.GetCurrentScenePath());

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
            const bool active = !activePathKey.empty() && ComparablePath(scenePath) == activePathKey;
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
    std::vector<AssetLibrary::Entry> scenes;
    ProjectManager& projects = ProjectManager::Instance();
    if (!projects.HasProject())
        return scenes;

    const std::filesystem::path scenesRoot = projects.ScenesPath();
    std::error_code ec;
    if (!std::filesystem::exists(scenesRoot, ec))
        return scenes;

    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             scenesRoot,
             std::filesystem::directory_options::skip_permission_denied,
             ec))
    {
        if (ec)
            break;
        std::error_code entryEc;
        if (!entry.is_regular_file(entryEc) || entry.path().extension() != ".scene")
            continue;

        std::filesystem::path absolutePath = std::filesystem::absolute(entry.path(), entryEc);
        if (entryEc)
            absolutePath = entry.path();
        const std::filesystem::path relativePath = std::filesystem::relative(absolutePath, projects.ProjectRoot(), entryEc);
        const std::string relative = entryEc ? entry.path().generic_string() : relativePath.generic_string();

        AssetLibrary::Entry scene;
        scene.category = AssetLibrary::Category::Scene;
        scene.displayName = absolutePath.stem().string();
        scene.filename = absolutePath.filename().string();
        scene.originalPath = absolutePath.string();
        const std::filesystem::path sceneRelativePath = std::filesystem::relative(absolutePath, scenesRoot, entryEc);
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

    std::sort(scenes.begin(), scenes.end(), [](const AssetLibrary::Entry& a, const AssetLibrary::Entry& b) {
        return ToLowerAscii(a.originalPath) < ToLowerAscii(b.originalPath);
    });
    return scenes;
}

bool EditorImGui::AttachSceneToHierarchy(const AssetLibrary::Entry& entry)
{
    if (entry.category != AssetLibrary::Category::Scene || entry.originalPath.empty())
        return false;

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

    const std::string label = std::string(ICON_FA_GLOBE) + " " + scene.name + "##" + scene.relativePath;
    const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
    if (ImGui::IsItemHovered() && !scene.relativePath.empty())
        ImGui::SetTooltip("%s", scene.relativePath.c_str());
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen() && !scene.active && !scene.path.empty())
    {
        if (SceneManager::Instance().LoadScene(scene.path.string()))
            m_projectStatus = "Scene loaded: " + scene.relativePath;
    }
    if (scene.active && ImGui::BeginPopupContextItem("##scene_context"))
    {
        if (ImGui::MenuItem(ICON_FA_MOUNTAIN " Create Terrain"))
        {
            if (m_terrainState.exists)
                m_replaceTerrainConfirmOpen = true;
            else
                m_createTerrainModalOpen = true;
        }
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
        }
        ImGui::EndDragDropTarget();
    }

    if (!scene.active)
        ImGui::PopStyleColor();

    ImGui::TableSetColumnIndex(1);
    const ImVec4 eyeColor = scene.active ? ImVec4(0.88f, 0.88f, 0.88f, 1.0f) : ImVec4(0.48f, 0.48f, 0.48f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, eyeColor);
    if (ImGui::SmallButton(scene.active ? ICON_FA_EYE : ICON_FA_EYE_SLASH))
    {
        if (!scene.active && !scene.path.empty())
        {
            if (SceneManager::Instance().LoadScene(scene.path.string()))
            {
                m_projectStatus = "Scene enabled: " + scene.relativePath;
                Tracenf("[HIERARCHY] Scene enabled: %s", scene.relativePath.c_str());
            }
        }
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.36f, 0.36f, 1.0f));
    if (ImGui::SmallButton(ICON_FA_TRASH))
        DetachSceneFromHierarchy(scene);
    ImGui::PopStyleColor();

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

    if (ImGui::MenuItem(ICON_FA_BULLSEYE " Focus Camera", "F"))
        QueueHierarchyFocus(entity);
    if (ImGui::MenuItem(ICON_FA_COPY " Duplicate"))
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
    if (ImGui::MenuItem(ICON_FA_PEN " Rename", "F2"))
        StartHierarchyRename(entity);

    if (entity.type == HierarchyEntityType::MeshEntity)
    {
        ImGui::Separator();
        if (ImGui::MenuItem("Export Selection to FBX..."))
            OpenFbxExportDialogForMeshEntity(entity);
    }

    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.30f, 0.30f, 1.0f));
    if (ImGui::MenuItem(ICON_FA_TRASH " Delete", "Del"))
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

    const char* icon = ICON_FA_CUBE;
    if (entity->type == HierarchyEntityType::Terrain)
        icon = ICON_FA_MOUNTAIN;
    else if (entity->type == HierarchyEntityType::WaterBody)
        icon = ICON_FA_DROPLET;
    else if (entity->type == HierarchyEntityType::PointLight)
        icon = ICON_FA_LIGHTBULB;
    else if (entity->type == HierarchyEntityType::SpotLight)
        icon = ICON_FA_BULLSEYE;
    else if (entity->type == HierarchyEntityType::MeshEntity)
        icon = ICON_FA_CUBE;

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
        const std::string label = std::string(icon) + " " + entity->name + "##" + std::to_string(entity->entity);
        open = ImGui::TreeNodeEx(label.c_str(), flags);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        {
            QueueHierarchySelection(*entity);
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                QueueHierarchyFocus(*entity);
        }
        if (selected)
            ImGui::SetScrollHereY(0.5f);
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
    if (ImGui::Begin(ICON_FA_LIST_TREE " Hierarchy"))
    {
        RenderHierarchyToolbar();
        ImGui::Separator();

        const std::vector<ProjectSceneEntry> projectScenes = QueryProjectScenes();
        if (projectScenes.empty())
        {
            ImGui::TextDisabled("No scene open");
            ImGui::TextWrapped(ProjectManager::Instance().HasProject()
                    ? "Create a scene from File to add it to this project."
                    : "Open a project or create a scene from File.");
        }
        else if (ImGui::BeginTable("HierarchyEntityTree", 2,
            ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Visibility", ImGuiTableColumnFlags_WidthFixed, 84.0f);
            ImGui::TableHeadersRow();

            if (ProjectManager::Instance().HasProject())
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                const ProjectData& project = ProjectManager::Instance().CurrentProject();
                const std::string rootLabel = std::string(ICON_FA_FOLDER_OPEN) + " " + project.name;
                const bool projectOpen = ImGui::TreeNodeEx(rootLabel.c_str(),
                    ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth);
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
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("-");
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
            ImGui::EndTable();

            if (!m_logHierarchyRendered)
            {
                m_logHierarchyRendered = true;
                Tracenf("[HIERARCHY] Project scene tree rendered, root=%llu scenes=%zu entities=%zu",
                    static_cast<unsigned long long>(m_sceneRootEntity),
                    projectScenes.size(),
                    m_hierarchyEntities.size());
            }
        }
        if (CanUseEditorTools() && ImGui::BeginPopupContextWindow("HierarchyCreateContext",
                ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
        {
            if (ImGui::BeginMenu("Create"))
            {
                if (ImGui::MenuItem(ICON_FA_CUBE " Cube"))
                {
                    m_commands.addPrimitiveEntity = true;
                    m_commands.primitiveType = "cube";
                    Tracen("[PRIMITIVE] Hierarchy create queued type=cube");
                }
                if (ImGui::MenuItem(ICON_FA_CUBE " Sphere"))
                {
                    m_commands.addPrimitiveEntity = true;
                    m_commands.primitiveType = "sphere";
                    Tracen("[PRIMITIVE] Hierarchy create queued type=sphere");
                }
                if (ImGui::MenuItem(ICON_FA_CUBE " Capsule"))
                {
                    m_commands.addPrimitiveEntity = true;
                    m_commands.primitiveType = "capsule";
                    Tracen("[PRIMITIVE] Hierarchy create queued type=capsule");
                }
                ImGui::EndMenu();
            }
            ImGui::EndPopup();
        }
    }
    ImGui::End();
}
void EditorImGui::RenderToolsPanel()
{
    if (ImGui::Begin("Tools"))
    {
        UI::SectionHeader(ICON_FA_WRENCH " Tools");
        const bool toolsEnabled = CanUseEditorTools();
        if (!toolsEnabled)
        {
            ImGui::TextColored(ImVec4(0.95f, 0.74f, 0.30f, 1.0f), "Tools disabled in Play Mode");
            ImGui::BeginDisabled();
        }
        if (UI::IconButton(ICON_FA_DROPLET, "Water", ImVec2(-1.0f, 0.0f)))
        {
            m_commands.addWaterBody = true;
            Tracen("[EDITOR-3D-SPAWN] Add water requested");
        }

        ImGui::Separator();
        UI::SectionHeader(ICON_FA_HAMMER " Editing");
        if (UI::IconButton(ICON_FA_WATER, "Water Sculpt", ImVec2(-1.0f, 0.0f)))
            m_waterSculptToolOpen = !m_waterSculptToolOpen;
        if (UI::IconButton(ICON_FA_MOUNTAIN, "Heightmap", ImVec2(-1.0f, 0.0f)))
            m_heightmapToolOpen = !m_heightmapToolOpen;
        if (UI::IconButton(ICON_FA_PAINTBRUSH, "Splat Paint", ImVec2(-1.0f, 0.0f)))
            m_splatPaintToolOpen = !m_splatPaintToolOpen;

        ImGui::Separator();
        if (UI::IconButton(ICON_FA_UNDO, "Undo", ImVec2(-1.0f, 0.0f)))
            m_commands.undo = true;
        if (UI::IconButton(ICON_FA_FLOPPY_DISK, "Save", ImVec2(-1.0f, 0.0f)))
            m_commands.save = true;
        if (UI::IconButton(ICON_FA_ROTATE, "Reload", ImVec2(-1.0f, 0.0f)))
            m_commands.reload = true;

        if (!toolsEnabled)
            ImGui::EndDisabled();
    }
    ImGui::End();

    if (!m_logToolsRendered)
    {
        m_logToolsRendered = true;
        Tracen("[EDITOR-IMGUI-2] Tools panel rendered");
    }
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

void EditorImGui::MarkSelectedMeshRendererChanged()
{
    if (!m_meshRendererState.selected)
        return;
    SceneManager::Instance().MarkDirty();
    m_commands.selectedMeshEntityChanged = true;
    m_commands.selectedMeshEntity = m_meshRendererState;
}

