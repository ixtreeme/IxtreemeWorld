// This file is included from EditorImGui.cpp inside the editor-enabled implementation block.
// Keep shared anonymous-namespace helpers in EditorImGui.cpp until this panel group is fully decoupled.

void EditorImGui::RenderEditorPanels()
{
    if (!m_editorModeActive)
        return;

    // The shell first: the menu bar, toolbar and status bar take their strips of the window and the
    // dock space fills the rest.
    RenderMenuBar();
    RenderMainToolbar();
    RenderStatusBar();
    RenderDockSpace();
    RenderProjectModal();
    HandleEditorHotkeys();
    RunProjectAutoSave();
    m_pendingScriptChanges = PollScriptFileChanges();  // save-to-live: drained by EngineApplication
    RenderHierarchyPanel();
    RenderSceneViewDropTarget();
    if (!m_sceneView2D && m_uiSelectionActive && GetViewportInputDiagnostics().sceneViewHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        m_uiSelectionActive = false;
    RenderGameViewPanel();
    RenderAnimatorPanel();
    RenderMaterialGraphPanel();
    RenderInspector();
    RenderSceneSettingsPanel();
    RenderAssetBrowser();
    RenderScriptsPanel();
    RenderConsolePanel();
    RenderBuildOutputPanel();
    RenderBuildGamePopup();
    RenderCreateTerrainModal();
    RenderStatisticsWindow();
    RenderPhysicsDebuggerWindow();
    RenderProjectSettingsWindow();
    RenderLayeredWorldWindow();
    RenderShortcutsWindow();
    m_helpBrowser.Render();
    RenderAboutPopup();
    RenderTreeGeneratorPanel();
    RenderMaterialEditor();
    RenderCreatePbrMaterialPopup();
    RenderFbxExportPopup();
    TrackStatusMessages();
    if (!ImGui::GetActiveID()) m_uiLastEditWidget = 0;

    // Auto-switch the active viewport tab on Play/Stop: Game (project Main Camera)
    // while playing, Scene View (free-fly editor camera) while editing. ImGui's public
    // focus API does not reliably select a docked tab, so set the dock node's selected
    // tab directly (imgui_internal), with SetWindowFocus as a fallback for the
    // non-docked (floating / side-by-side) layout.
    if (m_pendingViewFocusWindow)
    {
        ImGuiWindow* viewWindow = ImGui::FindWindowByName(m_pendingViewFocusWindow);
        if (viewWindow != nullptr && viewWindow->DockNode != nullptr)
        {
            if (viewWindow->DockNode->TabBar != nullptr)
                viewWindow->DockNode->TabBar->NextSelectedTabId = viewWindow->TabId;
            viewWindow->DockNode->SelectedTabId = viewWindow->TabId;
        }
        ImGui::SetWindowFocus(m_pendingViewFocusWindow);
        m_pendingViewFocusWindow = nullptr;
    }

    // Fresh default layout: a dock node shows the tab that joined it last (Scripts over the Asset
    // Browser), so pick the everyday tabs for the first few frames, until every docked window has
    // joined its node's tab bar.
    if (m_defaultLayoutTabSelectFrames > 0)
    {
        --m_defaultLayoutTabSelectFrames;
        for (const char* name : {EditorWindow::SceneView, EditorWindow::AssetBrowser, EditorWindow::Inspector, EditorWindow::Hierarchy})
        {
            ImGuiWindow* window = ImGui::FindWindowByName(name);
            if (window == nullptr || window->DockNode == nullptr)
                continue;
            if (window->DockNode->TabBar != nullptr)
                window->DockNode->TabBar->NextSelectedTabId = window->TabId;
            window->DockNode->SelectedTabId = window->TabId;
        }
    }
    // Any layout: every window created at startup takes focus on appearing, and a dock node shows its
    // focused window's tab, so the last one created (the Animator) covered the viewport. The editor
    // starts in Edit mode: focus the Scene View for the first few frames.
    if (m_startupViewFocusFrames > 0)
    {
        --m_startupViewFocusFrames;
        ImGui::SetWindowFocus(EditorWindow::SceneView);
    }
}

void EditorImGui::OpenMaterialGraphAsset(const AssetLibrary::Entry& entry)
{
    if (!m_assetLibrary) return;
    if (!m_materialGraphPanel) m_materialGraphPanel = std::make_unique<material_tool::MaterialGraphPanel>();
    m_materialGraphPanel->SetServices(m_assetLibrary.get(), m_textureProvider);
    m_materialGraphPanel->RequestOpen(m_assetLibrary->AbsolutePath(entry));
    m_materialGraphPanelOpen = true;
    m_inspectorShowsMaterialGraph = true;
    m_assetInspectorSelectionActive = false;
}

void EditorImGui::RenderMaterialGraphPanel()
{
    if (!m_materialGraphPanelOpen && !m_materialGraphPanel) return;
    if (!m_materialGraphPanel) m_materialGraphPanel = std::make_unique<material_tool::MaterialGraphPanel>();
    if (const auto* scene = ImGui::FindWindowByName(EditorWindow::SceneView); m_materialGraphPanelOpen && scene && scene->DockId)
        ImGui::SetNextWindowDockID(scene->DockId, ImGuiCond_FirstUseEver);
    m_materialGraphPanel->SetServices(m_assetLibrary.get(),m_textureProvider);
    std::filesystem::path modelPath;
    if (m_assetLibrary && m_meshRendererState.selected) {
        if (const auto asset=m_assetLibrary->FindById(m_meshRendererState.meshAssetId)) modelPath=m_assetLibrary->AbsolutePath(*asset);
        else if (const auto guid=Guid::fromString(m_meshRendererState.meshAssetId)) {
            if (const auto path=AssetDatabase::Instance().resolveGuid(*guid)) modelPath=*path;
        }
        if (modelPath.empty() && !m_meshRendererState.meshAssetPath.empty()) {
            modelPath=m_meshRendererState.meshAssetPath;
            if (modelPath.is_relative()) modelPath=ProjectManager::Instance().ProjectRoot()/modelPath;
        }
    }
    std::error_code modelPathError;
    if (!modelPath.empty() && (!std::filesystem::is_regular_file(modelPath,modelPathError) || detectAssetType(modelPath)!=AssetType::Model)) modelPath.clear();
    m_materialGraphPanel->SetApplyTarget(m_meshRendererState,modelPath,m_playModeState.mode==EditorPlayMode::Edit && ProjectManager::Instance().HasProject());
    m_materialGraphPanel->RenderView(m_materialGraphPanelOpen);
    if (auto assignment=m_materialGraphPanel->TakeMaterialAssignment()) m_commands.smartMaterialAssignments.push_back(std::move(*assignment));
    InspectorShowsMaterialGraph();
}

bool EditorImGui::InspectorShowsMaterialGraph()
{
    if (!m_materialGraphPanelOpen || !m_materialGraphPanel)
    {
        m_inspectorShowsMaterialGraph = false;
        return false;
    }
    if (m_materialGraphPanel->TakeInspectorRequest())
        m_inspectorShowsMaterialGraph = true;
    else if (const auto* focused = ImGui::GetCurrentContext()->NavWindow)
    {
        for (const char* name : {EditorWindow::SceneView, EditorWindow::Game, EditorWindow::Hierarchy, EditorWindow::Animator, "Tree Generator"})
            if (focused->RootWindow == ImGui::FindWindowByName(name)) m_inspectorShowsMaterialGraph = false;
    }
    return m_inspectorShowsMaterialGraph;
}

void EditorImGui::RenderTreeGeneratorPanel()
{
    if (!m_treeGeneratorPanel)
        return;
    m_treeGeneratorPanel->SetAssetLibrary(m_assetLibrary.get());
    m_treeGeneratorPanel->SetTextureProvider(m_textureProvider);
    m_treeGeneratorPanel->RenderView();
}

bool EditorImGui::InspectorShowsTreeGenerator()
{
    // From when the Tree Generator view takes focus (or opens) until a window that selects things
    // does: the Scene View, the Game view, the Hierarchy or the Asset Browser.
    if (!m_treeGeneratorPanel || !m_treeGeneratorPanel->IsOpen())
    {
        m_inspectorShowsTreeGenerator = false;
        return false;
    }
    if (m_treeGeneratorPanel->TakeInspectorRequest())
    {
        m_inspectorShowsTreeGenerator = true;
    }
    else if (const ImGuiWindow* focused = ImGui::GetCurrentContext()->NavWindow)
    {
        for (const char* name : {EditorWindow::SceneView, EditorWindow::Game, EditorWindow::Hierarchy, EditorWindow::AssetBrowser})
        {
            if (focused->RootWindow == ImGui::FindWindowByName(name))
                m_inspectorShowsTreeGenerator = false;
        }
    }
    return m_inspectorShowsTreeGenerator;
}

