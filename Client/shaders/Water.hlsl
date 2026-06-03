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

struct VSInput
{
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 worldPos : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float4 clipPos : TEXCOORD2;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    float3 worldPos = input.position;
    output.worldPos = worldPos;
    output.uv = input.uv;
    output.position = mul(float4(worldPos, 1.0), u_mvp);
    output.clipPos = output.position;
    return output;
}

float3 SafeNormalize(float3 value, float3 fallback)
{
    const float len2 = dot(value, value);
    return len2 > 0.000001 ? value * rsqrt(len2) : fallback;
}

float3 SampleWaveNormal(float2 worldXZ)
{
    const float time = u_levelTimeEnabled.y;
    const float fineTiling = max(u_waveParams1.x, 0.001);
    const float broadTiling = max(u_waveParams1.y, 0.001);
    float2 fineBasis = float2(dot(worldXZ, float2(0.83, 0.56)), dot(worldXZ, float2(-0.56, 0.83)));
    float2 broadBasis = float2(dot(worldXZ, float2(0.31, 0.95)), dot(worldXZ, float2(-0.95, 0.31)));
    float2 uvSmall = fineBasis * fineTiling + float2(time * u_waveParams1.z, time * u_waveParams1.z * 0.47);
    float2 uvLarge = broadBasis * broadTiling - float2(time * u_waveParams1.w * 0.63, time * u_waveParams1.w);

    float3 nSmall = u_waveNormalSmall.Sample(u_waveNormalSmallSampler, uvSmall).rgb * 2.0 - 1.0;
    float3 nLarge = u_waveNormalLarge.Sample(u_waveNormalLargeSampler, uvLarge).rgb * 2.0 - 1.0;
    float3 tangentNormal = (nSmall + nLarge) * 0.5;
    return SafeNormalize(float3(tangentNormal.x, 1.0, tangentNormal.z) * float3(u_waveParams2.x, 1.0, u_waveParams2.x),
        float3(0.0, 1.0, 0.0));
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

    float3 n = SampleWaveNormal(input.worldPos.xz);
    float3 v = SafeNormalize(u_cameraPos.xyz - input.worldPos, float3(0.0, 1.0, 0.0));
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

    float2 refractionUv = screenUv;
    if (u_refractionParams.x > 0.5)
    {
        float refractionFactor = u_refractionParams.y * (1.0 + u_refractionParams.z * depthT);
        refractionUv = saturate(screenUv + n.xz * refractionFactor);
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
        float foam = foamPattern * shoreFoam * saturate(u_foamParams.w) * 0.65;
        finalColor = lerp(finalColor, float3(0.95, 0.98, 1.0), foam);
    }
    return float4(max(finalColor, 0.0.xxx), u_refractionParams.x > 0.5 ? 1.0 : u_baseColor.a);
}
