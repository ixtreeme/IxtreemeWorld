#include "ParticleSimulator.h"

#include <algorithm>
#include <cmath>

namespace ixparticle
{
namespace
{

constexpr int kMaxSpawnsPerFrame = 64;
constexpr float kMaxStepSeconds = 0.1f;  // a stall must not explode the simulation

float Lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

// Orthonormal basis around `direction` (which must be normalized): `tangent`, `bitangent`.
void BuildBasis(const float direction[3], float tangent[3], float bitangent[3])
{
    float up[3] = {0.0f, 1.0f, 0.0f};
    if (std::fabs(direction[1]) > 0.999f)
    {
        up[0] = 1.0f;
        up[1] = 0.0f;
        up[2] = 0.0f;
    }
    // tangent = normalize(cross(up, direction))
    tangent[0] = up[1] * direction[2] - up[2] * direction[1];
    tangent[1] = up[2] * direction[0] - up[0] * direction[2];
    tangent[2] = up[0] * direction[1] - up[1] * direction[0];
    const float tangentLength = std::sqrt(tangent[0] * tangent[0] + tangent[1] * tangent[1] + tangent[2] * tangent[2]);
    if (tangentLength > 1e-6f)
    {
        tangent[0] /= tangentLength;
        tangent[1] /= tangentLength;
        tangent[2] /= tangentLength;
    }
    else
    {
        tangent[0] = 1.0f;
        tangent[1] = 0.0f;
        tangent[2] = 0.0f;
    }
    // bitangent = cross(direction, tangent)
    bitangent[0] = direction[1] * tangent[2] - direction[2] * tangent[1];
    bitangent[1] = direction[2] * tangent[0] - direction[0] * tangent[2];
    bitangent[2] = direction[0] * tangent[1] - direction[1] * tangent[0];
}

} // namespace

float ParticleSimulator::Random01()
{
    // xorshift32: deterministic, cheap, good enough for particle jitter.
    m_rng ^= m_rng << 13;
    m_rng ^= m_rng >> 17;
    m_rng ^= m_rng << 5;
    return static_cast<float>(m_rng & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
}

void ParticleSimulator::Reset(const ParticleSystemComponent& component, std::uint32_t seed, bool startEmitting)
{
    m_particles.clear();
    m_rng = seed != 0u ? seed : 1u;
    m_emissionTime = 0.0f;
    m_emitAccumulator = 0.0f;
    m_pendingBurst = startEmitting ? std::clamp(component.burstCount, 0, 10000) : 0;
    m_emitting = startEmitting;
    m_particles.reserve(static_cast<std::size_t>(std::max(component.maxParticles, 1)));
}

void ParticleSimulator::SpawnParticle(const ParticleSystemComponent& component,
                                      const float emitterPosition[3],
                                      const float direction[3],
                                      const float tangent[3],
                                      const float bitangent[3])
{
    Particle particle;

    // Spawn in a sphere around the emitter origin.
    float offsetDirection[3] = {Random01() * 2.0f - 1.0f, Random01() * 2.0f - 1.0f, Random01() * 2.0f - 1.0f};
    const float offsetLengthSq = offsetDirection[0] * offsetDirection[0] + offsetDirection[1] * offsetDirection[1] +
        offsetDirection[2] * offsetDirection[2];
    const float offsetLength = std::sqrt(std::max(offsetLengthSq, 1e-6f));
    const float offsetRadius = component.shapeRadius * std::cbrt(Random01());
    particle.position[0] = emitterPosition[0] + offsetDirection[0] / offsetLength * offsetRadius;
    particle.position[1] = emitterPosition[1] + offsetDirection[1] / offsetLength * offsetRadius;
    particle.position[2] = emitterPosition[2] + offsetDirection[2] / offsetLength * offsetRadius;

    // A random direction inside the cone around `direction`.
    const float cosMax = std::cos(component.coneAngle * 0.01745329252f);
    const float cosTheta = 1.0f - Random01() * (1.0f - cosMax);
    const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
    const float phi = Random01() * 6.28318530718f;
    const float coneDirection[3] = {
        direction[0] * cosTheta + (tangent[0] * std::cos(phi) + bitangent[0] * std::sin(phi)) * sinTheta,
        direction[1] * cosTheta + (tangent[1] * std::cos(phi) + bitangent[1] * std::sin(phi)) * sinTheta,
        direction[2] * cosTheta + (tangent[2] * std::cos(phi) + bitangent[2] * std::sin(phi)) * sinTheta};

    const float speed = Lerp(component.startSpeedMin, component.startSpeedMax, Random01());
    particle.velocity[0] = coneDirection[0] * speed;
    particle.velocity[1] = coneDirection[1] * speed;
    particle.velocity[2] = coneDirection[2] * speed;

    particle.age = 0.0f;
    particle.lifetime = Lerp(component.startLifetimeMin, component.startLifetimeMax, Random01());
    particle.spawnSize = Lerp(component.startSizeMin, component.startSizeMax, Random01());
    particle.size = particle.spawnSize;
    particle.rotation = Random01() * 6.28318530718f;
    particle.angularVelocity = (Random01() * 2.0f - 1.0f) * component.rotationSpeed * 0.01745329252f;
    for (int c = 0; c < 4; ++c)
        particle.color[c] = component.startColor[c];

    m_particles.push_back(particle);
}

void ParticleSimulator::Update(const ParticleSystemComponent& component,
                               const float emitterPosition[3],
                               const float emitterDirection[3],
                               float dtSeconds)
{
    const float dt = std::clamp(dtSeconds, 0.0f, kMaxStepSeconds);

    // Integrate + retire the dead (swap-and-pop keeps the array compact for the renderer).
    for (std::size_t i = 0; i < m_particles.size();)
    {
        Particle& particle = m_particles[i];
        particle.age += dt;
        if (particle.age >= particle.lifetime)
        {
            particle = m_particles.back();
            m_particles.pop_back();
            continue;
        }
        const float t = std::clamp(particle.age / std::max(particle.lifetime, 1e-4f), 0.0f, 1.0f);
        particle.velocity[1] += component.gravity * dt;
        if (component.drag > 0.0f)
        {
            const float damping = std::max(0.0f, 1.0f - component.drag * dt);
            particle.velocity[0] *= damping;
            particle.velocity[1] *= damping;
            particle.velocity[2] *= damping;
        }
        particle.position[0] += particle.velocity[0] * dt;
        particle.position[1] += particle.velocity[1] * dt;
        particle.position[2] += particle.velocity[2] * dt;
        particle.size = Lerp(particle.spawnSize, particle.spawnSize * component.endSizeScale, t);
        for (int c = 0; c < 4; ++c)
            particle.color[c] = Lerp(component.startColor[c], component.endColor[c], t);
        particle.rotation += particle.angularVelocity * dt;
        ++i;
    }

    // Emit.
    if (dt > 0.0f)
    {
        float direction[3] = {emitterDirection[0], emitterDirection[1], emitterDirection[2]};
        const float directionLength = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] +
            direction[2] * direction[2]);
        if (directionLength > 1e-6f)
        {
            direction[0] /= directionLength;
            direction[1] /= directionLength;
            direction[2] /= directionLength;
        }
        else
        {
            direction[0] = 0.0f;
            direction[1] = 1.0f;
            direction[2] = 0.0f;
        }
        float tangent[3];
        float bitangent[3];
        BuildBasis(direction, tangent, bitangent);

        int spawnBudget = kMaxSpawnsPerFrame;

        // Queued bursts (the component's own, or a script's ParticleEmit) fire even while emission
        // is stopped — ParticleEmit must work on a silent emitter.
        if (m_pendingBurst > 0)
        {
            const int burst = std::min(m_pendingBurst, spawnBudget);
            for (int i = 0; i < burst; ++i)
                SpawnParticle(component, emitterPosition, direction, tangent, bitangent);
            m_pendingBurst -= burst;
            spawnBudget -= burst;
        }

        if (m_emitting)
        {
            const float duration = std::max(component.duration, 0.05f);
            m_emissionTime += dt;

            m_emitAccumulator += component.emissionRate * dt;
            int rateSpawns = static_cast<int>(m_emitAccumulator);
            m_emitAccumulator -= static_cast<float>(rateSpawns);
            rateSpawns = std::min(rateSpawns, spawnBudget);
            for (int i = 0; i < rateSpawns; ++i)
                SpawnParticle(component, emitterPosition, direction, tangent, bitangent);

            if (m_emissionTime >= duration)
            {
                if (component.loop)
                {
                    m_emissionTime -= duration;
                    if (component.burstCount > 0 && m_pendingBurst == 0)
                        m_pendingBurst = component.burstCount;
                }
                else
                {
                    m_emitting = false;
                }
            }
        }
    }

    // Hard cap: drop the oldest particles first (they are the closest to dying anyway).
    if (m_particles.size() > static_cast<std::size_t>(component.maxParticles))
        m_particles.erase(m_particles.begin(),
            m_particles.begin() + static_cast<std::ptrdiff_t>(m_particles.size() - component.maxParticles));
}

} // namespace ixparticle
