// A compiled material graph drawn by the real static mesh renderer on the GPU, read back per pixel:
// the pipeline with descriptor set 1, its uniform block and textures, live value changes, the
// fallback to baked textures, and keeping the last working shader when a reload fails.
#include "StaticMeshRenderer.h"
#include "OffscreenSceneRenderer.h"
#include "GraphEnvironment.h"
#include "SkyRenderer.h"
#include <functional>
#include "tools/material/GraphShader.h"
#include "asset/AssetDatabase.h"
#include "asset/FileAssetReader.h"
#include "asset/MaterialAssetManager.h"
#include <stb_image_write.h>
#include <chrono>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

#if defined(_WIN32)
#include "IXVulkanDevice.h"
#include "VulkanDevice.h"
#include "NativeWindow_Win32.h"
#endif

using namespace material_tool;
namespace {
int failures = 0;
void Check(bool ok, const std::string& what) { std::cout << (ok ? "PASS " : "FAIL ") << what << '\n'; failures += !ok; }
bool Near(const float* value, std::array<float, 3> expected, float tolerance = 0.01f)
{
    for (int c = 0; c < 3; ++c) if (std::abs(value[c] - expected[c]) > tolerance) return false;
    return true;
}
bool WriteFile(const std::filesystem::path& path, const std::string& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(file);
}
std::string MaterialJson(const std::string& name, const std::string& graphShader, const std::string& emissiveTexture = {},
    const std::string& alphaMode = "Opaque")
{
    return "{\n  \"version\": 2,\n  \"shader\": \"PBR-Standard\",\n  \"shadingMode\": \"Lit\",\n  \"name\": \"" + name +
        "\",\n  \"baseColor\": [1, 1, 1, 1],\n  \"alphaMode\": \"" + alphaMode + "\",\n" +
        (graphShader.empty() ? std::string{} : "  \"graphShader\": \"" + graphShader + "\",\n") +
        (emissiveTexture.empty() ? std::string("  \"textures\": {}\n}\n") :
            "  \"emissive\": [1, 1, 1, 1],\n  \"textures\": {\"emissive\": \"" + emissiveTexture + "\"}\n}\n");
}
}

