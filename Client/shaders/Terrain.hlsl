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

[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] Texture2D u_layerTex : register(t0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState u_layerSampler : register(s0);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] Texture2D u_maskTex : register(t1);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] SamplerState u_maskSampler : register(s1);

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
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.position = mul(float4(input.position, 1.0), u_mvp);
    output.texUv = input.texUv * u_layerConstants.u_layerParams.xy;
    output.maskUv = input.maskUv;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    float3 color = u_layerTex.Sample(u_layerSampler, input.texUv).rgb;
    float mask = u_maskTex.Sample(u_maskSampler, input.maskUv).r;
    return float4(color, mask);
}
