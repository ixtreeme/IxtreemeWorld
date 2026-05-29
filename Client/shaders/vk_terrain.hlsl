#include "vk_common.hlsli"

[[vk::binding(0, 2)]] Texture2D g_colorTexture;
[[vk::binding(1, 2)]] Texture2D g_maskTexture;
[[vk::binding(2, 2)]] SamplerState g_linearWrapSampler;
[[vk::binding(3, 2)]] SamplerState g_linearClampSampler;

struct VSTerrainOutput
{
	float4 position : SV_POSITION;
	float3 lighting : TEXCOORD0;
	float2 colorUv : TEXCOORD1;
	float2 maskUv : TEXCOORD2;
	float fogFactor : TEXCOORD3;
};

VSTerrainOutput VSMain(VSInputPN input)
{
	VSTerrainOutput output;

	float4 worldPosition = mul(float4(input.position, 1.0f), g_world);
	float4 viewPosition = mul(worldPosition, g_view);
	float3 worldNormal = mul(float4(input.normal, 0.0f), g_world).xyz;
	float4 texGenPosition = float4(viewPosition.xyz, 1.0f);

	output.position = mul(viewPosition, g_proj);
	output.lighting = ComputeLambert(worldNormal);
	output.colorUv = mul(texGenPosition, g_texTransform0).xy;
	output.maskUv = mul(texGenPosition, g_texTransform1).xy;
	output.fogFactor = ComputeFogFactor(viewPosition.xyz);
	return output;
}

float4 PSMain(VSTerrainOutput input) : SV_TARGET
{
	float4 color = g_colorTexture.Sample(g_linearWrapSampler, input.colorUv);
	float mask = g_maskTexture.Sample(g_linearClampSampler, input.maskUv).a;

	color.rgb *= input.lighting;
	color.a *= mask;
	color.rgb = ApplyFog(color.rgb, input.fogFactor);
	return color;
}
