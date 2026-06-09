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

[[vk::binding(0, 0)]] cbuffer SkinnedMeshConstants : register(b0)
{
    float4x4 u_mvp;
    float4x4 u_model;
    float4 u_tint;
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
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.position = mul(float4(input.position, 1.0), u_mvp);
    const float4 worldPos = mul(float4(input.position, 1.0), u_model);
    output.normal = normalize(mul(float4(input.normal, 0.0), u_model).xyz);
    output.uv = input.uv;
    output.worldPos = worldPos.xyz;
    return output;
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

float4 PSMain(VSOutput input) : SV_Target0
{
    if (u_lightPadding.x > 0.5 && input.worldPos.y < u_lightPadding.y)
        discard;

    float3 normal = normalize(input.normal);
    float3 lightDir = normalize(u_sunDir.xyz);
    float ndotl = saturate(dot(normal, lightDir));
    float3 baseColor = float3(0.62, 0.66, 0.70);
    float3 texColor = u_diffuse.Sample(u_sampler, input.uv).rgb;
    float3 lighting = u_ambientColor.rgb + u_sunColor.rgb * ndotl;
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
            lighting += pointLight.color.rgb * pointLight.color.w * saturate(dot(normal, pointL)) * attenuation;
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
            }
        }
    }
    const float waterDepth = u_waterParams.y - input.worldPos.y;
    if (u_waterParams.x > 0.5 && u_waterParams.z > 0.5 && waterDepth > 0.0 && waterDepth < u_causticParams.z)
    {
        const float causticScale = max(u_causticParams.x, 0.05);
        const float time = u_causticParams.y;
        float2 causticUv = input.worldPos.xz / causticScale + float2(time * 0.15, -time * 0.09);
        const float caustic = CausticPattern(causticUv, time, u_waterParams.z);
        const float depthFade = 1.0 - smoothstep(0.0, max(u_causticParams.z, 0.01), waterDepth);
        const float sunFactor = saturate(dot(normal, normalize(u_sunDir.xyz)));
        lighting += u_sunColor.rgb * caustic * u_waterParams.w * depthFade * sunFactor;
    }
    float3 color = texColor * baseColor * u_tint.rgb * lighting;
    return float4(color, 1.0);
}
