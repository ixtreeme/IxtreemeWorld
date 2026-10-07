#pragma once

// EditorImGui — Phase-3B: zero Vulkan types in this header.
//
// The ImGui Vulkan backend (descriptor pool, backend init/shutdown, draw-data
// submission) and all UI texture registrations live in IXVulkanEditorAdapter
// (Engine/Graphics/Vulkan, backend-specific editor integration). This class
// consumes the Vk-free IEditorTextureProvider interface for scene/game views
// and asset-preview thumbnails. imgui.h inclusion is fine (not Vulkan).

#include "AssetLibrary.h"
#include "InputEvent.h"
#include "MapEditorTypes.h"
#include "WorldCamera.h"
#include "platform/dynamic_library.h"
#include "tools/tree/TreeGeneratorPanel.h"

#include "EditorGraphicsBridge.h"

#include <cstdint>
#include <cstddef>
#include <array>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

struct ImVec2;

class EditorImGui
{
public:
    struct ViewportInputDiagnostics
    {
        bool assetDragActive = false;
        bool dropTargetVisible = false;
        bool dropTargetHovered = false;
        bool dropTargetActive = false;
        bool overlayDropTargetHovered = false;
        bool overlayDropTargetActive = false;
        bool sceneViewRectValid = false;
        bool sceneViewHovered = false;
        bool sceneViewFocused = false;
        float sceneViewMin[2] = {0.0f, 0.0f};
        float sceneViewSize[2] = {0.0f, 0.0f};
        uint32_t sceneViewExtent[2] = {0u, 0u};
        std::uint32_t hoveredItemId = 0;
        std::uint32_t activeItemId = 0;
    };

    EditorImGui() = default;
    ~EditorImGui();

    // Generic UI init (context/IO/fonts/style). The backend adapter must have
    // created the ImGui context first (adapter->CreateBackend).
    bool Create();
    // Backend UI texture provider (adapter). Must outlive this object.
    void SetTextureProvider(ixeditor::graphics::IEditorTextureProvider* provider);
    bool HasTextureProvider() const { return m_textureProvider != nullptr; }

    void BeginFrame(bool editorModeActive);
    // Builds all panels and calls ImGui::Render (generic). The frame owner
    // submits draw data through the backend adapter afterwards.
    void RenderPanels();
    // True when the Game panel was actually visible (active dock tab, not collapsed) last
    // frame. Lets the engine skip the expensive Game-view scene render when it's not shown.
    bool IsGameViewVisible() const { return m_gameViewVisible; }
    // Whether the Scene View panel was visible last frame (e.g. not behind the Game tab in Play).
    bool IsSceneViewVisible() const { return !m_editorModeActive || m_sceneViewVisible; }
    // The editor UI is up and shows the offscreen scene as its Scene View panel image (so a full-window
    // composite of that scene under the UI would be covered by it).
    bool ShowsSceneViewAsPanel() const
    {
        return m_editorModeActive && m_textureProvider && m_textureProvider->GetSceneViewTexture() != nullptr;
    }
    // The image area of the Scene View / Game panel in framebuffer pixels, as laid out last frame
    // (0 x 0 before the panel was first shown). The views render at this size, not the window's.
    void GetSceneViewPanelPixels(std::uint32_t& width, std::uint32_t& height) const
    {
        width = m_sceneViewPanelPixels[0];
        height = m_sceneViewPanelPixels[1];
    }
    void GetGameViewPanelPixels(std::uint32_t& width, std::uint32_t& height) const
    {
        width = m_gameViewPanelPixels[0];
        height = m_gameViewPanelPixels[1];
    }

    // Script text prompts (IScriptApi::PromptText) drawn as small modal-less dialogs during Play. The
    // engine hands the open prompts in each frame and collects the answers after RenderPanels. Secret
    // prompts are masked, and every input buffer is wiped once answered or no longer listed.
    struct ScriptPromptView
    {
        std::uint32_t id = 0;
        std::string title;
        std::string label;
        bool secret = false;
    };
    struct ScriptPromptAnswer
    {
        std::uint32_t id = 0;
        bool submitted = false;  // false = cancelled
        std::string text;
    };
    void SetScriptPrompts(std::vector<ScriptPromptView> prompts);
    std::vector<ScriptPromptAnswer> TakeScriptPromptAnswers();

    // Native C++ game-module DLLs (Unreal-style): load/unload the project's modules, and the in-engine
    // Build pipeline driven from RunGame's frame loop. Called from EngineApplication.
    void LoadProjectGameModules(const std::filesystem::path& projectRoot);
    void UnloadGameModules();
    // Prepares <ProjectRoot>/Scripts for the LEGACY C++ build: creates the dirs, migrates stray
    // sources, and (re)generates the engine-owned CMakeLists.txt only when the project actually has
    // .cpp script sources. C++ is no longer a project language — nothing is seeded. Returns true when
    // there is something to build.
    bool EnsureProjectScriptsScaffold(const std::filesystem::path& projectRoot);
    std::filesystem::path EngineSdkIncludeDir() const;  // resolves <root>/sdk/include (searches up)
    void SetBuildRunning() { m_buildState = ScriptBuildState::Running; }
    void SetBuildResult(bool ok, std::string log)
    {
        m_buildState = ScriptBuildState::Done;
        m_buildSucceeded = ok;
        m_buildLog = std::move(log);
        m_buildOutputPanelOpen = true;
    }
    // "Build Game" (packaging the project into a folder that runs without the editor).
    void SetGameBuildRunning()
    {
        m_gameBuildState = ScriptBuildState::Running;
        m_buildOutputPanelOpen = true;
    }
    void SetGameBuildResult(bool ok, std::string log, std::filesystem::path executable)
    {
        m_gameBuildState = ScriptBuildState::Done;
        m_gameBuildSucceeded = ok;
        m_gameBuildLog = std::move(log);
        m_gameBuildExecutable = std::move(executable);
        m_buildOutputPanelOpen = true;
    }
    bool IsGameBuildRunning() const { return m_gameBuildState == ScriptBuildState::Running; }

