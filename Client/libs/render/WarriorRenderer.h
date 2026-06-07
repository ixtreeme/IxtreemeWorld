#pragma once

#include "VulkanDevice.h"
#include "WorldCamera.h"
#include "MapEditorTypes.h"

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace client::asset {
class IAssetReader;
}

class WarriorRenderer
{
public:
    WarriorRenderer();
    ~WarriorRenderer();

    enum class MotionState : uint32_t
    {
        Idle = 0,
        Walk = 1,
        Run = 2
    };

    struct Buffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };

    bool Create(VulkanDevice& device, client::asset::IAssetReader& assets,
        const std::string& modelPath);
    bool RecreatePipeline(VulkanDevice& device);
    void SetMainRenderPass(VkRenderPass renderPass);
    void Skin(VulkanDevice& device, double timeSeconds);
    void SkinInstance(VulkanDevice& device, uint32_t skinSlot, MotionState state, float animTimeSeconds);
    void Render(VulkanDevice& device, double timeSeconds);
    void RenderInWorld(VulkanDevice& device,
        double timeSeconds,
        const WorldCamera& camera,
        WorldVec3 position,
        float yawRadians,
        uint32_t skinSlot = 0,
        std::array<float, 4> tint = {1.0f, 1.0f, 1.0f, 1.0f});
    void RenderInWorldReflection(VulkanDevice& device,
        const WorldCamera& camera,
        VkExtent2D extent,
        VkRenderPass renderPass,
        float waterLevelY,
        WorldVec3 position,
        float yawRadians,
        uint32_t skinSlot = 0,
        std::array<float, 4> tint = {1.0f, 1.0f, 1.0f, 1.0f});
    void SetLightingState(const LightingState& lighting) { m_lightingState = lighting; }
    void SetMotionState(MotionState state);
    float GroundOffsetY() const;
    static constexpr uint32_t MaxSkinSlots() { return kSkinSlots; }
    void Destroy();

    struct Vertex
    {
        float position[3];
        float normal[3];
        float uv[2];
    };

    struct SourceVertex
    {
        float position[3];
        uint8_t boneWeights[4];
        uint8_t boneIndices[4];
        float normal[3];
        float uv[2];
    };
    static_assert(sizeof(SourceVertex) == 40, "Skinned source vertex layout must stay 40 bytes");

private:
    static constexpr uint32_t kFramesInFlight = 2;
    static constexpr uint32_t kTextureCount = 2;
    static constexpr uint32_t kUniformSlots = 32;
    static constexpr uint32_t kSkinSlots = 32;

    struct RawMesh
    {
        uint32_t meshIndex = 0;
        uint32_t baseVertex = 0;
        uint32_t vertexCount = 0;
        std::vector<SourceVertex> sourceVertices;
    };

    struct RestVertexGpu
    {
        float position[4];
        uint32_t packedWeights = 0;
        uint32_t packedBones = 0;
        uint32_t pad0 = 0;
        uint32_t pad1 = 0;
        float normal[4];
        float uv[4];
    };
    static_assert(sizeof(RestVertexGpu) == 64, "Compute rest vertex SSBO stride must stay 64 bytes");

    struct SkinPushConstants
    {
        uint32_t vertexCount = 0;
        uint32_t boneCount = 0;
    };

    struct MeshBounds
    {
        float min[3]{};
        float max[3]{};
        float center[3]{};
        float fitScale = 1.0f;
    };

    struct MeshDraw
    {
        uint32_t firstIndex = 0;
        uint32_t indexCount = 0;
        uint32_t textureIndex = 0;
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

    struct OzzRuntime;

    bool LoadGltfMesh(const std::string& modelPath);
    bool LoadOzzPose(const std::string& dir);
    bool CreateBuffers(VulkanDevice& device);
    bool CreateTextures(VulkanDevice& device, const std::string& modelPath);
    bool CreateDescriptors();
    bool CreatePipeline(VulkanDevice& device);
    bool CreateReflectionPipeline(VulkanDevice& device, VkRenderPass renderPass);
    bool CreateComputeResources(VulkanDevice& device);
    bool CreateComputeDescriptors();
    bool CreateComputePipeline();
    bool VerifyComputeSkin(VulkanDevice& device);
    bool SkinPose(float animTimeSeconds, bool updateBounds, bool logSamples);
    bool UploadBonePalette(MotionState state, float animTimeSeconds, uint32_t frameIndex, uint32_t skinSlot);
    bool UploadBonePalette(float animTimeSeconds, uint32_t frameIndex);
    void DispatchSkin(VkCommandBuffer cmd, uint32_t frameIndex, uint32_t skinSlot);
    void DispatchSkin(VkCommandBuffer cmd, uint32_t frameIndex);
    void DestroyComputeResources();
    void DestroyAnimation();
    void DestroyPipeline();
    void DestroyReflectionPipeline();
    void DestroyBuffer(Buffer& buffer);
    void DestroyTexture(Texture& texture);
    void UpdateUniform(uint32_t frameIndex, uint32_t uniformSlot, double timeSeconds, float aspect);
    void UpdateWorldUniform(uint32_t frameIndex,
        uint32_t uniformSlot,
        const WorldCamera& camera,
        WorldVec3 position,
        float yawRadians,
        double timeSeconds,
        std::array<float, 4> tint,
        bool reflectionPass = false,
        float waterLevelY = 0.0f);

    VkDevice m_device = VK_NULL_HANDLE;
    client::asset::IAssetReader* m_assets = nullptr;
    Buffer m_indexBuffer;
    std::array<std::array<Buffer, kUniformSlots>, kFramesInFlight> m_uniformBuffers{};
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    std::array<std::array<std::array<VkDescriptorSet, kTextureCount>, kUniformSlots>, kFramesInFlight> m_descriptorSets{};
    VkDescriptorSetLayout m_computeDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_computeDescriptorPool = VK_NULL_HANDLE;
    std::array<std::array<VkDescriptorSet, kSkinSlots>, kFramesInFlight> m_computeDescriptorSets{};
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkRenderPass m_mainRenderPass = VK_NULL_HANDLE;
    VkPipeline m_reflectionPipeline = VK_NULL_HANDLE;
    VkRenderPass m_reflectionRenderPass = VK_NULL_HANDLE;
    VkPipelineLayout m_computePipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_computePipeline = VK_NULL_HANDLE;
    std::vector<Vertex> m_vertices;
    std::vector<uint32_t> m_indices;
    std::vector<MeshDraw> m_draws;
    std::vector<RawMesh> m_rawMeshes;
    std::vector<RestVertexGpu> m_restVerticesGpu;
    std::array<Texture, kTextureCount> m_textures{};
    Buffer m_restVertexBuffer;
    std::array<std::array<Buffer, kSkinSlots>, kFramesInFlight> m_bonePaletteBuffers{};
    std::array<std::array<Buffer, kSkinSlots>, kFramesInFlight> m_skinnedOutputBuffers{};
    uint32_t m_indexCount = 0;
    MeshBounds m_bounds{};
    std::unique_ptr<OzzRuntime> m_ozz;
    std::vector<std::array<float, 16>> m_inverseBindMatrices;
    std::vector<std::array<float, 16>> m_bonePaletteCpu;
    uint32_t m_boneCount = 0;
    MotionState m_motionState = MotionState::Idle;
    LightingState m_lightingState;
    uint32_t m_worldRenderFrameIndex = std::numeric_limits<uint32_t>::max();
    uint32_t m_worldUniformCursor = 0;
    double m_lastAnimationLogTime = -1000.0;
};
