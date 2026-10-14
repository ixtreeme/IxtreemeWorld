#pragma pack_matrix(row_major)

// The scene sky: one full-screen triangle at the far plane. Depth test LessOrEqual without depth
// writes, so it fills exactly the pixels no geometry covered — whether it is drawn first (it then
// becomes the background) or after the geometry (the water reflection pass).

#include "SkyFunctions.hlsli"

[[vk::binding(0, 0)]] cbuffer SkyConstants : register(b0)
{
    float4 u_zenith;    // rgb linear; w = mode (0 colour, 1 procedural, 2 cubemap, 3 panorama)
    float4 u_horizon;   // rgb linear; w = exposure
    float4 u_ground;    // rgb linear; w = rotation (radians, around +Y)
    float4 u_sunDir;    // xyz towards the sun; w = cos(sun disc radius), > 1 hides the disc
    float4 u_sunColor;  // rgb linear (colour x intensity, at most the colour); w = glow strength
    float4 u_tint;      // rgb tint; w = 1 when the sun light is on
    float4 u_color;     // rgb linear flat colour (mode 0)
};

[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] TextureCube u_cube : register(t0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState u_cubeSampler : register(s0);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] Texture2D u_panorama : register(t1);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] SamplerState u_panoramaSampler : register(s1);

// Per view: the inverse view-projection turns a screen position into a world-space view ray.
struct SkyPush
{
    float4x4 invViewProjection;
};
[[vk::push_constant]] SkyPush u_view;

struct VSOutput
{
    float4 position : SV_Position;
    float2 ndc : TEXCOORD0;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    // (-1,-1), (3,-1), (-1,3): one triangle that covers the whole target.
    const float2 ndc = float2((vertexId == 1) ? 3.0 : -1.0, (vertexId == 2) ? 3.0 : -1.0);
    VSOutput output;
    output.position = float4(ndc, 1.0, 1.0);  // z = w: depth 1.0, the far plane
    output.ndc = ndc;
    return output;
}

float3 ViewRay(float2 ndc)
{
    const float4 nearPoint = mul(float4(ndc, 0.0, 1.0), u_view.invViewProjection);
    const float4 farPoint = mul(float4(ndc, 1.0, 1.0), u_view.invViewProjection);
    return normalize(farPoint.xyz / farPoint.w - nearPoint.xyz / nearPoint.w);
}

float4 PSMain(VSOutput input) : SV_Target
{
    SkyParameters sky;
    sky.zenith = u_zenith;
    sky.horizon = u_horizon;
    sky.ground = u_ground;
    sky.sunDir = u_sunDir;
    sky.sunColor = u_sunColor;
    sky.tint = u_tint;
    sky.color = u_color;
    if ((int)(u_zenith.w + 0.5) == 0)
        return float4(u_color.rgb, 1.0);
    return float4(SkyColor(sky, u_cube, u_cubeSampler, u_panorama, u_panoramaSampler, ViewRay(input.ndc)), 1.0);
}
