// gamma.hlsl - the display gamma (GammaPass): the game sets a gamma ramp, IDirect3DDevice9::SetGammaRamp (FUN_0059cec0), of
// (i / 255)^(1 / g) per channel (FUN_0059cfa0), which Direct3D 9 applies only in fullscreen. Here a last pass copies the finished frame
// to the swapchain with the same curve.

cbuffer FragmentUniforms : register(b0, space3)
{
    float4 gamma;     // x: 1 / g
};

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput vsMain(uint id : SV_VertexID)
{
    // One triangle over the screen.
    const float2 p = float2((id << 1) & 2, id & 2);
    VSOutput o;
    o.position = float4(p.x * 2 - 1, 1 - p.y * 2, 0, 1);
    o.uv = p;
    return o;
}

[[vk::combinedImageSampler]] Texture2D frameTexture : register(t0, space2);
[[vk::combinedImageSampler]] SamplerState frameSampler : register(s0, space2);

float4 psMain(VSOutput input) : SV_Target
{
    float4 c = frameTexture.Sample(frameSampler, input.uv);
    return float4(pow(c.rgb, gamma.x), 1);
}
