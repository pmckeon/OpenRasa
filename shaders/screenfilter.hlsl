// screenfilter.hlsl - SCRF screen filters (ScreenFilterPass): TRPostFX_ScreenFilter.fx technique PostEffect_ScreenFilter (two of that
// name: 0 SRCALPHA / INVSRCALPHA, 1 SRCALPHA / ONE; ZENABLE, CULLMODE NONE, no fog), pixel shader psShader[Mode], its ps_2_0 elements
// disassembled 2026-11-04 (scratchpad fwd/psshader.txt). Mode = the filter's base (0 Normal, 1 GreyScale, 2 White, 3 Red) + 4 x the
// variant (0: the overlay multiplies the frame, 1: adds to it, 2: adds as a detail map, (o - 0.5) x 2). Each element: the distortion map
// at the pixel's screen coordinate moves the lookup by (d.xy - 0.5) x 2 x Distortion pixels' worth of NDC (RenderTargetSize's
// reciprocal), the frame copy (RenderTarget) and the overlay are read there; alpha = overlay.a x OverlayAlpha^2 (preshader) x Alpha x
// the frame's alpha (taken as 1 here: the game's copy of its target is not traced).

cbuffer FragmentUniforms : register(b0, space3)
{
    float4 texel;      // xy: 1 / width, 1 / height
    float4 params;     // Intensity, Alpha, Distortion, OverlayAlpha
    float4 mode;       // x base filter 0..3, y variant 0..2
};

struct VSOutput
{
    float4 position : SV_Position;
    float2 ndc : TEXCOORD0;
};

VSOutput vsMain(uint id : SV_VertexID)
{
    const float2 p = float2((id << 1) & 2, id & 2);
    VSOutput o;
    o.position = float4(p.x * 2 - 1, 1 - p.y * 2, 0, 1);
    o.ndc = o.position.xy;
    return o;
}

[[vk::combinedImageSampler]] Texture2D frameTexture : register(t0, space2);
[[vk::combinedImageSampler]] SamplerState frameSampler : register(s0, space2);
[[vk::combinedImageSampler]] Texture2D overlayTexture : register(t1, space2);
[[vk::combinedImageSampler]] SamplerState overlaySampler : register(s1, space2);
[[vk::combinedImageSampler]] Texture2D distortTexture : register(t2, space2);
[[vk::combinedImageSampler]] SamplerState distortSampler : register(s2, space2);

float4 psMain(VSOutput input) : SV_Target
{
    const float intensity = params.x, alpha = params.y, distortion = params.z, overlayAlpha = params.w;
    const float2 d = distortTexture.Sample(distortSampler, float2(input.ndc.x * 0.5 + 0.5, input.ndc.y * -0.5 + 0.5)).xy;
    const float2 ndc = input.ndc + (d - 0.5) * distortion * 2 * texel.xy;
    const float2 uv = float2(ndc.x * 0.5 + 0.5, ndc.y * -0.5 + 0.5);
    const float4 ov = overlayTexture.Sample(overlaySampler, uv);
    const float4 rt = float4(frameTexture.Sample(frameSampler, uv).rgb, 1);
    const float a = ov.a * overlayAlpha * overlayAlpha * alpha;
    const int base = (int)(mode.x + 0.5), variant = (int)(mode.y + 0.5);
    const float3 c = variant == 0 ? ov.rgb * rt.rgb : variant == 1 ? ov.rgb + rt.rgb : rt.rgb + (ov.rgb - 0.5) * 2;
    const float mean = (c.r + c.g + c.b) * 0.333299994;
    if (base == 1)
    {
        // GreyScale: lerp_sat towards (grey, 1) by Intensity, alpha that x a.
        const float l = saturate(mean);
        const float4 g = saturate(lerp(float4(c, rt.a), float4(l, l, l, 1), intensity));
        return float4(g.rgb, g.a * a);
    }
    if (base == 2)
    {
        // White: + l x Intensity + Intensity / 4 (preshader) + 0.1 Intensity where l >= 0.5, saturated.
        const float l = saturate(mean);
        return float4(saturate(c + l * intensity + intensity * 0.25 + (l >= 0.5 ? 0.1 * intensity : 0)), rt.a * a);
    }
    if (base == 3)
    {
        // Red: red + 4 x saturate(mean) x Intensity.
        return float4(c.r + 4 * saturate(mean) * intensity, c.g, c.b, rt.a * a);
    }
    return float4(c, rt.a * a);
}
