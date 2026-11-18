// rt_scene.glsl: what a native program that traces rays (the manifest define RK_RT_QUERY: the loader adds `#extension GL_EXT_ray_query` to the head and the draw binds set 3) sees of the frame's ray scene:
// set 3 binding 0 the acceleration structure, binding 1 the geometry records of its masked structures (RKRtGeom: record 0 is the header, see rtSceneFrame in lib/rt.glsl), and the materials the records point at.
// The declarations are the loader's own for the stock shaders (rkvkshader.cpp, `w.mUsesRt`), as text: the layouts are the renderer's (rkvkrt.cpp, GpuGeom and the material table). The frame's 2D table of
// textures is the native one (rk_tbl2d, set 2 binding 0): `rk_rt_tex` is a name for it, so the stock code reads textures as it did.

layout(set = 3, binding = 0) uniform accelerationStructureEXT rk_rt_tlas;
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer RKRtPositions { vec4 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer RKRtTexcoords { vec2 t[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer RKRtColors { uint c[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer RKRtNormals { vec4 n[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer RKRtIndices { uint w[]; };
struct RKRtGeom { uvec2 positions; uvec2 indices; uvec2 texcoords; uvec2 colors; uvec2 slots; uint flags; float cutoff; uint first; uint material; vec2 uv0; vec2 uv1; vec2 uv2; uvec2 normals; uint vlimit; uint vpad; };
struct RKRtMaterial { uvec2 slots; uvec2 slots2; vec4 color; vec4 emissive; vec4 params; vec2 uv0; vec2 uv1; vec2 uv2; vec2 pad; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer RKRtMaterials { RKRtMaterial m[]; };
layout(std430, set = 3, binding = 1) readonly buffer RKRtGeoms { RKRtGeom g[]; } rk_rt_geoms;
// <FS> a vertex index of a record's triangle, held to what the arrays of the record can hold (vlimit, 0 = no limit): an index that is stale against a vertex buffer that is now shorter (a LOD change replaced the
// buffer) read 0.6 to 1.5 MB past it and lost the device (2026-11-14); it now reads a wrong vertex of the same buffer.
uint rtClampVertex(RKRtGeom g, uint v) { return g.vlimit != 0u ? min(v, g.vlimit - 1u) : v; }
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer RKRtPins { uint s[]; };
uint rk_rt_slot(uint pin) { return pin < 32768u ? RKRtPins(uvec2(floatBitsToUint(rk_rt_geoms.g[0].uv0.x), floatBitsToUint(rk_rt_geoms.g[0].uv0.y))).s[pin] : 0u; }

#define rk_rt_tex rk_tbl2d
