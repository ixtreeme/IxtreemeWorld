#pragma once

#include "IXRHIDevice.h"
#include "IXRHIFrame.h"
#include "WorldCamera.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

class OffscreenSceneRenderer;
namespace client::asset
{
class IAssetReader;
}

// Runtime image impostors. Captures preserve the source camera's actual HDR colour and depth,
// including arbitrary materials and the current animated pose. Small rotations about the same
// optical centre reuse captures through depth reprojection; translation uses original geometry.
// Call BeginView/EndView only around a storing offscreen MAIN pass, never a shadow/reflection pass.
class ViewImpostorRenderer
{
  public:
    enum class Kind : std::uint32_t
    {
        Object,
        Terrain,
        Character,
        Count
    };
    struct Rect
    {
        std::uint32_t x = 0, y = 0, width = 0, height = 0;
        bool operator==(const Rect&) const = default;
    };
    struct Stats
    {
        std::uint32_t draws[3]{};
        std::uint32_t captures[3]{};
        std::uint32_t fallbacks = 0;
        std::uint32_t reprojected = 0;
        std::uint64_t savedSourceTriangles = 0;
        std::uint64_t bytes = 0;
    };
    struct Settings
    {
        bool enabled = true;
        float distance = 180.0f;
        float characterDistance = 80.0f;
        std::uint32_t maxDimension = 256;
        std::uint32_t capturesPerFrame = 16;
        std::uint32_t entries = 512;
        std::uint64_t memoryBytes = 64ull * 1024 * 1024;
        double refreshSeconds = 1.0 / 60.0;
        double staticRefreshSeconds = 0.1;
        double settleSeconds = 0.05;
    };

    ViewImpostorRenderer();
    ~ViewImpostorRenderer();
    bool Create(ixrhi::IXRHIDevice&, client::asset::IAssetReader&);
    void BeginFrame(const ixrhi::IXRHIFrameInfo&);
    void BeginView(OffscreenSceneRenderer&, ixrhi::IXRHICommandList&, const ixrhi::IXRHIFrameInfo&, const WorldCamera&,
                   double seconds, std::uint32_t view, std::uint64_t lightingRevision);
    void EndView();
    void Destroy(); // after the device has drained, like the other renderers
    const Stats& GetStats() const;

    static ViewImpostorRenderer* Active();
    static bool ApplyCaptureViewport(ixrhi::IXRHICommandList&);
    bool IsCapturing() const;
    // false means draw the original. callback records the original MAIN draw, with its unchanged
    // camera/uniforms; ApplyCaptureViewport crops rasterization to the impostor's rectangle.
    bool TryDraw(const void* owner, std::uint64_t id, std::uint64_t sourceRevision, Kind, WorldVec3 minimum,
                 WorldVec3 maximum, std::uint64_t sourceTriangles, const std::function<std::uint64_t()>& callback);
    // Terrain may combine individually projected, distant chunks into one native-resolution strip.
    // The caller must filter each chunk through ProjectBounds before constructing the union.
    bool TryDrawTerrainRegion(const void* owner, std::uint64_t id, std::uint64_t sourceRevision, Rect,
                              std::uint64_t sourceTriangles, std::uint32_t chunks,
                              const std::function<std::uint64_t()>& callback);

    static std::optional<Rect> ProjectBounds(const WorldCamera&, WorldVec3 minimum, WorldVec3 maximum,
                                             std::uint32_t width, std::uint32_t height, float distance,
                                             std::uint32_t maxDimension);
    static bool Fresh(double now, double captured, double interval);
    // Current clip -> capture clip, with a common perspective origin and at most two degrees
    // between forward directions. No translation, orthographic cameras or singular projections.
    static std::optional<WorldMat4> RotationMapping(const WorldCamera& captured, const WorldCamera& current);
    // Every ray in the output rectangle must be contained in the captured pixels. Never leave
    // uncovered pixels in place of source geometry when a crop or screen edge is revealed.
    static bool ReprojectionCovered(const WorldMat4&, Rect capture, Rect output, std::uint32_t width,
                                    std::uint32_t height);

  private:
    bool TryDrawPixels(const void* owner, std::uint64_t id, std::uint64_t sourceRevision, Kind, Rect,
                       std::uint32_t items, const std::function<std::uint64_t()>& callback);
    struct Impl;
    std::unique_ptr<Impl> m;
};
