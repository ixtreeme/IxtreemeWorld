#pragma pack_matrix(row_major)

[[vk::binding(0, 0)]] cbuffer WorldLabelConstants : register(b0)
{
    float4x4 u_mvp;
};

[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] Texture2D u_fontAtlas : register(t0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState u_fontSampler : register(s0);

struct VSInput
{
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.position = mul(float4(input.position, 1.0), u_mvp);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    float alpha = u_fontAtlas.Sample(u_fontSampler, input.uv).a;
    return float4(input.color.rgb, input.color.a * alpha);
}
