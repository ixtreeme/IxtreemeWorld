#pragma pack_matrix(row_major)

[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2D u_sceneColor : register(t0);
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState u_sceneSampler : register(s0);

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    VSOutput output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(output.uv * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    float3 hdrColor = u_sceneColor.Sample(u_sceneSampler, input.uv).rgb;
    float3 ldrColor = hdrColor / (hdrColor + 1.0.xxx);
    return float4(saturate(ldrColor), 1.0);
}
