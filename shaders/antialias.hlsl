// antialias.hlsl - the game's software anti-aliasing (AntiAliasPass): TRAntiAlias.fx technique tecAA, pass AntiAlias (ZWRITEENABLE 0,
// ZFUNC ALWAYS), its ps_3_0 disassembled 2026-11-01. TRasa::GraphicsUtil::PostRender_AntiAlias::OnEvent (0x00803110) runs it over the main
// view's finished frame (a copy of it: CopyPrimaryRenderTarget) with HalfPixelSize = (0.5 / width, 0.5 / height). The pixel shader adds the
// half pixel to its texture coordinate (the Direct3D 9 texel-centre offset; here the coordinate is at the centre already) and, with
// c = the pixel and w = 3 - (2/3) (c.r + c.g + c.b), blends towards each of the four neighbours (one pixel right, left, up, down) by
// t = clamp(3 (w |n - c| - 0.2), 0, 0.5) per channel: the result is the mean of the four lerp(c, n, t); alpha 1.

cbuffer FragmentUniforms : register(b0, space3)
{
    float4 texel;     // xy: one source texel (1 / width, 1 / height)
};

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput vsMain(uint id : SV_VertexID)
{
    const float2 p = float2((id << 1) & 2, id & 2);
    VSOutput o;
    o.position = float4(p.x * 2 - 1, 1 - p.y * 2, 0, 1);
    o.uv = p;
    return o;
}

[[vk::combinedImageSampler]] Texture2D frameTexture : register(t0, space2);
[[vk::combinedImageSampler]] SamplerState frameSampler : register(s0, space2);

float3 blendTowards(float3 c, float3 n, float w)
{
    const float3 t = min(max((w * abs(n - c) - 0.2) * 3, 0), 0.5);
    return lerp(c, n, t);
}

float4 psMain(VSOutput input) : SV_Target
{
    const float2 p = input.uv;
    const float3 c = frameTexture.Sample(frameSampler, p).rgb;
    const float w = 3 - 0.666665971 * (c.r + c.g + c.b);
    float3 sum = blendTowards(c, frameTexture.Sample(frameSampler, p + float2(texel.x, 0)).rgb, w);
    sum += blendTowards(c, frameTexture.Sample(frameSampler, p - float2(texel.x, 0)).rgb, w);
    sum += blendTowards(c, frameTexture.Sample(frameSampler, p - float2(0, texel.y)).rgb, w);
    sum += blendTowards(c, frameTexture.Sample(frameSampler, p + float2(0, texel.y)).rgb, w);
    return float4(sum * 0.25, 1);
}
