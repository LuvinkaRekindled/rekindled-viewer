// lib/flags.glsl: the flag values of the normal target's alpha channel (the viewer prepends the same defines to every old shader, llshadermgr.cpp).
#define GBUFFER_FLAG_SKIP_ATMOS   0.0
#define GBUFFER_FLAG_HAS_ATMOS    0.34
#define GBUFFER_FLAG_HAS_PBR      0.67
#define GBUFFER_FLAG_HAS_HDRI     1.0
#define GET_GBUFFER_FLAG(data, flag)    (abs(data-flag)< 0.1)
