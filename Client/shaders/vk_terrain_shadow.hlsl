#include "vk_common.hlsli"

[[vk::binding(0, 2)]] Texture2D g_staticShadowTexture;
[[vk::binding(1, 2)]] Texture2D g_dynamicShadowTexture;
[[vk::binding(2, 2)]] SamplerState g_wrapSampler;
[[vk::binding(3, 2)]] SamplerState g_clampSampler;

struct VSOutputShadow
{
	float4 position : SV_POSITION;
	float2 uvStatic : TEXCOORD0;
	float2 uvDynamic : TEXCOORD1;
	float fogFactor : TEXCOORD2;
};

bool IsValidShadowUV(float2 uv)
{
	return all(uv >= float2(0.0f, 0.0f)) && all(uv <= float2(1.0f, 1.0f));
}

VSOutputShadow VSMain(VSInputPN input)
{
	VSOutputShadow output;
	float4 worldPosition = mul(float4(input.position, 1.0f), g_world);
	float4 viewPosition = mul(worldPosition, g_view);
	output.position = mul(viewPosition, g_proj);
	output.uvStatic = mul(float4(worldPosition.xyz, 1.0f), g_texTransform0).xy;
	output.uvDynamic = mul(float4(worldPosition.xyz, 1.0f), g_texTransform1).xy;
	output.fogFactor = ComputeFogFactor(viewPosition.xyz);
	return output;
}

float ComputeShadowLuma(float3 color)
{
	return dot(color, float3(0.299f, 0.587f, 0.114f));
}

float SampleStaticShadow(float2 uv)
{
	if (!IsValidShadowUV(uv))
		return 1.0f;
	return ComputeShadowLuma(g_staticShadowTexture.Sample(g_clampSampler, uv).rgb);
}

float SampleDynamicShadow(float2 uv)
{
	if (!IsValidShadowUV(uv))
		return 1.0f;

	uint width = 0;
	uint height = 0;
	g_dynamicShadowTexture.GetDimensions(width, height);
	if (width == 0 || height == 0)
		return 1.0f;

	float2 texel = 1.0f / float2(width, height);
	float accum = 0.0f;
	accum += ComputeShadowLuma(g_dynamicShadowTexture.Sample(g_clampSampler, uv).rgb);
	accum += ComputeShadowLuma(g_dynamicShadowTexture.Sample(g_clampSampler, uv + float2(texel.x, 0.0f)).rgb);
	accum += ComputeShadowLuma(g_dynamicShadowTexture.Sample(g_clampSampler, uv + float2(-texel.x, 0.0f)).rgb);
	accum += ComputeShadowLuma(g_dynamicShadowTexture.Sample(g_clampSampler, uv + float2(0.0f, texel.y)).rgb);
	accum += ComputeShadowLuma(g_dynamicShadowTexture.Sample(g_clampSampler, uv + float2(0.0f, -texel.y)).rgb);
	return accum * 0.2f;
}

float4 PSMain(VSOutputShadow input) : SV_TARGET
{
	float staticShadow = SampleStaticShadow(input.uvStatic);
	float dynamicShadow = SampleDynamicShadow(input.uvDynamic);
	float shadowFactor = saturate(staticShadow * dynamicShadow);
	float multiplyFactor = saturate(1.0f - (1.0f - shadowFactor) * 0.7f * input.fogFactor);
	return float4(multiplyFactor, multiplyFactor, multiplyFactor, 1.0f);
}
