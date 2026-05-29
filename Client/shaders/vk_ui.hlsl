#include "vk_common.hlsli"

[[vk::binding(0, 2)]] Texture2D g_diffuseTexture;
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
	float4 texel = g_diffuseTexture.Sample(g_linearClampSampler, input.uv);

	float alpha = texel.a;
	float magentaKey = step(0.99f, texel.r) * step(0.99f, texel.b) * (1.0f - step(0.05f, texel.g));

	if (magentaKey > 0.5f)
		alpha = 0.0f;

	// Preserve the original UI texture colors exactly and respect authored alpha.
	// This avoids the white fringe / broken logo artifacts caused by promoting
	// zero-alpha colored texels back to fully opaque.
	return float4(texel.rgb, alpha * input.color.a);
}
