#pragma once

#include "AssetLibrary.h"
#include "InputEvent.h"
#include "MapEditorTypes.h"

#if defined(_WIN32) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <vulkan/vulkan.h>

#include <cstdint>
#include <array>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

class VulkanDevice;

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
    bool WantsInputCapture(const InputEvent& event) const;
    bool IsSceneViewInputTarget(const InputEvent& event) const;
    InputEvent MapInputToSceneView(const InputEvent& event) const;
    void SetSceneViewKeyboardFocus(bool focused);
    void SetMapEditorSettings(const MapEditorSettings& settings);
    MapEditorSettings GetMapEditorSettings() const { return m_editorSettings; }
    void SetEditorPlayModeState(const EditorPlayModeState& state);
    void SetLightingState(const LightingState& state);
    LightingState GetLightingState() const { return m_lightingState; }
    void SetDynamicLightEditorState(const DynamicLightEditorState& state);
    void SetWaterBodyEditorState(const WaterBodyEditorState& state);
    void SetMeshRendererEditorState(const MeshRendererEditorState& state);
    void SetTerrainEditorState(const TerrainEditorState& state);
    void SetEngineStats(const EngineStats& stats);
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
        Material,
        WaterMaterial,
        Scene
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

    bool CreateDescriptorPool(VulkanDevice& device);
    bool InitVulkanBackend(VulkanDevice& device);
    void RenderEditorPanels();
    void RenderDemoPanels();
    void RenderDockSpace();
    void RenderSceneViewDropTarget();
    void ReleaseSceneViewTextureDescriptor();
    void RenderMenuBar();
    void RenderProjectModal();
    void RenderProjectBrowser(bool pickProjectFile);
    void OpenProjectDialog(ProjectDialogMode mode);
    bool NavigateProjectBrowser(const std::filesystem::path& path, bool createMissing = false);
    void ActivateCurrentProject();
    bool LoadProjectStartupScene();
    bool CreateDefaultProjectScene();
    void CreateProjectFromDialog();
    void OpenProjectFromDialog(const std::filesystem::path& manifestPath);
    void RenderEditorToolbar();
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
    void RenderSelectedMeshRendererInspector();
    void RenderAddComponentMenu();
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
    void RenderAssetBrowser();
    void RenderAssetBrowserToolbar();
    void RenderAssetTypeTabs();
    void RenderAssetFolderPanel();
    void RenderAssetFolderNode(const std::string& path, const std::vector<std::string>& folders);
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
    void ImportAssetWithDialog(AssetLibrary::Category category);
    void CreatePbrMaterialAsset();
    void CreateWaterMaterialAsset();
    bool CreateWaterMaterialAsset(const std::string& displayName, AssetLibrary::Entry& outEntry);
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
    bool AssetPassesCurrentFilters(const AssetLibrary::Entry& entry) const;
    bool ActiveAssetCategory(AssetLibrary::Category category) const;
    AssetLibrary::Category FolderCategory() const;
    const char* AssetFilterName() const;
    void ApplyTimeOfDayPreset(float hour);
    void MarkSelectedWaterBodyChanged();
    void MarkSelectedLightChanged();
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
    MapEditorGizmoOperation m_gizmoOperation = MapEditorGizmoOperation::Translate;
    bool m_gizmoSnapEnabled = false;
    int m_gizmoSnapIndex = 2;
    float m_gizmoSnapValue = 1.0f;
    MapEditorSettings m_editorSettings;
    EditorPlayModeState m_playModeState;
    LightingState m_lightingState;
    DynamicLightEditorState m_dynamicLightState;
    WaterBodyEditorState m_waterBodyState;
    MeshRendererEditorState m_meshRendererState;
    TerrainEditorState m_terrainState;
    EngineStats m_engineStats;
    MapEditorCommands m_commands;
    std::array<MapEditorPaletteSlot, 8> m_paletteSlots{};
    std::vector<std::pair<std::string, WaterMaterialData>> m_waterMaterials;
    std::unordered_map<std::string, std::uint32_t> m_waterMaterialUsageCounts;
    WaterMaterialEditorState m_waterMaterialEditor;
    PbrMaterialEditorState m_pbrMaterialEditor;
    std::filesystem::path m_engineRoot;
    std::unique_ptr<AssetLibrary> m_assetLibrary;
    ProjectDialogMode m_projectDialogMode = ProjectDialogMode::None;
    bool m_projectPopupNeedsOpen = false;
    bool m_projectCreateBrowserVisible = false;
    std::filesystem::path m_projectBrowserPath;
    std::string m_projectStatus;
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
    std::vector<std::string> m_activeAssetTags;
    std::string m_assetStatus;
    char m_assetSearchBuffer[128]{};
    char m_newAssetFolderName[64]{};
    char m_hierarchySearchBuffer[128]{};
    std::uint64_t m_sceneRootEntity = 0;
    std::string m_sceneRootName = "Untitled";
    std::vector<HierarchySceneEntity> m_hierarchyEntities;
    std::vector<std::string> m_attachedScenePaths;
    std::uint64_t m_selectedHierarchyEntity = 0;
    std::uint64_t m_hierarchyRenamingEntity = 0;
    char m_hierarchyRenameBuffer[128]{};
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
    bool m_sceneViewKeyboardFocus = false;
    bool m_viewportDropTargetLogged = false;
    ViewportInputDiagnostics m_viewportInputDiagnostics;
    float m_timeOfDayHours = 12.0f;
    uint64_t m_lastLoggedFrame = UINT64_MAX;
};
