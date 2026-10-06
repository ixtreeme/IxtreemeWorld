// Particles.hlsl — instanced camera-facing particle quads (CPU-simulated particles).
//
// One draw per emitter batch. The quad corners come from SV_VertexID (no vertex buffer); the
// per-particle transform/color comes from a StructuredBuffer indexed by SV_InstanceID (the same
// pattern StaticMesh.hlsl uses for per-instance data). The pixel shader outputs PREMULTIPLIED
// alpha so one shader serves both blend modes: alpha = One / OneMinusSrcAlpha, additive = One / One.
//
// Soft particles (u_particleParams.x > 0.5) fade the sprite where it comes close to scene geometry,
// sampling the scene depth snapshot: the same depth linearization and screen-UV math as Water.hlsl.
//
// Row-major matrices in the cbuffer, like WorldLabel.hlsl: the engine passes row-major matrices and
// transforms with mul(rowVector, matrix).

#pragma pack_matrix(row_major)

// All members are float4 so the structured-buffer element layout is 64 bytes under every packing
// rule (DX layout and Vulkan std430 alike) — a float3 member would be 16-byte aligned and make the
// stride larger, silently mismatching the C++ InstanceData.
struct ParticleInstanceData
{
    float4 positionSize;  // xyz = world position, w = size (m)
    float4 rotation;      // x = rotation in radians, yzw unused
    float4 color;         // straight rgba
    float4 uvRect;        // flipbook atlas cell: xy = offset, zw = size
};

[[vk::binding(0, 0)]] cbuffer ParticleView : register(b0)
{
    float4x4 u_viewProj;
    float4 u_cameraRight;     // xyz = the billboard's right axis (world)
    float4 u_cameraUp;        // xyz = the billboard's up axis (world)
    float4 u_particleParams;  // x = soft enabled, y = soft distance, z = near plane, w = far plane
};

[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] Texture2D u_texture : register(t0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState u_sampler : register(s0);

[[vk::binding(2, 0)]] StructuredBuffer<ParticleInstanceData> u_particles : register(t1);

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
    ParticleInstanceData particle = u_particles[instanceId];

    // Unit quad as two triangles (corners in [-1, 1]).
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
    // The flipbook atlas cell for this particle's current frame.
    output.uv = particle.uvRect.xy + (corner * 0.5 + 0.5) * particle.uvRect.zw;
    output.color = particle.color;
    return output;
}

// Standard [0, 1] depth (near = 0, far = 1): the same curve Water.hlsl linearizes.
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
        // The scene's view depth vs this fragment's: fade out as the gap shrinks.
        const float2 screenUv = (input.clipPos.xy / max(input.clipPos.w, 0.0001)) * 0.5 + 0.5;
        const float selfNdc = saturate(input.clipPos.z / max(input.clipPos.w, 0.0001));
        const float sceneNdc = u_sceneDepth.Sample(u_sceneDepthSampler, saturate(screenUv)).r;
        const float viewDepth = max(0.0, LinearizeDepth(sceneNdc) - LinearizeDepth(selfNdc));
        alpha *= saturate(viewDepth / max(u_particleParams.y, 0.001));
    }

    // Premultiplied: rgb is already multiplied by its own alpha (see the file header).
    return float4(tex.rgb * input.color.rgb * alpha, alpha);
}
