// alpha_pbr.vert: the vertex shader of the glTF (PBR) blended transparent surfaces, and (PBR_OPAQUE) of the glTF opaque and mask surfaces of the G-buffer pass: the same rows, the same outputs, plus the material's cutoff. A draw is one row of the scene table (alpha_pass.inc: rk_packet_table) and its material is one row of the PBR table
// (rk_packet_pbr: 10 vec4 = the transforms of the four textures, then roughness / metallic / emissive colour, then the cutoff); the vertex reads its own data from the arrays the row names.
// Variants by define: HAS_SKIN + HAS_INDEXED_DRAW (a rigged draw: the hardware reads the indices, the vertex number is the index), ALPHA_PBR_HUD (the fullbright HUD form: only the base colour and the emissive
// texture are used, so no normals or tangents are made).
// GLOW_EMISSIVE (with ALPHA_PBR_HUD: no normals): the glow second draw of a glTF blend surface (pbr_glow.frag): the vertex colour is the emissive array.
// Outputs (alpha_pbr.frag and alpha_pbr_hud.frag read these): eye space position, vertex colour (sRGB), the four texture coordinates, the TBN frame, and flat: the four texture slots and the material factors.

#include "alpha_pass.inc"
#include "lib/rows.glsl"
#include "lib/gltf.glsl"

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer PbrTable { vec4 q[]; };

layout(location = 0) out vec3 vary_position;
layout(location = 1) out vec4 vertex_color;
layout(location = 2) out vec2 vary_tc_base;
layout(location = 5) out vec2 vary_tc_emissive;
layout(location = 9) flat out uvec4 vary_slots;         // base colour, normal, occlusion roughness metallic, emissive
layout(location = 11) flat out vec3 vary_emissive;      // the emissive colour factor (linear)
#if defined(PBR_OPAQUE) || defined(GLOW_EMISSIVE)
layout(location = 12) flat out float vary_cutoff;       // the alpha cutoff of the material (opaque and mask surfaces of the G-buffer pass: pbr_opaque.frag; the glow pass: pbr_glow.frag)
#endif
#ifndef ALPHA_PBR_HUD
layout(location = 3) out vec2 vary_tc_normal;
layout(location = 4) out vec2 vary_tc_orm;
layout(location = 6) out vec3 vary_normal;
layout(location = 7) out vec3 vary_tangent;
layout(location = 8) flat out float vary_sign;
layout(location = 10) flat out vec2 vary_factors;       // roughness, metallic
#endif

#ifdef GB_MOTION
// the clip position of the vertex under the unjittered camera of this frame and of the one before, with the pose of this frame and of the one before (the fragment shader writes the motion, lib/motion.glsl)
layout(location = 14) out vec4 vary_cur;
layout(location = 15) out vec4 vary_prev;
#endif

#if defined(HAS_SKIN) && !defined(HAS_INDEXED_DRAW)
#error "a rigged draw of the alpha family is an indexed one"
#endif

