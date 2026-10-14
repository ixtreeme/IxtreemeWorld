// Reads one rendered scene pixel back without a renderer readback API.
[[vk::binding(0, 0)]] RWStructuredBuffer<float4> results : register(u0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] Texture2D<float4> sceneColor : register(t0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState sceneSampler : register(s0);

[numthreads(1, 1, 1)]
void SampleCS()
{
    results[0] = sceneColor.Load(int3(48, 32, 0));
}

// The whole scene image, row by row (the --compare mode).
[numthreads(8, 8, 1)]
void CopyCS(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    sceneColor.GetDimensions(width, height);
    if (id.x < width && id.y < height)
        results[id.y * width + id.x] = sceneColor.Load(int3(id.xy, 0));
}
