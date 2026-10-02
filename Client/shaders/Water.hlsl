#pragma pack_matrix(row_major)

[[vk::binding(0, 0)]] cbuffer WaterConstants : register(b0)
{
    float4x4 u_mvp;
    float4 u_cameraPos;
    float4 u_sunDir;
    float4 u_sunColor;
    float4 u_ambientColor;
    float4 u_baseColor;
    float4 u_reflectionColor;
    float4 u_waveParams1;
    float4 u_waveParams2;
    float4 u_levelTimeEnabled;
    float4 u_reflectionParams;
    float4 u_refractionParams;
    float4 u_shallowColor;
    float4 u_deepColor;
    float4 u_depthParams;
    float4 u_foamParams;
    float4 u_foamDepthParams;
    float4 u_causticParams;
    float4 u_cameraNearFar;
    float4 u_textureParams;
    float4 u_textureScroll;
};

[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] Texture2D u_waveNormalSmall : register(t0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState u_waveNormalSmallSampler : register(s0);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] Texture2D u_waveNormalLarge : register(t1);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] SamplerState u_waveNormalLargeSampler : register(s1);
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] Texture2D u_reflectionTexture : register(t2);
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] SamplerState u_reflectionSampler : register(s2);
[[vk::combinedImageSampler]] [[vk::binding(4, 0)]] Texture2D u_sceneColorTexture : register(t3);
[[vk::combinedImageSampler]] [[vk::binding(4, 0)]] SamplerState u_sceneColorSampler : register(s3);
[[vk::combinedImageSampler]] [[vk::binding(5, 0)]] Texture2D u_sceneDepthTexture : register(t4);
[[vk::combinedImageSampler]] [[vk::binding(5, 0)]] SamplerState u_sceneDepthSampler : register(s4);
[[vk::combinedImageSampler]] [[vk::binding(6, 0)]] Texture2D u_diffuseMap : register(t5);
[[vk::combinedImageSampler]] [[vk::binding(6, 0)]] SamplerState u_diffuseMapSampler : register(s5);

struct VSInput
{
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
    float edgeAlpha : TEXCOORD1;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 worldPos : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float4 clipPos : TEXCOORD2;
    float edgeAlpha : TEXCOORD3;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    float3 worldPos = input.position;
    output.worldPos = worldPos;
    output.uv = input.uv;
    output.position = mul(float4(worldPos, 1.0), u_mvp);
    output.clipPos = output.position;
    output.edgeAlpha = saturate(input.edgeAlpha);
    return output;
}

float3 SafeNormalize(float3 value, float3 fallback)
{
    const float len2 = dot(value, value);
    return len2 > 0.000001 ? value * rsqrt(len2) : fallback;
}

// The horizontal tilt (normal x/z, world space) of a Y-up encoded wave normal map sampled in a
// frame rotated by the orthonormal axes axisX/axisZ: rotating the sampling frame rotates the tilt
// too, so it is turned back into world space.
float2 SampleWaveTilt(Texture2D normalMap, SamplerState normalSampler, float2 uv, float2 axisX, float2 axisZ)
{
    const float2 tilt = normalMap.Sample(normalSampler, uv).rb * 2.0 - 1.0;
    return axisX * tilt.x + axisZ * tilt.y;
}

// How much of a wave layer survives at the pixel's footprint (in tiles of that layer): ripples
// shorter than a few pixels fade out instead of turning into sparkling noise far away.
float WaveLayerVisibility(float footprintMeters, float tilesPerMeter, float fadeStartTiles, float fadeEndTiles)
{
    return 1.0 - smoothstep(fadeStartTiles, fadeEndTiles, footprintMeters * tilesPerMeter);
}

