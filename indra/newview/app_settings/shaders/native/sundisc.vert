// sundisc.vert: the sun's quad, pushed to just in front of the stars and behind the clouds. A line for line translation of "Deferred Windlight Sun Program pull" (deferred/sunDiscV.glsl with LL_VERTEX_PULL; its
// call of calcAtmospherics only wrote varyings the fragment shader never reads); pairs with sundisc.frag.

#include "sky_pass.inc"
#include "lib/rows.glsl"
#include "lib/pullvert.glsl"

layout(location = 0) out vec2 vary_texcoord0;
layout(location = 1) out float sun_fade;

void main()
{
    const Row row = RowTable(ll_packet_table).r[gl_InstanceIndex];
    const ModelBlock model = ModelTable(ll_packet_models).m[row.model];
    const uint index = pull_index(row);
    const vec3 position = RowVec4(row.positions).v[index].xyz;
    const vec2 texcoord0 = RowVec2(row.texcoords).t[index];
    const mat4 texture_matrix0 = row_texture_matrix(row, TexMatTable(ll_packet_texmats));

    //transform vertex
    vec3 offset = vec3(0, 0, 50);
    vec4 vert = vec4(position.xyz - offset, 1.0);
    vec4 pos  = model.mvp * vert;

    sun_fade = smoothstep(0.3, 1.0, (position.z + 50) / 512.0f);

    // smash to *almost* far clip plane -- behind clouds but in front of stars
    pos.z = pos.w * 0.999999;
    gl_Position = pos;
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;      // (GL clip space to Vulkan's, as the old programs get it appended)

    vary_texcoord0 = (texture_matrix0 * vec4(texcoord0, 0, 1)).xy;
}
