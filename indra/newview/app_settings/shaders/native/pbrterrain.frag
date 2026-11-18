// pbrterrain.frag: the PBR terrain of the G-buffer pass: up to four glTF materials (base colour, and by the detail level metallic / roughness, normal, emissive) mixed by the heightmap with noise or by the paint
// map, planar or triplanar mapped. Writes the viewer's targets: albedo (linear), occlusion / roughness / metallic with the mixed base colour factor alpha, the octahedral normal with the flag HAS_PBR (0.67),
// and the emissive target (RenderEnableEmissiveBuffer). Matches "Deferred PBR Terrain Shader ... pull" (pbrterrainF.glsl and pbrterrainUtilF.glsl). Pairs with pbrterrain.vert.
// What is different from the old program (the numbers are the same):
//  - the materials are a loop over four, not four copies of the same block;
//  - the texture coordinates of a material are made here from the object space position, only for the materials the pixel uses (the old vertex shader made all four, 8 to 40 interpolated floats, and the tangents
//    of all four): the transforms are affine in the position, so the interpolation gives the same coordinates;
//  - the fetches are conditional (a material with no weight is not read, a slice of the triplanar mapping with no weight is not read), so their derivatives are taken from the position once, before the branches
//    (textureGrad), and are the same as the unconditional ones.
// Defines: TERRAIN_PBR_DETAIL (0 emissive, -1 occlusion, -2 normal, -3 metallic roughness, -4 base colour only), TERRAIN_PAINT_TYPE (0 heightmap with noise, 1 paint map),
// TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT (1 planar, 3 triplanar), TERRAIN_TRIPLANAR_BLEND_FACTOR, PBR_TERRAIN_PASS.

#include "alpha_pass.inc"
#include "lib/color.glsl"
#include "lib/clip.glsl"

#define TERRAIN_PBR_DETAIL_EMISSIVE 0
#define TERRAIN_PBR_DETAIL_OCCLUSION -1
#define TERRAIN_PBR_DETAIL_NORMAL -2
#define TERRAIN_PBR_DETAIL_METALLIC_ROUGHNESS -3

#define TERRAIN_PAINT_TYPE_HEIGHTMAP_WITH_NOISE 0
#define TERRAIN_PAINT_TYPE_PBR_PAINTMAP 1

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
#if TERRAIN_PAINT_TYPE == TERRAIN_PAINT_TYPE_HEIGHTMAP_WITH_NOISE
    uint ll_ti_alpha_ramp;
#else
    uint ll_ti_paint_map;
#endif
    uint ll_ti_detail_0_base_color;
    uint ll_ti_detail_1_base_color;
    uint ll_ti_detail_2_base_color;
    uint ll_ti_detail_3_base_color;
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_NORMAL)
    uint ll_ti_detail_0_normal;
    uint ll_ti_detail_1_normal;
    uint ll_ti_detail_2_normal;
    uint ll_ti_detail_3_normal;
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_METALLIC_ROUGHNESS)
    uint ll_ti_detail_0_metallic_roughness;
    uint ll_ti_detail_1_metallic_roughness;
    uint ll_ti_detail_2_metallic_roughness;
    uint ll_ti_detail_3_metallic_roughness;
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_EMISSIVE)
    uint ll_ti_detail_0_emissive;
    uint ll_ti_detail_1_emissive;
    uint ll_ti_detail_2_emissive;
    uint ll_ti_detail_3_emissive;
#endif
};

layout(location = 0) in vec3 vary_position;
layout(location = 1) in vec3 vary_normal;
layout(location = 2) in vec3 vary_object;
#if TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT == 3
layout(location = 3) in vec3 vary_object_normal;
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_NORMAL)
layout(location = 4) in vec3 vary_tangent;
layout(location = 5) flat in float vary_tangent_sign;
#endif
#if TERRAIN_PAINT_TYPE == TERRAIN_PAINT_TYPE_HEIGHTMAP_WITH_NOISE
layout(location = 6) in vec2 vary_tc;
#endif

layout(location = 0) out vec4 frag_albedo;
layout(location = 1) out vec4 frag_orm;
layout(location = 2) out vec4 frag_normal;
#if defined(HAS_EMISSIVE)
layout(location = 3) out vec4 frag_emissive;
#endif
// the motion target (attachment 4 of the world pass, RG16 unorm, only when an upscaler is on; an output no attachment backs is dropped): this surface does not move by itself, so it writes the value the clear holds, 1.0 in x = "no motion of its own" (the motion pass then takes the camera motion)
layout(location = 4) out vec2 frag_velocity;

