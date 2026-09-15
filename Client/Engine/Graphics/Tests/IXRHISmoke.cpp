// IXRHISmoke — headless CPU validation for the IXRHI contract (Phase 2, §46).
//
// Covers everything testable without a GPU/window:
// - every IXRHI public header compiles WITHOUT Vulkan headers on the include
//   path (proves the §1 rule at build time — this TU includes no Vulkan header
//   before the contract headers; the backend test section is opt-in via
//   IXRHI_SMOKE_WITH_VULKAN for the conversion checks, which need <vulkan> types
//   only as DATA, never as engine API).
// - enum/format conversion round-trips (backend, pure functions).
// - descriptor defaults + capability defaults.
// GPU tests (device/buffer/texture/shader/pipeline/command recording) require a
// window + swapchain and run under the editor frame loop; they are NOT faked
// here. Run with --gpu only where a Vulkan device exists (currently skipped;
// the migrated SelectionOutline/WorldLabel paths exercise them in-editor).

#include "IXRHIBinding.h"
#include "IXRHIBuffer.h"
#include "IXRHICapabilities.h"
#include "IXRHICommandList.h"
#include "IXRHIDevice.h"
#include "IXRHI.h"
#include "IXRHIPipeline.h"
#include "IXRHIRenderTarget.h"
#include "IXRHIShader.h"
#include "IXRHISwapchain.h"
#include "IXRHISync.h"
#include "IXRHITexture.h"
#include "IXRHITypes.h"

#include "EditorGraphicsBridge.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

int g_ixrhiSmokeFailures = 0;

