#pragma once

#include "VulkanDevice.h"
#include "WorldCamera.h"
#include "InputEvent.h"
#include "MapEditorTypes.h"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <string>
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
    bool LoadMap(VulkanDevice& device, const std::string& mapDirectory, int32_t serverX, int32_t serverY);
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
                               const std::function<void(const WorldCamera&, VkExtent2D, VkRenderPass)>& renderEntities = {});
    void Render(VulkanDevice& device, const WorldCamera& camera);
    void RenderWater(VulkanDevice& device, const WorldCamera& camera, double timeSeconds);
    void RenderSunShadowMap(VulkanDevice& device, const WorldCamera& camera);
    void ToggleWalkabilityDebug();
    bool IsWalkabilityDebugEnabled() const { return m_walkabilityDebug; }
    void SetMapEditorOpen(bool open);
    void SetMapEditorSettings(const MapEditorSettings& settings);
    void SetLightingState(const LightingState& lighting) { m_lightingState = lighting; }
    void SetWaterConfig(const WaterConfig& water);
    void SetPaletteSlots(const std::array<MapEditorPaletteSlot, 8>& slots);
    const std::array<MapEditorPaletteSlot, 8>& GetPaletteSlots() const { return m_paletteSlots; }
    bool ApplyPaletteSlots(VulkanDevice& device, const std::array<MapEditorPaletteSlot, 8>& slots);
    bool ApplyPaletteSlotChange(VulkanDevice& device, const MapEditorPaletteSlot& slot);
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

    struct Vertex
    {
        float position[3];
        float texUv[2];
        float maskUv[2];
    };

    struct WaterVertex
    {
        float position[3];
        float uv[2];
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

        WorldMat4 mvp;
        float materialTiling[8][4];
        float materialTintNormal[8][4];
        float materialPbr[8][4];
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
        float waterParams1[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float waterParams2[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float waterParams3[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float waterParams4[4] = {0.0f, 0.0f, 0.0f, 0.0f};
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
    bool CreateFallbackTexture(VulkanDevice& device);
    bool CreateFallbackMask(VulkanDevice& device);
    bool CreateFallbackSplatTextures(VulkanDevice& device);
    bool LoadTerrainPalette(VulkanDevice& device, const mx::map::Manifest& manifest, const std::string& mapDirectory);
    bool LoadTerrainPaletteFromPaths(VulkanDevice& device, const std::array<MapEditorPaletteSlot, 8>& slots);
    bool UploadRgbaTexture2D(VulkanDevice& device,
                             const std::string& name,
                             uint32_t width,
                             uint32_t height,
                             const std::vector<std::uint8_t>& pixels,
                             VkSamplerAddressMode addressMode,
                             Texture& out);
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
    bool CreateWaterMesh(VulkanDevice& device);
    bool LoadWaterBodies(VulkanDevice& device, const std::string& mapDirectory);
    bool CreateWaterBodyMesh(VulkanDevice& device, WaterBodyGpu& waterBody);
    bool CreateWaterBodyUniformBuffers(VulkanDevice& device, WaterBodyGpu& waterBody);
    bool AllocateWaterDescriptorSets(const std::array<Buffer, kFramesInFlight>& uniformBuffers,
                                     std::array<VkDescriptorSet, kFramesInFlight>& descriptorSets);
    bool CreateWaterNormalTextures(VulkanDevice& device);
    bool CreateWaterDescriptors();
    void UpdateWaterDescriptors();
    void WriteWaterDescriptorSets(const std::array<Buffer, kFramesInFlight>& uniformBuffers,
                                  const std::array<VkDescriptorSet, kFramesInFlight>& descriptorSets);
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
    WorldCamera ComputeMirrorCamera(const WorldCamera& camera, VkExtent2D extent) const;
    WorldCamera ComputeMirrorCamera(const WorldCamera& camera, VkExtent2D extent, float waterLevelY) const;
    const WaterBodyGpu* FindClosestWaterBody(const WorldCamera& camera, float* outDistanceMeters = nullptr) const;
    void DestroyWaterResources();
    void DestroyWaterBodyResources();
    void DestroyWaterBodyResources(WaterBodyGpu& waterBody);
    void DestroyWaterPipeline();
    void UpdateWaterUniform(uint32_t frameIndex, const WorldCamera& camera, double timeSeconds);
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
    bool SaveChunkHeights(uint32_t chunkX, uint32_t chunkY, uint32_t dirtyTexels);
    bool ReloadCurrentMap(VulkanDevice& device);
    std::string ResolveWritableMapPath(const std::string& relativePath) const;

    VkDevice m_device = VK_NULL_HANDLE;
    client::asset::IAssetReader* m_assets = nullptr;
    Buffer m_vertexBuffer;
    Buffer m_indexBuffer;
    Buffer m_debugVertexBuffer;
    Buffer m_debugIndexBuffer;
    Buffer m_logicVertexBuffer;
    Buffer m_logicIndexBuffer;
    Buffer m_waterVertexBuffer;
    Buffer m_waterIndexBuffer;
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
    uint32_t m_waterIndexCount = 0;
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
    bool m_mapLoaded = false;
    bool m_walkabilityDebug = false;
    bool m_editorRaiseHeld = false;
    bool m_editorLowerHeld = false;
    bool m_mapEditorOpen = false;
    bool m_editorLmbHeld = false;
    bool m_editorStrokeActive = false;
    bool m_editorCtrlHeld = false;
    bool m_editorBrushVisible = false;
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
    float m_editorFlattenTargetCm = 0.0f;
    bool m_editorHasFlattenTarget = false;
    MapEditorTool m_editorTool = MapEditorTool::Raise;
    MapEditorPaintMode m_editorPaintMode = MapEditorPaintMode::Replace;
    std::uint32_t m_editorTextureSlot = 4;
    LightingState m_lightingState;
    WaterConfig m_waterConfig;
    float m_waterMeshLevelY = std::numeric_limits<float>::quiet_NaN();
    float m_reflectionClipWaterLevelY = std::numeric_limits<float>::quiet_NaN();
    double m_latestWaterTimeSeconds = 0.0;
    double m_lastWaterDiagTimeSeconds = -1000.0;
    std::array<MapEditorPaletteSlot, 8> m_paletteSlots{};
    std::string m_loadedMapDirectory;
    int32_t m_loadedServerX = 0;
    int32_t m_loadedServerY = 0;
    std::vector<float> m_heightCmGrid;
    std::vector<std::uint16_t> m_attributes;
    std::vector<std::uint8_t> m_splatABytes;
    std::vector<std::uint8_t> m_splatBBytes;
    std::vector<uint32_t> m_dirtyChunkTexels;

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