    // Save-to-live iteration: a per-frame (throttled) mtime poll over the project's .lua/.as Script
    // assets and <ProjectRoot>/Scripts/*.cpp,*.h. EngineApplication drains m_pendingScriptChanges each
    // frame to hot-reload AngelScript/Lua (in Play) and auto-build legacy native C++ (in Edit).
    struct ScriptFileChanges
    {
        std::vector<std::string> changedLua;          // Script asset ids whose .lua mtime changed
        std::vector<std::string> changedAngelScript;  // Script asset ids whose .as mtime changed
        bool changedCpp = false;                      // any <Project>/Scripts/*.cpp,*.h,*.hpp changed/added/removed
    };
    ScriptFileChanges PollScriptFileChanges();
    ScriptFileChanges m_pendingScriptChanges;  // set in RenderEditorPanels, drained by EngineApplication
    bool AutoBuildOnSave() const { return m_autoBuildOnSave; }
    void SetAutoBuildOnSave(bool on) { m_autoBuildOnSave = on; }

    void SetSceneViewSelectionOutline(std::vector<std::array<float, 4>> segments);
    void SetSceneViewGizmo(HierarchyEntityType type,
                           std::uint32_t id,
                           const WorldCamera& camera,
                           const float* position,
                           const float* rotation,
                           const float* scale,
                           MapEditorGizmoOperation operation,
                           bool snapEnabled,
                           float snapValue);
    void ClearSceneViewGizmo();
    bool IsSceneGizmoInputActive() const { return m_sceneGizmoInputActive; }
    bool WantsInputCapture(const InputEvent& event) const;
    bool IsTextInputActive() const;
    bool IsSceneViewInputTarget(const InputEvent& event) const;
    InputEvent MapInputToSceneView(const InputEvent& event) const;
    // A window mouse event in the Game view's pixels (its render target), for the game's own UI.
    // False when the Game view is not shown or the pointer is outside its image.
    bool MapInputToGameView(const InputEvent& event, InputEvent& out) const;
    void SetSceneViewKeyboardFocus(bool focused);
    void SetMapEditorSettings(const MapEditorSettings& settings);
    MapEditorSettings GetMapEditorSettings() const { return m_editorSettings; }
    void SetEditorPlayModeState(const EditorPlayModeState& state);
    void SetLightingState(const LightingState& state);
    LightingState GetLightingState() const { return m_lightingState; }
    void SetSkySettings(const SkySettings& sky) { m_skySettings = sky; }
    const SkySettings& GetSkySettings() const { return m_skySettings; }
    // Shown under the sky settings while the sky cannot draw as set (a missing image, ...).
    void SetSkyStatus(const std::string& status) { m_skyStatus = status; }
    void SetDynamicLightEditorState(const DynamicLightEditorState& state);
    void SetCameraEditorState(const CameraEditorState& state);
    void SetWaterBodyEditorState(const WaterBodyEditorState& state);
    void SetMeshRendererEditorState(const MeshRendererEditorState& state);
    void SetLayerAuthoringStatus(std::string status) { m_layerAuthoringStatus = std::move(status); }
    void SetAnimatorGraphEditorState(const AnimatorGraphEditorState& state) { m_animatorGraphState = state; }
    bool IsAnimatorGraphVisible() const { return m_animatorGraphVisible; }
    void SetTerrainEditorState(const TerrainEditorState& state);
    void SetEngineStats(const EngineStats& stats);
    void SetPhysicsEvents(std::vector<PhysicsEventEditorState> events);
    void SetHierarchySceneState(std::uint64_t sceneRootEntity,
                                std::string sceneRootName,
                                std::vector<HierarchySceneEntity> entities);
    std::uint64_t GetSelectedHierarchyEntity() const { return m_selectedHierarchyEntity; }
    void SetWaterMaterials(std::vector<std::pair<std::string, WaterMaterialData>> materials);
    void SetWaterMaterialUsageCounts(std::vector<std::pair<std::string, std::uint32_t>> usageCounts);
    std::vector<std::pair<std::string, WaterMaterialData>> GetWaterMaterialsSnapshot() const;
    // Changes whenever GetWaterMaterialsSnapshot() may return something else: the asset library was
    // saved, reloaded or replaced, or a water/PBR material has unsaved edits (they are in it).
    std::uint64_t WaterMaterialsRevision() const;
    // Changes whenever the asset library may describe something else: refreshed, an entry edited
    // (a LOD default saved), or another library (a project opened). For caches over its entries.
    std::uint64_t AssetLibraryRevision() const;
    void SetPaletteSlots(const std::array<MapEditorPaletteSlot, 8>& slots);
    void SetEngineRoot(const std::filesystem::path& clientRoot);
    void InitializeAssetLibrary(const std::filesystem::path& clientRoot);
    void InitializeProjectAssetLibrary(const std::filesystem::path& projectRoot, const std::filesystem::path& assetRoot);
    void RefreshAssetLibrary();
    // The folder the asset browser shows (relative to the asset root): where new assets go.
    const std::string& CurrentAssetFolder() const { return m_assetSubpath; }
    // Generates retargetable .ixclip assets for a loaded rigged model's existing _anim_<i>.ozz
    // sidecars (joint names come from the model's skeleton). Idempotent; no-op if already present.
    void EnsureModelAnimationClips(const std::filesystem::path& modelPath, const std::vector<std::string>& jointNames);
    // Absolute filesystem path of an AnimationClip asset's .ixclip file (empty if not found).
    std::string AnimationClipFilePath(const std::string& clipId) const;
    // Absolute filesystem path of an AnimatorController asset's .controller file (empty if none).
    std::string AnimatorControllerFilePath(const std::string& controllerId) const;
    std::string AudioClipFilePath(const std::string& clipId) const;
    // A Texture asset id -> its file path ("" = not found). Used by the ParticleRenderer's
    // texture resolver (the renderer decodes + caches the image itself).
    std::string TextureFilePath(const std::string& textureId) const;
    // A .particle effect asset id -> its file path ("" = not found).
    std::string ParticleEffectFilePath(const std::string& effectId) const;
    // Reads a .particle preset and copies its parameters into `out` (the inspector's picker).
    bool ApplyParticleEffectPreset(const std::string& effectId, ixparticle::ParticleSystemComponent& out) const;
    // Absolute filesystem path of a Script asset's .lua file (empty if none) — for the Lua backend.
    std::string ScriptSourceFilePath(const std::string& scriptId) const;
    // Id of the first AnimationClip whose display name matches (empty if none) — for auto-filling
    // a controller's states with a character's own <stem>_anim_<i> clips.
    std::string FindAnimationClipIdByDisplayName(const std::string& displayName) const;
    void ImportExternalFiles(const std::vector<std::string>& paths, const char* trigger = "dragdrop");
    std::optional<LodConfig> FindModelLodDefault(const std::string& assetId) const;
    bool SaveModelLodDefault(const std::string& assetId, const LodConfig& config);
    std::optional<AssetLibrary::PhysicsMaterialData> FindPhysicsMaterial(const std::string& id) const;
    bool OpenWaterMaterialEditor(const std::string& materialId);
    bool OpenPbrMaterialEditor(const std::string& materialId);
    MapEditorCommands ConsumeCommands();
    ViewportInputDiagnostics GetViewportInputDiagnostics() const { return m_viewportInputDiagnostics; }
    void Destroy();

private:
    enum class ProjectDialogMode
    {
        None,
        NoProject,
        Create,
        Open
    };

