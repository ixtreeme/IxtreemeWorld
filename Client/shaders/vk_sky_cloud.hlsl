#include "vk_common.hlsli"

[[vk::binding(0, 2)]] Texture2D g_diffuseTexture;
[[vk::binding(2, 2)]] SamplerState g_linearWrapSampler;

VSOutput VSMain(VSInputPCT input)
{
	VSOutput output;
	float4 worldPosition = mul(float4(input.position, 1.0f), g_world);
	float4 viewPosition = mul(worldPosition, g_view);
	output.position = mul(viewPosition, g_proj);
	output.color = input.color * g_objectColor;
	output.uv = mul(float4(input.uv, 0.0f, 1.0f), g_texTransform0).xy;
	output.fogFactor = 1.0f;
	return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
	float4 texel = g_diffuseTexture.Sample(g_linearWrapSampler, input.uv);
	return float4(texel.rgb * input.color.rgb, texel.a * input.color.a);
}
