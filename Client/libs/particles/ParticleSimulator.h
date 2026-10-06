#pragma once

// CPU particle simulator: one instance per emitting entity, stepped from the Play loop with the
// frame's dt. Deterministic for a given seed (the entity id), world-space (particles keep their
// spawn position; the emitter may move without dragging them along), fixed max count, no
// allocations after the first frames. The renderer consumes Particles() as-is — no engine or RHI
// dependency in this module.

#include "ParticleComponents.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ixparticle
{

struct Particle
{
    float position[3] = {0.0f, 0.0f, 0.0f};
    float velocity[3] = {0.0f, 0.0f, 0.0f};
    float age = 0.0f;           // seconds lived
    float lifetime = 1.0f;      // total seconds (age >= lifetime = dead)
    float spawnSize = 1.0f;     // the size this particle was born with (the end scale multiplies it)
    float size = 1.0f;          // current world size (m)
    float rotation = 0.0f;      // radians
    float angularVelocity = 0.0f;
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};  // current color (start -> end lerp)
};

// Ground height query for collision: returns the world Y at (x, z). The engine wires the terrain
// (or a plane) here; a null query disables collision for the call.
using GroundHeightFn = float (*)(void* user, float x, float z);

class ParticleSimulator
{
public:
    // (Re)starts emission for an emitter: clears the live particles, applies the component's burst
    // and (when `startEmitting`) begins rate emission. `seed` makes the run reproducible (use the
    // entity id). Pass startEmitting=false for a silent simulator that only emits via QueueBurst.
    void Reset(const ParticleSystemComponent& component, std::uint32_t seed, bool startEmitting = true);

    // Begins/resumes rate emission (the burst from Reset already fired, if any).
    void Play() { m_emitting = true; }
    // Stops emitting; the live particles keep going until their lifetimes end.
    void Stop() { m_emitting = false; }
    // Spawns `count` particles on the next Update (respects maxParticles; script ParticleEmit).
    void QueueBurst(std::uint32_t count)
    {
        m_pendingBurst += static_cast<int>(std::min<std::uint32_t>(count, 10000u));
    }

    // Steps one frame. `emitterPosition` is the entity's world position; `emitterDirection` is the
    // component's direction rotated by the entity's rotation (normalized by the caller or here).
    // `groundHeight`/`groundUser`: the optional collision query (world-space emitters only).
    void Update(const ParticleSystemComponent& component,
                const float emitterPosition[3],
                const float emitterDirection[3],
                float dtSeconds,
                GroundHeightFn groundHeight = nullptr,
                void* groundUser = nullptr);

    void Clear() { m_particles.clear(); }

    const std::vector<Particle>& Particles() const { return m_particles; }
    std::size_t AliveCount() const { return m_particles.size(); }
    bool Emitting() const { return m_emitting; }

private:
    float Random01();
    void SpawnParticle(const ParticleSystemComponent& component,
                       const float emitterPosition[3],
                       const float direction[3],
                       const float tangent[3],
                       const float bitangent[3]);

    std::vector<Particle> m_particles;
    std::uint32_t m_rng = 1u;
    float m_emissionTime = 0.0f;
    float m_emitAccumulator = 0.0f;
    int m_pendingBurst = 0;
    bool m_emitting = false;
};

} // namespace ixparticle
