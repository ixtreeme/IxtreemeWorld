#pragma pack_matrix(row_major)

// Volumetric light: the sunlight the air scatters towards the camera along each view ray. The sun
// shadow cascades decide which parts of the ray the sun reaches, so shadowed air stays dark and lit
// air between the shadows shows as shafts — seen from the side too, wherever the sun is. Rendered at
// quarter size (half at high quality); a per-pixel offset turns march banding into noise that the blur pass
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
    float4 params;               // x = steps; y = depth-unit bias; z = bias correction in metres; w = shadow filter radius
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
float FilterShadow(float3 p, int cascade, float bias, float2 shadowTexel)
{
    // Four bilinear comparison taps soften the binary leaf shadows before ray integration.
    const float2 radius = shadowTexel * u_push.params.w;
    float visibility = 0.0;
    [unroll]
    for (int y = -1; y <= 1; y += 2)
    {
        [unroll]
        for (int x = -1; x <= 1; x += 2)
            visibility += u_shadow.SampleCmpLevelZero(u_shadowSampler,
                float3(clamp(p.xy + float2(x, y) * radius, shadowTexel * 0.5, 1.0 - shadowTexel * 0.5),
                    (float)cascade), p.z - bias);
    }
    return visibility * 0.25;
}

float CascadeVisibility(float3 p, float3 nextPoint, int cascade, float bias, float nextBias, float2 shadowTexel)
{
    const float visibility = FilterShadow(p, cascade, bias, shadowTexel);
    const float edge = min(min(p.x, 1.0 - p.x), min(p.y, 1.0 - p.y));
    const float weight = smoothstep(0.0, 0.06, edge);
    if (weight >= 1.0)
        return visibility;
    float nextVisibility = 1.0;
    if (cascade < 3 && all(nextPoint > 0.0) && all(nextPoint < 1.0))
        nextVisibility = FilterShadow(nextPoint, cascade + 1, nextBias, shadowTexel);
    return lerp(nextVisibility, visibility, weight);
}

