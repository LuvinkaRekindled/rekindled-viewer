// rt_reflection.frag: "Deferred RT Reflection Shader" (deferred/rtReflectionF.glsl with its library files, vertex stage softenLightV: fullscreen.vert); the manifest line defines HAS_SUN_SHADOW, LL_RT_REFLECTIONS, LL_RT_QUERY. The stock text as it is, from the stock loader's attach order (llshadermgr.cpp); the declarations (the loose blocks, the texture macros) are the stock translator's for this program
// (D:\claude\freeze_stock.py). GENERATED, then kept as the program: edit this file. The stock loader's globals the stock program did not see (the native-only HAS_SSAO) are undefined below.

#undef HAS_SSAO
#undef TRANSPARENT_WATER

#define transparent_surface (transparent_surface_ll_b != 0u)
layout(std140, set = 0, binding = 0) uniform LLLooseUniforms
{
    vec2 screen_res;
    float blur_size;
    float blur_fidelity;
    vec4 clipPlane;
    mat3 env_mat;
    mat3 ssao_effect_mat;
    vec3 sun_dir;
    vec3 moon_dir;
    int sun_up_factor;
    int classic_mode;
    mat4 inv_proj;
    vec4 waterPlane;
    int cube_snapshot;
    float sky_hdr_scale;
    int rt_glossy_reflections;
    float rr_hit_distance;
    int rt_hit_quality;
    float max_probe_lod;
    int rt_hit_light_count;
    vec4 rt_hit_light_pos[8];
    vec4 rt_hit_light_col[8];
    vec4 rt_hit_light_dir[8];
    int rt_hit_spot;
    vec3 rt_hit_spot_center;
    float scene_light_strength;
    vec3 sunlight_color;
    vec3 moonlight_color;
    vec3 ambient_color;
    vec3 blue_horizon;
    vec3 blue_density;
    float haze_horizon;
    float haze_density;
    float cloud_shadow;
    float density_multiplier;
    float distance_multiplier;
    float max_y;
    vec3 glow;
    float sun_moon_glow_factor;
    float sky_sunlight_scale;
    float sky_ambient_scale;
    float mirror_flag;
    float clipSign;
    float waterSign;
    mat4 projection_matrix;
    mat4 modelview_delta;
    mat4 inv_modelview_delta;
    float iterationCount;
    float rayStep;
    float distanceBias;
    float depthRejectBias;
    float glossySampleCount;
    float adaptiveStepMultiplier;
    float ssrTraceMode;
    vec3 POISSON3D_SAMPLES[128];
    vec2 shadow_res;
    mat4 shadow_matrix[6];
    vec4 shadow_clip;
    float shadow_bias;
    float shadow_offset;
    float rt_sun_radius;
    int rt_sun_samples;
    float rt_sun_alpha;
    float rt_sun_maxdist;
    vec2 rt_sun_lod;
    float rt_noise_frame;
    float rt_sun_strength;
    int rt_sun_pen_rays;
    float rt_sun_self_off;
    int rt_hit_shadow_variant;
    int rt_hair_soft;
    int rt_hit_shadow_samples;
    uint transparent_surface_ll_b;
    vec4 waterFogColor;
    float waterFogDensity;
    float waterFogKS;
};
layout(std140, set = 0, binding = 5) uniform LLLooseDraw
{
    uint ll_ti_lightMap;
    uint ll_ti_lightFunc;
    uint ll_ti_normalMap;
    uint ll_ti_depthMap;
    uint ll_ti_brdfLut;
    uint ll_ti_projectionMap;
    uint ll_ti_diffuseRect;
    uint ll_ti_specularRect;
    uint ll_ti_emissiveRect;
    uint ll_ti_sceneMap;
    uint ll_ti_sceneDepth;
    uint ll_ti_shadowMap0;
    uint ll_ti_shadowMap1;
    uint ll_ti_shadowMap2;
    uint ll_ti_shadowMap3;
    uint ll_ti_shadowMap4;
    uint ll_ti_shadowMap5;
    uint ll_ti_reflectionProbes;
    uint ll_ti_irradianceProbes;
    uint ll_ti_heroProbes;
    uint ll_ti_mirrorView;
};
layout(std140, set = 0, binding = 6) uniform LLLooseMaterial
{
    vec3 color;
};
layout(std140, set = 0, binding = 7) uniform LLLooseLight
{
    vec3 proj_origin;
    float proj_ambiance;
    float proj_lod;
    float proj_focus;
    float proj_range;
    float falloff;
    vec3 lightnorm;
    mat4 proj_mat;
    vec3 proj_n;
    vec3 proj_p;
    float size;
    float noiseSine;
    vec2 proj_shadow_res;
    float spot_shadow_bias;
    float spot_shadow_offset;
};
#define lightMap ll_tbl2d[LL_HM(ll_ti_lightMap)]
#define lightFunc ll_tbl2d[LL_HM(ll_ti_lightFunc)]
#define normalMap ll_tbl2d[LL_HM(ll_ti_normalMap)]
#define depthMap ll_tbl2d[LL_HM(ll_ti_depthMap)]
#define brdfLut ll_tbl2d[LL_HM(ll_ti_brdfLut)]
#define projectionMap ll_tbl2d[LL_HM(ll_ti_projectionMap)]
#define diffuseRect ll_tbl2d[LL_HM(ll_ti_diffuseRect)]
#define specularRect ll_tbl2d[LL_HM(ll_ti_specularRect)]
#define emissiveRect ll_tbl2d[LL_HM(ll_ti_emissiveRect)]
#define sceneMap ll_tbl2d[LL_HM(ll_ti_sceneMap)]
#define sceneDepth ll_tbl2d[LL_HM(ll_ti_sceneDepth)]
#define shadowMap0 ll_tbl2dshadow[LL_HM(ll_ti_shadowMap0)]
#define shadowMap1 ll_tbl2dshadow[LL_HM(ll_ti_shadowMap1)]
#define shadowMap2 ll_tbl2dshadow[LL_HM(ll_ti_shadowMap2)]
#define shadowMap3 ll_tbl2dshadow[LL_HM(ll_ti_shadowMap3)]
#define shadowMap4 ll_tbl2dshadow[LL_HM(ll_ti_shadowMap4)]
#define shadowMap5 ll_tbl2dshadow[LL_HM(ll_ti_shadowMap5)]
#define reflectionProbes ll_tblcubearray[LL_HM(ll_ti_reflectionProbes)]
#define irradianceProbes ll_tblcubearray[LL_HM(ll_ti_irradianceProbes)]
#define heroProbes ll_tblcubearray[LL_HM(ll_ti_heroProbes)]
#define mirrorView ll_tbl2d[LL_HM(ll_ti_mirrorView)]

struct GBufferInfo { vec4 albedo; vec4 specular; vec3 normal; vec4 emissive; float gbufferFlag; float envIntensity; };
layout(location = 0) in vec2 vary_fragcoord;
layout(location = 0) out vec4 frag_data[2];

#include "lib/flags.glsl"
#ifdef LL_RT_QUERY
#include "lib/rt_scene.glsl"
#endif

// ======== class1/deferred/globalF.glsl ========
// Global helper functions included in every fragment shader
 // DO NOT declare sampler uniforms here as OS X doesn't compile
 // them out


void mirrorClip(vec3 pos)
{
    if (mirror_flag > 0)
    {
        if ((dot(pos.xyz, clipPlane.xyz) + clipPlane.w) < 0.0)
        {
                discard;
        }
    }
}

vec4 encodeNormal(vec3 n, float env, float gbuffer_flag)
{
    float f = sqrt(8 * n.z + 8);
    return vec4(n.xy / f + 0.5, env, gbuffer_flag);
}

vec4 decodeNormal(vec4 norm)
{
    vec2 fenc = norm.xy*4-2;
    float f = dot(fenc,fenc);
    float g = sqrt(1-f/4);
    vec4 n;
    n.xy = fenc*g;
    n.z = 1-f/2;
    return n;
}


// ======== class1/environment/srgbF.glsl ========
vec3 srgb_to_linear(vec3 cs)
{
    vec3 low_range = cs / vec3(12.92);
    vec3 high_range = pow((cs+vec3(0.055))/vec3(1.055), vec3(2.4));
    bvec3 lte = lessThanEqual(cs,vec3(0.04045));

#ifdef OLD_SELECT
    vec3 result;
    result.r = lte.r ? low_range.r : high_range.r;
    result.g = lte.g ? low_range.g : high_range.g;
    result.b = lte.b ? low_range.b : high_range.b;
    return result;
#else
    return mix(high_range, low_range, lte);
#endif

}


vec4 srgb_to_linear4(vec4 cs)
{
    vec4 low_range = cs / vec4(12.92);
    vec4 high_range = pow((cs+vec4(0.055))/vec4(1.055), vec4(2.4));
    bvec4 lte = lessThanEqual(cs,vec4(0.04045));

#ifdef OLD_SELECT
    vec4 result;
    result.r = lte.r ? low_range.r : high_range.r;
    result.g = lte.g ? low_range.g : high_range.g;
    result.b = lte.b ? low_range.b : high_range.b;
    result.a = lte.a ? low_range.a : high_range.a;
    return result;
#else
    return mix(high_range, low_range, lte);
#endif

}

vec3 linear_to_srgb(vec3 cl)
{
    cl = clamp(cl, vec3(0), vec3(1));
    vec3 low_range  = cl * 12.92;
    vec3 high_range = 1.055 * pow(cl, vec3(0.41666)) - 0.055;
    bvec3 lt = lessThan(cl,vec3(0.0031308));

#ifdef OLD_SELECT
    vec3 result;
    result.r = lt.r ? low_range.r : high_range.r;
    result.g = lt.g ? low_range.g : high_range.g;
    result.b = lt.b ? low_range.b : high_range.b;
    return result;
#else
    return mix(high_range, low_range, lt);
#endif

}

vec3 ColorFromRadiance(vec3 radiance)
{
    return vec3(1.0) - exp(-radiance * 0.0001);
}

vec3 rgb2hsv(vec3 c)
{
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));

    float d = q.x - min(q.w, q.y);
    float e = 1.0e-3;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

