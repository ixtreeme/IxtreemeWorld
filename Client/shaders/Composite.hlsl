#pragma pack_matrix(row_major)

// The scene renders in floating point (light of any brightness); this pass brings it to the screen:
// exposure, then a tone curve, into an 8-bit (sRGB) image: the editor's view image or the swapchain.

[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2D u_sceneColor : register(t0);
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState u_sceneSampler : register(s0);
// Light added to the scene before exposure (OffscreenSceneRenderer::SetAddedLight: the god rays),
// each image weighed by addedLightWeight (0 where none is added).
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] Texture2D u_addedLight0 : register(t1);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState u_addedLight0Sampler : register(s1);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] Texture2D u_addedLight1 : register(t2);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] SamplerState u_addedLight1Sampler : register(s2);

struct ToneMapParams
{
    float exposure;  // light multiplier before the curve
    int mode;        // 0 none (clipped at white), 1 neutral, 2 filmic
    float2 addedLightWeight;
};
[[vk::push_constant]] ToneMapParams u_toneMap;

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    VSOutput output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(output.uv * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

// Neutral: colours up to 0.8 stay exactly as they are (what scenes were tuned with before the views
// rendered in floating point); brighter ones roll off smoothly towards white instead of clipping, and
// lose a little saturation as they do. The highlight curve of Khronos PBR Neutral, without its toe
// (that offset assumes a PBR Fresnel term these shaders do not add).
float3 NeutralToneMap(float3 color)
{
    const float startCompression = 0.8;
    const float desaturation = 0.15;
    const float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression)
        return color;
    const float d = 1.0 - startCompression;
    const float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;
    const float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return lerp(color, newPeak.xxx, g);
}

// Filmic: Krzysztof Narkowicz's fit of the ACES curve: more contrast and brighter midtones.
float3 FilmicToneMap(float3 x)
{
    return (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14);
}

float4 PSMain(VSOutput input) : SV_Target0
{
    float3 color = u_sceneColor.Sample(u_sceneSampler, input.uv).rgb;
    if (u_toneMap.addedLightWeight.x != 0.0)
        color += u_addedLight0.SampleLevel(u_addedLight0Sampler, input.uv, 0.0).rgb * u_toneMap.addedLightWeight.x;
    if (u_toneMap.addedLightWeight.y != 0.0)
        color += u_addedLight1.SampleLevel(u_addedLight1Sampler, input.uv, 0.0).rgb * u_toneMap.addedLightWeight.y;
    // A floating-point target keeps what an 8-bit one clipped: NaN, infinity, negative light.
    if (any(isnan(color)))
        color = 0.0.xxx;
    color = clamp(color, 0.0, 65504.0) * u_toneMap.exposure;
    if (u_toneMap.mode == 1)
        color = NeutralToneMap(color);
    else if (u_toneMap.mode == 2)
        color = FilmicToneMap(color);
    return float4(saturate(color), 1.0);
}
