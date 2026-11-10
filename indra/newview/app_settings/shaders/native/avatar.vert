// avatar.vert: the system avatar (the legacy body: skin, hair, eyes, eyelashes of LLViewerJointMesh) in the G-buffer pass, from the geometry packet path (the draw of one joint mesh range, ll_pull_draw_range): the
// row names the mesh's arrays; the vertex is skinned in eye space by the joint palette uniform (matrixPalette, 15 joints, three rows each) with the weight of the vertex (the joint in the integer part, the blend
// with the next one in the fraction), then projected. Stands in for "Deferred Avatar Shader pull" (deferred/avatarV.glsl with LL_VERTEX_PULL, avatar/avatarSkinV.glsl). Defines: AVATAR_PASS, AVATAR_CLOTH (the wind of
// the clothing, only for the old program that has the wind uniforms).

#include "alpha_pass.inc"
#include "lib/rows.glsl"

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer RowFloats { float f[]; };

layout(std140, set = 0, binding = 8) uniform LLLooseSkin
{
    vec4 matrixPalette[45];             // joint i: rows at i, i + 15 and i + 30
};

layout(location = 0) out vec3 vary_position;
layout(location = 1) out vec2 vary_texcoord0;
layout(location = 2) out vec3 vary_normal;

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
    const vec3 position = RowVec4(row.positions).v[index].xyz;
    const vec3 normal = RowVec4(row.normals).v[index].xyz;
    vary_texcoord0 = RowVec2(row.texcoords).t[index];

    // the skinning transform: a NaN or out of range weight reads joint 0 (the vertex data is rewritten every update); joint 14 blends with itself
    float wx = (row.weights.x != 0u || row.weights.y != 0u) ? RowFloats(row.weights).f[index] : 0.0;
    if (!(wx >= 0.0 && wx <= 14.0))
    {
        wx = 0.0;
    }
    const int i = int(floor(wx));
    const int n = min(i + 1, 14);
    const float x = fract(wx);
    vec4 t0 = matrixPalette[i];
    vec4 t1 = matrixPalette[i + 15];
    vec4 t2 = matrixPalette[i + 30];
    if (x > 0.0)
    {
        t0 = mix(t0, matrixPalette[n], x);
        t1 = mix(t1, matrixPalette[n + 15], x);
        t2 = mix(t2, matrixPalette[n + 30], x);
    }

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
}
