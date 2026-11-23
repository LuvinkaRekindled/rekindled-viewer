// light_volume.vert: the box of a light's volume, one instance a light, the light read from the record's table by the instance number (the light's centre, size, colour, falloff, and which of the box's
// eight fans the camera sees it from). The corners are made from the vertex number (the fan of get_box_fan_indices, the same as sOcclusionIndices and the cube of ll_create_cube_vb), nothing is bound but the table.
// A line for line translation of "Deferred Light Pull Shader" and "Deferred SpotLight Pull Shader" (deferred/pointLightV.glsl with RK_LIGHT_PULL, RK_PROJECTOR_PULL); pairs with light_point.frag / light_spot.frag.
// What is different: the vertex shader no longer sends the projector's thirty values as flat varyings (a projector's matrix alone was four locations, 27 in all, for every corner of every box): it sends what every
// light needs (the light and its eye space centre, 2 locations) and the instance, and the projector's fragment shader reads its row itself, after the pixels the volume does not reach are gone.

#include "light_pass.inc"

const uint kFan[64] = uint[64](
    7u, 6u, 2u, 3u, 1u, 5u, 4u, 6u,
    3u, 2u, 0u, 1u, 5u, 7u, 6u, 2u,
    5u, 4u, 6u, 7u, 3u, 1u, 0u, 4u,
    1u, 0u, 4u, 5u, 7u, 3u, 2u, 0u,
    6u, 0u, 2u, 3u, 7u, 5u, 4u, 0u,
    2u, 4u, 0u, 1u, 3u, 7u, 6u, 4u,
    4u, 2u, 6u, 7u, 5u, 1u, 0u, 2u,
    0u, 6u, 4u, 5u, 1u, 3u, 2u, 0u);       // (a fan of 8 corners for each of the 8 sides the camera can be on)

layout(location = 0) flat out vec4 vary_light_a;        // the light's colour (linear), its falloff
layout(location = 1) flat out vec4 vary_light_b;        // the light's centre in eye space, its size
layout(location = 2) flat out uvec2 vary_light_row;     // the instance (the row), classic mode

void main()
{
    LightRows rows = LightRows(rk_light_table);
    const uint at = uint(gl_InstanceIndex) * LIGHT_ROW_STRIDE;
    const vec4 r0 = rows.v[at];
    const vec4 r1 = rows.v[at + 1u];
    const vec4 r2 = rows.v[at + 2u];
    const vec3 center = r0.xyz;
    const float size = r0.w;

    const uint corner = kFan[floatBitsToUint(r2.x) * 8u + uint(gl_VertexIndex)];
    const vec3 position = vec3((corner & 4u) != 0u ? 1.0 : -1.0, (corner & 2u) != 0u ? 1.0 : -1.0, (corner & 1u) != 0u ? 1.0 : -1.0);

    gl_Position = modelview_projection_matrix * vec4(position * size + center, 1.0);
    // (the matrix is GL clip space: the z remap is the translator's line of the viewer's own vertex shaders, see alpha.vert)
    RK_CLIP_Z(gl_Position);

    vary_light_a = vec4(r1.xyz, r1.w);
    vary_light_b = vec4((modelview_matrix * vec4(center, 1.0)).xyz, size);
    vary_light_row = uvec2(uint(gl_InstanceIndex), floatBitsToUint(r2.y));
}
