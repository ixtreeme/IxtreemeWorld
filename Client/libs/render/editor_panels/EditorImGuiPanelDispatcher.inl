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
    RenderEditorToolbar();
    RenderHierarchyPanel();
    RenderSceneSettingsPanel();
    RenderWorldPanel();
    RenderToolsPanel();
    RenderAssetBrowser();
    RenderInspector();
    RenderSceneViewDropTarget();
    RenderWaterSculptToolPanel();
    RenderHeightmapToolPanel();
    RenderSplatPaintToolPanel();
    RenderTreeGeneratorPanel();
    RenderWaterMaterialEditor();
    RenderPbrMaterialEditor();
    RenderCreatePbrMaterialPopup();
    RenderFbxExportPopup();
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

