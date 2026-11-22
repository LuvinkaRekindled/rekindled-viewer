// lib/cover.glsl: the blended pass's coverage (attachment 1 of the screen target, R8, cleared to 0 by coverage_clear.frag). A blended surface writes red 1 with the alpha of its own colour times a weight of its class:
// the same blend equation as the colour then accumulates 1 - the product of what is left, the share of the pixel the blended surfaces cover. The upscaler's reactive mask is made from it (pipeline.cpp generateReactiveMask),
// in place of the colour difference against a copy of the opaque picture. The class weights: a rigged surface (hair, clothes: it moves with the body behind it, the motion pass keeps that motion) counts less than a static
// one (glass, a screen, foliage, particles: it moves with the camera and what is seen through it does not).
// A program that does not include this writes no coverage, and the renderer leaves the attachment alone for it (a pass writes only the outputs of the program: Program::outputMask).
#ifndef COVER_RIGGED
#define COVER_RIGGED 0.35
#endif
#ifndef COVER_STATIC
#define COVER_STATIC 1.0
#endif

layout(location = 1) out vec4 frag_cover;

void rk_write_cover(float alpha)
{
#ifdef HAS_SKIN
    frag_cover = vec4(1.0, 0.0, 0.0, COVER_RIGGED * alpha);
#else
    frag_cover = vec4(1.0, 0.0, 0.0, COVER_STATIC * alpha);
#endif
}
