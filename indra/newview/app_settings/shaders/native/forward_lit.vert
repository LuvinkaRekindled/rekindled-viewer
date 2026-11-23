// forward_lit.vert: the old forward lit vertex stage of the avatar previews: the vertex (FWD_AVATAR: skinned by the avatar's matrix palette; otherwise transformed by the draw's matrices), the preview's directional
// light summed per vertex (sumLights) and the atmospherics (calcAtmospherics: the haze colour and attenuation for the fragment). A line for line translation of "Avatar Shader" (avatar/avatarV.glsl, avatarSkinV.glsl)
// and "No color alpha mask Shader" (objects/simpleNoColorV.glsl) with the stock lighting files (lighting/sumLightsV.glsl, lightFuncV.glsl, class3 lighting/lightV.glsl, windlight/atmosphericsV.glsl); pairs with forward_lit.frag.

#define LEGACY_COLOR 1
#define LEGACY_SKY 1

#include "forward_pass.inc"
#include "lib/legacy.glsl"

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 texcoord0;
#ifdef FWD_AVATAR
layout(location = 9) in vec4 weight;
#endif

layout(location = 0) out vec4 vertex_color;
layout(location = 1) out vec2 vary_texcoord0;
layout(location = 2) out vec3 vary_AdditiveColor;
layout(location = 3) out vec3 vary_AtmosAttenuation;

float calcDirectionalLight(vec3 n, vec3 l)
{
    float a = max(dot(n,l),0.0);
    return a;
}

vec4 sumLights(vec3 pos, vec3 norm, vec4 color)
{
    vec4 col = vec4(0);
    col.a = color.a;

    col.rgb = light_diffuse[1].rgb * calcDirectionalLight(norm, light_position[1].xyz);
    col.rgb += light_diffuse[1].rgb * sqrt(calcDirectionalLight(norm, -light_position[1].xyz)*0.5+0.25);

    col.rgb = min(col.rgb*color.rgb, 1.0);
    return col;
}

// used for preview renders only
vec4 calcLighting(vec3 pos, vec3 norm, vec4 color)
{
    vec4 c = sumLights(pos, norm, color);

    return c;
}

void calcAtmospherics(vec3 inPositionEye) {
    vec3 P = inPositionEye;
    vec3 tmpsunlit = vec3(1);
    vec3 tmpamblit = vec3(1);
    vec3 tmpaddlit = vec3(1);
    vec3 tmpattenlit = vec3(1);
    vec3 light_dir = (sun_up_factor == 1) ? sun_dir : moon_dir;
    calcAtmosphericVars(inPositionEye, light_dir, 1, tmpsunlit, tmpamblit, tmpaddlit, tmpattenlit);
    vary_AdditiveColor = tmpaddlit;
    vary_AtmosAttenuation = tmpattenlit;
}

#ifdef FWD_AVATAR
mat4 getSkinnedTransform()
{
    mat4 ret;
    // a NaN or out-of-range weight (the vertex data is rewritten every update) turned into an index far outside the palette: the read then lands in memory that is not ours (device lost)
    float wx = weight.x;
    // The palette holds 15 joints (slots 0-14). A body mesh can use all 15 (the right wrist of a mesh with its ancestor entries is joint 14, weight 14.0 for its pure vertices): the
    // limit used to be 13.999, which sent every vertex of joint 14 to joint 0 (the stretched spike from the right hand). The blend partner of joint 14 is joint 14 itself
    // (weight 14.x does not occur), so no read goes past slot 14 of a row (slot 45 would be outside the array).
    if (!(wx >= 0.0 && wx <= 14.0))
    {
        wx = 0.0;
    }
    int i = int(floor(wx));
    int n = min(i + 1, 14);
    float x = fract(wx);

    ret[0] = matrixPalette[i+0];
    ret[1] = matrixPalette[i+15];
    ret[2] = matrixPalette[i+30];
    if (x > 0.0)        // (the second joint of the pair is not read when the weight is zero: most body vertices belong to one joint)
    {
        ret[0] = mix(ret[0], matrixPalette[n+0], x);
        ret[1] = mix(ret[1], matrixPalette[n+15], x);
        ret[2] = mix(ret[2], matrixPalette[n+30], x);
    }
    ret[3] = vec4(0,0,0,1);

    return ret;
}
#endif

void main()
{
#ifdef FWD_AVATAR
    vary_texcoord0 = texcoord0;

    vec4 pos;
    vec3 norm;

    vec4 pos_in = vec4(position.xyz, 1.0);

    mat4 trans = getSkinnedTransform();
    pos.x = dot(trans[0], pos_in);
    pos.y = dot(trans[1], pos_in);
    pos.z = dot(trans[2], pos_in);
    pos.w = 1.0;

    norm.x = dot(trans[0].xyz, normal);
    norm.y = dot(trans[1].xyz, normal);
    norm.z = dot(trans[2].xyz, normal);
    norm = normalize(norm);

    gl_Position = projection_matrix * pos;
#else
    //transform vertex
    vec4 pos = (modelview_matrix * vec4(position.xyz, 1.0));
    gl_Position = modelview_projection_matrix * vec4(position.xyz, 1.0);
    vary_texcoord0 = (texture_matrix0 * vec4(texcoord0,0,1)).xy;

    vec3 norm = normalize(normal_matrix * normal);
#endif
    RK_CLIP_Z(gl_Position);      // (GL clip space to Vulkan's, as the old programs get it appended)

    calcAtmospherics(pos.xyz);

    vec4 col = calcLighting(pos.xyz, norm, color);
    vertex_color = col;
}