    struct AssetPreviewTexture
    {
        ixeditor::graphics::EditorTextureHandle handle;
        uint32_t width = 0;
        uint32_t height = 0;
        bool failed = false;
    };

    struct PrefabInspectorEditRow
    {
        std::uint32_t localId = 0;
        std::uint32_t parentLocalId = 0;
        std::string type;
        std::string lightType;
        std::string meshAssetId;
        std::string meshAssetPath;
        std::string prefabAssetId;
        std::vector<std::array<char, 128>> materialSlotBuffers;
        char name[128]{};
        bool transformValid = false;
        float position[3] = {0.0f, 0.0f, 0.0f};
        float rotation[3] = {0.0f, 0.0f, 0.0f};
        float scale[3] = {1.0f, 1.0f, 1.0f};
        bool lightValid = false;
        float color[3] = {1.0f, 1.0f, 1.0f};
        float intensity = 1.0f;
        float radius = 1.0f;
        float innerConeDegrees = 20.0f;
        float outerConeDegrees = 35.0f;
        bool enabled = true;
    };

    void RenderEditorPanels();
    void RenderDemoPanels();
    void RenderDockSpace();
    void RenderSceneViewDropTarget();
    void RenderGameViewPanel();
    void RenderAnimatorPanel();
    void RenderSceneViewGizmo(const ImVec2& imageMin, const ImVec2& imageSize);
    void RenderMenuBar();
    bool SaveProjectAndCurrentScene(bool automatic = false);
    // True (with a status message) while Play is running: Play-mode state is never saved.
    bool RefuseSaveDuringPlay();
    void RunProjectAutoSave();
    void UpdateAutoSaveWindowTitle(double now);
    void LoadProjectPhysicsSettings();
    void StoreProjectPhysicsSettings();
    void RenderProjectModal();
    void RenderProjectBrowser(bool pickProjectFile);
    void OpenProjectDialog(ProjectDialogMode mode);
    bool NavigateProjectBrowser(const std::filesystem::path& path, bool createMissing = false);
    void ActivateCurrentProject();
    bool IsBuildRunning() const { return m_buildState == ScriptBuildState::Running; }
    bool LoadProjectStartupScene();
    bool CreateDefaultProjectScene();
    void CreateProjectFromDialog();
    void OpenProjectFromDialog(const std::filesystem::path& manifestPath);
    // Editor shell (EditorImGuiShellPanels.inl): fixed toolbar and status bar, Console, and the
    // windows opened from the menus.
    enum class ConsoleSeverity
    {
        Info,
        Warning,
        Error
    };
    struct ConsoleMessage
    {
        double time = 0.0;
        std::string clock;
        ConsoleSeverity severity = ConsoleSeverity::Info;
        std::string text;
    };
    void RenderMainToolbar();
    void RenderStatusBar();
    void RenderConsolePanel();
    void PushConsoleMessage(ConsoleSeverity severity, std::string text);
    // Lists every new status text (project, asset, layer, build) in the Console and the status bar.
    void TrackStatusMessages();
    void RenderStatisticsWindow();
    void RenderPhysicsDebuggerWindow();
    void RenderProjectSettingsWindow();
    void RenderLayeredWorldWindow();
    void RenderShortcutsWindow();
    void RenderAboutPopup();
    void QueueDebugToggleCommands();
    void ResetEditorLayout();
    void RenderBuildOutputPanel();
    void OpenBuildGameDialog();
    void RenderBuildGamePopup();
    void RenderScriptsPanel();
    void RenderSceneSettingsPanel();
    struct ProjectSceneEntry
    {
        std::string name;
        std::filesystem::path path;
        std::string relativePath;
        bool active = false;
    };