vec3 hsv2rgb(vec3 c)
{
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

const mat3 inv_ACESOutputMat = mat3(0.643038, 0.0592687, 0.0059619, 0.311187, 0.931436, 0.063929, 0.0457755, 0.00929492, 0.930118 );

const mat3 inv_ACESInputMat = mat3( 1.76474, -0.147028, -0.0363368, -0.675778, 1.16025, -0.162436, -0.0889633, -0.0132237, 1.19877 );

vec3 inv_RRTAndODTFit(vec3 x)
{
    float A = 0.0245786;
    float B = 0.000090537;
    float C = 0.983729;
    float D = 0.4329510;
    float E = 0.238081;

    return (A - D * x)/(2.0 * (C * x - 1.0)) - sqrt(pow(D * x - A, vec3(2.0)) - 4.0 * (C * x - 1.0) * (B + E * x))/(2.0 * (C * x - 1.0));
}

// experimental inverse of ACES Hill tonemapping
vec3 inv_toneMapACES_Hill(vec3 color)
{
    color = inv_ACESOutputMat * color;

    // Apply RRT and ODT
    color = inv_RRTAndODTFit(color);

    color = inv_ACESInputMat * color;

    return color;
}



// ======== class1/windlight/atmosphericsVarsF.glsl ========
vec3 getSunlitColor()
{
    return vec3(0, 0, 0);
}

vec3 getAmblitColor()
{
    return vec3(0, 0, 0);
}

vec3 getAdditiveColor()
{
    return vec3(0, 0, 0);       // (the stock vertex shader: setAdditiveColor(vec3(0)), a varying)
}

vec3 getAtmosAttenuation()
{
    return vec3(1, 1, 1);       // (setAtmosAttenuation(vec3(1)))
}


// ======== class1/windlight/atmosphericsHelpersF.glsl ========
// Output variables


vec3 atmosFragAmbient(vec3 light, vec3 amblit)
{
    return amblit + light / 2.0;
}

vec3 atmosFragAffectDirectionalLight(float lightIntensity, vec3 sunlit)
{
    return sunlit * lightIntensity;
}

vec3 scaleDownLightFrag(vec3 light)
{
    return (light / scene_light_strength );
}



// ======== class1/deferred/deferredUtil.glsl ========
/*  Parts of this file are taken from Sascha Willem's Vulkan GLTF refernce implementation
MIT License

Copyright (c) 2018 Sascha Willems

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/


#ifdef LL_PROJECTOR_PULL
// S6 LV2 (doc/vulkan/S6-census.md): the projectors of a lighting pass drawn from one record: what the old pass wrote as uniforms before each projector comes from the vertex shader as flat inputs (the
// light's row), the projected image from the bindless table by the slot in the row
#define proj_mat vary_proj_mat
#define proj_n vary_proj_n
#define proj_p vary_proj_p
#define proj_focus vary_proj_focus
#define proj_lod vary_proj_lod
#define proj_range vary_proj_range
#define proj_ambiance vary_proj_ambiance
#define classic_mode vary_light_classic
#define color vary_light_color
#define size vary_light_size
#define PROJECTION_MAP_LOD(tc, lod) textureLod(ll_bindless_textures[nonuniformEXT(LL_HM(vary_proj_slot))], tc, lod)
#define PROJECTION_MAP(tc) texture(ll_bindless_textures[nonuniformEXT(LL_HM(vary_proj_slot))], tc)
#else

// projected lighted params


// light params
#define PROJECTION_MAP_LOD(tc, lod) textureLod(projectionMap, tc, lod)
#define PROJECTION_MAP(tc) texture(projectionMap, tc)
#endif


const float M_PI = 3.14159265;
const float ONE_OVER_PI = 0.3183098861;

vec3 srgb_to_linear(vec3 cs);
vec3 linear_to_srgb(vec3 cs);
vec3 atmosFragLightingLinear(vec3 light, vec3 additive, vec3 atten);

vec4 decodeNormal(vec4 norm);

vec3 clampHDRRange(vec3 color)
{
    // Why do this?
    // There are situations where the color range will go to something insane - potentially producing infs and NaNs even.
    // This is a safety measure to prevent that.
    // As to the specific number there - allegedly some HDR displays expect values to be in the 0-11.2 range. Citation needed.
    // -Geenz 2025-03-05
    color = mix(color, vec3(1), isinf(color));
    color = mix(color, vec3(0.0), isnan(color));
    return clamp(color, vec3(0.0), vec3(11.2));
}

float calcLegacyDistanceAttenuation(float distance, float falloff)
{
    float dist_atten = 1.0 - clamp((distance + falloff)/(1.0 + falloff), 0.0, 1.0);
    dist_atten *= dist_atten;

    // Tweak falloff slightly to match pre-EEP attenuation
    // NOTE: this magic number also shows up in a great many other places, search for dist_atten *= to audit
    dist_atten *= 2.0;
    return dist_atten;
}

// In:
//   lv  unnormalized surface to light vector
//   n   normal of the surface
//   pos unnormalized camera to surface vector
// Out:
//   l   normalized surace to light vector
//   nl  diffuse angle
//   nh  specular angle
void calcHalfVectors(vec3 lv, vec3 n, vec3 v,
    out vec3 h, out vec3 l, out float nh, out float nl, out float nv, out float vh, out float lightDist)
{
    l  = normalize(lv);
    h  = normalize(l + v);

    // lower bound to avoid divide by zero
    float eps = 0.000001;
    nh = clamp(dot(n, h), eps, 1.0);
    nl = clamp(dot(n, l), eps, 1.0);
    nv = clamp(dot(n, v), eps, 1.0);
    vh = clamp(dot(v, h), eps, 1.0);

    lightDist = length(lv);
}

// In:
//   light_center
//   pos
// Out:
//   dist
//   l_dist
//   lv
//   proj_tc  Projector Textue Coordinates
bool clipProjectedLightVars(vec3 light_center, vec3 pos, out float dist, out float l_dist, out vec3 lv, out vec4 proj_tc )
{
    lv = light_center - pos.xyz;
    dist = length(lv);
    bool clipped = (dist >= size);
    if ( !clipped )
    {
        dist /= size;

        l_dist = -dot(lv, proj_n);
        vec4 projected_point = (proj_mat * vec4(pos.xyz, 1.0));
        clipped = (projected_point.z < 0.0);
        projected_point.xyz /= projected_point.w;
        proj_tc = projected_point;
    }

    return clipped;
}

vec2 getScreenCoordinate(vec2 screenpos)
{
    vec2 sc = screenpos.xy * 2.0;
    return sc - vec2(1.0, 1.0);
}

vec4 getNorm(vec2 screenpos)
{
    vec4 norm = decodeNormal(texture(normalMap, screenpos.xy));
    return norm;
}

vec4 getNormRaw(vec2 screenpos)
{
    vec4 norm = texture(normalMap, screenpos.xy);
    return norm;
}

// get linear depth value given a depth buffer sample d and znear and zfar values
float linearDepth(float d, float znear, float zfar)
{
    d = d * 2.0 - 1.0;
    return znear * 2.0 * zfar / (zfar + znear - d * (zfar - znear));
}

float linearDepth01(float d, float znear, float zfar)
{
    return linearDepth(d, znear, zfar) / zfar;
}

float getDepth(vec2 pos_screen)
{
    float depth = texture(depthMap, pos_screen).r;
    return depth;
}

vec4 getTexture2DLodAmbient(vec2 tc, float lod)
{
#ifndef FXAA_GLSL_120
    vec4 ret = PROJECTION_MAP_LOD(tc, lod);
#else
    vec4 ret = PROJECTION_MAP(tc);
#endif
    ret.rgb = srgb_to_linear(ret.rgb);

    vec2 dist = tc-vec2(0.5);
    float d = dot(dist,dist);
    ret *= min(clamp((0.25-d)/0.25, 0.0, 1.0), 1.0);

    return ret;
}

vec4 getTexture2DLodDiffuse(vec2 tc, float lod)
{
#ifndef FXAA_GLSL_120
    vec4 ret = PROJECTION_MAP_LOD(tc, lod);
#else
    vec4 ret = PROJECTION_MAP(tc);
#endif
    ret.rgb = srgb_to_linear(ret.rgb);

    vec2 dist = vec2(0.5) - abs(tc-vec2(0.5));
    float det = min(lod/(proj_lod*0.5), 1.0);
    float d = min(dist.x, dist.y);
    float edge = 0.25*det;
    ret *= clamp(d/edge, 0.0, 1.0);

    return ret;
}

// lit     This is set by the caller: if (nl > 0.0) { lit = attenuation * nl * noise; }
// Uses:
//   color   Projected spotlight color
vec3 getProjectedLightAmbiance(float amb_da, float attenuation, float lit, float nl, float noise, vec2 projected_uv)
{
    vec4 amb_plcol = getTexture2DLodAmbient(projected_uv, proj_lod);
    vec3 amb_rgb   = amb_plcol.rgb * amb_plcol.a;

    amb_da += proj_ambiance;
    amb_da += (nl*nl*0.5+0.5) * proj_ambiance;
    amb_da *= attenuation * noise;
    amb_da = min(amb_da, 1.0-lit);

    return (amb_da * color.rgb * amb_rgb);
}

// Returns projected light in Linear
// Uses global spotlight color:
//  color
// NOTE: projected.a will be pre-multiplied with projected.rgb
vec3 getProjectedLightDiffuseColor(float light_distance, vec2 projected_uv)
{
    float diff = clamp((light_distance - proj_focus)/proj_range, 0.0, 1.0);
    float lod = diff * proj_lod;
    vec4 plcol = getTexture2DLodDiffuse(projected_uv.xy, lod);

    return color.rgb * plcol.rgb * plcol.a;
}

vec4 texture2DLodSpecular(vec2 tc, float lod)
{
#ifndef FXAA_GLSL_120
    vec4 ret = PROJECTION_MAP_LOD(tc, lod);
#else
    vec4 ret = PROJECTION_MAP(tc);
#endif
    ret.rgb = srgb_to_linear(ret.rgb);

    vec2 dist = vec2(0.5) - abs(tc-vec2(0.5));
    float det = min(lod/(proj_lod*0.5), 1.0);
    float d = min(dist.x, dist.y);
    d *= min(1, d * (proj_lod - lod)); // BUG? extra factor compared to diffuse causes N repeats
    float edge = 0.25*det;
    ret *= clamp(d/edge, 0.0, 1.0);

    return ret;
}

// See: clipProjectedLightVars()
vec3 getProjectedLightSpecularColor(vec3 pos, vec3 n )
{
    vec3 slit = vec3(0);
    vec3 ref = reflect(normalize(pos), n);

    //project from point pos in direction ref to plane proj_p, proj_n
    vec3 pdelta = proj_p-pos;
    float l_dist = length(pdelta);
    float ds = dot(ref, proj_n);
    if (ds < 0.0)
    {
        vec3 pfinal = pos + ref * dot(pdelta, proj_n)/ds;
        vec4 stc = (proj_mat * vec4(pfinal.xyz, 1.0));
        if (stc.z > 0.0)
        {
            stc /= stc.w;
            slit = getProjectedLightDiffuseColor( l_dist, stc.xy ); // NOTE: Using diffuse due to texture2DLodSpecular() has extra: d *= min(1, d * (proj_lod - lod));
        }
    }
    return slit; // specular light
}

vec3 getProjectedLightSpecularColor(float light_distance, vec2 projected_uv)
{
    float diff = clamp((light_distance - proj_focus)/proj_range, 0.0, 1.0);
    float lod = diff * proj_lod;
    vec4 plcol = getTexture2DLodDiffuse(projected_uv.xy, lod); // NOTE: Using diffuse due to texture2DLodSpecular() has extra: d *= min(1, d * (proj_lod - lod));

    return color.rgb * plcol.rgb * plcol.a;
}

vec4 getPosition(vec2 pos_screen)
{
    float depth = getDepth(pos_screen);
    vec2 sc = getScreenCoordinate(pos_screen);
    vec4 ndc = vec4(sc.x, sc.y, 2.0*depth-1.0, 1.0);
    vec4 pos = inv_proj * ndc;
    pos /= pos.w;
    pos.w = 1.0;
    return pos;
}

// get position given a normalized device coordinate
vec3 getPositionWithNDC(vec3 ndc)
{
    vec4 pos = inv_proj * vec4(ndc, 1.0);
    return pos.xyz / pos.w;
}

vec4 getPositionWithDepth(vec2 pos_screen, float depth)
{
    vec2 sc = getScreenCoordinate(pos_screen);
    vec3 ndc = vec3(sc.x, sc.y, 2.0*depth-1.0);
    return vec4(getPositionWithNDC(ndc), 1.0);
}

vec2 getScreenCoord(vec4 clip)
{
    vec4 ndc = clip;
         ndc.xyz /= clip.w;
    vec2 screen = vec2( ndc.xy * 0.5 );
         screen += 0.5;
    return screen;
}

vec2 getScreenXY(vec4 clip)
{
    vec4 ndc = clip;
         ndc.xyz /= clip.w;
    vec2 screen = vec2( ndc.xy * 0.5 );
         screen += 0.5;
         screen *= screen_res;
    return screen;
}

// Color utils

vec3 colorize_dot(float x)
{
    if (x > 0.0) return vec3( 0, x, 0 );
    if (x < 0.0) return vec3(-x, 0, 0 );
                 return vec3( 0, 0, 1 );
}

vec3 hue_to_rgb(float hue)
{
    if (hue > 1.0) return vec3(0.5);
    vec3 rgb = abs(hue * 6. - vec3(3, 2, 4)) * vec3(1, -1, -1) + vec3(-1, 2, 2);
    return clamp(rgb, 0.0, 1.0);
}

// PBR Utils

vec2 BRDF(float NoV, float roughness)
{
    // <FS> keep both coordinates off the very edge of the lookup texture: a surface facing the camera head-on (a mirror: NoV exactly 1.0) read (0, 0) there and its reflection came out black
    return texture(brdfLut, clamp(vec2(NoV, roughness), vec2(0.004), vec2(0.996))).rg;
}

// set colorDiffuse and colorSpec to the results of GLTF PBR style IBL
void pbrIbl(vec3 diffuseColor,
            vec3 specularColor,
            vec3 radiance, // radiance map sample
            vec3 irradiance, // irradiance map sample
            float ao,       // ambient occlusion factor
            float nv,       // normal dot view vector
            float perceptualRough,
            out vec3 diffuseOut,
            out vec3 specularOut)
{
    // retrieve a scale and bias to F0. See [1], Figure 3
    vec2 brdf = BRDF(clamp(nv, 0, 1), 1.0-perceptualRough);
    vec3 diffuseLight = irradiance;
    vec3 specularLight = radiance;

    vec3 diffuse = diffuseLight * diffuseColor;
    vec3 specular = specularLight * (specularColor * brdf.x + brdf.y);

    diffuseOut = diffuse * ao;
    specularOut = specular * ao;
}


// Encapsulate the various inputs used by the various functions in the shading equation
// We store values in this struct to simplify the integration of alternative implementations
// of the shading terms, outlined in the Readme.MD Appendix.
struct PBRInfo
{
    float NdotL;                  // cos angle between normal and light direction
    float NdotV;                  // cos angle between normal and view direction
    float NdotH;                  // cos angle between normal and half vector
    float LdotH;                  // cos angle between light direction and half vector
    float VdotH;                  // cos angle between view direction and half vector
    float perceptualRoughness;    // roughness value, as authored by the model creator (input to shader)
    float metalness;              // metallic value at the surface
    vec3 reflectance0;            // full reflectance color (normal incidence angle)
    vec3 reflectance90;           // reflectance color at grazing angle
    float alphaRoughness;         // roughness mapped to a more linear change in the roughness (proposed by [2])
    vec3 diffuseColor;            // color contribution from diffuse lighting
    vec3 specularColor;           // color contribution from specular lighting
};

// Basic Lambertian diffuse
// Implementation from Lambert's Photometria https://archive.org/details/lambertsphotome00lambgoog
// See also [1], Equation 1
vec3 diffuse(PBRInfo pbrInputs)
{
    return pbrInputs.diffuseColor / M_PI;
}

// The following equation models the Fresnel reflectance term of the spec equation (aka F())
// Implementation of fresnel from [4], Equation 15
vec3 specularReflection(PBRInfo pbrInputs)
{
    return pbrInputs.reflectance0 + (pbrInputs.reflectance90 - pbrInputs.reflectance0) * pow(clamp(1.0 - pbrInputs.VdotH, 0.0, 1.0), 5.0);
}

// This calculates the specular geometric attenuation (aka G()),
// where rougher material will reflect less light back to the viewer.
// This implementation is based on [1] Equation 4, and we adopt their modifications to
// alphaRoughness as input as originally proposed in [2].
float geometricOcclusion(PBRInfo pbrInputs)
{
    float NdotL = pbrInputs.NdotL;
    float NdotV = pbrInputs.NdotV;
    float r = pbrInputs.alphaRoughness;

    float attenuationL = 2.0 * NdotL / (NdotL + sqrt(r * r + (1.0 - r * r) * (NdotL * NdotL)));
    float attenuationV = 2.0 * NdotV / (NdotV + sqrt(r * r + (1.0 - r * r) * (NdotV * NdotV)));
    return attenuationL * attenuationV;
}

// The following equation(s) model the distribution of microfacet normals across the area being drawn (aka D())
// Implementation from "Average Irregularity Representation of a Roughened Surface for Ray Reflection" by T. S. Trowbridge, and K. P. Reitz
// Follows the distribution function recommended in the SIGGRAPH 2013 course notes from EPIC Games [1], Equation 3.
float microfacetDistribution(PBRInfo pbrInputs)
{
    float roughnessSq = pbrInputs.alphaRoughness * pbrInputs.alphaRoughness;
    float f = (pbrInputs.NdotH * roughnessSq - pbrInputs.NdotH) * pbrInputs.NdotH + 1.0;
    return roughnessSq / (M_PI * f * f);
}

void pbrPunctual(vec3 diffuseColor, vec3 specularColor,
                    float perceptualRoughness,
                    float metallic,
                    vec3 n, // normal
                    vec3 v, // surface point to camera
                    vec3 l,
                    out float nl,
                    out vec3 diff,
                    out vec3 spec) //surface point to light
{
    // make sure specular highlights from punctual lights don't fall off of polished surfaces
    perceptualRoughness = max(perceptualRoughness, 8.0/255.0);

    float alphaRoughness = perceptualRoughness * perceptualRoughness;

    // Compute reflectance.
    float reflectance = max(max(specularColor.r, specularColor.g), specularColor.b);

    // For typical incident reflectance range (between 4% to 100%) set the grazing reflectance to 100% for typical fresnel effect.
    // For very low reflectance range on highly diffuse objects (below 4%), incrementally reduce grazing reflecance to 0%.
    float reflectance90 = clamp(reflectance * 25.0, 0.0, 1.0);
    vec3 specularEnvironmentR0 = specularColor.rgb;
    vec3 specularEnvironmentR90 = vec3(1.0, 1.0, 1.0) * reflectance90;

    vec3 h = normalize(l+v);                        // Half vector between both l and v
    vec3 reflection = -normalize(reflect(v, n));
    reflection.y *= -1.0f;

    float NdotL = clamp(dot(n, l), 0.001, 1.0);
    float NdotV = clamp(abs(dot(n, v)), 0.001, 1.0);
    float NdotH = clamp(dot(n, h), 0.0, 1.0);
    float LdotH = clamp(dot(l, h), 0.0, 1.0);
    float VdotH = clamp(dot(v, h), 0.0, 1.0);

    PBRInfo pbrInputs = PBRInfo(
        NdotL,
        NdotV,
        NdotH,
        LdotH,
        VdotH,
        perceptualRoughness,
        metallic,
        specularEnvironmentR0,
        specularEnvironmentR90,
        alphaRoughness,
        diffuseColor,
        specularColor
    );

    // Calculate the shading terms for the microfacet specular shading model
    vec3 F = specularReflection(pbrInputs);
    float G = geometricOcclusion(pbrInputs);
    float D = microfacetDistribution(pbrInputs);

    // Calculation of analytical lighting contribution
    vec3 diffuseContrib = (1.0 - F) * diffuse(pbrInputs);
    vec3 specContrib = F * G * D / (4.0 * NdotL * NdotV);

    nl = NdotL;

    diff = diffuseContrib;
    spec = specContrib;
}

vec3 pbrCalcPointLightOrSpotLight(vec3 diffuseColor, vec3 specularColor,
                    float perceptualRoughness,
                    float metallic,
                    vec3 n, // normal
                    vec3 p, // pixel position
                    vec3 v, // view vector (negative normalized pixel position)
                    vec3 lp, // light position
                    vec3 ld, // light direction (for spotlights)
                    vec3 lightColor,
                    float lightSize, float falloff, float is_pointlight, float ambiance)
{
    vec3 color = vec3(0,0,0);

    vec3 lv = lp.xyz - p;

    float lightDist = length(lv);

    float dist = lightDist / lightSize;
    if (dist <= 1.0)
    {
        lv /= lightDist;

        float dist_atten = calcLegacyDistanceAttenuation(dist, falloff);

        // spotlight coefficient.
        float spot = max(dot(-ld, lv), is_pointlight);
        // spot*spot => GL_SPOT_EXPONENT=2
        float spot_atten = spot*spot;

        vec3 intensity = spot_atten * dist_atten * lightColor * 3.0; //magic number to balance with legacy materials

        float nl = 0;
        vec3 diffPunc = vec3(0);
        vec3 specPunc = vec3(0);

        pbrPunctual(diffuseColor, specularColor, perceptualRoughness, metallic, n.xyz, v, lv, nl, diffPunc, specPunc);
        color = intensity * clamp(nl * (diffPunc + specPunc), vec3(0), vec3(10));
    }
    float final_scale = 1.0;
    if (classic_mode > 0)
        final_scale = 0.9;
    return color * final_scale;
}

void calcDiffuseSpecular(vec3 baseColor, float metallic, inout vec3 diffuseColor, inout vec3 specularColor)
{
    vec3 f0 = vec3(0.04);
    diffuseColor = baseColor*(vec3(1.0)-f0);
    diffuseColor *= 1.0 - metallic;
    specularColor = mix(f0, baseColor, metallic);
}

vec3 pbrBaseLight(vec3 diffuseColor, vec3 specularColor, float metallic, vec3 v, vec3 norm, float perceptualRoughness, vec3 light_dir, vec3 sunlit, float scol, vec3 radiance, vec3 irradiance, vec3 colorEmissive, float ao, vec3 additive, vec3 atten)
{
    vec3 color = vec3(0);

    float NdotV = clamp(abs(dot(norm, v)), 0.001, 1.0);
    vec3 iblDiff = vec3(0);
    vec3 iblSpec = vec3(0);
    pbrIbl(diffuseColor, specularColor, radiance, irradiance, ao, NdotV, perceptualRoughness, iblDiff, iblSpec);

    color += iblDiff;

    // For classic mode, we use a special version of pbrPunctual that basically gives us a deconstructed form of the lighting.
    float nl = 0;
    vec3 diffPunc = vec3(0);
    vec3 specPunc = vec3(0);
    pbrPunctual(diffuseColor, specularColor, perceptualRoughness, metallic, norm, v, normalize(light_dir), nl, diffPunc, specPunc);

    // Depending on the sky, we combine these differently.
    if (classic_mode > 0)
    {
        irradiance.rgb = srgb_to_linear(irradiance * 0.9); // BINGO

        // Reconstruct the diffuse lighting that we do for blinn-phong materials here.
        // A special note about why we do some really janky stuff for classic mode.
        // Since adding classic mode, we've moved the lambertian diffuse multiply out from pbrPunctual and instead handle it in the different light type calcs.
        // This will never be 100% correct, but at the very least we can make it look mostly correct with legacy skies and classic mode.

        float da = pow(nl, 1.2);

        vec3 sun_contrib = vec3(min(da, scol));

        // Multiply by PI to account for lambertian diffuse colors.  Otherwise things will be too dark when lit by the sun on legacy skies.
        sun_contrib = srgb_to_linear(linear_to_srgb(sun_contrib) * sunlit * 0.7) * M_PI;

        // Manually recombine everything here.  We have to separate the shading to ensure that lighting is able to more closely match blinn-phong.
        vec3 finalAmbient = irradiance.rgb * diffuseColor.rgb; // BINGO
        vec3 finalSun = clamp(sun_contrib * ((diffPunc.rgb + specPunc.rgb) * scol), vec3(0), vec3(10)); // QUESTIONABLE BINGO?
        color.rgb = srgb_to_linear(linear_to_srgb(finalAmbient) + (linear_to_srgb(finalSun) * 1.1));
        //color.rgb = sun_contrib * diffuseColor.rgb;
    }
    else
    {
        color += clamp(nl * (diffPunc + specPunc), vec3(0), vec3(10)) * sunlit * 3.0 * scol;
    }

    color.rgb += iblSpec.rgb;

    color += colorEmissive;

    return color;
}


// discard if given position in eye space is on the wrong side of the waterPlane according to waterSign
void waterClip(vec3 pos)
{
    // TODO: make this less branchy
    if (waterSign > 0)
    {
        if ((dot(pos.xyz, waterPlane.xyz) + waterPlane.w) < 0.0)
        {
            discard;
        }
    }
    else
    {
        if ((dot(pos.xyz, waterPlane.xyz) + waterPlane.w) > 0.0)
        {
            discard;
        }
    }

}



// ======== class1/deferred/gbufferUtil.glsl ========

#if defined(HAS_EMISSIVE)
#endif

vec4 getNormRaw(vec2 screenpos);
vec4 decodeNormal(vec4 norm);

GBufferInfo getGBuffer(vec2 screenpos)
{
    GBufferInfo ret;
    vec4 diffInfo = vec4(0);
    vec4 specInfo = vec4(0);
    vec4 emissInfo = vec4(0);

    diffInfo = texture(diffuseRect, screenpos.xy);
    specInfo = texture(specularRect, screenpos.xy);
    vec4 normInfo = getNormRaw(screenpos);

#if defined(HAS_EMISSIVE)
    emissInfo = texture(emissiveRect, screenpos.xy);
#endif

    ret.albedo = diffInfo;
    ret.normal = decodeNormal(normInfo).xyz;
    ret.specular = specInfo;
    ret.envIntensity = normInfo.b;
    ret.gbufferFlag = normInfo.w;
    ret.emissive = emissInfo;

    return ret;
}


// ======== class3/deferred/screenSpaceReflUtil.glsl ========

//uniform float zNear;
//uniform float zFar;

vec4 getPositionWithDepth(vec2 pos_screen, float depth);

float random (vec2 uv)
{
    return fract(sin(dot(uv, vec2(12.9898, 78.233))) * 43758.5453123); //simple random function
}

// Based off of https://github.com/RoundedGlint585/ScreenSpaceReflection/
// A few tweaks here and there to suit our needs.

vec2 generateProjectedPosition(vec3 pos)
{
    vec4 samplePosition = projection_matrix * vec4(pos, 1.f);
    samplePosition.xy = (samplePosition.xy / samplePosition.w) * 0.5 + 0.5;
    return samplePosition.xy;
}

bool isBinarySearchEnabled = true;
bool isAdaptiveStepEnabled = true;
bool isExponentialStepEnabled = true;
bool debugDraw = false;


float epsilon = 0.1;

float getLinearDepth(vec2 tc)
{
    float depth = texture(sceneDepth, tc).r;

    vec4 pos = getPositionWithDepth(tc, depth);

    return -pos.z;
}

// <FS> rejectUp: the view space up vector when the surface is water (zero otherwise). A hit on a pixel at or below the water plane is the water itself (the scene map
// is the previous frame, which contains the water): with the ripples those self hits come and go from pixel to pixel and from frame to frame, which showed as bands
// and flicker. Only things standing above the water count as reflections.
bool traceScreenRay(vec3 position, vec3 reflection, out vec4 hitColor, out float hitDepth, float depth, sampler2D textureFrame, vec3 rejectUp, vec2 uvOffset)
{
    bool rejectLow = dot(rejectUp, rejectUp) > 0.5;
    vec3 upPrev = mat3(inv_modelview_delta) * rejectUp;
    // transform position and reflection into same coordinate frame as the sceneMap and sceneDepth
    reflection += position;
    position = (inv_modelview_delta * vec4(position, 1)).xyz;
    reflection = (inv_modelview_delta * vec4(reflection, 1)).xyz;
    reflection -= position;

    depth = -position.z;
    vec3 originPrev = position;
    bool rejected = false;

    vec3 step = rayStep * reflection;
    vec3 marchingPosition = position + step;
    float delta;
    float depthFromScreen;
    vec2 screenPosition;
    bool hit = false;
    hitColor = vec4(0);

    int i = 0;
    if (depth > depthRejectBias)
    {
        for (; i < iterationCount && !hit; i++)
        {
            screenPosition = generateProjectedPosition(marchingPosition);
            if (screenPosition.x > 1 || screenPosition.x < 0 ||
                screenPosition.y > 1 || screenPosition.y < 0)
            {
                hit = false;
                break;
            }
            depthFromScreen = getLinearDepth(screenPosition);
            delta = abs(marchingPosition.z) - depthFromScreen;

            if (depth < depthFromScreen + epsilon && depth > depthFromScreen - epsilon)
            {
                break;
            }

            if (abs(delta) < distanceBias)
            {
                if (rejectLow && dot(getPositionWithDepth(screenPosition, texture(sceneDepth, screenPosition).r).xyz - originPrev, upPrev) < 0.5)
                {
                    rejected = true;
                    break;
                }
                vec4 color = vec4(1);
                if(debugDraw)
                    color = vec4( 0.5+ sign(delta)/2,0.3,0.5- sign(delta)/2, 0);
                hitColor = texture(sceneMap, screenPosition + uvOffset) * color;
                hitDepth = depthFromScreen;
                hit = true;
                break;
            }
            if (isBinarySearchEnabled && delta > 0)
            {
                break;
            }
            if (isAdaptiveStepEnabled)
            {
                float directionSign = sign(abs(marchingPosition.z) - depthFromScreen);
                //this is sort of adapting step, should prevent lining reflection by doing sort of iterative converging
                //some implementation doing it by binary search, but I found this idea more cheaty and way easier to implement
                step = step * (1.0 - rayStep * max(directionSign, 0.0));
                marchingPosition += step * (-directionSign);
            }
            else
            {
                marchingPosition += step;
            }

            if (isExponentialStepEnabled)
            {
                step *= adaptiveStepMultiplier;
            }
        }
        if(isBinarySearchEnabled && !rejected)
        {
            for(; i < iterationCount && !hit; i++)
            {
                step *= 0.5;
                marchingPosition = marchingPosition - step * sign(delta);

                screenPosition = generateProjectedPosition(marchingPosition);
                if (screenPosition.x > 1 || screenPosition.x < 0 ||
                    screenPosition.y > 1 || screenPosition.y < 0)
                {
                    hit = false;
                    break;
                }
                depthFromScreen = getLinearDepth(screenPosition);
                delta = abs(marchingPosition.z) - depthFromScreen;

                if (depth < depthFromScreen + epsilon && depth > depthFromScreen - epsilon)
                {
                    break;
                }

                if (abs(delta) < distanceBias && depthFromScreen != (depth - distanceBias))
                {
                    if (rejectLow && dot(getPositionWithDepth(screenPosition, texture(sceneDepth, screenPosition).r).xyz - originPrev, upPrev) < 0.5)
                    {
                        break;
                    }
                    vec4 color = vec4(1);
                    if(debugDraw)
                        color = vec4( 0.5+ sign(delta)/2,0.3,0.5- sign(delta)/2, 0);
                    hitColor = texture(sceneMap, screenPosition + uvOffset) * color;
                    hitDepth = depthFromScreen;
                    hit = true;
                    break;
                }
            }
        }
    }

    return hit;
}


vec3 getPoissonSample(int i) {
    return POISSON3D_SAMPLES[i] * 2 - 1;
}

// <FS> Fast reflection trace (water, and other glossy surfaces when FSSSRSurfaceTrace is on). The general marcher below takes exponentially growing steps and only counts a hit that lands within a fixed 1.5 cm of a surface, so
// which step the ray happens to land on decides if it hits: one flat band of reflection per step length, and grain where neighbouring ripples disagree. This one
// walks a fixed number of steps in screen space (perspective correct), accepts a crossing within a thickness that grows with the step, and finds the exact crossing
// with a binary search. The screen position of the hit is returned (alpha is the confidence).
vec4 traceScreenRayFast(vec3 viewPos, vec3 dir, vec3 rejectUp, vec2 uvOffset)
{
    bool water = dot(rejectUp, rejectUp) > 0.5;
    vec3 P0 = (inv_modelview_delta * vec4(viewPos, 1.0)).xyz;       // the scene map is the previous frame: work in its camera space
    vec3 R = normalize(mat3(inv_modelview_delta) * dir);
    vec3 upPrev = mat3(inv_modelview_delta) * rejectUp;

    const float nearZ = 0.1;
    vec3 P1 = P0 + R * 400.0;
    if (P1.z > -nearZ)
    {
        P1 = P0 + (P1 - P0) * ((-nearZ - P0.z) / (P1.z - P0.z));
    }

    vec4 c0 = projection_matrix * vec4(P0, 1.0);
    vec4 c1 = projection_matrix * vec4(P1, 1.0);
    float k0 = 1.0 / c0.w;
    float k1 = 1.0 / c1.w;
    vec2 s0 = c0.xy * k0 * 0.5 + 0.5;
    vec2 s1 = c1.xy * k1 * 0.5 + 0.5;
    vec2 d = s1 - s0;

    // stop where the ray leaves the screen
    float tmax = 1.0;
    if (d.x > 0.0) tmax = min(tmax, (1.0 - s0.x) / d.x); else if (d.x < 0.0) tmax = min(tmax, -s0.x / d.x);
    if (d.y > 0.0) tmax = min(tmax, (1.0 - s0.y) / d.y); else if (d.y < 0.0) tmax = min(tmax, -s0.y / d.y);
    tmax = max(tmax, 0.0);

    vec2 res = vec2(textureSize(sceneDepth, 0));
    float lenPx = length(d * res) * tmax;
    if (lenPx < 2.0)
    {
        return vec4(0);
    }

    // <FS> bit 2 of ssrTraceMode (FSWaterTraceAdaptive): water rays take fewer steps, spaced unevenly: fine next to the surface where nearby things reflect, coarse far away
    // where the reflected picture is small and soft anyway. The thickness test below already scales with the step.
    bool adaptive = water && ((int(ssrTraceMode + 0.5) & 4) != 0);
    const int maxSteps = 48;
    int steps = adaptive ? int(clamp(ceil(lenPx / 8.0), 2.0, 30.0)) : int(clamp(ceil(lenPx / 5.0), 2.0, float(maxSteps)));
    float dt = tmax / float(steps);
    float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));     // fixed per pixel: no shimmer over time

    float prevT = 0.0;
    float prevZ = 1.0 / k0;
    for (int i = 1; i <= maxSteps; i++)
    {
        if (i > steps)
        {
            break;
        }
        float t = min(dt * (float(i) + jitter), tmax);
        if (adaptive)
        {
            float u = min((float(i) + jitter) / float(steps), 1.0);
            t = tmax * u * sqrt(u);       // u^1.5
        }
        vec2 uv = s0 + d * t;
        float z = 1.0 / (k0 + (k1 - k0) * t);
        float sceneZ = getLinearDepth(uv);
        float diff = z - sceneZ;                    // positive: the ray is behind the surface
        float thick = max(0.3, abs(z - prevZ) * 1.5);
        if (diff > 0.0 && diff < thick)
        {
            // the crossing is between prevT and t: narrow it down
            float lo = prevT;
            float hi = t;
            for (int b = 0; b < 6; b++)
            {
                float mid = 0.5 * (lo + hi);
                vec2 muv = s0 + d * mid;
                float mz = 1.0 / (k0 + (k1 - k0) * mid);
                if (mz - getLinearDepth(muv) > 0.0)
                {
                    hi = mid;
                }
                else
                {
                    lo = mid;
                }
            }
            vec2 huv = s0 + d * hi;
            vec3 hp = getPositionWithDepth(huv, texture(sceneDepth, huv).r).xyz;
            if (water ? (dot(hp - P0, upPrev) < 0.5) : (length(hp - P0) < 0.15))
            {
                return vec4(0);                     // water: the water itself (the scene map contains it), not something standing on it; other surfaces: the surface itself
            }
            vec2 e2 = min(huv, 1.0 - huv);
            float edge = clamp(min(e2.x, e2.y) * 10.0, 0.0, 1.0);
            return vec4(texture(sceneMap, huv + uvOffset).rgb, edge);
        }
        prevT = t;
        prevZ = z;
    }
    return vec4(0);
}

float tapScreenSpaceReflection(int totalSamples, vec2 tc, vec3 viewPos, vec3 n, inout vec4 collectedColor, sampler2D source, float glossiness, vec3 rejectUp)
{
#ifdef TRANSPARENT_SURFACE
collectedColor = vec4(1, 0, 1, 1);
    return 0;
#endif
    collectedColor = vec4(0);
    int hits = 0;

    // <FS> which trace: the improved one for water (rejectUp is set) and/or for other surfaces, or the original marcher
    bool isWater = dot(rejectUp, rejectUp) > 0.5;
    int traceMode = int(ssrTraceMode + 0.5);
    bool useFastTrace = isWater ? ((traceMode & 1) != 0) : ((traceMode & 2) != 0);
    if (!useFastTrace)
    {
        rejectUp = vec3(0);
    }

    // water with the improved trace: the ray is traced against a mostly smoothed normal so that neighbouring pixels reflect a coherent picture instead of
    // each ripple sending its ray somewhere else, and the ripples only shift where the colour is read (so the reflection wobbles instead of flickering)
    vec3 nTrace = normalize(n);
    vec2 uvOffset = vec2(0);
    if (isWater && useFastTrace)
    {
        nTrace = normalize(mix(rejectUp, nTrace, 0.12));
        uvOffset = (normalize(n).xy - nTrace.xy) * 0.1;
    }

    float depth = -viewPos.z;

    vec3 rayDirection = normalize(reflect(viewPos, nTrace));

    if (isWater && useFastTrace)
    {
        // water: one ray (the normal is smoothed, extra random directions would only add grain)
        collectedColor = traceScreenRayFast(viewPos, rayDirection, rejectUp, uvOffset);
        return collectedColor.a;
    }

    vec2 uv2 = tc * screen_res;
    float c = (uv2.x + uv2.y) * 0.125;
    float jitter = mod( c, 1.0);

    vec2 screenpos = 1 - abs(tc * 2 - 1);
    float vignette = clamp((abs(screenpos.x) * abs(screenpos.y)) * 16,0, 1);
    vignette *= clamp((dot(normalize(viewPos), nTrace) * 0.5 + 0.5) * 5.5 - 0.8, 0, 1);

    float zFar = 128.0;
    vignette *= clamp(1.0+(viewPos.z/zFar), 0.0, 1.0);

    vignette *= clamp(glossiness * 3 - 1.7, 0, 1);

    vec4 hitpoint;

    glossiness = 1 - glossiness;

    totalSamples = int(max(glossySampleCount, glossySampleCount * glossiness * vignette));

    totalSamples = max(totalSamples, 1);
    if (glossiness <= 0.0)
    {
        // <FS> glossiness 1 (what a transparent surface asks for): the randomized directions below are the reflection ray plus a scatter that is multiplied by this zero, so every sample is the SAME ray. Four
        // identical traces averaged are the one trace: same colour, same alpha (hits / samples is 1 either way), a quarter of the work.
        totalSamples = 1;
    }
    if (glossiness < 0.35)
    {
        if (vignette > 0)
        {
            for (int i = 0; i < totalSamples; i++)
            {
                vec3 firstBasis = normalize(cross(getPoissonSample(i), rayDirection));
                vec3 secondBasis = normalize(cross(rayDirection, firstBasis));
                vec2 coeffs = vec2(random(tc + vec2(0, i)) + random(tc + vec2(i, 0)));
                vec3 reflectionDirectionRandomized = rayDirection + ((firstBasis * coeffs.x + secondBasis * coeffs.y) * glossiness);

                //float hitDepth;

                bool hit;
                if (useFastTrace)
                {
                    vec4 fast = traceScreenRayFast(viewPos, normalize(reflectionDirectionRandomized), vec3(0), vec2(0));
                    hit = fast.a > 0.0;
                    hitpoint = vec4(fast.rgb, 0.0);
                }
                else
                {
                    hit = traceScreenRay(viewPos, normalize(reflectionDirectionRandomized), hitpoint, depth, depth, source, rejectUp, uvOffset);
                }

                hitpoint.a = 0;

                if (hit)
                {
                    ++hits;
                    collectedColor += hitpoint;
                    collectedColor.a += 1;
                }
            }

            if (hits > 0)
            {
                collectedColor /= hits;
            }
            else
            {
                collectedColor = vec4(0);
            }
        }
    }
    float hitAlpha = hits;
    hitAlpha /= totalSamples;
    collectedColor.a = hitAlpha * vignette;
    return hits;
}


// ======== class1/deferred/shadowUtil.glsl ========

#if defined(SUN_SHADOW)
#endif

#if defined(SPOT_SHADOW)
#endif


float pcfShadow(sampler2DShadow shadowMap, vec3 norm, vec4 stc, float bias_mul, vec2 pos_screen, vec3 light_dir)
{
#if defined(SUN_SHADOW)
    float offset = shadow_bias * bias_mul;
    stc.xyz /= stc.w;
    stc.z += offset * 2.0;
    stc.x = floor(stc.x*shadow_res.x + fract(pos_screen.y*shadow_res.y))/shadow_res.x; // add some chaotic jitter to X sample pos according to Y to disguise the snapping going on here
    float cs = texture(shadowMap, stc.xyz);
    float shadow = cs * 4.0;
    shadow += texture(shadowMap, stc.xyz+vec3( 1.5/shadow_res.x,  0.5/shadow_res.y, 0.0));
    shadow += texture(shadowMap, stc.xyz+vec3( 0.5/shadow_res.x, -1.5/shadow_res.y, 0.0));
    shadow += texture(shadowMap, stc.xyz+vec3(-1.5/shadow_res.x, -0.5/shadow_res.y, 0.0));
    shadow += texture(shadowMap, stc.xyz+vec3(-0.5/shadow_res.x,  1.5/shadow_res.y, 0.0));
    return clamp(shadow * 0.125, 0.0, 1.0);
#else
    return 1.0;
#endif
}

float pcfSpotShadow(sampler2DShadow shadowMap, vec4 stc, float bias_scale, vec2 pos_screen)
{
#if defined(SPOT_SHADOW)
    stc.xyz /= stc.w;
    stc.z += spot_shadow_bias * bias_scale;
    stc.x = floor(proj_shadow_res.x * stc.x + fract(pos_screen.y*0.666666666)) / proj_shadow_res.x; // snap

    float cs = texture(shadowMap, stc.xyz);
    float shadow = cs;

    vec2 off = 1.0/proj_shadow_res;
    off.y *= 1.5;

    shadow += texture(shadowMap, stc.xyz+vec3(off.x*2.0, off.y, 0.0));
    shadow += texture(shadowMap, stc.xyz+vec3(off.x, -off.y, 0.0));
    shadow += texture(shadowMap, stc.xyz+vec3(-off.x, off.y, 0.0));
    shadow += texture(shadowMap, stc.xyz+vec3(-off.x*2.0, -off.y, 0.0));
    return shadow*0.2;
#else
    return 1.0;
#endif
}

#if defined(SUN_SHADOW) && defined(LL_RT_SHADOWS)
// <FS> PLAN-rt-shadow-penumbra.md step 1: the distance along a sun ray to the nearest thing that blocks it (an opaque hit, or a card that covers at least half), 0 = nothing. rtShadowRay and
// rtShadowRaySoftS set rt_blocker_t for the ray they trace; rtSunShadowCore keeps the CENTRE ray's value in rt_sun_blocker_t, which the sun lightmap pass writes to its second attachment. The
// penumbra at the receiver is this distance times twice the tangent of the sun's angular radius; a filter sized by it is the plan's next step (the noise of the random disc rays goes with it).
float rt_blocker_t = 0.0;
float rt_sun_blocker_t = 0.0;
float rtSunBlockerT()
{
    return rt_sun_blocker_t;
}
// <FS> Ray query sun shadow (LL_VK_RT_SHADOWS): one ray from the surface towards the sun against the frame's scene (llvkrt.cpp), in eye space like everything here.
// Opaque casters commit their hit at once; alpha masked ones (instance mask 2) hand each candidate triangle to rtAlphaHit, which repeats the test of shadowAlphaPullF.glsl
// with the texture of the geometry record (set 3 binding 1) read through the frame's bindless table. Rigged avatars are not in the scene yet (PLAN-rt-shadows.md stage C).
// <FS> a translucent glTF mesh (an avatar's shell: base colour alpha blended) is a layer for the traced reflection only (rtTraceHitEx); a sun ray goes through it, as it did before such draws were named to the ray scene
bool rtRecordGltfBlend(LLRtGeom g)
{
    uint mat_id = g.material;
    if (mat_id == 0u || mat_id >= 16384u)
    {
        return false;
    }
    uint mf = floatBitsToUint(LLRtMaterials(ll_rt_geoms.g[0].normals).m[mat_id].params.z);
    return (mf & 10u) == 10u;
}

bool rtAlphaHit(uint record, uint prim, vec2 bary, vec2 pos_screen, float lod)
{
    // record 0 (the header) carries the number of records this frame's table holds: a geometry that points past them (a structure that does not match its records) would make this
    // function follow addresses from memory that is not a record, and a GPU fault loses the device for good. Such a hit is not a hit.
    if (record == 0u || record >= ll_rt_geoms.g[0].first)
    {
        return false;
    }
    LLRtGeom g = ll_rt_geoms.g[record];
    if ((g.flags & 256u) != 0u)
    {
        return false;                       // a record the scene could not vouch for this frame: nothing is read
    }
    if (rtRecordGltfBlend(g))
    {
        return false;
    }
    vec3 w = vec3(1.0 - bary.x - bary.y, bary.x, bary.y);
    LLRtIndices ib = LLRtIndices(g.indices);
    uint ix[3];
    for (uint k = 0u; k < 3u; ++k)
    {
        uint i = g.first + prim * 3u + k;
        uint word = ib.w[i >> 1];
        ix[k] = rtClampVertex(g, ((i & 1u) != 0u) ? (word >> 16) : (word & 0xFFFFu));
    }
    vec2 uv = vec2(0.0);
    if ((g.flags & 8u) != 0u)
    {
        LLRtTexcoords tc = LLRtTexcoords(g.texcoords);
        uv = tc.t[ix[0]] * w.x + tc.t[ix[1]] * w.y + tc.t[ix[2]] * w.z;
        uv = g.uv0 * uv.x + g.uv1 * uv.y + g.uv2;          // the texture animation / glTF transform of the draw
    }
    uint tex_index = ((g.flags & 4u) != 0u) ? 0u : (floatBitsToUint(LLRtPositions(g.positions).v[ix[0]].w) & 3u);
    uint pair = (tex_index < 2u) ? g.slots.x : g.slots.y;
    uint slot = (pair >> ((tex_index & 1u) * 16u)) & 0xFFFFu;
    const bool slot_assigned = slot != 0u;      // <FS> 0: the record has no texture (plain white, by design: the CPU side names such a draw); a pin index or handle that resolves to 0 below is a texture without a descriptor
    if ((g.flags & 4096u) != 0u)
    {
        slot = ll_rt_slot(slot);            // the record holds a pin index (the persistent bindless table)
    }
    // <FS> (2026-11-14, the user: a head or hair shaped dark patch of the sun shadow for one frame) the same rule as the CPU side (pipeline.cpp rt_texture_ready: a masked caster whose texture has no data is not
    // named): a handle whose entry in this frame slot's handle map is 0 draws the opaque placeholder image, so a card that reads it would block the sun for a frame. Such a record casts nothing.
    if (slot_assigned && LL_HM(slot) == 0u) { return false; }
    float alpha = textureLod(ll_rt_tex[nonuniformEXT(LL_HM(slot))], uv, lod).a;
    if ((g.flags & 64u) != 0u)
    {
        // glTF alpha mask: texture alpha times vertex alpha against the material's cutoff, no dithering (pbrShadowAlphaMaskF.glsl)
        if ((g.flags & 16u) != 0u)
        {
            LLRtColors col = LLRtColors(g.colors);
            alpha *= (float(col.c[ix[0]] >> 24) * w.x + float(col.c[ix[1]] >> 24) * w.y + float(col.c[ix[2]] >> 24) * w.z) * (1.0 / 255.0);
        }
        return alpha >= g.cutoff;
    }
    if (alpha < g.cutoff)
    {
        return false;
    }
    if ((g.flags & 2u) == 0u)
    {
        if ((g.flags & 1u) == 0u && (g.flags & 16u) != 0u)
        {
            LLRtColors col = LLRtColors(g.colors);
            alpha *= (float(col.c[ix[0]] >> 24) * w.x + float(col.c[ix[1]] >> 24) * w.y + float(col.c[ix[2]] >> 24) * w.z) * (1.0 / 255.0);
        }
        if (alpha < 0.05)
        {
            return false;                   // totally transparent
        }
        if (alpha < 0.88 && fract(0.5 * floor(pos_screen.x)) < 0.25)
        {
            return false;                   // semi-transparent: dithered like the shadow maps
        }
    }
    return true;
}

// Coverage (0..1) of a masked or blended candidate for the SOFT shadows of traced hits (rtShadowRaySoft): the texture alpha is read from a blurred mip (hair strands and foliage are cut-outs finer than a
// pixel: a binary ray gives hard stripes where the main view's filtered shadow maps give a soft shade), and a ramp around the cutoff turns it into a fraction.
// LL_VK_RT_SUN_BLENDCOVER (default 1): a BLENDED card (record flag 512, the alpha pool's draws: most hair) covers by its alpha itself, which is what blending draws (a texel at alpha 0.4 lets 60 % through).
// The ramp around the cutoff is for masked cards; applied to blended hair (cutoff 0.598, the shadow maps' cut) it dropped every wisp under 0.43 from the shadow and made everything over 0.76 a solid.
#ifndef LL_RT_SUN_BLEND_COVER
#define LL_RT_SUN_BLEND_COVER 1
#endif
float rtAlphaCover(uint record, uint prim, vec2 bary, float lod)
{
    if (record == 0u || record >= ll_rt_geoms.g[0].first)
    {
        return 0.0;
    }
    LLRtGeom g = ll_rt_geoms.g[record];
    if ((g.flags & 256u) != 0u)
    {
        return 0.0;
    }
    if (rtRecordGltfBlend(g))
    {
        return 0.0;
    }
    vec3 w = vec3(1.0 - bary.x - bary.y, bary.x, bary.y);
    LLRtIndices ib = LLRtIndices(g.indices);
    uint ix[3];
    for (uint k = 0u; k < 3u; ++k)
    {
        uint i = g.first + prim * 3u + k;
        uint word = ib.w[i >> 1];
        ix[k] = rtClampVertex(g, ((i & 1u) != 0u) ? (word >> 16) : (word & 0xFFFFu));
    }
    vec2 uv = vec2(0.0);
    if ((g.flags & 8u) != 0u)
    {
        LLRtTexcoords tc = LLRtTexcoords(g.texcoords);
        uv = tc.t[ix[0]] * w.x + tc.t[ix[1]] * w.y + tc.t[ix[2]] * w.z;
        uv = g.uv0 * uv.x + g.uv1 * uv.y + g.uv2;
    }
    uint tex_index = ((g.flags & 4u) != 0u) ? 0u : (floatBitsToUint(LLRtPositions(g.positions).v[ix[0]].w) & 3u);
    uint pair = (tex_index < 2u) ? g.slots.x : g.slots.y;
    uint slot = (pair >> ((tex_index & 1u) * 16u)) & 0xFFFFu;
    const bool slot_assigned = slot != 0u;      // <FS> 0: the record has no texture (plain white, by design: the CPU side names such a draw); a pin index or handle that resolves to 0 below is a texture without a descriptor
    if ((g.flags & 4096u) != 0u)
    {
        slot = ll_rt_slot(slot);
    }
    // <FS> (2026-11-14, the user: a head or hair shaped dark patch of the sun shadow for one frame) the same rule as the CPU side (pipeline.cpp rt_texture_ready: a masked caster whose texture has no data is not
    // named): a handle whose entry in this frame slot's handle map is 0 draws the opaque placeholder image, so a card that reads it would block the sun for a frame. Such a record casts nothing.
    if (slot_assigned && LL_HM(slot) == 0u) { return 0.0; }
    float alpha = textureLod(ll_rt_tex[nonuniformEXT(LL_HM(slot))], uv, lod + 2.0).a;
    if ((g.flags & 64u) != 0u || ((g.flags & 2u) == 0u && (g.flags & 1u) == 0u))
    {
        if ((g.flags & 16u) != 0u)
        {
            LLRtColors col = LLRtColors(g.colors);
            alpha *= (float(col.c[ix[0]] >> 24) * w.x + float(col.c[ix[1]] >> 24) * w.y + float(col.c[ix[2]] >> 24) * w.z) * (1.0 / 255.0);
        }
    }
#if LL_RT_SUN_BLEND_COVER
    if ((g.flags & 512u) != 0u)
    {
        return clamp(alpha, 0.0, 1.0);
    }
#endif
    return clamp((alpha - g.cutoff) * 3.0 + 0.5, 0.0, 1.0);
}

// The scene's instances are in world space relative to an origin the scene call chose, so they do not depend on the camera: record 0 of the geometry table holds how a ray
// moves from eye space into that space: a rotation (3 columns) and the camera's place in it (12 floats packed into the first 48 bytes of the record).
void rtSceneFrame(out mat3 to_scene, out vec3 camera)
{
    LLRtGeom h = ll_rt_geoms.g[0];
    to_scene = mat3(uintBitsToFloat(uvec3(h.positions.x, h.positions.y, h.indices.x)), uintBitsToFloat(uvec3(h.indices.y, h.texcoords.x, h.texcoords.y)),
                    uintBitsToFloat(uvec3(h.colors.x, h.colors.y, h.slots.x)));
    camera = uintBitsToFloat(uvec3(h.slots.y, h.flags, floatBitsToUint(h.cutoff)));
}

// Soft shadows: the sun is a disc, not a point. Each pixel traces rt_sun_samples rays towards random points of the sun's disc (tangent of its angular radius: rt_sun_radius), so a
// shadow edge is as soft as the distance between the caster and the receiver makes it (hard where they touch). One or two rays per pixel are noisy: the sun lightmap's bilateral blur
// (blurLightF.glsl) smooths them. Both uniforms are set by the sun lightmap pass only; every other shader that samples the sun shadow (alpha, water) leaves them 0 and gets one hard ray.
                                       // (the translucency floor of 2026-10-28; the cone of wide rays that replaced it on 11-01 let rays escape the hair shell sideways, which no filter on the result can do)
                                       //      2: on, but the soft walk through hair runs to the end instead of ending once the floor has the answer (sunself=2)

// A ray whose origin or direction is NaN or infinite (a pixel whose eye space position was rebuilt from a zero or far-plane depth, a scene frame record not written yet) is undefined for a
// ray query (GPU-assisted validation: "Ray Origin contains a NaN"): no ray is cast, the caller takes the no-hit answer.
bool rtRayOk(vec3 o, vec3 d)
{
    return !(any(isnan(o)) || any(isinf(o)) || any(isnan(d)) || any(isinf(d)));
}

// Test switch of the traced glossy reflection (rt-debug.txt glossyrt=20 / 21 / 22, set by the soften pass only; 0 everywhere else): how a hit is lit by the sun. 1: always sunlit (no shadow ray),
// 2: the shadow ray starts 0.15 m off the surface (instead of 0.02 m), 3: a reflection ray may look at 1024 masked candidates (instead of 64) before it gives up.
// <FS> Hair in the traced mirror (test switch, second number of D:/claude/rr-debug.txt): a skinned alpha MASKED draw (hair cards) is tested hit or miss against its cutoff at the pixel's sub-pixel position, so
// thin strands pop in and out while the view moves (nothing averages one ray per pixel then). 1: the cutoff becomes a smooth step and such a candidate is treated like a blended layer (the opacity
// it has, not all or nothing), 2: also the opacity of the layer is the combined opacity of every such card the ray crossed (order independent), 3: the same on a blurrier mip of the alpha.
bool rt_soft_masked = false;            // set by rtShadeSurfaceEx: the last surface looked at was a skinned masked draw taken softly

// <FS> the number of masked candidates the sun ray may test before it stops and calls the pixel lit (LL_VK_RT_CANDIDATES, default 96). Under a dense canopy a ray meets hundreds of leaf quads; at 96 whether it reaches
// an opaque hit or enough leaves depends on the ray's start and the traversal order, so the shadow flips with the view. LL_VK_RT_CAPDEBUG=1: a ray that stopped at the limit is drawn fully shadowed instead of lit.
#ifndef LL_RT_MAX_CANDIDATES
#define LL_RT_MAX_CANDIDATES 96u
#endif
#ifdef LL_RT_CAP_DEBUG
#define LL_RT_CAPPED_RESULT 0.0
#else
#define LL_RT_CAPPED_RESULT 1.0
#endif

// 1 = nothing in the way for tmax metres (the sun: 2000, a local light: its distance), 0 = blocked
float rtShadowRay(vec3 origin, vec3 dir, float tmax, vec2 pos_screen, float lod)
{
    mat3 to_scene;
    vec3 camera;
    rtSceneFrame(to_scene, camera);
    const vec3 ray_origin = to_scene * origin + camera;
    const vec3 ray_dir = to_scene * dir;
    if (!rtRayOk(ray_origin, ray_dir) || !(tmax > 0.005))
    {
        return 1.0;
    }
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, ll_rt_tlas, gl_RayFlagsTerminateOnFirstHitEXT, 0x0F, ray_origin, 0.005, ray_dir, tmax);
    // a ray is given a limited number of masked candidates: a structure that is not what it should be (garbage geometry spread over the scene) must cost a frame some shadows, not
    // a GPU job that outlasts the 2 seconds the driver allows (VK_ERROR_DEVICE_LOST: the viewer cannot go on). The ray stops with what it has found: no hit, lit.
    uint candidates = 0u;
    bool capped = false;
    while (rayQueryProceedEXT(rq))
    {
        if (++candidates > LL_RT_MAX_CANDIDATES)
        {
            rayQueryTerminateEXT(rq);
            capped = true;
            break;
        }
        if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT)
        {
            uint record = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false)) + uint(rayQueryGetIntersectionGeometryIndexEXT(rq, false));
            if (rtAlphaHit(record, uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, false)), rayQueryGetIntersectionBarycentricsEXT(rq, false), pos_screen, lod))
            {
                rayQueryConfirmIntersectionEXT(rq);
            }
        }
    }
    const bool blocked = rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT;
    rt_blocker_t = blocked ? rayQueryGetIntersectionTEXT(rq, true) : 0.0;
    if (capped && !blocked)
    {
        return LL_RT_CAPPED_RESULT;
    }
    return blocked ? 0.0 : 1.0;
}

float rtSunRay(vec3 origin, vec3 dir, vec2 pos_screen, float lod)
{
    return rtShadowRay(origin, dir, 2000.0, pos_screen, lod);
}

// Soft version for the shadows of traced hits (the glossy mirror, local lights and the sun at a hit): masked and blended candidates do not stop the ray, each one lets through the fraction its blurred
// alpha leaves (rtAlphaCover), the product is the light that arrives; an opaque hit blocks all. 1 = lit, 0 = blocked.
// LL_VK_RT_SUN_SELFNEAR (metres, default 0.8): the distance up to which a skinned avatar surface counts as a translucent self shadow (see rtShadowRaySoftS). The floor of the light fades out between 40 % of it and
// all of it, so a hand held at that distance does not cast two shadows (a light one for the fingers inside the limit and a dark one for the wrist outside it)
#ifndef LL_RT_SUN_SELF_NEAR
#define LL_RT_SUN_SELF_NEAR 0.8
#endif
// LL_VK_RT_SUN_SELFFADE (fraction of SELFNEAR, default 0.4): where the fade of the floor starts; 1 = no fade (the floor is the same for every close avatar surface: no contours of the head's layers in a shadow)
#ifndef LL_RT_SUN_SELF_FADE
#define LL_RT_SUN_SELF_FADE 0.4
#endif
#define LL_RT_SUN_SELF_NEAR0 (LL_RT_SUN_SELF_NEAR * LL_RT_SUN_SELF_FADE)
// LL_VK_RT_SUN_PENUMBRA (metres, default 0.008 since 2026-11-01; 0 = off, with LL_VK_RT_SUN_ALPHA_STRENGTH 0.6 that is the translucent floor of 2026-10-28): the width of the penumbra of a close avatar self shadow on the skin. A ray from 5 cm away gives a crisp edge (a copy of the lips on the neck, the fingers on the shoulder),
// where skin scatters light a few millimetres sideways and blurs it. When the first ray of a pixel hit a close skinned surface at distance t, the other rays of the pixel go to a disc of radius PENUMBRA / t (at most 0.35).
#ifndef LL_RT_SUN_PENUMBRA
#define LL_RT_SUN_PENUMBRA 0.008
#endif
// LL_VK_RT_SUN_PENUMBRA_RAYS (default 8, at most 16): the rays of a pixel whose first ray met a close skinned surface. Two rays on a wide disc give a handful of levels per pixel, and a snapshot has no accumulation to
// smooth them: the soft edge is a stack of steps (the lips and the jaw drawn as contours in the shadow on the neck). Only those pixels pay.
#ifndef LL_RT_SUN_PENUMBRA_RAYS
#define LL_RT_SUN_PENUMBRA_RAYS 8
#endif
float rt_near_skin_t = 0.0;                 // out of rtShadowRaySoftS: the distance of the nearest close skinned surface its ray met (0 = none)
// how much of the floor (1 - strength) a close skinned surface at distance t keeps: 1 up to LL_RT_SUN_SELF_NEAR0, fading to 0 at LL_RT_SUN_SELF_NEAR (a plain cut when there is no fade: smoothstep is undefined then)
float rtSelfFloorScale(float t)
{
    if (LL_RT_SUN_SELF_NEAR0 >= LL_RT_SUN_SELF_NEAR)
    {
        return t < LL_RT_SUN_SELF_NEAR ? 1.0 : 0.0;
    }
    return 1.0 - smoothstep(LL_RT_SUN_SELF_NEAR0, LL_RT_SUN_SELF_NEAR, t);
}
// Self shadows of an avatar are translucent: a skinned avatar surface (a hair card, the chin, a strand over the cheek, an arm over the torso) within LL_RT_SUN_SELF_NEAR metres of the pixel it shadows lets
// light through (scattering in hair and skin, bounced light): together they block at most `strength` of the light. Farther away, and everything that is not an avatar, blocks what it covers (the ground shadow
// of an avatar stays dark). Nothing here has to know which object is hair: rigged hair and a mesh head are both just skinned records, and the distance decides.
float rtShadowRaySoftS(vec3 origin, vec3 dir, float tmax, float lod, float strength)
{
    mat3 to_scene;
    vec3 camera;
    rtSceneFrame(to_scene, camera);
    const vec3 ray_origin = to_scene * origin + camera;
    const vec3 ray_dir = to_scene * dir;
    if (!rtRayOk(ray_origin, ray_dir) || !(tmax > 0.005))
    {
        return 1.0;
    }
    rt_near_skin_t = 0.0;
    rt_blocker_t = 0.0;
    float transmit = 1.0;                       // the light that gets past everything but close skinned avatar surfaces
    float skin = 1.0;                           // the light that gets past the close skinned avatar surfaces: their product, but never less than the floor (a cap on the TOTAL, a lock of hair is dozens of cards)
                                                // (the floor 1 - strength fades to 0 between LL_RT_SUN_SELF_NEAR0 and LL_RT_SUN_SELF_NEAR of the NEAREST of those surfaces, rt_near_skin_t: the first occluder decides
                                                // how dark the shadow is and what is behind it adds nothing, a silhouette; the farthest one made fingers behind a hand darken the shadow through the hand)
    const float skin_floor = 1.0 - strength;
    float t_start = 0.005;
    uint mask = 0x0Fu;                          // everything; once the avatar's side of the answer is final, only the world (0x03)
    bool capped = false;                        // the candidate limit (LL_VK_RT_CANDIDATES, default 96) was reached: the walk stopped before it met everything on the way
    bool none = true;
    bool settled = false;                       // the walk through everything ended early: the avatar's side of the answer is final (see below), the world pass follows
    // The avatar's side of the answer becomes final in two ways, and then one more query goes through the WORLD alone, over the whole ray again (so a world card counted once is not counted twice: the
    // candidates come in no order), for a tree or an awning over the avatar:
    // - a close solid avatar hit: it counts as covering fully (so the floor applies). The walk does not go on through the avatar: a mesh head is several closed shells (the face, the mouth bag, two rows of
    //   teeth, the tongue, the eyes) and a walk through them that gave up after four solid hits drew the shells' contours in the chin's shadow on the neck. The nearest surface decides; the rest is a silhouette.
    // - the cards' product `skin` is under the floor while the nearest of them is inside the no-fade range: max(skin, floor) is the floor from here on whatever the remaining cards cover, so the walk
    //   through them (an alpha read per card, dozens in a lock) is wasted; it cost 0.3 ms of GPU a frame in the hair's own pixels (lap ss1-sunself1, 2026-10-28). rt-debug.txt sunself=2 keeps the
    //   full walk for an A/B.
    for (int segment = 0; segment < 2; ++segment)
    {
        rayQueryEXT rq;
        rayQueryInitializeEXT(rq, ll_rt_tlas, gl_RayFlagsNoneEXT, mask, ray_origin, t_start, ray_dir, tmax);
        uint candidates = 0u;
        while (rayQueryProceedEXT(rq))
        {
            if (++candidates > LL_RT_MAX_CANDIDATES)
            {
                rayQueryTerminateEXT(rq);
                capped = true;
                break;
            }
            if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT)
            {
                uint record = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false)) + uint(rayQueryGetIntersectionGeometryIndexEXT(rq, false));
                const bool skinned = record != 0u && record < ll_rt_geoms.g[0].first && (ll_rt_geoms.g[record].flags & 8192u) != 0u;
                const float t_cand = rayQueryGetIntersectionTEXT(rq, false);
                const bool close = t_cand < LL_RT_SUN_SELF_NEAR;
                const float cover = rtAlphaCover(record, uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, false)), rayQueryGetIntersectionBarycentricsEXT(rq, false), lod);
                if (cover >= 0.5)
                {
                    rt_blocker_t = (rt_blocker_t == 0.0) ? t_cand : min(rt_blocker_t, t_cand);      // a card that covers at least half is a blocker for the penumbra's distance
                }
                if (skinned && close)
                {
                    skin *= 1.0 - cover;
                    rt_near_skin_t = (rt_near_skin_t == 0.0) ? t_cand : min(rt_near_skin_t, t_cand);
                    if (skin_floor > 0.0 && skin <= skin_floor && rt_near_skin_t < LL_RT_SUN_SELF_NEAR0 && rt_sun_self_off < 1.5)
                    {
                        rayQueryTerminateEXT(rq);
                        settled = true;
                        break;
                    }
                }
                else
                {
                    transmit *= 1.0 - cover;
                }
                if (transmit * max(skin, skin_floor * rtSelfFloorScale(rt_near_skin_t)) < 0.03)
                {
                    rayQueryTerminateEXT(rq);
#if LL_RT_SUN_DEBUG == 1
                    return 1.0;             // test: only solid (opaque) hits make a shadow
#else
                    return 0.0;
#endif
                }
            }
        }
        if (settled)
        {
            // the world pass: the whole ray again through the world alone, its cards counted from scratch
            settled = false;
            transmit = 1.0;
            mask = 0x03u;
            continue;
        }
        none = rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT;
        if (none)
        {
            break;
        }
        // a solid hit: a close skinned avatar surface is translucent too (only when there is a floor to apply), anything else blocks
        const float t_hit = rayQueryGetIntersectionTEXT(rq, true);
        rt_blocker_t = (rt_blocker_t == 0.0) ? t_hit : min(rt_blocker_t, t_hit);
        const uint record = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true)) + uint(rayQueryGetIntersectionGeometryIndexEXT(rq, true));
        const bool skinned = record != 0u && record < ll_rt_geoms.g[0].first && (ll_rt_geoms.g[record].flags & 8192u) != 0u;
#if LL_RT_SUN_DEBUG == 0
        if (skin_floor > 0.0 && skinned && t_hit < LL_RT_SUN_SELF_NEAR && segment == 0)
        {
            skin = 0.0;
            rt_near_skin_t = (rt_near_skin_t == 0.0) ? t_hit : min(rt_near_skin_t, t_hit);
            transmit = 1.0;
            mask = 0x03u;
            continue;
        }
#endif
#if LL_RT_SUN_DEBUG == 0
        if (skin_floor <= 0.0 && skinned && t_hit < LL_RT_SUN_SELF_NEAR && LL_RT_SUN_PENUMBRA > 0.0)
        {
            // <FS> no floor (strength 1) with the penumbra cone on: the solid close avatar hit still blocks, but the pixel is told about it (a pixel's other rays widen with the distance of the nearest close avatar surface)
            rt_near_skin_t = (rt_near_skin_t == 0.0) ? t_hit : min(rt_near_skin_t, t_hit);
        }
#endif
        break;
    }
#ifdef LL_RT_CAP_DEBUG
    if (capped && none)
    {
        return 0.0;                         // test (LL_VK_RT_CAPDEBUG=1): a ray that stopped at the limit is drawn fully shadowed
    }
#endif
    const float floor_now = skin_floor * rtSelfFloorScale(rt_near_skin_t);
#if LL_RT_SUN_DEBUG == 1
    return none ? 1.0 : 0.0;                // test (LL_VK_RT_SUN_DEBUG=1): only solid (opaque) hits make a shadow, what the alpha loop found is ignored
#elif LL_RT_SUN_DEBUG == 2
    return none ? transmit * max(skin, floor_now) : 1.0;      // test (=2): only what the alpha loop found makes a shadow, solid hits are ignored
#else
    return none ? transmit * max(skin, floor_now) : 0.0;
#endif
}

// strength: the most light the skinned avatar cards (hair) may block together (1 = all, as before; only masked and blended candidates reach the loop: opaque geometry is a committed hit and blocks everything)
float rtShadowRaySoft(vec3 origin, vec3 dir, float tmax, float lod)
{
    return rtShadowRaySoftS(origin, dir, tmax, lod, 1.0);
}

// what the traced hits call (the sun at a hit and, through rtShadowRayArea, the local lights)
float rtShadowRayHit(vec3 origin, vec3 dir, float tmax, vec2 pos_screen, float lod)
{
    return rtShadowRaySoft(origin, dir, tmax, lod);
}

// Shadow of a local light of the traced hits: the light has a size (a lamp, a projector box), so its shadow edges have a penumbra: 4 soft rays to points of a disc of `radius` metres around the
// light's centre (perpendicular to the way to the light), rotated per pixel. A single ray from a point gave a hard edge that followed the triangles of the occluder (the hair on the shoulder:
// stair steps). rt_hit_shadow_variant 9 (glossyrt=28): the single ray of before. margin = how far short of the target a ray stops (the light sits inside its prim).
float rtShadowRayArea(vec3 origin, vec3 light_center, float margin, float radius, vec2 pos_screen, float lod)
{
    vec3 to_light = light_center - origin;
    float dist = length(to_light);
    if (!(dist > 0.01))
    {
        return 1.0;
    }
    vec3 L = to_light / dist;
    if (rt_hit_shadow_variant == 9)
    {
        return rtShadowRayHit(origin, L, dist - margin, pos_screen, lod);
    }
    const int samples = rt_hit_shadow_samples <= 0 ? 4 : min(rt_hit_shadow_samples, 4);
    if (samples == 1)
    {
        return rtShadowRayHit(origin, L, dist - margin, pos_screen, lod);
    }
    vec3 up = abs(L.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 t = normalize(cross(L, up));
    vec3 b = cross(L, t);
    // interleaved gradient noise of the pixel (a much finer, less grainy pattern than a hash: neighbouring pixels differ evenly), moved on every frame so that the temporal upscaler averages it
    const float rot = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy + 5.588238 * rt_noise_frame, vec2(0.06711056, 0.00583715))));
    float sum = 0.0;
    for (int i = 0; i < samples; ++i)
    {
        const float a = rot + (samples == 2 ? 3.1415927 : 1.5707963) * float(i);
        const float rr = radius * ((i & 1) == 0 ? 0.45 : 0.9);
        vec3 target = light_center + (t * cos(a) + b * sin(a)) * rr;
        vec3 dv = target - origin;
        float dd = length(dv);
        sum += rtShadowRayHit(origin, dv / dd, max(dd - margin, 0.01), pos_screen, lod);
    }
    return sum / float(samples);
}

// <FS> Ray traced reflections (doc/vulkan/PLAN-rt-materials.md, stage M4): one ray from a surface into the ray scene, the hit shaded from the geometry records and the material table.
// Used first by the water (waterF.glsl, permutation LL_RT_REFLECTIONS). They live here because the scene declarations (the ll_rt_* block, with the function ll_rt_slot) are added to every
// file that mentions ll_rt_tlas, and two files of one program may not both have them.
vec3 rtSrgbToLinear(vec3 c)
{
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

// What the surface of a hit looks like: linear albedo, emissive, the (smooth or flat) normal in object space, alpha for the mask test. false: the record cannot be read.
// The same decode as the material debug view (rtMaterialDebugF.glsl dbgSurface), which is the proof that records, uvs, colours and slots are right.
// full: also decode what the lighting of a hit needs (hit shading quality 1): n_shaded = the normal with the normal map applied (object space, a tangent frame from the triangle's uv derivatives),
// mr = (metallic, roughness, 1 for a glTF material else 0, the emissive amount of a legacy material whose diffuse alpha is an emissive mask (else 0)), spec = legacy specular map (linear rgb, alpha = glossiness; alpha 0 = no specular map). Without full they are neutral.
bool rtShadeSurfaceEx(uint record, uint prim, vec2 bary, float lod, bool full, out vec3 albedo, out vec3 emissive, out vec3 n_obj, out vec3 n_shaded, out float alpha, out bool fullbright, out vec4 mr, out vec4 spec)
{
    rt_soft_masked = false;
    mr = vec4(0.0, 1.0, 0.0, 0.0);
    spec = vec4(0.0);
    n_shaded = vec3(0.0, 1.0, 0.0);
    albedo = vec3(0.5);
    emissive = vec3(0.0);
    n_obj = vec3(0.0, 1.0, 0.0);
    alpha = 1.0;
    fullbright = false;
    if (record == 0u || record >= ll_rt_geoms.g[0].first)
    {
        return false;
    }
    LLRtGeom g = ll_rt_geoms.g[record];
    if ((g.flags & 256u) != 0u || (g.indices.x == 0u && g.indices.y == 0u) || (g.positions.x == 0u && g.positions.y == 0u))
    {
        return false;
    }
    vec3 w = vec3(1.0 - bary.x - bary.y, bary.x, bary.y);
    LLRtIndices ib = LLRtIndices(g.indices);
    uint ix[3];
    for (uint k = 0u; k < 3u; ++k)
    {
        uint i = g.first + prim * 3u + k;
        uint word = ib.w[i >> 1];
        ix[k] = rtClampVertex(g, ((i & 1u) != 0u) ? (word >> 16) : (word & 0xFFFFu));
    }
    LLRtPositions pb = LLRtPositions(g.positions);
    vec3 p0 = pb.v[ix[0]].xyz, p1 = pb.v[ix[1]].xyz, p2 = pb.v[ix[2]].xyz;
    n_obj = normalize(cross(p1 - p0, p2 - p0));
    if ((g.flags & 1024u) != 0u)
    {
        LLRtNormals nb = LLRtNormals(g.normals);
        vec3 ns = nb.n[ix[0]].xyz * w.x + nb.n[ix[1]].xyz * w.y + nb.n[ix[2]].xyz * w.z;
        n_obj = dot(ns, ns) > 1e-8 ? normalize(ns) : n_obj;
    }
    n_shaded = n_obj;
    uint mat_id = g.material;
    if (mat_id == 0u || mat_id >= 16384u)
    {
        return true;                    // no material: a neutral grey
    }
    LLRtMaterial m = LLRtMaterials(ll_rt_geoms.g[0].normals).m[mat_id];
    uint mflags = floatBitsToUint(m.params.z);
    vec2 uv = vec2(0.0);
    if ((g.flags & 8u) != 0u)
    {
        LLRtTexcoords tc = LLRtTexcoords(g.texcoords);
        uv = tc.t[ix[0]] * w.x + tc.t[ix[1]] * w.y + tc.t[ix[2]] * w.z;
        uv = m.uv0 * uv.x + m.uv1 * uv.y + m.uv2;
    }
    uint tex_index = 0u;
    if ((mflags & 16u) != 0u)
    {
        tex_index = floatBitsToUint(pb.v[ix[0]].w) & 3u;
    }
    uint pair = (tex_index < 2u) ? m.slots.x : m.slots.y;
    uint slot = ll_rt_slot((pair >> ((tex_index & 1u) * 16u)) & 0xFFFFu);
    rt_soft_masked = rt_hair_soft != 0 && (mflags & 4u) != 0u && (g.flags & 8192u) != 0u;
    vec4 tex = textureLod(ll_rt_tex[nonuniformEXT(LL_HM(slot))], uv, rt_soft_masked && rt_hair_soft == 3 ? lod + 1.5 : lod);
    vec4 vcol = vec4(1.0);
    if ((g.flags & 16u) != 0u)
    {
        LLRtColors col = LLRtColors(g.colors);
        vcol = (unpackUnorm4x8(col.c[ix[0]]) * w.x + unpackUnorm4x8(col.c[ix[1]]) * w.y + unpackUnorm4x8(col.c[ix[2]]) * w.z);
    }
    if ((mflags & 64u) != 0u)
    {
        vcol.a = 1.0;                   // the legacy materials passes test the texture alpha alone
    }
    alpha = tex.a * vcol.a * m.color.a;
    if ((mflags & 4u) != 0u)
    {
        if (rt_soft_masked)
        {
            alpha = smoothstep(m.emissive.w - 0.2, m.emissive.w + 0.2, alpha);       // hair taken softly: the opacity near the cutoff, not all or nothing
        }
        else
        {
            alpha = (alpha >= m.emissive.w) ? 1.0 : 0.0;        // alpha masked: the draw's own cutoff
        }
    }
    if ((mflags & 2u) != 0u)
    {
        albedo = rtSrgbToLinear(tex.rgb) * rtSrgbToLinear(vcol.rgb) * m.color.rgb;     // glTF: the factor is linear
        vec3 emissive_tex = vec3(1.0);
        uint emissive_pin = m.slots2.y & 0xFFFFu;
        if (emissive_pin != 0u)
        {
            uint emissive_slot = ll_rt_slot(emissive_pin);
            emissive_tex = vec3(0.0);           // <FS> a named emissive map that has no slot yet (not resident): no glow, not the factor alone (a white glow on hair)
            if (emissive_slot != 0u)
            {
                emissive_tex = rtSrgbToLinear(textureLod(ll_rt_tex[nonuniformEXT(LL_HM(emissive_slot))], uv, lod).rgb);
            }
        }
        emissive = m.emissive.xyz * emissive_tex;
    }
    else
    {
        albedo = rtSrgbToLinear(tex.rgb * vcol.rgb * m.color.rgb);                     // legacy: texture and vertex colour are sRGB values
    }
    fullbright = (mflags & 1u) != 0u;
    if (full && !fullbright)
    {
        if ((mflags & 8u) != 0u && (mflags & 2u) == 0u)
        {
            mr.w = -1.0;                            // a legacy material that is alpha BLENDED (hair, some clothes): the viewer lights it in the forward alpha pass, not in the soften pass (softenLightF.glsl rtHitForwardLights)
        }
        else if ((mflags & 32u) != 0u && (mflags & 2u) == 0u)
        {
            mr.w = clamp(tex.a, 0.0, 1.0);          // DIFFUSE_ALPHA_MODE_EMISSIVE: the stock lighting blends the lit colour towards the albedo by this (softenLightF.glsl: mix(color, baseColor, baseColor.a))
        }
        // the normal map through a tangent frame (the same decode as the material debug view, rtMaterialDebugF.glsl); its alpha is the glossiness mask of a legacy material
        float normal_alpha = 1.0;
        uint normal_pin = m.slots2.x & 0xFFFFu;
        if (normal_pin != 0u && (g.flags & 8u) != 0u)
        {
            uint normal_slot = ll_rt_slot(normal_pin);
            if (normal_slot != 0u && (g.flags & 8192u) != 0u)
            {
                // a skinned avatar draw (kRecSkinned): the tangent frame below is made per triangle and the avatar's normals are smooth (skinned by the compute pass): the frame jumps at every triangle edge
                // and roughens skin, hair edges and hands. Only the alpha of the map (the legacy glossiness mask) is read.
                normal_alpha = textureLod(ll_rt_tex[nonuniformEXT(LL_HM(normal_slot))], uv, lod).a;
            }
            else if (normal_slot != 0u)
            {
                LLRtTexcoords tcn = LLRtTexcoords(g.texcoords);
                vec2 a0 = tcn.t[ix[0]], a1 = tcn.t[ix[1]], a2 = tcn.t[ix[2]];
                a0 = m.uv0 * a0.x + m.uv1 * a0.y + m.uv2;
                a1 = m.uv0 * a1.x + m.uv1 * a1.y + m.uv2;
                a2 = m.uv0 * a2.x + m.uv1 * a2.y + m.uv2;
                vec3 e1 = p1 - p0, e2 = p2 - p0;
                vec2 d1 = a1 - a0, d2 = a2 - a0;
                float det = d1.x * d2.y - d2.x * d1.y;
                if (abs(det) > 1e-12)
                {
                    vec3 tangent = (e1 * d2.y - e2 * d1.y) / det;
                    vec3 bitangent = (e2 * d1.x - e1 * d2.x) / det;
                    tangent = tangent - n_obj * dot(n_obj, tangent);
                    if (dot(tangent, tangent) > 1e-12)
                    {
                        tangent = normalize(tangent);
                        vec3 bit = cross(n_obj, tangent);
                        bit *= (dot(bit, bitangent) < 0.0) ? -1.0 : 1.0;       // the handedness of the uv layout
                        vec4 nmt = textureLod(ll_rt_tex[nonuniformEXT(LL_HM(normal_slot))], uv, lod);
                        vec3 nm = nmt.xyz * 2.0 - 1.0;
                        normal_alpha = nmt.a;
                        n_shaded = normalize(tangent * nm.x + bit * nm.y + n_obj * nm.z);
                    }
                }
            }
        }
        // roughness and metallic: glTF = factors times the metallic-roughness map (green, blue); legacy = the specular map (rgb colour, alpha glossiness), none = matte
        uint mr_pin = m.slots2.x >> 16;
        uint mr_slot = mr_pin != 0u ? ll_rt_slot(mr_pin) : 0u;
        if ((mflags & 2u) != 0u)
        {
            float metallic = m.params.x, rough = m.params.y;
            if (mr_slot != 0u)
            {
                vec3 t = textureLod(ll_rt_tex[nonuniformEXT(LL_HM(mr_slot))], uv, lod).rgb;
                rough *= t.g;
                metallic *= t.b;
            }
            mr = vec4(clamp(metallic, 0.0, 1.0), clamp(rough, 0.0, 1.0), 1.0, 0.0);
        }
        else if (mr_slot != 0u)
        {
            // like the G-buffer pass of legacy materials (materialF.glsl): colour = specular map x the material's specular colour (m.emissive.xyz), glossiness = the exponent / 255 (m.params.w) x normal map alpha
            vec4 sm = textureLod(ll_rt_tex[nonuniformEXT(LL_HM(mr_slot))], uv, lod);
            spec = vec4(rtSrgbToLinear(sm.rgb * m.emissive.xyz), m.params.w * normal_alpha);
        }
    }
    return true;
}

bool rtShadeSurface(uint record, uint prim, vec2 bary, float lod, out vec3 albedo, out vec3 emissive, out vec3 n_obj, out float alpha, out bool fullbright)
{
    vec3 n_shaded;
    vec4 mr, spec;
    return rtShadeSurfaceEx(record, prim, bary, lod, false, albedo, emissive, n_obj, n_shaded, alpha, fullbright, mr, spec);
}

// Is the record's material alpha BLENDED (legacy: hair, some clothes; glTF: translucent meshes such as an avatar's shell)? The main view draws those in the forward alpha pass over the opaque layers behind them.
bool rtRecordBlended(uint record)
{
    if (record == 0u || record >= ll_rt_geoms.g[0].first)
    {
        return false;
    }
    uint mat_id = ll_rt_geoms.g[record].material;
    if (mat_id == 0u || mat_id >= 16384u)
    {
        return false;
    }
    uint mf = floatBitsToUint(LLRtMaterials(ll_rt_geoms.g[0].normals).m[mat_id].params.z);
    return (mf & 8u) != 0u;         // <FS> glTF blends too (the translucent shell of an avatar): a layer over what is behind it, lit like any hit
}

// Decode a hit that is known by record, triangle, barycentrics and distance (a committed intersection, or a blended candidate kept aside): the surface, the eye space normals facing the ray,
// the sun ray from the hit. w2o = the instance's world to object matrix of the intersection.
bool rtResolveHit(uint record, uint prim, vec2 bary, mat4x3 w2o, float t, vec3 origin, vec3 dir, vec3 sun_dir, vec2 pos_screen, bool full, mat3 to_scene,
                  out vec3 albedo, out vec3 emissive, out vec3 n_geo_eye, out vec3 n_eye, out vec3 hit_pos, out float shadow, out bool fullbright, out vec4 mr, out vec4 spec, out float alpha)
{
    n_geo_eye = vec3(0.0, 0.0, 1.0);
    n_eye = n_geo_eye;
    hit_pos = origin;
    shadow = 0.0;
    float lod = rt_hit_shadow_variant == 4 ? 0.0 : clamp(0.5 * log2(max(t + length(origin), 1.0)) - 1.0, 0.0, 4.0);
    vec3 n_obj, n_shaded;
    if (!rtShadeSurfaceEx(record, prim, bary, lod, full, albedo, emissive, n_obj, n_shaded, alpha, fullbright, mr, spec))
    {
        return false;                   // a record that cannot be read: not a hit
    }
    n_geo_eye = normalize(normalize(n_obj * mat3(w2o)) * to_scene);
    n_eye = full ? normalize(normalize(n_shaded * mat3(w2o)) * to_scene) : n_geo_eye;
    if (dot(n_geo_eye, dir) > 0.0)
    {
        n_geo_eye = -n_geo_eye;         // two sided
        n_eye = -n_eye;
    }
    hit_pos = origin + dir * t;
    if (!fullbright)
    {
        float nl = clamp(dot(n_geo_eye, sun_dir), 0.0, 1.0);
        if (nl > 0.0)
        {
            if (rt_hit_shadow_variant == 1)
            {
                shadow = 1.0;
            }
            else
            {
                shadow = rtShadowRayHit(hit_pos + n_geo_eye * ((rt_hit_shadow_variant == 2 ? 0.15 : 0.02) + 0.0006 * length(hit_pos)), sun_dir, 2000.0, pos_screen, lod);
            }
        }
    }
    return true;
}

// The trace and the decode of the hit, without lighting it: everything the lighting of the soften pass needs (hit quality 1, softenLightF.glsl) or the flat shading below.
// full: decode normal map, roughness / metallic / specular map as well. n_geo_eye = the (smooth) normal of the geometry, n_eye = the shaded one (normal map), both eye space, facing the ray.
// shadow = the sun ray towards sun_dir from the hit (0 or 1, 0 when the geometry faces away), mr / spec as rtShadeSurfaceEx. Returns false for a miss or a record that cannot be read.
// layers: alpha BLENDED surfaces (hair) never stop the ray, like they do not in the main view where the alpha pass draws them over the opaque scene: the nearest one in front of the hit comes back
// as l_* (layer_alpha its opacity, 0 = none), to be shaded and composited over the hit by the caller. A blended surface with nothing behind it is the hit itself, layer_alpha = -its opacity.
bool rtTraceHitEx(vec3 origin, vec3 dir, vec3 sun_dir, vec2 pos_screen, bool full, bool layers, out vec3 albedo, out vec3 emissive, out vec3 n_geo_eye, out vec3 n_eye, out vec3 hit_pos, out float t,
                  out float shadow, out bool fullbright, out vec4 mr, out vec4 spec,
                  out float layer_alpha, out vec3 l_albedo, out vec3 l_emissive, out vec3 l_n_geo, out vec3 l_n, out vec3 l_hp, out float l_t, out float l_shadow, out bool l_fb, out vec4 l_mr, out vec4 l_spec,
                  out float m_alpha, out vec3 m_albedo, out vec3 m_emissive, out vec3 m_n_geo, out vec3 m_n, out vec3 m_hp, out float m_t, out float m_shadow, out bool m_fb, out vec4 m_mr, out vec4 m_spec)
{
    albedo = vec3(0.0);
    emissive = vec3(0.0);
    n_geo_eye = vec3(0.0, 0.0, 1.0);
    n_eye = n_geo_eye;
    hit_pos = origin;
    t = 0.0;
    shadow = 0.0;
    fullbright = false;
    mr = vec4(0.0, 1.0, 0.0, 0.0);
    spec = vec4(0.0);
    layer_alpha = 0.0;
    l_albedo = vec3(0.0);
    l_emissive = vec3(0.0);
    l_n_geo = n_geo_eye;
    l_n = n_geo_eye;
    l_hp = origin;
    l_t = 0.0;
    l_shadow = 0.0;
    l_fb = false;
    l_mr = mr;
    l_spec = spec;
    m_alpha = 0.0;                      // a second blended layer behind the first (hair underside and the shell on it: both are drawn over the opaque surface, back to front)
    m_albedo = vec3(0.0);
    m_emissive = vec3(0.0);
    m_n_geo = n_geo_eye;
    m_n = n_geo_eye;
    m_hp = origin;
    m_t = 0.0;
    m_shadow = 0.0;
    m_fb = false;
    m_mr = mr;
    m_spec = spec;
    mat3 to_scene;
    vec3 camera;
    rtSceneFrame(to_scene, camera);
    float tmax = 1500.0;
    const vec3 ray_origin = to_scene * origin + camera;
    const vec3 ray_dir = to_scene * dir;
    if (!rtRayOk(ray_origin, ray_dir))
    {
        return false;
    }
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, ll_rt_tlas, 0, 0x0F, ray_origin, 0.02, ray_dir, tmax);
    float lod_start = rt_hit_shadow_variant == 4 ? 0.0 : clamp(0.5 * log2(max(length(origin), 1.0)) - 1.0, 0.0, 4.0);
    uint steps = 0u;
    // the nearest blended candidate seen so far (its place in the scene, kept until the ray is done)
    float c_t = 1e30, c_alpha = 0.0;
    float transmit = 1.0;                       // hair taken softly (rt_hair_soft 2, 3): what is left of the light after every such card the ray crossed
    float seen = 0.0, seen_alpha = 0.0;         // debug: how many blended candidates the ray met and the highest opacity among them
    uint c_record = 0u, c_prim = 0u;
    vec2 c_bary = vec2(0.0);
    mat4x3 c_w2o = mat4x3(1.0);
    float d_t = 1e30, d_alpha = 0.0;            // the second nearest blended candidate
    uint d_record = 0u, d_prim = 0u;
    vec2 d_bary = vec2(0.0);
    mat4x3 d_w2o = mat4x3(1.0);
    while (rayQueryProceedEXT(rq))
    {
        if (++steps > (rt_hit_shadow_variant == 3 ? 1024u : 64u))
        {
            rayQueryTerminateEXT(rq);
            break;
        }
        if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT)
        {
            // a masked structure: its own alpha decides
            uint record = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false)) + uint(rayQueryGetIntersectionGeometryIndexEXT(rq, false));
            uint prim = uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, false));
            vec2 bary = rayQueryGetIntersectionBarycentricsEXT(rq, false);
            vec3 a, e, n;
            float alpha;
            bool fb;
            if (rtShadeSurface(record, prim, bary, lod_start, a, e, n, alpha, fb))
            {
                if (layers && (rtRecordBlended(record) || rt_soft_masked))
                {
                    seen += 1.0;
                    seen_alpha = max(seen_alpha, alpha);
                    float ct = rayQueryGetIntersectionTEXT(rq, false);
                    if (alpha >= 0.03 && rt_soft_masked && rt_hair_soft >= 2)
                    {
                        transmit *= 1.0 - min(alpha, 1.0);
                    }
                    if (alpha >= 0.03 && ct < c_t)
                    {
                        d_t = c_t;                      // the nearest so far becomes the second
                        d_alpha = c_alpha;
                        d_record = c_record;
                        d_prim = c_prim;
                        d_bary = c_bary;
                        d_w2o = c_w2o;
                        c_t = ct;
                        c_alpha = min(alpha, 1.0);
                        c_record = record;
                        c_prim = prim;
                        c_bary = bary;
                        c_w2o = rayQueryGetIntersectionWorldToObjectEXT(rq, false);
                    }
                    else if (alpha >= 0.03 && ct < d_t)
                    {
                        d_t = ct;
                        d_alpha = min(alpha, 1.0);
                        d_record = record;
                        d_prim = prim;
                        d_bary = bary;
                        d_w2o = rayQueryGetIntersectionWorldToObjectEXT(rq, false);
                    }
                }
                else if (alpha >= 0.5)
                {
                    rayQueryConfirmIntersectionEXT(rq);
                }
            }
            else
            {
                rayQueryConfirmIntersectionEXT(rq);
            }
        }
    }
    if (rt_hair_soft >= 2 && c_record != 0u)
    {
        c_alpha = max(c_alpha, 1.0 - transmit);       // the combined opacity of the hair cards the ray crossed
    }
    const bool have_main = rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT;
    l_t = seen;                          // debug (rt_hit_quality 14): without a layer these two tell what the ray saw of blended surfaces
    l_shadow = seen_alpha;
    if (!have_main && c_record == 0u)
    {
        return false;
    }
    float alpha;
    if (have_main)
    {
        t = rayQueryGetIntersectionTEXT(rq, true);
        uint record = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true)) + uint(rayQueryGetIntersectionGeometryIndexEXT(rq, true));
        if (!rtResolveHit(record, uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, true)), rayQueryGetIntersectionBarycentricsEXT(rq, true), rayQueryGetIntersectionWorldToObjectEXT(rq, true),
                          t, origin, dir, sun_dir, pos_screen, full, to_scene, albedo, emissive, n_geo_eye, n_eye, hit_pos, shadow, fullbright, mr, spec, alpha))
        {
            return false;
        }
        if (c_record != 0u && c_t < t)
        {
            l_t = c_t;
            float la;
            if (rtResolveHit(c_record, c_prim, c_bary, c_w2o, c_t, origin, dir, sun_dir, pos_screen, full, to_scene, l_albedo, l_emissive, l_n_geo, l_n, l_hp, l_shadow, l_fb, l_mr, l_spec, la))
            {
                layer_alpha = c_alpha;
            }
            if (d_record != 0u && d_t < t)
            {
                m_t = d_t;
                float ma;
                if (rtResolveHit(d_record, d_prim, d_bary, d_w2o, d_t, origin, dir, sun_dir, pos_screen, full, to_scene, m_albedo, m_emissive, m_n_geo, m_n, m_hp, m_shadow, m_fb, m_mr, m_spec, ma))
                {
                    m_alpha = d_alpha;
                }
            }
        }
        return true;
    }
    // only a blended surface on the way: it is the hit, partly transparent over whatever the caller has behind it
    t = c_t;
    if (!rtResolveHit(c_record, c_prim, c_bary, c_w2o, c_t, origin, dir, sun_dir, pos_screen, full, to_scene, albedo, emissive, n_geo_eye, n_eye, hit_pos, shadow, fullbright, mr, spec, alpha))
    {
        return false;
    }
    layer_alpha = -c_alpha;
    return true;
}

bool rtTraceHit(vec3 origin, vec3 dir, vec3 sun_dir, vec2 pos_screen, bool full, out vec3 albedo, out vec3 emissive, out vec3 n_geo_eye, out vec3 n_eye, out vec3 hit_pos, out float t,
                out float shadow, out bool fullbright, out vec4 mr, out vec4 spec)
{
    float la, lt, ls;
    vec3 l_albedo, l_emissive, l_n_geo, l_n, l_hp;
    bool l_fb;
    vec4 l_mr, l_spec;
    float ma, mt, ms;
    vec3 m_albedo, m_emissive, m_n_geo, m_n, m_hp;
    bool m_fb;
    vec4 m_mr, m_spec;
    return rtTraceHitEx(origin, dir, sun_dir, pos_screen, full, false, albedo, emissive, n_geo_eye, n_eye, hit_pos, t, shadow, fullbright, mr, spec,
                        la, l_albedo, l_emissive, l_n_geo, l_n, l_hp, lt, ls, l_fb, l_mr, l_spec, ma, m_albedo, m_emissive, m_n_geo, m_n, m_hp, mt, ms, m_fb, m_mr, m_spec);
}

// A ray from origin (eye space) along dir (unit, eye space) against the scene. The hit is lit like a legacy surface of the deferred pass: (ambient + sun * N.L * shadow) * albedo + emissive,
// the shadow from a ray towards the sun; fullbright surfaces show their albedo. Hits far away fade into fallback (the probe / sky colour already known for that direction).
// Returns rgb and a = 1 for a hit, a = 0 for a miss (the caller keeps what it has). This is the flat shading of the water and of the glossy mirror at hit quality 0.
vec4 rtReflection(vec3 origin, vec3 dir, vec3 sun_dir, vec3 sunlit_linear, vec3 amblit_linear, vec2 pos_screen, vec3 fallback)
{
    vec3 albedo, emissive, n_geo_eye, n_eye, hit_pos;
    float t, shadow;
    bool fullbright;
    vec4 mr, spec;
    if (!rtTraceHit(origin, dir, sun_dir, pos_screen, false, albedo, emissive, n_geo_eye, n_eye, hit_pos, t, shadow, fullbright, mr, spec))
    {
        return vec4(0.0);
    }
    vec3 color;
    if (fullbright)
    {
        color = albedo + emissive;
    }
    else
    {
        float nl = clamp(dot(n_geo_eye, sun_dir), 0.0, 1.0);
        color = (amblit_linear + nl * shadow * sunlit_linear) * albedo + emissive;
    }
    float fade = smoothstep(250.0, 1200.0, t);
    return vec4(mix(color, fallback, fade), 1.0);
}

// <FS> test switches (LL_VK_RT_SUN_BIAS metres, default 0.003: how far off the surface along the normal the ray starts. It was 0.015 until 2026-10-27: anything closer to a surface than that could not shadow
// it, so the shadow of a strand of hair gave out where the strand came close to the face; 0.003 is what the user found right by eye. LL_VK_RT_SUN_NOFACE=1: a surface facing away from the light is traced too)
#ifndef LL_RT_SUN_BIAS
#define LL_RT_SUN_BIAS 0.003
#endif
// LL_VK_RT_SUN_TMIN (metres, default 0 = off): the sun ray starts this far along the light direction: whatever is closer to the surface than that along the light cannot shadow it, while a shadow from farther up
// (a chin on the chest) keeps its whole shape. The first answer to the hard streaks of hair on skin (0.04), not needed since the soft alpha ray and the small start offset (below) give hair a continuous shadow.
// LL_VK_RT_SUN_BIAS_ALPHA (metres, default 0.015): the same for blended surfaces (hair cards a few millimetres apart shade each other, grey strands in the hair, when it is as small as the one of skin)
#ifndef LL_RT_SUN_BIAS_ALPHA
#define LL_RT_SUN_BIAS_ALPHA 0.015
#endif
// LL_VK_RT_SUN_ALPHA_STRENGTH (0..1, default 1 since 2026-11-01 with the penumbra cone: the cone makes the self shadow soft, no floor; 0.6 = the translucent floor of 2026-10-28; 1 = an avatar blocks its own light fully): the most light the close surfaces of an avatar may block together on that same
// avatar (rtShadowRaySoftS: hair on the face, a hand on the shoulder, the chin on the chest). Hair and skin are translucent and bounce light, so a self shadow is never as dark as an opaque object's.
// 0.6 is the user's choice by eye (2026-10-28, with SELFNEAR 0.8, the receiver probe and the 4 mm start disc).
// LL_VK_RT_SUN_HAIRFILL (0..1, default 0): the shadow of a blended surface (the hair itself) is lifted toward lit by this fraction: bounced light and scattering fill the shadows hair has on itself.
#ifndef LL_RT_SUN_ALPHA_STRENGTH
#define LL_RT_SUN_ALPHA_STRENGTH 1.0
#endif
#ifndef LL_RT_SUN_HAIRFILL
#define LL_RT_SUN_HAIRFILL 0.0
#endif
#ifndef LL_RT_SUN_DEBUG
#define LL_RT_SUN_DEBUG 0
#endif
#ifndef LL_RT_SUN_TMIN
#define LL_RT_SUN_TMIN 0.0
#endif
// LL_VK_RT_SUN_SOFTALPHA (metres, default 12, 0 = off): up to this view distance the sun ray lets each masked or blended card (hair) through by the fraction its blurred alpha leaves (rtShadowRaySoft) instead of
// the yes/no alpha test of rtShadowRay: a lock of hair then casts a continuous, partly transparent shadow, not one that is a full shadow or nothing per pixel (gaps, stair steps along the cards)
#ifndef LL_RT_SUN_SOFTALPHA
#define LL_RT_SUN_SOFTALPHA 12.0
#endif
float rtSunRayAt(vec3 origin, vec3 dir, vec2 pos_screen, float lod, float view_dist, float strength)
{
    if (view_dist < LL_RT_SUN_SOFTALPHA)
    {
        return rtShadowRaySoftS(origin, dir, 2000.0, lod, strength);
    }
    return rtSunRay(origin, dir, pos_screen, lod);
}

// LL_VK_RT_SUN_SELFRECV (metres, default 0.02, 0 = off): the translucent self shadow is for an avatar surface shadowing the SAME avatar (hair on the face, the chin on the chest). The ground at the feet is
// within SELFNEAR of the legs too, and its shadow faded out toward the feet (the user's snapshot, 2026-10-28 01:31). The G-buffer does not say what a pixel is, the ray scene does: a probe through the
// receiver's own surface (from this far under it to this far over it, along its normal) into the avatar structures alone (mask 0x0C) hits when the receiver is avatar geometry. Only a pixel whose first
// ray met a close avatar surface pays the probe; a receiver that is not an avatar traces that ray again at full strength. (Verified 02:13 with a stripes view: hits the whole avatar, hair included, nothing else.)
#ifndef LL_RT_SUN_SELF_RECV
#define LL_RT_SUN_SELF_RECV 0.02
#endif
bool rtReceiverIsAvatar(vec3 pos, vec3 norm)
{
    mat3 to_scene;
    vec3 camera;
    rtSceneFrame(to_scene, camera);
    const vec3 ray_origin = to_scene * (pos - norm * LL_RT_SUN_SELF_RECV) + camera;
    const vec3 ray_dir = to_scene * norm;
    if (!rtRayOk(ray_origin, ray_dir))
    {
        return false;
    }
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, ll_rt_tlas, gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT, 0x0C, ray_origin, 0.0, ray_dir, 2.0 * LL_RT_SUN_SELF_RECV);
    uint steps = 0u;
    while (rayQueryProceedEXT(rq))
    {
        if (++steps > 16u)
        {
            rayQueryTerminateEXT(rq);
            break;
        }
    }
    return rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT;
}

// LL_VK_RT_SUN_SSS (metres, default 0.004, 0 = off; the user chose 4 mm by eye, 2026-10-28): each ray of a pixel starts at a random point of a disc of this radius on the receiver's surface (the tangent plane), not at the pixel itself. Skin scatters
// the light it receives a few millimetres sideways, so the shadow a face shows is the shadow averaged over that much of its surface: a soft edge on BOTH sides of the caster's silhouette. A wider sun disc
// (LL_RT_SUN_PENUMBRA) could only widen the rays of a pixel whose first ray had already met a caster, so the outside of every edge stayed crisp. A few millimetres on the ground is nothing next to the
// sun's own penumbra, so it applies to every receiver. No extra rays; the accumulation smooths the noise of the random start.
#ifndef LL_RT_SUN_SSS
#define LL_RT_SUN_SSS 0.004
#endif

float rtSunShadowCore(vec3 pos, vec3 norm, vec3 light_dir, vec2 pos_screen)
{
#ifndef LL_RT_SUN_NOFACE
    if (dot(norm, light_dir) <= 0.0)
    {
        return 0.0;                         // facing away from the light
    }
#endif
    if (-pos.z > shadow_clip.w)
    {
        return 1.0;                         // beyond the shadow range: lit, as with the cascades
    }
    if (rt_sun_maxdist > 0.0 && -pos.z > rt_sun_maxdist)
    {
        return 1.0;                         // <FS> a blended surface this far away: the shadow ray is not worth its cost (a few pixels of hair, glass or foliage), the opaque ground behind it keeps its shadow
    }
    vec3 origin = pos + norm * ((rt_sun_alpha > 0.5 ? LL_RT_SUN_BIAS_ALPHA : LL_RT_SUN_BIAS) + 0.0006 * -pos.z) + light_dir * LL_RT_SUN_TMIN;
    float lod = clamp(0.5 * log2(max(-pos.z, 1.0)) - 1.0, 0.0, 4.0);
    int samples = (rt_sun_radius > 0.0) ? clamp(rt_sun_samples, 1, 16) : 1;
    if (rt_sun_lod.x > 0.0 && samples > 4)
    {
        // <FS> far from the camera the penumbra is a fraction of a pixel and the temporal upscaler averages the noise: fewer rays (the alpha pass traces for every blended pixel, in a crowd that is hair, clothes and
        // glass of dozens of avatars and cost over 5 ms of GPU with 12 rays)
        float k = smoothstep(rt_sun_lod.x, max(rt_sun_lod.y, rt_sun_lod.x + 1.0), -pos.z);
        samples = clamp(int(mix(float(samples), float(max(samples / 4, 2)), k) + 0.5), 1, 16);
    }
    // a per pixel rotation (interleaved gradient noise, shifted by the frame when it changes every frame), used by the sun's disc and by the start disc on the receiver
    vec2 px = gl_FragCoord.xy + vec2(5.588238, 5.588238) * rt_noise_frame;
    float rot = fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715)))) * 6.2831853;
    // a frame of tangents on the receiver for the start disc (LL_RT_SUN_SSS)
    vec3 b1 = normalize(cross(norm, abs(norm.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
    vec3 b2 = cross(norm, b1);
    const bool self_on = rt_sun_self_off < 0.5 || rt_sun_self_off > 1.5;      // 1 = off; 2 = on with the full hair walk (the early exit of rtShadowRaySoftS off)
    float strength = self_on ? LL_RT_SUN_ALPHA_STRENGTH : 1.0;  // this pixel's: 1 (avatar surfaces block fully) once the probe says the receiver is not an avatar
    if (rt_sun_alpha < 0.5)
    {
        // <FS> every pass but the alpha ones (the sun lightmap pass above all; PLAN-rt-shadow-penumbra.md): ONE ray at the disc's centre, the same answer every frame for a still scene, so there is
        // nothing to average over frames (the history, the noise motion and the blur of the sun channel are gone, 2026-11-05). The soft walk through masked and blended cards stays (its coverage
        // products are deterministic too), as do the translucency and the receiver probe of the avatar self shadow. The penumbra is the filter's job, sized by this ray's blocker distance
        // (rt_sun_blocker_t -> the lightmap's second attachment). The alpha passes (hair, glass) keep the disc rays below until they get a treatment of their own (PLAN-rt-hair-shadows.md).
        // The translucency floor stands in for the wide rays of the cone (hair over the scalp and the skin: the old wide rays escaped the hair shell sideways and lit the layer under it)
        if (self_on && rt_sun_strength > 0.0)
        {
            strength = min(strength, rt_sun_strength);
        }
        float one = rtSunRayAt(origin, light_dir, pos_screen, lod, -pos.z, strength);
        if (strength < 1.0 && rt_near_skin_t > 0.0 && LL_RT_SUN_SELF_RECV > 0.0 && !rtReceiverIsAvatar(pos, norm))
        {
            one = rtSunRayAt(origin, light_dir, pos_screen, lod, -pos.z, 1.0);
        }
        float t_b = rt_blocker_t;
        if (t_b > 0.0 && rt_sun_radius > 0.0)
        {
            // the two receiver-side softenings of the avatar self shadow, as the blocker distance that gives the filter the same width (the filter's half-width is t x rt_sun_radius):
            // the avatar cone (LL_RT_SUN_PENUMBRA: a close skinned blocker at skin_t gives a penumbra half-width of 8 mm on the skin, at most 0.35 skin_t, where the other rays of the pixel used to go to a
            // wider disc) and the 4 mm start disc (LL_RT_SUN_SSS: skin scatters the light it receives a few millimetres sideways, a floor on the width of every blocked receiver when the self shadow work is on)
            if (rt_near_skin_t > 0.0 && LL_RT_SUN_PENUMBRA > 0.0)
            {
                t_b = max(t_b, min(LL_RT_SUN_PENUMBRA, 0.35 * rt_near_skin_t) / rt_sun_radius);
            }
            if (LL_RT_SUN_SSS > 0.0 && self_on)
            {
                t_b = max(t_b, LL_RT_SUN_SSS / rt_sun_radius);
            }
        }
        rt_sun_blocker_t = t_b;
        return one;
    }
    if (samples == 1 && rt_sun_radius <= 0.0)
    {
        vec3 o = origin;
        if (LL_RT_SUN_SSS > 0.0 && self_on)
        {
            o += (b1 * cos(rot) + b2 * sin(rot)) * (LL_RT_SUN_SSS * 0.7);
        }
        float one = rtSunRayAt(o, light_dir, pos_screen, lod, -pos.z, strength);
        if (strength < 1.0 && rt_near_skin_t > 0.0 && LL_RT_SUN_SELF_RECV > 0.0 && !rtReceiverIsAvatar(pos, norm))
        {
            one = rtSunRayAt(o, light_dir, pos_screen, lod, -pos.z, 1.0);
        }
        rt_sun_blocker_t = rt_blocker_t;
        return one;
    }
    // a frame of tangents for the sun's disc and a golden angle spiral
    vec3 t1 = normalize(cross(light_dir, abs(light_dir.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
    vec3 t2 = cross(light_dir, t1);
    float lit = 0.0;
    float lit0 = 0.0;                           // the first ray's answer (the pixel's own, on the sun's disc)
    bool pen_mode = false;                      // the first ray met a close skinned surface: the other rays go to the wide disc and, when fewer than LL_RT_SUN_PENUMBRA_RAYS, stand for the full set (below)
    float skin_t = 0.0;                         // the distance of the close avatar surface the first ray met (0 = none): the other rays of the pixel get a wider disc (LL_RT_SUN_PENUMBRA)
    for (int i = 0; i < samples; ++i)
    {
        float radius = rt_sun_radius;
        int ri = i;                             // the ray's place in the set it stands for
        int den = samples;
        if (i > 0 && skin_t > 0.0 && LL_RT_SUN_PENUMBRA > 0.0)
        {
            radius = clamp(LL_RT_SUN_PENUMBRA / skin_t, rt_sun_radius, 0.35);
            if (samples < LL_RT_SUN_PENUMBRA_RAYS)
            {
                // <FS> fewer wide rays than the reference set (rt_sun_pen_rays, 2 with the upscaler): each one stands for one of the reference set's wide rays, a different one every frame (the noise of the pixel's
                // rotation picks it), so the frames the upscaler averages cover the same disc as the full set did; the weights are below
                den = LL_RT_SUN_PENUMBRA_RAYS;
                ri = min(1 + int(fract(rot * 1.8621 + float(i) * 0.381966) * float(LL_RT_SUN_PENUMBRA_RAYS - 1)), LL_RT_SUN_PENUMBRA_RAYS - 1);
            }
        }
        float r = sqrt((float(ri) + 0.5) / float(den)) * radius;
        float phi = rot + float(ri) * 2.3999632;
        if (samples == 1 && i == 0 && rt_sun_radius > 0.0)
        {
            // <FS> a lone ray (sunsamples 1, the alpha passes): its radius used to be fixed at 0.7 of the sun's disc and only its angle moved, so the frames the upscaler averages covered a ring of the sun, not the disc
            // (a harder, banded penumbra). A point of the whole disc now, from a two dimensional low discrepancy sequence (the R2 sequence, a step of it per frame) added to a fixed per pixel offset: the same one ray,
            // spread evenly over the disc by the frames the upscaler accumulates. rt_noise_frame is 0 without the upscaler: a fixed pattern per pixel, as before.
            vec2 fc = gl_FragCoord.xy;
            float h1 = fract(52.9829189 * fract(dot(fc, vec2(0.06711056, 0.00583715))));
            float h2 = fract(52.9829189 * fract(dot(fc.yx + vec2(17.0, 31.0), vec2(0.06711056, 0.00583715))));
            vec2 u = fract(vec2(h1, h2) + rt_noise_frame * vec2(0.7548776662, 0.5698402910));
            r = sqrt(u.x) * radius;
            phi = 6.2831853 * u.y;
        }
        vec3 dir = normalize(light_dir + (t1 * cos(phi) + t2 * sin(phi)) * r);
        vec3 o = origin;
        if (LL_RT_SUN_SSS > 0.0 && self_on)
        {
            // the start disc: the same spiral as the sun's disc, turned so the two do not line up
            float rs = sqrt((float(ri) + 0.5) / float(den)) * LL_RT_SUN_SSS;
            float ps = rot + 1.0 + float(ri) * 2.3999632;
            o += (b1 * cos(ps) + b2 * sin(ps)) * rs;
        }
        float one = rtSunRayAt(o, dir, pos_screen, lod, -pos.z, strength);
        if (i == 0)
        {
            if (strength < 1.0 && rt_near_skin_t > 0.0 && LL_RT_SUN_SELF_RECV > 0.0 && !rtReceiverIsAvatar(pos, norm))
            {
                strength = 1.0;
                one = rtSunRayAt(o, dir, pos_screen, lod, -pos.z, strength);
            }
            skin_t = rt_near_skin_t;
            lit0 = one;
            rt_sun_blocker_t = rt_blocker_t;        // the centre ray's blocker (the first ray of the pixel is the one on the disc's centre: ri 0 of the spiral, or the R2 point of a lone ray)
            if (skin_t > 0.0 && LL_RT_SUN_PENUMBRA > 0.0)
            {
                pen_mode = true;
                samples = clamp(max(samples, rt_sun_pen_rays > 0 ? rt_sun_pen_rays : LL_RT_SUN_PENUMBRA_RAYS), 1, 16);
            }
        }
        lit += one;
    }
    if (pen_mode && samples > 1 && samples < LL_RT_SUN_PENUMBRA_RAYS)
    {
        // the first ray keeps its 1/8 of the answer and the wide rays share the other 7/8: the same average as the full set of rays, with the noise of fewer
        return (lit0 + float(LL_RT_SUN_PENUMBRA_RAYS - 1) * (lit - lit0) / float(samples - 1)) / float(LL_RT_SUN_PENUMBRA_RAYS);
    }
    return lit / float(samples);
}

float rtSunShadow(vec3 pos, vec3 norm, vec3 light_dir, vec2 pos_screen)
{
    float lit = rtSunShadowCore(pos, norm, light_dir, pos_screen);
    if (rt_sun_alpha > 0.5)
    {
        lit = mix(lit, 1.0, LL_RT_SUN_HAIRFILL);        // <FS> a blended surface (hair): bounced light and scattering fill part of the shadows it has on itself
    }
    return lit;
}
#endif

#if defined(SUN_SHADOW) && defined(LL_RT_SHADOWS)
float sampleCascadeShadow(vec3 pos, vec3 norm, vec2 pos_screen)
#else
float sampleDirectionalShadow(vec3 pos, vec3 norm, vec2 pos_screen)
#endif
{
#if defined(SUN_SHADOW)
    float shadow = 0.0f;
    vec3 light_dir = normalize((sun_up_factor == 1) ? sun_dir : moon_dir);

    float dp_directional_light = max(0.0, dot(norm.xyz, light_dir));
          dp_directional_light = clamp(dp_directional_light, 0.0, 1.0);

    vec3 shadow_pos = pos.xyz;

    vec3 offset = light_dir.xyz * (1.0 - dp_directional_light);

    shadow_pos += offset * shadow_offset * 2.0;

    vec4 spos = vec4(shadow_pos.xyz, 1.0);

    if (spos.z > -shadow_clip.w)
    {
        vec4 lpos;
        vec4 near_split = shadow_clip*-0.75;
        vec4 far_split = shadow_clip*-1.25;
        vec4 transition_domain = near_split-far_split;
        float weight = 0.0;

        if (spos.z < near_split.z)
        {
            lpos = shadow_matrix[3]*spos;

            float w = 1.0;
            w -= max(spos.z-far_split.z, 0.0)/transition_domain.z;
            //w = clamp(w, 0.0, 1.0);
            float contrib = pcfShadow(shadowMap3, norm, lpos, 1.0, pos_screen, light_dir)*w;
            //if (contrib > 0)
            {
                shadow += contrib;
                weight += w;
            }
            shadow += max((pos.z+shadow_clip.z)/(shadow_clip.z-shadow_clip.w)*2.0-1.0, 0.0);
        }

        if (spos.z < near_split.y && spos.z > far_split.z)
        {
            lpos = shadow_matrix[2]*spos;

            float w = 1.0;
            w -= max(spos.z-far_split.y, 0.0)/transition_domain.y;
            w -= max(near_split.z-spos.z, 0.0)/transition_domain.z;
            //w = clamp(w, 0.0, 1.0);
            float contrib = pcfShadow(shadowMap2, norm, lpos, 1.0, pos_screen, light_dir)*w;
            //if (contrib > 0)
            {
                shadow += contrib;
                weight += w;
            }
        }

        if (spos.z < near_split.x && spos.z > far_split.y)
        {
            lpos = shadow_matrix[1]*spos;

            float w = 1.0;
            w -= max(spos.z-far_split.x, 0.0)/transition_domain.x;
            w -= max(near_split.y-spos.z, 0.0)/transition_domain.y;
            //w = clamp(w, 0.0, 1.0);
            float contrib = pcfShadow(shadowMap1, norm, lpos, 1.0, pos_screen, light_dir)*w;
            //if (contrib > 0)
            {
                shadow += contrib;
                weight += w;
            }
        }

        if (spos.z > far_split.x)
        {
            lpos = shadow_matrix[0]*spos;

            float w = 1.0;
            w -= max(near_split.x-spos.z, 0.0)/transition_domain.x;
            //w = clamp(w, 0.0, 1.0);
            float contrib = pcfShadow(shadowMap0, norm, lpos, 1.0, pos_screen, light_dir)*w;
            //if (contrib > 0)
            {
                shadow += contrib;
                weight += w;
            }
        }

        shadow /= weight;
    }
    else
    {
        return 1.0f; // lit beyond the far split...
    }
    //shadow = min(dp_directional_light,shadow);
    return shadow;
#else
    return 1.0;
#endif
}

#if defined(SUN_SHADOW) && defined(LL_RT_SHADOWS)
float sampleDirectionalShadow(vec3 pos, vec3 norm, vec2 pos_screen)
{
    vec3 light_dir = normalize((sun_up_factor == 1) ? sun_dir : moon_dir);
    float rt = rtSunShadow(pos, norm, light_dir, pos_screen);
#if LL_RT_SHADOWS == 1
    return rt;
#elif LL_RT_SHADOWS == 5
    // debug: rays only, with the sun swung sideways so that shadows fall far from their casters (a high sun puts them under the feet and hides what is in the scene)
    return rtSunShadow(pos, norm, normalize(light_dir + vec3(1.2, 0.0, 0.0)), pos_screen);
#elif LL_RT_SHADOWS == 4
    // x-ray of the skinned avatar structures (instance mask 4): a ray from the eye through this pixel; lit where the ray scene has a surface at the depth of the visible one,
    // dark elsewhere. Shows what of the avatars the ray scene really covers, in their current pose.
    float depth = length(pos);
    mat3 x_to_scene;
    vec3 x_camera;
    rtSceneFrame(x_to_scene, x_camera);
    rayQueryEXT xq;
    rayQueryInitializeEXT(xq, ll_rt_tlas, gl_RayFlagsOpaqueEXT, 0x04, x_camera, 0.01, x_to_scene * (pos / depth), depth + 3.0);
    uint x_steps = 0u;
    while (rayQueryProceedEXT(xq))
    {
        if (++x_steps > 96u)
        {
            rayQueryTerminateEXT(xq);
            break;
        }
    }
    if (rayQueryGetIntersectionTypeEXT(xq, true) == gl_RayQueryCommittedIntersectionNoneEXT)
    {
        return 0.0;
    }
    return abs(rayQueryGetIntersectionTEXT(xq, true) - depth) < max(0.04, 0.012 * depth) ? 1.0 : 0.0;
#elif LL_RT_SHADOWS == 2
    // split screen: the cascades on the left half, the rays on the right
    if (abs(pos_screen.x * 2.0 - screen_res.x) < 4.0)
    {
        return 0.0;                         // a dark line where the halves meet
    }
    return (pos_screen.x * 2.0 < screen_res.x) ? sampleCascadeShadow(pos, norm, pos_screen) : rt;
#else
    // disagreement map: white where the two do not agree about lit and shadowed, black elsewhere
    float cascade = sampleCascadeShadow(pos, norm, pos_screen);
    return abs(cascade - rt) > 0.5 ? 1.0 : 0.0;
#endif
}
#endif

float sampleSpotShadow(vec3 pos, vec3 norm, int index, vec2 pos_screen)
{
#if defined(SPOT_SHADOW)
    float shadow = 0.0f;
    pos += norm * spot_shadow_offset;

    vec4 spos = vec4(pos,1.0);
    if (spos.z > -shadow_clip.w)
    {
        vec4 lpos;

        vec4 near_split = shadow_clip*-0.75;
        vec4 far_split = shadow_clip*-1.25;
        vec4 transition_domain = near_split-far_split;
        float weight = 0.0;

        {
            float w = 1.0;
            w -= max(spos.z-far_split.z, 0.0)/transition_domain.z;

            if (index == 0)
            {
                lpos = shadow_matrix[4]*spos;
            }
            else
            {
                lpos = shadow_matrix[5]*spos;
            }
            // <FS> only the pixels inside the projector's frustum read this value (spotLightF.glsl discards the rest before it looks at the lightmap): outside the shadow map there is nothing to look up
            {
                vec2 stc_xy = lpos.xy / lpos.w;
                if (lpos.w <= 0.0 || stc_xy.x < -0.05 || stc_xy.x > 1.05 || stc_xy.y < -0.05 || stc_xy.y > 1.05)
                {
                    return 1.0;
                }
            }
            if (index == 0)
            {
                shadow += pcfSpotShadow(shadowMap4, lpos, 0.8, spos.xy)*w;
            }
            else
            {
                shadow += pcfSpotShadow(shadowMap5, lpos, 0.8, spos.xy)*w;
            }
            weight += w;
            shadow += max((pos.z+shadow_clip.z)/(shadow_clip.z-shadow_clip.w)*2.0-1.0, 0.0);
        }

        shadow /= weight;
    }
    else
    {
        shadow = 1.0f;
    }
    return shadow;
#else
    return 1.0;
#endif
}



// ======== class3/deferred/reflectionProbeF.glsl ========
#define FLT_MAX 3.402823466e+38

#if defined(SSR)
float tapScreenSpaceReflection(int totalSamples, vec2 tc, vec3 viewPos, vec3 n, inout vec4 collectedColor, sampler2D source, float glossiness, vec3 rejectUp);
#endif





#define MAX_REFMAP_COUNT 256  // must match LL_MAX_REFLECTION_PROBE_COUNT

layout(std140, set = 0, binding = 1) uniform ReflectionProbes
{
    // list of OBBs for user override probes
    // box is a set of 3 planes outward facing planes and the depth of the box along that plane
    // for each box refBox[i]...
    /// box[0..2] - plane 0 .. 2 in [A,B,C,D] notation
    //  box[3][0..2] - plane thickness
    mat4 refBox[MAX_REFMAP_COUNT];
    mat4 heroBox;
    // list of bounding spheres for reflection probes sorted by distance to camera (closest first)
    vec4 refSphere[MAX_REFMAP_COUNT];
    // extra parameters
    //  x - irradiance scale
    //  y - radiance scale
    //  z - fade in
    //  w - znear
    vec4 refParams[MAX_REFMAP_COUNT];
    vec4 heroSphere;
    // index  of cube map in reflectionProbes for a corresponding reflection probe
    // e.g. cube map channel of refSphere[2] is stored in refIndex[2]
    // refIndex.x - cubemap channel in reflectionProbes
    // refIndex.y - index in refNeighbor of neighbor list (index is ivec4 index, not int index)
    // refIndex.z - number of neighbors
    // refIndex.w - priority, if negative, this probe has a box influence
    ivec4 refIndex[MAX_REFMAP_COUNT];

    // neighbor list data (refSphere indices, not cubemap array layer)
    ivec4 refNeighbor[1024];

    ivec4 refBucket[256];

    // number of reflection probes present in refSphere
    int refmapCount;

    int heroShape;
    int heroMipCount;
    int heroProbeCount;
};

// Inputs

// list of probeIndexes shader will actually use after "getRefIndex" is called
// (stores refIndex/refSphere indices, NOT rerflectionProbes layer)
int probeIndex[REF_SAMPLE_COUNT];

// number of probes stored in probeIndex
int probeInfluences = 0;

bool isAbove(vec3 pos, vec4 plane)
{
    return (dot(plane.xyz, pos) + plane.w) > 0;
}

bool sample_automatic = true;

// preProbeSample appends to probeIndex (it is meant to be called once for an invocation) and sample_automatic is changed by what it finds. A SECOND position in the same invocation (a hit of a traced
// reflection, softenLightF.glsl rtLitHitColor) needs an empty list, and the pixel's own list has to come back afterwards: without this the second call wrote past the end of probeIndex
// and the weights and the cost of every probe lookup grew with the number of hits lit.
int probeSavedCount = 0;
int probeSavedIndex[REF_SAMPLE_COUNT];
bool probeSavedAuto = true;
void probeListSave()
{
    probeSavedCount = probeInfluences;
    for (int i = 0; i < REF_SAMPLE_COUNT; ++i)
    {
        probeSavedIndex[i] = probeIndex[i];
    }
    probeSavedAuto = sample_automatic;
}
void probeListReset()
{
    probeInfluences = 0;
    sample_automatic = true;
}
void probeListRestore()
{
    probeInfluences = probeSavedCount;
    for (int i = 0; i < REF_SAMPLE_COUNT; ++i)
    {
        probeIndex[i] = probeSavedIndex[i];
    }
    sample_automatic = probeSavedAuto;
}


// return true if probe at index i influences position pos
bool shouldSampleProbe(int i, vec3 pos)
{
    if (refIndex[i].w < 0)
    {
        vec4 v = refBox[i] * vec4(pos, 1.0);
        if (abs(v.x) > 1 ||
            abs(v.y) > 1 ||
            abs(v.z) > 1)
        {
            return false;
        }

        // never allow automatic probes to encroach on box probes
        sample_automatic = false;
    }
    else
    {
        if (refIndex[i].w == 0 && !sample_automatic)
        {
            return false;
        }

        vec3 delta = pos.xyz - refSphere[i].xyz;
        float d = dot(delta, delta);
        float r2 = refSphere[i].w;
        r2 *= r2;

        if (d > r2)
        { // outside bounding sphere
            return false;
        }
    }

    return true;
}

int getStartIndex(vec3 pos)
{
#if 1
    int idx = clamp(int(floor(-pos.z)), 0, 255);
    return clamp(refBucket[idx].x, 1, refmapCount+1);
#else
    return 1;
#endif
}

// <FS> one past the last probe that can reach the depth bucket of pos (refBucket[].y, made in LLReflectionMapManager::updateUniforms from the sphere the loop below tests): the loop used to test every probe
// from the start to the end of the whole list, and a pixel 10 m away rejected the ones that begin 50 m out one by one. The same probes are found, in the same order.
int getEndIndex(vec3 pos)
{
    int idx = clamp(int(floor(-pos.z)), 0, 255);
    return min(refBucket[idx].y, refmapCount);
}

// call before sampleRef
// populate "probeIndex" with N probe indices that influence pos where N is REF_SAMPLE_COUNT
void preProbeSample(vec3 pos)
{
#if REFMAP_LEVEL > 0

    int start = getStartIndex(pos);
    int end = getEndIndex(pos);

    // TODO: make some sort of structure that reduces the number of distance checks
    for (int i = start; i < end; ++i)
    {
        // found an influencing probe
        if (shouldSampleProbe(i, pos))
        {
            probeIndex[probeInfluences] = i;
            ++probeInfluences;

            int neighborIdx = refIndex[i].y;
            if (neighborIdx != -1)
            {
                int neighborCount = refIndex[i].z;

                int count = 0;
                while (count < neighborCount)
                {
                    // check up to REF_SAMPLE_COUNT-1 neighbors (neighborIdx is ivec4 index)

                    // sample refNeighbor[neighborIdx].x
                    int idx = refNeighbor[neighborIdx].x;
                    if (shouldSampleProbe(idx, pos))
                    {
                        probeIndex[probeInfluences++] = idx;
                        if (probeInfluences == REF_SAMPLE_COUNT)
                        {
                            break;
                        }
                    }
                    count++;
                    if (count == neighborCount)
                    {
                        break;
                    }

                    // sample refNeighbor[neighborIdx].y
                    idx = refNeighbor[neighborIdx].y;
                    if (shouldSampleProbe(idx, pos))
                    {
                        probeIndex[probeInfluences++] = idx;
                        if (probeInfluences == REF_SAMPLE_COUNT)
                        {
                            break;
                        }
                    }
                    count++;
                    if (count == neighborCount)
                    {
                        break;
                    }

                    // sample refNeighbor[neighborIdx].z
                    idx = refNeighbor[neighborIdx].z;
                    if (shouldSampleProbe(idx, pos))
                    {
                        probeIndex[probeInfluences++] = idx;
                        if (probeInfluences == REF_SAMPLE_COUNT)
                        {
                            break;
                        }
                    }
                    count++;
                    if (count == neighborCount)
                    {
                        break;
                    }

                    // sample refNeighbor[neighborIdx].w
                    idx = refNeighbor[neighborIdx].w;
                    if (shouldSampleProbe(idx, pos))
                    {
                        probeIndex[probeInfluences++] = idx;
                        if (probeInfluences == REF_SAMPLE_COUNT)
                        {
                            break;
                        }
                    }
                    count++;

                    ++neighborIdx;
                }

                break;
            }
        }
    }

    if (sample_automatic)
    { // probe at index 0 is a special probe for smoothing out automatic probes
        probeIndex[probeInfluences++] = 0;
    }
#else
    probeIndex[probeInfluences++] = 0;
#endif
}

// from https://www.scratchapixel.com/lessons/3d-basic-rendering/minimal-ray-tracer-rendering-simple-shapes/ray-sphere-intersection

// original reference implementation:
/*
bool intersect(const Ray &ray) const
{
        float t0, t1; // solutions for t if the ray intersects
#if 0
        // geometric solution
        Vec3f L = center - orig;
        float tca = L.dotProduct(dir);
        // if (tca < 0) return false;
        float d2 = L.dotProduct(L) - tca * tca;
        if (d2 > radius2) return false;
        float thc = sqrt(radius2 - d2);
        t0 = tca - thc;
        t1 = tca + thc;
#else
        // analytic solution
        Vec3f L = orig - center;
        float a = dir.dotProduct(dir);
        float b = 2 * dir.dotProduct(L);
        float c = L.dotProduct(L) - radius2;
        if (!solveQuadratic(a, b, c, t0, t1)) return false;
#endif
        if (t0 > t1) std::swap(t0, t1);

        if (t0 < 0) {
            t0 = t1; // if t0 is negative, let's use t1 instead
            if (t0 < 0) return false; // both t0 and t1 are negative
        }

        t = t0;

        return true;
} */

// adapted -- assume that origin is inside sphere, return intersection of ray with edge of sphere
vec3 sphereIntersect(vec3 origin, vec3 dir, vec3 center, float radius2)
{
        float t0, t1; // solutions for t if the ray intersects

        vec3 L = center - origin;
        float tca = dot(L,dir);

        float d2 = dot(L,L) - tca * tca;

        float thc = sqrt(radius2 - d2);
        t0 = tca - thc;
        t1 = tca + thc;

        vec3 v = origin + dir * t1;
        return v;
}

void swap(inout float a, inout float b)
{
    float t = a;
    a = b;
    b = a;
}

// debug implementation, make no assumptions about origin
void sphereIntersectDebug(vec3 origin, vec3 dir, vec3 center, float radius2, float depth, inout vec4 col)
{
    float t[2]; // solutions for t if the ray intersects

    // geometric solution
    vec3 L = center - origin;
    float tca = dot(L, dir);
    // if (tca < 0) return false;
    float d2 = dot(L, L) - tca * tca;
    if (d2 > radius2) return;
    float thc = sqrt(radius2 - d2);
    t[0] = tca - thc;
    t[1] = tca + thc;

    for (int i = 0; i < 2; ++i)
    {
        if (t[i] > 0)
        {
            if (t[i] > depth)
            {
                float w = 0.125/((t[i]-depth)*0.125 + 1.0);
                col += vec4(0, 0, w, w)*(1.0-min(col.a, 1.0));
            }
            else
            {
                float w = 0.25;
                col += vec4(w,w,0,w)*(1.0-min(col.a, 1.0));
            }
        }
    }

}

// from https://seblagarde.wordpress.com/2012/09/29/image-based-lighting-approaches-and-parallax-corrected-cubemap/
/*
vec3 DirectionWS = normalize(PositionWS - CameraWS);
vec3 ReflDirectionWS = reflect(DirectionWS, NormalWS);

// Intersection with OBB convertto unit box space
// Transform in local unit parallax cube space (scaled and rotated)
vec3 RayLS = MulMatrix( float(3x3)WorldToLocal, ReflDirectionWS);
vec3 PositionLS = MulMatrix( WorldToLocal, PositionWS);

vec3 Unitary = vec3(1.0f, 1.0f, 1.0f);
vec3 FirstPlaneIntersect  = (Unitary - PositionLS) / RayLS;
vec3 SecondPlaneIntersect = (-Unitary - PositionLS) / RayLS;
vec3 FurthestPlane = max(FirstPlaneIntersect, SecondPlaneIntersect);
float Distance = min(FurthestPlane.x, min(FurthestPlane.y, FurthestPlane.z));

// Use Distance in WS directly to recover intersection
vec3 IntersectPositionWS = PositionWS + ReflDirectionWS * Distance;
vec3 ReflDirectionWS = IntersectPositionWS - CubemapPositionWS;

return texCUBE(envMap, ReflDirectionWS);
*/

// get point of intersection with given probe's box influence volume
// origin - ray origin in clip space
// dir - ray direction in clip space
// i - probe index in refBox/refSphere
// d - distance to nearest wall in clip space
// scale - scale of box, default 1.0
vec3 boxIntersect(vec3 origin, vec3 dir, mat4 i, out float d, float scale)
{
    // Intersection with OBB convert to unit box space
    // Transform in local unit parallax cube space (scaled and rotated)
    mat4 clipToLocal = i;

    vec3 RayLS = mat3(clipToLocal) * dir;
    vec3 PositionLS = (clipToLocal * vec4(origin, 1.0)).xyz;

    d = 1.0-max(max(abs(PositionLS.x), abs(PositionLS.y)), abs(PositionLS.z));

    vec3 Unitary = vec3(scale);
    vec3 FirstPlaneIntersect  = (Unitary - PositionLS) / RayLS;
    vec3 SecondPlaneIntersect = (-Unitary - PositionLS) / RayLS;
    vec3 FurthestPlane = max(FirstPlaneIntersect, SecondPlaneIntersect);
    float Distance = min(FurthestPlane.x, min(FurthestPlane.y, FurthestPlane.z));

    // Use Distance in CS directly to recover intersection
    vec3 IntersectPositionCS = origin + dir * Distance;

    return IntersectPositionCS;
}

vec3 boxIntersect(vec3 origin, vec3 dir, mat4 i, out float d)
{
    return boxIntersect(origin, dir, i, d, 1.0);
}

void debugBoxCol(vec3 ro, vec3 rd, float t, vec3 p, inout vec4 col)
{
    vec3 v = ro + rd * t;

    v -= ro;
    vec3 pos = p - ro;


    bool behind = dot(v,v) > dot(pos,pos);

    float w = 0.25;

    if (behind)
    {
        w *= 0.5;
        w /= (length(v)-length(pos))*0.5+1.0;
        col += vec4(0,0,w,w)*(1.0-min(col.a, 1.0));
    }
    else
    {
        col += vec4(w,w,0,w)*(1.0-min(col.a, 1.0));
    }
}

// cribbed from https://iquilezles.org/articles/intersectors/
// axis aligned box centered at the origin, with size boxSize
void boxIntersectionDebug( in vec3 ro, in vec3 p, vec3 boxSize, inout vec4 col)
{
    vec3 rd = normalize(p-ro);

    vec3 m = 1.0/rd; // can precompute if traversing a set of aligned boxes
    vec3 n = m*ro;   // can precompute if traversing a set of aligned boxes
    vec3 k = abs(m)*boxSize;
    vec3 t1 = -n - k;
    vec3 t2 = -n + k;
    float tN = max( max( t1.x, t1.y ), t1.z );
    float tF = min( min( t2.x, t2.y ), t2.z );
    if( tN>tF || tF<0.0) return ; // no intersection

    float t = tN < 0 ? tF : tN;

    debugBoxCol(ro, rd, t, p, col);

    if (tN > 0) // eye is outside box, check backside, too
    {
        debugBoxCol(ro, rd, tF, p, col);
    }
}


void boxIntersectDebug(vec3 origin, vec3 pos, mat4 i, inout vec4 col)
{
    mat4 clipToLocal = i;

    // transform into unit cube space
    origin = (clipToLocal * vec4(origin, 1.0)).xyz;
    pos = (clipToLocal * vec4(pos, 1.0)).xyz;

    boxIntersectionDebug(origin, pos, vec3(1), col);
}


// get the weight of a sphere probe
//  pos - position to be weighted
//  dir - normal to be weighted
//  origin - center of sphere probe
//  r - radius of probe influence volume
// i - index of probe in refSphere
// dw - distance weight
float sphereWeight(vec3 pos, vec3 dir, vec3 origin, float r, vec4 i, out float dw)
{
    float r1 = r * 0.5; // 50% of radius (outer sphere to start interpolating down)
    vec3 delta = pos.xyz - origin;
    float d2 = max(length(delta), 0.001);

    float atten = 1.0 - max(d2 - r1, 0.0) / max((r - r1), 0.001);
    float w = 1.0 / d2;

    w *= i.z;

    dw = w * atten * max(r, 1.0)*4;

    w *= atten;

    return w;
}

// Tap a reflection probe
// pos - position of pixel
// dir - pixel normal
//  w - weight of sample (distance and angular attenuation)
//  dw - weight of sample (distance only)
// lod - which mip to sample (lower is higher res, sharper reflections)
// c - center of probe
// r2 - radius of probe squared
// i - index of probe
vec3 tapRefMap(vec3 pos, vec3 dir, out float w, out float dw, float lod, vec3 c, int i)
{
    // parallax adjustment
    vec3 v;

    if (refIndex[i].w < 0)
    {  // box probe
        float d = 0;
        v = boxIntersect(pos, dir, refBox[i], d);

        w = max(d, 0.001);
    }
    else
    { // sphere probe
        float r = refSphere[i].w;

        float rr = r * r;

        v = sphereIntersect(pos, dir, c,
        refIndex[i].w < 1 ? 4096.0*4096.0 : // <== effectively disable parallax correction for automatically placed probes to keep from bombing the world with obvious spheres
                rr);

        w = sphereWeight(pos, dir, refSphere[i].xyz, r, refParams[i], dw);
    }

    v -= c;
    vec3 d = normalize(v);

    v = env_mat * v;

    vec4 ret = textureLod(reflectionProbes, vec4(v.xyz, refIndex[i].x), lod) * refParams[i].y;

    return ret.rgb;
}

// Tap an irradiance map
// pos - position of pixel
// dir - pixel normal
// w - weight of sample (distance and angular attenuation)
// dw - weight of sample (distance only)
// i - index of probe
vec3 tapIrradianceMap(vec3 pos, vec3 dir, out float w, out float dw, vec3 c, int i, vec3 amblit)
{
    // parallax adjustment
    vec3 v;
    if (refIndex[i].w < 0)
    {
        float d = 0.0;
        v = boxIntersect(pos, dir, refBox[i], d, 3.0);
        w = max(d, 0.001);
    }
    else
    {
        float r = refSphere[i].w; // radius of sphere volume

        // pad sphere for manual probe extending into automatic probe space
        float rr = r * r;

        v = sphereIntersect(pos, dir, c,
        refIndex[i].w < 1 ? 4096.0*4096.0 : // <== effectively disable parallax correction for automatically placed probes to keep from bombing the world with obvious spheres
                rr);

        w = sphereWeight(pos, dir, refSphere[i].xyz, r, refParams[i], dw);
    }

    v -= c;
    v = env_mat * v;

    vec3 col = textureLod(irradianceProbes, vec4(v.xyz, refIndex[i].x), 0).rgb * refParams[i].x;

    col = mix(amblit, col, min(refParams[i].x, 1.0));

    return col;
}

vec3 sampleProbes(vec3 pos, vec3 dir, float lod)
{
    float wsum[2];
    wsum[0] = 0;
    wsum[1] = 0;

    float dwsum[2];
    dwsum[0] = 0;
    dwsum[1] = 0;

    vec3 col[2];
    col[0] = vec3(0);
    col[1] = vec3(0);

    for (int idx = 0; idx < probeInfluences; ++idx)
    {
        int i = probeIndex[idx];
        int p = clamp(abs(refIndex[i].w), 0, 1);

        if (p == 0 && !sample_automatic)
        {
            continue;
        }

        float w = 0;
        float dw = 0;
        vec3 refcol;

        {
            refcol = tapRefMap(pos, dir, w, dw, lod, refSphere[i].xyz, i);

            col[p] += refcol.rgb*w;
            wsum[p] += w;
            dwsum[p] += dw;
        }
    }

    // mix automatic and manual probes
    if (sample_automatic && wsum[0] > 0.0)
    { // some automatic probes were sampled
        col[0] *= 1.0/wsum[0];
        if (wsum[1] > 0.0)
        { //some manual probes were sampled, mix between the two
            col[1] *= 1.0/wsum[1];
            col[1] = mix(col[0], col[1], min(dwsum[1], 1.0));
            col[0] = vec3(0);
        }
    }
    else if (wsum[1] > 0.0)
    {
        // manual probes were sampled but no automatic probes were
        col[1] *= 1.0/wsum[1];
        col[0] = vec3(0);
    }

    return col[1]+col[0];
}

vec3 sampleProbeAmbient(vec3 pos, vec3 dir, vec3 amblit)
{
    // modified copy/paste of sampleProbes follows, will likely diverge from sampleProbes further
    // as irradiance map mixing is tuned independently of radiance map mixing
    float wsum[2];
    wsum[0] = 0;
    wsum[1] = 0;

    float dwsum[2];
    dwsum[0] = 0;
    dwsum[1] = 0;

    vec3 col[2];
    col[0] = vec3(0);
    col[1] = vec3(0);

    for (int idx = 0; idx < probeInfluences; ++idx)
    {
        int i = probeIndex[idx];
        int p = clamp(abs(refIndex[i].w), 0, 1);

        if (p == 0 && !sample_automatic)
        {
            continue;
        }

        {
            float w = 0;
            float dw = 0;

            vec3 refcol = tapIrradianceMap(pos, dir, w, dw, refSphere[i].xyz, i, amblit);

            col[p] += refcol*w;
            wsum[p] += w;
            dwsum[p] += dw;
        }
    }

    // mix automatic and manual probes
    if (sample_automatic && wsum[0] > 0.0)
    { // some automatic probes were sampled
        col[0] *= 1.0/wsum[0];
        if (wsum[1] > 0.0)
        { //some manual probes were sampled, mix between the two
            col[1] *= 1.0/wsum[1];
            col[1] = mix(col[0], col[1], min(dwsum[1], 1.0));
            col[0] = vec3(0);
        }
    }
    else if (wsum[1] > 0.0)
    {
        // manual probes were sampled but no automatic probes were
        col[1] *= 1.0/wsum[1];
        col[0] = vec3(0);
    }

    return col[1]+col[0];
}

#if defined(HERO_PROBES)


void tapHeroProbe(inout vec3 glossenv, vec3 pos, vec3 norm, float glossiness)
{
    // <FS> below this the weight is 0 (see the mix on glossiness below): the box or sphere weight and the cube map lookup of the tap changed nothing, for every pixel of every lit pass (2026-11-04)
    if (glossiness <= 0.75)
    {
        return;
    }
    float clipDist = dot(pos.xyz, clipPlane.xyz) + clipPlane.w;
    float w = 0;
    float dw = 0;
    float falloffMult = 10;
    vec3 refnormpersp = reflect(pos.xyz, norm.xyz);
    if (heroShape < 1)
    {
        float d = 0;
        boxIntersect(pos, norm, heroBox, d, 1.0);

        w = max(d, 0);
    }
    else
    {
        float r = heroSphere.w;

        w = sphereWeight(pos, refnormpersp, heroSphere.xyz, r, vec4(1), dw);
    }

    clipDist = clipDist * 0.95 + 0.05;
    clipDist = clamp(clipDist * falloffMult, 0, 1);
    w = clamp(w * falloffMult * clipDist, 0, 1);
    w = mix(0, w, clamp(glossiness - 0.75, 0, 1) * 4); // We only generate a quarter of the mips for the hero probes.  Linearly interpolate between normal probes and hero probes based upon glossiness.
    if (heroMipCount < 0)
    {
        // <FS> the planar mirror: the reflected view was rendered from the camera mirrored across the plane with the same projection, so the glass pixel at screen position (u, v) shows what that view has at (1 - u, v)
        const vec2 view_size = vec2(textureSize(mirrorView, 0));
        glossenv = mix(glossenv, textureLod(mirrorView, vec2(1.0 - gl_FragCoord.x / view_size.x, gl_FragCoord.y / view_size.y), 0.0).xyz, w);
        return;
    }
    glossenv = mix(glossenv, textureLod(heroProbes, vec4(env_mat * refnormpersp, 0), (1.0-glossiness)*heroMipCount).xyz, w);
}

#else

void tapHeroProbe(inout vec3 glossenv, vec3 pos, vec3 norm, float glossiness)
{
}

#endif



// <FS> set by sampleReflectionProbesWater: the screen space reflection ignores hits on the water plane itself (see traceScreenRay)
bool probe_water_surface = false;

void doProbeSample(inout vec3 ambenv, inout vec3 glossenv,
        vec2 tc, vec3 pos, vec3 norm, float glossiness, bool transparent, vec3 amblit)
{
    // TODO - don't hard code lods
    float reflection_lods = max_probe_lod;

    vec3 refnormpersp = reflect(pos.xyz, norm.xyz);

    ambenv = amblit;

    if (classic_mode == 0)
        ambenv = sampleProbeAmbient(pos, norm, amblit);

    float lod = (1.0-glossiness)*reflection_lods;
    glossenv = sampleProbes(pos, normalize(refnormpersp), lod);

#if defined(SSR)
    if (cube_snapshot != 1 && glossiness >= 0.9)
    {
        vec4 ssr = vec4(0);
        vec3 rejectUp = probe_water_surface ? vec3(0, 0, 1) * env_mat : vec3(0);     // world up in view space
        if (transparent)
        {
            tapScreenSpaceReflection(1, tc, pos, norm, ssr, sceneMap, 1, rejectUp);
            ssr.a *= glossiness;
        }
        else
        {
            tapScreenSpaceReflection(1, tc, pos, norm, ssr, sceneMap, glossiness, rejectUp);
        }


        glossenv = mix(glossenv, ssr.rgb, ssr.a);
    }
#endif

    tapHeroProbe(glossenv, pos, norm, glossiness);
}

void sampleReflectionProbes(inout vec3 ambenv, inout vec3 glossenv,
        vec2 tc, vec3 pos, vec3 norm, float glossiness, bool transparent, vec3 amblit)
{
    preProbeSample(pos);
    doProbeSample(ambenv, glossenv, tc, pos, norm, glossiness, transparent, amblit);
}

void sampleReflectionProbesWater(inout vec3 ambenv, inout vec3 glossenv,
        vec2 tc, vec3 pos, vec3 norm, float glossiness, vec3 amblit)
{
    // don't sample automatic probes for water
    sample_automatic = false;
    preProbeSample(pos);
    sample_automatic = true;
    // always include void probe on water
    probeIndex[probeInfluences++] = 0;

    probe_water_surface = true;
    doProbeSample(ambenv, glossenv, tc, pos, norm, glossiness, false, amblit);
    probe_water_surface = false;
}

void debugTapRefMap(vec3 pos, vec3 dir, float depth, int i, inout vec4 col)
{
    vec3 origin = vec3(0,0,0);

    bool manual_probe = abs(refIndex[i].w) > 0;

    if (manual_probe)
    {
        if (refIndex[i].w < 0)
        {
            boxIntersectDebug(origin, pos, refBox[i], col);
        }
        else
        {
            float r = refSphere[i].w; // radius of sphere volume
            float rr = r * r; // radius squared

            float t = 0.0;

            sphereIntersectDebug(origin, dir, refSphere[i].xyz, rr, depth, col);
        }
    }
}

vec4 sampleReflectionProbesDebug(vec3 pos)
{
    vec4 col = vec4(0,0,0,0);

    vec3 dir = normalize(pos);

    float d = length(pos);

    for (int i = 1; i < refmapCount; ++i)
    {
        debugTapRefMap(pos, dir, d, i, col);
    }

#if 0 //debug getStartIndex
    col.g = float(getStartIndex(pos));

    col.g /= 255.0;
    col.rb = vec2(0);
    col.a = 1.0;
#endif

    return col;
}

void sampleReflectionProbesLegacy(inout vec3 ambenv, inout vec3 glossenv, inout vec3 legacyenv,
        vec2 tc, vec3 pos, vec3 norm, float glossiness, float envIntensity, bool transparent, vec3 amblit)
{
    float reflection_lods = max_probe_lod;
    preProbeSample(pos);

    vec3 refnormpersp = reflect(pos.xyz, norm.xyz);

    ambenv = amblit;

    if (classic_mode == 0)
        ambenv = sampleProbeAmbient(pos, norm, amblit);

    if (glossiness > 0.0)
    {
        float lod = (1.0-glossiness)*reflection_lods;
        glossenv = sampleProbes(pos, normalize(refnormpersp), lod);

    }

    if (envIntensity > 0.0)
    {
        legacyenv = sampleProbes(pos, normalize(refnormpersp), 0.0);
    }

#if defined(SSR)
    // <FS> a transparent surface that is not glossy: the trace's answer is multiplied by the glossiness below (0): nothing of it would show, so it is not made (same picture)
    if (cube_snapshot != 1 && (!transparent || glossiness > 0.0))
    {
        vec4 ssr = vec4(0);

        if (transparent)
        {
            tapScreenSpaceReflection(1, tc, pos, norm, ssr, sceneMap, 1, vec3(0));
            ssr.a *= glossiness;
        }
        else
        {
            tapScreenSpaceReflection(1, tc, pos, norm, ssr, sceneMap, glossiness, vec3(0));
        }

        glossenv = mix(glossenv, ssr.rgb, ssr.a);
        legacyenv = mix(legacyenv, ssr.rgb, ssr.a);
    }
#endif

    tapHeroProbe(glossenv, pos, norm, glossiness);
    if (envIntensity > 0.0)
    {
        tapHeroProbe(legacyenv, pos, norm, 1.0);        // <FS> every caller uses the legacy environment term only with an environment intensity above 0 (applyLegacyEnv mixes it in by that intensity)
    }

    glossenv = clamp(glossenv, vec3(0), vec3(10));
}

void applyGlossEnv(inout vec3 color, vec3 glossenv, vec4 spec, vec3 pos, vec3 norm)
{
    glossenv *= 0.5; // fudge darker
    float fresnel = clamp(1.0+dot(normalize(pos.xyz), norm.xyz), 0.3, 1.0);
    fresnel *= fresnel;
    fresnel *= spec.a;
    glossenv *= spec.rgb*fresnel;
    glossenv *= vec3(1.0) - color; // fake energy conservation
    color.rgb += glossenv*0.5;
}

 void applyLegacyEnv(inout vec3 color, vec3 legacyenv, vec4 spec, vec3 pos, vec3 norm, float envIntensity)
 {
    vec3 reflected_color = legacyenv;
    vec3 lookAt = normalize(pos);
    float fresnel = 1.0+dot(lookAt, norm.xyz);
    fresnel *= fresnel;
    fresnel = min(fresnel+envIntensity, 1.0);
    reflected_color *= (envIntensity*fresnel);
    color = mix(color.rgb, reflected_color*0.5, envIntensity);
 }



// ======== class1/windlight/gammaF.glsl ========
// DEPRECATED

//soft clip effect has been moved to postDeferredGammaCorrect legacyGamma, this file is effectively dead
// but these functions need to be removed from all existing shaders before removing this file

vec3 scaleSoftClipFrag(vec3 light)
{
    return light;
}

vec3 scaleSoftClipFragLinear(vec3 light)
{ // identical to non-linear version and that's probably close enough
    return light;
}

vec3 scaleSoftClip(vec3 light)
{
    return light;
}

vec3 fullbrightScaleSoftClipFrag(vec3 light, vec3 add, vec3 atten)
{
    return light;
}

vec3 fullbrightScaleSoftClip(vec3 light)
{
    return light;
}



// ======== class1/windlight/atmosphericsFuncs.glsl ========

float getAmbientClamp() { return 1.0f; }

vec3 srgb_to_linear(vec3 col);

// return colors in sRGB space
void calcAtmosphericVars(vec3 inPositionEye, vec3 light_dir, float ambFactor, out vec3 sunlit, out vec3 amblit, out vec3 additive,
                         out vec3 atten)
{
    vec3 rel_pos = inPositionEye;

    //(TERRAIN) limit altitude
    if (abs(rel_pos.y) > max_y) rel_pos *= (max_y / rel_pos.y);

    vec3  rel_pos_norm = normalize(rel_pos);
    float rel_pos_len  = length(rel_pos);

    vec3  sunlight     = (sun_up_factor == 1) ? sunlight_color: moonlight_color;

    // sunlight attenuation effect (hue and brightness) due to atmosphere
    // this is used later for sunlight modulation at various altitudes
    vec3 light_atten = (blue_density + vec3(haze_density * 0.25)) * (density_multiplier * max_y);
    // I had thought blue_density and haze_density should have equal weighting,
    // but attenuation due to haze_density tends to seem too strong

    vec3 combined_haze = max(blue_density + vec3(haze_density), vec3(1e-6));
    vec3 blue_weight   = blue_density / combined_haze;
    vec3 haze_weight   = vec3(haze_density) / combined_haze;

    //(TERRAIN) compute sunlight from lightnorm y component. Factor is roughly cosecant(sun elevation) (for short rays like terrain)
    float above_horizon_factor = 1.0 / max(1e-6, lightnorm.y);
    sunlight *= exp(-light_atten * above_horizon_factor);  // for sun [horizon..overhead] this maps to an exp curve [0..1]

    // main atmospheric scattering line integral
    float density_dist = rel_pos_len * density_multiplier;

    // Transparency (-> combined_haze)
    // ATI Bugfix -- can't store combined_haze*density_dist*distance_multiplier in a variable because the ati
    // compiler gets confused.
    combined_haze = exp(-combined_haze * density_dist * distance_multiplier);

    // final atmosphere attenuation factor
    atten = combined_haze.rgb;

    // compute haze glow
    float haze_glow = dot(rel_pos_norm, lightnorm.xyz);

    // dampen sun additive contrib when not facing it...
    // SL-13539: This "if" clause causes an "additive" white artifact at roughly 77 degreees.
    //    if (length(light_dir) > 0.01)
    haze_glow *= max(0.0f, dot(light_dir, rel_pos_norm));

    haze_glow = 1. - haze_glow;
    // haze_glow is 0 at the sun and increases away from sun
    haze_glow = max(haze_glow, .001);  // set a minimum "angle" (smaller glow.y allows tighter, brighter hotspot)
    haze_glow *= glow.x;
    // higher glow.x gives dimmer glow (because next step is 1 / "angle")
    haze_glow = clamp(pow(haze_glow, glow.z), -100000, 100000);
    // glow.z should be negative, so we're doing a sort of (1 / "angle") function

    // add "minimum anti-solar illumination"
    haze_glow += .25;

    haze_glow *= sun_moon_glow_factor;

    vec3 amb_color = ambient_color;

    // increase ambient when there are more clouds
    vec3 tmpAmbient = amb_color + (vec3(1.) - amb_color) * cloud_shadow * 0.5;

    // Similar/Shared Algorithms:
    //     indra\llinventory\llsettingssky.cpp                                        -- LLSettingsSky::calculateLightSettings()
    //     indra\newview\app_settings\shaders\class1\windlight\atmosphericsFuncs.glsl -- calcAtmosphericVars()
    // haze color
    vec3 cs = sunlight.rgb * (1. - cloud_shadow);
    additive = (blue_horizon.rgb * blue_weight.rgb) * (cs + tmpAmbient.rgb) + (haze_horizon * haze_weight.rgb) * (cs * haze_glow + tmpAmbient.rgb);

    // brightness of surface both sunlight and ambient

    sunlit = sunlight.rgb;
    amblit = pow(tmpAmbient.rgb, vec3(0.9)) * 0.57;

    additive *= vec3(1.0 - combined_haze);

    // sanity clamp haze contribution
    additive = min(additive, vec3(10));
}

vec3 srgb_to_linear(vec3 col);

// provide a touch of lighting in the opposite direction of the sun light
    // so areas in shadow don't lose all detail
float ambientLighting(vec3 norm, vec3 light_dir)
{
    float ambient = min(abs(dot(norm.xyz, light_dir.xyz)), 1.0);
    ambient *= 0.5;
    ambient *= ambient;
    ambient = (1.0 - ambient);
    return ambient;
}

// return lit amblit in linear space, leave sunlit and additive in sRGB space
void calcAtmosphericVarsLinear(vec3 inPositionEye, vec3 norm, vec3 light_dir, out vec3 sunlit, out vec3 amblit, out vec3 additive,
                         out vec3 atten)
{
    calcAtmosphericVars(inPositionEye, light_dir, 1.0, sunlit, amblit, additive, atten);

    amblit *= ambientLighting(norm, light_dir);

    if (classic_mode < 1)
    {
        amblit = srgb_to_linear(amblit);
        amblit = vec3(dot(amblit, vec3(0.2126, 0.7152, 0.0722)));
        sunlit = srgb_to_linear(sunlit);
    }

    // multiply to get similar colors as when the "scaleSoftClip" implementation was doubling color values
    // (allows for mixing of light sources other than sunlight e.g. reflection probes)
    sunlit *= sky_sunlight_scale;
    amblit *= sky_ambient_scale;
}


// ======== class1/windlight/atmosphericsF.glsl ========
vec3 getAdditiveColor();
vec3 getAtmosAttenuation();
vec3 scaleSoftClipFrag(vec3 light);

vec3 srgb_to_linear(vec3 col);
vec3 linear_to_srgb(vec3 col);


vec3 atmosFragLighting(vec3 light, vec3 additive, vec3 atten)
{
    light *= atten.r;
    additive = srgb_to_linear(additive*2.0);
    additive *= sky_hdr_scale;
    light += additive;
    return light;
}

vec3 atmosFragLightingLinear(vec3 light, vec3 additive, vec3 atten)
{
    return atmosFragLighting(light, additive, atten);
}

vec3 atmosLighting(vec3 light)
{
    return atmosFragLighting(light, getAdditiveColor(), getAtmosAttenuation());
}


// ======== class1/environment/waterFogF.glsl ========

vec3 srgb_to_linear(vec3 col);
vec3 linear_to_srgb(vec3 col);

vec3 atmosFragLighting(vec3 light, vec3 additive, vec3 atten);

// get a water fog color that will apply the appropriate haze to a color given
// a blend function of (ONE, SOURCE_ALPHA)
vec4 getWaterFogViewNoClip(vec3 pos)
{
    vec3 view = normalize(pos);
    //normalize view vector
    float es = -(dot(view, waterPlane.xyz));

    //find intersection point with water plane and eye vector

    //get eye depth
    float e0 = max(-waterPlane.w, 0.0);

    vec3 int_v = waterPlane.w > 0.0 ? view * waterPlane.w/es : vec3(0.0, 0.0, 0.0);

    //get object depth
    float depth = length(pos - int_v);

    //get "thickness" of water
    float l = max(depth, 0.1);

    float kd = waterFogDensity;
    float ks = waterFogKS;
    vec4 kc = waterFogColor;

    float F = 0.98;

    float t1 = -kd * pow(F, ks * e0);
    float t2 = kd + ks * es;
    float t3 = pow(F, t2*l) - 1.0;

    float L = pow(min(t1/t2*t3, 1.0), 1.0/1.7);

    float D = pow(0.98, l*kd);

    return vec4(srgb_to_linear(kc.rgb)*L, D);
}

vec4 getWaterFogView(vec3 pos)
{
    if (dot(pos, waterPlane.xyz) + waterPlane.w > 0.0)
    {
        return vec4(0,0,0,1);
    }

    return getWaterFogViewNoClip(pos);
}

vec4 applyWaterFogView(vec3 pos, vec4 color)
{
    vec4 fogged = getWaterFogView(pos);

    color.rgb = color.rgb * fogged.a + fogged.rgb;

    return color;
}

vec4 applyWaterFogViewLinearNoClip(vec3 pos, vec4 color)
{
    vec4 fogged = getWaterFogViewNoClip(pos);
    color.rgb *= fogged.a;
    color.rgb += fogged.rgb;
    return color;
}

vec4 applyWaterFogViewLinear(vec3 pos, vec4 color)
{
    if (dot(pos, waterPlane.xyz) + waterPlane.w > 0.0)
    {
        return color;
    }

    return applyWaterFogViewLinearNoClip(pos, color);
}

// for post deferred shaders, apply sky and water fog in a way that is consistent with
// the deferred rendering haze post effects
vec4 applySkyAndWaterFog(vec3 pos, vec3 additive, vec3 atten, vec4 color)
{
    bool eye_above_water = dot(vec3(0), waterPlane.xyz) + waterPlane.w > 0.0;
    bool obj_above_water = dot(pos.xyz, waterPlane.xyz) + waterPlane.w > 0.0;

    if (eye_above_water)
    {
        if (!obj_above_water)
        {
            color.rgb = applyWaterFogViewLinearNoClip(pos, color).rgb;
        }
        else
        {
            color.rgb = atmosFragLighting(color.rgb, additive, atten);
        }
    }
    else
    {
        if (obj_above_water)
        {
            color.rgb = atmosFragLighting(color.rgb, additive, atten);
        }
        else
        {
            color.rgb = applyWaterFogViewLinearNoClip(pos, color).rgb;
        }
    }

    return color;
}


// ======== class3/deferred/rtReflectionF.glsl ========


#define FLT_MAX 3.402823466e+38

// <FS> (compile time 2026-11-12) the test views of the hit (rt-debug.txt hitq 3, 4, 6, 7 - 15, 17, 18, 22 - 24) each called the hit function again: five inlined copies of the biggest function of the pass. They are
// compiled only with RT_REFL_DEBUG (LL_VK_RTREFL_DEBUG=1 in the environment); the pass the viewer draws with has one call site.
#ifdef RT_REFL_DEBUG
#define RT_Q23 (rt_hit_quality == 23)
#define RT_Q24 (rt_hit_quality == 24)
#else
#define RT_Q23 false
#define RT_Q24 false
#endif

// <FS> the RT Reflection pass (doc/vulkan/PLAN-rt-reflection-pass.md): the traced glossy reflection, carved out of softenLightF.glsl so the lighting pass compiles without the ray queries.
// Two outputs: 0 = the reflection (rgb) and how much of it replaces the probe result (a; -1 = the ray was traced and found nothing, 0 = not traced), 1 = the hit distance for ray reconstruction (R).

// (declared above: const float M_PI = 3.14159265;)

#if defined(HAS_SUN_SHADOW) || defined(HAS_SSAO)
#endif



#if defined(HAS_SSAO)
#endif

// Inputs



vec4 getNorm(vec2 pos_screen);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);

