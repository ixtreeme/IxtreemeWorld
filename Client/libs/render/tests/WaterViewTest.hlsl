[[vk::binding(0, 0)]] cbuffer TestColor { float4 color; };
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> result;
[[vk::binding(2, 0)]] Texture2D<float4> scene;
[[vk::binding(3, 0)]] Texture2D<float4> game;

float4 VSMain(uint id : SV_VertexID) : SV_Position
{
    return float4(id == 1 ? 3 : -1, id == 2 ? 3 : -1, 0, 1);
}
float4 PSMain() : SV_Target { return color; }
[numthreads(1, 1, 1)]
void CSMain()
{
    result[0] = scene.Load(int3(32, 32, 0));
    result[1] = game.Load(int3(32, 32, 0));
}
