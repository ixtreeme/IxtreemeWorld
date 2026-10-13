#include "FogMath.h"
#include "SceneManager.h"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>

#if defined(_WIN32)
#include "OffscreenSceneRenderer.h"
#include "SceneClearColor.h"
#include "IXVulkanDevice.h"
#include "VulkanDevice.h"
#include "NativeWindow_Win32.h"
#include "asset/FileAssetReader.h"
#include "IXRHIShader.h"
#endif

#if defined(_WIN32)
struct FogRendererTestAccess
{
    static void Draw(OffscreenSceneRenderer& view, ixrhi::IXRHICommandList& cmd,
        const ixrhi::IXRHIFrameInfo& frame)
    {
        view.DrawToneMap(cmd, frame, *view.m_displayPipeline, {0, 1});
    }
};
#endif

namespace {
namespace xm = ixtreeme::math;
int failures = 0;
void Check(bool ok, const char* label)
{
    std::cout << (ok ? "PASS " : "FAIL ") << label << '\n';
    failures += !ok;
}
bool Near(float a, float b, float tolerance = 0.0001f) { return std::abs(a - b) < tolerance; }
WorldCamera Camera(WorldVec3 eye = {})
{
    WorldCamera camera;
    camera.viewProjection = WorldIdentity(); camera.eye = eye; camera.farPlane = 1000;
    return camera;
}
MeshSceneEntity Volume()
{
    const float position[] = {0, 0, 10};
    auto entity = MakeEmptySceneEntity(1, "Fog", position);
    entity.hasFogVolume = true;
    entity.fogVolume.halfExtents[0] = 2; entity.fogVolume.halfExtents[1] = 3; entity.fogVolume.halfExtents[2] = 4;
    entity.fogVolume.density = 0.1f; entity.fogVolume.edgeFade = 0;
    return entity;
}

void Math()
{
    FogUniforms fog;
    GlobalFogSettings global;
    auto camera = Camera();
    Check(!BuildFogUniforms(fog, camera, global, {}), "old and new scenes have no fog until enabled");
    Check(EvaluateFog(fog, {0, 0, 1}, 100).transmission == 1, "disabled fog preserves the scene");
    global.enabled = true; global.maxOpacity = 1;
    BuildFogUniforms(fog, camera, global, {});
    Check(Near(EvaluateFog(fog, {0, 0, 1}, 100).transmission, std::exp(-1.0f)), "global exponential density uses metres");
    global.startDistance = 20;
    BuildFogUniforms(fog, camera, global, {});
    Check(EvaluateFog(fog, {0, 0, 1}, 15).transmission == 1 &&
        Near(EvaluateFog(fog, {0, 0, 1}, 120).transmission, std::exp(-1.0f)), "global start distance leaves the near field clear");
    global.maxDistance = 120;
    BuildFogUniforms(fog, camera, global, {});
    Check(Near(EvaluateFog(fog, {0, 0, 1}, 500).transmission, std::exp(-1.0f)), "global distance cap stops accumulation");
    global.maxOpacity = 0.4f; global.affectSky = false;
    BuildFogUniforms(fog, camera, global, {});
    Check(Near(EvaluateFog(fog, {0, 0, 1}, 500).transmission, 0.6f) &&
        EvaluateFog(fog, {0, 0, 1}, 500, true).transmission == 1, "global opacity cap and sky toggle work");
    global = {}; global.enabled = true; global.heightEnabled = true; global.heightFalloff = 0.1f; global.maxOpacity = 1;
    BuildFogUniforms(fog, camera, global, {});
    Check(Near(GlobalFogOpticalDepth(fog, {0, 1, 0}, 10), 0.01f * (1 - std::exp(-1.0f)) / 0.1f),
        "height fog integrates the upward ray analytically");
    camera.eye.y = 10;
    BuildFogUniforms(fog, camera, global, {});
    Check(Near(GlobalFogOpticalDepth(fog, {0, -1, 0}, 20), 0.1f + 0.01f * (1 - std::exp(-1.0f)) / 0.1f),
        "height fog handles crossing the base height");
    Check(Near(GlobalFogOpticalDepth(fog, {0, 0, 1}, 100), std::exp(-1.0f)), "horizontal height ray has no division by zero");

    auto volume = Volume();
    auto gpu = BuildFogVolume(volume);
    Check(Near(LocalFogOpticalDepth(gpu, {}, {0, 0, 1}, 100), 0.8f), "box integrates the exact entry and exit with zero object scale");
    Check(LocalFogOpticalDepth(gpu, {}, {0, 0, 1}, 5) == 0 &&
        Near(LocalFogOpticalDepth(gpu, {}, {0, 0, 1}, 8), 0.2f), "scene depth clips local fog behind surfaces");
    Check(LocalFogOpticalDepth(gpu, {3, 0, 0}, {0, 0, 1}, 100) == 0 &&
        LocalFogOpticalDepth(gpu, {0, 0, 20}, {0, 0, 1}, 100) == 0, "parallel misses and volumes behind the camera are clear");
    Check(Near(LocalFogOpticalDepth(gpu, {0, 0, 10}, {0, 0, 1}, 100), 0.4f), "camera inside a box only integrates the remaining path");
    volume.rotation[1] = xm::HalfPi;
    gpu = BuildFogVolume(volume);
    Check(Near(LocalFogOpticalDepth(gpu, {}, {0, 0, 1}, 100), 0.4f), "local box follows the object's rotation");
    volume.fogVolume.shape = FogVolumeComponent::Shape::Sphere; volume.fogVolume.radius = 3;
    gpu = BuildFogVolume(volume);
    Check(Near(LocalFogOpticalDepth(gpu, {}, {0, 0, 1}, 100), 0.6f) &&
        Near(LocalFogOpticalDepth(gpu, {0, 0, 10}, {0, 0, 1}, 100), 0.3f), "sphere intersections work inside and outside");
    volume.fogVolume.edgeFade = 1;
    gpu = BuildFogVolume(volume);
    Check(LocalFogOpticalDepth(gpu, {}, {0, 0, 1}, 100) < 0.6f &&
        LocalFogOpticalDepth(gpu, {2.99f, 0, 0}, {0, 0, 1}, 100) < 0.001f, "soft boundary fades toward a sphere's edge");

    std::vector<MeshSceneEntity> entities{Volume(), Volume()};
    entities[1].fogVolume.color[0] = 1; entities[1].fogVolume.color[1] = 0;
    camera = Camera(); global = {};
    Check(BuildFogUniforms(fog, camera, global, entities), "local fog works with global fog disabled");
    const auto combined = EvaluateFog(fog, {0, 0, 1}, 100);
    std::swap(entities[0], entities[1]); BuildFogUniforms(fog, camera, global, entities);
    const auto reversed = EvaluateFog(fog, {0, 0, 1}, 100);
    Check(Near(combined.transmission, std::exp(-1.6f)) && Near(combined.transmission, reversed.transmission) &&
        xm::Length(combined.color - reversed.color) < 0.00001f, "overlapping fog combines without entity-order changes");
    global.enabled = true; global.maxDistance = 1;
    BuildFogUniforms(fog, camera, global, entities);
    Check(Near(EvaluateFog(fog, {0, 0, 1}, 100).transmission, std::exp(-1.61f)), "global max distance never clips local volumes");
    entities[0].effectiveEnabled = false; entities[1].fogVolume.enabled = false; global.enabled = false;
    Check(!BuildFogUniforms(fog, camera, global, entities), "component and ancestor disable stop local fog");
    entities = {Volume()}; entities[0].editorHidden = true;
    Check(!BuildFogUniforms(fog, camera, global, entities, true) &&
        BuildFogUniforms(fog, camera, global, entities, false), "editor eye hides Scene fog but preserves Game fog");
    entities.assign(40, Volume()); BuildFogUniforms(fog, camera, global, entities);
    Check(fog.options[2] == 32, "local volume budget is bounded");
    // Farthest first in the scene: the budget must still go to the volumes nearest the camera.
    for (std::size_t i = 0; i < entities.size(); ++i) entities[i].position[2] = 1000.0f - 20.0f * static_cast<float>(i);
    BuildFogUniforms(fog, camera, global, entities);
    bool nearestKept = fog.options[2] == 32;
    for (std::size_t i = 0; i < 32 && nearestKept; ++i) nearestKept = fog.volumes[i].centerShape[2] <= 1000.0f - 20.0f * 8 + 0.01f;
    Check(nearestKept && Near(fog.volumes[0].centerShape[2], 1000.0f - 20.0f * 39), "over budget, the volumes nearest to the camera are kept");
    camera.viewProjection = {};
    Check(!BuildFogUniforms(fog, camera, global, entities), "singular camera matrix disables fog safely");
    global.density = std::numeric_limits<float>::quiet_NaN(); global.maxDistance = -5; global.startDistance = 500;
    SanitizeFog(global);
    Check(std::isfinite(global.density) && global.maxDistance == 0.1f && global.startDistance == 0.1f, "invalid global settings are sanitized");
    volume.fogVolume.halfExtents[0] = -5; volume.fogVolume.edgeFade = 900; volume.fogVolume.shape = FogVolumeComponent::Shape::Box;
    SanitizeFog(volume.fogVolume);
    Check(volume.fogVolume.halfExtents[0] == 0.01f && volume.fogVolume.edgeFade == 0.01f, "invalid volume sizes and fade are sanitized");
}

void Serialization()
{
    namespace fs = std::filesystem;
    const auto parent = fs::weakly_canonical(fs::temp_directory_path());
    const auto root = parent / ("ixw_fog_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (!fs::create_directory(root)) { Check(false, "temporary scene directory"); return; }
    SceneData scene;
    scene.sky.fog.enabled = true; scene.sky.fog.heightEnabled = true; scene.sky.fog.affectSky = false;
    scene.sky.fog.density = 0.037f; scene.sky.fog.startDistance = 13; scene.sky.fog.maxDistance = 333;
    scene.sky.fog.maxOpacity = 0.74f; scene.sky.fog.baseHeight = -2; scene.sky.fog.heightFalloff = 0.15f;
    scene.sky.fog.color[0] = 0.23f;
    scene.meshEntities = {Volume(), Volume()}; scene.meshEntities[1].id = 2;
    scene.meshEntities[1].fogVolume.shape = FogVolumeComponent::Shape::Sphere;
    scene.meshEntities[1].fogVolume.enabled = false; scene.meshEntities[1].fogVolume.radius = 7;
    scene.meshEntities[1].fogVolume.edgeFade = 2;
    auto& manager = SceneManager::Instance();
    manager.RestoreSceneSnapshot(scene, {}, false);
    const bool saved = manager.SaveSceneAs((root / "fog.scene").string()); manager.CloseScene();
    const bool loaded = saved && manager.LoadScene((root / "fog.scene").string());
    Check(loaded, "scene with global and local fog saves and loads");
    if (loaded)
    {
        const auto& read = manager.GetCurrentScene(); const auto& global = read.sky.fog;
        Check(global.enabled && global.heightEnabled && !global.affectSky && Near(global.density, 0.037f) &&
            global.startDistance == 13 && global.maxDistance == 333 && Near(global.maxOpacity, 0.74f) &&
            global.baseHeight == -2 && Near(global.heightFalloff, 0.15f) && Near(global.color[0], 0.23f), "scene preserves every global fog setting");
        Check(read.meshEntities.size() == 2 && read.meshEntities[0].hasFogVolume && !read.meshEntities[0].HasMeshGeometry() &&
            read.meshEntities[0].scale[0] == 0 && read.meshEntities[0].fogVolume.halfExtents[2] == 4 &&
            read.meshEntities[1].fogVolume.shape == FogVolumeComponent::Shape::Sphere &&
            !read.meshEntities[1].fogVolume.enabled && read.meshEntities[1].fogVolume.radius == 7 && read.meshEntities[1].fogVolume.edgeFade == 2,
            "scene preserves zero-scale box and disabled sphere volumes");
    }
    manager.CloseScene();
    { std::ofstream file(root / "legacy.scene"); file << "{\"version\":1,\"name\":\"Legacy\",\"environment\":{},\"entities\":[]}"; }
    Check(manager.LoadScene((root / "legacy.scene").string()) && !manager.GetCurrentScene().sky.fog.enabled,
        "legacy scenes retain their fog-free appearance");
    manager.CloseScene();
    if (root.parent_path() == parent && root.filename().string().starts_with("ixw_fog_test_"))
    { std::error_code error; fs::remove_all(root, error); }
}

#if defined(_WIN32)
void Gpu()
{
    NativeWindow_Win32 window;
    if (!window.Create(GetModuleHandle(nullptr), "Fog GPU regression", 96, 64))
    { Check(false, "GPU window"); return; }
    ShowWindow(window.GetHwnd(), SW_HIDE);
    VulkanDevice native;
    if (!native.Create(window, 96, 64)) { Check(false, "GPU device"); window.Destroy(); return; }
    auto rhi = ixvulkan::CreateDevice(native);
    [&] {
        client::asset::FileAssetReader assets(IX_FOG_TEST_ASSETS_ROOT);
        client::asset::FileAssetReader testShaders(IX_FOG_TEST_SHADER_ROOT);
        const auto shader = [&](const char* path, const char* entry) {
            ixrhi::IXRHIShaderDesc desc;
            desc.stage = ixrhi::IXRHIShaderStage::Compute; desc.entryPoint = entry;
            const auto bytes = testShaders.ReadAll(path);
            if (!bytes || bytes->empty()) return std::shared_ptr<ixrhi::IXRHIShader>{};
            const auto words = reinterpret_cast<const std::uint32_t*>(bytes->data());
            desc.spirv.assign(words, words + bytes->size() / sizeof(std::uint32_t));
            return rhi->CreateShader(desc);
        };
        auto cs = shader("fog_test_CSMain.spv", "CSMain"), sampleCs = shader("fog_test_SampleCS.spv", "SampleCS");
        Check(cs && sampleCs, "fog GPU test shaders");
        if (!cs || !sampleCs) return;
        using B = ixrhi::IXRHIBindingType; using S = ixrhi::IXRHIShaderStage;
        auto layout = rhi->CreateBindGroupLayout({{0, B::UniformBuffer, S::Compute},
            {1, B::StorageBuffer, S::Compute}, {2, B::StorageBuffer, S::Compute}});
        auto sampleLayout = rhi->CreateBindGroupLayout({{2, B::StorageBuffer, S::Compute}, {3, B::SampledTexture, S::Compute}});
        if (!layout || !sampleLayout) { Check(false, "fog GPU layouts"); return; }
        auto bindings = rhi->CreateBindGroup(*layout, 1), sampleBindings = rhi->CreateBindGroup(*sampleLayout, 1);
        ixrhi::IXRHIComputePipelineDesc desc;
        desc.computeShader = cs; desc.bindGroupLayouts = {layout.get()};
        auto pipeline = rhi->CreateComputePipeline(desc);
        desc.computeShader = sampleCs; desc.bindGroupLayouts = {sampleLayout.get()};
        auto samplePipeline = rhi->CreateComputePipeline(desc);
        constexpr int queryCount = 12;
        std::array<std::array<float, 4>, queryCount * 2> queries{};
        for (int i = 0; i < queryCount; ++i)
        {
            const auto dir = xm::Normalize(WorldVec3{(i % 3 - 1) * 0.15f, (i % 4 - 2) * 0.1f, 1});
            queries[i * 2] = {dir.x, dir.y, dir.z, i < 3 ? 5.0f : i < 6 ? 9.0f : 100.0f};
            queries[i * 2 + 1][0] = (i & 1) ? 1.0f : 0.0f;
        }
        const auto buffer = [&](std::uint64_t bytes, ixrhi::IXRHIBufferUsage usage, bool host, const void* data = nullptr) {
            ixrhi::IXRHIBufferDesc d; d.sizeBytes = bytes; d.usage = usage;
            d.cpuAccess = host ? ixrhi::IXRHICpuAccess::Write : ixrhi::IXRHICpuAccess::None;
            return rhi->CreateBuffer(d, data, data ? static_cast<std::size_t>(bytes) : 0);
        };
        using U = ixrhi::IXRHIBufferUsage;
        auto uniform = buffer(sizeof(FogUniforms), U::Uniform, true);
        auto queryBuffer = buffer(sizeof(queries), U::Storage, false, queries.data());
        auto output = buffer(queryCount * 16, U::Storage | U::TransferSrc, false);
        auto readback = buffer(queryCount * 16, U::TransferDst, true);
        Check(pipeline && samplePipeline && bindings && sampleBindings && uniform && queryBuffer && output && readback,
            "fog GPU resources");
        if (!pipeline || !samplePipeline || !bindings || !sampleBindings || !uniform || !queryBuffer || !output || !readback) return;
        bindings->UpdateBuffer(0, 0, uniform, 0, sizeof(FogUniforms));
        bindings->UpdateBuffer(0, 1, queryBuffer, 0, sizeof(queries));
        bindings->UpdateBuffer(0, 2, output, 0, queryCount * 16);
        sampleBindings->UpdateBuffer(0, 2, output, 0, queryCount * 16);
        using State = ixrhi::IXRHIBufferState;
        auto outputState = State::Undefined, readState = State::Undefined;
        const auto beginOutput = [&](ixrhi::IXRHICommandList& cmd) {
            cmd.TransitionBuffer(*output, outputState, State::ShaderWrite); outputState = State::ShaderWrite;
        };
        const auto copyOutput = [&](ixrhi::IXRHICommandList& cmd) {
            cmd.TransitionBuffer(*output, outputState, State::TransferSrc); outputState = State::TransferSrc;
            cmd.TransitionBuffer(*readback, readState, State::TransferDst);
            cmd.CopyBuffer(*output, *readback, queryCount * 16);
            cmd.TransitionBuffer(*readback, State::TransferDst, State::HostRead); readState = State::HostRead;
        };
        for (int fixture = 0; fixture < 5; ++fixture)
        {
            GlobalFogSettings global; global.enabled = fixture != 0; global.heightEnabled = fixture >= 2;
            global.baseHeight = 0; global.heightFalloff = 0.3f; global.affectSky = fixture != 3;
            global.startDistance = 2; global.maxDistance = 40; global.maxOpacity = fixture == 4 ? 0.1f : 0.98f;
            auto camera = Camera({0, fixture == 2 ? 5.0f : 0.0f, 0});
            std::vector<MeshSceneEntity> entities{Volume(), Volume()};
            entities[0].rotation[1] = 0.7f; entities[0].fogVolume.edgeFade = fixture ? 1.0f : 0.0f;
            entities[1].fogVolume.shape = FogVolumeComponent::Shape::Sphere; entities[1].fogVolume.radius = 3;
            entities[1].fogVolume.edgeFade = fixture ? 2.0f : 0.0f; entities[1].fogVolume.color[1] = 0.2f;
            FogUniforms fog; BuildFogUniforms(fog, camera, global, entities);
            uniform->Write(0, &fog, sizeof(fog));
            rhi->ExecuteAndWait([&](auto& cmd) {
                beginOutput(cmd); cmd.SetComputePipeline(*pipeline); cmd.BindGroup(0, *bindings, 0);
                cmd.Dispatch(queryCount, 1, 1); copyOutput(cmd);
            });
            std::array<std::array<float, 4>, queryCount> values{}; readback->Read(0, values.data(), sizeof(values));
            bool matches = true;
            for (int i = 0; i < queryCount; ++i)
            {
                const auto& q = queries[i * 2];
                const auto cpu = EvaluateFog(fog, {q[0], q[1], q[2]}, q[3], queries[i * 2 + 1][0] > 0);
                matches &= Near(values[i][0], cpu.color.x) && Near(values[i][1], cpu.color.y) &&
                    Near(values[i][2], cpu.color.z) && Near(values[i][3], cpu.transmission);
            }
            Check(matches, "real GPU fog agrees with CPU for clipped, rotated, overlapping and height rays");
        }
        OffscreenSceneRenderer scene, game;
        const auto depthFormat = rhi->GetMainSwapchain().DepthFormat();
        const bool targets = scene.Create(*rhi, assets, 96, 64, OffscreenSceneRenderer::kSceneColorFormat, depthFormat, "FogTest.Scene") &&
            game.Create(*rhi, assets, 96, 64, OffscreenSceneRenderer::kSceneColorFormat, depthFormat, "FogTest.Game");
        Check(targets, "actual fog composite pipelines for both views");
        if (targets)
        {
            auto sampler = rhi->CreateSampler({});
            for (int fixture = 0; fixture < 9; ++fixture)
            {
                auto& view = (fixture & 1) ? game : scene;
                auto camera = Camera({0, 0, fixture < 6 && (fixture & 1) ? -2.0f : 0.0f});
                GlobalFogSettings global; global.enabled = fixture == 1 || fixture == 3 || fixture == 8;
                global.density = 0.5f;
                std::vector<MeshSceneEntity> entities;
                if (fixture == 2 || fixture == 3) {
                    entities = {Volume()}; entities[0].position[2] = 0.5f; entities[0].fogVolume.density = 0.2f;
                }
                const float surfaceDepth = fixture == 6 || fixture == 8 ? 0.3f : fixture == 7 ? 0.9f : 1.0f;
                if (fixture >= 6) {
                    entities = {Volume()}; entities[0].position[2] = 0.8f;
                    for (auto& extent : entities[0].fogVolume.halfExtents) extent = 0.1f;
                }
                auto frame = rhi->BeginFrame();
                if (!frame) { Check(false, "composite frame"); break; }
                auto& cmd = *frame.info.commandList;
                view.BeginMainPass(cmd, frame.info);
                if (surfaceDepth != 1.0f) cmd.ClearDepth(surfaceDepth, 0, 0, 96, 64);
                if (fixture == 8) cmd.ClearDepth(0.9f, 48, 0, 48, 64);
                view.EndMainPass(cmd);
                view.PrepareFog(cmd, frame.info, camera, global, entities);
                view.BeginDisplayPass(cmd, frame.info, {0, 1});
                if (fixture == 8)
                {
                    cmd.SetViewport(0, 0, 192, 128);
                    FogRendererTestAccess::Draw(view, cmd, frame.info);
                }
                view.EndDisplayPass(cmd);
                cmd.TransitionTexture(*view.GetDisplayTexture(), ixrhi::IXRHIImageLayout::ShaderReadOnly,
                    ixrhi::IXRHIImageLayout::ShaderReadOnly);
                sampleBindings->UpdateTexture(0, 3, view.GetDisplayTexture(), sampler);
                beginOutput(cmd); cmd.SetComputePipeline(*samplePipeline); cmd.BindGroup(0, *sampleBindings, 0);
                cmd.Dispatch(1, 1, 1); copyOutput(cmd);
                rhi->GetMainRenderTarget()->Begin(cmd); rhi->GetMainRenderTarget()->End(cmd);
                rhi->EndFrame(frame); rhi->WaitIdle();
                float value[4]{}; readback->Read(0, value, sizeof(value));
                FogUniforms fog; BuildFogUniforms(fog, camera, global, entities);
                const WorldVec3 ray = WorldVec3{(48.5f / (fixture == 8 ? 192 : 96)) * 2 - 1,
                    (32.5f / (fixture == 8 ? 128 : 64)) * 2 - 1, surfaceDepth} - camera.eye;
                const auto expected = EvaluateFog(fog, xm::Normalize(ray), xm::Length(ray), surfaceDepth == 1.0f);
                const float color[] = {expected.color.x, expected.color.y, expected.color.z};
                bool matches = true;
                for (int c = 0; c < 3; ++c)
                    matches &= Near(value[c], kSceneClearColor[c] * expected.transmission + color[c] * (1 - expected.transmission), 0.001f);
                Check(matches, fixture == 8 ? "upscaled composite uses source UV depth rather than output pixel coordinates" :
                    fixture >= 6 ? "actual foreground depth clips local fog behind and across a surface" :
                    "actual composite pixels match fog and clear correctly when disabled across both frame slots and views");
            }
        }
        rhi->WaitIdle(); scene.Destroy(); game.Destroy();
    }();
    rhi->Shutdown(); rhi.reset(); native.Destroy(); window.Destroy();
}
#endif
}

int main(int argc, char** argv)
{
    std::cout << std::unitbuf;
    Math(); Serialization();
#if defined(_WIN32)
    if (argc > 1 && std::string(argv[1]) == "--gpu") Gpu();
#else
    (void)argc; (void)argv;
#endif
    return failures ? 1 : 0;
}
