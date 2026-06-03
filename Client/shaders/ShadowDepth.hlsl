#pragma pack_matrix(row_major)

struct ShadowPush
{
    float4x4 u_lightViewProj;
};

[[vk::push_constant]] ShadowPush u_shadow;

struct VSInput
{
    float3 position : POSITION;
    float2 texUv : TEXCOORD0;
    float2 maskUv : TEXCOORD1;
};

struct VSOutput
{
    float4 position : SV_Position;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.position = mul(float4(input.position, 1.0), u_shadow.u_lightViewProj);
    return output;
}
