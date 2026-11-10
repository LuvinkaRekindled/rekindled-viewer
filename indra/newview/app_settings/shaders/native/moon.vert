// moon.vert: the moon's quad, pushed to just in front of the stars (between the clouds and the stars). A line for line translation of "Deferred Windlight Moon Program pull" (deferred/moonV.glsl with
// LL_VERTEX_PULL); pairs with moon.frag.

#include "sky_pass.inc"
#include "lib/rows.glsl"
#include "lib/pullvert.glsl"

layout(location = 0) out vec2 vary_texcoord0;

void main()
{
    const Row row = RowTable(ll_packet_table).r[gl_InstanceIndex];
    const ModelBlock model = ModelTable(ll_packet_models).m[row.model];
    const uint index = pull_index(row);
    const vec3 position = RowVec4(row.positions).v[index].xyz;
    const vec2 texcoord0 = RowVec2(row.texcoords).t[index];
    const mat4 texture_matrix0 = row_texture_matrix(row, TexMatTable(ll_packet_texmats));

    //transform vertex
    vec4 vert = vec4(position.xyz, 1.0);
    vec4 pos = (model.mvp * vert);

    // smash to *almost* far clip plane -- stars are still behind
    // SL-19283 - finagle the moon position to be between clouds and stars.
    pos.z = pos.w * 0.999991;
    gl_Position = pos;
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;      // (GL clip space to Vulkan's, as the old programs get it appended)

    vary_texcoord0 = (texture_matrix0 * vec4(texcoord0, 0, 1)).xy;
}
