// velocity_object.vert: a moving object's parts for the motion vector pass: the 32 byte entry of the record (the positions and indices by address, the first index, the model matrix number), this frame's model
// matrix and last frame's by that number. A translation of "Object Velocity (packets)" (deferred/objectVelocityPullV.glsl); pairs with velocity.frag (VEL_OBJECT).
// What is different: the clip space z remap the old programs got from the translator is written here (the matrix is GL clip space).

#include "velocity_pass.inc"

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer ObjectPositions { vec4 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer ObjectIndices { uint w[]; };

struct ObjectPacket     // 32 bytes
{
    uvec2 positions;
    uvec2 indices;
    uint first;
    uint count;
    uint model;
    uint pad;
};

layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer ObjectPackets { ObjectPacket p[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer ObjectModels { mat4 m[]; };

layout(location = 0) out vec4 vary_clip;
layout(location = 1) out vec4 vary_cur;
layout(location = 2) out vec4 vary_prev;

void main()
{
    const ObjectPacket pk = ObjectPackets(ll_packet_table).p[gl_InstanceIndex];
    uint index = uint(gl_VertexIndex);
    if (object_indexed == 0)
    {
        const uint i = pk.first + uint(gl_VertexIndex);
        const uint word = ObjectIndices(pk.indices).w[i >> 1];
        index = ((i & 1u) != 0u) ? (word >> 16) : (word & 0xFFFFu);
    }
    const vec4 position = vec4(ObjectPositions(pk.positions).v[index].xyz, 1.0);

    const vec4 world_cur = ObjectModels(ll_packet_models).m[pk.model] * position;
    const vec4 world_prev = ObjectModels(ll_packet_prev_models).m[pk.model] * position;

    vary_clip = ll_view_proj * world_cur;
    vary_cur = view_proj_cur * world_cur;
    vary_prev = view_proj_prev * world_prev;
    gl_Position = vary_clip;
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
}
