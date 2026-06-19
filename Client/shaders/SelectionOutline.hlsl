#pragma pack_matrix(row_major)

[[vk::binding(0, 0)]] cbuffer SelectionOutlineConstants : register(b0)
{
    float4x4 u_mvp;
};

struct VSInput
{
    float3 position : POSITION;
    float4 color : COLOR0;
};

struct VSOutput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.position = mul(float4(input.position, 1.0), u_mvp);
    output.color = input.color;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
    return input.color;
}