// A relatively aggressive threshold for terrain material mixing: only one or two materials are used in most places. Should be greater than 0.
#define TERRAIN_RAMP_MIX_THRESHOLD 0.1
// A small threshold for the triplanar sampling cutoff: only one or two slices a texture in most places.
#define TERRAIN_TRIPLANAR_MIX_THRESHOLD 0.01

// ---- the material mix: what a pixel takes from each of the four materials ----

struct PBRMix
{
    vec4 col;       // RGB colour with alpha, linear
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_OCCLUSION)
    vec3 orm;       // occlusion, roughness, metallic
#elif (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_METALLIC_ROUGHNESS)
    vec2 rm;        // roughness, metallic
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_NORMAL)
    vec3 vNt;       // the unpacked normal texture sample
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_EMISSIVE)
    vec3 emissive;  // linear
#endif
};

PBRMix init_pbr_mix()
{
    PBRMix mix;
    mix.col = vec4(0.0);
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_OCCLUSION)
    mix.orm = vec3(0.0);
#elif (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_METALLIC_ROUGHNESS)
    mix.rm = vec2(0.0);
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_NORMAL)
    mix.vNt = vec3(0.0);
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_EMISSIVE)
    mix.emissive = vec3(0.0);
#endif
    return mix;
}

PBRMix mix_pbr(PBRMix mix1, PBRMix mix2, float mix2_weight)
{
    PBRMix mix;
    mix.col = mix1.col + (mix2.col * mix2_weight);
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_OCCLUSION)
    mix.orm = mix1.orm + (mix2.orm * mix2_weight);
#elif (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_METALLIC_ROUGHNESS)
    mix.rm = mix1.rm + (mix2.rm * mix2_weight);
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_NORMAL)
    mix.vNt = mix1.vNt + (mix2.vNt * mix2_weight);
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_EMISSIVE)
    mix.emissive = mix1.emissive + (mix2.emissive * mix2_weight);
#endif
    return mix;
}

// the texture slots of material m
uint slot_base_color(int m) { return m == 0 ? ll_ti_detail_0_base_color : (m == 1 ? ll_ti_detail_1_base_color : (m == 2 ? ll_ti_detail_2_base_color : ll_ti_detail_3_base_color)); }
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_NORMAL)
uint slot_normal(int m) { return m == 0 ? ll_ti_detail_0_normal : (m == 1 ? ll_ti_detail_1_normal : (m == 2 ? ll_ti_detail_2_normal : ll_ti_detail_3_normal)); }
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_METALLIC_ROUGHNESS)
uint slot_orm(int m) { return m == 0 ? ll_ti_detail_0_metallic_roughness : (m == 1 ? ll_ti_detail_1_metallic_roughness : (m == 2 ? ll_ti_detail_2_metallic_roughness : ll_ti_detail_3_metallic_roughness)); }
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_EMISSIVE)
uint slot_emissive(int m) { return m == 0 ? ll_ti_detail_0_emissive : (m == 1 ? ll_ti_detail_1_emissive : (m == 2 ? ll_ti_detail_2_emissive : ll_ti_detail_3_emissive)); }
#endif

// one sample of material m at uv with the given derivatives
PBRMix sample_pbr(int m, vec2 uv, vec2 duvdx, vec2 duvdy)
{
    PBRMix mix;
    mix.col = textureGrad(ll_tbl2d[nonuniformEXT(LL_HM(slot_base_color(m)))], uv, duvdx, duvdy);
    mix.col.rgb = srgb_to_linear(mix.col.rgb);
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_OCCLUSION)
    mix.orm = textureGrad(ll_tbl2d[nonuniformEXT(LL_HM(slot_orm(m)))], uv, duvdx, duvdy).xyz;
#elif (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_METALLIC_ROUGHNESS)
    mix.rm = textureGrad(ll_tbl2d[nonuniformEXT(LL_HM(slot_orm(m)))], uv, duvdx, duvdy).yz;
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_NORMAL)
    mix.vNt = textureGrad(ll_tbl2d[nonuniformEXT(LL_HM(slot_normal(m)))], uv, duvdx, duvdy).xyz * 2.0 - 1.0;
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_EMISSIVE)
    // (the caller multiplies it by the material's emissive colour, which is black for almost every terrain material: the map is not read then; explicit derivatives, so a branch is fine)
    mix.emissive = vec3(0.0);
    if (emissiveColors[m] != vec3(0.0))
    {
        mix.emissive = srgb_to_linear(textureGrad(ll_tbl2d[nonuniformEXT(LL_HM(slot_emissive(m)))], uv, duvdx, duvdy).xyz);
    }
