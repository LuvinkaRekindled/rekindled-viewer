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
#define rk_depth_recip(d) (d)                                // about 1 / distance (up to a constant and the far plane term): a stored depth is that in the reversed convention
#define rk_depth_span(far_d, near_d) ((near_d) - (far_d))    // how much farther the far depth is than the near one, as a positive number
#define rk_depth_push_far(d, x, y) max(0.0, (d) - (x) - (y)) // a depth moved farther by two margins, not past the far plane
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
#define rk_depth_recip(d) (1.0 - (d))
#define rk_depth_span(far_d, near_d) ((far_d) - (near_d))
#define rk_depth_push_far(d, x, y) min(1.0, (d) + (x) + (y))
#endif

// ---- the clip z of a vertex (PLAN-reversed-z.md) ----
// The projection of the viewer is the GL one (z of the clip space between -w and w); Vulkan wants 0 to w. RK_CLIP_Z_OF(dst, src) writes the z of dst from src: with RK_REVERSE_Z 0 the old remap,
// exactly the text it replaces. With RK_REVERSE_Z 1 the projection the program gets is already the reversed Vulkan one (LLRender gives it for everything but the shadow passes), so the z is taken as it is.
// The programs that draw into the shadow maps (shadow.vert, shadow_cube.vert, shadow_alpha.vert, avatar_shadow.vert) keep the standard convention: RK_CLIP_Z_STD, the remap in every case.
#define RK_CLIP_Z_STD_OF(dst, src) (dst).z = ((src).z + (src).w) * 0.5
#if RK_REVERSE_Z != 0
#define RK_CLIP_Z_OF(dst, src) (dst).z = (src).z
#else
#define RK_CLIP_Z_OF(dst, src) RK_CLIP_Z_STD_OF(dst, src)
#endif
#define RK_CLIP_Z(p) RK_CLIP_Z_OF(p, p)
// a vertex pinned to the far plane (the stars, the moon and the sun disc: the GL clip z is w times a factor just under 1): in the reversed convention the far plane is 0 and the same factor
// gives a depth just above it. RK_FAR_CLIP_Z(w) is exactly the far plane, RK_FAR_CLIP_Z_K(w, k) is w * k in the standard convention and the same distance from the far plane in the reversed one.
#if RK_REVERSE_Z != 0
#define RK_FAR_CLIP_Z(w) 0.0
#define RK_FAR_CLIP_Z_K(w, k) ((w) * ((1.0 - (k)) * 0.5))
#else
#define RK_FAR_CLIP_Z(w) (w)
#define RK_FAR_CLIP_Z_K(w, k) ((w) * (k))
#endif
#define RK_CLIP_Z_STD(p) RK_CLIP_Z_STD_OF(p, p)
