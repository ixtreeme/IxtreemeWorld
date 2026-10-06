// ParticlesSim.hlsl — the GPU particle simulation: one compute dispatch per emitter per frame.
//
// The state buffer doubles as the draw instance buffer: its first four float4s are exactly what
// ParticlesGpu.hlsl reads, and dead particles get size 0 so a FIXED instance count (maxParticles)
// can draw the whole buffer — no indirect draw needed. Spawning is atomic-free: the CPU passes a
// spawn budget and a rotating cursor, and the threads inside that ring range respawn their slot.
//
// All members are float4 (96-byte stride under every packing rule).

#pragma pack_matrix(row_major)

struct ParticleState
{
    float4 positionSize;  // xyz = position (world), w = size (0 = dead)
    float4 rotation;      // x = rotation radians
    float4 color;         // straight rgba
    float4 uvRect;        // atlas cell: xy = offset, zw = size
    float4 velocityAge;   // xyz = velocity, w = age
    float4 lifeSpawn;     // x = lifetime, y = spawn size, z = angular velocity, w unused
};

[[vk::binding(0, 0)]] cbuffer SimParams : register(b0)
{
    float4 u_emitterPos;    // xyz = emitter position, w = dt
    float4 u_emitterDir;    // xyz = normalized emission axis
    float4 u_params0;       // x gravity, y drag, z cone angle (rad), w rotation speed (rad)
    float4 u_params1;       // x shape radius, y shape arc (rad), z shape, w unused
    float4 u_shapeExtents;  // xyz = box/edge extents
    float4 u_spawn;         // x spawn budget, y spawn cursor, z frame seed, w max particles
    float4 u_sizeOverLife;  // 4 keys
    float4 u_color0;
    float4 u_color1;
    float4 u_color2;
    float4 u_color3;
    float4 u_life;          // x lifetime min, y lifetime max, z size min, w size max
    float4 u_speed;         // x speed min, y speed max, z atlas columns, w atlas rows
};

[[vk::binding(1, 0)]] RWStructuredBuffer<ParticleState> u_particles : register(u0);

float Hash01(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return float(x & 0x00FFFFFFu) / float(0x01000000u);
}

float LerpF(float a, float b, float t)
{
    return a + (b - a) * t;
}

float SampleCurve4(float4 keys, float t)
{
    const float scaled = saturate(t) * 3.0;
    const int index = min(int(scaled), 2);
    const float f = scaled - float(index);
    return LerpF(keys[index], keys[index + 1], f);
}

float4 SampleGradient(float4 c0, float4 c1, float4 c2, float4 c3, float t)
{
    const float scaled = saturate(t) * 3.0;
    const int index = min(int(scaled), 2);
    const float f = scaled - float(index);
    float4 a = c0;
    float4 b = c1;
    if (index == 1)
    {
        a = c1;
        b = c2;
    }
    else if (index == 2)
    {
        a = c2;
        b = c3;
    }
    return lerp(a, b, f);
}

void BuildBasis(float3 direction, out float3 tangent, out float3 bitangent)
{
    float3 up = float3(0.0, 1.0, 0.0);
    if (abs(direction.y) > 0.999)
        up = float3(1.0, 0.0, 0.0);
    tangent = normalize(cross(up, direction));
    bitangent = cross(direction, tangent);
}

