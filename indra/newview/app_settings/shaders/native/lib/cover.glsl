// lib/cover.glsl: the blended pass's coverage (attachment 1 of the screen target, R8, cleared to 0 by coverage_clear.frag). A blended surface writes red 1 with the alpha of its own colour times a weight of its class:
// the same blend equation as the colour then accumulates 1 - the product of what is left, the share of the pixel the blended surfaces cover. The upscaler's reactive mask is made from it (pipeline.cpp generateReactiveMask),
// in place of the colour difference against a copy of the opaque picture. The class weights: a rigged surface (hair, clothes: it moves with the body behind it, the motion pass keeps that motion) counts less than a static
// one (glass, a screen, foliage, particles: it moves with the camera and what is seen through it does not).
// A program that does not include this writes no coverage, and the renderer leaves the attachment alone for it (a pass writes only the outputs of the program: Program::outputMask).
#include "lib/motion.glsl"

#ifndef COVER_RIGGED
#define COVER_RIGGED 0.35
#endif
#ifndef COVER_STATIC
#define COVER_STATIC 1.0
#endif

layout(location = 1) out vec4 frag_cover;
// the motion of the blended pass (attachment 2 of the screen target, RG16 unorm, cleared to the sentinel 1,0 by coverage_clear.frag): a skinned blended surface (hair, shoes, clothes) writes its own screen motion where its alpha reaches
// RK_MOTION_ALPHA; the output alpha is the write mask of the blend (1 = replace, 0 = keep what is there), so a faint fragment leaves the pixel as it was. rk_write_cover writes the neutral value (alpha 0) for every program that
// includes this file; rk_write_motion after it sets the real one.
layout(location = 2) out vec4 frag_motion;
#ifndef RK_MOTION_ALPHA
#define RK_MOTION_ALPHA 0.3
#endif

// 'always': 1 for a surface whose picture changes by itself (a particle, an animated texture, water): the green channel accumulates the share of the pixel such surfaces cover, the reactive mask pass
// (reactive_mask.frag) lets that through whatever is behind; the red channel is the coverage of every blended surface, which counts where what is behind moves
void rk_write_cover(float alpha, float always)
{
    frag_motion = vec4(1.0, 0.0, 0.0, 0.0);       // (the sentinel "no motion of its own", with alpha 0: a blended draw leaves the pixel as it was, a draw with blending off writes the sentinel: what is seen there is no skinned blended surface)
#ifdef HAS_SKIN
    frag_cover = vec4(1.0, always, 0.0, COVER_RIGGED * alpha);
#else
    frag_cover = vec4(1.0, always, 0.0, COVER_STATIC * alpha);
#endif
}

void rk_write_motion(float alpha, bool valid, vec2 motion_px)
{
    if (valid && alpha >= RK_MOTION_ALPHA)
    {
        frag_motion = vec4(rk_motion_encode_px(motion_px), 0.0, 1.0);
    }
}
