[[vk::binding(0, 0)]] RWStructuredBuffer<float> result;
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] Texture2D source;
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState sourceSampler;
struct ReadParams { uint offset; };
[[vk::push_constant]] ReadParams u_read;

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    result[u_read.offset + id.y * 64 + id.x] = source.SampleLevel(sourceSampler, (id.xy + 0.5) / 64.0, 0).r;
}
