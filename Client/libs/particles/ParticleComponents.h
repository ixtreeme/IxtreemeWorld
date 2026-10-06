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

// Where new particles are born around the emitter origin (all offsets in the emitter's local axes).
enum class ParticleShape : std::uint8_t
{
    Point = 0,   // exactly at the origin
    Sphere,      // inside a sphere of shapeRadius
    Box,         // inside a box of shapeExtents half-extents
    Circle,      // inside a disc of shapeRadius in the local XZ plane (shapeArc degrees)
    Edge         // along the local X axis, +/- shapeExtents[0]
};

inline const char* ShapeName(ParticleShape shape)
{
    switch (shape)
    {
    case ParticleShape::Point: return "Point";
    case ParticleShape::Sphere: return "Sphere";
    case ParticleShape::Box: return "Box";
    case ParticleShape::Circle: return "Circle";
    case ParticleShape::Edge: return "Edge";
    }
    return "Sphere";
}

inline ParticleShape ParseShape(const std::string& s)
{
    if (s == "Point") return ParticleShape::Point;
    if (s == "Box") return ParticleShape::Box;
    if (s == "Circle") return ParticleShape::Circle;
    if (s == "Edge") return ParticleShape::Edge;
    return ParticleShape::Sphere;
}

// One emitter on one entity. Emission is a cone around `direction` (rotated by the entity's
// rotation), spawned in a sphere of `shapeRadius` at the entity origin. Per particle the sim
// interpolates size and color from the Start* values to the End* values over its lifetime.
struct ParticleSystemComponent
{
    std::string textureAssetId;  // AssetLibrary Texture id ("" = soft white round default)
    // The .particle preset this component's parameters were applied from ("" = authored inline).
    // A record for the inspector's picker; the parameters above stay the runtime source of truth.
    std::string effectAssetId;
    bool enabled = true;
    bool playOnStart = true;     // begin emitting when Play starts
    bool loop = true;            // false: emission stops after `duration`, live particles finish
    // GPU simulation: a compute pass simulates this emitter's particles (for high counts). The GPU
    // path has no ground collision, no local space and no duration/loop handling (continuous only).
    bool gpuSimulation = false;
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
    // Size over lifetime: a piecewise-linear multiplier curve sampled at t = 0, 1/3, 2/3, 1.
    float sizeOverLife[4] = {1.0f, 1.0f, 1.0f, 0.2f};

    float direction[3] = {0.0f, 1.0f, 0.0f};  // local emission axis
    float coneAngle = 20.0f;     // half-angle of the emission cone, degrees
    float shapeRadius = 0.15f;   // spawn sphere radius around the emitter origin

    // Emission shape (spawn volume) and simulation space.
    ParticleShape shape = ParticleShape::Sphere;
    float shapeExtents[3] = {0.5f, 0.1f, 0.5f};  // Box half-extents / Edge half-length (x)
    float shapeArc = 360.0f;                     // Circle arc, degrees (centred on local +X)
    // Local space: particles are simulated in the emitter's local axes and follow it as it moves and
    // rotates (smoke on a vehicle, an aura on a character). World space (default): they stay where
    // they spawned.
    bool localSpace = false;

    float gravity = -1.5f;       // m/s^2 along world Y
    float drag = 0.0f;           // velocity damping per second (0 = none)
    float rotationSpeed = 0.0f;  // deg/s; each particle gets a random +/- spin

    // Soft particles: fade the sprite out as it approaches scene geometry (sampling the scene depth
    // snapshot), so the plume does not cut hard against the terrain/walls. `softDistance` is the
    // world-space distance over which the fade happens.
    bool softParticles = true;
    float softDistance = 0.5f;

    // Flipbook: the texture is an atlas of atlasColumns x atlasRows cells, played over each
    // particle's lifetime (1x1 = a single sprite, the default).
    int atlasColumns = 1;
    int atlasRows = 1;

    // Ground collision (world space): particles bounce against the terrain height (or the fallback
    // plane when the scene has no terrain). Local-space emitters do not collide.
    bool collideWithGround = false;
    float collisionBounce = 0.3f;    // 0 = stick, 1 = perfect bounce
    float collisionFriction = 0.5f;  // horizontal velocity kept on impact
    float groundPlaneY = 0.0f;       // the fallback ground height without terrain

    // Colour gradient over lifetime: RGBA keys at t = 0, 1/3, 2/3, 1 (flattened: key i at
    // colorOverLife[i * 4 .. i * 4 + 3]).
    float colorOverLife[16] = {
        1.0f, 0.85f, 0.5f, 1.0f,
        1.0f, 0.60f, 0.3f, 0.8f,
        1.0f, 0.40f, 0.2f, 0.4f,
        1.0f, 0.25f, 0.1f, 0.0f};
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
    for (float& key : p.sizeOverLife)
        key = std::clamp(key, 0.0f, 100.0f);
    p.coneAngle = std::clamp(p.coneAngle, 0.0f, 180.0f);
    p.shapeRadius = std::clamp(p.shapeRadius, 0.0f, 1000.0f);
    for (float& e : p.shapeExtents)
        e = std::clamp(e, 0.0f, 1000.0f);
    p.shapeArc = std::clamp(p.shapeArc, 1.0f, 360.0f);
    p.gravity = std::clamp(p.gravity, -500.0f, 500.0f);
    p.drag = std::clamp(p.drag, 0.0f, 100.0f);
    p.rotationSpeed = std::clamp(p.rotationSpeed, -3600.0f, 3600.0f);
    p.softDistance = std::clamp(p.softDistance, 0.01f, 100.0f);
    p.atlasColumns = std::clamp(p.atlasColumns, 1, 64);
    p.atlasRows = std::clamp(p.atlasRows, 1, 64);
    p.collisionBounce = std::clamp(p.collisionBounce, 0.0f, 1.0f);
    p.collisionFriction = std::clamp(p.collisionFriction, 0.0f, 1.0f);
    p.groundPlaneY = std::clamp(p.groundPlaneY, -10000.0f, 10000.0f);
    for (auto& key : p.colorOverLife)
        key = std::clamp(key, 0.0f, 100.0f);
}

} // namespace ixparticle