#endif
    return mix;
}

// ---- the texture transform of material m (KHR_texture_transform without the offset of the coordinate system change: the old terrain_texture_transform) ----
// terrain_texture_transforms packs the four materials as 5 floats each: scale (2), rotation, offset (2)

float tt_float(int i) { return terrain_texture_transforms[i >> 2][i & 3]; }

vec2 terrain_uv(int m, vec2 v)
{
    const vec2 scale = vec2(tt_float(5 * m), tt_float(5 * m + 1));
    const float rotation = tt_float(5 * m + 2);
    const vec2 offset = vec2(tt_float(5 * m + 3), tt_float(5 * m + 4));
    v.y = -v.y;
    const mat3 scale_mat = mat3(scale.x, 0.0, 0.0, 0.0, scale.y, 0.0, 0.0, 0.0, 1.0);
    const mat3 offset_mat = mat3(1.0, 0.0, 0.0, 0.0, 1.0, 0.0, offset.x, offset.y, 1.0);
    const mat3 rotation_mat = mat3(cos(rotation), -sin(rotation), 0.0, sin(rotation), cos(rotation), 0.0, 0.0, 0.0, 1.0);
    vec2 r = ((offset_mat * rotation_mat * scale_mat) * vec3(v, 1.0)).xy;
    r.y = -r.y;
    return r;
}

// the same for a difference of coordinates (the transform is affine: its linear part)
vec2 terrain_uv_delta(int m, vec2 d)
{
    return terrain_uv(m, d) - terrain_uv(m, vec2(0.0));
}

#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_NORMAL)
// the tangent of material m: the rotation and the scale sign of its transform turn the vertex tangent in the plane of the surface (the old terrain_tangent_space_transform); returns the unit tangent
// and the sign of the binormal
vec3 terrain_tangent(int m, vec3 t, vec3 n, out float tangent_sign)
{
    const vec2 scale = vec2(tt_float(5 * m), tt_float(5 * m + 1));
    const float rotation = -tt_float(5 * m + 2);
    vec2 weights = mat2(cos(rotation), -sin(rotation), sin(rotation), cos(rotation)) * vec2(1.0, 0.0);
    const vec2 scale_sign = sign(scale);
    weights *= scale_sign;
    weights.x += 1.0 - abs(sign(sign(weights.x) + (0.5 * sign(weights.y))));     // the default when 0
    weights.y = -weights.y;
    const vec3 binormal = vary_tangent_sign * cross(n, t);
    tangent_sign = vary_tangent_sign * (scale_sign.x * scale_sign.y);
    return normalize(weights.x * t + weights.y * binormal);
}
#endif

// ---- triplanar mapping (adapted from Inigo Quilez' example shader, MIT license, https://www.shadertoy.com/view/MtsGWH, Copyright 2015 Inigo Quilez; as the old pbrterrainUtilF.glsl) ----
#if TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT == 3

// the slice coordinate before the material's transform, and its change over a pixel: axis 0 = x (yz), 1 = y (xz), 2 = z (xy); a slice seen from behind is flipped (z: no coordinate)
vec2 slice_coord(int axis, vec3 p)
{
    if (axis == 0) { return vary_object_normal.x > 0.0 ? p.yz : p.yz * vec2(-1.0, 1.0); }
    if (axis == 1) { return vary_object_normal.y > 0.0 ? p.xz * vec2(-1.0, 1.0) : p.xz; }
    return vary_object_normal.z > 0.0 ? p.xy : vec2(0.0);
}

// the normal texture fixes of the slices
vec3 _t_normal_post_1(vec3 vNt0, float sign_or_zero)
{
    vec3 vNt1 = vNt0;
    float sgn = (2.0 * sign_or_zero) + 1.0;
    sgn /= abs(sgn);
    vNt1.xy = (min(0.0, sgn) * vNt1.xy) + (min(0.0, -sgn) * -vNt1.xy);
    return vNt1;
}

#endif

