// ll_native.glsl: the prelude of every native program (see llrender/llvknative.h for the whole contract).
// Vulkan GLSL 460, explicit layouts, no GL-era names. The loader puts `#version 460`, the include extension, the stage macro (LL_VERTEX_STAGE or LL_FRAGMENT_STAGE)
// and the manifest's defines in front of this file.

#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#ifdef TEXNEED
// idea 3, the alpha pass (lib/texneed.glsl ll_texneed_store): one store a quad takes the quad operations
#extension GL_KHR_shader_subgroup_quad : require
#endif

// the bindless texture tables of the frame (set 2) and the map from a texture id to its descriptor index:  texture(ll_tbl2d[LL_HM(ll_ti_name)], uv)
layout(set = 2, binding = 0) uniform sampler2D ll_tbl2d[];
layout(set = 2, binding = 1) uniform samplerCube ll_tblcube[];
layout(set = 2, binding = 2) uniform samplerCubeArray ll_tblcubearray[];
layout(set = 2, binding = 3) uniform sampler2DShadow ll_tbl2dshadow[];
layout(std430, set = 2, binding = 4) readonly buffer LlHandleMap { uint ll_hm[]; };
#define LL_HM(id) ll_hm[(id)]
#ifdef TEXNEED
// the renderer's texture need buffer (H6, set 2 binding 5): a word a handle, the float bits of the smallest area of texture coordinates one pixel covered (0xFFFFFFFF: none)
layout(std430, set = 2, binding = 5) buffer LlTexNeed { uint ll_texneed_buf[]; };
#endif
