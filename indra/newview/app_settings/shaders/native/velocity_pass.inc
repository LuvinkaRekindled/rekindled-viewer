// velocity_pass.inc: the loose uniform blocks of the native motion vector programs (velocity_object.vert, velocity_skin.vert, velocity_body.vert and velocity.frag include this file, so both stages declare the
// same blocks). Every name and type is the one the viewer's velocity programs use. Defines of the manifest lines: VEL_OBJECT (moving objects: model matrices of this frame and the last), VEL_SKIN (rigged parts:
// the palette of this frame and the last), VEL_BODY (the system avatar's body parts: world palettes in the skin block).
// What the pass writes: the screen space motion of the surface in pixels (rg), where the depth of the surface is what the G-buffer holds (velocity.frag throws away what is behind or in front of it).

layout(std140, set = 0, binding = 0) uniform LLLooseUniforms
{
    uvec2 ll_packet_table;
#ifdef VEL_OBJECT
    uvec2 ll_packet_models;         // this frame's model matrices
    uvec2 ll_packet_prev_models;    // last frame's, same numbers
    int object_indexed;             // 1: an indexed draw (the hardware read the index buffer: gl_VertexIndex is the vertex), 0: a draw over the index list that reads the indices here
#endif
#ifdef VEL_SKIN
    int vel_indexed;                // the same for the rigged parts
#endif
#ifdef VEL_BODY
    mat4 modelview_matrix;
    mat4 projection_matrix;
#else
    mat4 ll_view_proj;              // jittered projection * view
#endif
    mat4 view_proj_cur;             // unjittered projection * view, this frame
    mat4 view_proj_prev;            // the same, last frame
    vec2 render_res;
    vec2 proj_z;                    // jittered projection [2][2] and [3][2]: the distance of a depth buffer value is proj_z.y / (ndc_z + proj_z.x)
    float depth_tolerance;          // relative
};

layout(std140, set = 0, binding = 5) uniform LLLooseDraw
{
    uint ll_ti_depthMap;
};
