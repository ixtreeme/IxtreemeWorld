#pragma once

#include "VulkanDevice.h"
#include "WorldCamera.h"
#include "InputEvent.h"
#include "MapEditorTypes.h"

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace client::asset {
class IAssetReader;
}
namespace mx::map {
struct Manifest;
}

class TerrainRenderer
{
public:
    struct PassDrawStats
    {
        bool executed = false;
        bool skipped = false;
        uint32_t drawCalls = 0;
        uint32_t chunksDrawn = 0;
        uint32_t chunksCulled = 0;
    };

    struct FrameDrawStats
    {
        std::array<PassDrawStats, 4> shadowCascades{};
        PassDrawStats waterReflection;
        PassDrawStats terrainMain;
    };

    struct Buffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };

    struct Texture
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t mipLevels = 0;
        std::string name;
    };

    struct TerrainLayer
    {
        uint32_t textureIndex = 0;
        Texture diffuse;
        Texture mask;
        float tilingU = 1.0f;
        float tilingV = 1.0f;
        uint32_t coverage = 0;
    };

    struct MovementBounds
    {
        bool valid = false;
        float minX = 0.0f;
        float maxX = 0.0f;
        float minZ = 0.0f;
        float maxZ = 0.0f;
    };

    bool Create(VulkanDevice& device, client::asset::IAssetReader& assets);
    void SetAdditionalAssetRoots(std::vector<std::filesystem::path> roots);
    bool LoadMap(VulkanDevice& device, const std::string& mapDirectory, int32_t serverX, int32_t serverY);
    bool CreateFlatTerrain(VulkanDevice& device, const TerrainSceneData& terrain);
    void ClearTerrain(VulkanDevice& device);
    bool HasTerrain() const { return m_sceneTerrainActive; }
    bool IsMapLoadedForDiagnostics() const { return m_mapLoaded; }
    TerrainSceneData GetTerrainSceneData() const;
    void SetTerrainSceneData(const TerrainSceneData& terrain);
    bool RecreatePipeline(VulkanDevice& device);
    void SetMainRenderPass(VkRenderPass renderPass);
    void SetWaterRefractionInputs(VkImageView colorView,
                                  VkImageView depthView,
                                  VkSampler sampler,
                                  VkExtent2D extent);
    bool HandleEditorInput(const InputEvent& event);
    void UpdateEditor(VulkanDevice& device,
                      double deltaSeconds,
                      const WorldCamera& camera,
                      uint32_t viewportWidth,
                      uint32_t viewportHeight);
    void RenderWaterReflection(VulkanDevice& device,
                               const WorldCamera& camera,
                               double timeSeconds,
                               const std::function<void(const WorldCamera&, VkExtent2D, VkRenderPass, float)>& renderEntities = {});
    void Render(VulkanDevice& device, const WorldCamera& camera, VkExtent2D targetExtent = {});
    void RenderWater(VulkanDevice& device, const WorldCamera& camera, double timeSeconds, VkExtent2D targetExtent = {});
    void RenderSunShadowMap(VulkanDevice& device, const WorldCamera& camera);
    void ResetFrameDrawStats() { m_frameDrawStats = {}; }
    FrameDrawStats GetFrameDrawStats() const { return m_frameDrawStats; }
    void ToggleWalkabilityDebug();
    bool IsWalkabilityDebugEnabled() const { return m_walkabilityDebug; }
    void SetMapEditorOpen(bool open);
    void SetMapEditorSettings(const MapEditorSettings& settings);
    void SetLightingState(const LightingState& lighting) { m_lightingState = lighting; }
    void SetPerformanceFps(double fps) { m_latestFps = fps; }
    void SetWaterMaterials(const std::vector<std::pair<std::string, WaterMaterialData>>& materials);
    std::vector<WaterBody> GetWaterBodies() const;
    bool SetWaterBodies(VulkanDevice& device, const std::vector<WaterBody>& bodies);
    bool SetSelectedWaterBodyHighlight(VulkanDevice& device, std::uint32_t selectedWaterBodyId);
    void RenderSelectedWaterBodyHighlight(VulkanDevice& device, const WorldCamera& camera, VkExtent2D targetExtent = {});
    void SetWaterSculptBrush(bool visible, float worldX, float worldZ, float radiusMeters, bool addMode);
    void SetPaletteSlots(const std::array<MapEditorPaletteSlot, 8>& slots);
    const std::array<MapEditorPaletteSlot, 8>& GetPaletteSlots() const { return m_paletteSlots; }
    bool ApplyPaletteSlots(VulkanDevice& device, const std::array<MapEditorPaletteSlot, 8>& slots);
    bool ApplyPaletteSlotParams(const MapEditorPaletteSlot& slot);
    bool ApplyPaletteSlotChange(VulkanDevice& device, const MapEditorPaletteSlot& slot);
    bool SetTriplanarSettings(bool enabled, float sharpness, float slopeThreshold, float slopeTransition);
    void RequestEditorSave();
    void RequestEditorReload();
    void RequestEditorUndo();
    MovementBounds GetMovementBounds() const;
    float SampleHeightAt(float localX, float localZ) const;
    float SampleHeight(WorldVec3 position) const;
    void Destroy();

