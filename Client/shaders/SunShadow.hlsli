// Sun shadow lookup for the mesh shaders (static and skinned). TerrainRenderer draws the cascades;
// the including shader declares, among its constants:
//   float4x4 u_shadowCascadeViewProj[4];  // light view-projection per cascade, finest first
//   float4 u_shadowParams;                // x: 1 when the map was drawn, y: map size in texels
//   float4 u_shadowDepthBias;             // per cascade, in its depth units
//   float4 u_shadowNormalOffset;          // per cascade, metres along the surface normal
// and the cascade array with its compare sampler as u_sunShadowMap / u_sunShadowSampler.

// 1 lit .. 0 shadowed at a cascade's uv and depth: a 5x5 grid of bilinear compare taps one texel
// apart, taken as 3x3 taps placed at each adjacent pair's weight ratio (as the terrain does).
float SunShadowPcf(float2 uv, float compareDepth, int cascade)
{
    const float shadowSize = max(u_shadowParams.y, 1.0);
    const float2 texCoord = uv * shadowSize - 0.5;
    const float2 base = floor(texCoord);
    const float2 f = texCoord - base;
    const float2 weight0 = 2.0 - f;
    const float2 weight2 = 1.0 + f;
    const float2 tap0 = base - 2.0 + 1.0 / weight0;
    const float2 tap1 = base + 0.5;
    const float2 tap2 = base + 2.0 + f / weight2;
    float lit = 0.0;
    [unroll]
    for (int ix = 0; ix < 3; ++ix)
    {
        const float tapX = ix == 0 ? tap0.x : (ix == 1 ? tap1.x : tap2.x);
        const float weightX = ix == 0 ? weight0.x : (ix == 1 ? 2.0 : weight2.x);
        [unroll]
        for (int iy = 0; iy < 3; ++iy)
        {
            const float tapY = iy == 0 ? tap0.y : (iy == 1 ? tap1.y : tap2.y);
            const float weightY = iy == 0 ? weight0.y : (iy == 1 ? 2.0 : weight2.y);
            lit += weightX * weightY * u_sunShadowMap.SampleCmpLevelZero(
                u_sunShadowSampler,
                float3((float2(tapX, tapY) + 0.5) / shadowSize, (float)cascade),
                compareDepth);
        }
    }
    return lit / 25.0;
}

// 1 where the sun reaches the surface point, 0 in shadow. The finest cascade that holds the point
// (with room for the filter) decides; the point is moved off the surface along its normal by about
// a texel of that cascade so the surface does not shadow itself.
float SunShadow(float3 worldPos, float3 normal)
{
    if (u_shadowParams.x < 0.5)
        return 1.0;
    const float margin = 3.0 / max(u_shadowParams.y, 1.0);
    [unroll]
    for (int cascade = 0; cascade < 4; ++cascade)
    {
        const float3 p = worldPos + normal * u_shadowNormalOffset[cascade];
        const float3 lightSpace = mul(float4(p, 1.0), u_shadowCascadeViewProj[cascade]).xyz;
        const float2 uv = lightSpace.xy * 0.5 + 0.5;
        if (all(uv > margin) && all(uv < 1.0 - margin) && lightSpace.z < 1.0)
            return SunShadowPcf(uv, lightSpace.z - u_shadowDepthBias[cascade], cascade);
    }
    return 1.0;  // past the last cascade: no shadow information
}
