#pragma once

// GraphEnvironment — what material graph shaders read from the scene besides their own material
// (the Time, Scene Depth and Sky Reflection nodes): the time, the camera planes, the scene depth
// behind a transparent surface and the sky. It is descriptor set 2 of every graph pipeline
// (StaticMeshRenderer), laid out as the graph pixel shader declares it (GraphShader.cpp):
//   binding 0  uniform block: frame (x seconds, y near, z far, w 1 = scene depth bound),
//              viewport (width, height, 1/width, 1/height), then SkyRenderer's sky parameters
//   binding 1  scene depth (a copy of the view's depth, sampled)
//   binding 2  sky cube map
//   binding 3  sky panorama
//
// The frame owner calls Begin before each group of draws of a view: before its opaque draws (no
// depth: those draws write it), and again with its depth copy before its transparent draws. The
// draws after a Begin bind its set; each Begin takes a set of its own for the frame, because a set
// must not change once the frame's command list has bound it.

#include "SkyRenderer.h"

#include "IXRHIBinding.h"
#include "IXRHIBuffer.h"
#include "IXRHICommandList.h"
#include "IXRHIDevice.h"
#include "IXRHIFrame.h"
#include "IXRHITexture.h"
#include "math/WorldMath.h"

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_set>
#include <utility>
#include <vector>

class GraphEnvironment
{
public:
    static constexpr std::uint32_t kSet = 2;
    static constexpr std::uint32_t kFramesInFlight = 2;
    static constexpr std::uint32_t kSetsPerFrame = 16;
    // Time wraps around once a day: a float keeps sub-frame precision that far.
    static constexpr double kTimePeriodSeconds = 86400.0;

    bool Create(ixrhi::IXRHIDevice& rhi);
    void Destroy();
    bool IsReady() const { return m_layout != nullptr; }
    const ixrhi::IXRHIBindGroupLayout* Layout() const { return m_layout.get(); }

    // The sky the following Begins hand to the shaders (SkyRenderer::GetLookup after its Update).
    void SetSky(const SkyRenderer::Lookup& sky);
    // Starts a group of draws of one view. sceneDepth: a sampled copy of the view's depth (the
    // transparent draws after a scene copy), or none.
    void Begin(const ixrhi::IXRHIFrameInfo& frame,
               double seconds,
               const WorldCamera& camera,
               std::uint32_t width,
               std::uint32_t height,
               std::shared_ptr<ixrhi::IXRHITexture> sceneDepth = nullptr);
    // Binds the set of the frame's last Begin. A frame without a Begin yet gets one with the last
    // Begin's time and camera and no depth (before any Begin: time 0, no sky).
    void Bind(ixrhi::IXRHICommandList& cmd, const ixrhi::IXRHIFrameInfo& frame);
    bool HasSceneDepth(std::uint32_t frameIndex) const;

    // A transparent material that reads the scene depth was drawn. The frame owner keeps copying the
    // depth before transparent draws while this happened within the last few frames.
    void NoteSceneDepthRead() { m_lastDepthRead = m_frameNumber; m_depthReadSeen = true; }
    bool SceneDepthReadRecently(std::uint64_t frameNumber) const
    {
        return m_depthReadSeen && frameNumber <= m_lastDepthRead + 3;
    }

private:
    struct Constants
    {
        float frame[4];
        float viewport[4];
        float sky[7][4];
    };
    // Bound images per set: a descriptor is rewritten only when its image changes.
    struct BoundImages
    {
        const ixrhi::IXRHITexture* depth = nullptr;
        const ixrhi::IXRHITexture* cube = nullptr;
        const ixrhi::IXRHITexture* panorama = nullptr;
        bool hasDepth = false;
        bool constants = false;  // the uniform binding points at the set's block
    };
    static constexpr std::uint64_t kConstantStride = 256;  // uniform offsets: 256-byte aligned
    // A bind group keeps every image ever bound to it alive: one that has seen this many is
    // replaced (view resizes make new depth copies).
    static constexpr std::size_t kMaxImagesPerGroup = 12;

    bool CreateGroup(std::uint32_t frameIndex);
    void BeginSet(const ixrhi::IXRHIFrameInfo& frame, std::shared_ptr<ixrhi::IXRHITexture> sceneDepth);
    void WriteSet(std::uint32_t frameIndex, std::uint32_t set, const Constants& constants,
                  const std::shared_ptr<ixrhi::IXRHITexture>& depth);

    ixrhi::IXRHIDevice* m_rhi = nullptr;
    std::unique_ptr<ixrhi::IXRHIBindGroupLayout> m_layout;
    std::array<std::unique_ptr<ixrhi::IXRHIBindGroup>, kFramesInFlight> m_groups{};
    // Replaced groups, kept until the frames that may still use them have finished.
    std::vector<std::pair<std::unique_ptr<ixrhi::IXRHIBindGroup>, std::uint64_t>> m_retired;
    std::array<std::array<BoundImages, kSetsPerFrame>, kFramesInFlight> m_bound{};
    std::array<std::unordered_set<const void*>, kFramesInFlight> m_imagesSeen{};
    std::shared_ptr<ixrhi::IXRHIBuffer> m_constants;
    std::shared_ptr<ixrhi::IXRHITexture> m_fallbackDepth;
    std::shared_ptr<ixrhi::IXRHITexture> m_fallbackCube;
    std::shared_ptr<ixrhi::IXRHITexture> m_fallbackPanorama;
    std::shared_ptr<ixrhi::IXRHISampler> m_depthSampler;
    std::shared_ptr<ixrhi::IXRHISampler> m_skySampler;
    SkyRenderer::Lookup m_sky;
    bool m_hasSky = false;
    std::array<std::uint64_t, kFramesInFlight> m_setFrame{};  // the frame the cursor counts sets of
    std::array<std::uint32_t, kFramesInFlight> m_cursor{};    // sets taken this frame
    std::array<std::uint32_t, kFramesInFlight> m_current{};   // the set draws bind
    std::uint64_t m_frameNumber = 0;
    // The last Begin's view, for a frame whose draws bind before any Begin.
    double m_seconds = 0.0;
    WorldCamera m_camera{};
    std::uint32_t m_width = 0, m_height = 0;
    std::uint64_t m_lastDepthRead = 0;
    bool m_depthReadSeen = false;
    bool m_loggedFull = false;
};