private:
    static constexpr uint32_t kFramesInFlight = 2;
    static constexpr uint32_t kShadowCascadeCount = 4;
    static constexpr uint32_t kShadowResolution = 2048;
    static constexpr uint32_t kMaxTerrainWaterBodies = 8;

    struct Vertex
    {
        float position[3];
        float texUv[2];
        float maskUv[2];
    };

    struct TerrainChunkDraw
    {
        uint32_t indexOffset = 0;
        uint32_t indexCount = 0;
        WorldVec3 worldMin;
        WorldVec3 worldMax;
    };

    struct WaterVertex
    {
        float position[3];
        float uv[2];
        float edgeAlpha;
    };

    struct UniformBlock
    {
        struct PointLightUniform
        {
            float position[4];
            float color[4];
        };

        struct SpotLightUniform
        {
            float position[4];
            float direction[4];
            float color[4];
        };

        struct TerrainWaterBodyUniform
        {
            float bboxMinMax[4] = {0.0f, 0.0f, 0.0f, 0.0f};      // minX, minZ, maxX, maxZ
            float levelModeEnabled[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // levelY, causticMode, enabled, foamEnabled
            float foamParams[4] = {0.0f, 0.0f, 0.0f, 0.0f};      // distance, softness, intensity, scale
            float causticParams[4] = {0.0f, 0.0f, 0.0f, 0.0f};   // intensity, scale, speed, maxDepth
            float edgeParams[4] = {0.0f, 0.0f, 0.0f, 0.0f};      // fade distance, curve, reserved, reserved
        };

        WorldMat4 mvp;
        float materialTiling[8][4];
        float materialTintNormal[8][4];
        float materialPbr[8][4];
        float terrainMaterialParams[4] = {0.0f, 4.0f, 0.18f, 0.20f}; // triplanar enabled, sharpness, slope threshold, slope transition
        float cameraPos[4];
        float sunDir[4];
        float sunColor[4];
        float ambientColor[4];
        WorldMat4 cascadeViewProj[kShadowCascadeCount];
        float cascadeSplits[4] = {5.0f, 15.0f, 50.0f, 200.0f};
        float shadowParams[4] = {0.0f, static_cast<float>(kShadowResolution), 0.0015f, 0.0f};
        std::int32_t numPointLights = 0;
        std::int32_t numSpotLights = 0;
        float lightPadding[2] = {0.0f, 0.0f};
        float waterGlobalParams[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // time, activeCount, truncatedCount, reserved
        TerrainWaterBodyUniform terrainWaterBodies[kMaxTerrainWaterBodies]{};
        PointLightUniform pointLights[kMaxDynamicPointLights]{};
        SpotLightUniform spotLights[kMaxDynamicSpotLights]{};
    };

    struct WaterUniformBlock
    {
        WorldMat4 mvp;
        float cameraPos[4];
        float sunDir[4];
        float sunColor[4];
        float ambientColor[4];
        float baseColor[4];
        float reflectionColor[4];
        float waveParams1[4];
        float waveParams2[4];
        float levelTimeEnabled[4];
        float reflectionParams[4];
        float refractionParams[4];
        float shallowColor[4];
        float deepColor[4];
        float depthParams[4];
        float foamParams[4];
        float foamDepthParams[4];
        float causticParams[4];
        float cameraNearFar[4];
        float textureParams[4]; // use normal A, use normal B, use diffuse, normal tiling
        float textureScroll[4]; // scroll A xy, scroll B xy
    };

    struct WaterReflectionResources
    {
        VkImage colorImage = VK_NULL_HANDLE;
        VkDeviceMemory colorMemory = VK_NULL_HANDLE;
        VkImageView colorView = VK_NULL_HANDLE;
        VkImage depthImage = VK_NULL_HANDLE;
        VkDeviceMemory depthMemory = VK_NULL_HANDLE;
        VkImageView depthView = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        VkRenderPass renderPass = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        VkFormat colorFormat = VK_FORMAT_UNDEFINED;
        VkFormat depthFormat = VK_FORMAT_UNDEFINED;
        uint32_t width = 0;
        uint32_t height = 0;
        WaterConfig::ReflectionQuality quality = WaterConfig::ReflectionQuality::Half;
        VkImageLayout colorLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    struct WaterBodyGpu
    {
        WaterBody body;
        Buffer vertexBuffer;
        Buffer indexBuffer;
        std::array<Buffer, kFramesInFlight> uniformBuffers{};
        std::array<VkDescriptorSet, kFramesInFlight> descriptorSets{};
        uint32_t indexCount = 0;
    };

    struct WaterMaterialTextureSet
    {
        Texture normalA;
        Texture normalB;
        Texture diffuse;
        std::string normalAPath;
        std::string normalBPath;
        std::string diffusePath;
    };

    struct DebugDrawRange
    {
        uint32_t indexOffset = 0;
        uint32_t indexCount = 0;
        float color[3] = {1.0f, 1.0f, 1.0f};
    };

    bool CreateBuffers(VulkanDevice& device);
    bool EnsureUniformBuffers(VulkanDevice& device);
    bool CreateFlatBuffers(VulkanDevice& device);
    bool CreateMapBuffers(VulkanDevice& device, const std::string& mapDirectory, int32_t serverX, int32_t serverY);
    void BuildTerrainChunkDraws(const std::vector<Vertex>& vertices, std::vector<uint32_t>& indices);
    bool CreateFallbackTexture(VulkanDevice& device);
    bool CreateFallbackMask(VulkanDevice& device);
    bool CreateFallbackSplatTextures(VulkanDevice& device);
    bool CreateSceneSplatTextures(VulkanDevice& device);
    bool LoadTerrainPalette(VulkanDevice& device, const mx::map::Manifest& manifest, const std::string& mapDirectory);
    bool LoadTerrainPaletteFromPaths(VulkanDevice& device, const std::array<MapEditorPaletteSlot, 8>& slots);
    bool UploadRgbaTexture2D(VulkanDevice& device,
                             const std::string& name,
                             uint32_t width,
                             uint32_t height,
                             const std::vector<std::uint8_t>& pixels,
                             VkSamplerAddressMode addressMode,
                             Texture& out,
                             VkFormat format = VK_FORMAT_R8G8B8A8_UNORM);
    bool UpdateRgbaTexture2D(VulkanDevice& device, Texture& texture, const std::vector<std::uint8_t>& pixels);
    bool UploadRgbaTextureArray(VulkanDevice& device,
                                const std::string& name,
                                uint32_t width,
                                uint32_t height,
                                uint32_t layers,
                                const std::vector<std::uint8_t>& pixels,
                                VkFormat format,
                                Texture& out);
    bool UploadR8TextureArray(VulkanDevice& device,
                              const std::string& name,
                              uint32_t width,
                              uint32_t height,
                              uint32_t layers,
                              const std::vector<std::uint8_t>& pixels,
                              Texture& out);
    bool LoadDominantTerrainTexture(VulkanDevice& device, const std::string& mapDirectory);
    bool LoadTileIndices(const std::string& mapDirectory);
    bool BuildTerrainLayers(VulkanDevice& device, const std::string& mapDirectory);
    bool GenerateLayerMask(VulkanDevice& device, TerrainLayer& layer);
    bool CreateDescriptors();
    void UpdateDescriptors();
    bool CreatePipeline(VulkanDevice& device);
    void DestroyPipeline();
    bool CreateWaterResources(VulkanDevice& device);
    bool LoadWaterBodies(VulkanDevice& device, const std::string& mapDirectory);
    bool CreateWaterBodyMesh(VulkanDevice& device, WaterBodyGpu& waterBody);
    bool CreateWaterBodyUniformBuffers(VulkanDevice& device, WaterBodyGpu& waterBody);
    bool RebuildSelectedWaterBodyHighlight(VulkanDevice& device, const WaterBody* body);
    bool AllocateWaterDescriptorSets(const std::array<Buffer, kFramesInFlight>& uniformBuffers,
                                     std::array<VkDescriptorSet, kFramesInFlight>& descriptorSets);
    bool CreateWaterNormalTextures(VulkanDevice& device);
    bool CreateWaterDescriptors();
    void UpdateWaterDescriptors();
    void WriteWaterDescriptorSets(const std::array<Buffer, kFramesInFlight>& uniformBuffers,
                                  const std::array<VkDescriptorSet, kFramesInFlight>& descriptorSets,
                                  const WaterMaterialTextureSet* materialTextures = nullptr);
    bool CreateWaterPipeline(VulkanDevice& device);
    bool CreateOrRecreateWaterReflectionResources(VulkanDevice& device, bool force);
    bool CreateOrRecreateWaterReflectionResources(VulkanDevice& device,
                                                  bool force,
                                                  WaterConfig::ReflectionQuality quality);
    bool CreateWaterReflectionPipeline(VulkanDevice& device);
    void DestroyWaterReflectionResources();
    void DestroyWaterReflectionPipeline();
    void DrawTerrainSurface(VkCommandBuffer cmd,
                            uint32_t frameIndex,
                            VkExtent2D extent,
                            VkPipeline pipeline,
                            VkPipelineLayout pipelineLayout,
                            bool includeDebug);
    WorldCamera ComputeMirrorCamera(const WorldCamera& camera, VkExtent2D extent, float waterLevelY) const;
    const WaterBodyGpu* FindClosestWaterBody(const WorldCamera& camera, float* outDistanceMeters = nullptr) const;
    const WaterConfig& ResolveWaterConfig(const WaterBody& body) const;
    void DestroyWaterResources();
    void DestroyWaterBodyResources();
    void DestroyWaterBodyResources(WaterBodyGpu& waterBody);
    void DestroyWaterMaterialTextureCache();
    bool LoadWaterMaterialTextureSet(VulkanDevice& device,
                                     const std::string& id,
                                     const WaterMaterialData& material,
                                     WaterMaterialTextureSet& out);
    const WaterMaterialTextureSet* ResolveWaterMaterialTextures(const WaterBody& body) const;
    void DestroyWaterPipeline();
    void UpdateWaterBodyUniform(uint32_t frameIndex,
                                const WorldCamera& camera,
                                double timeSeconds,
                                WaterBodyGpu& waterBody,
                                bool reflectionTarget);
    WaterUniformBlock BuildWaterUniform(const WorldCamera& camera,
                                        double timeSeconds,
                                        const WaterConfig& water,
                                        float waterLevelY,
                                        bool reflectionTarget) const;
    void UploadWaterUniform(Buffer& buffer, const WaterUniformBlock& uniform);
    void DestroyBuffer(Buffer& buffer);
    void DestroyTexture(Texture& texture);
    void DestroyTerrainLayers();
    void UpdateUniform(uint32_t frameIndex, const WorldCamera& camera, bool reflectionPass = false);
    bool CreateShadowResources(VulkanDevice& device);
    bool CreateShadowPipeline();
    void DestroyShadowResources();
    void DestroyShadowPipeline();
    void UpdateShadowCascades(const WorldCamera& camera);
    void LoadEditorConfig();
    void ApplyLegacyHeightBrush(VulkanDevice& device, float sign, double deltaSeconds);
    void ApplyEditorBrush(VulkanDevice& device, double deltaSeconds);
    bool RaycastEditorBrush(const WorldCamera& camera, uint32_t viewportWidth, uint32_t viewportHeight);
    void BeginEditorStroke();
    void EndEditorStroke();
    void RecordHeightUndo(size_t index);
    void RecordSplatUndo(size_t index);
    void UndoLastEditorStroke(VulkanDevice& device);
    void MarkHeightDirty(size_t heightIndex);
    void MarkSplatDirty(size_t splatIndex);
    bool RefreshSplatTextures(VulkanDevice& device);
    bool SaveDirtyChunks();
    bool SaveWorldPalette() const;
    bool SaveWaterBodies() const;
    bool SaveChunkHeights(uint32_t chunkX, uint32_t chunkY, uint32_t dirtyTexels);
    bool ReloadCurrentMap(VulkanDevice& device);
    std::string ResolveWritableMapPath(const std::string& relativePath) const;

    VkDevice m_device = VK_NULL_HANDLE;
    VulkanDevice* m_deviceOwner = nullptr;
    client::asset::IAssetReader* m_assets = nullptr;
    std::unordered_map<std::string, WaterMaterialData> m_waterMaterials;
    std::unordered_map<std::string, WaterMaterialTextureSet> m_waterMaterialTextures;
    std::string m_waterMaterialTextureSignature;
    std::string m_waterMaterialEdgeSignature;
    std::vector<std::filesystem::path> m_additionalAssetRoots;
    WaterMaterialData m_defaultWaterMaterial;
    Buffer m_vertexBuffer;
    Buffer m_indexBuffer;
    Buffer m_debugVertexBuffer;
    Buffer m_debugIndexBuffer;
    Buffer m_logicVertexBuffer;
    Buffer m_logicIndexBuffer;
    Buffer m_selectedWaterBodyVertexBuffer;
    Buffer m_selectedWaterBodyIndexBuffer;
    std::array<Buffer, kFramesInFlight> m_uniformBuffers{};
    std::array<Buffer, kFramesInFlight> m_waterUniformBuffers{};
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kFramesInFlight> m_descriptorSets{};
    std::vector<VkDescriptorSet> m_layerDescriptorSets;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_waterDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_waterDescriptorPool = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kFramesInFlight> m_waterDescriptorSets{};
    VkPipelineLayout m_waterPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_waterPipeline = VK_NULL_HANDLE;
    VkRenderPass m_mainRenderPass = VK_NULL_HANDLE;
    VkImageView m_waterSceneColorView = VK_NULL_HANDLE;
    VkImageView m_waterSceneDepthView = VK_NULL_HANDLE;
    VkSampler m_waterSceneSampler = VK_NULL_HANDLE;
    VkExtent2D m_waterSceneExtent{};
    WaterReflectionResources m_waterReflection;
    VkPipelineLayout m_waterReflectionPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_waterReflectionPipeline = VK_NULL_HANDLE;
    bool m_waterReflectionDescriptorsDirty = true;
    VkImage m_shadowImage = VK_NULL_HANDLE;
    VkDeviceMemory m_shadowMemory = VK_NULL_HANDLE;
    VkImageView m_shadowArrayView = VK_NULL_HANDLE;
    std::array<VkImageView, kShadowCascadeCount> m_shadowLayerViews{};
    VkSampler m_shadowSampler = VK_NULL_HANDLE;
    VkRenderPass m_shadowRenderPass = VK_NULL_HANDLE;
    std::array<VkFramebuffer, kShadowCascadeCount> m_shadowFramebuffers{};
    VkPipelineLayout m_shadowPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_shadowPipeline = VK_NULL_HANDLE;
    VkImageLayout m_shadowLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    std::array<WorldMat4, kShadowCascadeCount> m_shadowCascadeViewProj{};
    float m_shadowCascadeSplits[kShadowCascadeCount] = {5.0f, 15.0f, 50.0f, 200.0f};
    Texture m_baseTexture;
    Texture m_normalTexture;
    Texture m_aoTexture;
    Texture m_roughnessTexture;
    Texture m_metallicTexture;
    Texture m_heightTexture;
    Texture m_fallbackMask;
    Texture m_splatA;
    Texture m_splatB;
    Texture m_waterNormalSmall;
    Texture m_waterNormalLarge;
    std::vector<TerrainLayer> m_layers;
    std::vector<uint8_t> m_tileIndices;
    uint32_t m_tileGridWidth = 0;
    uint32_t m_tileGridHeight = 0;
    uint32_t m_indexCount = 0;
    uint32_t m_debugIndexCount = 0;
    uint32_t m_spawnDebugIndexOffset = 0;
    uint32_t m_spawnDebugIndexCount = 0;
    uint32_t m_logicDebugIndexOffset = 0;
    uint32_t m_logicDebugIndexCount = 0;
    uint32_t m_selectedWaterBodyIndexCount = 0;
    std::uint32_t m_selectedWaterBodyId = 0;
    float m_selectedWaterBodySignature[5] = {};
    std::vector<WaterBodyGpu> m_waterBodies;
    std::vector<DebugDrawRange> m_zoneFillDebugRanges;
    std::vector<DebugDrawRange> m_zoneBorderDebugRanges;
    std::vector<DebugDrawRange> m_zoneLabelDebugRanges;
    uint32_t m_heightGridWidth = 0;
    uint32_t m_heightGridHeight = 0;
    uint32_t m_splatWidth = 0;
    uint32_t m_splatHeight = 0;
    uint32_t m_chunkSplatWidth = 0;
    uint32_t m_chunkSplatHeight = 0;
    uint32_t m_mapSizeX = 0;
    uint32_t m_mapSizeY = 0;
    uint32_t m_chunkSizeCells = 0;
    float m_cellScaleMeters = 2.0f;
    float m_spawnLocalXcm = 0.0f;
    float m_spawnLocalYcm = 0.0f;
    float m_spawnHeightCm = 0.0f;
    float m_flatTerrainWidthMeters = 100.0f;
    float m_flatTerrainDepthMeters = 100.0f;
    bool m_sceneTerrainActive = false;
    TerrainSceneData m_sceneTerrain;
    bool m_mapLoaded = false;
    bool m_walkabilityDebug = false;
    bool m_editorRaiseHeld = false;
    bool m_editorLowerHeld = false;
    bool m_mapEditorOpen = false;
    bool m_editorLmbHeld = false;
    bool m_editorStrokeActive = false;
    bool m_editorCtrlHeld = false;
    bool m_editorBrushVisible = false;
    bool m_editorTerrainToolActive = false;
    bool m_waterSculptBrushVisible = false;
    bool m_waterSculptBrushAddMode = true;
    bool m_editorSplatGpuDirty = false;
    bool m_editorSaveRequested = false;
    bool m_editorReloadRequested = false;
    bool m_editorUndoRequested = false;
    int m_editorCursorX = 0;
    int m_editorCursorY = 0;
    float m_editorBrushRadiusMeters = 5.0f;
    float m_editorBrushStrength = 1.0f;
    float m_editorBrushLocalX = 0.0f;
    float m_editorBrushLocalZ = 0.0f;
    float m_waterSculptBrushWorldX = 0.0f;
    float m_waterSculptBrushWorldZ = 0.0f;
    float m_waterSculptBrushRadiusMeters = 3.0f;
    float m_editorFlattenTargetCm = 0.0f;
    bool m_editorHasFlattenTarget = false;
    MapEditorTool m_editorTool = MapEditorTool::Raise;
    MapEditorToolMode m_editorToolMode = MapEditorToolMode::None;
    MapEditorPaintMode m_editorPaintMode = MapEditorPaintMode::Replace;
    std::uint32_t m_editorTextureSlot = 4;
    LightingState m_lightingState;
    float m_reflectionClipWaterLevelY = std::numeric_limits<float>::quiet_NaN();
    double m_latestWaterTimeSeconds = 0.0;
    double m_latestFps = 0.0;
    double m_lastWaterDiagTimeSeconds = -1000.0;
    std::array<MapEditorPaletteSlot, 8> m_paletteSlots{};
    bool m_materialParamsDirty = false;
    bool m_triplanarParamsDirty = false;
    bool m_triPerfStaticLogged = false;
    bool m_triPerfPaletteLogged = false;
    bool m_triPerfMainPassLogged = false;
    bool m_triPerfReflectionPassLogged = false;
    bool m_triPerfShadowPassLogged = false;
    std::string m_loadedMapDirectory;
    int32_t m_loadedServerX = 0;
    int32_t m_loadedServerY = 0;
    std::vector<float> m_heightCmGrid;
    std::vector<std::uint16_t> m_attributes;
    std::vector<std::uint8_t> m_splatABytes;
    std::vector<std::uint8_t> m_splatBBytes;
    std::vector<uint32_t> m_dirtyChunkTexels;
    std::vector<TerrainChunkDraw> m_terrainChunks;
    std::vector<uint32_t> m_visibleTerrainChunksScratch;
    FrameDrawStats m_frameDrawStats{};

    struct HeightUndo
    {
        size_t index = 0;
        float oldCm = 0.0f;
    };

    struct SplatUndo
    {
        size_t index = 0;
        std::array<std::uint8_t, 8> oldWeights{};
    };

    struct EditorUndoEntry
    {
        std::vector<HeightUndo> heights;
        std::vector<SplatUndo> splats;
    };

    EditorUndoEntry m_currentUndo;
    std::vector<std::uint8_t> m_heightUndoRecorded;
    std::vector<std::uint8_t> m_splatUndoRecorded;
    std::deque<EditorUndoEntry> m_undoStack;
};
