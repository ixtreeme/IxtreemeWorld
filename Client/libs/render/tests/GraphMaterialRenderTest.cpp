// A compiled material graph drawn by the real static mesh renderer on the GPU, read back per pixel:
// the pipeline with descriptor set 1, its uniform block and textures, live value changes, the
// fallback to baked textures, and keeping the last working shader when a reload fails.
#include "StaticMeshRenderer.h"
#include "OffscreenSceneRenderer.h"
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
std::string MaterialJson(const std::string& name, const std::string& graphShader, const std::string& emissiveTexture = {})
{
    return "{\n  \"version\": 2,\n  \"shader\": \"PBR-Standard\",\n  \"shadingMode\": \"Lit\",\n  \"name\": \"" + name +
        "\",\n  \"baseColor\": [1, 1, 1, 1],\n  \"alphaMode\": \"Opaque\",\n" +
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
            if (!ready || !samplePipeline || !sampleGroup || !output || !readback) { rhi->WaitIdle(); renderer.Destroy(); rhi->Shutdown(); return; }
            sampleGroup->UpdateBuffer(0, 0, output, 0, 16);
            sampleGroup->UpdateTexture(0, 1, scene.GetColorTexture(), sampler);

            StaticMeshRenderer::Instance instance{};
            instance.entityId = 7;
            WorldCamera camera{};
            camera.viewProjection = WorldIdentity();
            bool shadowReady = false;
            using State = ixrhi::IXRHIBufferState;
            auto outputState = State::Undefined, readState = State::Undefined;
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
                scene.BeginMainPass(cmd, frame.info);
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

            rhi->WaitIdle();
            renderer.Destroy();
            scene.Destroy();
            shadow = {};
        }
        rhi->Shutdown(); rhi.reset(); native.Destroy(); window.Destroy();
        std::filesystem::remove_all(root, ec);
    }
};
#endif

int main(int argc, char** argv)
{
#if defined(_WIN32)
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
