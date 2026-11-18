// avatar.vert: the system avatar (the legacy body: skin, hair, eyes, eyelashes of LLViewerJointMesh) in the G-buffer pass, from the geometry packet path (the draw of one joint mesh range, rk_pull_draw_range): the
// row names the mesh's arrays; the vertex is skinned in eye space by the joint palette uniform (matrixPalette, 15 joints, three rows each) with the weight of the vertex (the joint in the integer part, the blend
// with the next one in the fraction), then projected. Stands in for "Deferred Avatar Shader pull" (deferred/avatarV.glsl with LL_VERTEX_PULL, avatar/avatarSkinV.glsl). Defines: AVATAR_PASS, AVATAR_CLOTH (the wind of
// the clothing, only for the old program that has the wind uniforms).

#include "alpha_pass.inc"
#include "lib/rows.glsl"


layout(std140, set = 0, binding = 8) uniform RKLooseSkin
{
    vec4 matrixPalette[45];             // joint i: rows at i, i + 15 and i + 30
#ifdef GB_MOTION
    vec4 matrixPaletteWorld[45];        // velocity fold: the same palette in world space (this frame) and last frame's (the viewer sets both, LLViewerJointMesh::uploadJointMatrices)
    vec4 matrixPalettePrev[45];
#endif
};

#include "lib/avatarskin.glsl"

layout(location = 0) out vec3 vary_position;
layout(location = 1) out vec2 vary_texcoord0;
layout(location = 2) out vec3 vary_normal;
#ifdef GB_MOTION
// the clip position of the vertex under the unjittered camera of this frame and of the one before (the fragment shader writes the motion, lib/motion.glsl)
layout(location = 14) out vec4 vary_cur;
layout(location = 15) out vec4 vary_prev;
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
    const vec3 position = RowVec4(row.positions).v[index].xyz;
    const vec3 normal = RowVec4(row.normals).v[index].xyz;
    vary_texcoord0 = RowVec2(row.texcoords).t[index];

    // the skinning transform (lib/avatarskin.glsl, shared with avatar_shadow.vert)
    vec4 t0, t1, t2;
    avatar_skin(row, index, t0, t1, t2);

    vec3 norm = normalize(vec3(dot(t0.xyz, normal), dot(t1.xyz, normal), dot(t2.xyz, normal)));
    const vec4 pos_in = vec4(position, 1.0);
    vec4 pos;

