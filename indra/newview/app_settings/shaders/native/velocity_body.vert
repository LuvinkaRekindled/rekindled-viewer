// velocity_body.vert: the system avatar's body parts (skin, hair, eyes, eyelashes: LLViewerJointMesh) for the motion vector pass, drawn like the G-buffer pass draws them (the packet path, ll_pull_draw_range: the row
// names the mesh's arrays) with the world space palettes of this frame and of the last (15 joints, three rows each, the weight of a vertex is one float: the joint in the integer part, the blend with the next one
// in the fraction). A translation of "Avatar Velocity (body) pull" (deferred/avatarBodyVelocityV.glsl through the pull twin); pairs with velocity.frag (VEL_BODY).
// What is different: the clip space z remap is written here; the weight is read once for both palettes.

#include "velocity_pass.inc"
#include "lib/rows.glsl"

layout(std140, set = 0, binding = 8) uniform LLLooseSkin
{
    vec4 matrixPalette[45];             // joint i: rows at i, i + 15 and i + 30 (world space)
    vec4 matrixPalettePrev[45];         // last frame's
};

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer RowFloats { float f[]; };

layout(location = 0) out vec4 vary_clip;
layout(location = 1) out vec4 vary_cur;
layout(location = 2) out vec4 vary_prev;

void main()
{
    const Row row = RowTable(ll_packet_table).r[gl_InstanceIndex];
    const uint at = row.first + uint(gl_VertexIndex);
    uint index = at;
    if ((row.flags & 4u) == 0u)
    {
        const uint word = RowWords(row.indices).w[at >> 1];
        index = (at & 1u) != 0u ? (word >> 16) : (word & 0xFFFFu);
    }
    const vec4 p = vec4(RowVec4(row.positions).v[index].xyz, 1.0);

    // a NaN or out of range weight reads joint 0 (the vertex data is rewritten every update); joint 14 blends with itself
    float wx = (row.weights.x != 0u || row.weights.y != 0u) ? RowFloats(row.weights).f[index] : 0.0;
    if (!(wx >= 0.0 && wx <= 14.0))
    {
        wx = 0.0;
    }
    const int i = int(floor(wx));
    const int n = min(i + 1, 14);
    const float x = fract(wx);

    // (the second joint of the pair is not read when the weight is zero: most body vertices belong to one joint)
    vec4 c0 = matrixPalette[i];
    vec4 c1 = matrixPalette[i + 15];
    vec4 c2 = matrixPalette[i + 30];
    vec4 p0 = matrixPalettePrev[i];
    vec4 p1 = matrixPalettePrev[i + 15];
    vec4 p2 = matrixPalettePrev[i + 30];
    if (x > 0.0)
    {
        c0 = mix(c0, matrixPalette[n], x);
        c1 = mix(c1, matrixPalette[n + 15], x);
        c2 = mix(c2, matrixPalette[n + 30], x);
        p0 = mix(p0, matrixPalettePrev[n], x);
        p1 = mix(p1, matrixPalettePrev[n + 15], x);
        p2 = mix(p2, matrixPalettePrev[n + 30], x);
    }
    const vec3 world_cur = vec3(dot(c0, p), dot(c1, p), dot(c2, p));
    const vec3 world_prev = vec3(dot(p0, p), dot(p1, p), dot(p2, p));

    vary_clip = projection_matrix * (modelview_matrix * vec4(world_cur, 1.0));
    vary_cur = view_proj_cur * vec4(world_cur, 1.0);
    vary_prev = view_proj_prev * vec4(world_prev, 1.0);
    gl_Position = vary_clip;
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
}
