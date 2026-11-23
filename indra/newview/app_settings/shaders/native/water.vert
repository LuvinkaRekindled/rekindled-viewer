// water.vert: the water plane. Where the vertex is, the three coordinates of the two wave maps (a big wave and two small ones, moving with the time), the vector from the camera, and the clip position the
// picture below the water is looked up by. A translation of the water vertex shader (class 1 environment/waterV.glsl); pairs with water.frag, and with underwater.frag (UNDERWATER). What is different: the
// plane's frame (normal, tangent) and the light direction in eye space are made in the fragment shader from the normal matrix (they are the same for every vertex: three varyings less), and the old
// shader's atmospherics call (its outputs were read by nothing in the class 3 water) is gone.

#include "water_pass.inc"

// WATER_PULL (the table form of the planes): the position is read from the record's rows, the rest of the program is the same
#ifdef WATER_PULL
#include "lib/rows.glsl"
#else
layout(location = 0) in vec3 position;
#endif

layout(location = 0) out vec4 refCoord;         // xyz: the clip position (z pushed by 0.2, the old shader's way of making it a w), w: the big wave's x
layout(location = 1) out vec4 littleWave;      // the two small waves' coordinates
layout(location = 2) out vec4 view;             // xyz: from the camera to the vertex, w: the big wave's y
layout(location = 3) out vec3 vary_position;

void main()
{
#ifdef WATER_PULL
    const Row row = RowTable(rk_packet_table).r[gl_InstanceIndex];
    const uint at = row.first + uint(gl_VertexIndex);
    const uint word = RowWords(row.indices).w[at >> 1];
    const uint index = (at & 1u) != 0u ? (word >> 16) : (word & 0xFFFFu);
    const vec3 position = RowVec4(row.positions).v[index].xyz;
#endif
    const vec4 pos = vec4(position, 1.0);
    vary_position = (modelview_matrix * pos).xyz;

    // far water is pulled in to 2560 m of the camera for the wave coordinates (the waves do not shrink to nothing at the horizon)
    const vec3 oEyeVec = position - eyeVec;
    const float d = length(oEyeVec.xy);
    const float ld = min(d, 2560.0);
    vec3 v = vec3(eyeVec.xy + oEyeVec.xy / d * ld, position.z);
    v.x += (cos(v.x * 0.08) + sin(v.y * 0.02)) * 6.0;

    const vec2 bigWave = v.xy * vec2(0.04, 0.04) + waveDir1 * time * 0.055;
    littleWave.xy = v.xy * vec2(0.45, 0.9) + waveDir2 * time * 0.13;
    littleWave.zw = v.xy * vec2(0.1, 0.2) + waveDir1 * time * 0.1;
    view = vec4(oEyeVec, bigWave.y);

    const vec4 clip = modelview_projection_matrix * pos;
    refCoord = vec4(clip.xy, RK_GL_CLIP_Z(clip) + 0.2, bigWave.x);      // (the GL clip z the fragment stage divides by, not the reversed one)
    gl_Position = clip;
    RK_CLIP_Z_OF(gl_Position, clip);        // (GL clip space to Vulkan's, as the old programs get it appended)
}
