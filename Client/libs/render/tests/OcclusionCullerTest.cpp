#include "OcclusionCuller.h"
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#if defined(_WIN32)
#include "IXVulkanDevice.h"
#include "VulkanDevice.h"
#include "NativeWindow_Win32.h"
#include "asset/FileAssetReader.h"
#endif

namespace
{
int failures = 0;
void Check(bool condition, const char* name)
{
    std::cout << (condition ? "PASS " : "FAIL ") << name << '\n';
    failures += !condition;
}
WorldCamera IdentityCamera() { WorldCamera camera; camera.viewProjection = WorldIdentity(); return camera; }
}

struct OcclusionCullerTestAccess
{
    static void Cpu()
    {
        using C = OcclusionCuller;
        Check(sizeof(C::GpuBounds) == 32 && sizeof(C::Seed) == 32 && sizeof(C::Push) == 96, "shader layout sizes");
        Check(C::PyramidElements(1, 1) == 1, "single pixel pyramid");
        Check(C::PyramidElements(3, 5) == 15 + 6 + 2 + 1, "odd pyramid includes edge cells");
        Check(C::PyramidElements(0, 5) == 0, "empty pyramid rejected");
        Check(C::PyramidElements(0xffffffffu, 0xffffffffu) == 0, "oversized pyramid rejected");
        auto camera = IdentityCamera();
        C::Projection p;
        const C::Bounds box{{-0.6f, -0.7f, 0.6f}, {-0.4f, -0.5f, 0.7f}};
        Check(C::Project(camera, box, 100, 100, p) && std::abs(p.minX - 20) < 0.01f &&
            std::abs(p.minY - 15) < 0.01f && std::abs(p.nearestDepth - 0.6f) < 0.001f, "positive Vulkan viewport projection");
        Check(!C::Project(camera, {{-0.1f, -0.1f, -0.1f}, {0.1f, 0.1f, 0.5f}}, 100, 100, p), "near-plane crossing stays visible");
        Check(!C::Project(camera, {{-0.1f, -0.1f, 0.5f}, {0.1f, 0.1f, 1.1f}}, 100, 100, p), "far-plane crossing stays visible");
        Check(!C::Project(camera, {{-1.1f, -0.1f, 0.5f}, {0.1f, 0.1f, 0.6f}}, 100, 100, p), "viewport crossing stays visible");
        Check(!C::Project(camera, box, 0, 100, p), "zero viewport rejected");
        C::Bounds invalid = box; invalid.min.x = std::numeric_limits<float>::quiet_NaN();
        Check(!C::Project(camera, invalid, 100, 100, p), "nonfinite bounds stay visible");
        camera.viewProjection.m[15] = -1;
        Check(!C::Project(camera, box, 100, 100, p), "behind-camera bounds stay visible");
        C culler;
        Check(!culler.WantsView(0, 0), "unavailable backend stays direct");
        culler.m_ready = true;
        Check(culler.WantsView(0, 0) && !culler.WantsView(0, 1) && !culler.WantsView(0, 119) &&
            culler.WantsView(0, 120), "low-benefit views only probe periodically");
        culler.m_useful[0] = true;
        Check(culler.WantsView(0, 121) && culler.WantsView(1, 121) && !culler.WantsView(1, 122),
            "benefit and probe state independent per view");
        culler.m_ready = false;
        C::Slot slot;
        culler.m_slot = &slot;
        for (unsigned i = 0; i < C::kGroups; ++i) culler.AddGroup(box, 7);
        Check(culler.AddGroup(box, 1) == C::kInvalidGroup && culler.m_bounds.size() == C::kGroups,
            "group overflow keeps direct-draw fallback");
        culler.m_slot = nullptr;
        Check(culler.AddGroup(box, 1) == C::kInvalidGroup, "inactive view rejects group");
    }
#if defined(_WIN32)
    static void Gpu()
    {
        _putenv_s("IX_OCCLUSION_STATS", "1");
        _putenv_s("IX_OCCLUSION_CULLING", "1");
        NativeWindow_Win32 window;
        if (!window.Create(GetModuleHandle(nullptr), "Occlusion GPU regression", 96, 64)) { Check(false, "GPU window"); return; }
        ShowWindow(window.GetHwnd(), SW_HIDE);
        VulkanDevice native;
        if (!native.Create(window, 96, 64)) { Check(false, "GPU device"); window.Destroy(); return; }
        auto rhi = ixvulkan::CreateDevice(native);
        client::asset::FileAssetReader assets(IX_OCCLUSION_TEST_ASSETS_ROOT);
        OcclusionCuller culler;
        if (!culler.Create(*rhi, assets)) { Check(false, "GPU culler create"); rhi->Shutdown(); rhi.reset(); native.Destroy(); window.Destroy(); return; }
        struct Command { unsigned indices, instances, firstIndex; int vertexOffset; unsigned firstInstance; };
        static_assert(sizeof(Command) == 20);
        ixrhi::IXRHIBufferDesc readDesc;
        readDesc.sizeBytes = 8 * sizeof(Command); readDesc.usage = ixrhi::IXRHIBufferUsage::TransferDst;
        readDesc.cpuAccess = ixrhi::IXRHICpuAccess::Write;
        auto readback = rhi->CreateBuffer(readDesc, nullptr, 0);
        // Odd resolutions, every frame slot and both views; changing the depth
        // and camera between frames proves decisions use this frame, not history.
        for (unsigned test = 0; test < 8; ++test)
        {
            const unsigned width = test % 2 ? 67 : 71, height = test % 2 ? 35 : 39;
            std::vector<float> depth(width * height, 1);
            if (test != 3)
                for (unsigned y = 0; y < height / 2; ++y)
                    for (unsigned x = 0; x < width / 2; ++x) depth[y * width + x] = 0.3f;
            if (test == 4) depth[height / 5 * width + width / 6] = 1; // one-pixel opening
            ixrhi::IXRHITextureDesc tex;
            tex.width = width; tex.height = height; tex.format = ixrhi::IXRHIFormat::R32Float;
            tex.usage = ixrhi::IXRHITextureUsage::Sampled | ixrhi::IXRHITextureUsage::TransferDst;
            auto texture = rhi->CreateTexture(tex, depth.data(), depth.size() * sizeof(float));
            auto frame = rhi->BeginFrame();
            if (!frame) { Check(false, "GPU frame acquire"); break; }
            auto& cmd = *frame.info.commandList;
            auto camera = IdentityCamera();
            if (test == 5) camera.viewProjection.m[13] = 1.0f; // immediate camera jump to open area
            culler.BeginFrame(frame.info);
            const bool began = culler.BeginView(cmd, frame.info, camera, texture, test % 2);
            Check(began, "GPU view begin");
            if (!began) { rhi->EndFrame(frame); break; }
            const OcclusionCuller::Bounds boxes[] = {
                {{-0.75f, -0.7f, 0.6f}, {-0.6f, -0.5f, 0.7f}}, // behind wall
                {{0.5f, -0.7f, 0.6f}, {0.7f, -0.5f, 0.7f}}, // empty right
                {{-0.75f, 0.5f, 0.6f}, {-0.6f, 0.7f, 0.7f}}, // empty bottom (Y regression)
                {{-0.75f, -0.7f, 0.3f}, {-0.6f, -0.5f, 0.3f}}, // equal depth (no self-occlusion)
                {{-0.75f, -0.7f, -0.1f}, {-0.6f, -0.5f, 0.6f}}, // near plane
                {{-1.1f, -0.7f, 0.6f}, {-0.6f, -0.5f, 0.7f}}, // viewport border
            };
            for (const auto& box : boxes)
            {
                const auto group = culler.AddGroup(box, 3);
                culler.m_seeds.push_back({12, 3, 6, -2, 17, group});
            }
            auto output = culler.m_slot->indirect;
            culler.EndView(cmd);
            using S = ixrhi::IXRHIBufferState;
            cmd.TransitionBuffer(*output, S::IndirectRead, S::TransferSrc);
            cmd.TransitionBuffer(*readback, S::HostRead, S::TransferDst);
            cmd.CopyBuffer(*output, *readback, sizeof(boxes) / sizeof(boxes[0]) * sizeof(Command));
            cmd.TransitionBuffer(*readback, S::TransferDst, S::HostRead);
            cmd.TransitionBuffer(*output, S::TransferSrc, S::IndirectRead);
            rhi->GetMainRenderTarget()->Begin(cmd); rhi->GetMainRenderTarget()->End(cmd);
            rhi->EndFrame(frame);
            rhi->WaitIdle(); // test-only inspection; production never waits for visibility
            Command commands[6]{};
            readback->Read(0, commands, sizeof(commands));
            const bool behindVisible = test == 3 || test == 4 || test == 5;
            Check(commands[0].instances == (behindVisible ? 3u : 0u), "GPU wall/hole/moved-camera visibility");
            for (unsigned i = 1; i < 6; ++i)
                Check(commands[i].instances == 3, "GPU visible/clip/self-depth conservative");
            Check(commands[0].indices == 12 && commands[0].firstIndex == 6 && commands[0].vertexOffset == -2 &&
                commands[0].firstInstance == 17, "GPU indirect command preserves draw addressing");
        }
        rhi->WaitIdle(); culler.Destroy(); readback.reset();
        rhi->Shutdown(); rhi.reset(); native.Destroy(); window.Destroy();
    }
#endif
};

int main(int argc, char** argv)
{
    OcclusionCullerTestAccess::Cpu();
#if defined(_WIN32)
    if (argc > 1 && std::strcmp(argv[1], "--gpu") == 0) OcclusionCullerTestAccess::Gpu();
#endif
    return failures ? 1 : 0;
}