    void RenderHierarchyPanel();
    void RenderHierarchyToolbar();
    std::vector<ProjectSceneEntry> QueryProjectScenes() const;
    std::vector<AssetLibrary::Entry> QuerySceneAssets() const;
    bool AttachSceneToHierarchy(const AssetLibrary::Entry& entry);
    bool DetachSceneFromHierarchy(const ProjectSceneEntry& scene);
    void RenderProjectSceneNode(const ProjectSceneEntry& scene);
    void RenderHierarchyEntityNode(std::uint64_t entity);
    void RenderHierarchyContextMenu(const HierarchySceneEntity& entity);
    bool HierarchySubtreePassesSearch(std::uint64_t entity) const;
    const HierarchySceneEntity* FindHierarchyEntity(std::uint64_t entity) const;
    const HierarchySceneEntity* FindHierarchyEntity(HierarchyEntityType type, std::uint32_t objectId) const;
    // The selected entity, picked in the Hierarchy or in the Scene View (nullptr: none).
    const HierarchySceneEntity* SelectedHierarchyEntity() const;
    bool HierarchyPassesSearch(const std::string& name) const;
    void QueueHierarchySelection(const HierarchySceneEntity& entity);
    void QueueHierarchyFocus(const HierarchySceneEntity& entity);
    void StartHierarchyRename(const HierarchySceneEntity& entity);
    void RenderHierarchyCreateMenuItems();
    void RenderInspector();
    void RenderSelectedWaterBodyInspector();
    void RenderSelectedTerrainInspector();
    void RenderSelectedLightInspector();
    void RenderSelectedCameraInspector();
    void RenderSelectedMeshRendererInspector();
    bool RenderSelectedMeshPhysicsComponents();
    void RenderPrefabOverrideControls(const std::string& assetId,
                                      const PrefabInstanceState& instance,
                                      const std::vector<std::string>& overrides);
    void RenderAddComponentMenu();
    // The Inspector's title row: what is selected, with the engine-id (debug info) toggle.
    void RenderInspectorTitle(const char* icon, const char* typeName, std::uint32_t objectId);
    bool RenderAttachedEditorComponents(std::vector<EditorAttachedComponent>& components);
    // meshScale: a model scale multiplier (fine steps down to 0.001) instead of a size in metres.
    bool RenderTransformComponent(float* position, float* rotation, float* scale, bool meshScale = false);
    bool RenderAxisFloat(const char* axis, float& value, float r, float g, float b, float speed, float minValue, float maxValue,
        const char* format = "%.2f");
    void RenderCreateTerrainModal();
    void OpenCreateTerrainDialog();
    void RenderLightingPanel();
    void RenderSkyPanel();
    void RenderGodRaysPanel();
    void RenderToneMappingPanel();
    void RenderGizmoControls();
    // Terrain and water editing tools, shown in the Inspector of the selected terrain / water body.
    void RenderWaterSculptTool();
    void RenderTerrainSculptTool();
    void RenderTerrainPaintTool();
    void RenderSplatLayerSlot(std::uint32_t slotIndex);
    void RenderWaterMaterialEditor();
    void RenderTreeGeneratorPanel();
    void RenderWaterMaterialHeader();
    void RenderWaterMaterialColorsSection(WaterMaterialData& material);
    void RenderWaterMaterialWaveSection(WaterMaterialData& material);
    void RenderWaterMaterialFoamSection(WaterMaterialData& material);
    void RenderWaterMaterialCausticSection(WaterMaterialData& material);
    void RenderWaterMaterialReflectionSection(WaterMaterialData& material);
    void RenderWaterMaterialRefractionSection(WaterMaterialData& material);
    void RenderWaterMaterialEdgeFadeSection(WaterMaterialData& material);
    void RenderWaterMaterialTexturesSection(WaterMaterialData& material);
    void RenderWaterTextureSlot(const char* label, std::string& texturePath, bool& changed);
    void RenderPbrMaterialEditor();
    void RenderPbrMaterialHeader();
    void RenderPbrTextureSlot(const char* label, std::string& textureId, bool& changed);
    bool RenderSelectedPhysicsMaterialAssetInspector();
    bool RenderSelectedPrefabAssetInspector();
    void RenderAssetBrowser();
    void RenderAssetBrowserToolbar();
    void RenderAssetBrowserFolderTree();
    void RenderAssetBrowserFolderTreeNode(const std::string& subpath);
    void RenderAssetBrowserContent();
    void RenderAssetBrowserBreadcrumb();
    void RenderAssetBrowserFolderTile(const std::string& subpath, float tileSize);
    // "New ..." items of a folder's context menu: everything is created in targetSubpath.
    void RenderAssetCreateMenuItems(const std::string& targetSubpath);
    // Drop target accepting assets and folders dragged inside the browser, moved into targetSubpath.
    void AcceptAssetBrowserDrop(const std::string& targetSubpath);
    void RenderAssetTile(const AssetLibrary::Entry& entry, float tileSize);
    void DestroyAssetPreviewTextures();
    void DestroyAssetPreviewTexture(AssetPreviewTexture& texture);
    std::optional<std::filesystem::path> AssetPreviewPathFor(const AssetLibrary::Entry& entry) const;
    std::optional<std::filesystem::path> ResolveAssetPreviewPath(const AssetLibrary::Entry& entry) const;
    AssetPreviewTexture* GetAssetPreviewTexture(const AssetLibrary::Entry& entry);
    bool LoadAssetPreviewTexture(const std::filesystem::path& path, AssetPreviewTexture& outTexture);
    void OpenImportAssetDialog(const std::string& targetSubpath);
    void ImportAssetFromPath(const std::filesystem::path& sourcePath,
                             const std::string& targetSubpath,
                             const char* trigger);
    // Copies a folder dropped from the OS (with everything in it) into the browser folder; the
    // library then picks up the assets inside.
    void ImportFolderFromPath(const std::filesystem::path& sourceFolder, const std::string& targetSubpath);
    // The asset creators below put the new asset into CreateTargetSubpath(): the folder a context
    // menu was opened on (m_assetCreateTarget, consumed), else the folder the browser shows.
    std::string CreateTargetSubpath();
    void RevealCreatedAsset(const AssetLibrary::Entry& entry);
    void CreatePbrMaterialAsset();
    void CreateLuaScriptAsset();                            // new .lua Script asset (browser/Scripts panel)
    void CreateAngelScriptAsset();                          // new .as Script asset (browser/Scripts panel)
    // New .particle preset: from the selected entity's Particle System component when it has one,
    // else the component defaults.
    void CreateParticleEffectAsset();
    // Writes `effect` out as a new .particle preset asset and reveals it in the browser.
    void CreateParticleEffectFromComponent(const ixparticle::ParticleSystemComponent& effect);
    void CreateNativeScriptAsset();                         // LEGACY: new .cpp Script asset (kept, not offered)
    void CreateNativeScriptFile(const std::string& className);  // LEGACY: new <className>.cpp Script asset
    void CreateAnimatorControllerAsset();
    // Native C++ game-script sources (.cpp) live anywhere under the project's asset folder, like
    // every other asset; the Build pipeline's CMake project (in <ProjectRoot>/Scripts) compiles all
    // of them from here.
    std::filesystem::path ProjectScriptSourceDir() const;
    // True while the project still has LEGACY C++ script sources (.cpp Script assets): the editor only
    // offers the C++ compile path in that case (AngelScript/Lua need no build).
    bool ProjectHasNativeScriptSources() const;
    // Script assets of one extension (".as" / ".lua" / ".cpp"), cached per asset-library revision:
    // the toolbar, the Scripts panel and the inspector need these lists every frame, and EntriesFor
    // copies every matching entry.
    const std::vector<AssetLibrary::Entry>& CachedScriptAssets(const char* extension) const;
    void CreateWaterMaterialAsset();
    bool CreateWaterMaterialAsset(const std::string& displayName, AssetLibrary::Entry& outEntry);
    void CreatePhysicsMaterialAsset();
    void AssignAssetToSelectedWaterBody(const std::string& assetId);
    void AssignAssetToSelectedMeshRenderer(const std::string& assetId);
    void MarkWaterMaterialChanged(const char* field);
    void MarkPbrMaterialChanged(const char* field);
    bool SaveWaterMaterialEditor();
    bool SavePbrMaterialEditor();
    bool DeleteWaterMaterialEditor();
    void SyncWaterMaterialSnapshot();
    // Both return references into the asset browser cache: callers must not hold them across a
    // cache invalidation (library swap) or across frames; use within the current frame is safe.
    const std::vector<std::string>& QueryFilesystemChildFolders(const std::string& subpath) const;
    const std::vector<AssetLibrary::Entry>& QueryFilesystemAssetsInFolder(const std::string& subpath) const;
    // Drops the cached asset browser listings when the asset library changed (or they are too old
    // to trust for edits made outside the editor).
    void ValidateAssetBrowserCache() const;
    void InvalidateAssetBrowserCache() const { m_assetBrowserCache = {}; }
    std::string CachedComparablePath(const std::filesystem::path& path) const;
    std::filesystem::path AssetBrowserRoot() const;
    std::filesystem::path AssetBrowserPath(const std::string& subpath) const;
    std::string AssetBrowserSubpath(const std::filesystem::path& path) const;
    void SelectAssetBrowserFolder(const std::string& subpath);
    void BeginAssetRename(const AssetLibrary::Entry& entry);
    void BeginFolderRename(const std::string& subpath);
    void RenderAssetBrowserOperationPopups();
    void RenderCreatePbrMaterialPopup();
    void OpenFbxExportDialogForAsset(const AssetLibrary::Entry& entry);
    void OpenFbxExportDialogForMeshEntity(const HierarchySceneEntity& entity);
    void RenderFbxExportPopup();
    void ExecuteFbxAssetExport();
    void CreateFilesystemFolder(const std::string& parentSubpath, const std::string& requestedName);
    void RenameFilesystemSelection();
    void DeleteFilesystemSelection();
    bool MoveAssetEntryToFolder(const std::string& assetId, const std::string& targetFolderSubpath);
    bool MoveFolderToFolder(const std::string& sourceSubpath, const std::string& targetFolderSubpath);
    void ApplyTimeOfDayPreset(float hour);
    void MarkSelectedWaterBodyChanged();
    void MarkSelectedLightChanged();
    void MarkSelectedCameraChanged();
    void MarkSelectedMeshRendererChanged();
    void HandleEditorHotkeys();
    bool CanUseEditorTools() const;
    void SetToolMode(MapEditorToolMode mode);
    MapEditorPaletteSlot BuildPaletteSlotFromAsset(std::uint32_t slotIndex, const AssetLibrary::Entry& entry) const;

