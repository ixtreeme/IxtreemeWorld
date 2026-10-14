struct PushConstants
{
    float2 viewport;
    float2 translation;
    float4 params;  // x: 1 = the target is sRGB (decode the sRGB UI colors to linear)
};

[[vk::push_constant]] PushConstants g_push;

struct VSInput
{
    float2 position : POSITION;
    float4 color : COLOR0;
    float2 texcoord : TEXCOORD0;
};

struct VSOutput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
    float2 texcoord : TEXCOORD0;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    const float2 pixel = input.position + g_push.translation;
    const float2 ndc = float2((pixel.x / g_push.viewport.x) * 2.0f - 1.0f,
                             (pixel.y / g_push.viewport.y) * 2.0f - 1.0f);
    output.position = float4(ndc, 0.0f, 1.0f);
    output.color = input.color;
    output.texcoord = input.texcoord;
    return output;
}

[[vk::binding(0, 0)]] Texture2D g_texture : register(t0);
[[vk::binding(1, 0)]] SamplerState g_sampler : register(s0);

float3 SrgbToLinear(float3 c)
{
    const float3 low = c / 12.92f;
    const float3 high = pow((c + 0.055f) / 1.055f, 2.4f);
    return lerp(high, low, step(c, 0.04045f));
}

float4 PSMain(VSOutput input) : SV_Target
{
    // RCSS colors and UI images are sRGB with premultiplied alpha. An sRGB target encodes what is
    // written as linear, so decode first (on the straight color) or every color shows too light.
    float4 color = g_texture.Sample(g_sampler, input.texcoord) * input.color;
    if (g_push.params.x > 0.5f && color.a > 0.0f)
        color.rgb = SrgbToLinear(saturate(color.rgb / color.a)) * color.a;
    return color;
}
