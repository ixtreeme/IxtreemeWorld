#include "ParticleRenderer.h"
#include "SceneManager.h"
#include "particles/ParticleEffectIO.h"
#include "particles/ParticleSimulator.h"
#include "asset/FileAssetReader.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>

#if defined(_WIN32)
#include "IXVulkanDevice.h"
#include "VulkanDevice.h"
#include "NativeWindow_Win32.h"
#endif

namespace {
int failures = 0;
void Check(bool ok, const char* label)
{
    std::cout << (ok ? "PASS " : "FAIL ") << label << '\n';
    failures += !ok;
}

ixparticle::ParticleSystemComponent Dust()
{
    ixparticle::ParticleSystemComponent p;
    p.shape = ixparticle::ParticleShape::Box;
    p.shapeExtents[0] = 5;
    p.shapeExtents[1] = 2;
    p.shapeExtents[2] = 5;
    p.emissionRate = 0;
    p.burstCount = 16;
    p.maxParticles = 32;
    p.duration = 100;
    p.startLifetimeMin = p.startLifetimeMax = 60;
    p.startSpeedMin = p.startSpeedMax = 0;
    p.gravity = 0;
    p.drag = 0.3f;
    p.turbulenceEnabled = true;
    p.turbulenceStrength = 0.03f;
    p.turbulenceScale = 3;
    p.turbulenceSpeed = 0.35f;
    return p;
}

constexpr float position[3] = {0.4f, 2.5f, -0.7f};
constexpr float direction[3] = {0, 1, 0};
void Step(ixparticle::ParticleSimulator& sim, const ixparticle::ParticleSystemComponent& p,
          int steps, float dt = 1.0f / 60.0f)
{
    for (int i = 0; i < steps; ++i) sim.Update(p, position, direction, dt);
}

bool SameMotion(const ixparticle::ParticleSimulator& a, const ixparticle::ParticleSimulator& b,
                float tolerance = 0)
{
    if (a.AliveCount() != b.AliveCount()) return false;
    for (std::size_t i = 0; i < a.AliveCount(); ++i)
        for (int c = 0; c < 3; ++c)
            if (std::abs(a.Particles()[i].position[c] - b.Particles()[i].position[c]) > tolerance ||
                std::abs(a.Particles()[i].velocity[c] - b.Particles()[i].velocity[c]) > tolerance)
                return false;
    return true;
}

void Cpu()
{
    auto p = Dust();
    ixparticle::ParticleSimulator sim;
    sim.Reset(p, 42);
    Step(sim, p, 1);
    const auto initial = sim.Particles();
    Step(sim, p, 600);
    bool moved = true, finite = true, bounded = true;
    for (std::size_t i = 0; i < sim.AliveCount(); ++i)
    {
        float distance2 = 0, speed2 = 0;
        for (int c = 0; c < 3; ++c)
        {
            const auto& particle = sim.Particles()[i];
            const float offset = particle.position[c] - initial[i].position[c];
            distance2 += offset * offset;
            speed2 += particle.velocity[c] * particle.velocity[c];
            finite &= std::isfinite(particle.position[c]) && std::isfinite(particle.velocity[c]);
        }
        moved &= distance2 > 0.00001f;
        bounded &= speed2 < 0.04f;
    }
    Check(sim.AliveCount() == 16 && moved, "stationary dust starts drifting throughout a box");
    Check(finite && bounded, "dust remains finite and slow with drag over ten seconds");
    Check(sim.Particles()[0].velocity[0] != sim.Particles()[1].velocity[0],
        "different positions experience different flow");

    ixparticle::ParticleSimulator repeat;
    repeat.Reset(p, 42);
    Step(repeat, p, 601);
    Check(SameMotion(sim, repeat), "same seed and timesteps reproduce turbulence");
    sim.Reset(p, 42);
    Step(sim, p, 601);
    Check(SameMotion(sim, repeat), "restart resets the turbulence clock");

    p.turbulenceEnabled = false;
    sim.Reset(p, 42);
    Step(sim, p, 601);
    bool stationary = true;
    for (std::size_t i = 0; i < sim.AliveCount(); ++i)
        for (int c = 0; c < 3; ++c)
            stationary &= sim.Particles()[i].position[c] == initial[i].position[c];
    Check(stationary, "disabled turbulence preserves existing stationary motion");
    p.turbulenceEnabled = true;
    p.turbulenceStrength = 0;
    repeat.Reset(p, 42);
    Step(repeat, p, 601);
    Check(SameMotion(sim, repeat), "zero strength preserves existing motion");

    p = Dust();
    sim.Reset(p, 42); repeat.Reset(p, 42);
    Step(sim, p, 1); Step(repeat, p, 1);
    Step(sim, p, 180); Step(repeat, p, 90, 1.0f / 30.0f);
    Check(SameMotion(sim, repeat, 0.006f), "30 and 60 FPS give consistent dust motion");

    sim.Reset(p, 42); repeat.Reset(p, 42);
    Step(sim, p, 1); Step(repeat, p, 1);
    p.burstCount = 0;
    sim.Stop();
    auto looping = p; looping.duration = 0.05f;
    Step(sim, p, 180); Step(repeat, looping, 180);
    Check(SameMotion(sim, repeat), "flow continues through Stop and emission loop boundaries");

    auto animated = Dust();
    animated.shape = ixparticle::ParticleShape::Point;
    animated.drag = 0;
    animated.turbulenceStrength = 0.00001f; // sample temporal evolution with negligible spatial drift
    auto staticFlow = animated; staticFlow.turbulenceSpeed = 0;
    sim.Reset(animated, 42); repeat.Reset(staticFlow, 42);
    Step(sim, animated, 601); Step(repeat, staticFlow, 601);
    Check(!SameMotion(sim, repeat, 0.00001f), "speed animates the flow independently of particle age");
}

void Serialization()
{
    auto p = Dust();
    p.turbulenceStrength = 0.0375f;
    p.turbulenceScale = 3.25f;
    p.turbulenceSpeed = 0.42f;
    const auto matches = [&](const ixparticle::ParticleSystemComponent& read) {
        return read.turbulenceEnabled && read.turbulenceStrength == p.turbulenceStrength &&
            read.turbulenceScale == p.turbulenceScale && read.turbulenceSpeed == p.turbulenceSpeed;
    };
    ixparticle::ParticleSystemComponent restored;
    Check(ixparticle::ParseParticleEffectJson(ixparticle::WriteParticleEffectJson(p, "dust", "Dust"), restored)
        && matches(restored), "effect presets preserve all turbulence settings");
    Check(ixparticle::ParseParticleEffectJson("{\"gravity\":0}", restored) && !restored.turbulenceEnabled,
        "applying an old preset disables turbulence even on an enabled emitter");
    Check(ixparticle::ParseParticleEffectJson(
        "{\"turbulence_strength\":-1,\"turbulence_scale\":0,\"turbulence_speed\":-2}", restored) &&
        restored.turbulenceStrength == 0 && restored.turbulenceScale == 0.01f && restored.turbulenceSpeed == 0,
        "invalid turbulence settings are clamped on load");

    namespace fs = std::filesystem;
    const auto parent = fs::weakly_canonical(fs::temp_directory_path());
    const auto root = parent / ("ixw_particle_test_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (!fs::create_directory(root)) { Check(false, "temporary scene directory"); return; }
    SceneData scene;
    MeshSceneEntity mesh;
    mesh.id = 1; mesh.name = "Dust"; mesh.hasParticleSystem = true; mesh.particleSystem = p;
    scene.meshEntities.push_back(mesh);
    auto& manager = SceneManager::Instance();
    manager.RestoreSceneSnapshot(scene, {}, false);
    const auto path = root / "dust.scene";
    const bool saved = manager.SaveSceneAs(path.string());
    manager.CloseScene();
    const bool loaded = saved && manager.LoadScene(path.string());
    Check(loaded && manager.GetCurrentScene().meshEntities.size() == 1 &&
        matches(manager.GetCurrentScene().meshEntities[0].particleSystem),
        "scene save and reload preserve all turbulence settings");
    manager.CloseScene();
    std::ifstream input(path);
    std::ofstream legacy(root / "legacy.scene");
    std::string line;
    while (std::getline(input, line))
        if (line.find("\"turbulence_") == std::string::npos) legacy << line << '\n';
    input.close(); legacy.close();
    Check(manager.LoadScene((root / "legacy.scene").string()) &&
        manager.GetCurrentScene().meshEntities.size() == 1 &&
        !manager.GetCurrentScene().meshEntities[0].particleSystem.turbulenceEnabled,
        "old scenes retain disabled turbulence");
    manager.CloseScene();
    if (root.parent_path() == parent && root.filename().string().starts_with("ixw_particle_test_"))
    {
        std::error_code error;
        fs::remove_all(root, error);
    }
}
} // namespace

struct ParticleTurbulenceTestAccess {
#if defined(_WIN32)
    static void Gpu()
    {
        NativeWindow_Win32 window;
        if (!window.Create(GetModuleHandle(nullptr), "Particle turbulence test", 96, 64))
        { Check(false, "GPU test window"); return; }
        ShowWindow(window.GetHwnd(), SW_HIDE);
        VulkanDevice native;
        if (!native.Create(window, 96, 64))
        { Check(false, "GPU device"); window.Destroy(); return; }
        auto rhi = ixvulkan::CreateDevice(native);
        client::asset::FileAssetReader assets(IX_PARTICLE_TEST_ASSETS_ROOT);
        ParticleRenderer renderer;
        if (!renderer.Create(*rhi, assets))
        { Check(false, "particle GPU resources"); renderer.Destroy(); rhi->Shutdown(); rhi.reset(); native.Destroy(); window.Destroy(); return; }

        auto p = Dust();
        p.shape = ixparticle::ParticleShape::Point;
        p.burstCount = p.maxParticles = 1;
        ParticleRenderer::GpuEmitterParams params;
        std::memcpy(params.emitterPosition, position, sizeof(position));
        params.shape = 0;
        params.gravity = p.gravity; params.drag = p.drag;
        params.turbulenceEnabled = true;
        params.turbulenceStrength = p.turbulenceStrength;
        params.turbulenceScale = p.turbulenceScale;
        params.turbulenceSpeed = p.turbulenceSpeed;
        params.speedMin = params.speedMax = 0;
        params.lifetimeMin = params.lifetimeMax = 60;
        params.emissionRate = 0; params.burstCount = params.maxParticles = 1;
        auto* emitter = renderer.EnsureGpuEmitter(*rhi, 42, 1);
        Check(emitter != nullptr, "GPU emitter allocation");
        if (emitter)
        {
            // Add transfer usage only to the test fixture so real compute results can be read back.
            ixrhi::IXRHIBufferDesc desc;
            desc.sizeBytes = ParticleRenderer::kGpuStateBytes;
            desc.usage = ixrhi::IXRHIBufferUsage::Storage | ixrhi::IXRHIBufferUsage::Vertex |
                ixrhi::IXRHIBufferUsage::TransferSrc;
            float zero[24]{};
            emitter->state = rhi->CreateBuffer(desc, zero, sizeof(zero));
            for (std::uint32_t i = 0; i < ParticleRenderer::kFramesInFlight; ++i)
                emitter->computeBindGroup->UpdateBuffer(i, 1, emitter->state, 0, desc.sizeBytes);
            desc.usage = ixrhi::IXRHIBufferUsage::TransferDst;
            desc.cpuAccess = ixrhi::IXRHICpuAccess::Write;
            auto readback = rhi->CreateBuffer(desc, nullptr, 0);
            Check(emitter->state && readback, "GPU readback fixture");
            ixparticle::ParticleSimulator cpu;
            cpu.Reset(p, 42);
            bool dispatched = true;
            for (int i = 0; i < 181 && dispatched; ++i)
            {
                auto frame = rhi->BeginFrame();
                if (!frame) { dispatched = false; break; }
                auto& cmd = *frame.info.commandList;
                dispatched &= renderer.SimulateGpuEmitter(cmd, frame.info, 42, params, 1.0f / 60.0f);
                if (i == 180)
                {
                    cmd.TransitionBuffer(*emitter->state, ixrhi::IXRHIBufferState::VertexRead,
                        ixrhi::IXRHIBufferState::TransferSrc);
                    cmd.CopyBuffer(*emitter->state, *readback, desc.sizeBytes);
                    cmd.TransitionBuffer(*readback, ixrhi::IXRHIBufferState::TransferDst,
                        ixrhi::IXRHIBufferState::HostRead);
                    cmd.TransitionBuffer(*emitter->state, ixrhi::IXRHIBufferState::TransferSrc,
                        ixrhi::IXRHIBufferState::VertexRead);
                }
                rhi->GetMainRenderTarget()->Begin(cmd);
                rhi->GetMainRenderTarget()->End(cmd);
                rhi->EndFrame(frame);
                Step(cpu, p, 1);
            }
            rhi->WaitIdle();
            float state[24]{};
            readback->Read(0, state, sizeof(state));
            bool agrees = dispatched;
            for (int c = 0; c < 3; ++c)
                agrees &= std::abs(state[c] - cpu.Particles()[0].position[c]) < 0.0001f &&
                    std::abs(state[16 + c] - cpu.Particles()[0].velocity[c]) < 0.0001f;
            Check(agrees, "real GPU compute and CPU turbulence agree after three seconds");
            renderer.GpuEmitterRestart(42);
            Check(emitter->simulationTime == 0, "GPU restart resets turbulence time");
            emitter->simulationTime = 9;
            renderer.ResetGpuEmitters();
            Check(emitter->simulationTime == 0, "Play session reset resets GPU turbulence time");

            params.turbulenceEnabled = false;
            params.shape = 2; // a thin box along local X, rotated onto world Y
            params.shapeExtents[0] = 2;
            params.shapeExtents[1] = params.shapeExtents[2] = 0;
            const float axes[9] = {0, 1, 0, -1, 0, 0, 0, 0, 1};
            std::memcpy(params.emitterAxes, axes, sizeof(axes));
            float spawned[24]{};
            bool boxFrames = true;
            for (int i = 0; i < 2; ++i)
            {
                auto frame = rhi->BeginFrame();
                if (!frame) { boxFrames = false; break; }
                auto& cmd = *frame.info.commandList;
                boxFrames &= renderer.SimulateGpuEmitter(cmd, frame.info, 42, params, 1.0f / 60.0f);
                cmd.TransitionBuffer(*emitter->state, ixrhi::IXRHIBufferState::VertexRead,
                    ixrhi::IXRHIBufferState::TransferSrc);
                cmd.TransitionBuffer(*readback, ixrhi::IXRHIBufferState::HostRead,
                    ixrhi::IXRHIBufferState::TransferDst);
                cmd.CopyBuffer(*emitter->state, *readback, desc.sizeBytes);
                cmd.TransitionBuffer(*readback, ixrhi::IXRHIBufferState::TransferDst,
                    ixrhi::IXRHIBufferState::HostRead);
                cmd.TransitionBuffer(*emitter->state, ixrhi::IXRHIBufferState::TransferSrc,
                    ixrhi::IXRHIBufferState::VertexRead);
                rhi->GetMainRenderTarget()->Begin(cmd);
                rhi->GetMainRenderTarget()->End(cmd);
                rhi->EndFrame(frame);
                rhi->WaitIdle();
                readback->Read(0, state, sizeof(state));
                if (i == 0) std::memcpy(spawned, state, sizeof(state));
            }
            Check(boxFrames && spawned[0] == position[0] && spawned[2] == position[2] &&
                std::abs(spawned[1] - position[1]) > 0.001f &&
                std::abs(spawned[1] - position[1]) <= 2,
                "GPU box particles spawn in the rotated volume");
            bool stationary = boxFrames;
            for (int c = 0; c < 3; ++c)
                stationary &= state[c] == spawned[c] && state[16 + c] == 0;
            Check(stationary, "disabling GPU turbulence preserves stationary dust");
            readback.reset();
        }
        rhi->WaitIdle(); renderer.Destroy();
        rhi->Shutdown(); rhi.reset(); native.Destroy(); window.Destroy();
    }
#endif
};

int main(int argc, char** argv)
{
    std::cout << std::unitbuf;
    Cpu(); Serialization();
#if defined(_WIN32)
    if (argc > 1 && std::string(argv[1]) == "--gpu") ParticleTurbulenceTestAccess::Gpu();
#endif
    return failures ? 1 : 0;
}