void main()
{
    const Row row = RowTable(rk_packet_table).r[gl_InstanceIndex];
    const ModelBlock model = ModelTable(rk_packet_models).m[row.model];

#ifdef HAS_SKIN
    const uint index = row_clamp_index(row, uint(gl_VertexIndex));
#else
    const uint at = row.first + uint(gl_VertexIndex);
    const uint word = RowWords(row.indices).w[at >> 1];
    const uint index = row_clamp_index(row, (at & 1u) != 0u ? (word >> 16) : (word & 0xFFFFu));
#endif
    const vec4 position = RowVec4(row.positions).v[index];

    // the material row
    PbrTable table = PbrTable(rk_packet_pbr);       // (not const: a buffer reference variable cannot be)
    const uint at_row = row.pad.y * 10u;
    const vec4 base0 = table.q[at_row + 0u];
    const vec4 base1 = table.q[at_row + 1u];
    const vec4 misc0 = table.q[at_row + 8u];
    const vec4 misc1 = table.q[at_row + 9u];

    const mat4 animation = row_texture_matrix(row, TexMatTable(rk_packet_texmats));
    const vec2 texcoord = RowVec2(row.texcoords).t[index];

#ifdef HAS_SKIN
    mat3 skin_rotation;
    vec3 skin_translation;
    row_skin(row, index, skin_rotation, skin_translation);
    const vec3 vertex = skin_rotation * position.xyz + skin_translation;
    const vec3 rig_anchor = row_rig_anchor(row);
#else
    const vec3 vertex = position.xyz;
    const vec3 rig_anchor = vec3(0.0);
#endif
    const vec4 eye = rig_transform(model.mv, vertex, rig_anchor);
#ifdef HAS_SKIN
    gl_Position = rk_packet_proj[0][0] != 0.0 ? rk_packet_proj * eye : rig_transform(model.mvp, vertex, rig_anchor);       // (a rigged vertex: the eye space position first, then the projection: see alpha.vert)
#else
    gl_Position = rig_transform(model.mvp, vertex, rig_anchor);
#endif
    // (the table's matrices are GL clip space: see alpha.vert)
    RK_CLIP_Z(gl_Position);
#ifndef HAS_SKIN
    RK_TIE(gl_Position, row.flags, rk_tie);
#endif
#if defined(GB_MOTION) && !defined(HAS_SKIN)
    {
        // a draw that is not rigged: its slot's model matrix of this frame and of the one before, when the slot moved (else the pixels keep the camera's motion: w = 0 gives the sentinel)
        // (a draw whose record does not set the table, the transient alpha entries among them, has the address 0: no motion, and no read through a null address: that hung the GPU on 2026-11-22)
        MotionTable motion_table = MotionTable(rk_packet_motion);
        mat4 prev_model = mat4(0.0);
        if (rk_packet_motion != uvec2(0u))
        {
            prev_model = motion_table.m[2u * row.model + 1u];
        }
        if (prev_model[3][3] != 0.0)
        {
            vary_cur = view_proj_cur * (motion_table.m[2u * row.model] * vec4(vertex, 1.0));
            vary_prev = view_proj_cur * (prev_model * vec4(vertex, 1.0));       // (see gbuffer.vert and lib/motion.glsl: the last pose under this frame's camera)
        }
        else
        {
            vary_cur = vec4(0.0);
            vary_prev = vec4(0.0);
        }
    }
#endif
#if defined(GB_MOTION) && defined(HAS_SKIN)
    {
        if (skin_palette_moving(row_palette(row), row.joints))
        {
            mat3 prev_rotation;
            vec3 prev_translation;
            skin_vertex_prev(row.weights, row_palette(row), row.joints, index, prev_rotation, prev_translation);
            vary_cur = rig_transform(view_proj_cur, vertex, rig_anchor);
            vary_prev = rig_transform(view_proj_cur, prev_rotation * position.xyz + prev_translation, skin_rig_anchor_prev(row_palette(row), row.joints));       // (see gbuffer.vert)
        }
        else
        {
            vary_cur = vec4(0.0);       // (w = 0: no motion of its own, the camera's motion stays)
            vary_prev = vec4(0.0);
        }
    }
#endif

    vary_position = eye.xyz;
#ifdef GLOW_EMISSIVE
    {
        // the glow pass (PBR Glow Pull Shader, pbrGlowPullV.glsl): the colour is the emissive array: the first byte of the vertex buffer (the positions) plus its offset in the row (pad2), as in alpha.vert
        uint carry;
        uvec2 emissive_address = row.positions;
        emissive_address.x = uaddCarry(emissive_address.x, floatBitsToUint(row.pad2), carry);
        emissive_address.y += carry;
        vertex_color = unpackUnorm4x8(RowWords(emissive_address).w[index]);
    }
#else
    vertex_color = unpackUnorm4x8(RowWords(row.colors).w[index]);
#endif
    vary_tc_base = gltf_texcoord(texcoord, base0, base1, animation);
    vary_tc_emissive = gltf_texcoord(texcoord, table.q[at_row + 6u], table.q[at_row + 7u], animation);
    vary_slots = uvec4(row.slots.x & 0xFFFFu, row.slots.x >> 16, row.slots.y & 0xFFFFu, row.slots.y >> 16);
    vary_emissive = vec3(misc0.z, misc0.w, misc1.x);
#if defined(PBR_OPAQUE) || defined(GLOW_EMISSIVE)
    vary_cutoff = misc1.y;
#endif

#ifndef ALPHA_PBR_HUD
    const vec4 normal_tr0 = table.q[at_row + 2u];
    vary_tc_normal = gltf_texcoord(texcoord, normal_tr0, table.q[at_row + 3u], animation);
    vary_tc_orm = gltf_texcoord(texcoord, table.q[at_row + 4u], table.q[at_row + 5u], animation);
    const vec3 normal = RowVec4(row.normals).v[index].xyz;
    const vec4 tangent = RowVec4(row.tangents).v[index];
#ifdef HAS_SKIN
    const vec3 n = normalize(mat3(model.mv) * (skin_rotation * normal));
    const vec3 t = mat3(model.mv) * (skin_rotation * tangent.xyz);
#else
    const mat3 normal_matrix = mat3(model.n0.xyz, model.n1.xyz, model.n2.xyz);
    const vec3 n = normalize(normal_matrix * normal);
    const vec3 t = normal_matrix * tangent.xyz;
#endif
    const vec4 frame = gltf_tangent(vec4(t, tangent.w), n, normal_tr0, animation);
    vary_normal = n;
    vary_tangent = normalize(frame.xyz);
    vary_sign = frame.w;
    vary_factors = misc0.xy;
#endif
}