float2 SampleWaveTilt(float2 worldXZ)
{
    const float time = u_levelTimeEnabled.y;
    // Long axis of the pixel's footprint on the water, in metres.
    const float footprint = max(length(ddx(worldXZ)), length(ddy(worldXZ)));
    // Each layer is sampled in its own rotated frame so the two tiles never line up into one grid.
    const float2 fineAxisX = float2(0.83, 0.56);
    const float2 fineAxisZ = float2(-0.56, 0.83);
    const float2 broadAxisX = float2(0.31, 0.95);
    const float2 broadAxisZ = float2(-0.95, 0.31);
    const float2 fineBasis = float2(dot(worldXZ, fineAxisX), dot(worldXZ, fineAxisZ));
    const float2 broadBasis = float2(dot(worldXZ, broadAxisX), dot(worldXZ, broadAxisZ));

    const bool useNormalA = u_textureParams.x > 0.5;
    const bool useNormalB = u_textureParams.y > 0.5;
    if (useNormalA || useNormalB)
    {
        // Authored maps: layer B at another scale and orientation than layer A (usually the same
        // map), otherwise both repeat on the same grid and the tiling shows.
        const float materialTiling = max(u_textureParams.w, 0.001);
        const float2 uvA = worldXZ * materialTiling + u_textureScroll.xy * time;
        const float2 uvB = broadBasis * (materialTiling * 0.62) + u_textureScroll.zw * time;
        float2 texturedTilt = float2(0.0, 0.0);
        float layerCount = 0.0;
        if (useNormalA)
        {
            texturedTilt += SampleWaveTilt(u_waveNormalSmall, u_waveNormalSmallSampler, uvA, float2(1.0, 0.0), float2(0.0, 1.0)) *
                WaveLayerVisibility(footprint, materialTiling, 0.01, 0.05);
            layerCount += 1.0;
        }
        if (useNormalB)
        {
            texturedTilt += SampleWaveTilt(u_waveNormalLarge, u_waveNormalLargeSampler, uvB, broadAxisX, broadAxisZ) *
                WaveLayerVisibility(footprint, materialTiling * 0.62, 0.01, 0.05);
            layerCount += 1.0;
        }
        return texturedTilt / max(layerCount, 1.0);
    }

    // The generated maps hold 5-28 (fine) and 2-10 (broad) wave cycles per tile.
    const float fineTiling = max(u_waveParams1.x, 0.001);
    const float broadTiling = max(u_waveParams1.y, 0.001);
    const float2 uvSmall = fineBasis * fineTiling + float2(time * u_waveParams1.z, time * u_waveParams1.z * 0.47);
    const float2 uvLarge = broadBasis * broadTiling - float2(time * u_waveParams1.w * 0.63, time * u_waveParams1.w);
    return (SampleWaveTilt(u_waveNormalSmall, u_waveNormalSmallSampler, uvSmall, fineAxisX, fineAxisZ) *
                   WaveLayerVisibility(footprint, fineTiling, 0.02, 0.09) +
               SampleWaveTilt(u_waveNormalLarge, u_waveNormalLargeSampler, uvLarge, broadAxisX, broadAxisZ) *
                   WaveLayerVisibility(footprint, broadTiling, 0.03, 0.15)) *
        0.5;
}

