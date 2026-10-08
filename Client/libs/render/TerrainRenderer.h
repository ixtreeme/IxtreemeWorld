#pragma once

// TerrainRenderer — Phase-3F IXRHI-native terrain + water rendering.
//
// ZERO Vk* dependency: mesh/uniform buffers, height/splat/material/water
// textures (including mipmapped arrays), samplers (incl. the shadow
// comparison sampler), bind groups, main/reflection/water/shadow pipelines
// and shadow/reflection render targets are IXRHI objects; all draws record
// through ixrhi::IXRHICommandList from the canonical frame context.
//
// Preserved exactly (NOT redesigned): chunk mesh generation + frustum draws,
// CPU mip-chain generation, full-texture sculpt/paint uploads, mapped-write
// sculpt semantics (via Read-modify-Write spans), dual camera-uniform paths
// (Scene/Game views), 4-cascade PCF shadows with depth bias, water
// reflection/refraction coupling (IXRHI-owned snapshots), editor tools.

#include "WorldCamera.h"
#include "InputEvent.h"
#include "MapEditorTypes.h"
#include "SunShadow.h"

#include "IXRHIBinding.h"
#include "IXRHIBuffer.h"
#include "IXRHICommandList.h"
#include "IXRHIDevice.h"
#include "IXRHIFrame.h"
#include "IXRHIPipeline.h"
#include "IXRHIRenderPass.h"
#include "IXRHIRenderTarget.h"
#include "IXRHITexture.h"

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace client::asset {
class IAssetReader;
}
namespace mx::map {
struct Manifest;
}

