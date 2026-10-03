#pragma pack_matrix(row_major)

// Volumetric light: the sunlight the air scatters towards the camera along each view ray. The sun
// shadow cascades decide which parts of the ray the sun reaches, so shadowed air stays dark and lit
// air between the shadows shows as shafts — seen from the side too, wherever the sun is. Rendered at
// half size; a per-pixel offset of the march steps turns banding into fine noise that the blur pass
// (GodRays.hlsl VolBlurPS) removes. The vertex stage is GodRays.hlsl VSMain (same interface).

[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2D u_depth : register(t0);
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState u_depthSampler : register(s0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] Texture2DArray<float> u_shadow : register(t1);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerComparisonState u_shadowSampler : register(s1);

[[vk::binding(2, 0)]] cbuffer ShadowCascades : register(b0)
{
    float4x4 u_cascadeViewProj[4];  // light view-projection per cascade, finest first
};

struct VolumetricPush
{
    float4x4 invViewProjection;  // screen position + depth -> world
    float4 cameraPos;            // xyz; w = how far along the ray light is gathered (m)
    float4 sunDir;               // xyz towards the sun; w = scattering per metre
    float4 sunColor;             // rgb linear (colour x intensity); w = forward-scattering g
    float4 params;               // x = steps; y = shadow depth bias
};
[[vk::push_constant]] VolumetricPush u_push;

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float2 ndc : TEXCOORD1;
};

// 1 where the sun reaches a point, 0 in its shadow, from the point's position in each cascade's
// light space (x/y: shadow map uv, z: depth); the finest cascade that contains it decides.
float SunVisibility(float3 lightSpace[4])
{
    [unroll]
    for (int cascade = 0; cascade < 4; ++cascade)
    {
        const float3 p = lightSpace[cascade];
        if (all(p > 0.0) && all(p < 1.0))
            return u_shadow.SampleCmpLevelZero(u_shadowSampler, float3(p.xy, (float)cascade), p.z - u_push.params.y);
    }
    return 1.0;  // past the last cascade there is no shadow information: the air there is lit
}

// A world position in a cascade's light space as (shadow map uv, depth). The cascades' projections
// are orthographic, so this is affine: along a ray it changes by the same step each march step.
float3 CascadeLightSpace(float3 p, int cascade)
{
    const float3 ndc = mul(float4(p, 1.0), u_cascadeViewProj[cascade]).xyz;
    return float3(ndc.xy * 0.5 + 0.5, ndc.z);
}

// Henyey-Greenstein phase, scaled so that even scattering (g = 0) is 1 in every direction.
float Phase(float cosTheta, float g)
{
    const float g2 = g * g;
    return (1.0 - g2) / pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4), 1.5);
}

float InterleavedGradientNoise(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

float4 MarchPS(VSOutput input) : SV_Target
{
    const float depth = u_depth.SampleLevel(u_depthSampler, input.uv, 0.0).r;
    const float3 camera = u_push.cameraPos.xyz;
    // The point the pixel shows (the far plane for the sky).
    const float4 hit = mul(float4(input.ndc, depth >= 0.99999 ? 1.0 : depth, 1.0), u_push.invViewProjection);
    const float3 ray = hit.xyz / hit.w - camera;
    const float distance = length(ray);
    const float3 dir = ray / max(distance, 1e-4);

    const float rayLength = min(distance, u_push.cameraPos.w);
    // Steps of (at most) the same length on every ray: a ray that ends on nearby ground takes a few
    // of them, not the full count a ray into the sky needs.
    const int maxSteps = clamp((int)(u_push.params.x + 0.5), 4, 128);
    const float maxStepLength = u_push.cameraPos.w / maxSteps;
    const int steps = clamp((int)ceil(rayLength / maxStepLength), 4, maxSteps);
    const float stepLength = rayLength / steps;
    const float stepTransmittance = exp(-u_push.sunDir.w * stepLength);
    const float t0 = stepLength * InterleavedGradientNoise(input.position.xy);
    // The march in each cascade's light space: a start and a per-step offset instead of four
    // matrix transforms per step.
    float3 lightSpace[4];
    float3 lightSpaceStep[4];
    [unroll]
    for (int cascade = 0; cascade < 4; ++cascade)
    {
        lightSpace[cascade] = CascadeLightSpace(camera + dir * t0, cascade);
        lightSpaceStep[cascade] = CascadeLightSpace(camera + dir * (t0 + stepLength), cascade) - lightSpace[cascade];
    }
    float transmittance = 1.0;
    float lit = 0.0;
    for (int i = 0; i < steps; ++i)
    {
        lit += SunVisibility(lightSpace) * transmittance;
        transmittance *= stepTransmittance;
        [unroll]
        for (int c = 0; c < 4; ++c)
            lightSpace[c] += lightSpaceStep[c];
    }
    // Each lit step scatters (1 - its transmittance) of the sunlight towards the camera. The 0.25 keeps
    // the haze under the brightness of the lit scene (the image is not HDR).
    const float scattered = lit * (1.0 - stepTransmittance);
    const float phase = Phase(dot(dir, normalize(u_push.sunDir.xyz)), u_push.sunColor.w);
    return float4(u_push.sunColor.rgb * scattered * phase * 0.25, 1.0);
}
