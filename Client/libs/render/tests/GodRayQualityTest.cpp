#include "GodRayRenderer.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>
#if defined(_WIN32)
#include "IXVulkanDevice.h"
#include "VulkanDevice.h"
#include "NativeWindow_Win32.h"
#include "asset/FileAssetReader.h"
#include "IXRHIShader.h"
#endif

struct GodRayQualityTestAccess
{
    static bool ScreenVisible(GodRayRenderer& renderer, const SkySettings& sky,
                              const LightingState& lights, const WorldCamera& camera)
    {
        GodRayRenderer::Push push{};
        return renderer.BuildPush(sky, lights, camera, push) == nullptr;
    }
    static std::shared_ptr<ixrhi::IXRHITexture> Mask(GodRayRenderer& renderer, unsigned view)
    { return renderer.m_views[view].mask; }
};

namespace {
int failures = 0;
void Check(bool ok, const char* name)
{
    std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
    failures += !ok;
}
WorldCamera Camera()
{
    WorldCamera camera;
    camera.eye = {}; camera.target = {0, .173648f, .984808f}; camera.farPlane = 100;
    camera.viewProjection = WorldMultiply(WorldLookAt(camera.eye, camera.target, {0,1,0}),
        WorldPerspective(.8f, 1, camera.nearPlane, camera.farPlane));
    return camera;
}
LightingState Lighting()
{
    LightingState lights;
    lights.directional.enabled = true; lights.directional.azimuthDegrees = 0;
    lights.directional.elevationDegrees = 10; lights.directional.intensity = 1;
    lights.directional.r = lights.directional.g = lights.directional.b = 1;
    return lights;
}
void Visibility()
{
    GodRayRenderer renderer;
    auto camera = Camera(); auto lights = Lighting(); SkySettings sky; sky.godRays = true;
    Check(GodRayQualityTestAccess::ScreenVisible(renderer, sky, lights, camera), "sun-facing rays are visible");
    sky.godRays = false;
    Check(!GodRayQualityTestAccess::ScreenVisible(renderer, sky, lights, camera), "disabled rays are hidden");
    sky.godRays = true; sky.godRayTechnique = 1;
    Check(!GodRayQualityTestAccess::ScreenVisible(renderer, sky, lights, camera), "volumetric-only omits screen shafts");
    sky.godRayTechnique = 2; lights.directional.elevationDegrees = -10;
    Check(!GodRayQualityTestAccess::ScreenVisible(renderer, sky, lights, camera), "sunset omits screen shafts");
    lights = Lighting(); lights.directional.azimuthDegrees = 180;
    Check(!GodRayQualityTestAccess::ScreenVisible(renderer, sky, lights, camera), "sun behind camera omits screen shafts");
    lights = Lighting(); camera.viewProjection = {};
    Check(!GodRayQualityTestAccess::ScreenVisible(renderer, sky, lights, camera), "invalid camera omits rays");
}
#if defined(_WIN32)
void Gpu()
{
    NativeWindow_Win32 window;
    if (!window.Create(GetModuleHandle(nullptr), "God rays GPU regression", 64, 64)) { Check(false, "window"); return; }
    ShowWindow(window.GetHwnd(), SW_HIDE);
    VulkanDevice native;
    if (!native.Create(window, 64, 64)) { Check(false, "GPU"); window.Destroy(); return; }
    auto rhi = ixvulkan::CreateDevice(native);
    [&] {
        client::asset::FileAssetReader assets(IX_RAYS_TEST_ASSETS_ROOT), shaders(IX_RAYS_TEST_SHADER_ROOT);
        auto bytes = shaders.ReadAll("god_ray_quality_test.spv");
        if (!bytes) { Check(false, "readback shader"); return; }
        using S = ixrhi::IXRHIShaderStage; using B = ixrhi::IXRHIBindingType; using U = ixrhi::IXRHIBufferUsage;
        ixrhi::IXRHIShaderDesc shader; shader.stage = S::Compute; shader.entryPoint = "CSMain";
        auto words = reinterpret_cast<const std::uint32_t*>(bytes->data());
        shader.spirv.assign(words, words + bytes->size() / 4);
        auto cs = rhi->CreateShader(shader);
        Check(cs != nullptr, "readback shader created");
        auto layout = rhi->CreateBindGroupLayout({{0, B::StorageBuffer, S::Compute}, {1, B::SampledTexture, S::Compute}});
        auto bindings = rhi->CreateBindGroup(*layout, 6);
        ixrhi::IXRHIComputePipelineDesc pipelineDesc; pipelineDesc.computeShader = cs;
        pipelineDesc.bindGroupLayouts = {layout.get()}; pipelineDesc.pushRanges = {{S::Compute, 0, 4}};
        auto compute = rhi->CreateComputePipeline(pipelineDesc); auto sampler = rhi->CreateSampler({});
        Check(compute && sampler && bindings, "GPU readback pipeline");
        constexpr unsigned samples = 64 * 64, bufferBytes = samples * 6 * sizeof(float);
        ixrhi::IXRHIBufferDesc buffer; buffer.sizeBytes = bufferBytes; buffer.usage = U::Storage | U::TransferSrc;
        auto output = rhi->CreateBuffer(buffer, nullptr, 0);
        buffer.usage = U::TransferDst; buffer.cpuAccess = ixrhi::IXRHICpuAccess::Write;
        auto host = rhi->CreateBuffer(buffer, nullptr, 0);
        Check(output && host, "GPU readback buffers");
        for (unsigned set = 0; set < 6; ++set) bindings->UpdateBuffer(set, 0, output, 0, bufferBytes);
        std::vector<std::shared_ptr<ixrhi::IXRHITexture>> depths, shadows;
        constexpr unsigned depthWidth = 130, depthHeight = 126;
        ixrhi::IXRHITextureDesc texture; texture.width = depthWidth; texture.height = depthHeight;
        texture.format = ixrhi::IXRHIFormat::D32Float; texture.usage = ixrhi::IXRHITextureUsage::Sampled;
        std::vector<float> pixels(depthWidth * depthHeight);
        for (int mode = 0; mode < 4; ++mode)
        {
            for (unsigned y = 0; y < depthHeight; ++y)
                for (unsigned x = 0; x < depthWidth; ++x)
                    pixels[y * depthWidth + x] = mode == 0 ? 1.f : mode == 1 ? .5f : ((x + y + mode) % 2 ? .5f : 1.f);
            depths.push_back(rhi->CreateTexture(texture, pixels.data(), pixels.size() * sizeof(float)));
            Check(depths.back() != nullptr, "depth fixture texture");
        }
        texture.width = texture.height = 128; texture.arrayLayers = 4; pixels.resize(128 * 128 * 4);
        for (int mode = 0; mode < 3; ++mode)
        {
            for (unsigned i = 0; i < pixels.size(); ++i)
                pixels[i] = mode == 0 ? 1.f : mode == 1 ? .3f : (i % 128 < 64 ? .3f : 1.f);
            shadows.push_back(rhi->CreateTexture(texture, pixels.data(), pixels.size() * sizeof(float)));
        }
        GodRayRenderer renderer;
        if (!renderer.Create(*rhi, assets)) { Check(false, "actual god rays pipelines"); return; }
        auto camera = Camera(); auto lights = Lighting(); SkySettings sky;
        auto otherCamera = camera; otherCamera.eye.x = otherCamera.target.x = 35;
        otherCamera.viewProjection = WorldMultiply(WorldLookAt(otherCamera.eye, otherCamera.target, {0,1,0}),
            WorldPerspective(.8f, 1, otherCamera.nearPlane, otherCamera.farPlane));
        sky.godRays = true; sky.godRayTechnique = 2; sky.godRayLength = .6f;
        sky.volumetricDensity = .01f; sky.volumetricDistance = 20;
        WorldMat4 cascades[4];
        for (int c = 0; c < 4; ++c)
        {
            cascades[c] = WorldIdentity(); cascades[c].m[0] = cascades[c].m[5] = .05f / (1 << c);
            cascades[c].m[10] = .001f; cascades[c].m[14] = .5f;
        }
        using State = ixrhi::IXRHIBufferState;
        auto outputState = State::Undefined, hostState = State::Undefined;
        std::vector<float> readback(samples * 6), clearMask(3), clearRays(3);
        for (int quality = 0; quality < 3; ++quality)
        {
            sky.godRayQuality = quality; sky.volumetricQuality = 2 - quality;
            std::vector<float> checker;
            for (int mode = 0; mode < 4; ++mode)
            {
                GodRayRenderer::SunShadow shadow{shadows[mode == 1 ? 1 : mode >= 2 ? 2 : 0], cascades, 0, 0};
                auto frame = rhi->BeginFrame(); if (!frame) { Check(false, "frame"); return; }
                auto& cmd = *frame.info.commandList;
                for (unsigned view = 0; view < 2; ++view)
                {
                    const int depthMode = view == 1 && mode == 1 ? 0 : mode;
                    Check(renderer.RenderRays(cmd, frame.info, view, sky, lights, view ? otherCamera : camera, depths[depthMode], shadow, depthWidth, depthHeight),
                        "both camera/frame slots render rays");
                    GodRayRenderer::AddedLight rays; Check(renderer.TakeAddedLight(view, rays), "per-view light available");
                    auto produced = rays;
                    Check(!renderer.TakeAddedLight(view, rays), "light consumed once");
                    const unsigned divisor = quality == 2 ? 2 : 4;
                    auto mask = GodRayQualityTestAccess::Mask(renderer, view);
                    Check(mask->Width() == (depthWidth + divisor - 1) / divisor &&
                        mask->Height() == (depthHeight + divisor - 1) / divisor, "quality resolution rounds odd extents up");
                    std::shared_ptr<ixrhi::IXRHITexture> reads[3] = {mask, produced.shafts, produced.volume};
                    cmd.TransitionBuffer(*output, outputState, State::ShaderWrite);
                    for (unsigned kind = 0; kind < 3; ++kind)
                    {
                        const unsigned set = view * 3 + kind, offset = set * samples;
                        bindings->UpdateTexture(set, 1, reads[kind], sampler);
                        cmd.SetComputePipeline(*compute); cmd.BindGroup(0, *bindings, set);
                        cmd.PushConstants(&offset, 4); cmd.Dispatch(8, 8, 1);
                    }
                    outputState = State::ShaderWrite;
                }
                cmd.TransitionBuffer(*output, State::ShaderWrite, State::TransferSrc); outputState = State::TransferSrc;
                cmd.TransitionBuffer(*host, hostState, State::TransferDst); cmd.CopyBuffer(*output, *host, bufferBytes);
                cmd.TransitionBuffer(*host, State::TransferDst, State::HostRead); hostState = State::HostRead;
                rhi->GetMainRenderTarget()->Begin(cmd); rhi->GetMainRenderTarget()->End(cmd);
                rhi->EndFrame(frame); rhi->WaitIdle(); host->Read(0, readback.data(), bufferBytes);
                const auto mean = [&](unsigned slot) {
                    return std::accumulate(readback.begin() + slot * samples, readback.begin() + (slot + 1) * samples, 0.f) / samples;
                };
                Check(std::all_of(readback.begin(), readback.end(), [](float v) { return std::isfinite(v) && v >= 0; }), "finite nonnegative GPU light");
                std::cout << "quality=" << quality << " mode=" << mode << " mask=" << mean(0)
                          << " rays=" << mean(1) << " volume=" << mean(2) << '\n';
                if (mode == 0) { clearMask[quality] = mean(0); clearRays[quality] = mean(1); Check(mean(2) > .001f, "lit air scatters light"); }
                if (mode == 1)
                {
                    Check(mean(0) < 1e-6f && mean(1) < 1e-6f, "solid occluder leaks no screen light");
                    Check(mean(3) > .01f && mean(4) > .01f, "hidden Scene cannot contaminate Game rays");
                    Check(mean(2) < 1e-6f && mean(5) < 1e-6f, "fully shadowed air leaks no volume light");
                }
                if (mode >= 2)
                {
                    Check(std::abs(mean(0) / clearMask[quality] - .5f) < .015f, "subpixel leaf gaps retain half sky coverage");
                    Check(std::abs(mean(1) / clearRays[quality] - .5f) < .025f, "filtered leaf gaps retain half ray energy");
                    if (mode == 2) checker.assign(readback.begin(), readback.begin() + samples * 3);
                    else
                    {
                        float difference = 0;
                        for (unsigned i = 0; i < samples * 3; ++i) difference = std::max(difference, std::abs(checker[i] - readback[i]));
                        Check(difference < .002f, "one-pixel leaf shift produces no screen or volumetric lighting pop");
                    }
                }
            }
        }
        const auto range = std::minmax_element(clearRays.begin(), clearRays.end());
        Check(*range.second / *range.first < 1.1f, "quality preserves ray brightness and falloff");
        sky.godRays = false;
        auto frame = rhi->BeginFrame();
        if (frame)
        {
            for (unsigned view = 0; view < 2; ++view)
            {
                Check(!renderer.RenderRays(*frame.info.commandList, frame.info, view, sky, lights, camera,
                    depths[0], {shadows[0], cascades, 0, 0}, 129, 127), "disabled rays skip rendering");
                GodRayRenderer::AddedLight light;
                Check(!renderer.TakeAddedLight(view, light), "disabled rays clear stale per-view light");
            }
            rhi->GetMainRenderTarget()->Begin(*frame.info.commandList); rhi->GetMainRenderTarget()->End(*frame.info.commandList);
            rhi->EndFrame(frame);
        }
        rhi->WaitIdle(); renderer.Destroy();
    }();
    rhi->Shutdown(); rhi.reset(); native.Destroy(); window.Destroy();
}
#endif
}
int main(int argc, char** argv)
{
    std::cout << std::unitbuf; Visibility();
#if defined(_WIN32)
    if (argc > 1 && std::string(argv[1]) == "--gpu") Gpu();
#endif
    return failures ? 1 : 0;
}
