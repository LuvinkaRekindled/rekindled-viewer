// rows.glsl: the scene table the alpha vertex shaders read (the worker's 160 byte row of a draw and the 176 byte matrix block of a model), by buffer address, and the pieces of a row a vertex shader needs.
// The layouts are the renderer's (llvkworker.cpp, GeoGpu); everything below the structs is this shader set's own code.
// Needs the uniform addresses of alpha_pass.inc and color-free math only.

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer RowVec4 { vec4 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer RowVec2 { vec2 t[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer RowWords { uint w[]; };

struct Row
{
    uvec2 positions;        // the arrays of the draw, by address
    uvec2 normals;
    uvec2 texcoords;
    uvec2 colors;
    uvec2 indices;
    uvec2 tangents;
    uvec2 texcoords1;
    uvec2 texcoords2;
    uint first;             // first index of the draw
    uint model;             // number of its model matrix block
    uint flags;             // bit 0: one texture for the draw, bit 2: indices are not read from the array, bit 4: a particle row, bit 5: a fullbright draw, bits 6-8: material
    uint mapSlots;
    uvec2 slots;            // four 16 bit texture slots of the draw
    uvec2 pad;              // pad.x: the slot of the draw's texture matrix (0: none)
    vec4 specular;
    float envIntensity;
    float cutoff;
    float brightness;
    float pad2;
    uvec2 weights;          // rigged draws: the weights array and the joint palette
    uvec2 palette;
    uint joints;
    uint skinPad0;
    uint skinPad1;
    uint skinPad2;
};

struct ModelBlock
{
    mat4 mv;                // model view
    mat4 mvp;               // model view projection (Vulkan clip space)
    vec4 n0;                // the normal matrix, by columns
    vec4 n1;
    vec4 n2;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer RowTable { Row r[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer ModelTable { ModelBlock m[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer TexMatTable { mat4 t[]; };

// the texture slot a vertex uses: one for the whole draw, or one of four picked by the low two bits of the batched texture index in the position's w
uint row_texture_slot(Row row, float position_w)
{
    const uint pick = (row.flags & 1u) != 0u ? 0u : (floatBitsToUint(position_w) & 3u);
    const uint pair = pick < 2u ? row.slots.x : row.slots.y;
    return (pair >> ((pick & 1u) * 16u)) & 0xFFFFu;
}

// ---- texture animation: a texture matrix slot may hold the PARAMETERS of an animation (a 0 in its last float) and the clock sits in slot 0 (llvktexanim.h is the CPU twin of this function) ----
mat4 texanim_matrix(mat4 m, vec2 clock)
{
    const uint bits = floatBitsToUint(m[0][0]);
    const uint mode = bits & 0xFFu;
    const float size_x = float((bits >> 8) & 0xFFu);
    const float size_y = float((bits >> 16) & 0xFFu);
    const float rate = m[0][1];
    const float start = m[0][2];
    const float len = m[0][3];
    const float elapsed = max((clock.x - m[1][0]) + (clock.y - m[1][1]), 0.0);
    const float num_frames = len != 0.0 ? len : max(1.0, size_x * size_y);
    float full_length = num_frames;
    if ((mode & 8u) != 0u)                          // ping pong
    {
        if ((mode & 16u) != 0u)
        {
            full_length = 2.0 * num_frames;
        }
        else if ((mode & 2u) != 0u)
        {
            full_length = max(1.0, 2.0 * num_frames - 2.0);
        }
        else
        {
            full_length = max(1.0, 2.0 * num_frames - 1.0);
        }
    }
    float frame = elapsed * rate;
    if ((mode & 2u) != 0u)                          // loop
    {
        frame -= full_length * trunc(frame / full_length);
    }
    else
    {
        frame = min(full_length - 1.0, frame);
    }
    if ((mode & 16u) == 0u)                         // not smooth: whole frames
    {
        frame = min(full_length - 1.0, floor(frame + 0.01));
    }
    if ((mode & 8u) != 0u && frame >= num_frames)
    {
        frame = (mode & 16u) != 0u ? num_frames - (frame - num_frames) : (num_frames - 1.99) - (frame - num_frames);
    }
    if ((mode & 4u) != 0u)                          // reverse
    {
        frame = (mode & 16u) != 0u ? num_frames - frame : (num_frames - 0.99) - frame;
    }
    frame += start;
    if ((mode & 16u) == 0u)
    {
        frame = floor(frame + 0.5);
    }
    float off_s = m[1][2];
    float off_t = m[1][3];
    float scale_s = m[2][0];
    float scale_t = m[2][1];
    float rot = m[2][2];
    if ((mode & 32u) != 0u)                         // rotate
    {
        rot = frame;
    }
    else if ((mode & 64u) != 0u)                    // scale
    {
        scale_s = frame;
        scale_t = frame;
    }
    else if (size_x != 0.0 && size_y != 0.0)        // a grid of frames
    {
        scale_s = 1.0 / size_x;
        scale_t = 1.0 / size_y;
        const float x_frame = frame - size_x * trunc(frame / size_x);
        const float y_frame = float(int(frame / size_x));
        off_s = (-0.5 + 0.5 * scale_s) + x_frame * scale_s;
        off_t = (0.5 - 0.5 * scale_t) - y_frame * scale_t;
    }
    else                                            // scrolling
    {
        off_s = frame;
        off_t = 0.0;
    }
    const float c = cos(rot);
    const float s = sin(rot) * m[2][3];
    return mat4(vec4(c * scale_s, -s * scale_t, 0.0, 0.0),
                vec4(s * scale_s, c * scale_t, 0.0, 0.0),
                vec4(0.0, 0.0, 1.0, 0.0),
                vec4(-0.5 * (c + s) * scale_s + off_s + 0.5, -0.5 * (-s + c) * scale_t + off_t + 0.5, 0.0, 1.0));
}

// the texture matrix of a row (identity for none)
mat4 row_texture_matrix(Row row, TexMatTable table)
{
    if (row.pad.x == 0u)
    {
        return mat4(1.0);
    }
    const mat4 m = table.t[row.pad.x];
    return m[3][3] == 0.0 ? texanim_matrix(m, table.t[0][0].yz) : m;
}

#ifdef HAS_SKIN
// ---- a rigged draw: four joints a vertex out of the draw's palette (three rows of a 3x4 matrix a joint); the weights array holds joint number + weight in one float (the integer part is the joint) ----
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer SkinVertexWeights { vec4 w[]; };
struct Joint { vec4 c0; vec4 c1; vec4 c2; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer SkinJoints { Joint j[]; };

void row_skin(Row row, uint index, out mat3 rotation, out vec3 translation)
{
    const vec4 joint_and_weight = SkinVertexWeights(row.weights).w[index];
    vec4 weight = fract(joint_and_weight);
    const vec4 joint = clamp(floor(joint_and_weight), vec4(0.0), vec4(float(row.joints - 1u)));
    weight /= weight.x + weight.y + weight.z + weight.w;
    SkinJoints palette = SkinJoints(row.palette);
    rotation = mat3(0.0);
    translation = vec3(0.0);
    for (int k = 0; k < 4; ++k)
    {
        const Joint j = palette.j[int(joint[k])];
        rotation += mat3(j.c0.xyz, j.c1.xyz, j.c2.xyz) * weight[k];
        translation += vec3(j.c0.w, j.c1.w, j.c2.w) * weight[k];
    }
}
#else
// ---- a particle row (flags bit 4): one row a particle in the arrays (positions: the centre in agent space, w > 0.5: a ribbon piece; normals: the velocity of a follow-velocity particle or the
// half axis of a ribbon piece; texcoords: half width and height) and six vertices a particle; the quad is made here, facing the camera ----
vec3 particle_billboard(ModelBlock m, vec3 center, vec3 velocity, vec2 half_size, uint corner)
{
    const mat3 rot = mat3(m.mv);
    const vec3 at = rot * center + m.mv[3].xyz;                 // from the camera to the particle
    const vec3 side = cross(at, m.mv[2].xyz);                    // (mv[2].xyz: the agent's up axis in eye space)
    const float side_len = length(side);
    vec3 right = side_len > 1e-8 ? side / side_len : rot * vec3(0.0, 1.0, 0.0);
    const vec3 up_raw = cross(right, at);
    const float up_len = length(up_raw);
    vec3 up = up_len > 1e-8 ? up_raw / up_len : m.mv[2].xyz;
    if (dot(velocity, velocity) > 0.0)
    {
        const vec3 v = rot * velocity;
        vec2 f = vec2(dot(v, right), dot(v, up));
        const float fl = length(f);
        if (fl > 1e-6)
        {
            f /= fl;
            const vec3 new_up = f.x * right + f.y * up;
            const vec3 new_right = f.y * right - f.x * up;
            up = normalize(new_up);
            right = normalize(new_right);
        }
    }
    const float sx = (corner & 2u) != 0u ? 1.0 : -1.0;
    const float sy = (corner & 1u) != 0u ? -1.0 : 1.0;
    return center + (right * (sx * half_size.x) + up * (sy * half_size.y)) * rot;      // (back to agent space: the transpose of the rotation)
}

// the corner of a particle quad for this vertex: its position in agent space; 'row' becomes the row whose colour and glow the vertex uses
vec3 particle_corner(Row pk, ModelBlock m, out uint row, out uint corner)
{
    const uint at = pk.first + uint(gl_VertexIndex);
    const uint n = at / 6u;
    const uint c = at - n * 6u;
    corner = c == 0u ? 0u : ((c == 1u || c == 3u) ? 1u : ((c == 2u || c == 5u) ? 2u : 3u));      // the corners of the two triangles of the quad, in the old quad's order
    row = n;
    const vec4 center4 = RowVec4(pk.positions).v[n];
    if (center4.w > 0.5)
    {
        if (center4.w > 1.5)
        {
            return center4.xyz;                                   // the second row of a ribbon piece draws a degenerate piece
        }
        const bool joined = corner < 2u;
        const vec4 other = RowVec4(pk.positions).v[n + 1u];
        const vec3 axis = RowVec4(pk.normals).v[joined ? n + 1u : n].xyz;
        const float sign_along = (corner & 1u) == 0u ? 1.0 : -1.0;
        if (joined)
        {
            row = n + 1u;
        }
        return (joined ? other.xyz : center4.xyz) + axis * sign_along;
    }
    return particle_billboard(m, center4.xyz, RowVec4(pk.normals).v[n].xyz, RowVec2(pk.texcoords).t[n], corner);
}
#endif