float FoamHash(float2 p)
{
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

float FoamNoise(float2 uv)
{
    float2 i = floor(uv);
    float2 f = frac(uv);
    f = f * f * (3.0 - 2.0 * f);

    float a = FoamHash(i);
    float b = FoamHash(i + float2(1.0, 0.0));
    float c = FoamHash(i + float2(0.0, 1.0));
    float d = FoamHash(i + float2(1.0, 1.0));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float LinearizeWaterDepth(float ndcDepth)
{
    const float nearPlane = max(u_cameraNearFar.x, 0.0001);
    const float farPlane = max(u_cameraNearFar.y, nearPlane + 0.001);
    const float a = farPlane / (farPlane - nearPlane);
    const float b = (nearPlane * farPlane) / (farPlane - nearPlane);
    return b / max(a - saturate(ndcDepth), 0.000001);
}

float4 PSMain(VSOutput input) : SV_Target0
{
    if (u_levelTimeEnabled.z < 0.5)
        discard;

    float3 v = SafeNormalize(u_cameraPos.xyz - input.worldPos, float3(0.0, 1.0, 0.0));
    // Toward the horizon the ripples lie flatter: seen at a grazing angle, their full tilt flips the
    // Fresnel term between "mirror" and "see-through" on every crest, banding the water.
    const float grazingFlatten = lerp(0.35, 1.0, saturate(abs(v.y) * 4.0));
    float2 tilt = SampleWaveTilt(input.worldPos.xz) * (u_waveParams2.x * grazingFlatten);
    float3 n = SafeNormalize(float3(tilt.x, 1.0, tilt.y), float3(0.0, 1.0, 0.0));
    float ndotv = saturate(dot(n, v));
    float fresnel = u_waveParams2.z + (1.0 - u_waveParams2.z) * pow(1.0 - ndotv, u_waveParams2.y);

    float3 l = SafeNormalize(u_sunDir.xyz, float3(0.0, 1.0, 0.0));
    float ndotl = saturate(dot(n, l));
    float3 diffuseLight = u_ambientColor.rgb + u_sunColor.rgb * ndotl;
    float3 h = SafeNormalize(v + l, l);
    float specular = pow(saturate(dot(n, h)), 96.0) * 2.0;

    float3 reflectionColor = u_reflectionColor.rgb;
    if (u_reflectionParams.x > 0.5)
    {
        float2 screenUv = (input.clipPos.xy / max(input.clipPos.w, 0.0001)) * 0.5 + 0.5;
        screenUv += n.xz * u_reflectionParams.y;
        reflectionColor = u_reflectionTexture.Sample(u_reflectionSampler, saturate(screenUv)).rgb;
    }

    float2 screenUv = (input.clipPos.xy / max(input.clipPos.w, 0.0001)) * 0.5 + 0.5;
    const float waterDepthNdc = saturate(input.clipPos.z / max(input.clipPos.w, 0.0001));

    float sceneDepthNdc = u_sceneDepthTexture.Sample(u_sceneDepthSampler, saturate(screenUv)).r;
    float waterViewDepth = max(0.0, LinearizeWaterDepth(sceneDepthNdc) - LinearizeWaterDepth(waterDepthNdc));
    float depthT = smoothstep(u_depthParams.x, max(u_depthParams.y, u_depthParams.x + 0.001), waterViewDepth);
    float fadeT = saturate(waterViewDepth / max(u_depthParams.z, 0.001));
    float3 depthWaterColor = lerp(u_shallowColor.rgb, u_deepColor.rgb, depthT);
    if (u_textureParams.z > 0.5)
    {
        const float materialTiling = max(u_textureParams.w, 0.001) * 0.5;
        float2 diffuseUv = input.worldPos.xz * materialTiling + u_textureScroll.xy * u_levelTimeEnabled.y * 0.3;
        float3 diffuseTint = u_diffuseMap.Sample(u_diffuseMapSampler, diffuseUv).rgb;
        depthWaterColor = lerp(depthWaterColor, depthWaterColor * diffuseTint, 0.5);
    }

    float2 refractionUv = screenUv;
    if (u_refractionParams.x > 0.5)
    {
        float refractionFactor = u_refractionParams.y * (1.0 + u_refractionParams.z * depthT);
        const float2 distortedUv = saturate(screenUv + n.xz * refractionFactor);
        // Refract only toward what lies under the water: a distorted sample landing on something in
        // front of the surface (the shore, a character) would smear it onto the water.
        const float distortedDepthNdc = u_sceneDepthTexture.Sample(u_sceneDepthSampler, distortedUv).r;
        refractionUv = distortedDepthNdc > waterDepthNdc ? distortedUv : screenUv;
    }
    float3 refractedColor = u_sceneColorTexture.Sample(u_sceneColorSampler, refractionUv).rgb;
    float3 underwaterColor = lerp(refractedColor, depthWaterColor, fadeT);

    float3 surfaceColor = u_baseColor.rgb * diffuseLight + u_sunColor.rgb * specular;
    float3 transmissiveColor = u_refractionParams.x > 0.5
        ? lerp(surfaceColor, underwaterColor, saturate(u_baseColor.a))
        : surfaceColor;
    float3 finalColor = lerp(transmissiveColor, reflectionColor, saturate(fresnel));
    if (u_foamParams.x > 0.5)
    {
        const float time = u_levelTimeEnabled.y;
        const float foamScale = max(u_foamParams.y, 0.05);
        float2 uv0 = input.worldPos.xz / max(foamScale * 6.0, 0.5) + float2(time * u_foamParams.z, time * u_foamParams.z * 0.67);
        float2 uv1 = input.worldPos.xz / max(foamScale * 14.0, 1.0) - float2(time * u_foamParams.z * 0.47, time * u_foamParams.z * 0.29);
        float foamPattern = FoamNoise(uv0) * 0.65 + FoamNoise(uv1) * 0.35;
        foamPattern = smoothstep(0.48, 0.78, foamPattern);
        float foamDistance = max(u_foamDepthParams.x, 0.02);
        float foamSoftness = max(u_foamDepthParams.y, 0.02);
        float depthGradient = fwidth(waterViewDepth);
        float shorelineEdge = smoothstep(0.002, 0.08, depthGradient);
        float shoreFoam = (1.0 - smoothstep(0.02, foamDistance + foamSoftness, waterViewDepth)) * shorelineEdge;
        float foamMask = smoothstep(0.5, 0.9, input.edgeAlpha);
        float foam = foamPattern * shoreFoam * foamMask * saturate(u_foamParams.w) * 0.65;
        finalColor = lerp(finalColor, float3(0.95, 0.98, 1.0), foam);
    }
    finalColor = min(finalColor, float3(10.0, 10.0, 10.0));
    float outputAlpha = (u_refractionParams.x > 0.5 ? 1.0 : u_baseColor.a) * saturate(input.edgeAlpha);
    return float4(max(finalColor, 0.0.xxx), outputAlpha);
}