#ifdef AVATAR_CLOTH
    // the clothing array travels in the tangent slot (the old pull program does the same); a mesh without it has the tangents address equal to the positions'
    const vec4 clothing = all(equal(row.tangents, row.positions)) ? vec4(0.0) : RowVec4(row.tangents).v[index];
    // wind
    vec4 windEffect = vec4(dot(norm, gWindDir.xyz));
    pos.x = dot(t2, pos_in);
    windEffect.xyz = pos.x * vec3(0.015, 0.015, 0.015) + windEffect.xyz;
    windEffect.w = windEffect.w * 2.0 + 1.0;                // move wind offset value to [-1, 3]
    windEffect.w = windEffect.w * gWindDir.w;               // modulate wind strength
    windEffect.xyz = windEffect.xyz * gSinWaveParams.xyz + vec3(gSinWaveParams.w);

    // reduce to period of 2 PI
    const vec4 gMinMaxConstants = vec4(1.0, 0.166666, 0.0083143, .00018542);     // minimax coefficients
    const vec4 gPiConstants = vec4(0.159154943, 6.28318530, 3.141592653, 1.5707963);   // 1/2PI, 2PI, PI, PI/2
    vec4 temp1 = vec4(0.0), temp0 = vec4(0.0), temp2, offsetPos;
    temp1.xyz = windEffect.xyz * gPiConstants.x;            // change input as multiple of [0-2PI] to [0-1]
    temp0.y = mod(temp1.x, 1.0);
    windEffect.x = temp0.y * gPiConstants.y;                // scale from [0,1] to [0, 2PI]
    temp1.z = temp1.z - gPiConstants.w;                     // shift normal oscillation by PI/2
    temp0.y = mod(temp1.z, 1.0);
    windEffect.z = temp0.y * gPiConstants.y;
    windEffect.xyz = windEffect.xyz + vec3(-3.141592);      // offset to [-PI, PI]

    // calculate sinusoid
    vec4 sinWave;
    temp1 = windEffect * windEffect;
    sinWave = -temp1 * gMinMaxConstants.w + vec4(gMinMaxConstants.z);
    sinWave = sinWave * -temp1 + vec4(gMinMaxConstants.y);
    sinWave = sinWave * -temp1 + vec4(gMinMaxConstants.x);
    sinWave = sinWave * windEffect;
    sinWave.xyz = sinWave.xyz * gWindDir.w + vec3(windEffect.w);

    // normal facing bias offset
    temp1 = vec4(dot(norm, gGravity.xyz));
    temp1 = min(temp1, vec4(0.2, 0.0, 0.0, 0.0));
    temp1 = temp1 * vec4(1.5, 0.0, 0.0, 0.0);
    sinWave.x = sinWave.x + temp1.x;
    sinWave.xyz = sinWave.xyz * clothing.w;                 // modulate by clothing coverage
    sinWave.xyz = max(sinWave.xyz, vec3(-1.0, -1.0, -1.0)); // clamp to underlying body shape
    offsetPos = clothing * sinWave.x;
    temp2 = gWindDir * sinWave.z + vec4(norm, 0.0);
    offsetPos = vec4(1.0, 1.0, 1.0, 0.0) * offsetPos + pos_in;
    norm += temp2.xyz * 2.0;
    norm = normalize(norm);

    pos.x = dot(t0, offsetPos);
    pos.y = dot(t1, offsetPos);
    pos.z = dot(t2, offsetPos);
    pos.w = 1.0;
#else
    pos.x = dot(t0, pos_in);
    pos.y = dot(t1, pos_in);
    pos.z = dot(t2, pos_in);
    pos.w = 1.0;
#endif

    vary_normal = norm;
    vary_position = pos.xyz;
    gl_Position = projection_matrix * pos;
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;      // (GL clip space to Vulkan's, as the old programs get it appended)
#ifdef GB_MOTION
    {
        // the motion of the vertex: the world space palettes of this frame and of the last (the avatar velocity pass did the same in its own pass until 2026-11-15); the wind of the clothing is left out, as there
        float wx = (row.weights.x != 0u || row.weights.y != 0u) ? RowFloats(row.weights).f[index] : 0.0;
        if (!(wx >= 0.0 && wx <= 14.0))
        {
            wx = 0.0;
        }
        const int i = int(floor(wx));
        const int n = min(i + 1, 14);
        const float x = fract(wx);
        vec4 c0 = matrixPaletteWorld[i], c1 = matrixPaletteWorld[i + 15], c2 = matrixPaletteWorld[i + 30];
        vec4 p0 = matrixPalettePrev[i], p1 = matrixPalettePrev[i + 15], p2 = matrixPalettePrev[i + 30];
        if (x > 0.0)
        {
            c0 = mix(c0, matrixPaletteWorld[n], x);
            c1 = mix(c1, matrixPaletteWorld[n + 15], x);
            c2 = mix(c2, matrixPaletteWorld[n + 30], x);
            p0 = mix(p0, matrixPalettePrev[n], x);
            p1 = mix(p1, matrixPalettePrev[n + 15], x);
            p2 = mix(p2, matrixPalettePrev[n + 30], x);
        }
        vary_cur = view_proj_cur * vec4(dot(c0, pos_in), dot(c1, pos_in), dot(c2, pos_in), 1.0);
        vary_prev = view_proj_prev * vec4(dot(p0, pos_in), dot(p1, pos_in), dot(p2, pos_in), 1.0);
    }
#endif
}
