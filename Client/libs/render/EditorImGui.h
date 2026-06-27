#pragma once

#include "AssetLibrary.h"
#include "InputEvent.h"
#include "MapEditorTypes.h"
#include "WorldCamera.h"
#include "platform/dynamic_library.h"
#include "tools/tree/TreeGeneratorPanel.h"

#if defined(_WIN32) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <vulkan/vulkan.h>

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

class VulkanDevice;
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

#if defined(_WIN32)
    bool Create(VulkanDevice& device, HWND hwnd);
    bool HandleWin32Message(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result);
#else
    bool Create(VulkanDevice& device, void* windowHandle);
#endif

    void BeginFrame(bool editorModeActive);
    void Render(VulkanDevice& device);
    void OnRenderPassChanged(VulkanDevice& device);
    void SetSceneViewTexture(VkSampler sampler, VkImageView imageView, VkImageLayout layout, VkExtent2D extent);
    void SetGameViewTexture(VkSampler sampler, VkImageView imageView, VkImageLayout layout, VkExtent2D extent);
    // True when the Game panel was actually visible (active dock tab, not collapsed) last
    // frame. Lets the engine skip the expensive Game-view scene render when it's not shown.
    bool IsGameViewVisible() const { return m_gameViewVisible; }

    // Native C++ game-module DLLs (Unreal-style): load/unload the project's modules, and the in-engine
    // Build pipeline driven from RunGame's frame loop. Called from EngineApplication.
    void LoadProjectGameModules(const std::filesystem::path& projectRoot);
    void UnloadGameModules();
    // Ensure <ProjectRoot>/Scripts exists with an engine-owned CMakeLists.txt (ALWAYS regenerated, the
    // dev never edits it) + a starter Game.cpp (only if no *.cpp). Idempotent; called before every Build.
    void EnsureProjectScriptsScaffold(const std::filesystem::path& projectRoot);
    std::filesystem::path EngineSdkIncludeDir() const;  // resolves <root>/sdk/include (searches up)
    void SetBuildRunning() { m_buildState = ScriptBuildState::Running; }
    void SetBuildResult(bool ok, std::string log)
    {
        m_buildState = ScriptBuildState::Done;
        m_buildSucceeded = ok;
        m_buildLog = std::move(log);
        m_buildOutputPanelOpen = true;
    }

    // Save-to-live iteration: a per-frame (throttled) mtime poll over the project's .lua Script assets
    // and <ProjectRoot>/Scripts/*.cpp,*.h. EngineApplication drains m_pendingScriptChanges each frame to
    // hot-reload Lua (in Play) and auto-build native C++ (in Edit).
    struct ScriptFileChanges
    {
        std::vector<std::string> changedLua;  // Script asset ids whose .lua mtime changed
        bool changedCpp = false;              // any <Project>/Scripts/*.cpp,*.h,*.hpp changed/added/removed
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
    void SetSceneViewKeyboardFocus(bool focused);
    void SetMapEditorSettings(const MapEditorSettings& settings);
    MapEditorSettings GetMapEditorSettings() const { return m_editorSettings; }
    void SetEditorPlayModeState(const EditorPlayModeState& state);
    void SetLightingState(const LightingState& state);
    LightingState GetLightingState() const { return m_lightingState; }
    void SetDynamicLightEditorState(const DynamicLightEditorState& state);
    void SetCameraEditorState(const CameraEditorState& state);
    void SetWaterBodyEditorState(const WaterBodyEditorState& state);
    void SetMeshRendererEditorState(const MeshRendererEditorState& state);
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
    void SetPaletteSlots(const std::array<MapEditorPaletteSlot, 8>& slots);
    void SetEngineRoot(const std::filesystem::path& clientRoot);
    void InitializeAssetLibrary(const std::filesystem::path& clientRoot);
    void InitializeProjectAssetLibrary(const std::filesystem::path& projectRoot, const std::filesystem::path& assetRoot);
    void RefreshAssetLibrary();
    // Generates retargetable .ixclip assets for a loaded rigged model's existing _anim_<i>.ozz
    // sidecars (joint names come from the model's skeleton). Idempotent; no-op if already present.
    void EnsureModelAnimationClips(const std::filesystem::path& modelPath, const std::vector<std::string>& jointNames);
    // Absolute filesystem path of an AnimationClip asset's .ixclip file (empty if not found).
    std::string AnimationClipFilePath(const std::string& clipId) const;
    // Absolute filesystem path of an AnimatorController asset's .controller file (empty if none).
    std::string AnimatorControllerFilePath(const std::string& controllerId) const;
    std::string AudioClipFilePath(const std::string& clipId) const;
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
    enum class AssetBrowserFilter
    {
        All,
        Texture,
        Model,
        Animation,
        AnimationClip,
        AnimatorController,
        Audio,
        Script,
        Material,
        WaterMaterial,
        PhysicsMaterial,
        Scene,
        Prefab
    };

    enum class ProjectDialogMode
    {
        None,
        NoProject,
        Create,
        Open
    };

    struct AssetPreviewTexture
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        VkDescriptorSet descriptor = VK_NULL_HANDLE;
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

    bool CreateDescriptorPool(VulkanDevice& device);
    bool InitVulkanBackend(VulkanDevice& device);
    void RenderEditorPanels();
    void RenderDemoPanels();
    void RenderDockSpace();
    void RenderSceneViewDropTarget();
    void RenderGameViewPanel();
    void RenderAnimatorPanel();
    void RenderSceneViewGizmo(const ImVec2& imageMin, const ImVec2& imageSize);
    void ReleaseSceneViewTextureDescriptor();
    void ReleaseGameViewTextureDescriptor();
    void RenderMenuBar();
    bool SaveProjectAndCurrentScene(bool automatic = false);
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
    void RenderEditorToolbar();
    void RenderBuildOutputPanel();
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
    bool HierarchyPassesSearch(const std::string& name) const;
    void QueueHierarchySelection(const HierarchySceneEntity& entity);
    void QueueHierarchyFocus(const HierarchySceneEntity& entity);
    void StartHierarchyRename(const HierarchySceneEntity& entity);
    void RenderToolsPanel();
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
    bool RenderAttachedEditorComponents(std::vector<EditorAttachedComponent>& components);
    bool RenderTransformComponent(float* position, float* rotation, float* scale);
    bool RenderAxisFloat(const char* axis, float& value, float r, float g, float b, float speed, float minValue, float maxValue);
    void RenderWorldPanel();
    void RenderPerformancePanel();
    void RenderCreateTerrainModal();
    void RenderLightingPanel();
    void RenderDynamicLightsPanel();
    void RenderGizmoControls();
    void RenderWaterSculptToolPanel();
    void RenderHeightmapToolPanel();
    void RenderSplatPaintToolPanel();
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
    void RenderAssetTypeTabs();
    void RenderAssetFolderPanel();
    void RenderAssetFolderNode(const std::string& path, const std::vector<std::string>& folders);
    void RenderAssetBrowserFolderTree();
    void RenderAssetBrowserFolderTreeNode(const std::string& subpath);
    void RenderAssetBrowserContent();
    void RenderAssetBrowserBreadcrumb();
    void RenderAssetBrowserFolderTile(const std::string& subpath, float tileSize);
    void RenderAssetGrid();
    void RenderAssetTile(const AssetLibrary::Entry& entry, float tileSize);
    void RenderAssetTagFilters();
    void DestroyAssetPreviewTextures();
    void DestroyAssetPreviewTexture(AssetPreviewTexture& texture);
    std::optional<std::filesystem::path> AssetPreviewPathFor(const AssetLibrary::Entry& entry) const;
    AssetPreviewTexture* GetAssetPreviewTexture(const AssetLibrary::Entry& entry);
    bool LoadAssetPreviewTexture(const std::filesystem::path& path, AssetPreviewTexture& outTexture);
    void CreateAssetFolder();
    void DeleteAssetFolder();
    void DeleteAsset(const AssetLibrary::Entry& entry);
    void OpenImportAssetDialog(const std::string& targetSubpath);
    void ImportAssetFromPath(const std::filesystem::path& sourcePath,
                             const std::string& targetSubpath,
                             const char* trigger);
    void CreatePbrMaterialAsset();
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
    std::vector<AssetLibrary::Entry> QueryVisibleAssets() const;
    std::vector<std::string> QueryVisibleFolders() const;
    std::vector<std::pair<std::string, std::uint32_t>> QueryVisibleTags() const;
    std::vector<std::string> QueryFilesystemChildFolders(const std::string& subpath) const;
    std::vector<AssetLibrary::Entry> QueryFilesystemAssetsInFolder(const std::string& subpath) const;
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
    bool AssetPassesCurrentFilters(const AssetLibrary::Entry& entry) const;
    bool ActiveAssetCategory(AssetLibrary::Category category) const;
    AssetLibrary::Category FolderCategory() const;
    const char* AssetFilterName() const;
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

    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    uint32_t m_graphicsQueueFamily = UINT32_MAX;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    bool m_initialized = false;
    bool m_vulkanBackendReady = false;
    bool m_frameActive = false;
    bool m_editorModeActive = false;
    bool m_showDemoWindow = false;
    bool m_applyDefaultDockLayout = false;
    bool m_defaultDockLayoutBuilt = false;
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
    DynamicLightEditorState m_dynamicLightState;
    CameraEditorState m_cameraEditorState;
    WaterBodyEditorState m_waterBodyState;
    MeshRendererEditorState m_meshRendererState;
    AnimatorGraphEditorState m_animatorGraphState;
    TerrainEditorState m_terrainState;
    EngineStats m_engineStats;
    std::vector<PhysicsEventEditorState> m_physicsEvents;
    MapEditorCommands m_commands;
    // Game-script build state (driven by EngineApplication's worker thread; UI reads it).
    enum class ScriptBuildState { Idle, Running, Done };
    ScriptBuildState m_buildState = ScriptBuildState::Idle;
    bool m_buildSucceeded = false;
    bool m_buildOutputPanelOpen = false;
    std::string m_buildLog;
    std::array<MapEditorPaletteSlot, 8> m_paletteSlots{};
    std::vector<std::pair<std::string, WaterMaterialData>> m_waterMaterials;
    std::unordered_map<std::string, std::uint32_t> m_waterMaterialUsageCounts;
    WaterMaterialEditorState m_waterMaterialEditor;
    PbrMaterialEditorState m_pbrMaterialEditor;
    std::unique_ptr<tree_tool::TreeGeneratorPanel> m_treeGeneratorPanel;
    std::filesystem::path m_engineRoot;
    std::unique_ptr<AssetLibrary> m_assetLibrary;
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
    std::unordered_map<std::string, std::filesystem::file_time_type> m_cppMtimes;  // key = abs source path
    int m_lastAutoSaveTitleRemainingSeconds = -1;
    char m_projectParentBuffer[512]{};
    char m_projectNameBuffer[128] = "NewProject";
    char m_projectOpenPathBuffer[512]{};
    char m_projectBrowsePathBuffer[512]{};
    char m_projectBrowseFilterBuffer[128]{};
    bool m_waterSculptToolOpen = false;
    bool m_heightmapToolOpen = false;
    bool m_splatPaintToolOpen = false;
    bool m_createTerrainModalOpen = false;
    bool m_replaceTerrainConfirmOpen = false;
    float m_createTerrainWidthMeters = 200.0f;
    float m_createTerrainDepthMeters = 200.0f;
    float m_createTerrainCellSizeMeters = 1.0f;
    int m_createTerrainChunkSizeCells = 64;
    AssetBrowserFilter m_assetFilter = AssetBrowserFilter::All;
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
    bool m_assetRenameIsFolder = false;
    std::string m_assetDeletePath;
    bool m_assetDeleteIsFolder = false;
    bool m_assetOpenNewFolderPopup = false;
    bool m_assetOpenRenamePopup = false;
    bool m_assetOpenDeletePopup = false;
    bool m_assetOpenImportPopup = false;
    bool m_assetOpenCreateMaterialPopup = false;
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
    bool m_assetBrowserLogged = false;
    std::string m_loggedDragAssetId;
    VkSampler m_sceneViewSampler = VK_NULL_HANDLE;
    VkImageView m_sceneViewImageView = VK_NULL_HANDLE;
    VkImageLayout m_sceneViewImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkExtent2D m_sceneViewExtent{};
    VkDescriptorSet m_sceneViewDescriptor = VK_NULL_HANDLE;
    VkSampler m_sceneViewDescriptorSampler = VK_NULL_HANDLE;
    VkImageView m_sceneViewDescriptorImageView = VK_NULL_HANDLE;
    VkImageLayout m_sceneViewDescriptorImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkSampler m_gameViewSampler = VK_NULL_HANDLE;
    VkImageView m_gameViewImageView = VK_NULL_HANDLE;
    VkImageLayout m_gameViewImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkExtent2D m_gameViewExtent{};
    bool m_gameViewVisible = false;
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
    VkDescriptorSet m_gameViewDescriptor = VK_NULL_HANDLE;
    VkSampler m_gameViewDescriptorSampler = VK_NULL_HANDLE;
    VkImageView m_gameViewDescriptorImageView = VK_NULL_HANDLE;
    VkImageLayout m_gameViewDescriptorImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
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
    uint64_t m_lastLoggedFrame = UINT64_MAX;
};
