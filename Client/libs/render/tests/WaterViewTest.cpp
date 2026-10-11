#include "SceneManager.h"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#if defined(_WIN32)
#include "TerrainRenderer.h"
#include "SkyRenderer.h"
#include "OffscreenSceneRenderer.h"
#include "IXVulkanDevice.h"
#include "VulkanDevice.h"
#include "NativeWindow_Win32.h"
#include "asset/FileAssetReader.h"
#include "IXRHIShader.h"
#endif

namespace {
int failures = 0;
void Check(bool ok, const char* name) { std::cout << (ok ? "PASS " : "FAIL ") << name << '\n'; failures += !ok; }
void Serialization()
{
    namespace fs = std::filesystem;
    const auto parent = fs::weakly_canonical(fs::temp_directory_path());
    const auto root = parent / ("ixw_water_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    SceneData scene;
    WaterBody body; body.id = 1; body.name = "Lake"; body.bboxMin[0] = body.bboxMin[1] = -2;
    body.bboxMax[0] = body.bboxMax[1] = 2; body.waterLevelY = 0.6f;
    body.maskWidth = body.maskHeight = 1; body.shapeMask = {255};
    auto& c = body.config;
    c.reflectionEnabled = false; c.refractionEnabled = false; c.foamEnabled = false;
    c.reflectionQuality = WaterConfig::ReflectionQuality::Quarter; c.causticMode = WaterConfig::CausticMode::Off;
    c.normalStrength = 0.32f; c.waveSpeedSmall = 0.012f; c.waveSpeedLarge = 0.008f;
    c.waveScaleSmall = 0.021f; c.waveScaleLarge = 0.013f; c.fresnelPower = 7; c.fresnelMin = 0.13f;
    c.reflectionDistortionStrength = 0.027f; c.reflectionColor[0] = 0.21f;
    c.depthColorMin = 0.8f; c.depthColorMax = 5; c.depthFadeDistance = 19;
    c.refractionStrength = 0.031f; c.refractionDepthStrength = 0.42f;
    c.foamDistance = 0.11f; c.foamSoftness = 0.043f; c.foamScrollSpeed = 0.071f;
    c.foamScale = 0.87f; c.foamTerrainThickness = 0.14f;
    c.causticIntensity = 0.27f; c.causticScale = 0.41f; c.causticSpeed = 0.82f; c.causticMaxDepth = 17;
    scene.waterBodies = {body};
    auto& manager = SceneManager::Instance(); manager.RestoreSceneSnapshot(scene, {}, false);
    const bool saved = manager.SaveSceneAs((root / "lake.scene").string()); manager.CloseScene();
    const bool loaded = saved && manager.LoadScene((root / "lake.scene").string());
    Check(loaded && manager.GetCurrentScene().waterBodies.size() == 1, "water scene round trip");
    if (loaded && manager.GetCurrentScene().waterBodies.size() == 1)
    {
        const auto& r = manager.GetCurrentScene().waterBodies[0].config;
        Check(!r.reflectionEnabled && !r.refractionEnabled && !r.foamEnabled &&
            r.reflectionQuality == c.reflectionQuality && r.causticMode == c.causticMode,
            "scene keeps disabled effects and reflection quality");
        Check(std::abs(r.normalStrength - c.normalStrength) < 1e-6f &&
            r.waveSpeedSmall == c.waveSpeedSmall && r.waveSpeedLarge == c.waveSpeedLarge &&
            r.waveScaleSmall == c.waveScaleSmall && r.waveScaleLarge == c.waveScaleLarge &&
            r.fresnelPower == c.fresnelPower && r.fresnelMin == c.fresnelMin &&
            r.reflectionDistortionStrength == c.reflectionDistortionStrength && r.reflectionColor[0] == c.reflectionColor[0] &&
            r.depthColorMin == c.depthColorMin && r.depthColorMax == c.depthColorMax && r.depthFadeDistance == c.depthFadeDistance &&
            r.refractionStrength == c.refractionStrength && r.refractionDepthStrength == c.refractionDepthStrength &&
            r.foamDistance == c.foamDistance && r.foamSoftness == c.foamSoftness && r.foamScrollSpeed == c.foamScrollSpeed &&
            r.foamScale == c.foamScale && r.foamTerrainThickness == c.foamTerrainThickness &&
            r.causticIntensity == c.causticIntensity && r.causticScale == c.causticScale &&
            r.causticSpeed == c.causticSpeed && r.causticMaxDepth == c.causticMaxDepth,
            "scene keeps ripple, optical, foam and caustic parameters");
    }
    manager.CloseScene();
    { std::ofstream f(root / "legacy.scene"); f << R"({"version":1,"name":"Legacy","entities":[{"type":"water_body","id":1,"name":"Water","water_config":{"enabled":true}}]})"; }
    Check(manager.LoadScene((root / "legacy.scene").string()), "legacy water scenes still load");
    manager.CloseScene();
    if (root.parent_path() == parent && root.filename().string().starts_with("ixw_water_test_")) fs::remove_all(root);
}
#if defined(_WIN32)
void Gpu()
{
    NativeWindow_Win32 window;
    if (!window.Create(GetModuleHandle(nullptr), "Water GPU regression", 64, 64)) { Check(false, "window"); return; }
    ShowWindow(window.GetHwnd(), SW_HIDE);
    VulkanDevice native;
    if (!native.Create(window, 64, 64)) { Check(false, "GPU"); window.Destroy(); return; }
    auto rhi = ixvulkan::CreateDevice(native);
    [&] {
        client::asset::FileAssetReader assets(IX_WATER_TEST_ASSETS_ROOT), shaders(IX_WATER_TEST_SHADER_ROOT);
        const auto load = [&](const char* path, const char* entry, ixrhi::IXRHIShaderStage stage) {
            ixrhi::IXRHIShaderDesc desc; desc.stage = stage; desc.entryPoint = entry;
            auto bytes = shaders.ReadAll(path);
            if (!bytes) return std::shared_ptr<ixrhi::IXRHIShader>{};
            auto words = reinterpret_cast<const std::uint32_t*>(bytes->data());
            desc.spirv.assign(words, words + bytes->size() / 4); return rhi->CreateShader(desc);
        };
        using S = ixrhi::IXRHIShaderStage; using B = ixrhi::IXRHIBindingType; using U = ixrhi::IXRHIBufferUsage;
        auto vs = load("water_test_VSMain.spv", "VSMain", S::Vertex), ps = load("water_test_PSMain.spv", "PSMain", S::Fragment);
        auto cs = load("water_test_CSMain.spv", "CSMain", S::Compute);
        auto colorLayout = rhi->CreateBindGroupLayout({{0, B::UniformBuffer, S::Fragment}});
        auto readLayout = rhi->CreateBindGroupLayout({{1, B::StorageBuffer, S::Compute}, {2, B::SampledTexture, S::Compute}, {3, B::SampledTexture, S::Compute}});
        auto colors = rhi->CreateBindGroup(*colorLayout, 2), reads = rhi->CreateBindGroup(*readLayout, 1);
        auto buffer = [&](uint64_t size, U usage, bool host, const void* data = nullptr) {
            ixrhi::IXRHIBufferDesc d; d.sizeBytes = size; d.usage = usage;
            d.cpuAccess = host ? ixrhi::IXRHICpuAccess::Write : ixrhi::IXRHICpuAccess::None;
            return rhi->CreateBuffer(d, data, data ? size_t(size) : 0);
        };
        const float red[] = {1,0,0,1}, green[] = {0,1,0,1};
        auto redBuffer = buffer(16, U::Uniform, true, red), greenBuffer = buffer(16, U::Uniform, true, green);
        colors->UpdateBuffer(0, 0, redBuffer, 0, 16); colors->UpdateBuffer(1, 0, greenBuffer, 0, 16);
        auto output = buffer(32, U::Storage | U::TransferSrc, false), host = buffer(32, U::TransferDst, true);
        reads->UpdateBuffer(0, 1, output, 0, 32);
        ixrhi::IXRHIComputePipelineDesc cd; cd.computeShader = cs; cd.bindGroupLayouts = {readLayout.get()};
        auto compute = rhi->CreateComputePipeline(cd); auto sampler = rhi->CreateSampler({});
        OffscreenSceneRenderer scene, game;
        const auto depth = rhi->GetMainSwapchain().DepthFormat();
        Check(scene.Create(*rhi, assets, 64, 64, OffscreenSceneRenderer::kSceneColorFormat, depth, "Water.Scene") &&
            game.Create(*rhi, assets, 64, 64, OffscreenSceneRenderer::kSceneColorFormat, depth, "Water.Game"), "two offscreen views");
        TerrainRenderer terrain; terrain.SetTargetPass(scene.GetTargetPass());
        Check(terrain.Create(*rhi, assets), "actual terrain and water pipelines");
        TerrainSceneData ground; ground.exists = true; ground.widthMeters = ground.depthMeters = 8;
        ground.cellsX = ground.cellsZ = 8; ground.cellSizeMeters = 1; ground.heightCmGrid.assign(81, -200);
        Check(terrain.CreateFlatTerrain(*rhi, ground), "water test terrain");
        auto solid = [&](const ixrhi::IXRHIRenderPass* pass) {
            ixrhi::IXRHIGraphicsPipelineDesc d; d.vertexShader = vs; d.fragmentShader = ps;
            d.bindGroupLayouts = {colorLayout.get()}; d.cullMode = ixrhi::IXRHICullMode::None;
            d.depthTestEnable = false; d.depthWriteEnable = false; d.sampleCount = 1;
            d.blendAttachments = {{}}; d.targetRenderPass = pass; return rhi->CreateGraphicsPipeline(d);
        };
        auto fill = solid(scene.GetTargetPass());
        SkyRenderer sky;
        Check(sky.Create(*rhi, assets), "sky reflection pipelines");
        WaterBody body; body.id = 1; body.bboxMin[0] = body.bboxMin[1] = -3;
        body.bboxMax[0] = body.bboxMax[1] = 3; body.maskWidth = body.maskHeight = 8; body.shapeMask.assign(64, 255);
        body.config.foamEnabled = false; body.config.normalStrength = 0; body.config.refractionStrength = 0;
        body.config.depthFadeDistance = 50; body.config.baseColor[3] = 1; body.config.fresnelMin = 0.5f;
        WorldCamera camera; camera.eye = {0,4,0}; camera.target = {}; camera.nearPlane = 0.1f; camera.farPlane = 100;
        camera.viewProjection = {}; camera.viewProjection.m[0] = .25f; camera.viewProjection.m[9] = .25f;
        camera.viewProjection.m[6] = -.1f; camera.viewProjection.m[14] = .5f; camera.viewProjection.m[15] = 1;
        using L = ixrhi::IXRHIImageLayout; using State = ixrhi::IXRHIBufferState;
        auto outState = State::Undefined, hostState = State::Undefined;
        for (int fixture = 0; fixture < 8; ++fixture)
        {
            std::vector<std::unique_ptr<ixrhi::IXRHIGraphicsPipeline>> reflectionFills;
            const bool reflection = fixture >= 4;
            body.config.reflectionEnabled = reflection; body.config.refractionEnabled = !reflection;
            Check(terrain.SetWaterBodies(*rhi, {body}), "water body setup");
            auto frame = rhi->BeginFrame(); if (!frame) { Check(false, "frame"); break; }
            auto& cmd = *frame.info.commandList;
            sky.Update(frame.info, {}, {}, {});
            // Both views in one command buffer, then only Game on the next frame. Color inputs
            // and descriptors must remain independent when Scene is stale or absent.
            for (unsigned view = fixture & 1 ? 1u : 0u; view < 2; ++view)
            {
                auto& target = view ? game : scene;
                if (reflection)
                    terrain.RenderWaterReflection(cmd, frame.info, camera, 64, 64, fixture,
                        [&](const WorldCamera& mirror, uint32_t width, uint32_t height, const ixrhi::IXRHIRenderPass* pass, float) {
                            sky.Render(cmd, frame.info, mirror, width, height, pass, view);
                            reflectionFills.push_back(solid(pass));
                            cmd.SetGraphicsPipeline(*reflectionFills.back()); cmd.BindGroup(0, *colors, view); cmd.Draw(3, 1, 0, 0);
                        }, view);
                target.BeginMainPass(cmd, frame.info);
                if (!reflection)
                {
                    cmd.SetViewport(0, 0, 64, 64); cmd.SetScissor(0, 0, 64, 64);
                    cmd.SetGraphicsPipeline(*fill); cmd.BindGroup(0, *colors, view); cmd.Draw(3, 1, 0, 0);
                }
                cmd.ClearDepth(0.99f, 0, 0, 64, 64); target.EndMainPass(cmd);
                target.SnapshotScene(cmd, frame.info);
                terrain.SetWaterRefractionInputs(target.GetColorSnapshotTexture(), target.GetDepthSnapshotTexture(), sampler, 64, 64, view);
                target.BeginMainPass(cmd, frame.info, false);
                terrain.RenderWater(cmd, frame.info, camera, fixture, 64, 64, view);
                target.EndMainPass(cmd); target.SnapshotScene(cmd, frame.info);
            }
            reads->UpdateTexture(0, 2, scene.GetColorSnapshotTexture(), sampler);
            reads->UpdateTexture(0, 3, game.GetColorSnapshotTexture(), sampler);
            cmd.TransitionBuffer(*output, outState, State::ShaderWrite); outState = State::TransferSrc;
            cmd.SetComputePipeline(*compute); cmd.BindGroup(0, *reads, 0); cmd.Dispatch(1, 1, 1);
            cmd.TransitionBuffer(*output, State::ShaderWrite, State::TransferSrc);
            cmd.TransitionBuffer(*host, hostState, State::TransferDst); cmd.CopyBuffer(*output, *host, 32);
            cmd.TransitionBuffer(*host, State::TransferDst, State::HostRead); hostState = State::HostRead;
            rhi->GetMainRenderTarget()->Begin(cmd); rhi->GetMainRenderTarget()->End(cmd);
            rhi->EndFrame(frame); rhi->WaitIdle();
            float pixels[8]{}; host->Read(0, pixels, sizeof(pixels));
            std::cout << "fixture " << fixture << " scene " << pixels[0] << ',' << pixels[1]
                << " game " << pixels[4] << ',' << pixels[5] << '\n';
            Check(pixels[0] > pixels[1] + .1f && pixels[5] > pixels[4] + .1f,
                reflection ? "reflections belong to their own camera across frame slots and hidden Scene" :
                    "refraction never projects Scene color into Game across frame slots and hidden Scene");
        }
        rhi->WaitIdle(); sky.Destroy(); terrain.Destroy(); scene.Destroy(); game.Destroy();
    }();
    rhi->Shutdown(); rhi.reset(); native.Destroy(); window.Destroy();
}
#endif
}
int main(int argc, char** argv)
{
    std::cout << std::unitbuf; Serialization();
#if defined(_WIN32)
    if (argc > 1 && std::string(argv[1]) == "--gpu") Gpu();
#endif
    return failures ? 1 : 0;
}
