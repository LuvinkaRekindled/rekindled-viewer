// shadow.vert: a shadow caster drawn from a record of shadow packets (the worker's packet table, LLVkBridge::shadowPackets): no vertex attributes and no bound vertex or index buffer. The worker writes one
// packet a draw (the buffer addresses, the index range and the model matrix number) and draws every packet as a draw of its own whose instance index is the packet's number. Only the position is read (the vertex
// buffers keep the positions first, as 16 byte vec4s), the indices are 16 bit.
// Variants by define: HAS_SKIN (a rigged caster: the 48 byte packet that also names the weights and the joint palette; its vertex goes through the same skin function as the scene rows, lib/rows.glsl skin_vertex).
// What the pass writes is depth: shadow.frag only has to exist.
//
// The viewer's old program (shadowPullV.glsl, shadowSkinPullV.glsl) multiplied the view projection by the model matrix for every vertex (`ll_view_proj * model * vec4` is (ll_view_proj * model) * vec4: a 4x4 by 4x4
// product a vertex); here the vertex goes through the model matrix first and the view projection second, two matrix by vector products.

#include "lib/rows.glsl"

layout(std140, set = 0, binding = 0) uniform LLLooseUniforms
{
    uvec2 ll_packet_table;      // the packets of the record (written by the draw record)
    uvec2 ll_packet_models;     // the model matrices of the record
    mat4 ll_view_proj;          // the shadow camera's view projection (GL clip space, as the viewer's matrices are)
#ifdef HAS_SKIN
    int shadow_indexed;         // 1: an indexed draw (the hardware read the index buffer: gl_VertexIndex is the vertex), 0: a draw over the index list that reads the indices here
#endif
};

#ifdef HAS_SKIN
struct ShadowPacket
{
    uvec2 positions;
    uvec2 indices;
    uvec2 weights;
    uvec2 palette;
    uint first;
    uint joints;
    uint model;
    uint pad;
};
#else
struct ShadowPacket
{
    uvec2 positions;
    uvec2 indices;
    uint first;
    uint count;
    uint model;
    uint pad;
};
#endif
layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer ShadowPackets { ShadowPacket p[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer ShadowModels { mat4 m[]; };

void main()
{
    const ShadowPacket pk = ShadowPackets(ll_packet_table).p[gl_InstanceIndex];
#ifdef HAS_SKIN
    uint index = uint(gl_VertexIndex);
    if (shadow_indexed == 0)
    {
        const uint at = pk.first + uint(gl_VertexIndex);
        const uint word = RowWords(pk.indices).w[at >> 1];
        index = (at & 1u) != 0u ? (word >> 16) : (word & 0xFFFFu);
    }
#else
    const uint at = pk.first + uint(gl_VertexIndex);
    const uint word = RowWords(pk.indices).w[at >> 1];
    const uint index = (at & 1u) != 0u ? (word >> 16) : (word & 0xFFFFu);
#endif
    const vec3 position = RowVec4(pk.positions).v[index].xyz;

    vec3 vertex = position;
    vec3 rig_anchor = vec3(0.0);
#ifdef HAS_SKIN
    mat3 skin_rotation;
    vec3 skin_translation;
    skin_vertex(pk.weights, pk.palette, pk.joints, index, skin_rotation, skin_translation);
    vertex = skin_rotation * position + skin_translation;
    rig_anchor = skin_rig_anchor(pk.palette, pk.joints);
#endif
    gl_Position = ll_view_proj * rig_transform(ShadowModels(ll_packet_models).m[pk.model], vertex, rig_anchor);
    // GL clip space z is [-w, w], Vulkan's is [0, w]: the viewer's own vertex shaders get this line from the translator, a native one does it itself (see alpha.vert)
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
}
