// ============================================================================
//  vk_common.hlsli  —  Vulkan (SPIR-V) common header
//
//  Migrated from the DX11 HLSL set. Resource bindings are made EXPLICIT via
//  [[vk::binding(binding, set)]] so each shader is self-documenting and the
//  pipeline layout can be reconstructed directly from the source (no reliance
//  on DXC register-shift flags).
//
//  DESCRIPTOR SET LAYOUT (frequency-based):
//    set 0  — Frame    (per-frame, bound once per frame)
//      binding 0 : FrameConstants            (was b0)
//    set 1  — Object   (per-draw / per-material)
//      binding 0 : ObjectConstants           (was b1)
//      binding 1 : MaterialConstants         (was b2)
//      binding 2 : LightConstants            (was b3)
//      binding 3 : SkinConstants             (was b4, skinned_mesh only)
//    set 2  — Material resources (per-material)
//      binding 0 : Texture2D  slot 0         (was t0)
//      binding 1 : Texture2D  slot 1         (was t1)
//      binding 2 : SamplerState slot 0       (was s0)
//      binding 3 : SamplerState slot 1       (was s1)
//
//  COORDINATE SYSTEM:
//    Vulkan NDC has Y pointing DOWN and depth range [0,1].
//    The Y-flip is handled CPU-side (negative-height viewport recommended),
//    NOT in these shaders. g_proj is uploaded ready-to-use.
// ============================================================================

#ifndef VK_COMMON_HLSLI
#define VK_COMMON_HLSLI

// ---- set 0 : Frame ---------------------------------------------------------
[[vk::binding(0, 0)]]
cbuffer FrameConstants
{
	row_major float4x4 g_view;
	row_major float4x4 g_proj;
	float4 g_cameraPosition;
	float4 g_fogColor;
	float4 g_fogParams;
};

// ---- set 1 : Object / Material / Light -------------------------------------
[[vk::binding(0, 1)]]
cbuffer ObjectConstants
{
	row_major float4x4 g_world;
	row_major float4x4 g_texTransform0;
	row_major float4x4 g_texTransform1;
	float4 g_objectColor;
};

[[vk::binding(1, 1)]]
cbuffer MaterialConstants
{
	float4 g_diffuseColor;
	float4 g_ambientColor;
	float4 g_specularColor;
	float4 g_emissiveColor;
};

[[vk::binding(2, 1)]]
cbuffer LightConstants
{
	float4 g_lightDirection;
	float4 g_lightAmbient;
	float4 g_lightDiffuse;
};

// ---- Vertex input layouts (unchanged from DX11) ----------------------------
struct VSInputPCT
{
	float3 position : POSITION;
	float4 color : COLOR0;
	float2 uv : TEXCOORD0;
};

struct VSInputPC
{
	float3 position : POSITION;
	float4 color : COLOR0;
};

struct VSInputPNT
{
	float3 position : POSITION;
	float3 normal : NORMAL;
	float2 uv : TEXCOORD0;
};

struct VSInputPN
{
	float3 position : POSITION;
	float3 normal : NORMAL;
};

struct VSInputPNT2
{
	float3 position : POSITION;
	float3 normal : NORMAL;
	float2 uv : TEXCOORD0;
	float2 uv1 : TEXCOORD1;
};

struct VSOutput
{
	float4 position : SV_POSITION;
	float4 color : COLOR0;
	float2 uv : TEXCOORD0;
	float fogFactor : TEXCOORD1;
};

// ---- Shared helpers (unchanged) --------------------------------------------
float ComputeFogFactor(float3 viewPosition)
{
	if (g_fogParams.w < 0.5f)
		return 1.0f;

	float fogRange = max(g_fogParams.y - g_fogParams.x, 0.0001f);
	return saturate((g_fogParams.y - abs(viewPosition.z)) / fogRange);
}

float3 ApplyFog(float3 color, float fogFactor)
{
	if (g_fogParams.w < 0.5f)
		return color;

	return lerp(g_fogColor.rgb, color, fogFactor);
}

float3 ComputeLambert(float3 worldNormal)
{
	float3 normal = normalize(worldNormal);
	float3 lightDir = -normalize(g_lightDirection.xyz);
	float ndl = saturate(dot(normal, lightDir));

	float3 ambient = g_lightAmbient.rgb * max(g_ambientColor.rgb, float3(0.25f, 0.25f, 0.25f));
	float3 diffuse = g_lightDiffuse.rgb * ndl;
	float3 emissive = g_emissiveColor.rgb;
	return saturate(ambient + diffuse + emissive);
}

#endif // VK_COMMON_HLSLI
