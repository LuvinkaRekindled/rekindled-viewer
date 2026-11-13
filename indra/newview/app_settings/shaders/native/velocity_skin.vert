// velocity_skin.vert: a rigged part for the motion vector pass: the 48 byte entry of the record (the positions, indices and weights by address, this frame's joint palette and last frame's, the first index, the
// number of joints), skinned twice and projected twice. A translation of "Avatar Velocity (rigged, packets)" (deferred/avatarVelocityPullV.glsl); pairs with velocity.frag (VEL_SKIN, HAS_SKIN).
// What is different: the weights of a vertex are read once for both palettes (the old program read them once each way and made the blend twice); the skin loop is the one of lib/rows.glsl's skin_vertex; the
// anchor of an avatar relative palette goes through rig_transform (lib/rows.glsl); and the clip space z remap the old program got from the translator is written here.

#include "velocity_pass.inc"
#include "lib/rows.glsl"

struct VelocityPacket   // 48 bytes
{
    uvec2 positions;
    uvec2 indices;
    uvec2 weights;
    uvec2 palette;
    uvec2 prevPalette;
    uint first;
    uint joints;
};

layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer VelocityPackets { VelocityPacket p[]; };

layout(location = 0) out vec4 vary_clip;
layout(location = 1) out vec4 vary_cur;
layout(location = 2) out vec4 vary_prev;

void main()
{
    const VelocityPacket pk = VelocityPackets(ll_packet_table).p[gl_InstanceIndex];
    uint index = uint(gl_VertexIndex);
    if (vel_indexed == 0)
    {
        const uint i = pk.first + uint(gl_VertexIndex);
        const uint word = RowWords(pk.indices).w[i >> 1];
        index = ((i & 1u) != 0u) ? (word >> 16) : (word & 0xFFFFu);
    }
    const vec3 position = RowVec4(pk.positions).v[index].xyz;

    // the blend of the vertex: four joints, the integer part of a float names the joint and the fraction is its weight
    const vec4 joint_and_weight = SkinVertexWeights(pk.weights).w[index];
    vec4 weight = fract(joint_and_weight);
    const vec4 joint = clamp(floor(joint_and_weight), vec4(0.0), vec4(float(pk.joints - 1u)));
    weight /= weight.x + weight.y + weight.z + weight.w;

    SkinJoints palette = SkinJoints(pk.palette);              // (not const: a buffer reference variable cannot be)
    SkinJoints prev_palette = SkinJoints(pk.prevPalette);
    vec3 world_cur = vec3(0.0);
    vec3 world_prev = vec3(0.0);
    for (int k = 0; k < 4; ++k)
    {
        if (weight[k] > 0.0)        // (a joint with no weight is not read: most vertices have one to three, and a zero weight adds nothing)
        {
            const Joint a = palette.j[int(joint[k])];
            world_cur += (mat3(a.c0.xyz, a.c1.xyz, a.c2.xyz) * position + vec3(a.c0.w, a.c1.w, a.c2.w)) * weight[k];
            const Joint b = prev_palette.j[int(joint[k])];
            world_prev += (mat3(b.c0.xyz, b.c1.xyz, b.c2.xyz) * position + vec3(b.c0.w, b.c1.w, b.c2.w)) * weight[k];
        }
    }

    // avatar relative skinning: each palette names its own anchor in its tail entry (the one of last frame may differ, it was made then); zero: plain world matrices
    vary_clip = rig_transform(ll_view_proj, world_cur, skin_rig_anchor(pk.palette, pk.joints));
    vary_cur = rig_transform(view_proj_cur, world_cur, skin_rig_anchor(pk.palette, pk.joints));
    vary_prev = rig_transform(view_proj_prev, world_prev, skin_rig_anchor(pk.prevPalette, pk.joints));
    gl_Position = vary_clip;
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
}
