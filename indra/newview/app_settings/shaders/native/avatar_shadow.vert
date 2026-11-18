// avatar_shadow.vert: the system avatar's body parts (skin, hair, eyes, eyelashes) in the shadow passes, from the geometry packet path: one row of the scene table a joint mesh range, skinned by the joint
// palette uniform (lib/avatarskin.glsl, the same code as the G-buffer pass avatar.vert) and projected by the shadow camera's matrix. Stands in for the pull programs "Deferred Avatar Shadow Shader pull",
// "Deferred Avatar Alpha Shadow Shader pull" and "Deferred Avatar Alpha Mask Shadow Shader pull" (deferred/avatarShadowV.glsl and avatarAlphaShadowV.glsl with RK_VERTEX_PULL). Define SHADOW_ALPHA: the two
// alpha tested ones, which need the texture coordinate (avatar_shadow.frag). The old programs also skinned the normal (three dot products and a normalize that nothing read) and handed the fragment stage
// `pos_w` and `target_pos_x` for the dither of the semi transparent parts: the fragment stage takes the pixel column from gl_FragCoord now, so the vertex stage hands over the texture coordinate only.

#include "lib/rows.glsl"

#include "avatar_shadow.inc"

layout(std140, set = 0, binding = 8) uniform RKLooseSkin
{
    vec4 matrixPalette[45];     // joint i: rows at i, i + 15 and i + 30
};

#include "lib/avatarskin.glsl"

#ifdef SHADOW_ALPHA
layout(location = 0) out vec2 vary_texcoord0;
#endif

void main()
{
    const Row row = RowTable(rk_packet_table).r[gl_InstanceIndex];
    const uint at = row.first + uint(gl_VertexIndex);
    uint index = at;
    if ((row.flags & 4u) == 0u)
    {
        const uint word = RowWords(row.indices).w[at >> 1];
        index = (at & 1u) != 0u ? (word >> 16) : (word & 0xFFFFu);
    }
    const vec4 pos_in = vec4(RowVec4(row.positions).v[index].xyz, 1.0);

    vec4 t0, t1, t2;
    avatar_skin(row, index, t0, t1, t2);
    gl_Position = projection_matrix * vec4(dot(t0, pos_in), dot(t1, pos_in), dot(t2, pos_in), 1.0);
    // GL clip space z is [-w, w], Vulkan's is [0, w]: the viewer's own vertex shaders get this line from the translator, a native one does it itself (see alpha.vert)
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
#ifdef SHADOW_ALPHA
    vary_texcoord0 = RowVec2(row.texcoords).t[index];
#endif
}
