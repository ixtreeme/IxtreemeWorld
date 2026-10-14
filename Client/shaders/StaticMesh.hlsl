#include "StaticMeshCommon.hlsli"

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

VSOutput VSMain(VSInput input, uint instanceId : SV_InstanceID)
{
    StaticMeshInstanceData instance = u_instances[instanceId];
    VSOutput output;
    float3 localPosition = input.position;
#if defined(STATIC_MESH_OUTLINE)
    const float normalLenSq = dot(input.normal, input.normal);
    const float3 outlineNormal = normalLenSq > 0.000001 ? input.normal * rsqrt(normalLenSq) : float3(0.0, 1.0, 0.0);
    localPosition += outlineNormal * 0.045;
#endif
    output.position = mul(float4(localPosition, 1.0), instance.mvp);
    const float4 worldPos = mul(float4(localPosition, 1.0), instance.model);
    output.normal = normalize(mul(float4(input.normal, 0.0), instance.model).xyz);
    output.uv = input.uv * max(instance.materialUv.xy, float2(0.001, 0.001)) + instance.materialUv.zw;
    output.worldPos = worldPos.xyz;
    output.tint = instance.tint;
    output.materialBaseColor = instance.materialBaseColor;
    output.materialParams = instance.materialParams;
    output.materialEmissive = instance.materialEmissive;
    output.materialAlpha = instance.materialAlpha;
#if defined(STATIC_MESH_SHADOW)
    // Into a sun shadow cascade (mvp = model x light view-projection, orthographic): a caster
    // between the sun and the cascade's depth range is flattened onto its near plane, not clipped.
    output.position.z = max(output.position.z, 0.0);
#endif
    return output;
}

void ShadowMaskPS(VSOutput input)
{
    ApplyLodCoverage(input);
    const float alpha = u_diffuse.Sample(u_sampler, input.uv).a * input.materialBaseColor.a * input.tint.a;
    if (input.materialAlpha.x > 0.5 && alpha < input.materialAlpha.y)
        discard;
}

float3 ApplyNormalMap(float3 vertexNormal, float3 worldPos, float2 uv, float strength)
{
    float3 mapNormal = u_normalMap.Sample(u_normalSampler, uv).xyz * 2.0 - 1.0;
    mapNormal.xy *= max(strength, 0.0);
    mapNormal = normalize(mapNormal);
    return PerturbNormal(vertexNormal, worldPos, uv, mapNormal);
}

float4 PSMain(VSOutput input) : SV_Target0
{
#if defined(STATIC_MESH_OUTLINE)
    ShadowMaskPS(input);
    return float4(1.0, 0.78, 0.18, 1.0);
#else
    // The opaque draws' variant (STATIC_MESH_OPAQUE) has no discard at all: a shader that can discard
    // keeps the hardware from testing depth before shading, so every hidden layer was shaded.
#if !defined(STATIC_MESH_OPAQUE)
    ApplyLodCoverage(input);
    if (u_lightPadding.x > 0.5 && input.worldPos.y < u_lightPadding.y)
        discard;
#endif

    float4 texSample = u_diffuse.Sample(u_sampler, input.uv);
    const float alpha = texSample.a * input.materialBaseColor.a * input.tint.a;
#if !defined(STATIC_MESH_OPAQUE)
    if (input.materialAlpha.x > 0.5 && input.materialAlpha.x < 1.5 && alpha < input.materialAlpha.y)
        discard;
#endif

    float3 texColor = texSample.rgb;
    float3 albedo = texColor * input.materialBaseColor.rgb * input.tint.rgb;

#if defined(SHADING_MODE_UNLIT)
    float3 unlitColor = albedo + input.materialEmissive.rgb * input.materialEmissive.a;
    return float4(unlitColor, alpha);
#else
    float3 normal = ApplyNormalMap(normalize(input.normal), input.worldPos, input.uv, input.materialParams.z);
    float3 orm = u_ormMap.Sample(u_ormSampler, input.uv).rgb;
    const float metallic = saturate(input.materialParams.x * orm.b);
    const float roughness = saturate(input.materialParams.y * max(orm.g, 0.04));
    const float ao = saturate(input.materialParams.w * orm.r);
    float3 color = StaticMeshLighting(albedo, normal, normalize(input.normal), input.worldPos, roughness, metallic, ao) +
        input.materialEmissive.rgb * input.materialEmissive.a;
    return float4(color, alpha);
#endif
#endif
}