void calcAtmosphericVarsLinear(vec3 inPositionEye, vec3 norm, vec3 light_dir, out vec3 sunlit, out vec3 amblit, out vec3 atten, out vec3 additive);
vec3  atmosFragLightingLinear(vec3 l, vec3 additive, vec3 atten);
vec3  scaleSoftClipFragLinear(vec3 l);

// reflection probe interface
void sampleReflectionProbes(inout vec3 ambenv, inout vec3 glossenv,
    vec2 tc, vec3 pos, vec3 norm, float glossiness, bool transparent, vec3 amblit_linear);
void sampleReflectionProbesLegacy(inout vec3 ambenv, inout vec3 glossenv, inout vec3 legacyenv,
        vec2 tc, vec3 pos, vec3 norm, float glossiness, float envIntensity, bool transparent, vec3 amblit_linear);
void applyGlossEnv(inout vec3 color, vec3 glossenv, vec4 spec, vec3 pos, vec3 norm);
void applyLegacyEnv(inout vec3 color, vec3 legacyenv, vec4 spec, vec3 pos, vec3 norm, float envIntensity);
float getDepth(vec2 pos_screen);

vec3 linear_to_srgb(vec3 c);
vec3 srgb_to_linear(vec3 c);




void calcHalfVectors(vec3 lv, vec3 n, vec3 v, out vec3 h, out vec3 l, out float nh, out float nl, out float nv, out float vh, out float lightDist);
void calcDiffuseSpecular(vec3 baseColor, float metallic, inout vec3 diffuseColor, inout vec3 specularColor);

