// rk_native.glsl: the prelude of every native program (see llrender/rkvknative.h for the whole contract).
// Vulkan GLSL 460, explicit layouts, no GL-era names. The loader puts `#version 460`, the include extension, the stage macro (RK_VERTEX_STAGE or RK_FRAGMENT_STAGE)
// and the manifest's defines in front of this file.

#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

// the bindless texture tables of the frame (set 2) and the map from a texture id to its descriptor index:  texture(rk_tbl2d[RK_HM(rk_ti_name)], uv)
layout(set = 2, binding = 0) uniform sampler2D rk_tbl2d[];
layout(set = 2, binding = 1) uniform samplerCube rk_tblcube[];
layout(set = 2, binding = 2) uniform samplerCubeArray rk_tblcubearray[];
layout(set = 2, binding = 3) uniform sampler2DShadow rk_tbl2dshadow[];
layout(std430, set = 2, binding = 4) readonly buffer RkHandleMap { uint rk_hm[]; };
#define RK_HM(id) rk_hm[(id)]

// ---- the depth convention (PLAN-reversed-z.md) ----
// RK_REVERSE_Z 0 (today): the depth buffer holds 0.0 at the near plane and 1.0 at the far plane and the sky. 1: reversed, 1.0 near, 0.0 far and sky, and the projection is built directly in Vulkan form.
// Shader code does not compare against 1.0 or remap depth by itself: it asks these. They are MACROS, not functions, so that with RK_REVERSE_Z 0 the compiler sees exactly the text they replaced
// (spv-equiv.py proves that the compiled code is the same).
#ifndef RK_REVERSE_Z
#define RK_REVERSE_Z 0
#endif
#if RK_REVERSE_Z != 0
#define rk_depth_ndc(d) (d)                                  // the z of the normalised device coordinate that goes with a stored depth, for the inverse projection of the main view
#define rk_depth_far() 0.0                                   // the value the depth buffer holds for the far plane and the sky
#define rk_depth_is_far(d) ((d) <= 0.0)                      // a pixel with no surface (sky, clouds, sun, moon, stars)
#define rk_depth_has_surface(d) ((d) > 0.0)                  // the opposite
#define rk_depth_in_front(a, b, eps) ((a) > (b) + (eps))     // a is nearer to the camera than b by more than eps (in stored depth units)
#define rk_depth_behind(a, b) ((a) < (b))                    // a is farther from the camera than b
#define rk_depth_nearest(a, b) max(a, b)
#define rk_depth_farthest(a, b) min(a, b)
#define rk_clip_depth(clip) ((clip).z / (clip).w)            // the stored depth of a vertex output position (clip space, as the vertex stage wrote gl_Position)
#else
#define rk_depth_ndc(d) (2.0 * (d) - 1.0)
#define rk_depth_far() 1.0
#define rk_depth_is_far(d) ((d) >= 1.0)
#define rk_depth_has_surface(d) ((d) < 1.0)
#define rk_depth_in_front(a, b, eps) ((a) < (b) - (eps))
#define rk_depth_behind(a, b) ((a) > (b))
#define rk_depth_nearest(a, b) min(a, b)
#define rk_depth_farthest(a, b) max(a, b)
#define rk_clip_depth(clip) ((clip).z / (clip).w * 0.5 + 0.5)
#endif
