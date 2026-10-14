#pragma pack_matrix(row_major)

// God rays — screen-space light shafts (GPU Gems 3, "Volumetric Light Scattering as a Post-Process"):
//   MaskPS      the sun and its halo where the sky shows (depth at the far plane), black elsewhere;
//   BlurPS      a radial blur of the mask towards the sun's screen position, fading along the ray;
// The blurred result is added to the scene by its tone-map pass (Composite.hlsl).
// Each pass reads one texture (binding 0) and takes all its parameters from the push constants.

[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2D u_source : register(t0);
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState u_sourceSampler : register(s0);

struct GodRayPush
{
    float4x4 invViewProjection;  // MaskPS: screen position -> world-space view ray
    float4 sunDir;               // xyz towards the sun; w = overall strength (fades off screen / at night)
    float4 sunColor;             // rgb linear
    float4 rayParams;            // xy = sun position in uv; z = length; w = falloff per sample
    float4 composite;            // x = intensity; y = sample count; zw = output texel size
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
    // Average sky coverage, not depth. Interpolating a leaf's depth with the sky and then
    // thresholding it erases small gaps and makes the entire low-resolution pixel pop on/off.
    uint width, height;
    u_source.GetDimensions(width, height);
    const float2 sourceSize = float2(width, height);
    const int2 coverageSamples = clamp(int2(ceil(sourceSize * u_push.composite.zw - 1e-3)), 1, 4);
    float coverage = 0.0;
    for (int y = 0; y < coverageSamples.y; ++y)
    {
        for (int x = 0; x < coverageSamples.x; ++x)
        {
            const float2 offset = (float2(x + 0.5, y + 0.5) / coverageSamples - 0.5) * u_push.composite.zw;
            const int2 pixel = clamp(int2((input.uv + offset) * sourceSize), int2(0, 0), int2(width, height) - 1);
            coverage += step(0.99999, u_source.Load(int3(pixel, 0)).r);
        }
    }
    coverage /= coverageSamples.x * coverageSamples.y;
    const float c = saturate(dot(ViewRay(input.ndc), normalize(u_push.sunDir.xyz)));
    // A bright core (the disc and its rim), plus a faint wide halo so rays still come while the disc
    // itself is just covered. A wider or stronger halo blurs into one washed-out blob.
    const float glow = 3.0 * pow(c, 2048.0) + 0.6 * pow(c, 256.0) + 0.08 * pow(c, 24.0);
    return float4(u_push.sunColor.rgb * glow * u_push.sunDir.w * coverage, 1.0);
}

float4 BlurPS(VSOutput input) : SV_Target
{
    const int samples = clamp((int)(u_push.composite.y + 0.5), 8, 128);
    const float2 delta = (input.uv - u_push.rayParams.xy) * (u_push.rayParams.z / samples);
    // Stratified, stationary sampling breaks the repeated radial bands without temporal flicker.
    const float jitter = frac(52.9829189 * frac(dot(input.position.xy, float2(0.06711056, 0.00583715))));
    float2 uv = input.uv - delta * (0.25 + 0.5 * jitter);
    // Falloff describes the ray's length, independent of the quality's number of samples.
    const float decay = pow(u_push.rayParams.w, 64.0 / samples);
    float illumination = pow(decay, 0.5);
    float3 sum = float3(0.0, 0.0, 0.0);
    for (int i = 0; i < samples; ++i)
    {
        // Past the screen edge there is nothing known to sample (the edge would smear inwards).
        if (any(uv < 0.0) || any(uv > 1.0))
            break;
        sum += u_source.SampleLevel(u_sourceSampler, uv, 0.0).rgb * illumination;
        illumination *= decay;
        uv -= delta;
    }
    return float4(sum * (3.0 / samples), 1.0);
}

// A Gaussian 5x5 filter, also used to soften the coverage mask before its radial integration.
// rayParams.xy = one source texel in uv.
float4 VolBlurPS(VSOutput input) : SV_Target
{
    // Binomial weights [1,4,6,4,1], combined into nine bilinear taps.
    const float offsets[3] = {-1.2, 0.0, 1.2};
    const float weights[3] = {5.0, 6.0, 5.0};
    float3 sum = float3(0.0, 0.0, 0.0);
    [unroll]
    for (int y = 0; y < 3; ++y)
    {
        [unroll]
        for (int x = 0; x < 3; ++x)
            sum += u_source.SampleLevel(u_sourceSampler, input.uv + float2(offsets[x], offsets[y]) * u_push.rayParams.xy, 0.0).rgb *
                (weights[x] * weights[y]);
    }
    return float4(sum / 256.0, 1.0);
}
