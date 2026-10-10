#include "StaticMeshRenderer.h"
#include "LODSystem.h"
#include "asset/FileAssetReader.h"
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#if defined(_WIN32)
#include "IXVulkanDevice.h"
#include "VulkanDevice.h"
#include "NativeWindow_Win32.h"
#endif

namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::cout << (ok ? "PASS " : "FAIL ") << label << '\n';
    failures += !ok;
}
}

struct StaticMeshLodGenerationTestAccess {
    static void Seed(StaticMeshRenderer& renderer) {
        // Two continuous grids with separate materials: real simplification,
        // not a diagnostics fixture with invented triangle counts.
        constexpr unsigned side = 64;
        for (unsigned part = 0; part < 2; ++part) {
            const auto base = static_cast<unsigned>(renderer.m_vertices.size());
            const auto first = static_cast<unsigned>(renderer.m_indices.size());
            for (unsigned y = 0; y <= side; ++y)
                for (unsigned x = 0; x <= side; ++x) {
                    StaticMeshRenderer::Vertex vertex{};
                    vertex.position[0] = float(x + part * (side + 1));
                    vertex.position[1] = float(y);
                    vertex.normal[2] = 1;
                    renderer.m_vertices.push_back(vertex);
                }
            for (unsigned y = 0; y < side; ++y)
                for (unsigned x = 0; x < side; ++x) {
                    const unsigned a = base + y * (side + 1) + x;
                    for (const auto index : {a, a + 1, a + side + 2, a, a + side + 2, a + side + 1})
                        renderer.m_indices.push_back(index);
                }
            renderer.m_draws.push_back({first, side * side * 6, part, (side + 1) * (side + 1)});
        }
    }
    static void Cpu() {
        LodConfig config;
        const auto hash = HashLodConfig(config);
        auto thresholds = config;
        thresholds.distances[1] = 40;
        thresholds.hysteresisMeters = 12;
        Check(HashLodConfig(thresholds) == hash, "moving transitions reuses generated geometry");
        thresholds.targetRatios[1] = 0.4f;
        Check(HashLodConfig(thresholds) != hash, "changing target geometry invalidates cache");
        thresholds = config; thresholds.levelCount = 3;
        Check(HashLodConfig(thresholds) != hash, "changing level count invalidates cache");
        Check(SelectLodLevel(config, 1.9f, 0) == 0 && SelectLodLevel(config, 40, 0) == 1 &&
            SelectLodLevel(config, 90, 1) == 2 && SelectLodLevel(config, 210, 2) == 3,
            "camera distance selects all four levels");
        const SpatialIndex::Aabb largeBounds{{-40, -10, -40}, {40, 10, 40}};
        Check(DistanceToAabb({30, 0, 0}, largeBounds) == 0 && MeshLodDistance({30, 0, 0}, largeBounds) == 30,
            "camera inside a large box still has a meaningful LOD distance");
        Check(SelectLodLevel(config, MeshLodDistance({35, 0, 0}, largeBounds), 0) == 1,
            "large bounding box does not pin model to LOD zero");
        StaticMeshRenderer renderer;
        Seed(renderer);
        auto cpu = renderer.BuildLodCpuSet(config, hash, true, 42);
        Check(cpu.levels == 4 && cpu.triangles[0] == 16384, "quality build preserves original LOD zero");
        for (unsigned level = 1; level < 4; ++level) {
            Check(cpu.triangles[level] > 0 && cpu.triangles[level] < cpu.triangles[0], "generated level reduces geometry");
            Check(cpu.draws[level * 2].materialSlot == 0 && cpu.draws[level * 2 + 1].materialSlot == 1,
                "generated levels preserve both material slots");
        }
        bool valid = true;
        for (auto index : cpu.indices) valid &= index < renderer.m_vertices.size();
        Check(valid, "generated indices reference the original vertex pool");
        StaticMeshRenderer split;
        Seed(split);
        std::vector<StaticMeshRenderer::Vertex> splitVertices;
        for (auto& index : split.m_indices) {
            auto vertex = split.m_vertices[index];
            // Imported faces with independent UV/normal seams, even where
            // positions meet. The conservative simplifier locks these vertices.
            vertex.uv[0] = float(splitVertices.size());
            index = static_cast<unsigned>(splitVertices.size());
            splitVertices.push_back(vertex);
        }
        split.m_vertices = std::move(splitVertices);
        const auto originalVertices = split.m_vertices;
        config.targetRatios[1] = 0.215f;
        config.targetRatios[2] = 0.125f;
        config.targetRatios[3] = 0.01f;
        auto aggressive = split.BuildLodCpuSet(config, HashLodConfig(config), true, 43);
        Check(aggressive.triangles[3] > 0 && aggressive.triangles[3] <= aggressive.triangles[0] * 0.015f,
            "split-vertex mesh reaches an aggressive distant triangle budget");
        Check(aggressive.triangles[1] >= aggressive.triangles[2] && aggressive.triangles[2] >= aggressive.triangles[3],
            "decreasing targets produce progressively coarser levels");
        Check(split.m_vertices.size() == originalVertices.size() &&
            std::memcmp(split.m_vertices.data(), originalVertices.data(), originalVertices.size() * sizeof(originalVertices[0])) == 0,
            "aggressive build preserves original positions normals and UV attributes");
        bool nondegenerate = true;
        for (std::size_t i = 0; i < aggressive.indices.size(); i += 3) {
            const auto a = aggressive.indices[i], b = aggressive.indices[i + 1], c = aggressive.indices[i + 2];
            nondegenerate &= a < split.m_vertices.size() && b < split.m_vertices.size() && c < split.m_vertices.size() &&
                a != b && b != c && a != c;
        }
        Check(nondegenerate, "aggressive fallback never fabricates zero-index triangles");
    }
    static void Model(const std::string& path) {
        client::asset::FileAssetReader assets(std::filesystem::path(path).parent_path());
        StaticMeshRenderer renderer;
        if (!renderer.LoadCpu(assets, path)) { Check(false, "load real FBX geometry"); return; }
        LodConfig config;
        config.targetRatios[1] = 0.215f;
        config.targetRatios[2] = 0.125f;
        config.targetRatios[3] = 0.01f;
        auto cpu = renderer.BuildLodCpuSet(config, HashLodConfig(config), true, 42);
        std::cout << "Model LOD triangles: " << cpu.triangles[0] << " / " << cpu.triangles[1]
            << " / " << cpu.triangles[2] << " / " << cpu.triangles[3] << '\n';
        Check(cpu.triangles[3] > 0 && cpu.triangles[3] <= cpu.triangles[0] * 0.015f,
            "one percent FBX target no longer stops at sixteen percent");
        bool valid = true, materials = true;
        for (auto index : cpu.indices) valid &= index < renderer.m_vertices.size();
        for (const auto& draw : cpu.draws)
            materials &= draw.sourceDraw < renderer.m_draws.size() &&
                draw.materialSlot == renderer.m_draws[draw.sourceDraw].materialSlot && draw.indexCount >= 3;
        Check(valid && materials, "real FBX LOD retains valid indices and all submesh materials");
    }
#if defined(_WIN32)
    static void Gpu() {
        NativeWindow_Win32 window;
        if (!window.Create(GetModuleHandle(nullptr), "LOD generation regression", 96, 64)) {
            Check(false, "hidden GPU window"); return;
        }
        ShowWindow(window.GetHwnd(), SW_HIDE);
        VulkanDevice native;
        if (!native.Create(window, 96, 64)) {
            Check(false, "GPU device"); window.Destroy(); return;
        }
        auto rhi = ixvulkan::CreateDevice(native);
        client::asset::FileAssetReader assets(IX_LOD_TEST_ASSETS_ROOT);
        StaticMeshRenderer renderer;
        Seed(renderer);
        renderer.m_assets = &assets;
        renderer.m_materialDefaults.resize(2);
        renderer.DecodeTextures("");
        if (!renderer.FinishGpu(*rhi)) {
            Check(false, "GPU mesh pipeline"); renderer.Destroy();
            rhi->Shutdown(); rhi.reset(); native.Destroy(); window.Destroy(); return;
        }
        ixrhi::IXRHITextureDesc shadowDesc;
        shadowDesc.width = shadowDesc.height = 1;
        shadowDesc.arrayLayers = 4;
        shadowDesc.format = ixrhi::IXRHIFormat::D32Float;
        shadowDesc.usage = ixrhi::IXRHITextureUsage::Sampled | ixrhi::IXRHITextureUsage::TransferDst;
        SunShadowReceive shadow;
        shadow.texture = rhi->CreateTexture(shadowDesc, nullptr, 0);
        ixrhi::IXRHISamplerDesc sampler;
        sampler.compareEnable = true;
        sampler.compareOp = ixrhi::IXRHICompareOp::LessOrEqual;
        shadow.sampler = rhi->CreateSampler(sampler);
        renderer.SetSunShadow(shadow);
        Check(shadow.texture && shadow.sampler, "GPU shadow descriptors ready");
        LodConfig config;
        auto hash = HashLodConfig(config);
        StaticMeshRenderer::Instance instance{};
        instance.entityId = 42;
        instance.selectedForOutline = true;
        const StaticMeshRenderer::InstanceList instances{&instance};
        WorldCamera camera{};
        camera.viewProjection = WorldIdentity();
        bool shadowTransitioned = false;
        auto pump = [&] {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (std::chrono::steady_clock::now() < deadline) {
                auto frame = rhi->BeginFrame();
                if (!frame) break;
                if (!shadowTransitioned) {
                    frame.info.commandList->TransitionTexture(*shadow.texture, ixrhi::IXRHIImageLayout::Undefined,
                        ixrhi::IXRHIImageLayout::ShaderReadOnly);
                    shadowTransitioned = true;
                }
                // Regression: only LOD zero is submitted, and the instance stays
                // selected. This must still generate and upload all distant LODs.
                rhi->GetMainRenderTarget()->Begin(*frame.info.commandList);
                renderer.RenderLodBatchInWorld(*frame.info.commandList, frame.info, 0, camera,
                    instances, config, hash, 0, 96, 64);
                rhi->GetMainRenderTarget()->End(*frame.info.commandList);
                rhi->EndFrame(frame);
                if (renderer.GetLodDiagnostics(hash).bufferValid) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return false;
        };
        Check(pump(), "selected LOD zero drawing completes background build and GPU upload");
        const auto diag = renderer.GetLodDiagnostics(hash);
        Check(diag.levelCount == 4 && !diag.pendingUpload && !diag.buildPending,
            "all generated levels become ready");
        for (unsigned level = 1; level < 4; ++level)
            Check(renderer.TriangleCountForLod(hash, level) == diag.levelTris[level] &&
                diag.levelTris[level] > 0 && diag.levelTris[level] < 16384,
                "uploaded distant LOD uses fewer triangles");
        config.targetRatios[1] = 0.35f;
        hash = HashLodConfig(config);
        renderer.RequestLodQualityBuild(config, hash, 42);
        Check(pump(), "inspector rebuild completes without switching away from LOD zero");
        config.levelCount = 3;
        hash = HashLodConfig(config);
        Check(pump() && renderer.GetLodDiagnostics(hash).levelCount == 3,
            "changed level count automatically rebuilds without a manual request");
        for (const bool occluder : {false, true}) {
            instance.selectedForOutline = !occluder;
            instance.forceFullResolution = occluder;
            for (unsigned level = 0; level < config.levelCount; ++level) {
                auto frame = rhi->BeginFrame();
                if (!frame) { Check(false, "GPU LOD drawing frame"); break; }
                rhi->GetMainRenderTarget()->Begin(*frame.info.commandList);
                renderer.RenderLodBatchInWorld(*frame.info.commandList, frame.info, 0, camera,
                    instances, config, hash, level, 96, 64);
                Check(renderer.LastSubmittedTriangles() == renderer.TriangleCountForLod(hash, level) &&
                    renderer.LastSubmittedTriangles() > 0,
                    occluder ? "occluder colour pass draws the requested mesh LOD" : "selected model draws the requested mesh LOD");
                rhi->GetMainRenderTarget()->End(*frame.info.commandList);
                rhi->EndFrame(frame);
            }
        }
        rhi->WaitIdle();
        renderer.Destroy();
        shadow.texture.reset(); shadow.sampler.reset();
        rhi->Shutdown(); rhi.reset(); native.Destroy(); window.Destroy();
    }
#endif
};

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    StaticMeshLodGenerationTestAccess::Cpu();
    if (argc > 2 && std::string(argv[1]) == "--model") StaticMeshLodGenerationTestAccess::Model(argv[2]);
#if defined(_WIN32)
    if (argc > 1 && std::string(argv[1]) == "--gpu") StaticMeshLodGenerationTestAccess::Gpu();
#endif
    return failures ? 1 : 0;
}
