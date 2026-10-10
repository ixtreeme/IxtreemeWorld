#pragma pack_matrix(row_major)

[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2D<float> depthImage;
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState depthSampler;
[[vk::binding(1, 0)]] RWStructuredBuffer<float> pyramid;
struct Bounds { float4 minimum; float4 maximum; };
[[vk::binding(2, 0)]] StructuredBuffer<Bounds> bounds;
struct Seed { uint indices, instances, firstIndex; int vertexOffset;
              uint firstInstance, group, padding0, padding1; };
// Element zero is a header: indices = group count, instances = command count.
[[vk::binding(3, 0)]] StructuredBuffer<Seed> seeds;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> visible;
struct IndexedCommand { uint indices, instances, firstIndex; int vertexOffset; uint firstInstance; };
[[vk::binding(5, 0)]] RWStructuredBuffer<IndexedCommand> commands;
struct Push
{
    float4x4 vp;
    uint width, height, baseWidth, baseHeight;
    uint inWidth, inHeight, inOffset, outOffset;
};
[[vk::push_constant]] Push push;

groupshared float tile[128];
[numthreads(8, 16, 1)]
void BaseCS(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID, uint lane : SV_GroupIndex)
{
    uint2 pixel = group.xy * 16 + thread.xy;
    float d = pixel.x < push.width && pixel.y < push.height ? depthImage.Load(int3(pixel, 0)) : 1.0;
    pixel.x += 8;
    float e = pixel.x < push.width && pixel.y < push.height ? depthImage.Load(int3(pixel, 0)) : 1.0;
    tile[lane] = max(isfinite(d) ? d : 1.0, isfinite(e) ? e : 1.0);
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 64; stride > 0; stride >>= 1)
    {
        if (lane < stride) tile[lane] = max(tile[lane], tile[lane + stride]);
        GroupMemoryBarrierWithGroupSync();
    }
    if (lane == 0) pyramid[group.y * push.baseWidth + group.x] = tile[0];
}

[numthreads(8, 8, 1)]
void ReduceCS(uint3 thread : SV_DispatchThreadID)
{
    uint2 targetSize = uint2((push.inWidth + 1) / 2, (push.inHeight + 1) / 2);
    if (any(thread.xy >= targetSize)) return;
    float d = 0;
    for (uint y = 0; y < 2; ++y)
    for (uint x = 0; x < 2; ++x)
    {
        uint2 source = thread.xy * 2 + uint2(x, y);
        if (source.x < push.inWidth && source.y < push.inHeight)
            d = max(d, pyramid[push.inOffset + source.y * push.inWidth + source.x]);
    }
    pyramid[push.outOffset + thread.y * targetSize.x + thread.x] = d;
}

[numthreads(64, 1, 1)]
void TestCS(uint3 thread : SV_DispatchThreadID)
{
    uint id = thread.x;
    if (id >= seeds[0].indices) return;
    visible[id] = 1;
    Bounds box = bounds[id];
    if (!all(isfinite(box.minimum.xyz)) || !all(isfinite(box.maximum.xyz)) ||
        any(box.minimum.xyz > box.maximum.xyz)) return;
    float2 lo = float2(1, 1), hi = float2(0, 0);
    float nearest = 1;
    for (uint corner = 0; corner < 8; ++corner)
    {
        float3 p = float3(corner & 1 ? box.maximum.x : box.minimum.x,
                          corner & 2 ? box.maximum.y : box.minimum.y,
                          corner & 4 ? box.maximum.z : box.minimum.z);
        float4 clip = mul(float4(p, 1), push.vp);
        if (!all(isfinite(clip)) || clip.w <= 0.0001) return;
        float3 ndc = clip.xyz / clip.w;
        if (any(abs(ndc.xy) >= 1) || ndc.z <= 0 || ndc.z >= 1) return;
        // Positive Vulkan viewport push.height; the projection already flips Y.
        float2 uv = ndc.xy * 0.5 + 0.5;
        lo = min(lo, uv); hi = max(hi, uv); nearest = min(nearest, ndc.z);
    }
    float2 lowPixel = max(float2(0, 0), floor(lo * float2(push.width, push.height)) - 2);
    float2 highPixel = min(float2(push.width - 1, push.height - 1), ceil(hi * float2(push.width, push.height)) + 2);
    uint2 low = (uint2)lowPixel / 16, high = (uint2)highPixel / 16;
    uint2 size = uint2(push.baseWidth, push.baseHeight);
    uint offset = 0;
    while (max(high.x - low.x, high.y - low.y) > 3 && any(size > 1))
    {
        offset += size.x * size.y;
        size = (size + 1) / 2;
        low /= 2; high /= 2;
    }
    float farthest = 0;
    for (uint y = low.y; y <= high.y; ++y)
    for (uint x = low.x; x <= high.x; ++x)
        farthest = max(farthest, pyramid[offset + y * size.x + x]);
    // Max reduction includes EVERY covered pixel. Holes (clear depth 1) keep
    // the group visible; strict bias also prevents self-occlusion/roundoff.
    visible[id] = farthest + 0.00002 < nearest ? 0 : 1;
}

[numthreads(64, 1, 1)]
void CommandCS(uint3 thread : SV_DispatchThreadID)
{
    uint id = thread.x;
    if (id >= seeds[0].instances) return;
    Seed seed = seeds[id + 1];
    IndexedCommand command;
    command.indices = seed.indices;
    command.instances = seed.group < seeds[0].indices && visible[seed.group] == 0 ? 0 : seed.instances;
    command.firstIndex = seed.firstIndex;
    command.vertexOffset = seed.vertexOffset;
    command.firstInstance = seed.firstInstance;
    commands[id] = command;
}
