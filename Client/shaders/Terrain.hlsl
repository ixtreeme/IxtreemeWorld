#pragma pack_matrix(row_major)

struct PointLightUbo
{
    float4 position;
    float4 color;
};

struct SpotLightUbo
{
    float4 position;
    float4 direction;
    float4 color;
};

struct TerrainWaterBodyUbo
{
    float4 bboxMinMax;       // minX, minZ, maxX, maxZ
    float4 levelModeEnabled; // levelY, causticMode, enabled, foamEnabled
    float4 foamParams;       // distance, softness, intensity, scale
    float4 causticParams;    // intensity, scale, speed, maxDepth
    float4 edgeParams;       // fadeDistance, curve, reserved, reserved
};

[[vk::binding(0, 0)]] cbuffer TerrainConstants : register(b0)
{
    float4x4 u_mvp;
    float4 u_materialTiling[8];
    float4 u_materialTintNormal[8];
    float4 u_materialPbr[8];
    float4 u_cameraPos;
    float4 u_sunDir;
    float4 u_sunColor;
    float4 u_ambientColor;
    float4x4 u_cascadeViewProj[4];
    float4 u_cascadeSplits;
    float4 u_shadowParams;
    int u_numPointLights;
    int u_numSpotLights;
    float2 u_lightPadding;
    float4 u_waterGlobalParams; // time, activeCount, truncatedCount, reserved
    TerrainWaterBodyUbo u_terrainWaterBodies[8];
    PointLightUbo u_pointLights[16];
    SpotLightUbo u_spotLights[16];
};

struct TerrainLayerConstants
{
    float4 u_layerParams;
};

[[vk::push_constant]] TerrainLayerConstants u_layerConstants;

[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] Texture2DArray u_paletteTex : register(t0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState u_paletteSampler : register(s0);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] Texture2D u_splatATex : register(t1);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] SamplerState u_splatASampler : register(s1);
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] Texture2D u_splatBTex : register(t2);
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] SamplerState u_splatBSampler : register(s2);
[[vk::combinedImageSampler]] [[vk::binding(4, 0)]] Texture2DArray u_normalTex : register(t3);
[[vk::combinedImageSampler]] [[vk::binding(4, 0)]] SamplerState u_normalSampler : register(s3);
[[vk::combinedImageSampler]] [[vk::binding(5, 0)]] Texture2DArray u_aoTex : register(t4);
[[vk::combinedImageSampler]] [[vk::binding(5, 0)]] SamplerState u_aoSampler : register(s4);
[[vk::combinedImageSampler]] [[vk::binding(6, 0)]] Texture2DArray u_roughnessTex : register(t5);
[[vk::combinedImageSampler]] [[vk::binding(6, 0)]] SamplerState u_roughnessSampler : register(s5);
[[vk::combinedImageSampler]] [[vk::binding(7, 0)]] Texture2DArray u_metallicTex : register(t6);
[[vk::combinedImageSampler]] [[vk::binding(7, 0)]] SamplerState u_metallicSampler : register(s6);
[[vk::combinedImageSampler]] [[vk::binding(8, 0)]] Texture2DArray u_heightTex : register(t7);
[[vk::combinedImageSampler]] [[vk::binding(8, 0)]] SamplerState u_heightSampler : register(s7);
[[vk::combinedImageSampler]] [[vk::binding(9, 0)]] Texture2DArray<float> u_shadowTex : register(t8);
[[vk::combinedImageSampler]] [[vk::binding(9, 0)]] SamplerComparisonState u_shadowSampler : register(s8);

struct VSInput
{
    float3 position : POSITION;
    float2 texUv : TEXCOORD0;
    float2 maskUv : TEXCOORD1;
};

struct VSOutput
{
    float4 position : SV_Position;
    float2 texUv : TEXCOORD0;
    float2 maskUv : TEXCOORD1;
    float2 localXZ : TEXCOORD2;
    float3 worldPos : TEXCOORD3;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.position = mul(float4(input.position, 1.0), u_mvp);
    output.texUv = input.texUv * u_layerConstants.u_layerParams.xy;
    output.maskUv = input.maskUv;
    output.localXZ = input.position.xz;
    output.worldPos = input.position;
    return output;
}

static const float PI = 3.14159265359;

