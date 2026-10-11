#ifndef IX_FOG_HLSLI
#define IX_FOG_HLSLI

struct FogVolumeGpu
{
    float4 centerShape;
    float4 axisXExtent;
    float4 axisYExtent;
    float4 axisZExtent;
    float4 colorDensity;
    float4 fade;
};

struct FogUniforms
{
    float4x4 inverseViewProjection;
    float4 camera;
    float4 colorDensity;
    float4 globalParams;
    float4 options;
    FogVolumeGpu volumes[32];
};

float FogHeightIntegral(float y0, float y1, float rayLength, float base, float falloff)
{
    if (falloff <= 0.0 || max(y0, y1) <= base) return rayLength;
    const float low = min(y0, y1), high = max(y0, y1);
    const float below = low < base ? (base - low) / (high - low) : 0.0;
    const float h0 = max(0.0, low - base), h1 = max(0.0, high - base);
    const float delta = (h1 - h0) * falloff;
    const float average = delta < 0.001 ? exp(-(h0 + h1) * 0.5 * falloff) :
        (exp(-h0 * falloff) - exp(-h1 * falloff)) / delta;
    return rayLength * (below + (1.0 - below) * average);
}

float GlobalFogOpticalDepth(FogUniforms fog, float3 direction, float distance, bool sky)
{
    if (fog.options.x == 0.0 || (sky && fog.options.y == 0.0)) return 0.0;
    const float end = min(distance, fog.options.w);
    const float begin = min(end, fog.globalParams.x);
    const float optical = fog.colorDensity.w * FogHeightIntegral(fog.camera.y + direction.y * begin,
        fog.camera.y + direction.y * end, end - begin, fog.globalParams.y, fog.globalParams.z);
    return min(optical, -log(max(1.0e-9, 1.0 - fog.globalParams.w)));
}

float LocalFogOpticalDepth(FogVolumeGpu fog, float3 origin, float3 direction, float distance)
{
    const float3 relative = origin - fog.centerShape.xyz;
    const float3 p = float3(dot(relative, fog.axisXExtent.xyz), dot(relative, fog.axisYExtent.xyz), dot(relative, fog.axisZExtent.xyz));
    const float3 d = float3(dot(direction, fog.axisXExtent.xyz), dot(direction, fog.axisYExtent.xyz), dot(direction, fog.axisZExtent.xyz));
    const float3 extents = float3(fog.axisXExtent.w, fog.axisYExtent.w, fog.axisZExtent.w);
    float enter = 0.0, exit = distance;
    if (fog.centerShape.w == 1.0)
    {
        const float b = dot(p, d), c = dot(p, p) - extents.x * extents.x;
        const float discriminant = b * b - c;
        if (discriminant <= 0.0) return 0.0;
        const float root = sqrt(discriminant);
        enter = max(enter, -b - root); exit = min(exit, -b + root);
    }
    else
    {
        [unroll]
        for (int axis = 0; axis < 3; ++axis)
        {
            if (abs(d[axis]) < 1.0e-6)
            {
                if (abs(p[axis]) > extents[axis]) return 0.0;
            }
            else
            {
                const float a = (-extents[axis] - p[axis]) / d[axis];
                const float b = ( extents[axis] - p[axis]) / d[axis];
                enter = max(enter, min(a, b)); exit = min(exit, max(a, b));
            }
        }
    }
    if (exit <= enter) return 0.0;
    if (fog.fade.x <= 0.0) return (exit - enter) * fog.colorDensity.w;
    float weight = 0.0;
    // Integrate only inside the exact intersection, so small or distant volumes cannot be skipped.
    [loop]
    for (int stepIndex = 0; stepIndex < 16; ++stepIndex)
    {
        const float3 samplePosition = p + d * (enter + (exit - enter) * (stepIndex + 0.5) / 16.0);
        const float3 margin = extents - abs(samplePosition);
        const float edge = fog.centerShape.w == 1.0 ? extents.x - length(samplePosition) : min(margin.x, min(margin.y, margin.z));
        const float t = saturate(edge / fog.fade.x);
        weight += t * t * (3.0 - 2.0 * t);
    }
    return weight * (exit - enter) / 16.0 * fog.colorDensity.w;
}

// xyz: density-weighted fog colour, w: transmission. Stable when volumes overlap or swap order.
float4 EvaluateFog(FogUniforms fog, float3 direction, float distance, bool sky)
{
    float depth = GlobalFogOpticalDepth(fog, direction, distance, sky);
    float3 weighted = fog.colorDensity.xyz * depth;
    [loop]
    for (int i = 0; i < clamp((int)fog.options.z, 0, 32); ++i)
    {
        const float local = LocalFogOpticalDepth(fog.volumes[i], fog.camera.xyz, direction, distance);
        depth += local; weighted += fog.volumes[i].colorDensity.xyz * local;
    }
    return float4(depth > 1.0e-6 ? weighted / depth : 0.0.xxx, exp(-depth));
}

#endif
