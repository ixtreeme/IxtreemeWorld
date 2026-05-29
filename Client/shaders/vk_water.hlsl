#include "vk_common.hlsli"

[[vk::binding(0, 2)]] Texture2D g_baseTexture;
[[vk::binding(2, 2)]] SamplerState g_linearWrapSampler;

struct VSOutputWater
{
	float4 position : SV_POSITION;
	float4 color : COLOR0;
	float2 uv : TEXCOORD0;
	float2 uv1 : TEXCOORD1;
	float fogFactor : TEXCOORD2;
};

VSOutputWater VSMain(VSInputPC input)
{
	VSOutputWater output;
	float4 worldPosition = mul(float4(input.position, 1.0f), g_world);
	float4 viewPosition = mul(worldPosition, g_view);
	output.position = mul(viewPosition, g_proj);
	output.color = input.color;
	output.uv = mul(float4(viewPosition.xyz, 1.0f), g_texTransform0).xy;
	output.uv1 = mul(float4(viewPosition.xyz, 1.0f), g_texTransform1).xy;
	output.fogFactor = ComputeFogFactor(viewPosition.xyz);
	return output;
}

float4 PSMain(VSOutputWater input) : SV_TARGET
{
	float4 color0 = g_baseTexture.Sample(g_linearWrapSampler, input.uv);
	float4 color1 = g_baseTexture.Sample(g_linearWrapSampler, input.uv1);
	float4 color = lerp(color0, color1, 0.5f);
	color.a *= input.color.a;
	color.rgb = lerp(color.rgb, color.rgb * float3(0.82f, 0.90f, 1.10f), 0.2f);
	color.rgb = ApplyFog(color.rgb, input.fogFactor);
	return color;
}