    struct WaterMaterialEditorState
    {
        bool windowOpen = false;
        bool dirty = false;
        std::string materialId;
        WaterMaterialData draft;
        char name[96]{};
        char newName[96] = "Water_Material";
    };

    struct PbrMaterialEditorState
    {
        bool windowOpen = false;
        bool dirty = false;
        std::string materialId;
        AssetLibrary::MaterialData draft;
        char name[96]{};
        char newName[96] = "material";
    };

    // Backend-owned graphics state (no Vulkan types here by design).
    ixeditor::graphics::IEditorTextureProvider* m_textureProvider = nullptr;
    bool m_initialized = false;
    bool m_frameActive = false;
    bool m_editorModeActive = false;
    bool m_showDemoWindow = false;
    bool m_applyDefaultDockLayout = false;
    bool m_defaultDockLayoutBuilt = false;
    int m_defaultLayoutTabSelectFrames = 0; // frames left to pick the default layout's front tabs
    int m_startupViewFocusFrames = 3;       // frames left to focus the Scene View after startup
    bool m_logToolsRendered = false;
    bool m_logInspectorRendered = false;
    bool m_debugDisableShadowPass = false;
    bool m_debugDisableWaterReflectionPass = false;
    bool m_debugDisableAssetLibraryDiscovery = false;
    bool m_debugDisableAssetWatcherPoll = false;
    bool m_debugDisableHierarchyIteration = false;
    bool m_debugShowPhysicsColliders = false;
    bool m_debugShowPhysicsContacts = false;
    bool m_debugShowPhysicsBodyCenters = false;
    std::uint32_t m_physicsRaycastLayerMask = ixtreeme::physics::AllPhysicsLayerMask();
    bool m_physicsRaycastHitTriggers = true;
    float m_physicsRaycastDistance = 500.0f;
    std::uint32_t m_physicsOverlapLayerMask = ixtreeme::physics::AllPhysicsLayerMask();
    bool m_physicsOverlapHitTriggers = true;
    float m_physicsOverlapDistance = 20.0f;
    float m_physicsOverlapRadius = 2.0f;
    float m_physicsOverlapBoxHalfExtents[3] = {1.0f, 1.0f, 1.0f};
    float m_physicsOverlapCapsuleRadius = 0.5f;
    float m_physicsOverlapCapsuleHeight = 2.0f;
    float m_physicsTestLinearVelocity[3] = {0.0f, 0.0f, 0.0f};
    float m_physicsTestForce[3] = {0.0f, 20.0f, 0.0f};
    float m_physicsTestImpulse[3] = {0.0f, 5.0f, 0.0f};
    float m_physicsTestAngularImpulse[3] = {0.0f, 1.0f, 0.0f};
    bool m_editSelectedColliderInScene = false;
    PhysicsLayerMatrix m_physicsLayerMatrix = [] {
        PhysicsLayerMatrix matrix{};
        for (std::size_t a = 0; a < static_cast<std::size_t>(ixtreeme::physics::PhysicsLayer::Count); ++a)
        {
            for (std::size_t b = 0; b < static_cast<std::size_t>(ixtreeme::physics::PhysicsLayer::Count); ++b)
            {
                matrix[a][b] = ixtreeme::physics::DefaultLayerCollision(
                    static_cast<ixtreeme::physics::PhysicsLayer>(a),
                    static_cast<ixtreeme::physics::PhysicsLayer>(b));
            }
        }
        return matrix;
    }();
    int m_renderResolutionMode = 0;
    int m_customRenderResolutionWidth = 1920;
    int m_customRenderResolutionHeight = 1080;
    MapEditorGizmoOperation m_gizmoOperation = MapEditorGizmoOperation::Translate;
    bool m_gizmoSnapEnabled = false;
    int m_gizmoSnapIndex = 2;
    float m_gizmoSnapValue = 1.0f;
    MapEditorSettings m_editorSettings;
    EditorPlayModeState m_playModeState;
    // When entering/leaving Play mode, the viewport auto-switches between the Scene
    // View (free-fly editor camera) and the Game view (project Main Camera). Holds the
    // pending ImGui window to focus, applied at the start of the next panel render.
    const char* m_pendingViewFocusWindow = nullptr;
    LightingState m_lightingState;
    SkySettings m_skySettings;
    std::string m_skyStatus;
    DynamicLightEditorState m_dynamicLightState;
    CameraEditorState m_cameraEditorState;
    WaterBodyEditorState m_waterBodyState;
    MeshRendererEditorState m_meshRendererState;
    AnimatorGraphEditorState m_animatorGraphState;
    TerrainEditorState m_terrainState;
    EngineStats m_engineStats;
    std::vector<PhysicsEventEditorState> m_physicsEvents;
    MapEditorCommands m_commands;
    bool m_showLayerVolumes = false;
    float m_serverWorldSpawn[2] = {0, 0};
    std::uint32_t m_serverWorldSpawnVolume = 0;
    struct ScriptPromptBuffer
    {
        ScriptPromptView view;
        std::array<char, 257> text{};
        bool focusPending = true;
    };
    std::vector<ScriptPromptBuffer> m_scriptPrompts;
    std::vector<ScriptPromptAnswer> m_scriptPromptAnswers;
    void RenderScriptPrompts();
    char m_serverWorldId[65] = "editor-world";
    std::uint32_t m_layerGroundVolume = 1;
    float m_layerGroundPoint[2] = {0, 0};
    std::string m_layerAuthoringStatus;
    // Game-script build state (driven by EngineApplication's worker thread; UI reads it).
    enum class ScriptBuildState { Idle, Running, Done };
    ScriptBuildState m_buildState = ScriptBuildState::Idle;
    bool m_buildSucceeded = false;
    bool m_buildOutputPanelOpen = false;
    std::string m_buildLog;
    // "Build Game" state (packaging; shown in the Build Output panel) and its dialog.
    ScriptBuildState m_gameBuildState = ScriptBuildState::Idle;
    bool m_gameBuildSucceeded = false;
    std::string m_gameBuildLog;
    std::filesystem::path m_gameBuildExecutable;
    bool m_openBuildGamePopup = false;
    char m_buildGameName[128]{};
    char m_buildGameOutputDir[512]{};
    std::string m_buildGameStartupScene;  // project-relative
    bool m_buildGameCompileScripts = true;
    bool m_buildGameRunWhenDone = true;
    std::array<MapEditorPaletteSlot, 8> m_paletteSlots{};
    std::vector<std::pair<std::string, WaterMaterialData>> m_waterMaterials;
    std::unordered_map<std::string, std::uint32_t> m_waterMaterialUsageCounts;
    WaterMaterialEditorState m_waterMaterialEditor;
    PbrMaterialEditorState m_pbrMaterialEditor;
    // WaterMaterialsRevision: bumped per unsaved material edit and when the asset library is replaced.
    mutable std::uint64_t m_waterMaterialsTick = 0;
    std::uint64_t m_assetLibraryGeneration = 0;  // bumped for each new library (AssetLibraryRevision)
    mutable bool m_waterMaterialsDrafting = false;
    std::unique_ptr<tree_tool::TreeGeneratorPanel> m_treeGeneratorPanel;
    std::filesystem::path m_engineRoot;
    std::unique_ptr<AssetLibrary> m_assetLibrary;
    // Cached script lists (see CachedScriptAssets), keyed by extension and invalidated by the
    // library revision.
    struct CachedScriptList
    {
        const AssetLibrary* library = nullptr;
        std::uint64_t revision = 0;
        bool valid = false;
        std::vector<AssetLibrary::Entry> entries;
    };
    mutable std::unordered_map<std::string, CachedScriptList> m_scriptAssetCache;
    // ProjectHasNativeScriptSources, cached per library revision and script folder.
    mutable const AssetLibrary* m_nativeSourcesLibrary = nullptr;
    mutable std::uint64_t m_nativeSourcesRevision = 0;
    mutable std::filesystem::path m_nativeSourcesDir;
    mutable bool m_nativeSourcesValid = false;
    mutable bool m_hasNativeSources = false;
    ProjectDialogMode m_projectDialogMode = ProjectDialogMode::None;
    bool m_projectPopupNeedsOpen = false;
    bool m_projectCreateBrowserVisible = false;
    std::filesystem::path m_projectBrowserPath;
    std::string m_projectStatus;
    double m_lastAutoSaveSeconds = 0.0;
    // Script file-change poll (save-to-live).
    double m_lastScriptPollSeconds = 0.0;
    bool m_autoBuildOnSave = true;
    std::unordered_map<std::string, std::filesystem::file_time_type> m_luaMtimes;  // key = Script asset id
    std::unordered_map<std::string, std::filesystem::file_time_type> m_angelMtimes;  // key = Script asset id
    std::unordered_map<std::string, std::filesystem::file_time_type> m_cppMtimes;  // key = abs source path
    int m_lastAutoSaveTitleRemainingSeconds = -1;
    char m_projectParentBuffer[512]{};
    char m_projectNameBuffer[128] = "NewProject";
    char m_projectOpenPathBuffer[512]{};
    char m_projectBrowsePathBuffer[512]{};
    char m_projectBrowseFilterBuffer[128]{};
    bool m_createTerrainModalOpen = false;
    bool m_replaceTerrainConfirmOpen = false;
    float m_createTerrainWidthMeters = 200.0f;
    float m_createTerrainDepthMeters = 200.0f;
    float m_createTerrainCellSizeMeters = 1.0f;
    int m_createTerrainChunkSizeCells = 64;
    std::string m_assetSubpath;
    std::string m_selectedAssetId;
    bool m_assetInspectorSelectionActive = false;
    std::string m_prefabInspectorEditAssetId;
    char m_prefabInspectorNameBuffer[128]{};
    std::vector<PrefabInspectorEditRow> m_prefabInspectorEditRows;
    bool m_prefabInspectorDirty = false;
    std::vector<std::string> m_activeAssetTags;
    std::string m_assetStatus;
    char m_assetSearchBuffer[128]{};
    char m_newAssetFolderName[64]{};
    std::string m_assetNewFolderParent;
    char m_assetRenameBuffer[128]{};
    std::string m_assetRenamePath;
    std::string m_assetRenameAssetId;  // library asset being renamed (empty: a folder or a scene file)
    bool m_assetRenameIsFolder = false;
    std::optional<std::string> m_assetCreateTarget;  // see CreateTargetSubpath()
    std::string m_createMaterialTargetSubpath;       // folder of the pending "Create New Material" popup
    std::string m_assetDeletePath;
    bool m_assetDeleteIsFolder = false;
    bool m_assetOpenNewFolderPopup = false;
    bool m_assetOpenRenamePopup = false;
    bool m_assetOpenDeletePopup = false;
    bool m_assetOpenImportPopup = false;
    bool m_assetOpenCreateMaterialPopup = false;
    char m_newCppScriptName[96] = "MyScript";  // New C++ Script class-name popup
    bool m_openNewCppScriptPopup = false;
    char m_createMaterialName[96] = "material";
    int m_createMaterialShadingMode = -1;
    enum class FbxExportSource
    {
        None,
        Asset,
        MeshEntity
    };
    bool m_fbxExportPopupOpen = false;
    FbxExportSource m_fbxExportSource = FbxExportSource::None;
    std::string m_fbxExportAssetId;
    std::uint32_t m_fbxExportMeshEntityId = 0;
    char m_fbxExportPathBuffer[512]{};
    bool m_fbxExportEmbedTextures = true;
    bool m_fbxExportMaterials = true;
    bool m_fbxExportAnimations = true;
    std::string m_assetImportTargetSubpath;
    std::filesystem::path m_assetImportBrowserPath;
    char m_assetImportPathBuffer[512]{};
    char m_assetImportBrowserPathBuffer[512]{};
    char m_assetImportFilterBuffer[128]{};
    char m_hierarchySearchBuffer[128]{};
    std::uint64_t m_sceneRootEntity = 0;
    std::string m_sceneRootName = "Untitled";
    std::vector<HierarchySceneEntity> m_hierarchyEntities;
    std::vector<std::string> m_attachedScenePaths;
    std::vector<platform::DynamicLibraryHandle> m_loadedGameModules;  // native C++ game-module DLLs
    std::uint64_t m_selectedHierarchyEntity = 0;
    std::uint64_t m_hierarchyRenamingEntity = 0;
    char m_hierarchyRenameBuffer[128]{};
    char m_componentSearchBuffer[128]{};
    bool m_componentRegistryLogged = false;
    bool m_logHierarchyRendered = false;
    std::unordered_map<std::string, AssetPreviewTexture> m_assetPreviewTextures;
    // The asset browser's folder/asset/scene listings and preview paths come from directory scans and
    // path canonicalization; redone every frame they cost ~4 ms with a project open. Cached per asset
    // library revision (the file watcher refreshes the library on outside changes), and re-read every
    // few seconds as a safety net.
    struct AssetBrowserCache
    {
        const AssetLibrary* library = nullptr;
        std::uint64_t revision = 0;
        double builtAt = -1.0;
        int validatedFrame = -1;  // the ImGui frame it was last checked in (at most once a frame)
        std::unordered_map<std::string, std::vector<std::string>> childFolders;
        std::unordered_map<std::string, std::vector<AssetLibrary::Entry>> folderAssets;
        std::optional<std::vector<AssetLibrary::Entry>> sceneAssets;
        std::unordered_map<std::string, std::optional<std::filesystem::path>> previewPaths;
        // Scripts panel: native C++ sources (absolute path, path shown relative to the scripts dir).
        std::optional<std::vector<std::pair<std::filesystem::path, std::string>>> nativeScriptSources;
        // ComparablePath (absolute + weakly_canonical: filesystem calls) of the Hierarchy's scene paths.
        std::unordered_map<std::string, std::string> comparablePaths;
    };
    mutable AssetBrowserCache m_assetBrowserCache;
    bool m_assetBrowserLogged = false;
    std::string m_loggedDragAssetId;
    bool m_gameViewVisible = false;
    struct GameViewRect
    {
        bool valid = false;
        float min[2] = {0.0f, 0.0f};   // window pixels
        float size[2] = {0.0f, 0.0f};
        std::uint32_t extent[2] = {0u, 0u};  // render target pixels
    };
    GameViewRect m_gameViewRect;
    bool m_sceneViewVisible = true;
    std::uint32_t m_sceneViewPanelPixels[2] = {0u, 0u};
    std::uint32_t m_gameViewPanelPixels[2] = {0u, 0u};
    bool m_animatorGraphVisible = false;
    bool m_animatorPanelOpen = true;
    float m_animatorPan[2] = {0.0f, 0.0f};
    float m_animatorZoom = 1.0f;
    std::uint32_t m_animatorSelectedStateId = 0;
    std::string m_animatorCenteredControllerId;  // pan auto-centered once per controller (Stage 7)
    float m_audioVolume[3] = {1.0f, 1.0f, 1.0f};   // audio mixer sliders (Master/Music/SFX)
    bool m_animatorRenameRequested = false;       // Stage 7: state-rename modal trigger
    std::uint32_t m_animatorRenameStateId = 0;
    char m_animatorRenameBuf[128] = {};
    // Stage 7: transition editor working copy (seeded from the snapshot edge, edited in place so the
    // widgets don't snap back; committed via EditTransition with replace-all semantics).
    bool m_animatorEditEdgeValid = false;
    std::uint32_t m_animatorEditEdgeFrom = 0;
    std::uint32_t m_animatorEditEdgeTo = 0;
    bool m_animatorEditHasExitTime = false;
    float m_animatorEditExitTime = 0.0f;
    float m_animatorEditDuration = 0.15f;
    bool m_animatorEditCanSelf = false;
    std::vector<AnimatorGraphCondition> m_animatorEditConditions;
    std::uint32_t m_animatorEditSeededFrom = 0xFFFFFFFEu;  // "not seeded" sentinel
    std::uint32_t m_animatorEditSeededTo = 0xFFFFFFFEu;
    bool m_sceneViewKeyboardFocus = false;
    std::vector<std::array<float, 4>> m_sceneViewSelectionOutline;
    bool m_viewportDropTargetLogged = false;
    ViewportInputDiagnostics m_viewportInputDiagnostics;
    bool m_sceneGizmoVisible = false;
    bool m_sceneGizmoInputActive = false;
    HierarchyEntityType m_sceneGizmoEntityType = HierarchyEntityType::None;
    std::uint32_t m_sceneGizmoEntityId = 0;
    WorldCamera m_sceneGizmoCamera{};
    float m_sceneGizmoPosition[3] = {0.0f, 0.0f, 0.0f};
    float m_sceneGizmoRotation[3] = {0.0f, 0.0f, 0.0f};
    float m_sceneGizmoScale[3] = {1.0f, 1.0f, 1.0f};
    MapEditorGizmoOperation m_sceneGizmoOperation = MapEditorGizmoOperation::Translate;
    bool m_sceneGizmoSnapEnabled = false;
    float m_sceneGizmoSnapValue = 1.0f;
    float m_timeOfDayHours = 12.0f;

