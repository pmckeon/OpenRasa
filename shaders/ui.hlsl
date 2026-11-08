// ui.hlsl - the game's UI, one textured quad per draw (UiGpu). Mirrors UiCanvas's CPU drawing (texture region, stretch / tile,
// bitmap-animation rotation and wrap, mask texture, the TRUIBackgroundMask mouse mask, colour) and the UI effects' blending, which
// the pipelines set (SRCALPHA / INVSRCALPHA, SRCALPHA / ONE, ONE / ONE).
// SDL GPU register spaces: vertex uniforms space1, fragment textures / samplers space2, fragment uniforms space3.

cbuffer VertexUniforms : register(b0, space1)
{
    float4 rect;      // pixels: x0, y0, x1, y1
    float4 screen;    // pixels: width, height
};

struct VSOutput
{
    float4 position : SV_Position;
    float2 local : TEXCOORD0;     // 0..1 across the quad
};

VSOutput vsMain(uint id : SV_VertexID)
{
    // Two triangles: (0,0) (1,0) (0,1), (0,1) (1,0) (1,1).
    const float2 corners[6] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(0, 1), float2(1, 0), float2(1, 1) };
    float2 q = corners[id];
    float2 p = lerp(rect.xy, rect.zw, q);
    VSOutput o;
    o.position = float4(p.x / screen.x * 2 - 1, 1 - p.y / screen.y * 2, 0, 1);
    o.local = q;
    return o;
}

cbuffer FragmentUniforms : register(b0, space3)
{
    float4 colour;     // the widget's colour x its render context, 0..1
    float4 region;     // texture coordinates (0..1) of the region: u0, v0, u1, v1
    float4 tiling;     // x, y: region repeats across the quad (0 = stretched); z: 1 = wrap the region coordinates (animated regions)
    float4 rotation;   // cos, sin, anchor x, anchor y (bitmap animation rotation of the quad's 0..1 coordinates)
    float4 mouse;      // mouse mask: centre x, y (pixels), size (pixels)
    float4 flags;      // x: mask texture, y: 1 mouse mask (rgb), 2 radar mask (rgba; same placement), z: output alpha 1 (ONE / ONE), w: rotate
};

[[vk::combinedImageSampler]] Texture2D mainTexture : register(t0, space2);
[[vk::combinedImageSampler]] SamplerState mainSampler : register(s0, space2);
[[vk::combinedImageSampler]] Texture2D maskTexture : register(t1, space2);
[[vk::combinedImageSampler]] SamplerState maskSampler : register(s1, space2);
[[vk::combinedImageSampler]] Texture2D mouseTexture : register(t2, space2);
[[vk::combinedImageSampler]] SamplerState mouseSampler : register(s2, space2);

float4 psMain(VSOutput input) : SV_Target
{
    float2 q = input.local;
    float2 r = q;
    if (flags.w > 0.5)
    {
        float2 d = q - rotation.zw;
        r = float2(d.x * rotation.x - d.y * rotation.y, d.x * rotation.y + d.y * rotation.x) + rotation.zw;
    }
    if (tiling.x > 0) r.x = frac(q.x * tiling.x);
    if (tiling.y > 0) r.y = frac(q.y * tiling.y);
    float2 uv = lerp(region.xy, region.zw, r);
    if (tiling.z > 0.5) uv = frac(uv);
    float4 t = mainTexture.Sample(mainSampler, uv);
    if (flags.x > 0.5) t *= maskTexture.Sample(maskSampler, q);
    if (flags.y > 0.5)
    {
        float2 m = saturate((input.position.xy - mouse.xy) / mouse.z + 0.5);
        if (flags.y > 1.5)
            t *= mouseTexture.Sample(mouseSampler, m);
        else
            t.rgb *= mouseTexture.Sample(mouseSampler, m).rgb;
    }
    return float4(t.rgb * colour.rgb, flags.z > 0.5 ? 1.0 : t.a * colour.a);
}