vec3 pbrBaseLight(vec3 diffuseColor,
                  vec3 specularColor,
                  float metallic,
                  vec3 pos,
                  vec3 norm,
                  float perceptualRoughness,
                  vec3 light_dir,
                  vec3 sunlit,
                  float scol,
                  vec3 radiance,
                  vec3 irradiance,
                  vec3 colorEmissive,
                  float ao,
                  vec3 additive,
                  vec3 atten);

GBufferInfo getGBuffer(vec2 screenpos);
vec2 BRDF(float NoV, float roughness);       // probe (modes 10, 11)

#if defined(HAS_SUN_SHADOW) && defined(LL_RT_REFLECTIONS)
#define SOFTEN_RT_REFLECTION 1
vec4 rtReflection(vec3 origin, vec3 dir, vec3 sun_dir, vec3 sunlit_linear, vec3 amblit_linear, vec2 pos_screen, vec3 fallback);
bool rtTraceHit(vec3 origin, vec3 dir, vec3 sun_dir, vec2 pos_screen, bool full, out vec3 albedo, out vec3 emissive, out vec3 n_geo_eye, out vec3 n_eye, out vec3 hit_pos, out float t,
                out float shadow, out bool fullbright, out vec4 mr, out vec4 spec);
bool rtTraceHitEx(vec3 origin, vec3 dir, vec3 sun_dir, vec2 pos_screen, bool full, bool layers, out vec3 albedo, out vec3 emissive, out vec3 n_geo_eye, out vec3 n_eye, out vec3 hit_pos, out float t,
                  out float shadow, out bool fullbright, out vec4 mr, out vec4 spec,
                  out float layer_alpha, out vec3 l_albedo, out vec3 l_emissive, out vec3 l_n_geo, out vec3 l_n, out vec3 l_hp, out float l_t, out float l_shadow, out bool l_fb, out vec4 l_mr, out vec4 l_spec,
                  out float m_alpha, out vec3 m_albedo, out vec3 m_emissive, out vec3 m_n_geo, out vec3 m_n, out vec3 m_hp, out float m_t, out float m_shadow, out bool m_fb, out vec4 m_mr, out vec4 m_spec);
