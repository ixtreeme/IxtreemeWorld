#ifndef IX_SKY_FUNCTIONS_HLSLI
#define IX_SKY_FUNCTIONS_HLSLI

// The scene sky's colour in a world direction: Sky.hlsl draws it as the background, material graph
// shaders mirror it (Sky Reflection). The parameters are SkyRenderer's uniform block, in its order.
struct SkyParameters
{
    float4 zenith;    // rgb linear; w = mode (0 colour, 1 procedural, 2 cubemap, 3 panorama)
    float4 horizon;   // rgb linear; w = exposure
    float4 ground;    // rgb linear; w = rotation (radians, around +Y)
    float4 sunDir;    // xyz towards the sun; w = cos(sun disc radius), > 1 hides the disc
    float4 sunColor;  // rgb linear (colour x intensity, at most the colour); w = glow strength
    float4 tint;      // rgb tint; w = 1 when the sun light is on
    float4 color;     // rgb linear flat colour (mode 0)
};

static const float kSkyPi = 3.14159265;

float3 SkyRotateY(float3 dir, float angle)
{
    const float s = sin(angle);
    const float c = cos(angle);
    return float3(c * dir.x + s * dir.z, dir.y, -s * dir.x + c * dir.z);
}

float3 SkyProcedural(SkyParameters sky, float3 dir)
{
    const float3 sunDir = normalize(sky.sunDir.xyz);
    const float up = dir.y;

    // Horizon to zenith above, a quick fade into the ground colour below.
    float3 color = lerp(sky.horizon.rgb, sky.zenith.rgb, pow(saturate(up), 0.45));
    // Low sun: a warm band along the horizon on the sun's side (sunrise / sunset).
    const float lowSun = saturate(1.0 - abs(sunDir.y) * 4.0) * step(-0.25, sunDir.y);
    const float2 dirFlat = normalize(dir.xz + float2(1e-5, 0.0));
    const float2 sunFlat = normalize(sunDir.xz + float2(1e-5, 0.0));
    const float towardSun = pow(saturate(dot(dirFlat, sunFlat) * 0.5 + 0.5), 3.0);
    const float horizonBand = pow(1.0 - saturate(abs(up)), 6.0);
    color = lerp(color, float3(1.0, 0.42, 0.16) * max(max(sky.horizon.r, sky.horizon.g), sky.horizon.b),
        lowSun * towardSun * horizonBand * 0.75);
    const float3 ground = lerp(sky.horizon.rgb, sky.ground.rgb, saturate(-up * 8.0));
    color = up >= 0.0 ? color : ground;

    // Night: once the sun is below the horizon the sky fades to a dark blue.
    const float day = saturate((sunDir.y + 0.18) / 0.36);
    color = lerp(sky.zenith.rgb * float3(0.02, 0.025, 0.05), color, day);

    // The sun disc (above the horizon) and its glow, only while the sun light is on.
    if (sky.tint.w > 0.5 && up > -0.02)
    {
        const float cosAngle = dot(dir, sunDir);
        const float discEdge = sky.sunDir.w;
        if (discEdge <= 1.0)
        {
            const float soft = (1.0 - discEdge) * 0.25 + 1e-5;
            const float disc = smoothstep(discEdge - soft, discEdge + soft, cosAngle);
            color += sky.sunColor.rgb * disc * 6.0;
        }
        // A tight bright halo plus a faint wide one; the rest of the sky keeps its own colours.
        const float glow = 0.8 * pow(saturate(cosAngle), 256.0) + 0.12 * pow(saturate(cosAngle), 16.0);
        color += sky.sunColor.rgb * glow * sky.sunColor.w * saturate(day + 0.25);
    }
    return color;
}

// Equirectangular: u = longitude (0 at +Z, growing towards +X), v = 0 at the zenith.
float2 SkyPanoramaUv(float3 dir)
{
    return float2(0.5 + atan2(dir.x, dir.z) / (2.0 * kSkyPi), acos(clamp(dir.y, -1.0, 1.0)) / kSkyPi);
}

// The sky towards dir (unit length), as the background shows it: exposure and tint applied, except
// to a flat colour sky. Images are read at level 0: the panorama's longitude seam would otherwise
// pick a tiny mip from the jump in derivatives.
float3 SkyColor(SkyParameters sky, TextureCube cube, SamplerState cubeSampler, Texture2D panorama,
                SamplerState panoramaSampler, float3 dir)
{
    const int mode = (int)(sky.zenith.w + 0.5);
    if (mode == 0)
        return sky.color.rgb;
    float3 color;
    if (mode == 2)
        color = cube.SampleLevel(cubeSampler, SkyRotateY(dir, sky.ground.w), 0.0).rgb;
    else if (mode == 3)
        color = panorama.SampleLevel(panoramaSampler, SkyPanoramaUv(SkyRotateY(dir, sky.ground.w)), 0.0).rgb;
    else
        color = SkyProcedural(sky, dir);
    return color * sky.tint.rgb * sky.horizon.w;
}

#endif