namespace
{

void Check(bool condition, const char* name)
{
    if (condition)
    {
        std::printf("[PASS] %s\n", name);
        return;
    }
    ++g_ixrhiSmokeFailures;
    std::printf("[FAIL] %s\n", name);
}

void TestDescriptorDefaults()
{
    const ixrhi::IXRHIBufferDesc buffer{};
    Check(buffer.sizeBytes == 0, "buffer desc zero-init");
    Check(buffer.cpuAccess == ixrhi::IXRHICpuAccess::None, "buffer cpu access defaults None");

    const ixrhi::IXRHITextureDesc texture{};
    Check(texture.width == 1 && texture.height == 1, "texture desc 1x1 default");
    Check(texture.format == ixrhi::IXRHIFormat::Undefined, "texture format defaults Undefined");

    const ixrhi::IXRHIBufferDesc sized{sizeof(float) * 16,
        ixrhi::IXRHIBufferUsage::Vertex,
        ixrhi::IXRHICpuAccess::Write,
        "SmokeVB"};
    Check(sized.sizeBytes == 64, "buffer desc carries size");
    Check(ixrhi::HasUsage(sized.usage, ixrhi::IXRHIBufferUsage::Vertex), "buffer usage flag");
    Check(!ixrhi::HasUsage(sized.usage, ixrhi::IXRHIBufferUsage::Index), "buffer usage negative");
    Check(sized.debugName == "SmokeVB", "debug name carried");

    Check(ixrhi::IXRHIFormatByteSize(ixrhi::IXRHIFormat::R32G32B32Float) == 12,
        "format R32G32B32 size 12");
    Check(ixrhi::IXRHIFormatByteSize(ixrhi::IXRHIFormat::R8G8B8A8Unorm) == 4,
        "format RGBA8 size 4");
    Check(ixrhi::IXRHIFormatByteSize(ixrhi::IXRHIFormat::Undefined) == 0,
        "format Undefined size 0");

    const ixrhi::IXRHICapabilities caps{};
    Check(!caps.supportsDynamicRendering, "dynamic rendering off by default");
    Check(!caps.supportsRayTracing, "ray tracing off by default");
    Check(caps.maxAnisotropy == 1, "anisotropy defaults 1");

    Check(std::strcmp(ixrhi::IXRHIResultName(ixrhi::IXRHIResult::Ok), "Ok") == 0,
        "result names");
    Check(std::strcmp(ixrhi::IXRHIResultName(ixrhi::IXRHIResult::DeviceLost), "DeviceLost") == 0,
        "result names lost");

    const ixrhi::IXRHIFrameInfo frame{};
    Check(!frame.frameActive && frame.frameIndex == 0, "frame info defaults");

    ixrhi::IXRHIGraphicsPipelineDesc pipeline{};
    Check(pipeline.topology == ixrhi::IXRHIPrimitiveTopology::TriangleList,
        "pipeline topology default");
    Check(!pipeline.depthTestEnable, "depth test default off");

    const ixrhi::IXRHISamplerDesc sampler{};
    Check(sampler.maxLod == 1.0f, "sampler lod default");

    Check(ixrhi::IXRHIFormatByteSize(ixrhi::IXRHIFormat::R8G8B8A8Srgb) == 4,
        "format sRGB RGBA8 size 4");

    ixrhi::IXRHIGraphicsPipelineDesc pipelineWithPass{};
    Check(pipelineWithPass.targetRenderPass == nullptr, "pipeline target pass defaults null");
    Check(pipelineWithPass.bindGroupLayouts.empty(), "pipeline layouts default empty");

    const ixrhi::IXRHITextureDesc texDesc{};
    Check(texDesc.sampleCount == 1, "texture samples default 1");

    const ixrhi::IXRHIRenderTargetDesc targetDesc{};
    Check(targetDesc.colorLoad == ixrhi::IXRHILoadOp::Clear, "target color load defaults Clear");
    Check(targetDesc.color == nullptr && targetDesc.depth == nullptr, "target textures default null");
    Check(targetDesc.clearColor[0] == 0.04f && targetDesc.clearColor[3] == 1.0f,
        "target clear color defaults");
    Check(targetDesc.clearDepth == 1.0f, "target clear depth defaults");

    const ixrhi::IXRHIImageLayout transferSrc = ixrhi::IXRHIImageLayout::TransferSrc;
    Check(transferSrc != ixrhi::IXRHIImageLayout::TransferDst, "transfer layouts distinct");
}

// CPU-only IXRHITexture/Sampler doubles for bridge registry tests (no GPU).
class StubTexture final : public ixrhi::IXRHITexture
{
public:
    std::uint32_t Width() const override { return 4; }
    std::uint32_t Height() const override { return 4; }
    ixrhi::IXRHIFormat Format() const override { return ixrhi::IXRHIFormat::R8G8B8A8Unorm; }
    const std::string& DebugName() const override { return m_name; }

private:
    std::string m_name = "Stub";
};

class StubSampler final : public ixrhi::IXRHISampler
{
public:
    const std::string& DebugName() const override { return m_name; }

private:
    std::string m_name = "StubSampler";
};

void TestEditorBridge()
{
    ixeditor::graphics::EditorGraphicsBridge bridge;
    Check(bridge.EntryCount() == 0, "bridge starts empty");

    auto texture = std::make_shared<StubTexture>();
    auto sampler = std::make_shared<StubSampler>();

    const auto nullHandle = bridge.Register(nullptr, sampler, "null");
    Check(!nullHandle.IsValid(), "bridge rejects null texture");
    Check(bridge.EntryCount() == 0, "bridge empty after rejected register");

    const auto first = bridge.Register(texture, sampler, "scene");
    Check(first.IsValid(), "bridge register valid");
    Check(bridge.EntryCount() == 1, "bridge count after register");

    std::shared_ptr<ixrhi::IXRHITexture> foundTexture;
    std::shared_ptr<ixrhi::IXRHISampler> foundSampler;
    Check(bridge.Lookup(first, foundTexture, foundSampler), "bridge lookup hit");
    Check(foundTexture == texture && foundSampler == sampler, "bridge lookup identity");

    bridge.Unregister(first);
    Check(bridge.EntryCount() == 0, "bridge count after unregister");
    Check(!bridge.Lookup(first, foundTexture, foundSampler), "bridge lookup miss after unregister");

    const auto second = bridge.Register(texture, sampler, "scene");
    Check(second.IsValid() && !(second == first), "bridge re-register yields new handle");
    bridge.Clear();
    Check(bridge.EntryCount() == 0, "bridge clear empties");
}

} // namespace

int main(int argc, char** argv)
{
    bool wantGpu = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--gpu") == 0)
            wantGpu = true;
    }

    TestDescriptorDefaults();
    TestEditorBridge();

#ifdef IXRHI_SMOKE_WITH_VULKAN
    extern void RunConversionChecks();
    RunConversionChecks();
#else
    std::printf("[SKIP] conversion checks (IXRHI_SMOKE_WITH_VULKAN off)\n");
#endif

    if (wantGpu)
    {
        ++g_ixrhiSmokeFailures;
        std::printf("[FAIL] --gpu requested but no headless device harness exists; "
                    "GPU paths are exercised by the migrated renderers in-editor\n");
    }
    else
    {
        std::printf("[SKIP] gpu device/buffer/pipeline tests (need window; see report)\n");
    }

    std::printf("[SUMMARY] failures=%d\n", g_ixrhiSmokeFailures);
    return g_ixrhiSmokeFailures == 0 ? 0 : 1;
}
