#pragma pack_matrix(row_major)

[[vk::binding(0, 0)]] cbuffer WarriorConstants : register(b0)
{
    float4x4 u_mvp;
    float4x4 u_model;
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
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.position = mul(float4(input.position, 1.0), u_mvp);
    output.normal = normalize(mul(float4(input.normal, 0.0), u_model).xyz);
    output.uv = input.uv;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    float3 normal = normalize(input.normal);
    float3 lightDir = normalize(float3(-0.35, 0.70, -0.55));
    float ndotl = saturate(dot(normal, lightDir));
    float3 baseColor = float3(0.62, 0.66, 0.70);
    float3 texColor = u_diffuse.Sample(u_sampler, input.uv).rgb;
    float3 color = texColor * baseColor * (0.24 + ndotl * 0.76);
    return float4(color, 1.0);
}
