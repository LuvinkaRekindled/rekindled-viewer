// rkreversez.h: the depth convention (PLAN-reversed-z). Every depth target except the shadow maps holds the REVERSED convention: 1.0 at the near plane, 0.0 at the far plane and the sky (float depth
// spends its precision where the far surfaces are). The projection the shaders get is the reversed Vulkan form (LLRender::getShaderProjectionMatrix), the compare ops, the clear value and the depth
// bias of those targets are mirrored by the renderer (rkvkrenderer.cpp, RKImage::mReverseZ), and the native shaders ask the macros of rk_native.glsl instead of comparing against 1.0 themselves.
// The shadow maps keep the standard convention and the GL projection (RK_CLIP_Z_STD, LLRenderTarget::setReverseZ(false)).
#pragma once

#include <cstdlib>
