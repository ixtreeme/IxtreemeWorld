#pragma once

// Particle system component data (plain old data — NO renderer/VM dependency). Lives on a
// MeshSceneEntity (like the audio/script components) and is serialized into the scene/prefab.
// The runtime side (the CPU simulator) lives in ParticleSimulator.h/.cpp; the renderer lives in
// libs/render/ParticleRenderer. Keeping this header dependency-free lets MapEditorTypes /
// SceneManager / PrefabDocument include it without dragging in the renderer or the simulator.

#include <algorithm>
#include <cstdint>
#include <string>

namespace ixparticle
{

// How the particles blend over the scene: straight alpha (soft smoke/dust) or additive (fire,
// sparks, magic). Both are premultiplied on the shader side.
enum class ParticleBlendMode : std::uint8_t
{
    Alpha = 0,
    Additive = 1
};

inline const char* BlendModeName(ParticleBlendMode mode)
{
    switch (mode)
    {
    case ParticleBlendMode::Alpha: return "Alpha";
    case ParticleBlendMode::Additive: return "Additive";
    }
    return "Alpha";
}

inline ParticleBlendMode ParseBlendMode(const std::string& s)
{
    if (s == "Additive") return ParticleBlendMode::Additive;
    return ParticleBlendMode::Alpha;
}

// One emitter on one entity. Emission is a cone around `direction` (rotated by the entity's
// rotation), spawned in a sphere of `shapeRadius` at the entity origin. Per particle the sim
// interpolates size and color from the Start* values to the End* values over its lifetime.
struct ParticleSystemComponent
{
    std::string textureAssetId;  // AssetLibrary Texture id ("" = soft white round default)
    bool enabled = true;
    bool playOnStart = true;     // begin emitting when Play starts
    bool loop = true;            // false: emission stops after `duration`, live particles finish
    ParticleBlendMode blendMode = ParticleBlendMode::Alpha;

    float duration = 3.0f;       // seconds of emission per cycle (loop) / total (no loop)
    float emissionRate = 25.0f;  // particles/second while emitting
    int burstCount = 12;         // extra particles spawned when emission (re)starts
    int maxParticles = 256;      // hard cap for this emitter

    float startLifetimeMin = 0.7f;
    float startLifetimeMax = 1.2f;
    float startSpeedMin = 0.6f;
    float startSpeedMax = 1.4f;
    float startSizeMin = 0.12f;
    float startSizeMax = 0.25f;
    float endSizeScale = 0.2f;   // size multiplier at the end of a particle's life

    float direction[3] = {0.0f, 1.0f, 0.0f};  // local emission axis
    float coneAngle = 20.0f;     // half-angle of the emission cone, degrees
    float shapeRadius = 0.15f;   // spawn sphere radius around the emitter origin

    float gravity = -1.5f;       // m/s^2 along world Y
    float drag = 0.0f;           // velocity damping per second (0 = none)
    float rotationSpeed = 0.0f;  // deg/s; each particle gets a random +/- spin

    // Soft particles: fade the sprite out as it approaches scene geometry (sampling the scene depth
    // snapshot), so the plume does not cut hard against the terrain/walls. `softDistance` is the
    // world-space distance over which the fade happens.
    bool softParticles = true;
    float softDistance = 0.5f;

    float startColor[4] = {1.0f, 0.85f, 0.5f, 1.0f};
    float endColor[4] = {1.0f, 0.25f, 0.1f, 0.0f};
};

inline void Sanitize(ParticleSystemComponent& p)
{
    p.duration = std::clamp(p.duration, 0.05f, 3600.0f);
    p.emissionRate = std::clamp(p.emissionRate, 0.0f, 5000.0f);
    p.burstCount = std::clamp(p.burstCount, 0, 10000);
    p.maxParticles = std::clamp(p.maxParticles, 1, 65536);
    p.startLifetimeMin = std::clamp(p.startLifetimeMin, 0.02f, 3600.0f);
    p.startLifetimeMax = std::clamp(p.startLifetimeMax, p.startLifetimeMin, 3600.0f);
    p.startSpeedMin = std::clamp(p.startSpeedMin, 0.0f, 1000.0f);
    p.startSpeedMax = std::clamp(p.startSpeedMax, p.startSpeedMin, 1000.0f);
    p.startSizeMin = std::clamp(p.startSizeMin, 0.0f, 1000.0f);
    p.startSizeMax = std::clamp(p.startSizeMax, p.startSizeMin, 1000.0f);
    p.endSizeScale = std::clamp(p.endSizeScale, 0.0f, 100.0f);
    p.coneAngle = std::clamp(p.coneAngle, 0.0f, 180.0f);
    p.shapeRadius = std::clamp(p.shapeRadius, 0.0f, 1000.0f);
    p.gravity = std::clamp(p.gravity, -500.0f, 500.0f);
    p.drag = std::clamp(p.drag, 0.0f, 100.0f);
    p.rotationSpeed = std::clamp(p.rotationSpeed, -3600.0f, 3600.0f);
    p.softDistance = std::clamp(p.softDistance, 0.01f, 100.0f);
    for (float& c : p.startColor)
        c = std::clamp(c, 0.0f, 100.0f);
    for (float& c : p.endColor)
        c = std::clamp(c, 0.0f, 100.0f);
}

} // namespace ixparticle
