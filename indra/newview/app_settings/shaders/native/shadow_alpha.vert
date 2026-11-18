// shadow_alpha.vert: an alpha tested shadow caster (leaves, glass, cut-out textures, glTF alpha mask) from a record of alpha shadow packets (the worker's 80 byte packet, RKVkBridge::shadowAlphaPackets):
// no vertex attributes; one packet a draw, drawn as a draw of its own whose instance index is the packet's number. Static and rigged casters are the same program: flag 32 of the packet says the vertex has
// weights and a palette (the shared skin function of lib/rows.glsl, as shadow.vert). Stands in for "Deferred Shadow Alpha Pull Shader" (deferred/shadowAlphaPullV.glsl).
// Packet flags: 1 fullbright (the vertex colour's alpha does not count), 2 tree style (no vertex alpha, no dithering), 4 one texture (no index in position.w), 8 texcoords present, 16 colours present,
// 32 rigged (weights and palette; joints = the palette's joint count), bits 12 - 15 the texture window, bits 16 - 17 the window width, 64 glTF alpha mask, 2048 opaque caster.
// What the old program also did and this one does not: it handed the fragment stage `post_pos` and `target_pos_x` for the dither of the semi transparent texels (a vec4, a float and a division a pixel);
// shadow_alpha.frag takes the pixel column from gl_FragCoord, which is the same column.

#include "lib/rows.glsl"

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
    uvec2 ll_packet_table;      // the packets of the record
    uvec2 ll_packet_models;     // the model matrices of the record
    uvec2 ll_packet_texmats;    // the record's texture matrices (the pad of a packet is its number, 0 = none; slot 0 holds the texture animation clock)
    mat4 ll_view_proj;          // the shadow camera's view projection (GL clip space)
    int shadow_alpha_indexed;   // 1: an indexed draw (the hardware read the index buffer: gl_VertexIndex is the vertex), 0: a draw over the index list that reads the indices here
};

struct AlphaPacket
{
    uvec2 positions;
    uvec2 indices;
    uvec2 texcoords;
    uvec2 colors;
    uint first;
    uint joints;
    uint model;
    uint flags;
    uvec2 slots;                // four 16 bit texture slots
    float cutoff;
    uint pad;                   // the numbers of up to two texture matrices (low half first)
    uvec2 weights;
    uvec2 palette;
};
layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer AlphaPackets { AlphaPacket p[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer AlphaModels { mat4 m[]; };

layout(location = 0) out float vary_alpha;
layout(location = 1) out vec2 vary_texcoord0;
layout(location = 2) flat out uint vary_slot;
layout(location = 3) flat out uint vary_flags;
layout(location = 4) flat out float vary_cutoff;

void main()
{
    const AlphaPacket pk = AlphaPackets(ll_packet_table).p[gl_InstanceIndex];
    uint index = uint(gl_VertexIndex);
    if (shadow_alpha_indexed == 0)
    {
        const uint at = pk.first + uint(gl_VertexIndex);
        const uint word = RowWords(pk.indices).w[at >> 1];
        index = (at & 1u) != 0u ? (word >> 16) : (word & 0xFFFFu);
    }
    const vec4 position = RowVec4(pk.positions).v[index];

    vec3 vertex = position.xyz;
    vec3 rig_anchor = vec3(0.0);
    if ((pk.flags & 32u) != 0u)
    {
        mat3 skin_rotation;
        vec3 skin_translation;
        skin_vertex(pk.weights, pk.palette, pk.joints, index, skin_rotation, skin_translation);
        vertex = skin_rotation * position.xyz + skin_translation;
        rig_anchor = skin_rig_anchor(pk.palette, pk.joints);
    }
    gl_Position = ll_view_proj * rig_transform(AlphaModels(ll_packet_models).m[pk.model], vertex, rig_anchor);
    // GL clip space z is [-w, w], Vulkan's is [0, w]: the viewer's own vertex shaders get this line from the translator, a native one does it itself (see alpha.vert)
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;

    // texture windows (RKVkBridge::kAlphaWindowShift): a batch of more than four textures is drawn by several entries over the same index range, each naming the textures of one window (the number of the
    // texture in position.w, shifted by the window's width; the window number is in bits 12 - 15 of the flags). The triangles of the other windows are moved outside the clip volume (the three vertices of
    // a triangle share the texture number, so a triangle goes whole)
    const uint tex_raw = (pk.flags & 4u) != 0u ? 0u : (floatBitsToUint(position.w) & 15u);
    const uint width_code = (pk.flags >> 16u) & 3u;
    const uint width_shift = width_code == 0u ? 2u : (width_code == 1u ? 1u : 0u);
    if ((tex_raw >> width_shift) != ((pk.flags >> 12u) & 15u))
    {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    }
    const uint tex_index = tex_raw & ((1u << width_shift) - 1u);
    const uint pair = tex_index < 2u ? pk.slots.x : pk.slots.y;
    vary_slot = (pair >> ((tex_index & 1u) * 16u)) & 0xFFFFu;
    vary_flags = pk.flags;
    vary_cutoff = pk.cutoff;

    vary_texcoord0 = (pk.flags & 8u) != 0u ? RowVec2(pk.texcoords).t[index] : vec2(0.0);
    if (pk.pad != 0u && (pk.flags & 8u) != 0u)
    {
        // up to two texture matrices, the low half of pad first (a glTF face with a texture animation: the animation, then the glTF transform; one for the others)
        TexMatTable table = TexMatTable(ll_packet_texmats);        // (a buffer reference variable cannot be const)
        for (uint part = 0u; part < 2u; ++part)
        {
            const uint number = part == 0u ? (pk.pad & 0xFFFFu) : (pk.pad >> 16u);
            if (number == 0u)
            {
                continue;
            }
            mat4 m = table.t[number];
            if (m[3][3] == 0.0)
            {
                m = texanim_matrix(m, table.t[0][0].yz);        // a texture animation's slot holds its parameters
            }
            vary_texcoord0 = (m * vec4(vary_texcoord0, 0.0, 1.0)).xy;
        }
    }
    vary_alpha = (pk.flags & 16u) != 0u ? float(RowWords(pk.colors).w[index] >> 24) * (1.0 / 255.0) : 1.0;
}
