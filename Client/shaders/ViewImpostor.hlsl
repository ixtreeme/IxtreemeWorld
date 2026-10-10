[[vk::binding(0, 0)]] Texture2D<float4> CaptureColor;
[[vk::binding(0, 0)]] SamplerState CaptureSampler;
[[vk::binding(1, 0)]] Texture2D<float> CaptureDepth;
[[vk::binding(1, 0)]] SamplerState DepthSampler;

struct Parameters
{
    float4 rectangle;
    float4 capture;
    float4 extent; // viewport XY, reprojection flag Z
    float4 sourceX;
    float4 sourceY;
    float4 sourceZ;
    float4 sourceW;
};
[[vk::push_constant]] Parameters Params;
struct VertexOutput { float4 position : SV_Position; };

VertexOutput VSMain(uint id : SV_VertexID)
{
    // Two triangles, integer pixel edges. Positive viewport height and Vulkan projection Y.
    const float2 corners[6] = {float2(0,0), float2(1,0), float2(0,1),
                               float2(0,1), float2(1,0), float2(1,1)};
    float2 pixel = Params.rectangle.xy + corners[id] * Params.rectangle.zw;
    VertexOutput result;
    result.position = float4(pixel / Params.extent.xy * 2 - 1, 0, 1);
    return result;
}
struct PixelOutput { float4 color : SV_Target; float depth : SV_Depth; };
PixelOutput PSMain(VertexOutput input)
{
    float2 sourcePixel = input.position.xy;
    float4 current = float4(input.position.xy / Params.extent.xy * 2 - 1, 0, 1);
    if (Params.extent.z != 0)
    {
        float w = dot(current, Params.sourceW);
        if (w <= 0) discard;
        sourcePixel = (float2(dot(current, Params.sourceX), dot(current, Params.sourceY)) / w + 1)
                      * 0.5 * Params.extent.xy;
    }
    int2 texel = int2(floor(sourcePixel - Params.capture.xy));
    if (any(texel < 0) || any(texel >= int2(Params.capture.zw))) discard;
    float depth = CaptureDepth.Load(int3(texel, 0));
    // The capture contains only this object, cleared to far depth. Preserve alpha holes and
    // empty space without writing a flat quad's depth over neighbouring geometry.
    if (depth >= 1 || depth < 0) discard;
    PixelOutput result;
    result.color = CaptureColor.Load(int3(texel, 0));
    if (Params.extent.z != 0)
    {
        // Solve sourceZ/sourceW = captured depth for CURRENT clip Z. Sampling colour alone
        // would put the old camera's depth into the new frame and break terrain/character occlusion.
        float denominator = depth * Params.sourceW.z - Params.sourceZ.z;
        if (abs(denominator) < 1e-6) discard;
        depth = (dot(current, Params.sourceZ) - depth * dot(current, Params.sourceW)) / denominator;
        if (depth < 0 || depth >= 1) discard;
    }
    result.depth = depth;
    return result;
}
