// rkreversez.h: the depth convention switch of PLAN-reversed-z (a development switch: RK_VK_REVERSEZ=1 in the environment). With it off (the default) the viewer
// is exactly what it was: standard depth, 0.0 at the near plane and 1.0 at the far plane and the sky. With it on, every depth target except the shadow maps holds the
// REVERSED convention: 1.0 at the near plane, 0.0 at the far plane and the sky; the projection the shaders get is the reversed Vulkan form, the compare ops, the
// clear value and the depth bias of those targets are mirrored by the renderer, and the native programs are built with RK_REVERSE_Z=1 (rk_native.glsl).
// The switch is deleted when the stages of the plan are proven (the convention is then the only one).
#pragma once

#include <cstdlib>

inline bool rk_reverse_z()
{
    static const bool on = []() { const char* e = getenv("RK_VK_REVERSEZ"); return e && atoi(e) != 0; }();
    return on;
}
