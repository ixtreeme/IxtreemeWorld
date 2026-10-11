[[vk::binding(0, 0)]] RWStructuredBuffer<float4> pixels;
[[vk::binding(1, 0)]] Texture2D<float4> image;
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint width, height; image.GetDimensions(width, height);
    if (id.x < width && id.y < height) pixels[id.y * width + id.x] = image.Load(int3(id.xy, 0));
}
