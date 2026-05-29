#include "vk_common.hlsli"

[[vk::binding(0, 2)]] Texture2D g_diffuseTexture;
[[vk::binding(2, 2)]] SamplerState g_linearWrapSampler;

VSOutput VSMain(VSInputPNT input)
{
	VSOutput output;
	float4 worldPosition = mul(float4(input.position, 1.0f), g_world);
	float4 viewPosition = mul(worldPosition, g_view);
	float3 worldNormal = mul(float4(input.normal, 0.0f), g_world).xyz;

	output.position = mul(viewPosition, g_proj);
	output.color = float4(ComputeLambert(worldNormal), 1.0f) * g_diffuseColor;
	output.uv = input.uv;
	output.fogFactor = ComputeFogFactor(viewPosition.xyz);
	return output;
}

VSOutput VSMainPNT2(VSInputPNT2 input)
{
	VSOutput output;
	float4 worldPosition = mul(float4(input.position, 1.0f), g_world);
	float4 viewPosition = mul(worldPosition, g_view);
	float3 worldNormal = mul(float4(input.normal, 0.0f), g_world).xyz;

	output.position = mul(viewPosition, g_proj);
	output.color = float4(ComputeLambert(worldNormal), 1.0f) * g_diffuseColor;
	output.uv = input.uv;
	output.fogFactor = ComputeFogFactor(viewPosition.xyz);
	return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
	float4 color = g_diffuseTexture.Sample(g_linearWrapSampler, input.uv) * input.color;
	clip(color.a - 0.5f);
	color.rgb = ApplyFog(color.rgb, input.fogFactor);
	return color;
}
