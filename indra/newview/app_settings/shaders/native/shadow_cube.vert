// shadow_cube.vert: the box of the shadow of a cube snapshot's light volume (Deferred Shadow Cube Shader): a unit box vertex (the position attribute) scaled and moved by the box's size and centre, through the
// model view projection matrix of the viewer's matrix stack (GL clip space: the z remap below is the translator's line of the viewer's own vertex shaders, see alpha.vert).

layout(std140, set = 0, binding = 0) uniform LLLooseUniforms
{
    mat4 modelview_projection_matrix;
    vec3 box_center;
    vec3 box_size;
};

layout(location = 0) in vec3 position;

void main()
{
    gl_Position = modelview_projection_matrix * vec4(position * box_size + box_center, 1.0);
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
}
