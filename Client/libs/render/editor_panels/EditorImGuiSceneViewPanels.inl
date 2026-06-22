// This file is included from EditorImGui.cpp inside the editor-enabled implementation block.
// Keep shared anonymous-namespace helpers in EditorImGui.cpp until this panel group is fully decoupled.

void EditorImGui::RenderDemoPanels()
{
    if (!m_editorModeActive)
        return;

    if (m_showDemoWindow)
        ImGui::ShowDemoWindow(&m_showDemoWindow);
}

void EditorImGui::RenderDockSpace()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoInputs;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("Editor DockSpace", nullptr, flags);
    ImGui::PopStyleVar(3);

    const ImGuiID dockspaceId = ImGui::GetID("EditorDockSpace");
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);

    if (m_applyDefaultDockLayout && !m_defaultDockLayoutBuilt)
    {
        m_defaultDockLayoutBuilt = true;
        ImGui::DockBuilderRemoveNode(dockspaceId);
        const ImGuiDockNodeFlags dockBuilderFlags = static_cast<ImGuiDockNodeFlags>(
            static_cast<int>(ImGuiDockNodeFlags_DockSpace) |
            static_cast<int>(ImGuiDockNodeFlags_PassthruCentralNode));
        ImGui::DockBuilderAddNode(dockspaceId, dockBuilderFlags);
        ImGui::DockBuilderSetNodeSize(dockspaceId, viewport->WorkSize);

        ImGuiID mainId = dockspaceId;
        ImGuiID leftId = 0;
        ImGuiID rightId = 0;
        ImGuiID bottomId = 0;
        ImGuiID topId = 0;
        ImGui::DockBuilderSplitNode(mainId, ImGuiDir_Up, 0.06f, &topId, &mainId);
        ImGui::DockBuilderSplitNode(mainId, ImGuiDir_Left, 0.20f, &leftId, &mainId);
        ImGui::DockBuilderSplitNode(mainId, ImGuiDir_Right, 0.25f, &rightId, &mainId);
        ImGui::DockBuilderSplitNode(mainId, ImGuiDir_Down, 0.30f, &bottomId, &mainId);
        ImGui::DockBuilderDockWindow("Editor Toolbar", topId);
        ImGui::DockBuilderDockWindow(ICON_FA_LIST_TREE " Hierarchy", leftId);
        ImGui::DockBuilderDockWindow("Tools", leftId);
        ImGui::DockBuilderDockWindow("Inspector", rightId);
        ImGui::DockBuilderDockWindow("Scene Settings", rightId);
        ImGui::DockBuilderDockWindow(ICON_FA_GLOBE " World", rightId);
        ImGui::DockBuilderDockWindow("Asset Browser", bottomId);
        ImGui::DockBuilderDockWindow("Scene View", mainId);
        ImGui::DockBuilderFinish(dockspaceId);
        Tracen("[EDITOR-LAYOUT] Default Unity-style dock layout applied");
    }
    ImGui::End();
}