    // Editor shell: panels the View menu shows/hides, the Console log and the status bar.
    bool m_hierarchyPanelOpen = true;
    bool m_inspectorPanelOpen = true;
    bool m_sceneSettingsPanelOpen = true;
    bool m_assetBrowserPanelOpen = true;
    bool m_scriptsPanelOpen = true;
    bool m_consolePanelOpen = true;
    bool m_statisticsWindowOpen = false;
    bool m_physicsDebuggerOpen = false;
    bool m_projectSettingsOpen = false;
    bool m_layeredWorldOpen = false;
    bool m_shortcutsWindowOpen = false;
    bool m_openAboutPopup = false;
    bool m_inspectorShowDebugInfo = false;  // engine ids and asset paths in the Inspector
    int m_terrainToolTab = 0;               // Inspector > Terrain: 0 Sculpt, 1 Paint, 2 Settings
    std::vector<ConsoleMessage> m_consoleMessages;
    std::optional<ConsoleMessage> m_statusBarMessage;
    bool m_consoleShowInfo = true;
    bool m_consoleShowWarnings = true;
    bool m_consoleShowErrors = true;
    bool m_consoleScrollToBottom = false;
    std::string m_lastSeenProjectStatus;
    std::string m_lastSeenAssetStatus;
    std::string m_lastSeenLayerStatus;
    ScriptBuildState m_lastSeenBuildState = ScriptBuildState::Idle;
    ScriptBuildState m_lastSeenGameBuildState = ScriptBuildState::Idle;
};