float SunVisibility(float3 lightSpace[4], float compareBias[4], float2 shadowTexel)
{
    [unroll]
    for (int cascade = 0; cascade < 4; ++cascade)
    {
        const float3 p = lightSpace[cascade];
        if (all(p > 0.0) && all(p < 1.0))
            return CascadeVisibility(p, lightSpace[min(cascade + 1, 3)], cascade,
                compareBias[cascade], compareBias[min(cascade + 1, 3)], shadowTexel);
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

float RayCoverage(int stepIndex, float stepLength, float nearLength, float farWeight)
{
    return lerp(farWeight, 1.0, saturate(nearLength / max(stepLength, 1e-5) - stepIndex));
}

float GeometricSum(float transmittance, int steps)
{
    const float oneMinus = 1.0 - transmittance;
    return oneMinus > 1e-5 ? (1.0 - pow(transmittance, (float)steps)) / oneMinus : (float)steps;
}

float4 MarchPS(VSOutput input) : SV_Target
{
    uint depthWidth, depthHeight, shadowWidth, shadowHeight, shadowLayers;
    u_depth.GetDimensions(depthWidth, depthHeight);
    u_shadow.GetDimensions(shadowWidth, shadowHeight, shadowLayers);
    const float2 shadowTexel = 1.0 / float2(shadowWidth, shadowHeight);
    const float3 camera = u_push.cameraPos.xyz;
    const float4 farHit = mul(float4(input.ndc, 1.0, 1.0), u_push.invViewProjection);
    const float3 ray = farHit.xyz / farHit.w - camera;
    const float distance = length(ray);
    const float3 dir = ray / max(distance, 1e-4);

    // A low-resolution pixel may cover both a leaf and sky. March once to the farthest
    // surface, then weight the air beyond the nearest surface by the remaining coverage.
    // This preserves partial light through small gaps without marching four/sixteen rays.
    const float2 footprint = float2(abs(ddx(input.uv.x)), abs(ddy(input.uv.y)));
    const float2 depthSize = float2(depthWidth, depthHeight);
    const int2 coverageSamples = clamp(int2(ceil(footprint * depthSize - 1e-3)), 1, 4);
    const float4 nearHit = mul(float4(input.ndc, 0.0, 1.0), u_push.invViewProjection);
    const float4 depthAxis = mul(float4(0.0, 0.0, 1.0, 0.0), u_push.invViewProjection);
    float nearRayLength = u_push.cameraPos.w;
    float rayLength = 0.0;
    float meanLength = 0.0;
    for (int y = 0; y < coverageSamples.y; ++y)
    {
        for (int x = 0; x < coverageSamples.x; ++x)
        {
            const float2 offset = (float2(x + 0.5, y + 0.5) / coverageSamples - 0.5) * footprint;
            const int2 pixel = clamp(int2((input.uv + offset) * depthSize), int2(0, 0), int2(depthWidth, depthHeight) - 1);
            const float depth = u_depth.Load(int3(pixel, 0)).r;
            const float4 hit = nearHit + depth * depthAxis;
            const float lengthToSurface = min(length(hit.xyz / hit.w - camera), u_push.cameraPos.w);
            nearRayLength = min(nearRayLength, lengthToSurface);
            rayLength = max(rayLength, lengthToSurface);
            meanLength += lengthToSurface;
        }
    }
    meanLength /= coverageSamples.x * coverageSamples.y;
    const float span = rayLength - nearRayLength;
    const float farWeight = span > 1e-4 ? saturate((meanLength - nearRayLength) / span) : 1.0;
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
    float compareBias[4];
    [unroll]
    for (int cascade = 0; cascade < 4; ++cascade)
    {
        lightSpace[cascade] = CascadeLightSpace(camera + dir * t0, cascade);
        lightSpaceStep[cascade] = CascadeLightSpace(camera + dir * (t0 + stepLength), cascade) - lightSpace[cascade];
        const float4x4 matrix = u_cascadeViewProj[cascade];
        const float depthPerMetre = length(float3(matrix[0][2], matrix[1][2], matrix[2][2]));
        compareBias[cascade] = max(0.0, u_push.params.y - u_push.params.z * depthPerMetre);
    }
    // The last step (from 0) at which the march is still inside each cascade's box, and whether it
    // starts inside all of them.
    bool startsInsideAll = true;
    int lastInside[4];
    [unroll]
    for (int box = 0; box < 4; ++box)
    {
        const float3 a = lightSpace[box];
        const float3 b = lightSpaceStep[box];
        startsInsideAll = startsInsideAll && all(a > 0.0) && all(a < 1.0);
        // Steps until each coordinate reaches the edge (0 or 1) it moves towards.
        const float3 towardsOne = step(0.0, b);
        const float3 exitAt = lerp(a, 1.0 - a, towardsOne) / max(abs(b), 1e-12);
        lastInside[box] = (int)ceil(min(min(min(exitAt.x, exitAt.y), exitAt.z), 4096.0)) - 1;
    }
    float transmittance = 1.0;
    float lit = 0.0;
    if (startsInsideAll)
    {
        // The usual case: the march starts inside every box (the cascades are centred on the eye that
        // drives them). A ray leaves a convex box once and for all, so the finest cascade holding a
        // step only grows along the ray: each cascade takes the stretch up to where the ray leaves it,
        // with no per-step search.
        int cursor = 0;
        [unroll]
        for (int c = 0; c < 4; ++c)
        {
            const int end = min(lastInside[c] + 1, steps);
            float3 p = lightSpace[c] + lightSpaceStep[c] * (float)cursor;
            const int next = min(c + 1, 3);
            float3 nextPoint = lightSpace[next] + lightSpaceStep[next] * (float)cursor;
            for (int i = cursor; i < end; ++i)
            {
                lit += CascadeVisibility(p, nextPoint, c, compareBias[c], compareBias[next], shadowTexel) *
                    transmittance * RayCoverage(i, stepLength, nearRayLength, farWeight);
                transmittance *= stepTransmittance;
                p += lightSpaceStep[c];
                nextPoint += lightSpaceStep[next];
            }
            cursor = max(cursor, end);
        }
        // Past the last cascade there is no shadow information: the air there is lit. Those steps add
        // up to a geometric series.
        const int rest = steps - cursor;
        if (rest > 0)
        {
            const float nearSteps = nearRayLength / max(stepLength, 1e-5) - cursor;
            const int full = clamp((int)floor(nearSteps), 0, rest);
            const float partial = full < rest ? saturate(nearSteps - full) : 0.0;
            lit += transmittance * (farWeight * GeometricSum(stepTransmittance, rest) +
                (1.0 - farWeight) * (GeometricSum(stepTransmittance, full) + partial * pow(stepTransmittance, (float)full)));
        }
    }
    else
    {
        // A camera other than the one the cascades follow (the editor Game view while the Scene view
        // drives them): the finest cascade holding each step, step by step.
        for (int i = 0; i < steps; ++i)
        {
            lit += SunVisibility(lightSpace, compareBias, shadowTexel) *
                transmittance * RayCoverage(i, stepLength, nearRayLength, farWeight);
            transmittance *= stepTransmittance;
            [unroll]
            for (int c = 0; c < 4; ++c)
                lightSpace[c] += lightSpaceStep[c];
        }
    }
    // Each lit step scatters (1 - its transmittance) of the sunlight towards the camera. The 0.25 keeps
    // the haze under the brightness of the lit scene (the image is not HDR).
    const float scattered = lit * (1.0 - stepTransmittance);
    const float phase = Phase(dot(dir, normalize(u_push.sunDir.xyz)), u_push.sunColor.w);
    return float4(u_push.sunColor.rgb * scattered * phase * 0.25, 1.0);
}
