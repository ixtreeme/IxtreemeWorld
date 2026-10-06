// ParticlesGpu.hlsl — the draw pass of the GPU-simulated particles. Same visuals as Particles.hlsl,
// but the instance buffer is the compute's 96-byte ParticleState buffer (the first four float4s are
// the draw fields). The engine draws a fixed instance count (maxParticles); dead particles have
// size 0 and rasterize nothing.

#pragma pack_matrix(row_major)

struct ParticleState
{
    float4 positionSize;  // xyz = position, w = size (0 = dead)
    float4 rotation;      // x = rotation radians
    float4 color;         // straight rgba
    float4 uvRect;        // atlas cell
    float4 velocityAge;   // (simulation only)
    float4 lifeSpawn;     // (simulation only)
};

[[vk::binding(0, 0)]] cbuffer ParticleView : register(b0)
{
    float4x4 u_viewProj;
    float4 u_cameraRight;
    float4 u_cameraUp;
    float4 u_particleParams;  // x soft enabled, y soft distance, z near, w far
};

[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] Texture2D u_texture : register(t0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState u_sampler : register(s0);

[[vk::binding(2, 0)]] StructuredBuffer<ParticleState> u_particles : register(t1);

[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] Texture2D<float> u_sceneDepth : register(t2);
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] SamplerState u_sceneDepthSampler : register(s2);

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float4 clipPos : TEXCOORD1;
};

VSOutput VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    ParticleState particle = u_particles[instanceId];

    const float2 corners[6] =
    {
        float2(-1.0, -1.0), float2(1.0, -1.0), float2(1.0, 1.0),
        float2(-1.0, -1.0), float2(1.0, 1.0), float2(-1.0, 1.0)
    };
    float2 corner = corners[vertexId];

    float s, c;
    sincos(particle.rotation.x, s, c);
    const float2 rotated = float2(corner.x * c - corner.y * s, corner.x * s + corner.y * c);

    const float3 world = particle.positionSize.xyz +
        (u_cameraRight.xyz * rotated.x + u_cameraUp.xyz * rotated.y) * (particle.positionSize.w * 0.5);

    VSOutput output;
    output.clipPos = mul(float4(world, 1.0), u_viewProj);
    output.position = output.clipPos;
    output.uv = particle.uvRect.xy + (corner * 0.5 + 0.5) * particle.uvRect.zw;
    output.color = particle.color;
    return output;
}

float LinearizeDepth(float ndcDepth)
{
    const float nearPlane = max(u_particleParams.z, 0.0001);
    const float farPlane = max(u_particleParams.w, nearPlane + 0.001);
    const float a = farPlane / (farPlane - nearPlane);
    const float b = (nearPlane * farPlane) / (farPlane - nearPlane);
    return b / max(a - saturate(ndcDepth), 0.000001);
}

float4 PSMain(VSOutput input) : SV_Target0
{
    const float4 tex = u_texture.Sample(u_sampler, input.uv);
    float alpha = tex.a * input.color.a;

    if (u_particleParams.x > 0.5)
    {
        const float2 screenUv = (input.clipPos.xy / max(input.clipPos.w, 0.0001)) * 0.5 + 0.5;
        const float selfNdc = saturate(input.clipPos.z / max(input.clipPos.w, 0.0001));
        const float sceneNdc = u_sceneDepth.Sample(u_sceneDepthSampler, saturate(screenUv)).r;
        const float viewDepth = max(0.0, LinearizeDepth(sceneNdc) - LinearizeDepth(selfNdc));
        alpha *= saturate(viewDepth / max(u_particleParams.y, 0.001));
    }

    // Premultiplied, like the CPU path.
    return float4(tex.rgb * input.color.rgb * alpha, alpha);
}