void preProbeSample(vec3 pos);
void probeListSave();
void probeListReset();
void probeListRestore();
vec3 sampleProbes(vec3 pos, vec3 dir, float lod);
vec3 sampleProbeAmbient(vec3 pos, vec3 dir, vec3 amblit);

// local lights at a hit (rt_hit_quality 2): the nearest point lights as arrays (eye space centre and size, linear colour and falloff) and the first projector (spot light) through the
// uniforms its own shader uses (proj_mat, proj_n, proj_origin, proj_lod ... set by LLPipeline::setupSpotLight, the projected texture is projectionMap, colour / size / falloff of the light)
const int RT_HIT_LIGHTS = 8;
const float RT_LIGHT_SIZE = 0.5;        // metres: the radius of the disc the soft shadow rays of a local light are aimed at (rtShadowRayArea)
float rtShadowRay(vec3 origin, vec3 dir, float tmax, vec2 pos_screen, float lod);
float rtShadowRayHit(vec3 origin, vec3 dir, float tmax, vec2 pos_screen, float lod);
float rtShadowRayArea(vec3 origin, vec3 light_center, float margin, float radius, vec2 pos_screen, float lod);
bool clipProjectedLightVars(vec3 light_center, vec3 pos, out float dist, out float l_dist, out vec3 lv, out vec4 proj_tc);
vec3 getProjectedLightDiffuseColor(float light_distance, vec2 projected_uv);
vec3 getProjectedLightAmbiance(float amb_da, float attenuation, float lit, float nl, float noise, vec2 projected_uv);
float calcLegacyDistanceAttenuation(float distance, float falloff);
void pbrPunctual(vec3 diffuseColor, vec3 specularColor, float perceptualRoughness, float metallic, vec3 n, vec3 v, vec3 l, out float nl, out vec3 diff, out vec3 spec);
#endif
vec3 clampHDRRange(vec3 color);

