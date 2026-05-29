#include "vk_common.hlsli"

struct VSOutputDepth
{
	float4 position : SV_POSITION;
};

VSOutputDepth VSMain(VSInputPNT input)
{
	VSOutputDepth output;
	float4 worldPosition = mul(float4(input.position, 1.0f), g_world);
	float4 viewPosition = mul(worldPosition, g_view);
	output.position = mul(viewPosition, g_proj);
	return output;
}

VSOutputDepth VSMainPNT2(VSInputPNT2 input)
{
	VSOutputDepth output;
	float4 worldPosition = mul(float4(input.position, 1.0f), g_world);
	float4 viewPosition = mul(worldPosition, g_view);
	output.position = mul(viewPosition, g_proj);
	return output;
}

float4 PSMain(VSOutputDepth input) : SV_TARGET
{
	return float4(0.0f, 0.0f, 0.0f, 1.0f);
}
