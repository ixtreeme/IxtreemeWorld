#pragma pack_matrix(row_major)

// Instance-ready notes for the future N>1 path:
// - Store bone palettes as [instance][bone] and add a paletteBase = instanceIndex * boneCount.
// - Dispatch can use SV_DispatchThreadID.z as the instance index, or scale X by vertexCount per instance.
// - Per-instance model matrices remain outside this shader; graphics should fetch/apply them separately.

struct SkinConstants
{
    uint vertexCount;
    uint boneCount;
};

[[vk::push_constant]] SkinConstants g_constants;

struct RestVertex
{
    float4 position;
    uint packedWeights;
    uint packedBones;
    uint pad0;
    uint pad1;
    float4 normal;
    float4 uv;
};

struct SkinnedVertexPacked
{
    float4 position_normalX;
    float4 normalYZ_uv;
};

[[vk::binding(0, 0)]] StructuredBuffer<RestVertex> g_restVertices : register(t0);
[[vk::binding(1, 0)]] StructuredBuffer<float4x4> g_bonePalette : register(t1);
[[vk::binding(2, 0)]] RWStructuredBuffer<SkinnedVertexPacked> g_skinnedVertices : register(u0);

uint UnpackByte(uint packedValue, uint index)
{
    return (packedValue >> (index * 8u)) & 0xffu;
}

float3 SafeNormalize(float3 value)
{
    float lenSq = dot(value, value);
    if (lenSq <= 0.000000000001)
        return value;
    return value * rsqrt(lenSq);
}

[numthreads(64, 1, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint vertexIndex = dispatchThreadId.x;
    if (vertexIndex >= g_constants.vertexCount)
        return;

    RestVertex source = g_restVertices[vertexIndex];
    uint totalWeight =
        UnpackByte(source.packedWeights, 0) +
        UnpackByte(source.packedWeights, 1) +
        UnpackByte(source.packedWeights, 2) +
        UnpackByte(source.packedWeights, 3);

    float3 outputPosition = source.position.xyz;
    float3 outputNormal = SafeNormalize(source.normal.xyz);
    float2 outputUv = source.uv.xy;

    if (totalWeight > 0)
    {
        float3 skinnedPosition = float3(0.0, 0.0, 0.0);
        float3 skinnedNormal = float3(0.0, 0.0, 0.0);

        [unroll]
        for (uint influence = 0; influence < 4; ++influence)
        {
            uint weightByte = UnpackByte(source.packedWeights, influence);
            uint boneIndex = UnpackByte(source.packedBones, influence);
            if (weightByte == 0 || boneIndex >= g_constants.boneCount)
                continue;

            float weight = ((float)weightByte) / ((float)totalWeight);
            float4x4 bone = g_bonePalette[boneIndex];
            skinnedPosition += mul(float4(source.position.xyz, 1.0), bone).xyz * weight;
            skinnedNormal += mul(float4(source.normal.xyz, 0.0), bone).xyz * weight;
        }

        outputPosition = skinnedPosition;
        outputNormal = SafeNormalize(skinnedNormal);
    }

    SkinnedVertexPacked output;
    output.position_normalX = float4(outputPosition, outputNormal.x);
    output.normalYZ_uv = float4(outputNormal.yz, outputUv);
    g_skinnedVertices[vertexIndex] = output;
}
