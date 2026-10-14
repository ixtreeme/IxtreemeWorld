#ifndef IX_STATIC_MESH_COMMON_HLSLI
#define IX_STATIC_MESH_COMMON_HLSLI

// Shared by StaticMesh.hlsl and the pixel shaders compiled from material graphs: the static mesh
// descriptor set 0, the vertex output and the lit shading. A graph shader embeds this file, so a
// change here changes the layout every compiled graph material is drawn with.

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

[[vk::binding(0, 0)]] cbuffer StaticMeshConstants : register(b0)
{
    float4x4 u_mvp;
    float4x4 u_model;
    float4 u_tint;
    float4 u_materialBaseColor;
    float4 u_materialParams; // metallic, roughness, normal strength, AO strength
    float4 u_materialEmissive; // rgb, intensity
    float4 u_materialUv; // tiling.xy, offset.xy
    float4 u_materialAlpha; // mode: 0 opaque, 1 mask, 2 blend; cutoff; coverage min/max
    float4 u_cameraPosition;
    float4 u_sunDir;
    float4 u_sunColor;
    float4 u_ambientColor;
    float4 u_waterParams;
    float4 u_causticParams;
    int u_numPointLights;
    int u_numSpotLights;
    float2 u_lightPadding;
    PointLightUbo u_pointLights[16];
    SpotLightUbo u_spotLights[16];
    float4x4 u_shadowCascadeViewProj[4];
    float4 u_shadowParams;
    float4 u_shadowDepthBias;
    float4 u_shadowNormalOffset;
};

[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] Texture2D u_diffuse : register(t0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState u_sampler : register(s0);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] Texture2D u_normalMap : register(t1);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] SamplerState u_normalSampler : register(s1);
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] Texture2D u_ormMap : register(t2);
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] SamplerState u_ormSampler : register(s2);

struct StaticMeshInstanceData
{
    float4x4 mvp;
    float4x4 model;
    float4 tint;
    float4 materialBaseColor;
    float4 materialParams;
    float4 materialEmissive;
    float4 materialUv;
    float4 materialAlpha;
};

[[vk::binding(4, 0)]] StructuredBuffer<StaticMeshInstanceData> u_instances : register(t3);

[[vk::combinedImageSampler]] [[vk::binding(5, 0)]] Texture2DArray<float> u_sunShadowMap : register(t4);
[[vk::combinedImageSampler]] [[vk::binding(5, 0)]] SamplerComparisonState u_sunShadowSampler : register(s4);
// The material's emissive map (sRGB view; white when it has none), multiplied by its emissive colour.
[[vk::combinedImageSampler]] [[vk::binding(6, 0)]] Texture2D u_emissiveMap : register(t5);
[[vk::combinedImageSampler]] [[vk::binding(6, 0)]] SamplerState u_emissiveSampler : register(s5);

#include "SunShadow.hlsli"

struct VSOutput
{
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
    nointerpolation float4 tint : TEXCOORD2;
    nointerpolation float4 materialBaseColor : TEXCOORD3;
    nointerpolation float4 materialParams : TEXCOORD4;
    nointerpolation float4 materialEmissive : TEXCOORD5;
    nointerpolation float4 materialAlpha : TEXCOORD6;
};

// The sun shadow pass of alpha-tested (mask) materials: their cut-out parts cast no shadow.
void ApplyLodCoverage(VSOutput input)
{
    if (input.materialAlpha.z > 0.0 || input.materialAlpha.w < 1.0)
    {
        // Complementary intervals keep mesh/view transitions stable without blending or sorting.
        float noise = frac(52.9829189 * frac(dot(floor(input.position.xy), float2(0.06711056, 0.00583715))));
        if (noise < input.materialAlpha.z || noise >= input.materialAlpha.w) discard;
    }
}

