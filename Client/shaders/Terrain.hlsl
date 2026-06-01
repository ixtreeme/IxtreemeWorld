#pragma pack_matrix(row_major)

[[vk::binding(0, 0)]] cbuffer TerrainConstants : register(b0)
{
    float4x4 u_mvp;
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
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.position = mul(float4(input.position, 1.0), u_mvp);
    output.texUv = input.texUv * u_layerConstants.u_layerParams.xy;
    output.maskUv = input.maskUv;
    output.localXZ = input.position.xz;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    if (u_layerConstants.u_layerParams.z > 5.5)
    {
        const float2 center = u_layerConstants.u_layerParams.xy;
        const float radius = max(u_layerConstants.u_layerParams.w, 0.001);
        const float dist = distance(input.localXZ, center);
        const float ring = 1.0 - smoothstep(radius * 0.92, radius, dist);
        const float hole = smoothstep(radius * 0.68, radius * 0.78, dist);
        const float alpha = saturate(ring * hole) * 0.72;
        return float4(1.0, 0.92, 0.15, alpha);
    }
    if (u_layerConstants.u_layerParams.z > 4.5)
        return float4(u_layerConstants.u_layerParams.x,
                      u_layerConstants.u_layerParams.y,
                      u_layerConstants.u_layerParams.w,
                      0.68);
    if (u_layerConstants.u_layerParams.z > 3.5)
        return float4(u_layerConstants.u_layerParams.x,
                      u_layerConstants.u_layerParams.y,
                      u_layerConstants.u_layerParams.w,
                      0.16);
    if (u_layerConstants.u_layerParams.z > 2.5)
        return float4(0.1, 0.55, 1.0, 0.35);
    if (u_layerConstants.u_layerParams.z > 1.5)
        return float4(0.0, 1.0, 0.25, 0.38);
    if (u_layerConstants.u_layerParams.z > 0.5)
        return float4(1.0, 0.08, 0.04, 0.42);

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

    float3 color = 0.0.xxx;
    [unroll]
    for (int layer = 0; layer < 8; ++layer)
        color += weights[layer] * u_paletteTex.Sample(u_paletteSampler, float3(input.texUv, (float)layer)).rgb;

    return float4(color, 1.0);
}
