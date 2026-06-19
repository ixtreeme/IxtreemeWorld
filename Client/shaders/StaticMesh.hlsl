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
    float4 u_materialAlpha; // mode: 0 opaque, 1 mask, 2 blend; cutoff
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

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

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

VSOutput VSMain(VSInput input, uint instanceId : SV_InstanceID)
{
    StaticMeshInstanceData instance = u_instances[instanceId];
    VSOutput output;
    float3 localPosition = input.position;
#if defined(STATIC_MESH_OUTLINE)
    const float normalLenSq = dot(input.normal, input.normal);
    const float3 outlineNormal = normalLenSq > 0.000001 ? input.normal * rsqrt(normalLenSq) : float3(0.0, 1.0, 0.0);
    localPosition += outlineNormal * 0.045;
#endif
    output.position = mul(float4(localPosition, 1.0), instance.mvp);
    const float4 worldPos = mul(float4(localPosition, 1.0), instance.model);
    output.normal = normalize(mul(float4(input.normal, 0.0), instance.model).xyz);
    output.uv = input.uv * max(instance.materialUv.xy, float2(0.001, 0.001)) + instance.materialUv.zw;
    output.worldPos = worldPos.xyz;
    output.tint = instance.tint;
    output.materialBaseColor = instance.materialBaseColor;
    output.materialParams = instance.materialParams;
    output.materialEmissive = instance.materialEmissive;
    output.materialAlpha = instance.materialAlpha;
    return output;
}

float SpecularTerm(float3 normal, float3 lightDir, float3 viewDir, float roughness, float metallic)
{
    const float3 halfDir = normalize(lightDir + viewDir);
    const float ndoth = saturate(dot(normal, halfDir));
    const float power = lerp(160.0, 8.0, saturate(roughness));
    const float fresnel = lerp(0.04, 0.9, saturate(metallic));
    return pow(ndoth, power) * fresnel * (1.0 - saturate(roughness) * 0.75);
}

float3 ApplyNormalMap(float3 vertexNormal, float3 worldPos, float2 uv, float strength)
{
    float3 n = normalize(vertexNormal);
    float3 mapNormal = u_normalMap.Sample(u_normalSampler, uv).xyz * 2.0 - 1.0;
    mapNormal.xy *= max(strength, 0.0);
    mapNormal = normalize(mapNormal);

    float3 dp1 = ddx(worldPos);
    float3 dp2 = ddy(worldPos);
    float2 duv1 = ddx(uv);
    float2 duv2 = ddy(uv);
    float3 tangent = dp1 * duv2.y - dp2 * duv1.y;
    if (dot(tangent, tangent) < 0.000001)
        return n;
    tangent = normalize(tangent);
    float3 bitangent = normalize(cross(n, tangent));
    return normalize(tangent * mapNormal.x + bitangent * mapNormal.y + n * mapNormal.z);
}

float4 PSMain(VSOutput input) : SV_Target0
{
#if defined(STATIC_MESH_OUTLINE)
    return float4(1.0, 0.78, 0.18, 1.0);
#else
    if (u_lightPadding.x > 0.5 && input.worldPos.y < u_lightPadding.y)
        discard;

    float4 texSample = u_diffuse.Sample(u_sampler, input.uv);
    const float alpha = texSample.a * input.materialBaseColor.a * input.tint.a;
    if (input.materialAlpha.x > 0.5 && input.materialAlpha.x < 1.5 && alpha < input.materialAlpha.y)
        discard;

    float3 texColor = texSample.rgb;
    float3 albedo = texColor * input.materialBaseColor.rgb * input.tint.rgb;

#if defined(SHADING_MODE_UNLIT)
    float3 unlitColor = albedo + input.materialEmissive.rgb * input.materialEmissive.a;
    return float4(unlitColor, alpha);
#else
    float3 normal = ApplyNormalMap(normalize(input.normal), input.worldPos, input.uv, input.materialParams.z);
    float3 viewDir = normalize(u_cameraPosition.xyz - input.worldPos);
    float3 lightDir = normalize(u_sunDir.xyz);
    float ndotl = saturate(dot(normal, lightDir));

    float3 orm = u_ormMap.Sample(u_ormSampler, input.uv).rgb;
    const float metallic = saturate(input.materialParams.x * orm.b);
    const float roughness = saturate(input.materialParams.y * max(orm.g, 0.04));
    const float ao = saturate(input.materialParams.w * orm.r);
    float3 lighting = u_ambientColor.rgb * ao + u_sunColor.rgb * ndotl;
    float3 specular = u_sunColor.rgb * SpecularTerm(normal, lightDir, viewDir, roughness, metallic);

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
        const float3 lightVec = spotLight.position.xyz - input.worldPos;
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

    float3 color = albedo * lighting + specular + input.materialEmissive.rgb * input.materialEmissive.a;
    return float4(color, alpha);
#endif
#endif
}