float SpecularTerm(float3 normal, float3 lightDir, float3 viewDir, float roughness, float metallic)
{
    const float3 halfDir = normalize(lightDir + viewDir);
    const float ndoth = saturate(dot(normal, halfDir));
    const float power = lerp(160.0, 8.0, saturate(roughness));
    const float fresnel = lerp(0.04, 0.9, saturate(metallic));
    return pow(ndoth, power) * fresnel * (1.0 - saturate(roughness) * 0.75);
}

// A unit tangent-space normal (z out of the surface) onto the surface, with the tangent frame
// derived from screen-space position and UV derivatives.
float3 PerturbNormal(float3 vertexNormal, float3 worldPos, float2 uv, float3 tangentNormal)
{
    float3 n = normalize(vertexNormal);
    float3 dp1 = ddx(worldPos);
    float3 dp2 = ddy(worldPos);
    float2 duv1 = ddx(uv);
    float2 duv2 = ddy(uv);
    float3 tangent = dp1 * duv2.y - dp2 * duv1.y;
    // Atlas UVs are small: retain their tangent basis at distant-tree screen sizes.
    if (dot(tangent, tangent) < 0.000000000001)
        return n;
    tangent = normalize(tangent);
    float3 bitangent = normalize(cross(n, tangent));
    return normalize(tangent * tangentNormal.x + bitangent * tangentNormal.y + n * tangentNormal.z);
}

// Sun, ambient, point and spot lighting of an opaque lit surface; emissive light is added by the caller.
float3 StaticMeshLighting(float3 albedo, float3 normal, float3 geometricNormal, float3 worldPos,
    float roughness, float metallic, float ao)
{
    float3 viewDir = normalize(u_cameraPosition.xyz - worldPos);
    float3 lightDir = normalize(u_sunDir.xyz);
    float ndotl = saturate(dot(normal, lightDir));
    const float sunShadow = SunShadow(worldPos, geometricNormal);
    float3 lighting = u_ambientColor.rgb * ao + u_sunColor.rgb * ndotl * sunShadow;
    float3 specular = u_sunColor.rgb * SpecularTerm(normal, lightDir, viewDir, roughness, metallic) * sunShadow;

    [loop]
    for (int p = 0; p < u_numPointLights; ++p)
    {
        PointLightUbo pointLight = u_pointLights[p];
        const float radius = max(pointLight.position.w, 0.1);
        const float3 lightVec = pointLight.position.xyz - worldPos;
        const float distanceToLight = length(lightVec);
        if (distanceToLight <= radius)
        {
            const float3 pointL = lightVec / max(distanceToLight, 0.001);
            const float attenuation = 1.0 - smoothstep(radius * 0.7, radius, distanceToLight);
            const float nDotPoint = saturate(dot(normal, pointL));
            lighting += pointLight.color.rgb * pointLight.color.w * nDotPoint * attenuation;
            specular += pointLight.color.rgb * pointLight.color.w *
                SpecularTerm(normal, pointL, viewDir, roughness, metallic) * attenuation;
        }
    }

    [loop]
    for (int s = 0; s < u_numSpotLights; ++s)
    {
        SpotLightUbo spotLight = u_spotLights[s];
        const float radius = max(spotLight.position.w, 0.1);
        const float3 lightVec = spotLight.position.xyz - worldPos;
        const float distanceToLight = length(lightVec);
        if (distanceToLight <= radius)
        {
            const float3 spotL = lightVec / max(distanceToLight, 0.001);
            const float3 spotDir = normalize(spotLight.direction.xyz);
            const float coneDot = dot(-spotL, spotDir);
            if (coneDot >= spotLight.color.w)
            {
                const float attenuation =
                    (1.0 - smoothstep(radius * 0.7, radius, distanceToLight)) *
                    smoothstep(spotLight.color.w, spotLight.direction.w, coneDot);
                lighting += spotLight.color.rgb * saturate(dot(normal, spotL)) * attenuation;
                specular += spotLight.color.rgb *
                    SpecularTerm(normal, spotL, viewDir, roughness, metallic) * attenuation;
            }
        }
    }
    return albedo * lighting + specular;
}

#endif