// Respawns `slot` as a fresh particle. `salt` decorrelates the per-slot RNG streams.
void Spawn(uint slot, uint salt)
{
    const uint seed = uint(u_spawn.z) * 747796405u + slot * 2891336453u + salt;
    const float r0 = Hash01(seed);
    const float r1 = Hash01(seed ^ 0x68bc21ebu);
    const float r2 = Hash01(seed ^ 0x02e5be93u);
    const float r3 = Hash01(seed ^ 0x967a889bu);
    const float r4 = Hash01(seed ^ 0x3d5c4b2fu);
    const float r5 = Hash01(seed ^ 0x8a5f1cd1u);

    ParticleState p;
    const float3 emitterPosition = u_emitterPos.xyz;

    // Spawn volume (same shapes as the CPU simulator).
    const uint shape = uint(u_params1.z + 0.5);
    float3 offset = float3(0.0, 0.0, 0.0);
    if (shape == 1)  // sphere
    {
        const float3 direction = float3(r0 * 2.0 - 1.0, r1 * 2.0 - 1.0, r2 * 2.0 - 1.0);
        offset = direction / max(length(direction), 1e-5) * (u_params1.x * pow(r3, 1.0 / 3.0));
    }
    else if (shape == 2)  // box
    {
        offset = float3((r0 * 2.0 - 1.0) * u_shapeExtents.x,
                        (r1 * 2.0 - 1.0) * u_shapeExtents.y,
                        (r2 * 2.0 - 1.0) * u_shapeExtents.z);
    }
    else if (shape == 3)  // circle (local XZ)
    {
        const float angle = (r0 * 2.0 - 1.0) * u_params1.y;
        const float radius = u_params1.x * sqrt(r1);
        offset = float3(cos(angle) * radius, 0.0, sin(angle) * radius);
    }
    else if (shape == 4)  // edge (local X)
    {
        offset = float3((r0 * 2.0 - 1.0) * u_shapeExtents.x, 0.0, 0.0);
    }
    p.positionSize.xyz = emitterPosition + offset;

    // Random direction inside the cone around the emission axis.
    float3 direction = normalize(max(u_emitterDir.xyz, 1e-5));
    float3 tangent;
    float3 bitangent;
    BuildBasis(direction, tangent, bitangent);
    const float cosMax = cos(u_params0.z);
    const float cosTheta = 1.0 - r2 * (1.0 - cosMax);
    const float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    const float phi = r3 * 6.28318530718;
    const float3 coneDirection = direction * cosTheta +
        (tangent * cos(phi) + bitangent * sin(phi)) * sinTheta;

    const float speed = LerpF(u_speed.x, u_speed.y, r4);
    const float lifetime = LerpF(u_life.x, u_life.y, r5);
    const float spawnSize = LerpF(u_life.z, u_life.w, Hash01(seed ^ 0x51ed270bu));
    const float angularVelocity = (r1 * 2.0 - 1.0) * u_params0.w;
    p.positionSize = float4(emitterPosition + offset, spawnSize);
    p.velocityAge = float4(coneDirection * speed, 0.0);
    p.rotation = float4(r0 * 6.28318530718, 0.0, 0.0, 0.0);
    p.lifeSpawn = float4(lifetime, spawnSize, angularVelocity, 0.0);
    p.color = u_color0;
    p.uvRect = float4(0.0, 0.0, 1.0 / max(u_speed.z, 1.0), 1.0 / max(u_speed.w, 1.0));
    u_particles[slot] = p;
}

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint slot = id.x;
    const uint maxParticles = uint(u_spawn.w);
    if (slot >= maxParticles)
        return;

    // Spawning: the slots inside [cursor, cursor + budget) of the ring get respawned.
    const uint budget = min(uint(u_spawn.x), maxParticles);
    const uint cursor = uint(u_spawn.y);
    if (budget > 0 && ((slot + maxParticles - cursor) % maxParticles) < budget)
    {
        Spawn(slot, 0u);
        return;
    }

    ParticleState p = u_particles[slot];
    if (p.lifeSpawn.x <= 0.0 || p.velocityAge.w >= p.lifeSpawn.x)
    {
        p.positionSize.w = 0.0;  // dead: degenerate (no pixels)
        u_particles[slot] = p;
        return;
    }

    const float dt = u_emitterPos.w;
    p.velocityAge.w += dt;
    if (p.velocityAge.w >= p.lifeSpawn.x)
    {
        p.positionSize.w = 0.0;
        u_particles[slot] = p;
        return;
    }
    const float t = saturate(p.velocityAge.w / max(p.lifeSpawn.x, 1e-4));

    p.velocityAge.y += u_params0.x * dt;  // gravity (world Y)
    if (u_params0.y > 0.0)
    {
        const float damping = max(0.0, 1.0 - u_params0.y * dt);
        p.velocityAge.xyz *= damping;
    }
    p.positionSize.xyz += p.velocityAge.xyz * dt;

    p.positionSize.w = p.lifeSpawn.y * SampleCurve4(u_sizeOverLife, t);
    p.color = SampleGradient(u_color0, u_color1, u_color2, u_color3, t);
    p.rotation.x += p.lifeSpawn.z * dt;

    // Flipbook cell over the lifetime (clamped, like the CPU path).
    const float columns = max(u_speed.z, 1.0);
    const float rows = max(u_speed.w, 1.0);
    const uint frames = uint(columns * rows);
    const uint frame = frames > 1 ? min(uint(t * float(frames)), frames - 1) : 0;
    p.uvRect = float4(float(frame % uint(columns)) / columns, float(frame / uint(columns)) / rows,
        1.0 / columns, 1.0 / rows);

    u_particles[slot] = p;
}
