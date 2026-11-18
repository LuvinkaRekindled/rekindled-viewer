// terrain_bake.frag: the PBR terrain's paint bake: the weights of the materials from the alpha ramp, written as the paint map's colour (the manifest lines name the bit depth: TERRAIN_PAINT_PRECISION). A line for
// line translation of "Terrain Bake Shader" (interface/pbrTerrainBakeF.glsl; get_terrain_mix_weights of deferred/pbrterrainUtilF.glsl, as native/pbrterrain.frag has it); pairs with terrain_bake.vert.
// (the draw block is declared the same in both stages)

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    mat4 modelview_projection_matrix;
    uint ll_ti_alpha_ramp;
};

layout(location = 0) in vec4 vary_texcoord0;
layout(location = 1) in vec4 vary_texcoord1;
layout(location = 0) out vec4 frag_color;

// A relatively agressive threshold for terrain material mixing sampling cutoff. This ensures that only one or two materials are used in most places, making PBR terrain blending more performant.
// Should be greater than 0 to work.
#define TERRAIN_RAMP_MIX_THRESHOLD 0.1

const int kMixBit = 8;       // the usage bit of material 0 (1 << 3); material m is kMixBit << m

struct TerrainMix
{
    vec4 weight;
    int type;
};

TerrainMix get_terrain_mix_weights(float alpha1, float alpha2, float alphaFinal)
{
    TerrainMix tm;
    vec4 sample_x = vec4(1,0,0,0);
    vec4 sample_y = vec4(0,1,0,0);
    vec4 sample_z = vec4(0,0,1,0);
    vec4 sample_w = vec4(0,0,0,1);
    tm.weight = mix( mix(sample_w, sample_z, alpha2), mix(sample_y, sample_x, alpha1), alphaFinal );
    tm.weight -= TERRAIN_RAMP_MIX_THRESHOLD;
    ivec4 usage = max(ivec4(0), ivec4(ceil(tm.weight)));
    // Prevent negative weights and keep weights balanced
    tm.weight = tm.weight*vec4(usage);
    tm.weight /= (tm.weight.x + tm.weight.y + tm.weight.z + tm.weight.w);
    tm.type = (usage.x * kMixBit) | (usage.y * (kMixBit << 1)) | (usage.z * (kMixBit << 2)) | (usage.w * (kMixBit << 3));
    return tm;
}

void main()
{
    TerrainMix tm;
    float alpha1 = texture(ll_tbl2d[LL_HM(ll_ti_alpha_ramp)], vary_texcoord0.zw).a;
    float alpha2 = texture(ll_tbl2d[LL_HM(ll_ti_alpha_ramp)], vary_texcoord1.xy).a;
    float alphaFinal = texture(ll_tbl2d[LL_HM(ll_ti_alpha_ramp)], vary_texcoord1.zw).a;
    tm = get_terrain_mix_weights(alpha1, alpha2, alphaFinal);

    // tm.weight.x can be ignored. The paintmap saves a channel by not storing
    // swatch 1, and assuming the weights of the 4 swatches add to 1.

    // TERRAIN_PAINT_PRECISION emulates loss of precision at lower bit depth
    // when a corresponding low-bit image format is not available. Because
    // integral values at one depth cannot be precisely represented at another
    // bit depth, rounding is required. To maximize numerical stability for
    // future conversions, bit depth conversions should round to the nearest
    // integer, not floor or ceil.
    frag_color = max(vec4(round(tm.weight.yzw * TERRAIN_PAINT_PRECISION) / TERRAIN_PAINT_PRECISION, 1.0), vec4(0));
}