// the mix of material m at the pixel (the object position p, its derivatives dpdx / dpdy)
PBRMix terrain_sample_and_multiply_pbr(int m, vec3 p, vec3 dpdx, vec3 dpdy, float tangent_sign)
{
    PBRMix mix = init_pbr_mix();
#if TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT == 3
    float sharpness = TERRAIN_TRIPLANAR_BLEND_FACTOR;
    vec3 weight_signed = pow(abs(vary_object_normal), vec3(sharpness));
    weight_signed /= (weight_signed.x + weight_signed.y + weight_signed.z);
    weight_signed -= vec3(TERRAIN_TRIPLANAR_MIX_THRESHOLD);
    vec3 tw = max(vec3(0.0), weight_signed);
    tw /= (tw.x + tw.y + tw.z);
    for (int axis = 0; axis < 3; ++axis)
    {
        if (weight_signed[axis] <= 0.0)
        {
            continue;
        }
        const vec2 raw = slice_coord(axis, p);
        const bool absent = axis == 2 && vary_object_normal.z <= 0.0;       // (the bottom slice has no coordinate: the texel at 0, no change over the pixel)
        const vec2 uv = absent ? vec2(0.0) : terrain_uv(m, raw);
        const vec2 duvdx = absent ? vec2(0.0) : terrain_uv_delta(m, slice_coord(axis, dpdx));
        const vec2 duvdy = absent ? vec2(0.0) : terrain_uv_delta(m, slice_coord(axis, dpdy));
        PBRMix s = sample_pbr(m, uv, duvdx, duvdy);
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_NORMAL)
        if (axis == 0)
        {
            vec3 v = _t_normal_post_1(s.vNt, sign(vary_object_normal.x));
            v.xy = vec2(-v.y, v.x);         // transform the normals according to the orientation of the UVs
            v.xy *= tangent_sign;
            s.vNt = v;
        }
        else if (axis == 1)
        {
            vec3 v = _t_normal_post_1(s.vNt, sign(vary_object_normal.y));
            v.xy = -v.xy;
            s.vNt = v;
        }
        else
        {
            s.vNt = _t_normal_post_1(s.vNt, sign(vary_object_normal.z));        // (the bottom face has not been tested)
        }
#endif
        mix = mix_pbr(mix, s, tw[axis]);
    }
#else
    const vec2 uv = terrain_uv(m, p.xy);
    mix = sample_pbr(m, uv, terrain_uv_delta(m, dpdx.xy), terrain_uv_delta(m, dpdy.xy));
#endif
    // the factors of the material
    mix.col *= baseColorFactors[m];
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_OCCLUSION)
    mix.orm *= vec3(1.0, roughnessFactors[m], metallicFactors[m]);
#elif (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_METALLIC_ROUGHNESS)
    mix.rm *= vec2(roughnessFactors[m], metallicFactors[m]);
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_EMISSIVE)
    mix.emissive *= emissiveColors[m];
#endif
    return mix;
}

// ---- the weights of the materials ----

const int kMixBit = 8;       // the usage bit of material 0 (1 << 3); material m is kMixBit << m

struct TerrainMix
{
    vec4 weight;
    int type;
};

TerrainMix get_terrain_mix_weights(float alpha1, float alpha2, float alphaFinal)
{
    TerrainMix tm;
    const vec4 sample_x = vec4(1.0, 0.0, 0.0, 0.0);
    const vec4 sample_y = vec4(0.0, 1.0, 0.0, 0.0);
    const vec4 sample_z = vec4(0.0, 0.0, 1.0, 0.0);
    const vec4 sample_w = vec4(0.0, 0.0, 0.0, 1.0);
    tm.weight = mix(mix(sample_w, sample_z, alpha2), mix(sample_y, sample_x, alpha1), alphaFinal);
    tm.weight -= TERRAIN_RAMP_MIX_THRESHOLD;
    const ivec4 usage = max(ivec4(0), ivec4(ceil(tm.weight)));
    tm.weight = tm.weight * vec4(usage);                    // (no negative weights, and the rest stays balanced)
    tm.weight /= (tm.weight.x + tm.weight.y + tm.weight.z + tm.weight.w);
    tm.type = (usage.x * kMixBit) | (usage.y * (kMixBit << 1)) | (usage.z * (kMixBit << 2)) | (usage.w * (kMixBit << 3));
    return tm;
}

TerrainMix get_terrain_usage_from_weight3(vec3 weight3)
{
    weight3 = max(vec3(0.0), weight3);
    weight3 /= max(1.0, weight3.x + weight3.y + weight3.z);
    TerrainMix tm;
    tm.weight.x = 1.0 - (weight3.x + weight3.y + weight3.z);        // the first weight is what the others leave
    tm.weight.yzw = weight3;
    const ivec4 usage = max(ivec4(0), ivec4(ceil(tm.weight)));
    tm.type = (usage.x * kMixBit) | (usage.y * (kMixBit << 1)) | (usage.z * (kMixBit << 2)) | (usage.w * (kMixBit << 3));
    return tm;
}

