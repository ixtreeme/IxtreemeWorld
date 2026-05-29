#include "vk_common.hlsli"

[[vk::binding(0, 2)]] Texture2D g_fontAtlas;
[[vk::binding(2, 2)]] SamplerState g_linearClampSampler;

VSOutput VSMain(VSInputPCT input)
{
	VSOutput output;
	float4 worldPosition = mul(float4(input.position, 1.0f), g_world);
	float4 viewPosition = mul(worldPosition, g_view);
	output.position = mul(viewPosition, g_proj);
	output.color = input.color * g_objectColor;
	output.uv = input.uv;
	output.fogFactor = 1.0f;
	return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
	float alpha = g_fontAtlas.Sample(g_linearClampSampler, input.uv).r;
	return float4(input.color.rgb, input.color.a * alpha);
}