void adjustIrradiance(inout vec3 irradiance, float ambocc)
{
    // use sky settings ambient or irradiance map sample, whichever is brighter
    //irradiance = max(amblit_linear, irradiance);

#if defined(HAS_SSAO)
    irradiance = mix(ssao_effect_mat * min(irradiance.rgb*ssao_irradiance_scale, vec3(ssao_irradiance_max)), irradiance.rgb, ambocc);
#endif
}

#ifdef SOFTEN_RT_REFLECTION
// What one local light adds to a hit, from the same formulas as pointLightF / spotLightF: radiance = the light's colour x its distance attenuation x its shadow (x the projected texture for a
// projector), L = unit vector from the hit to the light. pbr: pbrPunctual with the 3.25 balance factor, legacy: lambert on the albedo plus the blinn-phong highlight of the specular map.
struct RtHitSurface    // the parts of the hit the lights need (same unit only: a plain struct of the soften pass)
{
    bool pbr;
    vec3 diffuseColor;
    vec3 specularColor;
    float roughness;
    float metallic;
    vec3 albedo;
    vec4 spec;
};

vec3 rtLightOnHit(vec3 radiance, vec3 L, vec3 n, vec3 v, RtHitSurface s)
{
    if (s.pbr)
    {
        float nl = 0.0;
        vec3 diffPunc = vec3(0.0);
        vec3 specPunc = vec3(0.0);
        pbrPunctual(s.diffuseColor, s.specularColor, s.roughness, s.metallic, n, v, L, nl, diffPunc, specPunc);
        return radiance * 3.25 * clamp(nl * (diffPunc + specPunc), vec3(0.0), vec3(10.0));
    }
    vec3 h, l;
    float nh, nl, nv, vh, lightDist;
    calcHalfVectors(L, n, v, h, l, nh, nl, nv, vh, lightDist);
    vec3 c = radiance * nl * s.albedo;
    if (s.spec.a > 0.0 && nh > 0.0)
    {
        float lit = min(nl * 6.0, 1.0);
        float fres = pow(1.0 - vh, 5.0) * 0.4 + 0.5;
        float gtdenom = 2.0 * nh;
        float gt = max(0.0, min(gtdenom * nv / vh, gtdenom * nl / vh));
        float scol = fres * textureLod(lightFunc, vec2(nh, s.spec.a), 0.0).r * gt / (nh * nl);
        c += clamp(radiance * lit * scol * s.spec.rgb, vec3(0.0), vec3(1.0));
    }
    return c;
}

