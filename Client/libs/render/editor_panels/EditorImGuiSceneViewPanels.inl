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
        ImGui::DockBuilderDockWindow(ICON_FA_PERSON_RUNNING " Animator", mainId);
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
        // Scene View is not visible (e.g. the Game tab is in front in the same dock).
        // Invalidate its viewport input rect so mouse input over the now-hidden region
        // does not leak into scene picking / object manipulation, and so the free-fly
        // camera input gate (which keys off this rect) does not misfire.
        m_viewportInputDiagnostics.sceneViewRectValid = false;
        m_viewportInputDiagnostics.sceneViewHovered = false;
        m_viewportInputDiagnostics.sceneViewFocused = false;
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

    const bool editingColliderCenter =
        m_editSelectedColliderInScene &&
        m_sceneGizmoEntityType == HierarchyEntityType::MeshEntity &&
        m_meshRendererState.selected &&
        m_meshRendererState.id == m_sceneGizmoEntityId &&
        m_meshRendererState.hasCollider;
    if (m_sceneGizmoOperation == MapEditorGizmoOperation::Translate &&
        !editingColliderCenter &&
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
    m_commands.sceneGizmoTarget = editingColliderCenter
        ? SceneGizmoTargetKind::ColliderCenter
        : SceneGizmoTargetKind::Object;
    m_commands.sceneGizmoEntityType = m_sceneGizmoEntityType;
    m_commands.sceneGizmoEntityId = m_sceneGizmoEntityId;
    m_commands.sceneGizmoOperation = m_sceneGizmoOperation;
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

void EditorImGui::RenderGameViewPanel()
{
    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoCollapse;
    if (!ImGui::Begin("Game", nullptr, flags))
    {
        // Window collapsed or its dock tab is inactive — not visible, so the engine can
        // skip rendering the Game view this frame.
        m_gameViewVisible = false;
        ImGui::End();
        return;
    }
    m_gameViewVisible = true;

    const ImVec2 regionMin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 imageMin = regionMin;
    ImVec2 imageSize = avail;
    if (avail.x > 1.0f && avail.y > 1.0f &&
        m_gameViewExtent.width > 0 && m_gameViewExtent.height > 0)
    {
        const float targetAspect = static_cast<float>(m_gameViewExtent.width) /
            static_cast<float>(m_gameViewExtent.height);
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
    if (m_gameViewDescriptor && avail.x > 1.0f && avail.y > 1.0f)
    {
        ImGui::SetCursorScreenPos(imageMin);
        ImGui::Image(reinterpret_cast<ImTextureID>(m_gameViewDescriptor), imageSize);
    }
    else if (avail.x > 1.0f && avail.y > 1.0f)
    {
        ImGui::BeginDisabled();
        ImGui::TextUnformatted("Game render target unavailable");
        ImGui::EndDisabled();
    }
    ImGui::End();
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
    // Reconcile once on open so script sources sitting in the asset scripts folder (e.g. a .cpp added
    // outside the editor, or migrated from the legacy <ProjectRoot>/Scripts on a prior build) are
    // registered as browsable Script assets immediately — Initialize() only LoadManifest()s.
    if (m_assetLibrary)
        RefreshAssetLibrary();
    LoadProjectGameModules(projects.ProjectRoot());  // native C++ game-module DLLs (Unreal-style)
    // Reset the save-to-live poll for the new project: clear the old project's mtimes and force a fresh
    // first-pass seed (so a project switch never spuriously reports changes / auto-builds).
    m_luaMtimes.clear();
    m_cppMtimes.clear();
    m_lastScriptPollSeconds = 0.0;
    LoadProjectPhysicsSettings();
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

void EditorImGui::LoadProjectGameModules(const std::filesystem::path& projectRoot)
{
    // Native C++ game modules (Unreal-style): a third-party dev compiles their NativeScript classes into
    // a DLL against the SDK and drops it in <ProjectRoot>/Binaries. We load each, resolve the C-ABI entry
    // point, and let it register its classes into the shared native registry — so they appear in the
    // Script inspector and run in Play, all without the engine source. Reload on every project activation.
    UnloadGameModules();

    const std::filesystem::path modulesDir = projectRoot / "Binaries";
    std::error_code ec;
    if (!std::filesystem::is_directory(modulesDir, ec))
        return;

#if defined(_WIN32)
    const std::string moduleExt = ".dll";
#else
    const std::string moduleExt = ".so";
#endif

    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(modulesDir, ec))
    {
        if (!entry.is_regular_file(ec))
            continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != moduleExt)
            continue;

        std::string loadError;
        platform::DynamicLibraryHandle handle = platform::OpenLibrary(entry.path(), &loadError);
        if (!handle)
        {
            TraceError("[SCRIPT] game module failed to load: %s (%s)",
                entry.path().filename().string().c_str(), loadError.c_str());
            continue;
        }
        auto entryFn = reinterpret_cast<ixscript::IxModuleEntryFn>(
            platform::GetLibrarySymbol(handle, IXTREEME_MODULE_ENTRY_SYMBOL));
        if (!entryFn)
        {
            // Not an Ixtreeme game module (no entry export) — unload and ignore.
            platform::CloseLibrary(handle);
            continue;
        }
        const ixscript::ModuleLoadResult result = ixscript::InvokeGameModule(entryFn);
        if (!result.versionOk)
        {
            platform::CloseLibrary(handle);
            continue;  // InvokeGameModule already logged the version mismatch
        }
        m_loadedGameModules.push_back(handle);
        Tracenf("[SCRIPT] loaded game module %s: %d native class(es) registered",
            entry.path().filename().string().c_str(), result.registeredCount);
    }
}

void EditorImGui::UnloadGameModules()
{
    // Purge module-registered classes from the native registry BEFORE unloading their DLLs, so no
    // factory points into freed code. Only safe outside Play (live ScriptInstances hold DLL vtables);
    // ActivateCurrentProject runs at project open, never mid-Play. v1: no hot reload.
    if (!m_loadedGameModules.empty())
        ixscript::ClearExternalNativeScripts();
    for (platform::DynamicLibraryHandle handle : m_loadedGameModules)
        platform::CloseLibrary(handle);
    m_loadedGameModules.clear();
}

std::filesystem::path EditorImGui::EngineSdkIncludeDir() const
{
    // m_engineRoot may be the engine SOURCE root (has sdk/) OR the exe directory (several levels below
    // it, e.g. build/apps/client/Release). Search the engine root and its parents for a real
    // sdk/include/ixtreeme; this also picks up a packaged "sdk next to the exe" layout (checked first).
    auto hasSdk = [](const std::filesystem::path& base) {
        std::error_code ec;
        return std::filesystem::exists(base / "sdk" / "include" / "ixtreeme" / "NativeScript.h", ec);
    };
    std::filesystem::path dir = m_engineRoot;
    for (int i = 0; i < 10 && !dir.empty(); ++i)
    {
        if (hasSdk(dir))
            return dir / "sdk" / "include";
        if (dir.parent_path() == dir)
            break;
        dir = dir.parent_path();
    }
    return m_engineRoot / "sdk" / "include";  // not found — emit the nominal path (build fails loudly)
}

void EditorImGui::EnsureProjectScriptsScaffold(const std::filesystem::path& projectRoot)
{
    std::error_code ec;
    // scriptsDir holds the engine-owned CMake project + build tree only. The dev's .cpp SOURCES live
    // in the asset library scripts folder (srcDir) alongside .lua, so both are first-class, browsable,
    // drag-attachable assets — and the build never pollutes the asset folder with intermediates.
    const std::filesystem::path scriptsDir = projectRoot / "Scripts";
    const std::filesystem::path srcDir = ProjectScriptSourceDir();
    std::filesystem::create_directories(scriptsDir, ec);
    if (ec)
    {
        m_projectStatus = "Failed to create Scripts/: " + ec.message();
        return;
    }
    std::filesystem::create_directories(srcDir, ec);

    // Migrate legacy layout: native sources used to live in <ProjectRoot>/Scripts/*.cpp. Move any
    // stray top-level .cpp into the asset scripts folder so existing projects upgrade seamlessly (the
    // asset-library reconcile then registers them as Script assets). The build tree is left untouched.
    // Snapshot the paths FIRST — renaming entries out of the directory we're iterating would otherwise
    // invalidate the iterator.
    std::vector<std::filesystem::path> strayCpp;
    for (const std::filesystem::directory_entry& e : std::filesystem::directory_iterator(scriptsDir, ec))
    {
        if (ec)
            break;
        if (e.is_regular_file(ec) && e.path().extension() == ".cpp")
            strayCpp.push_back(e.path());
    }
    for (const std::filesystem::path& stray : strayCpp)
    {
        const std::filesystem::path moved = srcDir / stray.filename();
        if (std::filesystem::exists(moved, ec))
            continue;  // a same-named source already lives in the asset folder — leave the stray one
        std::error_code moveEc;
        std::filesystem::rename(stray, moved, moveEc);
        if (moveEc)
        {
            std::filesystem::copy_file(stray, moved, std::filesystem::copy_options::overwrite_existing, moveEc);
            if (!moveEc)
                std::filesystem::remove(stray, moveEc);
        }
    }

    // CMake target name must be a bare identifier — sanitize the (possibly spaced) project name.
    std::string proj = ProjectManager::Instance().CurrentProject().name;
    if (proj.empty())
        proj = "Game";
    for (char& c : proj)
    {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '_')
            c = '_';
    }
    if (std::isdigit(static_cast<unsigned char>(proj[0])))
        proj = "_" + proj;

    // Generated CMakeLists.txt — ALWAYS regenerated (engine-owned, like Unreal's UBT-generated project
    // files): the dev never edits it, and regenerating each build keeps the SDK include path + flags
    // correct (a stale path can't persist). The dev's *.cpp are NEVER touched.
    const std::filesystem::path cmakeFile = scriptsDir / "CMakeLists.txt";
    {
        const std::string sdkInclude = EngineSdkIncludeDir().generic_string();
        const std::string scriptsSrc = srcDir.generic_string();
        std::string tmpl =
            "# GENERATED by the IxtreemeWorld editor — DO NOT EDIT. Regenerated on every build.\n"
            "# Compiles the project's *.cpp game scripts (in the asset scripts folder) into\n"
            "# <ProjectRoot>/Binaries/@PROJ@.dll (SDK headers only).\n"
            "cmake_minimum_required(VERSION 3.20)\n"
            "project(@PROJ@ CXX)\n\n"
            "# Recurse so scripts authored in browser subfolders compile too (the asset scripts folder\n"
            "# holds only game sources — the build tree lives elsewhere, under <ProjectRoot>/Scripts).\n"
            "file(GLOB_RECURSE GAME_MODULE_SOURCES CONFIGURE_DEPENDS \"@SCRIPTS_SRC@/*.cpp\")\n\n"
            "# Runtime-loaded shared library (MODULE — only loaded by the engine, never linked).\n"
            "add_library(@PROJ@ MODULE ${GAME_MODULE_SOURCES})\n\n"
            "set_target_properties(@PROJ@ PROPERTIES\n"
            "    PREFIX \"\"\n"
            "    CXX_STANDARD 20\n"
            "    CXX_STANDARD_REQUIRED ON\n"
            "    LIBRARY_OUTPUT_DIRECTORY         \"${CMAKE_CURRENT_SOURCE_DIR}/../Binaries\"\n"
            "    RUNTIME_OUTPUT_DIRECTORY         \"${CMAKE_CURRENT_SOURCE_DIR}/../Binaries\"\n"
            "    LIBRARY_OUTPUT_DIRECTORY_RELEASE \"${CMAKE_CURRENT_SOURCE_DIR}/../Binaries\"\n"
            "    RUNTIME_OUTPUT_DIRECTORY_RELEASE \"${CMAKE_CURRENT_SOURCE_DIR}/../Binaries\")\n\n"
            "target_compile_definitions(@PROJ@ PRIVATE IXTREEME_GAME_MODULE=1)\n"
            "target_include_directories(@PROJ@ PRIVATE \"@SDK_INCLUDE@\")\n\n"
            "if(MSVC)\n"
            "    # Match the engine's STATIC CRT (/MT) so the C++/heap ABI lines up across the load boundary.\n"
            "    set_property(TARGET @PROJ@ PROPERTY\n"
            "        MSVC_RUNTIME_LIBRARY \"MultiThreaded$<$<CONFIG:Debug>:Debug>\")\n"
            "endif()\n";
        for (size_t p = tmpl.find("@PROJ@"); p != std::string::npos; p = tmpl.find("@PROJ@"))
            tmpl.replace(p, 6, proj);
        for (size_t p = tmpl.find("@SDK_INCLUDE@"); p != std::string::npos; p = tmpl.find("@SDK_INCLUDE@"))
            tmpl.replace(p, 13, sdkInclude);
        for (size_t p = tmpl.find("@SCRIPTS_SRC@"); p != std::string::npos; p = tmpl.find("@SCRIPTS_SRC@"))
            tmpl.replace(p, 13, scriptsSrc);
        std::ofstream(cmakeFile, std::ios::binary) << tmpl;
    }

    // Seed a starter Game.cpp (in the asset scripts folder) if the project has no C++ source yet. The
    // class is named after the file (Game) so dragging it onto an entity attaches the right class — and
    // this is the ONE .cpp that owns the module entry point (IxModuleRegistry.inl).
    bool hasCpp = false;
    for (const std::filesystem::directory_entry& e : std::filesystem::directory_iterator(srcDir, ec))
    {
        if (e.is_regular_file(ec) && e.path().extension() == ".cpp")
        {
            hasCpp = true;
            break;
        }
    }
    if (!hasCpp)
    {
        const char* seed =
            "#include \"ixtreeme/NativeScript.h\"\n"
            "#include \"ixtreeme/IxModuleRegistry.inl\"\n\n"
            "// Your first game script. Spins the entity around Y at `speed` deg/sec. `speed` is editable\n"
            "// in the inspector and serialized, thanks to IX_REFLECT. Each script is self-contained (the\n"
            "// registry .inl is inline-merged), so add more via \"New C++ Script\" — no special file.\n"
            "class Game : public ixscript::NativeScript\n"
            "{\n"
            "public:\n"
            "    float speed = 90.0f;  // deg/sec\n\n"
            "    void OnUpdate(float dt) override\n"
            "    {\n"
            "        float r[3];\n"
            "        GetRotation(r);  // Euler degrees\n"
            "        r[1] += speed * dt;\n"
            "        SetRotation(r);\n"
            "    }\n\n"
            "    IX_REFLECT(Game, speed)\n"
            "};\n"
            "IXSCRIPT_REGISTER(Game)\n";
        const std::filesystem::path seedPath = srcDir / "Game.cpp";
        std::ofstream(seedPath, std::ios::binary) << seed;
        // Stamp the just-seeded file into the poll map so it isn't seen as a "new .cpp" on the next
        // poll (which would otherwise fire one redundant auto-build right after the first build).
        const std::filesystem::file_time_type mt = std::filesystem::last_write_time(seedPath, ec);
        if (!ec)
            m_cppMtimes[seedPath.generic_string()] = mt;
    }

    // Register any migrated/seeded .cpp as Script assets now, so they appear in the asset browser
    // immediately (Refresh's reconcile discovery picks up sources not yet in the manifest).
    if (m_assetLibrary)
    {
        std::string reconcileError;
        m_assetLibrary->Refresh(reconcileError);
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

