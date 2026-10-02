// This file is included from EditorImGui.cpp inside the editor-enabled implementation block.
// Keep shared anonymous-namespace helpers in EditorImGui.cpp until this panel group is fully decoupled.

void EditorImGui::RenderEditorPanels()
{
    if (!m_editorModeActive)
        return;

    RenderDockSpace();
    RenderMenuBar();
    RenderProjectModal();
    HandleEditorHotkeys();
    RunProjectAutoSave();
    m_pendingScriptChanges = PollScriptFileChanges();  // save-to-live: drained by EngineApplication
    RenderEditorToolbar();
    RenderHierarchyPanel();
    RenderSceneSettingsPanel();
    RenderWorldPanel();
    RenderToolsPanel();
    RenderAssetBrowser();
    RenderScriptsPanel();
    RenderInspector();
    RenderBuildOutputPanel();
    RenderSceneViewDropTarget();
    RenderGameViewPanel();
    RenderAnimatorPanel();
    RenderWaterSculptToolPanel();
    RenderHeightmapToolPanel();
    RenderSplatPaintToolPanel();
    RenderTreeGeneratorPanel();
    RenderWaterMaterialEditor();
    RenderPbrMaterialEditor();
    RenderCreatePbrMaterialPopup();
    RenderFbxExportPopup();

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
        for (const char* name : {"Scene View", "Asset Browser", "Inspector", ICON_FA_LIST_TREE " Hierarchy"})
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
        ImGui::SetWindowFocus("Scene View");
    }
}

void EditorImGui::RenderTreeGeneratorPanel()
{
    if (!m_treeGeneratorPanel)
        return;
    m_treeGeneratorPanel->SetAssetLibrary(m_assetLibrary.get());
    if (m_treeGeneratorPanel->Render())
    {
        RefreshAssetLibrary();
        m_assetFilter = AssetBrowserFilter::Model;
        m_assetStatus = m_treeGeneratorPanel->Status();
    }
}

