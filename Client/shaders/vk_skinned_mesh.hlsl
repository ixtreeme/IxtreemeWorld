#include "vk_common.hlsli"

// NOTE: bumped 64 -> 128 bones for current humanoid skeletons,
// so the original DX11 array of 64 would overflow / clamp. 128 * 64 bytes =
// 8 KB, still well under the 16 KB guaranteed maxUniformBufferRange minimum.
// If you ever exceed ~256 bones, switch this to a StructuredBuffer (SSBO).
[[vk::binding(3, 1)]]
cbuffer SkinConstants
{
	row_major float4x4 g_bones[128];
};

[[vk::binding(0, 2)]] Texture2D g_diffuseTexture;
[[vk::binding(2, 2)]] SamplerState g_linearWrapSampler;

struct VSInputSkinned
{
	float3 position : POSITION;
	float3 normal : NORMAL;
	float2 uv : TEXCOORD0;
	uint4 blendIndices : BLENDINDICES0;
	float4 blendWeights : BLENDWEIGHT0;
};

VSOutput VSMain(VSInputSkinned input)
{
	float4 localPosition = float4(input.position, 1.0f);
	float4 skinnedPosition =
		mul(localPosition, g_bones[input.blendIndices.x]) * input.blendWeights.x +
		mul(localPosition, g_bones[input.blendIndices.y]) * input.blendWeights.y +
		mul(localPosition, g_bones[input.blendIndices.z]) * input.blendWeights.z +
		mul(localPosition, g_bones[input.blendIndices.w]) * input.blendWeights.w;

	float3 localNormal = input.normal;
	float3 skinnedNormal =
		mul(float4(localNormal, 0.0f), g_bones[input.blendIndices.x]).xyz * input.blendWeights.x +
		mul(float4(localNormal, 0.0f), g_bones[input.blendIndices.y]).xyz * input.blendWeights.y +
		mul(float4(localNormal, 0.0f), g_bones[input.blendIndices.z]).xyz * input.blendWeights.z +
		mul(float4(localNormal, 0.0f), g_bones[input.blendIndices.w]).xyz * input.blendWeights.w;

	VSOutput output;
	float4 worldPosition = mul(skinnedPosition, g_world);
	float4 viewPosition = mul(worldPosition, g_view);
	output.position = mul(viewPosition, g_proj);
	output.color = float4(ComputeLambert(mul(float4(normalize(skinnedNormal), 0.0f), g_world).xyz), 1.0f) * g_diffuseColor;
	output.uv = input.uv;
	output.fogFactor = ComputeFogFactor(viewPosition.xyz);
	return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
	float4 color = g_diffuseTexture.Sample(g_linearWrapSampler, input.uv) * input.color;
	color.rgb = ApplyFog(color.rgb, input.fogFactor);
	return color;
}
