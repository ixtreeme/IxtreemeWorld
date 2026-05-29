#include "vk_common.hlsli"

[[vk::binding(0, 2)]] Texture2D g_diffuseTexture;
[[vk::binding(2, 2)]] SamplerState g_linearClampSampler;

struct VSInputPT
{
	float3 position : POSITION;
	float2 uv : TEXCOORD0;
};

VSOutput VSMain(VSInputPT input)
{
	VSOutput output;
	float4 worldPosition = mul(float4(input.position, 1.0f), g_world);
	float4 viewPosition = mul(worldPosition, g_view);
	output.position = mul(viewPosition, g_proj);
	output.color = g_diffuseColor;
	output.uv = input.uv;
	output.fogFactor = ComputeFogFactor(viewPosition.xyz);
	return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
	float4 color = g_diffuseTexture.Sample(g_linearClampSampler, input.uv) * input.color;
	color.rgb = ApplyFog(color.rgb, input.fogFactor);
	return color;
}