float3 SafeNormalize(float3 v, float3 fallback)
{
    const float len2 = dot(v, v);
    return len2 > 0.000001 ? v * rsqrt(len2) : fallback;
}

float DistributionGGX(float nDotH, float roughness)
{
    const float a = roughness * roughness;
    const float a2 = a * a;
    const float denomTerm = nDotH * nDotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denomTerm * denomTerm, 0.0001);
}

float GeometrySchlickGGX(float nDotX, float roughness)
{
    const float r = roughness + 1.0;
    const float k = (r * r) / 8.0;
    return nDotX / max(nDotX * (1.0 - k) + k, 0.0001);
}

float GeometrySmith(float nDotV, float nDotL, float roughness)
{
    return GeometrySchlickGGX(nDotV, roughness) * GeometrySchlickGGX(nDotL, roughness);
}

float3 FresnelSchlick(float hDotV, float3 f0)
{
    return f0 + (1.0 - f0) * pow(1.0 - saturate(hDotV), 5.0);
}

float3 EvaluateCookTorrance(float3 albedo,
                            float3 n,
                            float3 v,
                            float nDotV,
                            float roughness,
                            float metallic,
                            float3 f0,
                            float3 l,
                            float3 radiance)
{
    const float3 h = SafeNormalize(v + l, l);
    const float nDotL = saturate(dot(n, l));
    const float nDotH = saturate(dot(n, h));
    const float hDotV = saturate(dot(h, v));
    const float d = DistributionGGX(nDotH, roughness);
    const float g = GeometrySmith(nDotV, nDotL, roughness);
    const float3 f = FresnelSchlick(hDotV, f0);
    const float3 specular = (d * g * f) / max(4.0 * nDotV * nDotL, 0.0001);
    const float3 kS = f;
    const float3 kD = (1.0.xxx - kS) * (1.0 - metallic);
    return (kD * albedo / PI + specular) * radiance * nDotL;
}

int SelectShadowCascade(float distanceToCamera)
{
    if (distanceToCamera < u_cascadeSplits.x) return 0;
    if (distanceToCamera < u_cascadeSplits.y) return 1;
    if (distanceToCamera < u_cascadeSplits.z) return 2;
    return 3;
}

float SampleShadowPCF(float3 worldPos, int cascadeIndex)
{
    if (u_shadowParams.x < 0.5)
        return 1.0;

    const float4 lightSpace = mul(float4(worldPos, 1.0), u_cascadeViewProj[cascadeIndex]);
    const float3 ndc = lightSpace.xyz / max(lightSpace.w, 0.0001);
    const float2 uv = ndc.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z < 0.0 || ndc.z > 1.0)
        return 1.0;

    const float texelSize = 1.0 / max(u_shadowParams.y, 1.0);
    const float compareDepth = ndc.z - u_shadowParams.z;
    float lit = 0.0;
    [unroll]
    for (int x = -2; x <= 2; ++x)
    {
        [unroll]
        for (int y = -2; y <= 2; ++y)
        {
            lit += u_shadowTex.SampleCmpLevelZero(
                u_shadowSampler,
                float3(uv + float2((float)x, (float)y) * texelSize, (float)cascadeIndex),
                compareDepth);
        }
    }
    return lit / 25.0;
}

float SampleShadowBlended(float3 worldPos, float distanceToCamera)
{
    const int cascade = SelectShadowCascade(distanceToCamera);
    float shadow = SampleShadowPCF(worldPos, cascade);
    if (cascade < 3)
    {
        const float currentSplit = cascade == 0 ? u_cascadeSplits.x :
            (cascade == 1 ? u_cascadeSplits.y : u_cascadeSplits.z);
        const float previousSplit = cascade == 0 ? 0.0 :
            (cascade == 1 ? u_cascadeSplits.x : u_cascadeSplits.y);
        const float range = max(currentSplit - previousSplit, 0.001);
        const float blendStart = currentSplit - range * 0.1;
        if (distanceToCamera > blendStart)
        {
            const float t = saturate((distanceToCamera - blendStart) / max(currentSplit - blendStart, 0.001));
            shadow = lerp(shadow, SampleShadowPCF(worldPos, cascade + 1), t);
        }
    }
    return shadow;
}

