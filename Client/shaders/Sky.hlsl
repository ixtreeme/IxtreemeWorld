#pragma pack_matrix(row_major)

// The scene sky: one full-screen triangle at the far plane. Depth test LessOrEqual without depth
// writes, so it fills exactly the pixels no geometry covered — whether it is drawn first (it then
// becomes the background) or after the geometry (the water reflection pass).

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

static const float kPi = 3.14159265;

float3 ViewRay(float2 ndc)
{
    const float4 nearPoint = mul(float4(ndc, 0.0, 1.0), u_view.invViewProjection);
    const float4 farPoint = mul(float4(ndc, 1.0, 1.0), u_view.invViewProjection);
    return normalize(farPoint.xyz / farPoint.w - nearPoint.xyz / nearPoint.w);
}

float3 RotateY(float3 dir, float angle)
{
    const float s = sin(angle);
    const float c = cos(angle);
    return float3(c * dir.x + s * dir.z, dir.y, -s * dir.x + c * dir.z);
}

float3 ProceduralSky(float3 dir)
{
    const float3 sunDir = normalize(u_sunDir.xyz);
    const float up = dir.y;

    // Horizon to zenith above, a quick fade into the ground colour below.
    float3 sky = lerp(u_horizon.rgb, u_zenith.rgb, pow(saturate(up), 0.45));
    // Low sun: a warm band along the horizon on the sun's side (sunrise / sunset).
    const float lowSun = saturate(1.0 - abs(sunDir.y) * 4.0) * step(-0.25, sunDir.y);
    const float2 dirFlat = normalize(dir.xz + float2(1e-5, 0.0));
    const float2 sunFlat = normalize(sunDir.xz + float2(1e-5, 0.0));
    const float towardSun = pow(saturate(dot(dirFlat, sunFlat) * 0.5 + 0.5), 3.0);
    const float horizonBand = pow(1.0 - saturate(abs(up)), 6.0);
    sky = lerp(sky, float3(1.0, 0.42, 0.16) * max(max(u_horizon.r, u_horizon.g), u_horizon.b),
        lowSun * towardSun * horizonBand * 0.75);
    const float3 ground = lerp(u_horizon.rgb, u_ground.rgb, saturate(-up * 8.0));
    float3 color = up >= 0.0 ? sky : ground;

    // Night: once the sun is below the horizon the sky fades to a dark blue.
    const float day = saturate((sunDir.y + 0.18) / 0.36);
    color = lerp(u_zenith.rgb * float3(0.02, 0.025, 0.05), color, day);

    // The sun disc (above the horizon) and its glow, only while the sun light is on.
    if (u_tint.w > 0.5 && up > -0.02)
    {
        const float cosAngle = dot(dir, sunDir);
        const float discEdge = u_sunDir.w;
        if (discEdge <= 1.0)
        {
            const float soft = (1.0 - discEdge) * 0.25 + 1e-5;
            const float disc = smoothstep(discEdge - soft, discEdge + soft, cosAngle);
            color += u_sunColor.rgb * disc * 6.0;
        }
        // A tight bright halo plus a faint wide one; the rest of the sky keeps its own colours.
        const float glow = 0.8 * pow(saturate(cosAngle), 256.0) + 0.12 * pow(saturate(cosAngle), 16.0);
        color += u_sunColor.rgb * glow * u_sunColor.w * saturate(day + 0.25);
    }
    return color;
}

float3 PanoramaSky(float3 dir)
{
    // Equirectangular: u = longitude (0 at +Z, growing towards +X), v = 0 at the zenith.
    const float u = 0.5 + atan2(dir.x, dir.z) / (2.0 * kPi);
    const float v = acos(clamp(dir.y, -1.0, 1.0)) / kPi;
    // Level 0: the longitude seam would otherwise pick a tiny mip from the jump in derivatives.
    return u_panorama.SampleLevel(u_panoramaSampler, float2(u, v), 0.0).rgb;
}

float4 PSMain(VSOutput input) : SV_Target
{
    const int mode = (int)(u_zenith.w + 0.5);
    if (mode == 0)
        return float4(u_color.rgb, 1.0);

    const float3 dir = ViewRay(input.ndc);
    float3 color;
    if (mode == 2)
        color = u_cube.SampleLevel(u_cubeSampler, RotateY(dir, u_ground.w), 0.0).rgb;
    else if (mode == 3)
        color = PanoramaSky(RotateY(dir, u_ground.w));
    else
        color = ProceduralSky(dir);
    return float4(color * u_tint.rgb * u_horizon.w, 1.0);
}
