// dnsr_sunpack.frag: the hit mask the AMD FidelityFX shadow denoiser takes (PLAN-amd-denoiser.md): one 32 bit value for every tile of 8 x 4 pixels of the sun lightmap's centre ray result, bit
// (y % 4) * 8 + (x % 8) set where the pixel is in shadow (the denoiser reads a set bit as "the ray hit an occluder"). The target is RGBA8 and one texel a tile: the four bytes of the 32 bit value are its
// four channels, byte k = row k of the tile, the lowest bit of a byte the leftmost pixel (the wrapper copies the target into an R32_UINT image: the two formats are copy compatible, the bytes stay as
// they are). Pairs with fullscreen.vert.

layout(std140, set = 0, binding = 0) uniform LLLooseUniforms
{
    vec2 tile_count;            // the size of this target: the lightmap's width / 8 and height / 4, rounded up
};

layout(std140, set = 0, binding = 5) uniform LLLooseDraw
{
    uint ll_ti_diffuseMap;      // the sun lightmap (r: the sun shadow, 1 lit .. 0 shadowed)
};

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 0) out vec4 frag_color;

void main()
{
    const ivec2 size = textureSize(ll_tbl2d[LL_HM(ll_ti_diffuseMap)], 0);
    const ivec2 tile = ivec2(floor(vary_fragcoord * tile_count));
    float bytes[4];
    for (int row = 0; row < 4; ++row)
    {
        int bits = 0;
        for (int x = 0; x < 8; ++x)
        {
            const ivec2 p = tile * ivec2(8, 4) + ivec2(x, row);
            if (p.x < size.x && p.y < size.y && texelFetch(ll_tbl2d[LL_HM(ll_ti_diffuseMap)], p, 0).r < 0.5)
            {
                bits |= 1 << x;
            }
        }
        bytes[row] = float(bits) / 255.0;
    }
    frag_color = vec4(bytes[0], bytes[1], bytes[2], bytes[3]);
}