// the ray to a light stops short of the light's centre: the light sits inside a prim (a lamp, the projector box), whose own surface would block the ray; the main pass does not shadow lights at all.
// rt_hit_quality 4 (and 5 below) skip the shadow rays: a test of that
bool rt_lights_noshadow = false;
vec3 rt_dbg_point = vec3(0.0);       // the debug view (rt_hit_quality 3 / 4): what the point lights and the projector added, and how far the projector got
vec3 rt_dbg_spot = vec3(0.0);
float rt_dbg_status = 0.0;
float rtLightMargin(float d)
{
    return max(0.3, 0.1 * d);
}

vec3 rtHitLocalLights(vec3 hp, vec3 n_geo, vec3 n, vec3 v, float lod, vec2 tc, RtHitSurface s)
{
    vec3 sum = vec3(0.0);
    vec3 base = hp + n_geo * (0.02 + 0.0006 * length(hp));
    for (int i = 0; i < RT_HIT_LIGHTS; ++i)
    {
        if (i >= rt_hit_light_count)
        {
            break;
        }
        vec3 lv = rt_hit_light_pos[i].xyz - hp;
        float d = length(lv);
        float sz = rt_hit_light_pos[i].w;
        if (d >= sz || d < 0.001)
        {
            continue;
        }
        vec3 L = lv / d;
        if (dot(n, L) <= 0.0 && dot(n_geo, L) <= 0.0)
        {
            continue;
        }
        float atten = calcLegacyDistanceAttenuation(d / sz, rt_hit_light_col[i].a);
        if (atten <= 0.0)
        {
            continue;
        }
        if (rt_hit_light_dir[i].w > 0.5)
        {
            continue;                       // spot lights: the textured projector below, or the forward lighting of blended hits
        }
        // a light that adds next to nothing at this hit needs no shadow ray
        const bool faint = max(rt_hit_light_col[i].r, max(rt_hit_light_col[i].g, rt_hit_light_col[i].b)) * atten * max(dot(n, L), 0.0) < 0.004;
        float sh = (rt_lights_noshadow || faint) ? 1.0 : rtShadowRayArea(base, rt_hit_light_pos[i].xyz, rtLightMargin(d), RT_LIGHT_SIZE, tc, lod);
        vec3 add = rtLightOnHit(rt_hit_light_col[i].rgb * atten * sh, L, n, v, s);
        sum += add;
        rt_dbg_point += add;
    }
    if (rt_hit_spot != 0)
    {
        float sdist, l_dist;
        vec3 slv;
        vec4 proj_tc;
        if (!clipProjectedLightVars(rt_hit_spot_center, hp, sdist, l_dist, slv, proj_tc))
        {
            float atten = calcLegacyDistanceAttenuation(sdist, falloff);
            rt_dbg_status = 0.3;
            if (atten > 0.0 && proj_tc.z > 0.0 && proj_tc.x > 0.0 && proj_tc.x < 1.0 && proj_tc.y > 0.0 && proj_tc.y < 1.0)
            {
                rt_dbg_status = 0.6;
                vec3 L = normalize(proj_origin - hp);
                float nl = dot(n, L);
                vec3 to_center = rt_hit_spot_center - hp;
                float cd = length(to_center);
                float sh = 1.0;
                if (nl > 0.0 && cd > 0.05 && !rt_lights_noshadow)
                {
                    sh = rtShadowRayArea(base, rt_hit_spot_center, rtLightMargin(cd), RT_LIGHT_SIZE, tc, lod);
                }
                if (nl > 0.0)
                {
                    rt_dbg_status = 1.0;
                }
                float amb_da = 0.0;
                float lit = 0.0;
                if (nl > 0.0)
                {
                    lit = nl * atten;
                    amb_da = (nl * 0.5 + 0.5) * proj_ambiance;
                    vec3 add = rtLightOnHit(getProjectedLightDiffuseColor(l_dist, proj_tc.xy) * atten * sh, L, n, v, s);
                    sum += add;
                    rt_dbg_spot += add;
                }
                // the projector's ambiance (the soft light around the cone), not shadowed in the main pass either
                vec3 amb_rgb = getProjectedLightAmbiance(amb_da, atten, lit, nl, 1.0, proj_tc.xy);
                if (s.pbr)
                {
                    float nl2 = 0.0;
                    vec3 diffPunc = vec3(0.0);
                    vec3 specPunc = vec3(0.0);
                    pbrPunctual(s.diffuseColor, s.specularColor, s.roughness, s.metallic, n, v, L, nl2, diffPunc, specPunc);
                    sum += amb_rgb * 3.25 * clamp(nl2 * (diffPunc + specPunc), vec3(0.0), vec3(10.0));
                }
                else
                {
                    sum += s.albedo * amb_rgb * max(dot(-L, n), 0.0);
                }
            }
        }
    }
    return sum;
}