#if defined(_WIN32)
struct GraphMaterialRenderTestAccess
{
    // --compare <project> <graph material GUID> <baked material GUID> <out.png>: draws a quad with each
    // material through the real renderer, lit by sun and ambient, writes them side by side and
    // reports how far the per-pixel graph is from its baked textures.
    static void Compare(const std::filesystem::path& project, const std::string& graphMaterial, const std::string& bakedMaterial,
        const std::filesystem::path& output)
    {
        constexpr std::uint32_t size = 256;
        AssetDatabase::Instance().scan(project);
        NativeWindow_Win32 window;
        if (!window.Create(GetModuleHandle(nullptr), "Graph material compare", 96, 64)) { Check(false, "hidden GPU window"); return; }
        ShowWindow(window.GetHwnd(), SW_HIDE);
        VulkanDevice native;
        if (!native.Create(window, 96, 64)) { Check(false, "GPU device"); window.Destroy(); return; }
        auto rhi = ixvulkan::CreateDevice(native);
        client::asset::FileAssetReader assets(IX_GRAPH_RENDER_TEST_ASSETS_ROOT);
        GraphEnvironment environment;
        environment.Create(*rhi);
        StaticMeshRenderer::SetGraphEnvironment(&environment);
        std::vector<std::array<float, 4>> images[2];
        {
            OffscreenSceneRenderer scene;
            const bool target = scene.Create(*rhi, assets, size, size, OffscreenSceneRenderer::kSceneColorFormat,
                rhi->GetMainSwapchain().DepthFormat(), "GraphMaterialCompare");
            StaticMeshRenderer renderer;
            const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
            for (int i = 0; i < 4; ++i)
            {
                StaticMeshRenderer::Vertex vertex{};
                vertex.position[0] = corners[i][0]; vertex.position[1] = corners[i][1]; vertex.position[2] = 0.5f;
                vertex.normal[2] = -1; vertex.uv[0] = (corners[i][0] + 1) * 0.5f; vertex.uv[1] = (corners[i][1] + 1) * 0.5f;
                renderer.m_vertices.push_back(vertex);
            }
            renderer.m_indices = {0, 1, 2, 0, 2, 3};
            renderer.m_draws.push_back({0, 6, 0, 4});
            renderer.m_assets = &assets;
            renderer.m_materialDefaults.resize(1);
            renderer.DecodeTextures("");
            renderer.SetTargetPass(scene.GetTargetPass());
            const bool ready = target && renderer.FinishGpu(*rhi);
            ixrhi::IXRHITextureDesc shadowDesc;
            shadowDesc.width = shadowDesc.height = 1; shadowDesc.arrayLayers = 4;
            shadowDesc.format = ixrhi::IXRHIFormat::D32Float;
            shadowDesc.usage = ixrhi::IXRHITextureUsage::Sampled | ixrhi::IXRHITextureUsage::TransferDst;
            SunShadowReceive shadow;
            shadow.texture = rhi->CreateTexture(shadowDesc, nullptr, 0);
            ixrhi::IXRHISamplerDesc shadowSampler; shadowSampler.compareEnable = true; shadowSampler.compareOp = ixrhi::IXRHICompareOp::LessOrEqual;
            shadow.sampler = rhi->CreateSampler(shadowSampler);
            renderer.SetSunShadow(shadow);
            LightingState lighting;
            lighting.sunShadowsEnabled = false;
            lighting.ambient.r = lighting.ambient.g = lighting.ambient.b = 1.0f; lighting.ambient.intensity = 0.45f;
            renderer.SetLightingState(lighting);
            using B = ixrhi::IXRHIBindingType; using S = ixrhi::IXRHIShaderStage; using U = ixrhi::IXRHIBufferUsage;
            std::ifstream file(std::filesystem::path(IX_GRAPH_RENDER_TEST_SHADER_ROOT) / "graph_render_test_CopyCS.spv", std::ios::binary);
            const std::string code((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            ixrhi::IXRHIShaderDesc copyShader; copyShader.stage = S::Compute; copyShader.entryPoint = "CopyCS";
            copyShader.spirv.resize(code.size() / 4); std::memcpy(copyShader.spirv.data(), code.data(), copyShader.spirv.size() * 4);
            auto layout = rhi->CreateBindGroupLayout({{0, B::StorageBuffer, S::Compute}, {1, B::SampledTexture, S::Compute}});
            auto group = layout ? rhi->CreateBindGroup(*layout, 1) : nullptr;
            ixrhi::IXRHIComputePipelineDesc computeDesc; computeDesc.computeShader = rhi->CreateShader(copyShader);
            computeDesc.bindGroupLayouts = {layout.get()};
            auto pipeline = computeDesc.computeShader ? rhi->CreateComputePipeline(computeDesc) : nullptr;
            const std::uint64_t bytes = std::uint64_t(size) * size * 16;
            ixrhi::IXRHIBufferDesc outDesc; outDesc.sizeBytes = bytes; outDesc.usage = U::Storage | U::TransferSrc;
            ixrhi::IXRHIBufferDesc readDesc; readDesc.sizeBytes = bytes; readDesc.usage = U::TransferDst; readDesc.cpuAccess = ixrhi::IXRHICpuAccess::Write;
            auto out = rhi->CreateBuffer(outDesc, nullptr, 0), readback = rhi->CreateBuffer(readDesc, nullptr, 0);
            auto sampler = rhi->CreateSampler({});
            Check(ready && pipeline && group && out && readback, "comparison renderer and read-back pass");
            if (ready && pipeline && group && out && readback)
            {
                group->UpdateBuffer(0, 0, out, 0, bytes);
                group->UpdateTexture(0, 1, scene.GetColorTexture(), sampler);
                StaticMeshRenderer::Instance instance{};
                instance.entityId = 9;
                WorldCamera camera{};
                camera.viewProjection = WorldIdentity();
                camera.eye = {0, 0, -1};
                bool shadowReady = false;
                using State = ixrhi::IXRHIBufferState;
                auto outState = State::Undefined, readState = State::Undefined;
                const std::string materials[2] = {graphMaterial, bakedMaterial};
                for (int which = 0; which < 2; ++which)
                {
                    instance.materialSlots = {materials[which]};
                    // A few frames: textures are decoded on a worker and uploaded by the next frame.
                    for (int frameNumber = 0; frameNumber < 40; ++frameNumber)
                    {
                        auto frame = rhi->BeginFrame();
                        if (!frame) break;
                        auto& cmd = *frame.info.commandList;
                        if (!shadowReady)
                        {
                            cmd.TransitionTexture(*shadow.texture, ixrhi::IXRHIImageLayout::Undefined, ixrhi::IXRHIImageLayout::ShaderReadOnly);
                            shadowReady = true;
                        }
                        renderer.RecordPendingUploads(cmd, frame.info);
                        scene.BeginMainPass(cmd, frame.info);
                        renderer.RenderBatchInWorld(cmd, frame.info, 0, camera, {&instance}, size, size);
                        scene.EndMainPass(cmd);
                        cmd.TransitionBuffer(*out, outState, State::ShaderWrite); outState = State::ShaderWrite;
                        cmd.SetComputePipeline(*pipeline); cmd.BindGroup(0, *group, 0); cmd.Dispatch(size / 8, size / 8, 1);
                        cmd.TransitionBuffer(*out, outState, State::TransferSrc); outState = State::TransferSrc;
                        cmd.TransitionBuffer(*readback, readState, State::TransferDst);
                        cmd.CopyBuffer(*out, *readback, bytes);
                        cmd.TransitionBuffer(*readback, State::TransferDst, State::HostRead); readState = State::HostRead;
                        rhi->GetMainRenderTarget()->Begin(cmd); rhi->GetMainRenderTarget()->End(cmd);
                        rhi->EndFrame(frame); rhi->WaitIdle();
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    }
                    images[which].resize(std::size_t(size) * size);
                    readback->Read(0, images[which].data(), bytes);
                    const bool graphDrawn = !renderer.m_lastMaterialBindings.empty() &&
                        (renderer.m_lastMaterialBindings[0].pipelineName == "StaticMesh:GraphOpaque") == (which == 0);
                    Check(graphDrawn, which == 0 ? "published material is drawn by its graph shader" : "its baked copy is drawn from textures");
                }
            }
            rhi->WaitIdle();
            renderer.Destroy();
            scene.Destroy();
            shadow = {};
        }
        StaticMeshRenderer::SetGraphEnvironment(nullptr);
        environment.Destroy();
        rhi->Shutdown(); rhi.reset(); native.Destroy(); window.Destroy();
        if (images[0].empty() || images[1].empty()) return;
        double sum = 0, worst = 0;
        std::vector<unsigned char> png(std::size_t(size) * 2 * size * 3);
        for (std::uint32_t y = 0; y < size; ++y)
            for (std::uint32_t x = 0; x < size; ++x)
                for (int which = 0; which < 2; ++which)
                {
                    const auto& pixel = images[which][std::size_t(size - 1 - y) * size + x];
                    for (int c = 0; c < 3; ++c)
                    {
                        png[(std::size_t(y) * size * 2 + which * size + x) * 3 + c] =
                            static_cast<unsigned char>(std::lround(std::clamp(LinearToSrgb(std::clamp(pixel[c], 0.0f, 1.0f)), 0.0f, 1.0f) * 255));
                        if (which == 1)
                        {
                            const double difference = std::abs(images[0][std::size_t(size - 1 - y) * size + x][c] - pixel[c]);
                            sum += difference; worst = std::max(worst, difference);
                        }
                    }
                }
        std::cout << "graph vs baked: mean absolute difference " << sum / (double(size) * size * 3) << ", largest " << worst << " (linear HDR)\n";
        Check(stbi_write_png(output.string().c_str(), size * 2, size, 3, png.data(), size * 2 * 3) != 0, "comparison image is written");
    }

    // --water <out.png> [seconds]: the Water template on a water plane over a sloping sandy shore,
    // drawn as the engine draws a view: the shore, the sky, a copy of the scene depth, then the water in
    // the transparent pass, under a low sun. Writes the tone-mapped image.
    static void WaterScene(const std::filesystem::path& outputPath, double seconds)
    {
        constexpr std::uint32_t width = 640, height = 360;
        std::error_code ec;
#if defined(IX_GRAPH_DXC)
        const std::filesystem::path compiler = IX_GRAPH_DXC;
#else
        const std::filesystem::path compiler = FindShaderCompiler();
#endif
        const auto root = std::filesystem::temp_directory_path() / ("ix-graph-water-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
        AssetDatabase::Instance().scan(root);
        MaterialGraphProgram program; GraphShaderPackage package; std::string error, bytes;
        const bool published = program.Compile(MaterialGraphDocument::Water(), error) && CompileGraphShader(program, package, error, compiler) &&
            program.BuildRuntimeUniforms({}, {}, package.uniforms, error) && EncodeGraphShaderPackage(package, bytes, error) &&
            WriteFile(root / "Water" / "graph.ixgraphshader", bytes) &&
            WriteFile(root / "Water" / "Water.material", MaterialJson("Water", "graph.ixgraphshader", {}, "Blend")) &&
            WriteFile(root / "Sand" / "Sand.material", MaterialJson("Sand", ""));
        Check(published, "the Water template is published: " + error);
        if (!published) return;
        const auto waterMaterial = AssetDatabase::Instance().getOrCreateGuid(root / "Water" / "Water.material").toString();
        const auto sandMaterial = AssetDatabase::Instance().getOrCreateGuid(root / "Sand" / "Sand.material").toString();

        NativeWindow_Win32 window;
        if (!window.Create(GetModuleHandle(nullptr), "Graph water scene", 96, 64)) { Check(false, "hidden GPU window"); return; }
        ShowWindow(window.GetHwnd(), SW_HIDE);
        VulkanDevice native;
        if (!native.Create(window, 96, 64)) { Check(false, "GPU device"); window.Destroy(); return; }
        auto rhi = ixvulkan::CreateDevice(native);
        client::asset::FileAssetReader assets(IX_GRAPH_RENDER_TEST_ASSETS_ROOT);
        GraphEnvironment environment;
        environment.Create(*rhi);
        StaticMeshRenderer::SetGraphEnvironment(&environment);
        std::vector<std::array<float, 4>> image;
        {
            OffscreenSceneRenderer scene;
            const bool target = scene.Create(*rhi, assets, width, height, OffscreenSceneRenderer::kSceneColorFormat,
                rhi->GetMainSwapchain().DepthFormat(), "GraphWaterScene");
            // The shore: rises towards the camera, the waterline curving with the bumps.
            const auto groundHeight = [](float x, float z) { return -0.28f * z - 1.2f + 0.6f * std::sin(x * 0.45f) + 0.25f * std::sin(z * 0.9f + x * 0.3f); };
            StaticMeshRenderer ground, water;
            constexpr int cells = 96; constexpr float extent = 40.0f;
            for (int j = 0; j <= cells; ++j)
                for (int i = 0; i <= cells; ++i)
                {
                    const float x = -extent + 2 * extent * i / cells, z = -extent + 2 * extent * j / cells, e = 0.05f;
                    StaticMeshRenderer::Vertex vertex{};
                    vertex.position[0] = x; vertex.position[1] = groundHeight(x, z); vertex.position[2] = z;
                    const float dx = (groundHeight(x + e, z) - groundHeight(x - e, z)) / (2 * e), dz = (groundHeight(x, z + e) - groundHeight(x, z - e)) / (2 * e);
                    const float length = std::sqrt(dx * dx + 1 + dz * dz);
                    vertex.normal[0] = -dx / length; vertex.normal[1] = 1 / length; vertex.normal[2] = -dz / length;
                    vertex.uv[0] = static_cast<float>(i) / cells; vertex.uv[1] = static_cast<float>(j) / cells;
                    ground.m_vertices.push_back(vertex);
                }
            for (int j = 0; j < cells; ++j)
                for (int i = 0; i < cells; ++i)
                {
                    const std::uint32_t a = j * (cells + 1) + i, b = a + 1, c = a + cells + 1, d = c + 1;
                    ground.m_indices.insert(ground.m_indices.end(), {a, c, b, b, c, d});
                }
            ground.m_draws.push_back({0, static_cast<std::uint32_t>(ground.m_indices.size()), 0, static_cast<std::uint32_t>(ground.m_vertices.size())});
            const float plane[4][2] = {{-extent, -extent}, {extent, -extent}, {extent, extent}, {-extent, extent}};
            for (const auto& corner : plane)
            {
                StaticMeshRenderer::Vertex vertex{};
                vertex.position[0] = corner[0]; vertex.position[2] = corner[1]; vertex.normal[1] = 1;
                vertex.uv[0] = (corner[0] + extent) / (2 * extent); vertex.uv[1] = (corner[1] + extent) / (2 * extent);
                water.m_vertices.push_back(vertex);
            }
            water.m_indices = {0, 2, 1, 0, 3, 2};
            water.m_draws.push_back({0, 6, 0, 4});
            for (StaticMeshRenderer* renderer : {&ground, &water})
            {
                renderer->m_assets = &assets;
                renderer->m_materialDefaults.resize(1);
                renderer->DecodeTextures("");
                renderer->SetTargetPass(scene.GetTargetPass());
            }
            const bool ready = target && ground.FinishGpu(*rhi) && water.FinishGpu(*rhi);
            ixrhi::IXRHITextureDesc shadowDesc;
            shadowDesc.width = shadowDesc.height = 1; shadowDesc.arrayLayers = 4;
            shadowDesc.format = ixrhi::IXRHIFormat::D32Float;
            shadowDesc.usage = ixrhi::IXRHITextureUsage::Sampled | ixrhi::IXRHITextureUsage::TransferDst;
            SunShadowReceive shadow;
            shadow.texture = rhi->CreateTexture(shadowDesc, nullptr, 0);
            ixrhi::IXRHISamplerDesc shadowSampler; shadowSampler.compareEnable = true; shadowSampler.compareOp = ixrhi::IXRHICompareOp::LessOrEqual;
            shadow.sampler = rhi->CreateSampler(shadowSampler);
            LightingState lighting;
            lighting.sunShadowsEnabled = false;
            lighting.directional.azimuthDegrees = 200; lighting.directional.elevationDegrees = 28; lighting.directional.intensity = 2.2f;
            lighting.ambient.r = .55f; lighting.ambient.g = .65f; lighting.ambient.b = .8f; lighting.ambient.intensity = .55f;
            for (StaticMeshRenderer* renderer : {&ground, &water}) { renderer->SetSunShadow(shadow); renderer->SetLightingState(lighting); }
            SkyRenderer sky;
            sky.SetTargetPass(scene.GetTargetPass());
            const bool skyReady = sky.Create(*rhi, assets);
            using B = ixrhi::IXRHIBindingType; using S = ixrhi::IXRHIShaderStage; using U = ixrhi::IXRHIBufferUsage;
            std::ifstream file(std::filesystem::path(IX_GRAPH_RENDER_TEST_SHADER_ROOT) / "graph_render_test_CopyCS.spv", std::ios::binary);
            const std::string code((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            ixrhi::IXRHIShaderDesc copyShader; copyShader.stage = S::Compute; copyShader.entryPoint = "CopyCS";
            copyShader.spirv.resize(code.size() / 4); std::memcpy(copyShader.spirv.data(), code.data(), copyShader.spirv.size() * 4);
            auto layout = rhi->CreateBindGroupLayout({{0, B::StorageBuffer, S::Compute}, {1, B::SampledTexture, S::Compute}});
            auto group = layout ? rhi->CreateBindGroup(*layout, 1) : nullptr;
            ixrhi::IXRHIComputePipelineDesc computeDesc; computeDesc.computeShader = rhi->CreateShader(copyShader);
            computeDesc.bindGroupLayouts = {layout.get()};
            auto pipeline = computeDesc.computeShader ? rhi->CreateComputePipeline(computeDesc) : nullptr;
            const std::uint64_t imageBytes = std::uint64_t(width) * height * 16;
            ixrhi::IXRHIBufferDesc outDesc; outDesc.sizeBytes = imageBytes; outDesc.usage = U::Storage | U::TransferSrc;
            ixrhi::IXRHIBufferDesc readDesc; readDesc.sizeBytes = imageBytes; readDesc.usage = U::TransferDst; readDesc.cpuAccess = ixrhi::IXRHICpuAccess::Write;
            auto out = rhi->CreateBuffer(outDesc, nullptr, 0), readback = rhi->CreateBuffer(readDesc, nullptr, 0);
            auto sampler = rhi->CreateSampler({});
            Check(ready && skyReady && pipeline && group && out && readback, "water scene renderers and read-back pass");
            if (ready && skyReady && pipeline && group && out && readback)
            {
                group->UpdateBuffer(0, 0, out, 0, imageBytes);
                group->UpdateTexture(0, 1, scene.GetColorTexture(), sampler);
                WorldCamera camera{};
                camera.eye = {-3, 5.5f, -16};
                camera.target = {2, -1.5f, 6};
                camera.nearPlane = 0.1f; camera.farPlane = 400;
                camera.viewProjection = WorldMultiply(WorldLookAt(camera.eye, camera.target, {0, 1, 0}),
                    WorldPerspective(ixtreeme::math::DegreesToRadians(50.0f), static_cast<float>(width) / height, camera.nearPlane, camera.farPlane));
                StaticMeshRenderer::Instance groundInstance{}, waterInstance{};
                groundInstance.entityId = 1; groundInstance.tint = {.62f, .52f, .36f, 1}; groundInstance.materialSlots = {sandMaterial};
                waterInstance.entityId = 2; waterInstance.materialSlots = {waterMaterial};
                const SkySettings skySettings;
                bool shadowReady = false;
                using State = ixrhi::IXRHIBufferState;
                auto outState = State::Undefined, readState = State::Undefined;
                for (int frameNumber = 0; frameNumber < 12; ++frameNumber)
                {
                    auto frame = rhi->BeginFrame();
                    if (!frame) break;
                    auto& cmd = *frame.info.commandList;
                    if (!shadowReady)
                    {
                        cmd.TransitionTexture(*shadow.texture, ixrhi::IXRHIImageLayout::Undefined, ixrhi::IXRHIImageLayout::ShaderReadOnly);
                        shadowReady = true;
                    }
                    ground.RecordPendingUploads(cmd, frame.info);
                    water.RecordPendingUploads(cmd, frame.info);
                    sky.Update(frame.info, skySettings, lighting, {});
                    environment.SetSky(sky.GetLookup());
                    environment.Begin(frame.info, seconds, camera, width, height);
                    scene.BeginMainPass(cmd, frame.info);
                    ground.RenderBatchInWorld(cmd, frame.info, seconds, camera, {&groundInstance}, width, height);
                    sky.Render(cmd, frame.info, camera, width, height);
                    scene.EndMainPass(cmd);
                    scene.SnapshotScene(cmd, frame.info);
                    scene.BeginMainPass(cmd, frame.info, false);
                    environment.Begin(frame.info, seconds, camera, width, height, scene.GetDepthSnapshotTexture());
                    water.RenderTransparentInWorld(cmd, frame.info, seconds, camera, waterInstance, width, height);
                    scene.EndMainPass(cmd);
                    cmd.TransitionBuffer(*out, outState, State::ShaderWrite); outState = State::ShaderWrite;
                    cmd.SetComputePipeline(*pipeline); cmd.BindGroup(0, *group, 0); cmd.Dispatch((width + 7) / 8, (height + 7) / 8, 1);
                    cmd.TransitionBuffer(*out, outState, State::TransferSrc); outState = State::TransferSrc;
                    cmd.TransitionBuffer(*readback, readState, State::TransferDst);
                    cmd.CopyBuffer(*out, *readback, imageBytes);
                    cmd.TransitionBuffer(*readback, State::TransferDst, State::HostRead); readState = State::HostRead;
                    rhi->GetMainRenderTarget()->Begin(cmd); rhi->GetMainRenderTarget()->End(cmd);
                    rhi->EndFrame(frame); rhi->WaitIdle();
                }
                image.resize(std::size_t(width) * height);
                readback->Read(0, image.data(), imageBytes);
            }
            rhi->WaitIdle();
            sky.Destroy();
            ground.Destroy();
            water.Destroy();
            scene.Destroy();
            shadow = {};
        }
        StaticMeshRenderer::SetGraphEnvironment(nullptr);
        environment.Destroy();
        rhi->Shutdown(); rhi.reset(); native.Destroy(); window.Destroy();
        std::filesystem::remove_all(root, ec);
        if (image.empty()) return;
        // ACES-style tone curve, then sRGB.
        const auto tone = [](float x) { x = std::max(x, 0.0f); return std::clamp(x * (2.51f * x + 0.03f) / (x * (2.43f * x + 0.59f) + 0.14f), 0.0f, 1.0f); };
        std::vector<unsigned char> png(std::size_t(width) * height * 3);
        for (std::uint32_t y = 0; y < height; ++y)
            for (std::uint32_t x = 0; x < width; ++x)
                for (int c = 0; c < 3; ++c)
                    png[(std::size_t(y) * width + x) * 3 + c] = static_cast<unsigned char>(std::lround(LinearToSrgb(tone(image[std::size_t(y) * width + x][c])) * 255));
        Check(stbi_write_png(outputPath.string().c_str(), width, height, 3, png.data(), width * 3) != 0, "water scene image is written");
    }

    static void Run()
    {
        std::error_code ec;
#if defined(IX_GRAPH_DXC)
        const std::filesystem::path compiler = IX_GRAPH_DXC;
#else
        const std::filesystem::path compiler = FindShaderCompiler();
#endif
        if (!std::filesystem::is_regular_file(compiler, ec)) { std::cout << "SKIP no shader compiler\n"; return; }
        const auto root = std::filesystem::temp_directory_path() / ("ix-graph-render-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
        AssetDatabase::Instance().scan(root);

        // A texture whose raw RGB values reach the emissive output without colour decoding.
        const unsigned char glow[] = {128, 32, 200, 255};
        std::vector<unsigned char> glowPixels; for (int i = 0; i < 16; ++i) glowPixels.insert(glowPixels.end(), glow, glow + 4);
        Check(stbi_write_png((root / "glow.png").string().c_str(), 4, 4, 4, glowPixels.data(), 16) != 0, "glow texture fixture writes");
        const auto glowGuid = AssetDatabase::Instance().getOrCreateGuid(root / "glow.png").toString();

        MaterialGraphDocument graph; std::string error;
        const auto albedo = graph.AddNode(NodeKind::ColorParameter, {0, 0});
        graph.FindNode(albedo)->parameter = "Albedo"; graph.FindNode(albedo)->value = {0.5f, 0.25f, 0.125f, 1};
        const auto sample = graph.AddNode(NodeKind::TextureSample, {0, 200});
        graph.FindNode(sample)->assetGuid = glowGuid; graph.FindNode(sample)->resource = "glow.png"; graph.FindNode(sample)->srgb = false;
        const auto output = graph.AddNode(NodeKind::PbrOutput, {300, 0});
        Check(graph.Connect(PinId(albedo, 0), PinId(output, 0), error) && graph.Connect(PinId(sample, 2), PinId(output, 3), error),
              "graph fixture connects albedo and texture emissive: " + error);
        MaterialGraphProgram program;
        Check(program.Compile(graph, error), "graph fixture compiles: " + error);
        GraphShaderPackage package;
        const bool compiled = CompileGraphShader(program, package, error, compiler);
        Check(compiled, "graph fixture compiles to SPIR-V: " + error);
        if (!compiled) return;
        package.textures = {{kGraphFirstTextureBinding, glowGuid}};
        Check(program.BuildRuntimeUniforms({}, {false}, package.uniforms, error), "uniform block builds");
        std::string bytes;
        Check(EncodeGraphShaderPackage(package, bytes, error) && WriteFile(root / "Graph" / "graph.ixgraphshader", bytes),
              "package writes beside its material");
        Check(WriteFile(root / "Graph" / "Graph.material", MaterialJson("Graph", "graph.ixgraphshader")) &&
              WriteFile(root / "Plain" / "Plain.material", MaterialJson("Plain", "")) &&
              WriteFile(root / "Glowing" / "Glowing.material", MaterialJson("Glowing", "", glowGuid)), "material fixtures write");
        const auto glowingMaterial = AssetDatabase::Instance().getOrCreateGuid(root / "Glowing" / "Glowing.material").toString();
        const auto graphMaterial = AssetDatabase::Instance().getOrCreateGuid(root / "Graph" / "Graph.material").toString();
        const auto plainMaterial = AssetDatabase::Instance().getOrCreateGuid(root / "Plain" / "Plain.material").toString();

        NativeWindow_Win32 window;
        if (!window.Create(GetModuleHandle(nullptr), "Graph material render", 96, 64)) { Check(false, "hidden GPU window"); return; }
        ShowWindow(window.GetHwnd(), SW_HIDE);
        VulkanDevice native;
        if (!native.Create(window, 96, 64)) { Check(false, "GPU device"); window.Destroy(); return; }
        auto rhi = ixvulkan::CreateDevice(native);
        client::asset::FileAssetReader assets(IX_GRAPH_RENDER_TEST_ASSETS_ROOT);
        GraphEnvironment environment;
        Check(environment.Create(*rhi), "graph environment (descriptor set 2)");
        StaticMeshRenderer::SetGraphEnvironment(&environment);
        {
            OffscreenSceneRenderer scene;
            const bool target = scene.Create(*rhi, assets, 96, 64, OffscreenSceneRenderer::kSceneColorFormat,
                rhi->GetMainSwapchain().DepthFormat(), "GraphMaterialTest");
            Check(target, "offscreen scene target");
            StaticMeshRenderer renderer;
            // One quad over the whole view at half depth, drawn with material slot 0.
            const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
            for (int i = 0; i < 4; ++i)
            {
                StaticMeshRenderer::Vertex vertex{};
                vertex.position[0] = corners[i][0]; vertex.position[1] = corners[i][1]; vertex.position[2] = 0.5f;
                vertex.normal[2] = -1; vertex.uv[0] = (corners[i][0] + 1) * 0.5f; vertex.uv[1] = (corners[i][1] + 1) * 0.5f;
                renderer.m_vertices.push_back(vertex);
            }
            renderer.m_indices = {0, 1, 2, 0, 2, 3};
            renderer.m_draws.push_back({0, 6, 0, 4});
            renderer.m_assets = &assets;
            renderer.m_materialDefaults.resize(1);
            renderer.DecodeTextures("");
            renderer.SetTargetPass(scene.GetTargetPass());
            const bool ready = target && renderer.FinishGpu(*rhi);
            Check(ready, "static mesh renderer pipelines");
            ixrhi::IXRHITextureDesc shadowDesc;
            shadowDesc.width = shadowDesc.height = 1; shadowDesc.arrayLayers = 4;
            shadowDesc.format = ixrhi::IXRHIFormat::D32Float;
            shadowDesc.usage = ixrhi::IXRHITextureUsage::Sampled | ixrhi::IXRHITextureUsage::TransferDst;
            SunShadowReceive shadow;
            shadow.texture = rhi->CreateTexture(shadowDesc, nullptr, 0);
            ixrhi::IXRHISamplerDesc shadowSampler; shadowSampler.compareEnable = true; shadowSampler.compareOp = ixrhi::IXRHICompareOp::LessOrEqual;
            shadow.sampler = rhi->CreateSampler(shadowSampler);
            renderer.SetSunShadow(shadow);
            LightingState lighting;  // ambient only: the colour is albedo * ambient + emissive
            lighting.directional.enabled = false;
            lighting.ambient.r = lighting.ambient.g = lighting.ambient.b = 1.0f; lighting.ambient.intensity = 0.5f;
            lighting.sunShadowsEnabled = false;
            renderer.SetLightingState(lighting);

            // Read-back compute pass.
            using B = ixrhi::IXRHIBindingType; using S = ixrhi::IXRHIShaderStage; using U = ixrhi::IXRHIBufferUsage;
            const auto spirv = [](const std::filesystem::path& path) {
                std::ifstream file(path, std::ios::binary); std::vector<std::uint32_t> words;
                const std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                words.resize(data.size() / 4); std::memcpy(words.data(), data.data(), words.size() * 4); return words;
            };
            ixrhi::IXRHIShaderDesc sampleShader; sampleShader.stage = S::Compute; sampleShader.entryPoint = "SampleCS";
            sampleShader.spirv = spirv(std::filesystem::path(IX_GRAPH_RENDER_TEST_SHADER_ROOT) / "graph_render_test_SampleCS.spv");
            auto sampleLayout = rhi->CreateBindGroupLayout({{0, B::StorageBuffer, S::Compute}, {1, B::SampledTexture, S::Compute}});
            auto sampleGroup = sampleLayout ? rhi->CreateBindGroup(*sampleLayout, 1) : nullptr;
            ixrhi::IXRHIComputePipelineDesc computeDesc; computeDesc.computeShader = rhi->CreateShader(sampleShader);
            computeDesc.bindGroupLayouts = {sampleLayout.get()};
            auto samplePipeline = computeDesc.computeShader ? rhi->CreateComputePipeline(computeDesc) : nullptr;
            const auto buffer = [&](U usage, bool host) {
                ixrhi::IXRHIBufferDesc d; d.sizeBytes = 16; d.usage = usage; d.cpuAccess = host ? ixrhi::IXRHICpuAccess::Write : ixrhi::IXRHICpuAccess::None;
                return rhi->CreateBuffer(d, nullptr, 0);
            };
            auto output = buffer(U::Storage | U::TransferSrc, false), readback = buffer(U::TransferDst, true);
            auto sampler = rhi->CreateSampler({});
            Check(samplePipeline && sampleGroup && output && readback && sampler, "read-back compute pass");
            if (!ready || !samplePipeline || !sampleGroup || !output || !readback)
            { rhi->WaitIdle(); renderer.Destroy(); StaticMeshRenderer::SetGraphEnvironment(nullptr); environment.Destroy(); rhi->Shutdown(); return; }
            sampleGroup->UpdateBuffer(0, 0, output, 0, 16);
            sampleGroup->UpdateTexture(0, 1, scene.GetColorTexture(), sampler);

            StaticMeshRenderer::Instance instance{};
            instance.entityId = 7;
            WorldCamera camera{};
            camera.viewProjection = WorldIdentity();
            bool shadowReady = false;
            using State = ixrhi::IXRHIBufferState;
            auto outputState = State::Undefined, readState = State::Undefined;
            std::function<void(const ixrhi::IXRHIFrameInfo&)> beforeDraw;  // e.g. GraphEnvironment::Begin
            bool transparent = false;  // drawn by the transparent pass instead of the opaque one
            const auto render = [&](const std::string& material, float* pixel) {
                instance.materialSlots = {material};
                auto frame = rhi->BeginFrame();
                if (!frame) return false;
                auto& cmd = *frame.info.commandList;
                if (!shadowReady)
                {
                    cmd.TransitionTexture(*shadow.texture, ixrhi::IXRHIImageLayout::Undefined, ixrhi::IXRHIImageLayout::ShaderReadOnly);
                    shadowReady = true;
                }
                renderer.RecordPendingUploads(cmd, frame.info);  // as the engine does every frame
                if (beforeDraw)
                    beforeDraw(frame.info);
                scene.BeginMainPass(cmd, frame.info);
                if (transparent)
                    renderer.RenderTransparentInWorld(cmd, frame.info, 0, camera, instance, 96, 64);
                else
                    renderer.RenderBatchInWorld(cmd, frame.info, 0, camera, {&instance}, 96, 64);
                scene.EndMainPass(cmd);
                cmd.TransitionBuffer(*output, outputState, State::ShaderWrite); outputState = State::ShaderWrite;
                cmd.SetComputePipeline(*samplePipeline); cmd.BindGroup(0, *sampleGroup, 0); cmd.Dispatch(1, 1, 1);
                cmd.TransitionBuffer(*output, outputState, State::TransferSrc); outputState = State::TransferSrc;
                cmd.TransitionBuffer(*readback, readState, State::TransferDst);
                cmd.CopyBuffer(*output, *readback, 16);
                cmd.TransitionBuffer(*readback, State::TransferDst, State::HostRead); readState = State::HostRead;
                rhi->GetMainRenderTarget()->Begin(cmd); rhi->GetMainRenderTarget()->End(cmd);
                rhi->EndFrame(frame); rhi->WaitIdle();
                readback->Read(0, pixel, 16);
                return true;
            };
            // Textures load on a worker: the graph is drawn once its texture is resident.
            const std::array<float, 3> expected = {0.5f * 0.5f + 128 / 255.0f, 0.25f * 0.5f + 32 / 255.0f, 0.125f * 0.5f + 200 / 255.0f};
            float pixel[4]{};
            bool drawn = false;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!drawn && std::chrono::steady_clock::now() < deadline)
            {
                drawn = render(graphMaterial, pixel) && Near(pixel, expected);
                if (!drawn) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            std::cout << "graph pixel " << pixel[0] << ' ' << pixel[1] << ' ' << pixel[2] << '\n';
            Check(drawn, "compiled graph shades the pixel: albedo parameter lit by ambient plus a sampled texture as emissive");
            Check(!renderer.m_lastMaterialBindings.empty() && renderer.m_lastMaterialBindings[0].pipelineName == "StaticMesh:GraphOpaque",
                  "the opaque draw uses the graph pipeline");

            // Live value change from an editor: a new uniform block reaches the next frame without recompiling.
            auto liveValues = package.uniforms;
            liveValues[0] = {1.0f, 0.0f, 0.0f, 1.0f};
            GraphLiveValues::Instance().Set(graphMaterial, liveValues);
            Check(render(graphMaterial, pixel) && Near(pixel, {0.5f + 128 / 255.0f, 32 / 255.0f, 200 / 255.0f}),
                  "a live parameter value is drawn by the next frame");
            auto wrongSize = liveValues; wrongSize.push_back({});
            GraphLiveValues::Instance().Set(graphMaterial, wrongSize);
            Check(render(graphMaterial, pixel) && Near(pixel, {0.5f + 128 / 255.0f, 32 / 255.0f, 200 / 255.0f}),
                  "live values that do not fit the loaded shader are ignored");
            GraphLiveValues::Instance().Set(graphMaterial, liveValues);

            // A material without a graph keeps the baked path: white default texture lit by ambient.
            Check(render(plainMaterial, pixel) && Near(pixel, {0.5f, 0.5f, 0.5f}) &&
                  renderer.m_lastMaterialBindings[0].pipelineName == "StaticMesh:Opaque",
                  "a material without a compiled graph is drawn from its textures");

            // An emissive map is sampled (sRGB) and multiplied by the emissive colour, not added as plain white.
            const auto linear = [](float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
            const std::array<float, 3> glowing = {0.5f + linear(128 / 255.0f), 0.5f + linear(32 / 255.0f), 0.5f + linear(200 / 255.0f)};
            bool emissiveDrawn = false;
            const auto emissiveDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!emissiveDrawn && std::chrono::steady_clock::now() < emissiveDeadline)
                emissiveDrawn = render(glowingMaterial, pixel) && Near(pixel, glowing);
            std::cout << "emissive pixel " << pixel[0] << ' ' << pixel[1] << ' ' << pixel[2] << '\n';
            Check(emissiveDrawn, "a material's emissive map is sampled, not drawn as uniform emissive colour");

            // A broken republished package keeps the last working shader.
            Check(WriteFile(root / "Graph" / "graph.ixgraphshader", "not a package"), "package is overwritten with garbage");
            MaterialAssetManager::Instance().markChanged();
            Check(render(graphMaterial, pixel) && Near(pixel, {0.5f + 128 / 255.0f, 32 / 255.0f, 200 / 255.0f}) &&
                  renderer.m_lastMaterialBindings[0].pipelineName == "StaticMesh:GraphOpaque",
                  "a failed reload keeps drawing with the last working graph shader");

            // A fixed package replaces it, without stopping frames in flight.
            GraphLiveValues::Instance().Clear(graphMaterial);  // a new publication starts without live edits
            package.uniforms[0] = {0.0f, 0.5f, 0.0f, 1.0f};
            Check(EncodeGraphShaderPackage(package, bytes, error) && WriteFile(root / "Graph" / "graph.ixgraphshader", bytes), "package is republished");
            MaterialAssetManager::Instance().markChanged();
            Check(render(graphMaterial, pixel) && render(graphMaterial, pixel) && Near(pixel, {128 / 255.0f, 0.25f + 32 / 255.0f, 200 / 255.0f}),
                  "a republished graph shader replaces the previous one");

            // The scene inputs (GraphEnvironment, set 2): the emissive colour carries the time, the scene
            // depth behind the quad and the mirrored sky (R = time / 4, G = depth / 4, B = sky blue).
            MaterialGraphDocument sceneGraph;
            const auto time = sceneGraph.AddNode(NodeKind::Time, {}), depth = sceneGraph.AddNode(NodeKind::SceneDepth, {}),
                       mirror = sceneGraph.AddNode(NodeKind::SkyReflection, {}), split = sceneGraph.AddNode(NodeKind::SplitColor, {}),
                       timeScale = sceneGraph.AddNode(NodeKind::Multiply, {}), depthScale = sceneGraph.AddNode(NodeKind::Multiply, {}),
                       make = sceneGraph.AddNode(NodeKind::MakeColor, {}), black = sceneGraph.AddNode(NodeKind::Color, {}),
                       sceneOutput = sceneGraph.AddNode(NodeKind::PbrOutput, {});
            sceneGraph.FindNode(black)->value = {0, 0, 0, 1};
            sceneGraph.FindNode(timeScale)->value[1] = 0.25f;
            sceneGraph.FindNode(depthScale)->value[1] = 0.25f;
            sceneGraph.FindNode(depth)->value[0] = 3;  // the fallback depth
            Check(sceneGraph.Connect(PinId(time, 0), PinId(timeScale, 0), error) && sceneGraph.Connect(PinId(depth, 0), PinId(depthScale, 0), error) &&
                  sceneGraph.Connect(PinId(mirror, 1), PinId(split, 0), error) && sceneGraph.Connect(PinId(timeScale, 2), PinId(make, 0), error) &&
                  sceneGraph.Connect(PinId(depthScale, 2), PinId(make, 1), error) && sceneGraph.Connect(PinId(split, 3), PinId(make, 2), error) &&
                  sceneGraph.Connect(PinId(make, 4), PinId(sceneOutput, 3), error) && sceneGraph.Connect(PinId(black, 0), PinId(sceneOutput, 0), error),
                  "scene input graph connects: " + error);
            MaterialGraphProgram sceneProgram;
            GraphShaderPackage scenePackage;
            Check(sceneProgram.Compile(sceneGraph, error) && CompileGraphShader(sceneProgram, scenePackage, error, compiler) &&
                  sceneProgram.BuildRuntimeUniforms({}, {}, scenePackage.uniforms, error) && EncodeGraphShaderPackage(scenePackage, bytes, error) &&
                  WriteFile(root / "Scene" / "graph.ixgraphshader", bytes) &&
                  WriteFile(root / "Scene" / "Scene.material", MaterialJson("Scene", "graph.ixgraphshader")) &&
                  WriteFile(root / "SceneBlend" / "graph.ixgraphshader", bytes) &&
                  WriteFile(root / "SceneBlend" / "SceneBlend.material", MaterialJson("SceneBlend", "graph.ixgraphshader", {}, "Blend")),
                  "scene input graph is published: " + error);
            Check(GraphShaderReadsSceneDepth(scenePackage.maskedSpirv) && !GraphShaderReadsSceneDepth(package.maskedSpirv),
                  "only a graph with Scene Depth samples the scene depth");
            const auto sceneMaterial = AssetDatabase::Instance().getOrCreateGuid(root / "Scene" / "Scene.material").toString();
            const auto sceneBlendMaterial = AssetDatabase::Instance().getOrCreateGuid(root / "SceneBlend" / "SceneBlend.material").toString();
            SkyRenderer::Lookup sky;
            sky.parameters[3] = 0;  // mode 0: a flat colour, the same in every direction
            sky.parameters[24] = 0.2f; sky.parameters[25] = 0.4f; sky.parameters[26] = 0.6f;
            environment.SetSky(sky);
            // The view's depth copy: one texel at depth 191/255, behind the quad at 0.5.
            ixrhi::IXRHITextureDesc depthDesc;
            depthDesc.width = depthDesc.height = 1; depthDesc.format = ixrhi::IXRHIFormat::R8G8B8A8Unorm;
            depthDesc.usage = ixrhi::IXRHITextureUsage::Sampled;
            const std::uint8_t depthTexel[4] = {191, 0, 0, 255};
            auto sceneDepth = rhi->CreateTexture(depthDesc, depthTexel, sizeof(depthTexel));
            camera.nearPlane = 1; camera.farPlane = 3;
            const auto linearDepth = [&](float ndc) {
                const float a = camera.farPlane / (camera.farPlane - camera.nearPlane), b = camera.nearPlane * camera.farPlane / (camera.farPlane - camera.nearPlane);
                return b / (a - ndc);
            };
            const float behind = linearDepth(191 / 255.0f) - linearDepth(0.5f);
            std::uint64_t lastFrame = 0;
            const auto begin = [&](double seconds, bool withDepth) {
                beforeDraw = [&, seconds, withDepth](const ixrhi::IXRHIFrameInfo& info) {
                    lastFrame = info.frameNumber;
                    environment.Begin(info, seconds, camera, 96, 64, withDepth ? sceneDepth : nullptr);
                };
            };
            begin(2.0, true);
            Check(sceneDepth && render(sceneMaterial, pixel) && Near(pixel, {0.5f, behind * 0.25f, 0.6f}) &&
                  renderer.m_lastMaterialBindings[0].pipelineName == "StaticMesh:GraphOpaque",
                  "Time, Scene Depth and Sky Reflection read the view's time, depth copy and sky");
            std::cout << "scene pixel " << pixel[0] << ' ' << pixel[1] << ' ' << pixel[2] << " (depth behind " << behind << ")\n";
            begin(1.0, false);
            Check(render(sceneMaterial, pixel) && Near(pixel, {0.25f, 0.75f, 0.6f}),
                  "the next frame's time is drawn; without a depth copy Scene Depth uses its fallback");
            beforeDraw = nullptr;
            Check(render(sceneMaterial, pixel) && Near(pixel, {0.25f, 0.75f, 0.6f}),
                  "a frame drawn without a Begin keeps the last view's time");
            transparent = true;
            begin(2.0, true);
            Check(render(sceneBlendMaterial, pixel) && Near(pixel, {0.5f, behind * 0.25f, 0.6f}) &&
                  environment.SceneDepthReadRecently(lastFrame),
                  "a transparent graph reads the depth copy and asks the frame owner to keep copying it");
            transparent = false;
            beforeDraw = nullptr;

            rhi->WaitIdle();
            renderer.Destroy();
            scene.Destroy();
            shadow = {};
        }
        StaticMeshRenderer::SetGraphEnvironment(nullptr);
        environment.Destroy();
        rhi->Shutdown(); rhi.reset(); native.Destroy(); window.Destroy();
        std::filesystem::remove_all(root, ec);
    }
};
#endif

int main(int argc, char** argv)
{
#if defined(_WIN32)
    if ((argc == 3 || argc == 4) && std::string(argv[1]) == "--water")
    {
        GraphMaterialRenderTestAccess::WaterScene(argv[2], argc == 4 ? std::atof(argv[3]) : 0.0);
        std::cout << "Graph water scene failures: " << failures << '\n';
        return failures ? 1 : 0;
    }
    if (argc == 6 && std::string(argv[1]) == "--compare")
    {
        GraphMaterialRenderTestAccess::Compare(argv[2], argv[3], argv[4], argv[5]);
        std::cout << "Graph material compare failures: " << failures << '\n';
        return failures ? 1 : 0;
    }
    GraphMaterialRenderTestAccess::Run();
#else
    std::cout << "SKIP GPU graph material test needs the Windows Vulkan harness\n";
#endif
    std::cout << "Graph material render failures: " << failures << '\n';
    return failures ? 1 : 0;
}