float terrain_mix(TerrainMix tm, vec4 tms4)
{
    return (tm.weight.x * tms4[0]) + (tm.weight.y * tms4[1]) + (tm.weight.z * tms4[2]) + (tm.weight.w * tms4[3]);
}

void main()
{
    frag_velocity = vec2(1.0, 0.0);
    mirror_clip(vary_position);

    // the derivatives of the object position, before any branch
    const vec3 dpdx = dFdx(vary_object);
    const vec3 dpdy = dFdy(vary_object);

    TerrainMix tm;
#if TERRAIN_PAINT_TYPE == TERRAIN_PAINT_TYPE_HEIGHTMAP_WITH_NOISE
    const float alpha1 = texture(ll_tbl2d[nonuniformEXT(LL_HM(ll_ti_alpha_ramp))], vary_tc).a;
    const float alpha2 = texture(ll_tbl2d[nonuniformEXT(LL_HM(ll_ti_alpha_ramp))], vary_tc - vec2(2.0, 0.0)).a;
    const float alphaFinal = texture(ll_tbl2d[nonuniformEXT(LL_HM(ll_ti_alpha_ramp))], vary_tc - vec2(1.0, 0.0)).a;
    tm = get_terrain_mix_weights(alpha1, alpha2, alphaFinal);
#else
    tm = get_terrain_usage_from_weight3(texture(ll_tbl2d[nonuniformEXT(LL_HM(ll_ti_paint_map))], vary_object.xy / region_scale).xyz);
#endif

    PBRMix pbr_mix = init_pbr_mix();
    for (int m = 0; m < 4; ++m)
    {
        if ((tm.type & (kMixBit << m)) == 0)
        {
            continue;
        }
        float tangent_sign = 1.0;
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_NORMAL)
        vec3 tangent = vec3(1.0, 0.0, 0.0);
        tangent = terrain_tangent(m, vary_tangent, vary_normal, tangent_sign);
#endif
        PBRMix mix2 = terrain_sample_and_multiply_pbr(m, vary_object, dpdx, dpdy, tangent_sign);
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_NORMAL)
        {
            // mikktspace: the normal texture's vector in the frame of the material's tangent
            const vec3 binormal = tangent_sign * cross(vary_normal, tangent);
            mix2.vNt = normalize(mix2.vNt.x * tangent + mix2.vNt.y * binormal + mix2.vNt.z * vary_normal) * (gl_FrontFacing ? 1.0 : -1.0);
        }
#endif
        pbr_mix = mix_pbr(pbr_mix, mix2, tm.weight[m]);
    }

    const float terrain_cutoff = terrain_mix(tm, minimum_alphas);
    if (pbr_mix.col.a < terrain_cutoff)
    {
        discard;
    }
    const float base_color_factor_alpha = terrain_mix(tm, vec4(baseColorFactors[0].z, baseColorFactors[1].z, baseColorFactors[2].z, baseColorFactors[3].z));

#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_NORMAL)
    const vec3 tnorm = normalize(pbr_mix.vNt);
#else
    const vec3 tnorm = vary_normal * (gl_FrontFacing ? 1.0 : -1.0);
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_OCCLUSION)
    const vec3 mix_orm = pbr_mix.orm;
#elif (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_METALLIC_ROUGHNESS)
    const vec3 mix_orm = vec3(1.0, pbr_mix.rm);
#else
    const vec3 mix_orm = vec3(1.0, 1.0, 0.0);               // matte plastic potato terrain
#endif

    frag_albedo = max(vec4(pbr_mix.col.xyz, 0.0), vec4(0.0));
    frag_orm = max(vec4(mix_orm, base_color_factor_alpha), vec4(0.0));
    frag_normal = vec4(tnorm.xy / sqrt(8.0 * tnorm.z + 8.0) + 0.5, 0.0, 0.67);      // (the octahedral normal, the environment intensity 0, the flag HAS_PBR)
#if defined(HAS_EMISSIVE)
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_EMISSIVE)
    frag_emissive = max(vec4(pbr_mix.emissive, 0.0), vec4(0.0));
#else
    frag_emissive = vec4(0.0);
#endif
#endif
}