// Blended legacy hits (hair): the viewer lights them in the forward alpha pass (materialF.glsl calcPointLightOrSpotLight) with the nearby lights as plain lights: a spot light is a wide cone with the
// spot exponent 2 (no projected texture, no edge, no shadow), the attenuation is tweaked to match the deferred one, the result is scaled by 0.9 (classic mode) and the whole colour by 1.1 (classic mode)
vec3 rtHitForwardLights(vec3 hp, vec3 n, vec3 albedo)
{
    vec3 sum = vec3(0.0);
    for (int i = 0; i < RT_HIT_LIGHTS; ++i)
    {
        if (i >= rt_hit_light_count)
        {
            break;
        }
        vec3 lv = rt_hit_light_pos[i].xyz - hp;
        float d = length(lv);
        float sz = rt_hit_light_pos[i].w;
        if (d >= sz || d < 0.001)
        {
            continue;
        }
        lv /= d;
        float atten = calcLegacyDistanceAttenuation(d / sz, rt_hit_light_col[i].a);
        float da = 1.0;
        if (rt_hit_light_dir[i].w > 0.5)
        {
            float spot = max(dot(-rt_hit_light_dir[i].xyz, lv), 0.0);
            da *= spot * spot;
        }
        da *= dot(n, lv);
        if (da > 0.0)
        {
            sum += clamp(da * atten, 0.0, 1.0) * rt_hit_light_col[i].rgb * albedo;
        }
    }
    return sum * (classic_mode > 0 ? 0.9 : 1.0);
}

// The lighting of one decoded hit (the steps of the header comment above): the colour of the surface seen along dir at hp, before the fade into the fallback.
vec3 rtLitHitColor(vec3 origin, vec3 dir, vec3 light_dir_n, vec3 sunlit_in, vec3 amblit_in, vec2 tc, vec3 albedo, vec3 emissive, vec3 n_geo, vec3 n, vec3 hp, float t, float shadow, bool fb, vec4 mr, vec4 spec)
{
    vec3 color;
    if (fb)
    {
        color = albedo + emissive;
    }
    else
    {
        // the probe list is state of the invocation (reflectionProbeF.glsl): the pixel's own list is put aside, the hit gets a clean one, the pixel's comes back at the end
        probeListSave();
        probeListReset();
        if (!RT_Q23)           // (23: a cost test without any probe lookup at the hit)
        {
            preProbeSample(hp);
        }
        vec3 irradiance = amblit_in;
        if (classic_mode == 0 && !RT_Q23)
        {
            irradiance = sampleProbeAmbient(hp, n, amblit_in);
        }
        vec3 refl = reflect(dir, n);
        vec3 v = -dir;
        if (mr.z > 0.5)
        {
            float rough = max(mr.y, 0.04);
            vec3 radiance = RT_Q23 ? vec3(0.1) : sampleProbes(hp, refl, rough * max_probe_lod);
            vec3 diffuseColor, specularColor;
            calcDiffuseSpecular(albedo, mr.x, diffuseColor, specularColor);
            color = pbrBaseLight(diffuseColor, specularColor, mr.x, v, n, rough, light_dir_n, sunlit_in, shadow, radiance, irradiance, emissive, 1.0, vec3(0.0), vec3(1.0));
            if (rt_hit_quality >= 2)
            {
                RtHitSurface s;
                s.pbr = true;
                s.diffuseColor = diffuseColor;
                s.specularColor = specularColor;
                s.roughness = rough;
                s.metallic = mr.x;
                s.albedo = albedo;
                s.spec = vec4(0.0);
                color += rtHitLocalLights(hp, n_geo, n, v, clamp(0.5 * log2(max(t + length(origin), 1.0)) - 1.0, 0.0, 4.0), tc, s);
            }
        }
        else
        {
            float da = clamp(dot(n, light_dir_n), 0.0, 1.0);
            vec3 sl = sunlit_in;
            vec3 c;
            if (classic_mode > 0)
            {
                da = pow(da, 1.2);
                vec3 sun_contrib = vec3(min(da, shadow));
                c = srgb_to_linear(irradiance * 0.9 + linear_to_srgb(sun_contrib) * sl * 0.7);
                sl = srgb_to_linear(sl);
            }
            else
            {
                c = irradiance + min(da, shadow) * sl;
            }
            c *= albedo;
            if (spec.a > 0.0)
            {
                vec3 h, l;
                float nh, nl, nv, vh, lightDist;
                calcHalfVectors(light_dir_n, n, v, h, l, nh, nl, nv, vh, lightDist);
                if (nl > 0.0 && nh > 0.0)
                {
                    float lit = min(nl * 6.0, 1.0);
                    float fres = pow(1.0 - vh, 5.0) * 0.4 + 0.5;
                    float gtdenom = 2.0 * nh;
                    float gt = max(0.0, min(gtdenom * nv / vh, gtdenom * nl / vh));
                    float sc = shadow * fres * textureLod(lightFunc, vec2(nh, spec.a), 0.0).r * gt / (nh * nl);
                    c += lit * sc * sl * spec.rgb;
                }
                vec3 glossenv = RT_Q23 ? vec3(0.0) : sampleProbes(hp, refl, (1.0 - spec.a) * max_probe_lod);
                applyGlossEnv(c, glossenv, spec, dir, n);
            }
            if (rt_hit_quality >= 2 && mr.w > -0.5)
            {
                RtHitSurface s;
                s.pbr = false;
                s.diffuseColor = albedo;
                s.specularColor = vec3(0.0);
                s.roughness = 1.0;
                s.metallic = 0.0;
                s.albedo = albedo;
                s.spec = spec;
                c += rtHitLocalLights(hp, n_geo, n, v, clamp(0.5 * log2(max(t + length(origin), 1.0)) - 1.0, 0.0, 4.0), tc, s);
            }
            c = mix(c, albedo, max(mr.w, 0.0));       // a legacy material with an emissive mask in the diffuse alpha: the lit colour is blended towards the albedo, like the soften pass does for the pixels of the main view
            if (mr.w < -0.5 && rt_hit_quality >= 2)
            {
                c += rtHitForwardLights(hp, n, albedo);         // blended: lit like the forward alpha pass (the lights block above is for opaque hits)
                c *= (classic_mode > 0 ? 1.1 : 1.0);
            }
            color = c + emissive;
        }
    }
    if (!fb)
    {
        probeListRestore();
    }
    return color;
}

// <FS> A traced reflection ray whose hit is lit like a surface of the main pass (doc/vulkan/PLAN-glossy-mirror-quality.md, steps A and B): the hit's normal map, roughness / metallic
// (glTF, through pbrBaseLight) or specular map (legacy materials, the blinn-phong lines of main() below), the sun with the ray's shadow, and the reflection probes sampled AT THE HIT for ambient
// light and for the surface's own specular reflection. rt_hit_quality 0 keeps the flat diffuse shading of rtReflection (the water uses that one only).
#ifdef RT_REFL_DEBUG
vec4 rtHitLightsOnly(vec3 origin, vec3 dir, vec3 light_dir_n, vec2 tc);
vec4 rtHitProjectorInternals(vec3 origin, vec3 dir, vec3 light_dir_n, vec2 tc);
#endif

// <FS> For DLSS ray reconstruction (pipeline.cpp, rrHitDistF.glsl): how far the traced reflection ray of this pixel went (metres from its origin to the hit; RT_HIT_DISTANCE_MISS when it found nothing,
// a far away environment). 0 = not traced. Written to the alpha of the soften output, moved to a target of its own right after the lighting.
const float RT_HIT_DISTANCE_MISS = 500.0;
float rt_hit_dist_out = 0.0;

vec4 rtReflectionLit(vec3 origin, vec3 dir, vec3 light_dir_n, vec3 sunlit_in, vec3 amblit_in, vec2 tc, vec3 fallback)
{
    if (rt_hit_quality <= 0)
    {
        return rtReflection(origin, dir, light_dir_n, sunlit_in, amblit_in, tc, fallback);
    }
#ifdef RT_REFL_DEBUG
    rt_lights_noshadow = (rt_hit_quality == 4 || rt_hit_quality == 5);
    if (rt_hit_quality == 3 || rt_hit_quality == 4)
    {
        return rtHitLightsOnly(origin, dir, light_dir_n, tc);
    }
    if (rt_hit_quality == 6)
    {
        return rtHitProjectorInternals(origin, dir, light_dir_n, tc);
    }
    if (rt_hit_quality >= 7 && rt_hit_quality <= 9 || rt_hit_quality >= 11 && rt_hit_quality <= 13)
    {
        // test views of the hit: 7 = material (red metallic, green roughness, blue 0.5 for a glTF material / the legacy glossiness x 0.5 + 0.5 for one with a specular map), 8 = the probe ambient at the hit
        // (irradiance), 9 = the sun term alone (N.L x shadow x sunlit, white = lit)
        vec3 a7, e7, ng7, n7, hp7;
        float t7, sh7;
        bool fb7;
        vec4 mr7, sp7;
        if (!rtTraceHit(origin, dir, light_dir_n, tc, true, a7, e7, ng7, n7, hp7, t7, sh7, fb7, mr7, sp7))
        {
            return vec4(0.0);
        }
        if (rt_hit_quality == 7)
        {
            return vec4(mr7.x, mr7.y, (mr7.z > 0.5 ? 0.5 : 0.0) + (sp7.a > 0.0 ? 0.5 * sp7.a + 0.5 : 0.0), fb7 ? 1.0 : 0.75);
        }
        if (rt_hit_quality == 11)
        {
            return vec4(a7, 1.0);                       // the albedo of the hit (linear)
        }
        if (rt_hit_quality == 12)
        {
            preProbeSample(hp7);
            return vec4((classic_mode == 0 ? sampleProbeAmbient(hp7, n7, amblit_in) : amblit_in) * a7, 1.0);       // the ambient part of a legacy hit: irradiance x albedo
        }
        if (rt_hit_quality == 13)
        {
            // yellow: a legacy material that is alpha blended; else red: a glTF material, green: the emissive mask amount, blue: the legacy glossiness
            return mr7.w < -0.5 ? vec4(1.0, 1.0, 0.0, 1.0) : vec4(mr7.z, mr7.w, sp7.a, 1.0);
        }
        if (rt_hit_quality == 8)
        {
            preProbeSample(hp7);
            return vec4(classic_mode == 0 ? sampleProbeAmbient(hp7, n7, amblit_in) : amblit_in, 1.0);
        }
        return vec4(vec3(clamp(dot(n7, light_dir_n), 0.0, 1.0) * sh7) * sunlit_in, 1.0);
    }
#endif
    vec3 albedo, emissive, n_geo, n, hp;
    float t, shadow;
    bool fb;
    vec4 mr, spec;
    float layer_alpha, l_t, l_shadow;
    vec3 l_albedo, l_emissive, l_n_geo, l_n, l_hp;
    bool l_fb;
    vec4 l_mr, l_spec;
    float m_alpha, m_t, m_shadow;
    vec3 m_albedo, m_emissive, m_n_geo, m_n, m_hp;
    bool m_fb;
    vec4 m_mr, m_spec;
    if (!rtTraceHitEx(origin, dir, light_dir_n, tc, !RT_Q24, true, albedo, emissive, n_geo, n, hp, t, shadow, fb, mr, spec, layer_alpha, l_albedo, l_emissive, l_n_geo, l_n, l_hp, l_t, l_shadow, l_fb, l_mr, l_spec,
                      m_alpha, m_albedo, m_emissive, m_n_geo, m_n, m_hp, m_t, m_shadow, m_fb, m_mr, m_spec))
    {
        rt_hit_dist_out = RT_HIT_DISTANCE_MISS;
        return vec4(0.0);
    }
    rt_hit_dist_out = t;
#ifdef RT_REFL_DEBUG
    if (rt_hit_quality == 22)
    {
        // debug: red = opacity of the nearest blended layer, green = of the second, blue = a blended surface is the hit itself (nothing opaque behind it)
        return vec4(max(layer_alpha, 0.0), m_alpha, layer_alpha < 0.0 ? 1.0 : 0.0, 1.0);
    }
    if (rt_hit_quality == 14)
    {
        // the hit's own albedo dimmed (so the avatar and the room can be told apart), plus: red = opacity of a blended layer in front of it, green = the highest opacity of any blended surface the ray met without using it
        // (it was behind the hit or too transparent), blue = how many (x0.25), yellow added where the opaque hit is a blended material itself, white where a blended surface was the hit (layer_alpha < 0)
        vec3 dbg = 0.3 * albedo;
        if (layer_alpha > 0.0)
        {
            dbg += vec3(layer_alpha, 0.0, 0.0);
        }
        else
        {
            dbg += vec3(0.0, l_shadow, min(l_t * 0.25, 1.0)) * 0.7;
        }
        if (layer_alpha < 0.0)
        {
            dbg += vec3(-layer_alpha);
        }
        if (mr.w < -0.5)
        {
            dbg += vec3(0.4, 0.4, 0.0);
        }
        return vec4(dbg, 1.0);
    }
    if (rt_hit_quality == 15)
    {
        // the albedo of the blended layer (linear), magenta where there is none; its distance in front of the hit in blue-less red channel is not shown
        return layer_alpha > 0.0 ? vec4(l_albedo, 1.0) : vec4(1.0, 0.0, 1.0, 1.0);
    }
    if (rt_hit_quality == 18)
    {
        return vec4(n_geo * 0.5 + 0.5, 1.0);          // test: the geometric normal of the hit (eye space)
    }
    if (rt_hit_quality == 17)
    {
        // test: every hit is lit as if its normal looked at the viewer (the flat triangle normals of avatars are suspected of leaving the hair and face in the dark)
        n = -dir;
        n_geo = -dir;
    }
#endif
    // the hit, then the blended surfaces in front of it (hair, an avatar's shell), the farther one first: each lit like any hit and composited over what is behind by its opacity, like the alpha pass of
    // the main view. One call site of the hit lighting for all of them (the function is big: three inlined copies tripled the compile time of this pass)
    vec3 color = vec3(0.0);
    for (int i = 0; i < 3; ++i)
    {
        const bool layer_pass = i > 0;
        if (layer_pass && (layer_alpha <= 0.0 || (i == 1 && m_alpha <= 0.0)))
        {
            continue;
        }
        const bool far_layer = i == 1;
        const vec3 s_albedo = i == 0 ? albedo : (far_layer ? m_albedo : l_albedo);
        const vec3 s_emissive = i == 0 ? emissive : (far_layer ? m_emissive : l_emissive);
        const vec3 s_n_geo = i == 0 ? n_geo : (far_layer ? m_n_geo : l_n_geo);
        const vec3 s_n = i == 0 ? n : (far_layer ? m_n : l_n);
        const vec3 s_hp = i == 0 ? hp : (far_layer ? m_hp : l_hp);
        const float s_t = i == 0 ? t : (far_layer ? m_t : l_t);
        const float s_shadow = i == 0 ? shadow : (far_layer ? m_shadow : l_shadow);
        const bool s_fb = i == 0 ? fb : (far_layer ? m_fb : l_fb);
        const vec4 s_mr = i == 0 ? mr : (far_layer ? m_mr : l_mr);
        const vec4 s_spec = i == 0 ? spec : (far_layer ? m_spec : l_spec);
        const vec3 lit = rtLitHitColor(origin, dir, light_dir_n, sunlit_in, amblit_in, tc, s_albedo, s_emissive, s_n_geo, s_n, s_hp, s_t, s_shadow, s_fb, s_mr, s_spec);
        color = i == 0 ? lit : mix(color, lit, far_layer ? m_alpha : layer_alpha);
    }
    float fade = smoothstep(250.0, 1200.0, t);
    color = mix(color, fallback, fade);
    if (layer_alpha < 0.0)
    {
        color = mix(fallback, color, -layer_alpha);        // only a blended surface on the way: partly transparent over what the caller has behind
    }
    return vec4(color, 1.0);
}
#endif

#if defined(SOFTEN_RT_REFLECTION) && defined(RT_REFL_DEBUG)
// rt_hit_quality 3 (test): a traced hit shows only what the local lights add (x4), magenta where the soften pass was sent no light at all
vec4 rtHitLightsOnly(vec3 origin, vec3 dir, vec3 light_dir_n, vec2 tc)
{
    vec3 albedo, emissive, n_geo, n, hp;
    float t, shadow;
    bool fb;
    vec4 mr, spec;
    if (!rtTraceHit(origin, dir, light_dir_n, tc, true, albedo, emissive, n_geo, n, hp, t, shadow, fb, mr, spec))
    {
        return vec4(0.0);
    }
    if (rt_hit_light_count == 0 && rt_hit_spot == 0)
    {
        return vec4(1.0, 0.0, 1.0, 1.0);
    }
    RtHitSurface s;
    s.pbr = mr.z > 0.5;
    calcDiffuseSpecular(albedo, mr.x, s.diffuseColor, s.specularColor);
    s.roughness = max(mr.y, 0.04);
    s.metallic = mr.x;
    s.albedo = albedo;
    s.spec = spec;
    if (!s.pbr)
    {
        s.diffuseColor = albedo;
        s.specularColor = vec3(0.0);
        s.roughness = 1.0;
        s.metallic = 0.0;
    }
    rtHitLocalLights(hp, n_geo, n, -dir, clamp(0.5 * log2(max(t + length(origin), 1.0)) - 1.0, 0.0, 4.0), tc, s);
    // red: the point lights, green: the projector (x4), blue: how far the projector got (0.3 inside its range, 0.6 inside its cone, 1 facing the hit)
    return vec4(4.0 * dot(rt_dbg_point, vec3(0.3, 0.59, 0.11)), 4.0 * dot(rt_dbg_spot, vec3(0.3, 0.59, 0.11)), rt_dbg_status, 1.0);
}
#endif

#if defined(SOFTEN_RT_REFLECTION) && defined(RT_REFL_DEBUG)
// rt_hit_quality 6 (test): the projector's uniforms and what its texture returns at the hit. red: |light colour| / 2, green: proj_lod / 10, blue: the projected diffuse colour x4 (luminance);
// the same pixel is also marked (alpha 1) only when the projector's range / cone test passed; magenta: no projector uploaded
vec4 rtHitProjectorInternals(vec3 origin, vec3 dir, vec3 light_dir_n, vec2 tc)
{
    vec3 albedo, emissive, n_geo, n, hp;
    float t, shadow;
    bool fb;
    vec4 mr, spec;
    if (!rtTraceHit(origin, dir, light_dir_n, tc, true, albedo, emissive, n_geo, n, hp, t, shadow, fb, mr, spec))
    {
        return vec4(0.0);
    }
    if (rt_hit_spot == 0)
    {
        return vec4(1.0, 0.0, 1.0, 1.0);
    }
    float sdist, l_dist;
    vec3 slv;
    vec4 proj_tc;
    vec3 dl = vec3(0.0);
    if (!clipProjectedLightVars(rt_hit_spot_center, hp, sdist, l_dist, slv, proj_tc))
    {
        if (proj_tc.x > 0.0 && proj_tc.x < 1.0 && proj_tc.y > 0.0 && proj_tc.y < 1.0 && proj_tc.z > 0.0)
        {
            dl = getProjectedLightDiffuseColor(l_dist, proj_tc.xy);
        }
    }
    return vec4(clamp(length(color) * 0.5, 0.0, 1.0), clamp(proj_lod * 0.1, 0.0, 1.0), clamp(4.0 * dot(dl, vec3(0.3, 0.59, 0.11)), 0.0, 1.0), 1.0);
}
#endif

// <FS> (mirror quality 2026-11-12) the hero probe's own weight at a pixel (reflectionProbeF.glsl): the glass of a planar mirror takes the planar view over whatever else its reflection is (the lighting
// resolve mixes it in after the traced reflection), so a pixel the mirror covers completely needs no trace. tapHeroProbe mixes the view into the colour by that weight: from a huge colour a result below
// 1000 means a weight of 1 (a weight below 0.999 leaves at least 1000 of the huge colour). Without HERO_PROBES the function does nothing, the colour stays huge and nothing is skipped.
void tapHeroProbe(inout vec3 glossenv, vec3 pos, vec3 norm, float glossiness);

bool planar_mirror_covers(vec3 pos, vec3 norm, float glossiness)
{
    vec3 probe = vec3(1.0e6);
    tapHeroProbe(probe, pos, norm, glossiness);
    return probe.r < 1.0e3;
}

void main()
{
    frag_data[0] = vec4(0.0);
    frag_data[1] = vec4(0.0);
#ifdef SOFTEN_RT_REFLECTION
    if (rt_glossy_reflections == 0)
    {
        return;
    }
    vec2  tc    = vary_fragcoord.xy;
    float depth = getDepth(tc.xy);
    if (depth >= 1.0)
    {
        return;         // the sky
    }
    vec4  pos = getPositionWithDepth(tc, depth);
    GBufferInfo gb = getGBuffer(tc);

    const bool sky_pixel = GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_HAS_HDRI) || GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_SKIP_ATMOS);
    if (sky_pixel)
    {
        return;
    }
    {
        // <FS> (mirror quality 2026-11-12) the glossiness test first: most pixels are not glossy, and the atmospheric set up below (the sky's light at the pixel) was done for all of them before the test
        // rejected them; a pixel the planar mirror covers completely needs no trace either (see planar_mirror_covers)
        const bool has_pbr = GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_HAS_PBR);
        const float gloss_here = has_pbr ? 1.0 - gb.specular.g : gb.specular.a;
        const float gloss_min = has_pbr ? (rt_glossy_reflections >= 4 ? 0.0 : 0.9) : (rt_glossy_reflections >= 4 ? 0.01 : 0.9);
        if (gloss_here < gloss_min || planar_mirror_covers(pos.xyz, gb.normal, gloss_here))
        {
            return;
        }
    }
    vec3 light_dir = (sun_up_factor == 1) ? sun_dir : moon_dir;
    vec3 sunlit, amblit, additive, atten;
    calcAtmosphericVarsLinear(pos.xyz, gb.normal, light_dir, sunlit, amblit, additive, atten);
    if (classic_mode > 0)
    {
        sunlit *= 1.35;
    }
    vec3 sunlit_linear = sunlit;
    vec3 amblit_linear = amblit;

    vec4 rt_refl = vec4(0.0);
    bool traced = false;
    if (GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_HAS_PBR))
    {
        float gloss = 1.0 - gb.specular.g;
        if (gloss >= (rt_glossy_reflections >= 4 ? 0.0 : 0.9))
        {
            // the probe result of the lighting pass (its fallback for a miss, for the far fade and for a blended surface alone)
            vec3 irradiance = amblit_linear;
            vec3 radiance = vec3(0.0);
            sampleReflectionProbes(irradiance, radiance, tc, pos.xyz, gb.normal, gloss, false, amblit_linear);
            rt_refl = rtReflectionLit(pos.xyz + gb.normal * 0.02, normalize(reflect(pos.xyz, gb.normal)), normalize(light_dir), sunlit_linear, amblit_linear, tc, radiance);
            traced = true;
        }
    }
    else
    {
        vec4 spec = gb.specular;
        spec.rgb = srgb_to_linear(spec.rgb);
        if (spec.a >= (rt_glossy_reflections >= 4 ? 0.01 : 0.9))
        {
            vec3 irradiance = amblit;
            vec3 glossenv = vec3(0);
            vec3 legacyenv = vec3(0);
            sampleReflectionProbesLegacy(irradiance, glossenv, legacyenv, tc, pos.xyz, gb.normal, spec.a, gb.envIntensity, false, amblit_linear);
            rt_refl = rtReflectionLit(pos.xyz + gb.normal * 0.02, normalize(reflect(pos.xyz, gb.normal)), normalize(light_dir), sunlit_linear, amblit_linear, tc, glossenv);
            traced = true;
        }
    }
    if (traced)
    {
        frag_data[0] = vec4(rt_refl.rgb, rt_refl.a != 0.0 ? rt_refl.a : -1.0);
        frag_data[1] = vec4(rt_hit_dist_out, 0.0, 0.0, 0.0);
    }
#endif
}

