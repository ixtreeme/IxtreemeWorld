#pragma once

#include "VulkanDevice.h"
#include "WorldCamera.h"
#include "InputEvent.h"
#include "MapEditorTypes.h"

#include <array>
#include <cstdint>
#include <deque>
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
    bool HandleEditorInput(const InputEvent& event);
    void UpdateEditor(VulkanDevice& device,
                      double deltaSeconds,
                      const WorldCamera& camera,
                      uint32_t viewportWidth,
                      uint32_t viewportHeight);
    void Render(VulkanDevice& device, const WorldCamera& camera);
    void ToggleWalkabilityDebug();
    bool IsWalkabilityDebugEnabled() const { return m_walkabilityDebug; }
    void SetMapEditorOpen(bool open);
    void SetMapEditorSettings(const MapEditorSettings& settings);
    void RequestEditorSave();
    void RequestEditorReload();
    void RequestEditorUndo();
    MovementBounds GetMovementBounds() const;
    float SampleHeightAt(float localX, float localZ) const;
    float SampleHeight(WorldVec3 position) const;
    void Destroy();

private:
    static constexpr uint32_t kFramesInFlight = 2;

    struct Vertex
    {
        float position[3];
        float texUv[2];
        float maskUv[2];
    };

    struct UniformBlock
    {
        WorldMat4 mvp;
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
                                Texture& out);
    bool LoadDominantTerrainTexture(VulkanDevice& device, const std::string& mapDirectory);
    bool LoadTileIndices(const std::string& mapDirectory);
    bool BuildTerrainLayers(VulkanDevice& device, const std::string& mapDirectory);
    bool GenerateLayerMask(VulkanDevice& device, TerrainLayer& layer);
    bool CreateDescriptors();
    void UpdateDescriptors();
    bool CreatePipeline(VulkanDevice& device);
    void DestroyPipeline();
    void DestroyBuffer(Buffer& buffer);
    void DestroyTexture(Texture& texture);
    void DestroyTerrainLayers();
    void UpdateUniform(uint32_t frameIndex, const WorldCamera& camera);
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
    std::array<Buffer, kFramesInFlight> m_uniformBuffers{};
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kFramesInFlight> m_descriptorSets{};
    std::vector<VkDescriptorSet> m_layerDescriptorSets;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    Texture m_baseTexture;
    Texture m_fallbackMask;
    Texture m_splatA;
    Texture m_splatB;
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
    std::uint32_t m_editorTextureSlot = 4;
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
