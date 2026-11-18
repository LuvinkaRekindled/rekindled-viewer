// stars.vert: the stars: smashed to the far clip plane, the twinkle coordinate from the time. A line for line translation of "Deferred Star Program pull" (deferred/starsV.glsl with RK_VERTEX_PULL);
// pairs with stars.frag.

#include "sky_pass.inc"
#include "lib/rows.glsl"
#include "lib/pullvert.glsl"

layout(location = 0) out vec4 vertex_color;
layout(location = 1) out vec2 vary_texcoord0;
layout(location = 2) out vec2 screenpos;

void main()
{
    const Row row = RowTable(rk_packet_table).r[gl_InstanceIndex];
    const ModelBlock model = ModelTable(rk_packet_models).m[row.model];
    const uint index = pull_index(row);
    const vec3 position = RowVec4(row.positions).v[index].xyz;
    const vec2 texcoord0 = RowVec2(row.texcoords).t[index];
    const vec4 diffuse_color = unpackUnorm4x8(RowWords(row.colors).w[index]);
    const mat4 texture_matrix0 = row_texture_matrix(row, TexMatTable(rk_packet_texmats));

    //transform vertex
    vec4 pos = model.mvp * vec4(position, 1.0);

    // smash to far clip plane to
    // avoid rendering on top of moon (do NOT write to gl_FragDepth, it's slow)
    pos.z = pos.w;
    gl_Position = pos;
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;      // (GL clip space to Vulkan's, as the old programs get it appended)

    float t = mod(time, 1.25f);
    screenpos = position.xy * vec2(t, t);

    vary_texcoord0 = (texture_matrix0 * vec4(texcoord0, 0, 1)).xy;
    vertex_color = diffuse_color;
}
