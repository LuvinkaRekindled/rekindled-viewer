// rr_guides.frag: the guide buffers of the denoiser (ray reconstruction): diffuse and specular albedo, the world normal and roughness, and the motion vector of what a traced reflection shows. A line for line
// translation of "Ray Reconstruction Guides" (deferred/rrGuidesF.glsl); pairs with fullscreen.vert. (getGBuffer is the stock deferred/gbufferUtil.glsl, calcDiffuseSpecular the stock deferredUtil.glsl: local copies.)

#define LEGACY_COLOR 1
#define LEGACY_DEPTH 1

layout(std140, set = 0, binding = 0) uniform LLLooseUniforms
{
    mat4 inv_proj;
    mat3 view_to_world;         // the rotation from the eye space the G-buffer normals are in to the world space the denoiser is told about
    // <FS> the fourth output is the motion vector of what the traced reflection shows (what rrReflMotionF.glsl made in a pass of its own): the reads and the maths are the same
    mat4 inv_scene;             // jittered clip space of this frame -> world
    mat4 view_proj_cur;         // world -> unjittered clip space of this frame
    mat4 view_proj_prev;        // world -> unjittered clip space of the last frame
    vec3 cam_pos;               // the camera in the world
    vec2 render_res;
    float use_refl;             // 0: the hit distance is not valid yet (the output is the surface motion)
};

layout(std140, set = 0, binding = 5) uniform LLLooseDraw
{
    uint ll_ti_diffuseRect;
    uint ll_ti_specularRect;
    uint ll_ti_normalMap;
    uint ll_ti_depthMap;
    uint ll_ti_diffuseMap;      // the surface motion vectors (rg, pixels)
    uint ll_ti_bumpMap;         // the traced reflection's hit distance (r, metres from the surface; 0 = not traced)
};

#include "lib/flags.glsl"
#include "lib/legacy.glsl"

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 0) out vec4 frag_data0;
layout(location = 1) out vec4 frag_data1;
layout(location = 2) out vec4 frag_data2;
layout(location = 3) out vec4 frag_data3;

struct GBufferInfo
{
    vec4 albedo;
    vec3 normal;
    vec4 specular;
    float envIntensity;
    float gbufferFlag;
    vec4 emissive;
};

vec4 getNormRaw(vec2 screenpos)
{
    return texture(ll_tbl2d[LL_HM(ll_ti_normalMap)], screenpos.xy);
}

vec4 getNorm(vec2 screenpos)
{
    return decodeNormal(getNormRaw(screenpos));
}

float getDepth(vec2 pos_screen)
{
    return texture(ll_tbl2d[LL_HM(ll_ti_depthMap)], pos_screen).r;
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

GBufferInfo getGBuffer(vec2 screenpos)
{
    GBufferInfo ret;
    vec4 diffInfo = vec4(0);
    vec4 specInfo = vec4(0);
    vec4 emissInfo = vec4(0);

    diffInfo = texture(ll_tbl2d[LL_HM(ll_ti_diffuseRect)], screenpos.xy);
    specInfo = texture(ll_tbl2d[LL_HM(ll_ti_specularRect)], screenpos.xy);
    vec4 normInfo = getNormRaw(screenpos);

    ret.albedo = diffInfo;
    ret.normal = decodeNormal(normInfo).xyz;
    ret.specular = specInfo;
    ret.envIntensity = normInfo.b;
    ret.gbufferFlag = normInfo.w;
    ret.emissive = emissInfo;

    return ret;
}

void calcDiffuseSpecular(vec3 baseColor, float metallic, inout vec3 diffuseColor, inout vec3 specularColor)
{
    vec3 f0 = vec3(0.04);
    diffuseColor = baseColor*(vec3(1.0)-f0);
    diffuseColor *= 1.0 - metallic;
    specularColor = mix(f0, baseColor, metallic);
}

// the analytic approximation of the split sum environment BRDF (Karis, mobile) applied to F0
vec3 envBrdfApprox(vec3 f0, float roughness, float nov)
{
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * nov)) * r.x + r.y;
    vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + vec3(ab.y);
}

void main()
{
    vec2 tc = vary_fragcoord.xy;
    float depth = getDepth(tc);

    vec3 diffuse = vec3(1.0);
    vec3 specular = vec3(0.0);
    vec3 n = vec3(0.0, 0.0, 1.0);
    float rough = 1.0;

    if (depth < 1.0)
    {
        GBufferInfo gb = getGBuffer(tc);
        if (GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_HAS_HDRI) || GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_SKIP_ATMOS))
        {
            // the sky: the defaults
        }
        else
        {
            vec3 v = -normalize(getPosition(tc).xyz);
            float nov = clamp(abs(dot(gb.normal, v)), 0.001, 1.0);
            vec3 f0 = vec3(0.04);
            if (GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_HAS_PBR))
            {
                vec3 orm = gb.specular.rgb;
                rough = clamp(orm.g, 0.0, 1.0);
                vec3 d = vec3(0.0);
                calcDiffuseSpecular(gb.albedo.rgb, orm.b, d, f0);
                diffuse = d;
            }
            else
            {
                // legacy materials: sRGB colours, the alpha of the specular is the glossiness
                diffuse = srgb_to_linear(gb.albedo.rgb);
                float gloss = gb.specular.a;
                rough = clamp(1.0 - gloss, 0.0, 1.0);
                if (gloss > 0.0)
                {
                    f0 = max(srgb_to_linear(gb.specular.rgb), vec3(0.04));
                }
            }
            specular = envBrdfApprox(f0, rough, nov);
            n = normalize(view_to_world * gb.normal);
        }
    }

    frag_data0 = vec4(clamp(diffuse, 0.0, 1.0), 1.0);
    frag_data1 = vec4(clamp(specular, 0.0, 1.0), 1.0);
    frag_data2 = vec4(n, rough);

    vec2 mv = texture(ll_tbl2d[LL_HM(ll_ti_diffuseMap)], tc).rg;
    float t = use_refl > 0.5 ? texture(ll_tbl2d[LL_HM(ll_ti_bumpMap)], tc).r : 0.0;
    if (t > 0.0 && depth < 1.0)
    {
        vec4 w = inv_scene * vec4(tc * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
        vec3 p = w.xyz / w.w;
        vec3 nw = normalize(view_to_world * getNorm(tc).xyz);
        vec3 r = reflect(normalize(p - cam_pos), nw);
        vec4 h = vec4(p + r * t, 1.0);
        vec4 cur = view_proj_cur * h;
        vec4 prev = view_proj_prev * h;
        if (cur.w > 0.0 && prev.w > 0.0)
        {
            mv = (prev.xy / prev.w - cur.xy / cur.w) * 0.5 * render_res;
        }
    }
    frag_data3 = vec4(mv, 0.0, 0.0);
}
