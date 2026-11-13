// probe_block.glsl: the reflection probes of the view: one block for the whole frame, bound by the renderer (named block 0 = binding 1). The members and their order are the viewer's (the CPU side writes the
// block). Included by the pass files of every program that reads the probes (alpha_pass.inc, resolve_pass.inc, water_pass.inc), so that all of them declare the same block.

#define MAX_REFMAP_COUNT 256
layout(std140, set = 0, binding = 1) uniform ReflectionProbes
{
    mat4 refBox[MAX_REFMAP_COUNT];          // the box of a user override probe: three outward planes and the box's thickness along them
    mat4 heroBox;
    vec4 refSphere[MAX_REFMAP_COUNT];       // bounding spheres of the probes, the closest to the camera first
    vec4 refParams[MAX_REFMAP_COUNT];       // x irradiance scale, y radiance scale, z fade in, w near
    vec4 heroSphere;
    ivec4 refIndex[MAX_REFMAP_COUNT];       // x cube map layer, y index of the neighbour list (in ivec4s), z neighbour count, w priority (negative: a box probe)
    ivec4 refNeighbor[1024];
    ivec4 refBucket[256];                   // by depth: the first and the one past the last probe that can reach it
    int refmapCount;
    int heroShape;
    int heroMipCount;
    int heroProbeCount;
};
