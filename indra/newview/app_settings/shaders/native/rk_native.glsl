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
