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
    // Terrain between the sun and the cascade's depth range still throws its shadow into it (a ridge
    // far towards a low sun): flattened onto the near plane instead of clipped. The projection is
    // orthographic (w = 1).
    output.position.z = max(output.position.z, 0.0);
    return output;
}
