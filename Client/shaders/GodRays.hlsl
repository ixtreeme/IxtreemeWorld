#pragma pack_matrix(row_major)

// God rays — screen-space light shafts (GPU Gems 3, "Volumetric Light Scattering as a Post-Process"):
//   MaskPS      the sun and its halo where the sky shows (depth at the far plane), black elsewhere;
//   BlurPS      a radial blur of the mask towards the sun's screen position, fading along the ray;
//   CompositePS the result added over the scene image.
// Each pass reads one texture (binding 0) and takes all its parameters from the push constants.

[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2D u_source : register(t0);
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState u_sourceSampler : register(s0);

struct GodRayPush
{
    float4x4 invViewProjection;  // MaskPS: screen position -> world-space view ray
    float4 sunDir;               // xyz towards the sun; w = overall strength (fades off screen / at night)
    float4 sunColor;             // rgb linear
    float4 rayParams;            // xy = sun position in uv; z = length; w = falloff per sample
    float4 composite;            // x = intensity; y = sample count
};
[[vk::push_constant]] GodRayPush u_push;

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float2 ndc : TEXCOORD1;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    const float2 ndc = float2((vertexId == 1) ? 3.0 : -1.0, (vertexId == 2) ? 3.0 : -1.0);
    VSOutput output;
    output.position = float4(ndc, 0.0, 1.0);
    output.ndc = ndc;
    output.uv = ndc * 0.5 + 0.5;
    return output;
}

float3 ViewRay(float2 ndc)
{
    const float4 nearPoint = mul(float4(ndc, 0.0, 1.0), u_push.invViewProjection);
    const float4 farPoint = mul(float4(ndc, 1.0, 1.0), u_push.invViewProjection);
    return normalize(farPoint.xyz / farPoint.w - nearPoint.xyz / nearPoint.w);
}

float4 MaskPS(VSOutput input) : SV_Target
{
    // Only the sky lets the sun through: geometry (terrain, meshes, characters) blocks it.
    const float depth = u_source.SampleLevel(u_sourceSampler, input.uv, 0.0).r;
    if (depth < 0.99999)
        return float4(0.0, 0.0, 0.0, 1.0);
    const float c = saturate(dot(ViewRay(input.ndc), normalize(u_push.sunDir.xyz)));
    // A bright core (the disc and its rim), plus a faint wide halo so rays still come while the disc
    // itself is just covered. A wider or stronger halo blurs into one washed-out blob.
    const float glow = 3.0 * pow(c, 2048.0) + 0.6 * pow(c, 256.0) + 0.08 * pow(c, 24.0);
    return float4(u_push.sunColor.rgb * glow * u_push.sunDir.w, 1.0);
}

float4 BlurPS(VSOutput input) : SV_Target
{
    const int samples = clamp((int)(u_push.composite.y + 0.5), 8, 128);
    float2 uv = input.uv;
    const float2 delta = (uv - u_push.rayParams.xy) * (u_push.rayParams.z / samples);
    float illumination = 1.0;
    float3 sum = float3(0.0, 0.0, 0.0);
    for (int i = 0; i < samples; ++i)
    {
        uv -= delta;
        // Past the screen edge there is nothing known to sample (the edge would smear inwards).
        if (any(uv < 0.0) || any(uv > 1.0))
            break;
        sum += u_source.SampleLevel(u_sourceSampler, uv, 0.0).rgb * illumination;
        illumination *= u_push.rayParams.w;
    }
    return float4(sum * (3.0 / samples), 1.0);
}

// The volumetric light (VolumetricLight.hlsl) blurred over 5x5 texels to hide its step noise;
// rayParams.xy = one texel in uv.
float4 VolBlurPS(VSOutput input) : SV_Target
{
    // The 5x5 box in 9 bilinear taps: a tap 1.5 texels out averages two texels, so weighed twice it
    // stands for both of them.
    const float offsets[3] = {-1.5, 0.0, 1.5};
    const float weights[3] = {2.0, 1.0, 2.0};
    float3 sum = float3(0.0, 0.0, 0.0);
    [unroll]
    for (int y = 0; y < 3; ++y)
    {
        [unroll]
        for (int x = 0; x < 3; ++x)
            sum += u_source.SampleLevel(u_sourceSampler, input.uv + float2(offsets[x], offsets[y]) * u_push.rayParams.xy, 0.0).rgb *
                (weights[x] * weights[y]);
    }
    return float4(sum / 25.0, 1.0);
}

float4 CompositePS(VSOutput input) : SV_Target
{
    return float4(u_source.SampleLevel(u_sourceSampler, input.uv, 0.0).rgb * u_push.composite.x, 0.0);
}