void EditorImGui::RenderSceneViewDropTarget()
{
    const ImGuiPayload* activePayload = ImGui::GetDragDropPayload();
    const bool assetDragActive = activePayload && std::strcmp(activePayload->DataType, kAssetPayloadType) == 0;
    m_viewportInputDiagnostics = {};
    m_viewportInputDiagnostics.assetDragActive = assetDragActive;
    if (!assetDragActive)
    {
        m_viewportDropTargetLogged = false;
        m_loggedDragAssetId.clear();
    }

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoCollapse;

    auto acceptModelDrop = [&]() {
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayloadType))
            {
                if (payload->IsDelivery())
                {
                    const std::string assetId(static_cast<const char*>(payload->Data), payload->DataSize);
                    const auto entry = m_assetLibrary ? m_assetLibrary->FindById(assetId) : std::optional<AssetLibrary::Entry>{};
                    if (entry && entry->category == AssetLibrary::Category::Model)
                    {
                        const ImVec2 mouse = ImGui::GetMousePos();
                        const ImGuiViewport* viewport = ImGui::GetMainViewport();
                        const ImVec2 viewportPos = viewport ? viewport->Pos : ImVec2(0.0f, 0.0f);
                        InputEvent dropEvent{};
                        dropEvent.type = InputEvent::MouseUp;
                        dropEvent.x = static_cast<int>(ixtreeme::math::Round(mouse.x - viewportPos.x));
                        dropEvent.y = static_cast<int>(ixtreeme::math::Round(mouse.y - viewportPos.y));
                        const InputEvent mappedDropEvent = MapInputToSceneView(dropEvent);
                        m_commands.addMeshEntity = true;
                        m_commands.meshAssetId = entry->id;
                        m_commands.meshDropScreenPositionValid = true;
                        m_commands.meshDropScreenPosition[0] = static_cast<float>(mappedDropEvent.x);
                        m_commands.meshDropScreenPosition[1] = static_cast<float>(mappedDropEvent.y);
                        m_assetStatus = "Mesh entity dropped: " + entry->displayName;
                        Tracenf("[DND] payload accepted: %s", entry->id.c_str());
                    }
                    else if (entry && entry->category == AssetLibrary::Category::Prefab)
                    {
                        const ImVec2 mouse = ImGui::GetMousePos();
                        const ImGuiViewport* viewport = ImGui::GetMainViewport();
                        const ImVec2 viewportPos = viewport ? viewport->Pos : ImVec2(0.0f, 0.0f);
                        InputEvent dropEvent{};
                        dropEvent.type = InputEvent::MouseUp;
                        dropEvent.x = static_cast<int>(ixtreeme::math::Round(mouse.x - viewportPos.x));
                        dropEvent.y = static_cast<int>(ixtreeme::math::Round(mouse.y - viewportPos.y));
                        const InputEvent mappedDropEvent = MapInputToSceneView(dropEvent);
                        m_commands.addPrefabInstance = true;
                        m_commands.prefabAssetId = entry->id;
                        m_commands.prefabDropScreenPositionValid = true;
                        m_commands.prefabDropScreenPosition[0] = static_cast<float>(mappedDropEvent.x);
                        m_commands.prefabDropScreenPosition[1] = static_cast<float>(mappedDropEvent.y);
                        m_assetStatus = "Prefab dropped: " + entry->displayName;
                        Tracenf("[PREFAB] viewport drop queued: asset_id=%s", entry->id.c_str());
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
    };

    if (!ImGui::Begin("Scene View", nullptr, flags))
    {
        ImGui::End();
        return;
    }

    const ImVec2 sceneMin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    const ImVec2 viewportPos = mainViewport ? mainViewport->Pos : ImVec2(0.0f, 0.0f);
    ImVec2 imageMin = sceneMin;
    ImVec2 imageSize = avail;
    if (avail.x > 1.0f && avail.y > 1.0f &&
        m_sceneViewExtent.width > 0 && m_sceneViewExtent.height > 0)
    {
        const float targetAspect = static_cast<float>(m_sceneViewExtent.width) /
            static_cast<float>(m_sceneViewExtent.height);
        const float availableAspect = avail.x / avail.y;
        if (availableAspect > targetAspect)
        {
            imageSize.x = avail.y * targetAspect;
            imageMin.x += (avail.x - imageSize.x) * 0.5f;
        }
        else
        {
            imageSize.y = avail.x / targetAspect;
            imageMin.y += (avail.y - imageSize.y) * 0.5f;
        }
    }
    if (avail.x > 1.0f && avail.y > 1.0f)
    {
        m_viewportInputDiagnostics.sceneViewRectValid = true;
        m_viewportInputDiagnostics.sceneViewMin[0] = imageMin.x - viewportPos.x;
        m_viewportInputDiagnostics.sceneViewMin[1] = imageMin.y - viewportPos.y;
        m_viewportInputDiagnostics.sceneViewSize[0] = imageSize.x;
        m_viewportInputDiagnostics.sceneViewSize[1] = imageSize.y;
        m_viewportInputDiagnostics.sceneViewExtent[0] = m_sceneViewExtent.width;
        m_viewportInputDiagnostics.sceneViewExtent[1] = m_sceneViewExtent.height;
    }
    const bool canDrawSceneView = m_sceneViewDescriptor && avail.x > 1.0f && avail.y > 1.0f;
    bool sceneViewItemDrawn = false;
    if (canDrawSceneView)
    {
        ImGui::SetCursorScreenPos(imageMin);
        ImGui::Image(reinterpret_cast<ImTextureID>(m_sceneViewDescriptor), imageSize);
        sceneViewItemDrawn = true;
        if (!m_sceneViewSelectionOutline.empty() &&
            m_sceneViewExtent.width > 0 &&
            m_sceneViewExtent.height > 0)
        {
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            auto toScenePoint = [&](float x, float y) {
                const float u = x / static_cast<float>(m_sceneViewExtent.width);
                const float v = y / static_cast<float>(m_sceneViewExtent.height);
                return ImVec2(
                    imageMin.x + u * imageSize.x,
                    imageMin.y + v * imageSize.y);
            };
            constexpr ImU32 shadowColor = IM_COL32(28, 22, 12, 230);
            constexpr ImU32 outlineColor = IM_COL32(255, 156, 18, 255);
            for (const std::array<float, 4>& segment : m_sceneViewSelectionOutline)
            {
                const ImVec2 a = toScenePoint(segment[0], segment[1]);
                const ImVec2 b = toScenePoint(segment[2], segment[3]);
                drawList->AddLine(a, b, shadowColor, 5.0f);
                drawList->AddLine(a, b, outlineColor, 2.5f);
            }
        }
        RenderSceneViewGizmo(imageMin, imageSize);
    }
    else if (avail.x > 1.0f && avail.y > 1.0f)
    {
        ImGui::BeginDisabled();
        ImGui::TextUnformatted("Scene render target unavailable");
        ImGui::EndDisabled();
    }
    m_viewportInputDiagnostics.sceneViewHovered =
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) ||
        (sceneViewItemDrawn && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem));
    m_viewportInputDiagnostics.sceneViewFocused =
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    if (assetDragActive && avail.x > 1.0f && avail.y > 1.0f)
    {
        if (!m_viewportDropTargetLogged)
        {
            m_viewportDropTargetLogged = true;
            Tracen("[DND] viewport drop target active");
        }

        ImGuiID dropItemId = 0;
        if (sceneViewItemDrawn)
        {
            dropItemId = ImGui::GetItemID();
        }
        else
        {
            dropItemId = ImGui::GetID("##SceneViewDropTarget");
            ImGui::SetCursorScreenPos(imageMin);
            ImGui::InvisibleButton("##SceneViewDropTarget", imageSize);
        }
        m_viewportInputDiagnostics.dropTargetVisible = true;
        m_viewportInputDiagnostics.dropTargetHovered = ImGui::IsItemHovered();
        m_viewportInputDiagnostics.dropTargetActive = ImGui::IsItemActive();
        if (m_viewportInputDiagnostics.dropTargetHovered)
            m_viewportInputDiagnostics.hoveredItemId = static_cast<std::uint32_t>(dropItemId);
        m_viewportInputDiagnostics.activeItemId = m_viewportInputDiagnostics.dropTargetActive
            ? static_cast<std::uint32_t>(dropItemId)
            : 0u;
        acceptModelDrop();
    }

    ImGui::End();

    if (!assetDragActive || m_commands.addMeshEntity)
        return;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (!viewport)
        return;

    const ImVec2 workPos = viewport->WorkPos;
    const ImVec2 workSize = viewport->WorkSize;
    const ImVec2 overlayPos(workPos.x + workSize.x * 0.20f, workPos.y + workSize.y * 0.06f);
    const ImVec2 overlaySize(workSize.x * 0.55f, workSize.y * 0.64f);
    if (overlaySize.x <= 1.0f || overlaySize.y <= 1.0f)
        return;

    ImGui::SetNextWindowPos(overlayPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(overlaySize, ImGuiCond_Always);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.0f);
    const ImGuiWindowFlags overlayFlags =
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoNavFocus;
    if (ImGui::Begin("##ViewportDropOverlay", nullptr, overlayFlags))
    {
        const ImGuiID overlayItemId = ImGui::GetID("##ViewportDropOverlayTarget");
        ImGui::InvisibleButton("##ViewportDropOverlayTarget", ImGui::GetContentRegionAvail());
        m_viewportInputDiagnostics.dropTargetVisible = true;
        m_viewportInputDiagnostics.overlayDropTargetHovered = ImGui::IsItemHovered();
        m_viewportInputDiagnostics.overlayDropTargetActive = ImGui::IsItemActive();
        if (m_viewportInputDiagnostics.overlayDropTargetHovered)
            m_viewportInputDiagnostics.hoveredItemId = static_cast<std::uint32_t>(overlayItemId);
        if (m_viewportInputDiagnostics.overlayDropTargetActive)
            m_viewportInputDiagnostics.activeItemId = static_cast<std::uint32_t>(overlayItemId);
        acceptModelDrop();
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void EditorImGui::RenderSceneViewGizmo(const ImVec2& imageMin, const ImVec2& imageSize)
{
    m_sceneGizmoInputActive = false;
    if (!m_sceneGizmoVisible ||
        m_sceneGizmoEntityType == HierarchyEntityType::None ||
        m_sceneGizmoEntityId == 0 ||
        imageSize.x <= 1.0f ||
        imageSize.y <= 1.0f)
    {
        return;
    }

    const float aspect = imageSize.y > 0.0f ? imageSize.x / imageSize.y : 1.0f;
    const WorldMat4 view = WorldLookAt(m_sceneGizmoCamera.eye, m_sceneGizmoCamera.target, {0.0f, 1.0f, 0.0f});
    const WorldMat4 projection = ImGuizmoPerspective(45.0f * kPi / 180.0f,
        aspect,
        m_sceneGizmoCamera.nearPlane,
        m_sceneGizmoCamera.farPlane);

    float translation[3] = {
        m_sceneGizmoPosition[0],
        m_sceneGizmoPosition[1],
        m_sceneGizmoPosition[2],
    };
    float rotationDegrees[3] = {
        Degrees(m_sceneGizmoRotation[0]),
        Degrees(m_sceneGizmoRotation[1]),
        Degrees(m_sceneGizmoRotation[2]),
    };
    float scale[3] = {
        std::max(0.001f, m_sceneGizmoScale[0]),
        std::max(0.001f, m_sceneGizmoScale[1]),
        std::max(0.001f, m_sceneGizmoScale[2]),
    };
    float model[16]{};
    ImGuizmo::RecomposeMatrixFromComponents(translation, rotationDegrees, scale, model);

    float snap[3] = {
        m_sceneGizmoSnapValue,
        m_sceneGizmoSnapValue,
        m_sceneGizmoSnapValue,
    };
    ImGuizmo::BeginFrame();
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(imageMin.x, imageMin.y, imageSize.x, imageSize.y);

    const bool changed = ImGuizmo::Manipulate(view.m,
        projection.m,
        ToImGuizmoOperation(m_sceneGizmoOperation),
        ToImGuizmoMode(m_sceneGizmoOperation),
        model,
        nullptr,
        m_sceneGizmoSnapEnabled ? snap : nullptr);

    m_sceneGizmoInputActive = ImGuizmo::IsUsing() || ImGuizmo::IsOver();
    if (!changed)
        return;

    float outTranslation[3]{};
    float outRotationDegrees[3]{};
    float outScale[3]{};
    ImGuizmo::DecomposeMatrixToComponents(model, outTranslation, outRotationDegrees, outScale);
    for (int i = 0; i < 3; ++i)
        outRotationDegrees[i] = UnwrapDegreesNear(outRotationDegrees[i], Degrees(m_sceneGizmoRotation[i]));

    if (m_sceneGizmoOperation == MapEditorGizmoOperation::Translate &&
        (m_sceneGizmoEntityType == HierarchyEntityType::MeshEntity ||
            m_sceneGizmoEntityType == HierarchyEntityType::WaterBody))
    {
        float translationDelta[3] = {
            outTranslation[0] - m_sceneGizmoPosition[0],
            outTranslation[1] - m_sceneGizmoPosition[1],
            outTranslation[2] - m_sceneGizmoPosition[2],
        };
        int changedAxes = 0;
        for (float delta : translationDelta)
        {
            if (ixtreeme::math::Abs(delta) > 0.0005f)
                ++changedAxes;
        }
        if (changedAxes >= 2)
        {
            int planeAxis = 0;
            float smallestDelta = ixtreeme::math::Abs(translationDelta[0]);
            for (int i = 1; i < 3; ++i)
            {
                const float delta = ixtreeme::math::Abs(translationDelta[i]);
                if (delta < smallestDelta)
                {
                    smallestDelta = delta;
                    planeAxis = i;
                }
            }
            const float horizontalScaleDelta = ImGui::GetIO().MouseDelta.x * kGizmoPlaneScaleUnitsPerPixel;
            if (ixtreeme::math::Abs(horizontalScaleDelta) <= 0.000001f)
                return;
            for (int i = 0; i < 3; ++i)
            {
                outTranslation[i] = m_sceneGizmoPosition[i];
                outRotationDegrees[i] = Degrees(m_sceneGizmoRotation[i]);
                outScale[i] = std::max(0.001f, m_sceneGizmoScale[i]);
            }
            outScale[planeAxis] = std::max(0.001f, m_sceneGizmoScale[planeAxis] + horizontalScaleDelta);
        }
    }

    m_commands.sceneGizmoTransformChanged = true;
    m_commands.sceneGizmoEntityType = m_sceneGizmoEntityType;
    m_commands.sceneGizmoEntityId = m_sceneGizmoEntityId;
    for (int i = 0; i < 3; ++i)
    {
        m_commands.sceneGizmoPosition[i] = outTranslation[i];
        m_commands.sceneGizmoRotation[i] = Radians(outRotationDegrees[i]);
        m_commands.sceneGizmoScale[i] = std::max(0.001f, outScale[i]);
    }

    std::copy(std::begin(m_commands.sceneGizmoPosition), std::end(m_commands.sceneGizmoPosition), m_sceneGizmoPosition);
    std::copy(std::begin(m_commands.sceneGizmoRotation), std::end(m_commands.sceneGizmoRotation), m_sceneGizmoRotation);
    std::copy(std::begin(m_commands.sceneGizmoScale), std::end(m_commands.sceneGizmoScale), m_sceneGizmoScale);
}

void EditorImGui::OpenProjectDialog(ProjectDialogMode mode)
{
    m_projectDialogMode = mode;
    m_projectPopupNeedsOpen = true;
    m_projectCreateBrowserVisible = mode == ProjectDialogMode::Open;
    if (m_projectBrowserPath.empty())
        m_projectBrowserPath = InitialProjectBrowserPath(m_engineRoot);
    CopyToBuffer(m_projectBrowsePathBuffer, sizeof(m_projectBrowsePathBuffer), m_projectBrowserPath.string());
    if (m_projectParentBuffer[0] == '\0')
        CopyToBuffer(m_projectParentBuffer, sizeof(m_projectParentBuffer), m_projectBrowserPath.string());
}

bool EditorImGui::NavigateProjectBrowser(const std::filesystem::path& path, bool createMissing)
{
    std::error_code ec;
    std::filesystem::path target = path.empty() ? InitialProjectBrowserPath(m_engineRoot) : path;
    target = std::filesystem::absolute(target, ec);
    if (ec)
    {
        m_projectStatus = "Browse failed: " + ec.message();
        return false;
    }

    if (std::filesystem::is_regular_file(target, ec))
        target = target.parent_path();
    bool createdFolder = false;
    if (!std::filesystem::exists(target, ec) || !std::filesystem::is_directory(target, ec))
    {
        if (!createMissing)
        {
            m_projectStatus = "Browse failed: folder not found";
            return false;
        }

        ec.clear();
        std::filesystem::create_directories(target, ec);
        if (ec)
        {
            m_projectStatus = "Browse create failed: " + ec.message();
            return false;
        }
        createdFolder = true;
        m_projectStatus = "Folder created: " + target.string();
    }

    m_projectBrowserPath = target;
    CopyToBuffer(m_projectBrowsePathBuffer, sizeof(m_projectBrowsePathBuffer), m_projectBrowserPath.string());
    if (!createdFolder)
        m_projectStatus = "Browse folder: " + target.string();
    return true;
}

void EditorImGui::ActivateCurrentProject()
{
    ProjectManager& projects = ProjectManager::Instance();
    if (!projects.HasProject())
        return;

    AssetDatabase::Instance().scan(projects.ProjectRoot());
    AssetWatcher::Instance().start(projects.ProjectRoot());
    InitializeProjectAssetLibrary(projects.ProjectRoot(), projects.AssetRootPath());
    SceneManager::Instance().CloseScene();
    const bool openedScene = LoadProjectStartupScene();
    const bool createdScene = openedScene ? false : CreateDefaultProjectScene();
    m_projectDialogMode = ProjectDialogMode::None;
    m_projectPopupNeedsOpen = false;
    m_lastAutoSaveSeconds = ImGui::GetTime();
    m_projectStatus = "Project active: " + projects.CurrentProject().name;
    if (openedScene)
        m_projectStatus += " | scene loaded";
    else if (createdScene)
        m_projectStatus += " | default scene created";
    else
        m_projectStatus += " | no scene";

    m_attachedScenePaths.clear();
    const std::string activePath = SceneManager::Instance().GetCurrentScenePath();
    if (!activePath.empty())
    {
        std::error_code ec;
        const std::filesystem::path relative = std::filesystem::relative(activePath, projects.ProjectRoot(), ec);
        m_attachedScenePaths.push_back(ec ? std::filesystem::path(activePath).generic_string() : relative.generic_string());
    }
}

bool EditorImGui::LoadProjectStartupScene()
{
    ProjectManager& projects = ProjectManager::Instance();
    if (!projects.HasProject())
        return false;

    std::vector<std::string> candidates;
    const ProjectData& project = projects.CurrentProject();
    if (!project.startupScene.empty())
        candidates.push_back(project.startupScene);
    for (const std::string& recentScene : project.recentScenes)
    {
        if (!recentScene.empty() &&
            std::find(candidates.begin(), candidates.end(), recentScene) == candidates.end())
        {
            candidates.push_back(recentScene);
        }
    }

    for (const std::string& candidate : candidates)
    {
        std::filesystem::path scenePath(candidate);
        if (!scenePath.is_absolute())
            scenePath = projects.ProjectRoot() / scenePath;
        if (!std::filesystem::exists(scenePath))
        {
            Tracenf("[PROJECT] startup scene missing: %s", scenePath.string().c_str());
            continue;
        }
        if (SceneManager::Instance().LoadScene(scenePath.string()))
        {
            Tracenf("[PROJECT] startup scene loaded: %s", scenePath.string().c_str());
            return true;
        }
        Tracenf("[PROJECT] startup scene load failed: %s", scenePath.string().c_str());
    }

    return false;
}

bool EditorImGui::CreateDefaultProjectScene()
{
    ProjectManager& projects = ProjectManager::Instance();
    if (!projects.HasProject())
        return false;

    std::filesystem::path scenePath = projects.ScenesPath() / "Main" / "Main.scene";
    if (std::filesystem::exists(scenePath))
    {
        if (SceneManager::Instance().LoadScene(scenePath.string()))
        {
            Tracenf("[PROJECT] existing default scene loaded: %s", scenePath.string().c_str());
            return true;
        }

        for (int index = 2; index < 1000; ++index)
        {
            const std::string name = "Main_" + std::to_string(index);
            std::filesystem::path candidate = projects.ScenesPath() / name / (name + ".scene");
            if (!std::filesystem::exists(candidate))
            {
                scenePath = std::move(candidate);
                break;
            }
        }
    }

    SceneManager& scenes = SceneManager::Instance();
    scenes.NewScene();
    scenes.SetSceneName("Main");

    if (!scenes.SaveSceneAs(scenePath.string()))
    {
        TraceError("[PROJECT] default scene create failed: %s", scenePath.string().c_str());
        return false;
    }

    AssetDatabase::Instance().getOrCreateGuid(scenePath);
    Tracenf("[PROJECT] default scene created: %s", scenePath.string().c_str());
    return true;
}

void EditorImGui::CreateProjectFromDialog()
{
    std::string error;
    if (!ProjectManager::Instance().CreateProject(m_projectParentBuffer, m_projectNameBuffer, error))
    {
        m_projectStatus = "Create failed: " + error;
        return;
    }

    ActivateCurrentProject();
    ImGui::CloseCurrentPopup();
}

void EditorImGui::OpenProjectFromDialog(const std::filesystem::path& manifestPath)
{
    std::string error;
    if (!ProjectManager::Instance().OpenProject(manifestPath, error))
    {
        m_projectStatus = "Open failed: " + error;
        return;
    }

    ActivateCurrentProject();
    ImGui::CloseCurrentPopup();
}