// CPU sampling of the actual rendered/Jolt terrain triangles. Coordinates
// are centimetres in the existing engine source grid (row zero is north).
// Client queries retain closed/clamped outer boundaries. This CPU-only entry
// point can be tested without creating a renderer/device/window.
float SampleTerrainCollisionHeightCm(const std::vector<float>& grid,
                                    std::uint32_t width, std::uint32_t height,
                                    float localXcm, float localYcm, float cellScaleCm);

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

    struct Texture
    {
        std::shared_ptr<ixrhi::IXRHITexture> image;
        std::shared_ptr<ixrhi::IXRHISampler> sampler;
        ixrhi::IXRHIFormat format = ixrhi::IXRHIFormat::Undefined;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t mipLevels = 0;
        uint32_t arrayLayers = 0;
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

    bool Create(ixrhi::IXRHIDevice& rhi, client::asset::IAssetReader& assets);
    void SetAdditionalAssetRoots(std::vector<std::filesystem::path> roots);
    bool LoadMap(ixrhi::IXRHIDevice& rhi, const std::string& mapDirectory, int32_t serverX, int32_t serverY);
    bool CreateFlatTerrain(ixrhi::IXRHIDevice& rhi, const TerrainSceneData& terrain);
    void ClearTerrain();
    bool HasTerrain() const { return m_sceneTerrainActive; }
    bool IsMapLoadedForDiagnostics() const { return m_mapLoaded; }
    TerrainSceneData GetTerrainSceneData() const;
    // Same, filled into `out` reusing its grid buffers (no allocation once sized): for per-frame snapshots.
    void GetTerrainSceneData(TerrainSceneData& out) const;
    // Everything but the height/attribute/splat grids (name, size, flags): cheap, unlike the above.
    TerrainSceneData GetTerrainSceneInfo() const;
    void SetTerrainSceneData(const TerrainSceneData& terrain);
    bool RecreatePipeline(ixrhi::IXRHIDevice& rhi);
    // Borrowed IXRHI pass token (null = backend default, i.e. the swapchain pass).
    void SetTargetPass(const ixrhi::IXRHIRenderPass* pass);
    // Backend for GPU timestamp markers + swapchain queries + teardown drain.
    // Borrowed, may be null (markers skipped).
    void SetRhiDevice(ixrhi::IXRHIDevice* rhi) { m_rhi = rhi; }
    // IXRHI-owned refraction inputs (shared lifetime: recreating the
    // offscreen target cannot dangle these). Consumed directly as sampled
    // textures — no native handle resolution in Terrain.
    void SetWaterRefractionInputs(std::shared_ptr<ixrhi::IXRHITexture> colorSnapshot,
                                  std::shared_ptr<ixrhi::IXRHITexture> depthSnapshot,
                                  std::shared_ptr<ixrhi::IXRHISampler> sampler,
                                  std::uint32_t width,
                                  std::uint32_t height);
    // Whether any enabled water body is inside the camera's view (its water pass would draw).
    bool AnyWaterBodyInView(const WorldCamera& camera) const;
    // Whether the Scene view's water is seen past the opaque scene, by an occlusion query of the
    // water surface (depth test only) drawn every frame it is in view. Results come a frame slot
    // later, so the water comes back two frames after it shows from behind something (and is kept
    // for a few frames after it is last seen). Per frame: BeginWaterVisibility outside any pass before
    // the scene pass, then QueryWaterVisibility inside it once the opaque scene is drawn.
    void BeginWaterVisibility(ixrhi::IXRHICommandList& cmd, const ixrhi::IXRHIFrameInfo& frame);
    void QueryWaterVisibility(ixrhi::IXRHICommandList& cmd, const ixrhi::IXRHIFrameInfo& frame,
                              const WorldCamera& camera, std::uint32_t viewWidth, std::uint32_t viewHeight);
    bool WaterMayBeVisible(std::uint64_t frameNumber) const;
    // Whether a world box lies wholly under the surface of a water body it is over (by the body's
    // extent): a transparent thing there is drawn before the water, which then refracts it.
    bool BoxUnderWater(WorldVec3 boundsMin, WorldVec3 boundsMax) const;
    bool HandleEditorInput(const InputEvent& event);
    void UpdateEditor(ixrhi::IXRHIDevice& rhi,
                      double deltaSeconds,
                      const WorldCamera& camera,
                      uint32_t viewportWidth,
                      uint32_t viewportHeight);
    // Reflection callback receives the borrowed IXRHI pass of the reflection
    // target (plus its extent) so guest draws bake against the real pass.
    // viewWidth x viewHeight is the size of the view the water is drawn into (the reflection is a
    // fraction of it); nothing is drawn while no water body is in the camera's view.
    void RenderWaterReflection(ixrhi::IXRHICommandList& cmd,
                               const ixrhi::IXRHIFrameInfo& frame,
                               const WorldCamera& camera,
                               std::uint32_t viewWidth,
                               std::uint32_t viewHeight,
                               double timeSeconds,
                               const std::function<void(const WorldCamera&,
                                                        std::uint32_t,
                                                        std::uint32_t,
                                                        const ixrhi::IXRHIRenderPass*,
                                                        float)>& renderEntities = {});
    // viewIndex selects which camera-uniform path to use: 0 = primary (editor Scene
    // View / free-fly), 1 = secondary (Game view / project Main Camera). Each view has
    // its own per-frame uniform buffer + descriptor set so the terrain can be drawn from
    // two cameras in the same frame without the second draw clobbering the first.
    void Render(ixrhi::IXRHICommandList& cmd,
                const ixrhi::IXRHIFrameInfo& frame,
                const WorldCamera& camera,
                std::uint32_t targetWidth = 0,
                std::uint32_t targetHeight = 0,
                uint32_t viewIndex = 0,
                bool clearDepth = true);
    // clearDepth = false: the pass already cleared depth and opaque meshes were drawn first, so
    // the terrain they cover is rejected by the depth test instead of being shaded and overdrawn.
    // viewIndex selects the camera-uniform path: 0 = primary (Scene View / free-fly),
    // 1 = secondary (Game view / Main Camera). Mirrors TerrainRenderer::Render.
    void RenderWater(ixrhi::IXRHICommandList& cmd,
                     const ixrhi::IXRHIFrameInfo& frame,
                     const WorldCamera& camera,
                     double timeSeconds,
                     std::uint32_t targetWidth = 0,
                     std::uint32_t targetHeight = 0,
                     uint32_t viewIndex = 0);
    // Records the uploads of this frame's terrain edits: sculpted vertices into the drawn (video
    // memory) vertex buffer and the painted splat rectangle into the splat textures. Call once per
    // frame outside any render pass, before the terrain is drawn.
    void UploadEditedTerrain(ixrhi::IXRHICommandList& cmd, const ixrhi::IXRHIFrameInfo& frame);
    // Draws the sun shadow cascades: the terrain (if any), then in each cascade what the casters add
    // (the meshes), given the cascade, its light view-projection and its render pass. A cascade is
    // drawn again only when what it holds may have changed: its placement (the camera moved past one
    // of its texels, the sun turned), the terrain, or its casters' revision, which changes whenever
    // the casters drawn into it do (moved, animated). The others keep what they hold.
    //
    // The static casters are drawn with the terrain into a cache layer of the cascade's own, kept
    // while they, the terrain and the cascade stay the same. A cascade holding moving casters (an
    // animated character redraws it every frame) is then a copy of that layer with only the moving
    // ones drawn over it, instead of the whole scene again.
    using ShadowCasterDraw = std::function<void(std::uint32_t cascade,
                                                const WorldMat4& lightViewProj,
                                                const ixrhi::IXRHIRenderPass* pass)>;
    using CascadeRevisions = std::array<std::uint64_t, SunShadowReceive::kCascades>;
    // The part of a cascade a static caster change draws again: its clip-space xy range (of the
    // cascade's lightViewProj), and that range stretched to the whole clip square (regionViewProj).
    struct ShadowRegion
    {
        float clipMin[2] = {-1.0f, -1.0f};
        float clipMax[2] = {1.0f, 1.0f};
        WorldMat4 viewProj{};
    };
    // Draws the static casters over a region of a cascade, with the cascade's lightViewProj.
    using ShadowCasterRegionDraw = std::function<void(std::uint32_t cascade,
                                                      const WorldMat4& lightViewProj,
                                                      const ShadowRegion& region,
                                                      const ixrhi::IXRHIRenderPass* pass)>;
    struct StaticCasterBounds
    {
        WorldVec3 min{};
        WorldVec3 max{};
    };
    struct ShadowCasters
    {
        CascadeRevisions staticRevisions{};
        CascadeRevisions dynamicRevisions{};  // 0: no moving caster in the cascade
        ShadowCasterDraw drawStatic;
        ShadowCasterDraw drawDynamic;
        // How the static casters changed since the frame before, when their revision was
        // staticDeltaFrom (0: not known): staticJoined of them joined (drawStaticJoined draws those),
        // and those in staticLeft left or moved (their bounds as drawn). A static cache layer that holds
        // staticDeltaFrom takes just this: the joined drawn over it, and where each that left was, the
        // layer cleared and drawn again (drawStaticRegion). Any other changed layer is drawn whole.
        std::uint64_t staticDeltaFrom = 0;
        std::uint32_t staticJoined = 0;
        const std::vector<StaticCasterBounds>* staticLeft = nullptr;
        ShadowCasterDraw drawStaticJoined;
        ShadowCasterRegionDraw drawStaticRegion;
    };
    void RenderSunShadowMap(ixrhi::IXRHICommandList& cmd,
                            const ixrhi::IXRHIFrameInfo& frame,
                            const WorldCamera& camera,
                            const ShadowCasters& casters = {});
    // Places the cascades around the camera (RenderSunShadowMap does it too): call it first to know
    // what each cascade will cover (SunShadowCascadeViewProj).
    void UpdateSunShadowCascades(const WorldCamera& camera) { UpdateShadowCascades(camera); }
    // The camera distances whose surfaces sample a cascade (the terrain picks one by distance and
    // blends into the next over the last tenth of each range): a caster whose shadow cannot land
    // between them need not be drawn into it.
    static void SunShadowCascadeServes(std::uint32_t cascade, float& nearest, float& farthest);
    // Clears the cascades once (all lit) so the map is shader-readable before anything draws into
    // it; the terrain and mesh draws bind it either way. Call outside render passes.
    void EnsureSunShadowMapReadable(ixrhi::IXRHICommandList& cmd);
    // The map as the mesh renderers sample it (enabled when it was drawn in frame `frameNumber`).
    SunShadowReceive SunShadowForMeshes(std::uint64_t frameNumber) const;
    // The sun shadow cascades for other passes (volumetric light): the D32 array (one layer per
    // cascade, shader-readable) when RenderSunShadowMap drew it in the frame `frameNumber`, else null.
    std::shared_ptr<ixrhi::IXRHITexture> SunShadowTexture(std::uint64_t frameNumber) const
    {
        return m_shadowDrawnFrame == frameNumber ? m_shadowTexture : nullptr;
    }
    // Light view-projection per cascade (row vectors), as drawn; finest first.
    const WorldMat4* SunShadowCascadeViewProj() const { return m_shadowCascadeViewProj.data(); }
    static constexpr std::uint32_t SunShadowCascadeCount() { return kShadowCascadeCount; }
    static constexpr float kSunShadowDepthBias = 0.0015f;
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
    bool SetWaterBodies(ixrhi::IXRHIDevice& rhi, const std::vector<WaterBody>& bodies);
    bool SetSelectedWaterBodyHighlight(ixrhi::IXRHIDevice& rhi, std::uint32_t selectedWaterBodyId);
    void RenderSelectedWaterBodyHighlight(ixrhi::IXRHICommandList& cmd,
                                          const ixrhi::IXRHIFrameInfo& frame,
                                          const WorldCamera& camera,
                                          std::uint32_t targetWidth = 0,
                                          std::uint32_t targetHeight = 0);
    void SetWaterSculptBrush(bool visible, float worldX, float worldZ, float radiusMeters, bool addMode);
    void SetPaletteSlots(const std::array<MapEditorPaletteSlot, 8>& slots);
    const std::array<MapEditorPaletteSlot, 8>& GetPaletteSlots() const { return m_paletteSlots; }
    bool ApplyPaletteSlots(ixrhi::IXRHIDevice& rhi, const std::array<MapEditorPaletteSlot, 8>& slots);
    bool ApplyPaletteSlotParams(const MapEditorPaletteSlot& slot);
    bool ApplyPaletteSlotChange(ixrhi::IXRHIDevice& rhi, const MapEditorPaletteSlot& slot);
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
    // The cascades' depth ranges move in steps of this many metres (see UpdateShadowCascades); their
    // padding carries one more step.
    static constexpr float kShadowDepthStepMeters = 64.0f;
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
        std::int32_t activeLayerCount = 1;
        std::int32_t terrainPadding0[3] = {0, 0, 0};
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
        std::shared_ptr<ixrhi::IXRHITexture> color;
        std::shared_ptr<ixrhi::IXRHITexture> depth;
        std::shared_ptr<ixrhi::IXRHISampler> sampler;
        std::unique_ptr<ixrhi::IXRHIRenderTarget> target;
        ixrhi::IXRHIFormat colorFormat = ixrhi::IXRHIFormat::Undefined;
        ixrhi::IXRHIFormat depthFormat = ixrhi::IXRHIFormat::Undefined;
        uint32_t width = 0;
        uint32_t height = 0;
        WaterConfig::ReflectionQuality quality = WaterConfig::ReflectionQuality::Half;
    };

    struct WaterBodyGpu
    {
        WaterBody body;
        std::shared_ptr<ixrhi::IXRHIBuffer> vertexBuffer;
        std::shared_ptr<ixrhi::IXRHIBuffer> indexBuffer;
        std::array<std::shared_ptr<ixrhi::IXRHIBuffer>, kFramesInFlight> uniformBuffers{};
        // Secondary camera-uniform path for the Game view (project Main Camera), parallel to
        // the primary (Scene View / free-fly) so the same water body can be drawn from two
        // cameras in one frame without clobbering. Selected by RenderWater(viewIndex).
        std::array<std::shared_ptr<ixrhi::IXRHIBuffer>, kFramesInFlight> uniformBuffersSecondary{};
        uint32_t indexCount = 0;
        // World bounds of the mesh, for skipping a body (and its reflection) no camera sees.
        WorldVec3 boundsMin{};
        WorldVec3 boundsMax{};
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

    bool CreateBuffers(ixrhi::IXRHIDevice& rhi);
    bool EnsureUniformBuffers(ixrhi::IXRHIDevice& rhi);
    bool CreateFlatBuffers(ixrhi::IXRHIDevice& rhi);
    bool CreateMapBuffers(ixrhi::IXRHIDevice& rhi, const std::string& mapDirectory, int32_t serverX, int32_t serverY);
    void BuildTerrainChunkDraws(const std::vector<Vertex>& vertices, std::vector<uint32_t>& indices);
    // Grows the culling bounds of the chunks under edited vertices (sculpt writes heights straight
    // into the vertex buffer, after the bounds were built).
    void ExpandTerrainChunkBounds(const Vertex* vertices, std::size_t count);
    bool CreateFallbackTexture(ixrhi::IXRHIDevice& rhi);
    bool CreateFallbackMask(ixrhi::IXRHIDevice& rhi);
    bool CreateFallbackSplatTextures(ixrhi::IXRHIDevice& rhi);
    bool CreateSceneSplatTextures(ixrhi::IXRHIDevice& rhi);
    bool LoadTerrainPalette(ixrhi::IXRHIDevice& rhi, const mx::map::Manifest& manifest, const std::string& mapDirectory);
    bool LoadTerrainPaletteFromPaths(ixrhi::IXRHIDevice& rhi, const std::array<MapEditorPaletteSlot, 8>& slots);
    bool UploadRgbaTexture2D(ixrhi::IXRHIDevice& rhi,
                             const std::string& name,
                             uint32_t width,
                             uint32_t height,
                             const std::vector<std::uint8_t>& pixels,
                             ixrhi::IXRHISamplerAddress addressMode,
                             Texture& out,
                             ixrhi::IXRHIFormat format = ixrhi::IXRHIFormat::R8G8B8A8Unorm,
                             bool generateMips = false);
    bool UpdateRgbaTexture2D(ixrhi::IXRHIDevice& rhi, Texture& texture, const std::vector<std::uint8_t>& pixels);
    bool UploadRgbaTextureArray(ixrhi::IXRHIDevice& rhi,
                                const std::string& name,
                                uint32_t width,
                                uint32_t height,
                                uint32_t layers,
                                const std::vector<std::uint8_t>& pixels,
                                ixrhi::IXRHIFormat format,
                                Texture& out);
    bool UploadR8TextureArray(ixrhi::IXRHIDevice& rhi,
                              const std::string& name,
                              uint32_t width,
                              uint32_t height,
                              uint32_t layers,
                              const std::vector<std::uint8_t>& pixels,
                              Texture& out);
    bool CreateBindGroup(ixrhi::IXRHIDevice& rhi);
    void UpdateBindGroup();
    bool CreatePipeline(ixrhi::IXRHIDevice& rhi);
    void DestroyPipeline();
    bool CreateWaterResources(ixrhi::IXRHIDevice& rhi);
    bool LoadWaterBodies(ixrhi::IXRHIDevice& rhi, const std::string& mapDirectory);
    bool CreateWaterBodyMesh(ixrhi::IXRHIDevice& rhi, WaterBodyGpu& waterBody);
    bool CreateWaterBodyUniformBuffers(ixrhi::IXRHIDevice& rhi, WaterBodyGpu& waterBody);
    bool RebuildSelectedWaterBodyHighlight(ixrhi::IXRHIDevice& rhi, const WaterBody* body);
    bool CreateWaterBindGroup(ixrhi::IXRHIDevice& rhi);
    bool CreateWaterNormalTextures(ixrhi::IXRHIDevice& rhi);
    void UpdateWaterBindGroup();
    void WriteWaterBindGroupSets(std::uint32_t bodyIndex,
                                 uint32_t viewIndex,
                                 uint32_t frameIndex,
                                 const WaterMaterialTextureSet* materialTextures = nullptr);
    bool CreateWaterPipeline(ixrhi::IXRHIDevice& rhi);
    bool CreateOrRecreateWaterReflectionResources(ixrhi::IXRHIDevice& rhi, bool force);
    // viewWidth/viewHeight: the view the reflection is for (0: the swapchain's size).
    bool CreateOrRecreateWaterReflectionResources(ixrhi::IXRHIDevice& rhi,
                                                  bool force,
                                                  WaterConfig::ReflectionQuality quality,
                                                  std::uint32_t viewWidth = 0,
                                                  std::uint32_t viewHeight = 0);
    bool CreateWaterReflectionPipeline(ixrhi::IXRHIDevice& rhi);
    void DestroyWaterReflectionResources();
    void DestroyWaterReflectionPipeline();
    WorldCamera ComputeMirrorCamera(const WorldCamera& camera,
                                    std::uint32_t targetWidth,
                                    std::uint32_t targetHeight,
                                    float waterLevelY) const;
    // The enabled water body closest to the camera among those in its view (null: none in view).
    const WaterBodyGpu* FindClosestWaterBody(const WorldCamera& camera, float* outDistanceMeters = nullptr) const;
    bool WaterBodyInView(const WaterBodyGpu& waterBody, const WorldCamera& camera) const;
    const WaterConfig& ResolveWaterConfig(const WaterBody& body) const;
    void DestroyWaterResources();
    void DestroyWaterBodyResources();
    void DestroyWaterBodyResources(WaterBodyGpu& waterBody);
    void DestroyWaterMaterialTextureCache();
    bool LoadWaterMaterialTextureSet(ixrhi::IXRHIDevice& rhi,
                                     const std::string& id,
                                     const WaterMaterialData& material,
                                     WaterMaterialTextureSet& out);
    const WaterMaterialTextureSet* ResolveWaterMaterialTextures(const WaterBody& body) const;
    void DestroyWaterPipeline();
    void UpdateWaterBodyUniform(uint32_t frameIndex,
                                const WorldCamera& camera,
                                double timeSeconds,
                                WaterBodyGpu& waterBody,
                                bool reflectionTarget,
                                uint32_t viewIndex = 0);
    WaterUniformBlock BuildWaterUniform(const WorldCamera& camera,
                                        double timeSeconds,
                                        const WaterConfig& water,
                                        float waterLevelY,
                                        bool reflectionTarget) const;
    void UploadWaterUniform(const std::shared_ptr<ixrhi::IXRHIBuffer>& buffer, const WaterUniformBlock& uniform);
    void DestroyTerrainLayers();
    void UpdateUniform(uint32_t frameIndex, const WorldCamera& camera, bool reflectionPass = false, uint32_t viewIndex = 0);
    bool CreateShadowResources(ixrhi::IXRHIDevice& rhi);
    bool CreateShadowPipeline(ixrhi::IXRHIDevice& rhi);
    void DestroyShadowResources();
    void DestroyShadowPipeline();
    void UpdateShadowCascades(const WorldCamera& camera);
    void LoadEditorConfig();
    void ApplyLegacyHeightBrush(float sign, double deltaSeconds);
    void ApplyEditorBrush(ixrhi::IXRHIDevice& rhi, double deltaSeconds);
    bool RaycastEditorBrush(const WorldCamera& camera, uint32_t viewportWidth, uint32_t viewportHeight);
    void BeginEditorStroke();
    void EndEditorStroke();
    void RecordHeightUndo(size_t index);
    void RecordSplatUndo(size_t index);
    void UndoLastEditorStroke(ixrhi::IXRHIDevice& rhi);
    void MarkHeightDirty(size_t heightIndex);
    void MarkSplatDirty(size_t splatIndex);
    // Highest painted splat layer + 1 (the shader's layer loop bound). Scanning both splat maps is
    // ~1 ms at 512 m, so it is cached: painting grows it per texel (MarkSplatDirty), bulk changes reset it.
    uint32_t ActiveSplatLayerSpan();
    bool RefreshSplatTextures(ixrhi::IXRHIDevice& rhi);
    bool SaveDirtyChunks();
    bool SaveWorldPalette() const;
    bool SaveWaterBodies() const;
    bool SaveChunkHeights(uint32_t chunkX, uint32_t chunkY, uint32_t dirtyTexels);
    bool ReloadCurrentMap(ixrhi::IXRHIDevice& rhi);
    std::string ResolveWritableMapPath(const std::string& relativePath) const;

    ixrhi::IXRHIDevice* m_rhi = nullptr; // borrowed backend (timestamps, swapchain queries, teardown drain)
    const ixrhi::IXRHIRenderPass* m_targetPass = nullptr; // borrowed (frame owner)
    client::asset::IAssetReader* m_assets = nullptr;
    std::unordered_map<std::string, WaterMaterialData> m_waterMaterials;
    std::unordered_map<std::string, WaterMaterialTextureSet> m_waterMaterialTextures;
    std::string m_waterMaterialTextureSignature;
    std::string m_waterMaterialEdgeSignature;
    std::vector<std::filesystem::path> m_additionalAssetRoots;
    WaterMaterialData m_defaultWaterMaterial;
    std::shared_ptr<ixrhi::IXRHIBuffer> m_vertexBuffer;      // drawn: video memory
    std::shared_ptr<ixrhi::IXRHIBuffer> m_vertexEditBuffer;  // host-visible copy the sculpt reads/writes
    bool m_vertexBufferUploadPending = false;                // edit copy changed: UploadEditedTerrain
    std::shared_ptr<ixrhi::IXRHIBuffer> m_indexBuffer;
    std::shared_ptr<ixrhi::IXRHIBuffer> m_debugVertexBuffer;
    std::shared_ptr<ixrhi::IXRHIBuffer> m_debugIndexBuffer;
    std::shared_ptr<ixrhi::IXRHIBuffer> m_logicVertexBuffer;
    std::shared_ptr<ixrhi::IXRHIBuffer> m_logicIndexBuffer;
    std::shared_ptr<ixrhi::IXRHIBuffer> m_selectedWaterBodyVertexBuffer;
    std::shared_ptr<ixrhi::IXRHIBuffer> m_selectedWaterBodyIndexBuffer;
    std::array<std::shared_ptr<ixrhi::IXRHIBuffer>, kFramesInFlight> m_uniformBuffers{};
    // Secondary camera-uniform path for the Game view (project Main Camera). Parallel to
    // m_uniformBuffers so terrain can be drawn from a second camera in the same frame
    // without clobbering the primary (free-fly) terrain draw. See Render(viewIndex).
    std::array<std::shared_ptr<ixrhi::IXRHIBuffer>, kFramesInFlight> m_uniformBuffersSecondary{};
    // The water reflection's mirrored camera (view kReflectionUniformView). It is recorded in the
    // same frame as the Scene View terrain: sharing that view's buffer, the reflection was drawn with
    // whichever camera was written last (the unmirrored Scene View, without the water-level clip).
    static constexpr uint32_t kReflectionUniformView = 2;
    std::array<std::shared_ptr<ixrhi::IXRHIBuffer>, kFramesInFlight> m_uniformBuffersReflection{};
    // One bind-group layout (UBO + 9 combined samplers) with a slot per
    // (view, frame): slot = viewIndex * kFramesInFlight + frameIndex.
    std::unique_ptr<ixrhi::IXRHIBindGroupLayout> m_bindLayout;
    std::unique_ptr<ixrhi::IXRHIBindGroup> m_bindGroup;
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_pipeline;
    // One water bind-group layout (UBO + 6 combined samplers) with a slot per
    // (body, view, frame): slot = (bodyIndex * 2 + viewIndex) * kFramesInFlight + frameIndex.
    std::unique_ptr<ixrhi::IXRHIBindGroupLayout> m_waterBindLayout;
    std::unique_ptr<ixrhi::IXRHIBindGroup> m_waterBindGroup;
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_waterPipeline;
    // The water visibility query (see WaterMayBeVisible): the surface drawn for the depth test only,
    // one query per frame slot, and the last frame it was seen.
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_waterOcclusionPipeline;
    std::unique_ptr<ixrhi::IXRHIOcclusionQueries> m_waterQueries;
    std::array<bool, kFramesInFlight> m_waterQueryIssued{};
    std::uint64_t m_waterSeenFrame = 0;
    bool m_waterSeen = false;  // m_waterSeenFrame holds a frame
    // Refraction inputs are IXRHI-owned (shared lifetime: recreating the
    // offscreen target cannot dangle these). Native handles resolve locally
    // at descriptor-write time (backend bridge, transition-only).
    std::shared_ptr<ixrhi::IXRHITexture> m_waterSceneColor;
    std::shared_ptr<ixrhi::IXRHITexture> m_waterSceneDepth;
    std::shared_ptr<ixrhi::IXRHISampler> m_waterSceneSampler;
    std::uint32_t m_waterSceneWidth = 0;
    std::uint32_t m_waterSceneHeight = 0;
    WaterReflectionResources m_waterReflection;
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_reflectionPipeline;
    bool m_waterReflectionDescriptorsDirty = true;
    // 4-cascade depth array + comparison sampler + one depth-only target per
    // cascade slice. Sampled by the main pass (binding 9) after an explicit
    // DepthStencilAttachment -> ShaderReadOnly transition per shadow render.
    std::shared_ptr<ixrhi::IXRHITexture> m_shadowTexture;
    std::shared_ptr<ixrhi::IXRHISampler> m_shadowSampler;
    std::array<std::unique_ptr<ixrhi::IXRHIRenderTarget>, kShadowCascadeCount> m_shadowTargets{};
    // The static cache (see RenderSunShadowMap): made the first time a cascade holds moving casters.
    // Its layers are drawn by their own targets and copied into the cascades, which the moving casters
    // are then drawn over by targets that keep (load) the depth instead of clearing it.
    std::shared_ptr<ixrhi::IXRHITexture> m_staticShadowTexture;
    std::array<std::unique_ptr<ixrhi::IXRHIRenderTarget>, kShadowCascadeCount> m_staticShadowTargets{};
    std::array<std::unique_ptr<ixrhi::IXRHIRenderTarget>, kShadowCascadeCount> m_shadowOverlayTargets{};
    // The static cache layers kept (load): what a static caster change is drawn over.
    std::array<std::unique_ptr<ixrhi::IXRHIRenderTarget>, kShadowCascadeCount> m_staticShadowLoadTargets{};
    // Where a static caster that left a cascade was: the texels taken out and drawn again, and the
    // cascade's view-projection stretched over them (to pick what is drawn there).
    struct StaticShadowRegion
    {
        std::uint32_t x = 0;
        std::uint32_t y = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        ShadowRegion region;
    };
    static constexpr std::size_t kMaxStaticShadowRegions = 16;
    // The regions of a cascade's static layer to draw again: the edges a shift (in texels) uncovered,
    // and where the casters that left were; false when that is more than drawing it whole (too many,
    // or a quarter of it).
    bool StaticShadowRegions(std::uint32_t cascade, const ShadowCasters& casters, std::int32_t shiftX,
                             std::int32_t shiftY, std::vector<StaticShadowRegion>& regions) const;
    // Whether a cascade moved only across its own texel grid since a layer was drawn with oldViewProj:
    // the layer's contents are then still right, moved by (shiftX, shiftY) texels.
    static bool StaticShadowShift(const std::array<float, 16>& oldViewProj, const std::array<float, 16>& newViewProj,
                                  std::int32_t& shiftX, std::int32_t& shiftY);
    bool m_staticShadowCacheFailed = false;
    bool EnsureStaticShadowCache(ixrhi::IXRHICommandList& cmd);
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_shadowPipeline;
    std::array<WorldMat4, kShadowCascadeCount> m_shadowCascadeViewProj{};
    std::uint64_t m_shadowDrawnFrame = ~0ull;  // frame number of the last RenderSunShadowMap draw
    bool m_shadowMapReadable = false;           // cleared or drawn at least once: shader-readable
    std::array<float, kShadowCascadeCount> m_shadowCascadeDepthRange{};  // metres per depth unit
    std::array<float, kShadowCascadeCount> m_shadowCascadeTexelSize{};   // metres per texel
    // What each cascade was last drawn from. While it stays the same the cascade is kept instead of
    // drawn again (its light view-projection covers the camera and the sun: a camera turning in
    // place, or moving less than a texel, keeps it).
    struct ShadowCascadeInputs
    {
        std::array<float, 16> viewProj{};
        const void* vertexBuffer = nullptr;
        const void* indexBuffer = nullptr;
        const void* shadowTexture = nullptr;
        std::uint32_t indexCount = 0;
        std::uint64_t geometryRevision = 0;
        std::uint64_t castersRevision = 0;
        std::uint64_t dynamicRevision = 0;
        bool operator==(const ShadowCascadeInputs&) const = default;
    };
    std::array<std::optional<ShadowCascadeInputs>, kShadowCascadeCount> m_shadowCascadeInputs;
    // What each static cache layer was drawn from (the terrain and the static casters only).
    std::array<std::optional<ShadowCascadeInputs>, kShadowCascadeCount> m_staticShadowInputs;
    std::uint64_t m_terrainGeometryRevision = 0;  // bumped by every vertex upload (sculpting)
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
    // Brush applications in the current stroke: the per-application diagnostics (flushed log lines)
    // are written for the first few only, not every frame while the button is held.
    std::uint32_t m_brushDiagApplications = 0;
    bool BrushDiagLogs() const { return m_brushDiagApplications < 2; }
    bool m_editorStrokeActive = false;
    bool m_editorCtrlHeld = false;
    bool m_editorBrushVisible = false;
    bool m_editorTerrainToolActive = false;
    bool m_waterSculptBrushVisible = false;
    bool m_waterSculptBrushAddMode = true;
    bool m_editorSplatGpuDirty = false;
    // Painted splat texels since the last upload (MarkSplatDirty): UploadEditedTerrain copies just this
    // rectangle through a per-frame-slot staging buffer in the frame's command list, instead of a
    // synchronous whole-texture upload (queue wait) on every painted frame.
    struct SplatDirtyRect
    {
        uint32_t minX = std::numeric_limits<uint32_t>::max();
        uint32_t minY = std::numeric_limits<uint32_t>::max();
        uint32_t maxX = 0;
        uint32_t maxY = 0;
        bool Valid() const { return minX <= maxX && minY <= maxY; }
    };
    SplatDirtyRect m_splatDirtyRect;
    std::array<std::shared_ptr<ixrhi::IXRHIBuffer>, kFramesInFlight> m_splatStaging;
    void UploadEditedSplat(ixrhi::IXRHICommandList& cmd, uint32_t frameIndex);
    std::int32_t m_activeSplatLayerSpan = -1;  // ActiveSplatLayerSpan() cache; -1 = splat data changed
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
    bool m_terrainShaderOptimDiagLogged = false;
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
