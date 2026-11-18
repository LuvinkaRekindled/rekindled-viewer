// dnsr_prep.frag: the inputs the AMD FidelityFX reflection denoiser takes from the G-buffer (PLAN-amd-denoiser.md): the world normal (rgb, RGBA16F), the roughness (r, R16F) and the raw depth (r, R32F:
// the denoiser's own passes read a colour image, the G-buffer's depth image is a depth format). The reads and the maths are those of rr_guides.frag (getGBuffer, the roughness of a PBR or a legacy
// material, the normal turned into the world); pairs with fullscreen.vert.

#define LEGACY_COLOR 1
#define LEGACY_DEPTH 1

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
    mat4 inv_proj;
    mat3 view_to_world;         // the rotation from the eye space the G-buffer normals are in to the world space the denoiser is told about
};

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    uint rk_ti_diffuseRect;
    uint rk_ti_specularRect;
    uint rk_ti_normalMap;
    uint rk_ti_depthMap;
};

#include "lib/flags.glsl"
#include "lib/legacy.glsl"

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 0) out vec4 frag_data0;       // world normal
layout(location = 1) out vec4 frag_data1;       // roughness
layout(location = 2) out vec4 frag_data2;       // raw depth

vec4 getNormRaw(vec2 screenpos)
{
    return texture(rk_tbl2d[LL_HM(rk_ti_normalMap)], screenpos.xy);
}

float getDepth(vec2 pos_screen)
{
    return texture(rk_tbl2d[LL_HM(rk_ti_depthMap)], pos_screen).r;
}

void main()
{
    vec2 tc = vary_fragcoord.xy;
    float depth = getDepth(tc);

    vec3 n = vec3(0.0, 0.0, 1.0);
    float rough = 1.0;

    if (depth < 1.0)
    {
        vec4 normInfo = getNormRaw(tc);
        vec4 specInfo = texture(rk_tbl2d[LL_HM(rk_ti_specularRect)], tc);
        float gbufferFlag = normInfo.w;
        if (GET_GBUFFER_FLAG(gbufferFlag, GBUFFER_FLAG_HAS_HDRI) || GET_GBUFFER_FLAG(gbufferFlag, GBUFFER_FLAG_SKIP_ATMOS))
        {
            // the sky: the defaults
        }
        else
        {
            if (GET_GBUFFER_FLAG(gbufferFlag, GBUFFER_FLAG_HAS_PBR))
            {
                rough = clamp(specInfo.g, 0.0, 1.0);
            }
            else
            {
                // legacy materials: the alpha of the specular is the glossiness
                rough = clamp(1.0 - specInfo.a, 0.0, 1.0);
            }
            n = normalize(view_to_world * decodeNormal(normInfo).xyz);
        }
    }

    frag_data0 = vec4(n, 0.0);
    frag_data1 = vec4(rough, 0.0, 0.0, 1.0);
    frag_data2 = vec4(depth, 0.0, 0.0, 1.0);
}