float3x3 ComputeTbn(float3 worldPos, float2 uv, float3 surfaceNormal)
{
    const float3 posDx = ddx(worldPos);
    const float3 posDy = ddy(worldPos);
    const float2 uvDx = ddx(uv);
    const float2 uvDy = ddy(uv);
    float3 tangent = posDx * uvDy.y - posDy * uvDx.y;
    tangent = SafeNormalize(tangent, float3(1.0, 0.0, 0.0));
    const float3 bitangent = SafeNormalize(cross(surfaceNormal, tangent), float3(0.0, 0.0, 1.0));
    return float3x3(tangent, bitangent, surfaceNormal);
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

float CausticPattern(float2 uv, float time, float mode)
{
    if (mode < 1.5)
    {
        float2 a = uv + float2(time * 0.17, time * 0.09);
        float2 b = uv * 1.37 - float2(time * 0.11, time * 0.19);
        float lines = abs(sin(a.x * 6.2831 + sin(a.y * 4.0)) * cos(b.y * 6.2831 + sin(b.x * 3.0)));
        return pow(saturate(lines), 5.0);
    }

    float2 cell = frac(uv) - 0.5;
    float d = dot(cell, cell);
    float rings = 1.0 - smoothstep(0.04, 0.22, d);
    float veins = pow(saturate(abs(sin((uv.x + uv.y + time * 0.2) * 18.0))), 8.0);
    return saturate(max(rings, veins) * 0.85);
}

bool IsInsideWaterBodyBbox(float3 worldPos, TerrainWaterBodyUbo body)
{
    return body.levelModeEnabled.z > 0.5 &&
        worldPos.x >= body.bboxMinMax.x &&
        worldPos.x <= body.bboxMinMax.z &&
        worldPos.z >= body.bboxMinMax.y &&
        worldPos.z <= body.bboxMinMax.w;
}

float ApplyWaterEdgeCurve(float t, float curve)
{
    t = saturate(t);
    if (curve < 0.5)
        return t;
    if (curve < 1.5)
        return t * t * (3.0 - 2.0 * t);
    return t * t;
}

float WaterBodyBboxEdgeAlpha(float3 worldPos, TerrainWaterBodyUbo body)
{
    const float fadeDistance = max(body.edgeParams.x, 0.0);
    if (fadeDistance <= 0.0001)
        return 1.0;

    const float distToEdge = min(
        min(worldPos.x - body.bboxMinMax.x, body.bboxMinMax.z - worldPos.x),
        min(worldPos.z - body.bboxMinMax.y, body.bboxMinMax.w - worldPos.z));
    return ApplyWaterEdgeCurve(saturate(distToEdge / fadeDistance), body.edgeParams.y);
}

float4 PSMain(VSOutput input) : SV_Target0
{
    if (u_layerConstants.u_layerParams.z > 5.5)
    {
        if (u_layerConstants.u_layerParams.z > 7.5)
        {
            const float2 center = u_layerConstants.u_layerParams.xy;
            const float radius = max(u_layerConstants.u_layerParams.w, 0.001);
            const float dist = distance(input.localXZ, center);
            const float ring = 1.0 - smoothstep(radius * 0.92, radius, dist);
            const float hole = smoothstep(radius * 0.68, radius * 0.78, dist);
            const float alpha = saturate(ring * hole) * 0.78;
            const bool addMode = u_layerConstants.u_layerParams.z < 8.5;
            const float3 color = addMode ? float3(0.34, 0.82, 1.0) : float3(1.0, 0.18, 0.12);
            return float4(color, alpha);
        }
        if (u_layerConstants.u_layerParams.z > 6.5)
            return float4(1.0, 0.86, 0.05, 1.0);

        const float2 center = u_layerConstants.u_layerParams.xy;
        const float radius = max(u_layerConstants.u_layerParams.w, 0.001);
        const float dist = distance(input.localXZ, center);
        const float ring = 1.0 - smoothstep(radius * 0.92, radius, dist);
        const float hole = smoothstep(radius * 0.68, radius * 0.78, dist);
        const float alpha = saturate(ring * hole) * 0.72;
        return float4(1.0, 0.92, 0.15, alpha);
    }

    if (u_lightPadding.x > 0.5 && input.worldPos.y < u_lightPadding.y)
        discard;

    float4 splatA = u_splatATex.Sample(u_splatASampler, input.maskUv);
    float4 splatB = u_splatBTex.Sample(u_splatBSampler, input.maskUv);
    float weights[8] = {
        splatA.r, splatA.g, splatA.b, splatA.a,
        splatB.r, splatB.g, splatB.b, splatB.a
    };

    float total = 0.0;
    [unroll]
    for (int i = 0; i < 8; ++i)
        total += weights[i];

    if (total > 0.0001)
    {
        const float invTotal = 1.0 / total;
        [unroll]
        for (int i = 0; i < 8; ++i)
            weights[i] *= invTotal;
    }
    else
    {
        weights[0] = 1.0;
    }

    float3 albedo = 0.0.xxx;
    float3 normalTs = 0.0.xxx;
    float ao = 0.0;
    float roughness = 0.0;
    float metallic = 0.0;
    [unroll]
    for (int layer = 0; layer < 8; ++layer)
    {
        const float2 materialUv = input.texUv * u_materialTiling[layer].xy;
        const float3 diffuse = u_paletteTex.Sample(u_paletteSampler, float3(materialUv, (float)layer)).rgb;
        float3 sampledNormal = u_normalTex.Sample(u_normalSampler, float3(materialUv, (float)layer)).rgb * 2.0 - 1.0;
        sampledNormal.xy *= u_materialTintNormal[layer].w;
        sampledNormal = SafeNormalize(sampledNormal, float3(0.0, 0.0, 1.0));
        const float sampledAo = u_aoTex.Sample(u_aoSampler, float3(materialUv, (float)layer)).r;
        const float sampledRoughness = u_roughnessTex.Sample(u_roughnessSampler, float3(materialUv, (float)layer)).r;
        const float sampledMetallic = u_metallicTex.Sample(u_metallicSampler, float3(materialUv, (float)layer)).r;
        albedo += weights[layer] * diffuse * u_materialTintNormal[layer].rgb;
        normalTs += weights[layer] * sampledNormal;
        ao += weights[layer] * lerp(1.0, sampledAo, saturate(u_materialPbr[layer].x));
        roughness += weights[layer] * clamp(sampledRoughness * u_materialPbr[layer].y, 0.04, 1.0);
        metallic += weights[layer] * clamp(sampledMetallic * u_materialPbr[layer].z, 0.0, 1.0);
    }

    normalTs = SafeNormalize(normalTs, float3(0.0, 0.0, 1.0));
    float3 surfaceNormal = SafeNormalize(cross(ddx(input.worldPos), ddy(input.worldPos)), float3(0.0, 1.0, 0.0));
    if (surfaceNormal.y < 0.0)
        surfaceNormal = -surfaceNormal;
    const float3x3 tbn = ComputeTbn(input.worldPos, input.texUv, surfaceNormal);
    const float3 n = SafeNormalize(mul(normalTs, tbn), surfaceNormal);
    const float3 v = SafeNormalize(u_cameraPos.xyz - input.worldPos, float3(0.0, 1.0, 0.0));
    const float3 l = SafeNormalize(u_sunDir.xyz, float3(0.0, 1.0, 0.0));
    const float nDotV = max(dot(n, v), 0.001);

    roughness = clamp(roughness, 0.04, 1.0);
    metallic = saturate(metallic);
    ao = saturate(ao);

    const float3 f0 = lerp(0.04.xxx, albedo, metallic);

    const float shadowFactor = SampleShadowBlended(input.worldPos, length(u_cameraPos.xyz - input.worldPos));
    const float3 direct = EvaluateCookTorrance(albedo, n, v, nDotV, roughness, metallic, f0, l, u_sunColor.rgb) * shadowFactor;
    const float3 ambient = albedo * u_ambientColor.rgb * ao;
    float3 finalColor = ambient + direct;

    [loop]
    for (int p = 0; p < u_numPointLights; ++p)
    {
        PointLightUbo pointLight = u_pointLights[p];
        const float radius = max(pointLight.position.w, 0.1);
        const float3 lightVec = pointLight.position.xyz - input.worldPos;
        const float distanceToLight = length(lightVec);
        if (distanceToLight <= radius)
        {
            const float3 pointL = lightVec / max(distanceToLight, 0.001);
            const float attenuation = 1.0 - smoothstep(radius * 0.7, radius, distanceToLight);
            const float3 radiance = pointLight.color.rgb * pointLight.color.w * attenuation;
            finalColor += EvaluateCookTorrance(albedo, n, v, nDotV, roughness, metallic, f0, pointL, radiance);
        }
    }

    [loop]
    for (int s = 0; s < u_numSpotLights; ++s)
    {
        SpotLightUbo spotLight = u_spotLights[s];
        const float radius = max(spotLight.position.w, 0.1);
        const float3 lightVec = spotLight.position.xyz - input.worldPos;
        const float distanceToLight = length(lightVec);
        if (distanceToLight <= radius)
        {
            const float3 spotL = lightVec / max(distanceToLight, 0.001);
            const float3 spotDir = SafeNormalize(spotLight.direction.xyz, float3(0.0, -1.0, 0.0));
            const float coneDot = dot(-spotL, spotDir);
            if (coneDot >= spotLight.color.w)
            {
                const float coneFactor = smoothstep(spotLight.color.w, spotLight.direction.w, coneDot);
                const float attenuation = (1.0 - smoothstep(radius * 0.7, radius, distanceToLight)) * coneFactor;
                const float3 radiance = spotLight.color.rgb * attenuation;
                finalColor += EvaluateCookTorrance(albedo, n, v, nDotV, roughness, metallic, f0, spotL, radiance);
            }
        }
    }

    const float waterTime = u_waterGlobalParams.x;
    const int terrainWaterBodyCount = min((int)u_waterGlobalParams.y, 8);

    [loop]
    for (int w = 0; w < terrainWaterBodyCount; ++w)
    {
        TerrainWaterBodyUbo body = u_terrainWaterBodies[w];
        if (!IsInsideWaterBodyBbox(input.worldPos, body))
            continue;

        const float bodyWaterDepth = body.levelModeEnabled.x - input.worldPos.y;
        if (bodyWaterDepth <= 0.0)
            continue;

        const float waterEdgeAlpha = WaterBodyBboxEdgeAlpha(input.worldPos, body);
        const float waterFoamEdgeMask = smoothstep(0.5, 0.9, waterEdgeAlpha);

        if (body.levelModeEnabled.w > 0.5)
        {
            const float foamThickness = max(body.foamParams.x, 0.001);
            if (bodyWaterDepth < foamThickness)
            {
                const float time = waterTime * 0.02;
                const float foamScale = max(body.foamParams.w, 0.05);
                float2 foamUv0 = input.worldPos.xz / max(foamScale * 5.0, 0.5) + float2(time * 0.21, time * 0.13);
                float2 foamUv1 = input.worldPos.xz / max(foamScale * 11.0, 1.0) - float2(time * 0.17, time * 0.25);
                float terrainFoam = FoamNoise(foamUv0) * 0.6 + FoamNoise(foamUv1) * 0.4;
                terrainFoam = smoothstep(0.55, 0.82, terrainFoam);
                const float foamSoftness = max(body.foamParams.y, 0.001);
                const float shoreMask = 1.0 - smoothstep(0.0, foamThickness + foamSoftness, bodyWaterDepth);
                terrainFoam *= shoreMask * waterFoamEdgeMask * saturate(body.foamParams.z) * 0.12;
                finalColor = lerp(finalColor, float3(0.88, 0.94, 0.92), terrainFoam);
            }
        }

        const float causticMode = body.levelModeEnabled.y;
        const float causticMaxDepth = max(body.causticParams.w, 0.01);
        if (causticMode > 0.5 && bodyWaterDepth < causticMaxDepth)
        {
            const float time = waterTime * body.causticParams.z;
            const float causticScale = max(body.causticParams.y, 0.05);
            float2 causticUv = input.worldPos.xz / causticScale + float2(time * 0.15, -time * 0.09);
            const float caustic = CausticPattern(causticUv, time, causticMode);
            const float depthFade = 1.0 - smoothstep(0.0, causticMaxDepth, bodyWaterDepth);
            const float sunFactor = saturate(dot(n, SafeNormalize(u_sunDir.xyz, float3(0.0, 1.0, 0.0))));
            finalColor += u_sunColor.rgb * caustic * body.causticParams.x * depthFade * sunFactor * waterEdgeAlpha;
        }

        break;
    }

    return float4(max(finalColor, 0.0.xxx), 1.0);
}
