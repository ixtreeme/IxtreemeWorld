#pragma pack_matrix(row_major)
#include "Fog.hlsli"

[[vk::binding(0, 0)]] ConstantBuffer<FogUniforms> fog : register(b0);
[[vk::binding(1, 0)]] StructuredBuffer<float4> queries : register(t0);
[[vk::binding(2, 0)]] RWStructuredBuffer<float4> results : register(u0);

[numthreads(1, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const float4 query = queries[id.x * 2];
    results[id.x] = EvaluateFog(fog, query.xyz, query.w, queries[id.x * 2 + 1].x > 0.0);
}

// Read actual composite pixels back without introducing a renderer readback API.
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] Texture2D<float4> display : register(t1);
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] SamplerState displaySampler : register(s0);
[numthreads(1, 1, 1)]
void SampleCS()
{
    results[0] = display.Load(int3(48, 32, 0));
}
