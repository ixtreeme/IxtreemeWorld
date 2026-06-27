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

