/**
 * @file llvkrt.h
 * @brief Ray tracing scene for the native Vulkan renderer: bottom-level structures per vertex/index buffer pair, a top-level structure per frame, and the descriptor
 *        set shaders trace against (inline ray queries). See doc/vulkan/PLAN-rt-shadows.md.
 *
 * Worker thread only, like the buffer table it reads (LLVkGL::getBufferInfo).
 */

#ifndef LL_LLVKRT_H
#define LL_LLVKRT_H

#if defined(LL_VULKAN)

#include "llvkcontext.h"

#include <string>

namespace LLVk
{
namespace Rt
{

// LL_VK_RT_SHADOWS: 0 off, 1 ray query shadows replace the cascades, 2 split screen (left cascades, right rays), 3 disagreement map. Decided once, before the shaders compile.
U32 shadowMode();

// A shadow caster draw as the shadow passes name it. mKind 0 = opaque casters, 1 = alpha masked or blended. mModel: 16 floats column-major (null = identity),
// mMatrixId: stable identity of the matrix (the object it belongs to), the same value for the same object on every frame.
struct Caster
{
    U32 mVb = 0, mIb = 0;               // GL buffer names
    U32 mFirst = 0, mCount = 0;         // index range
    U32 mMatrixId = 0;
    U32 mKind = 0;
    const F32* mModel = nullptr;
};

// the device can do it and the mode is on (the descriptor set layout exists, structures are built)
bool active();

// Creates the descriptor pool and sets and an empty structure per frame slot (start-up, after the renderer's layouts exist).
bool init(VkDescriptorSetLayout set_layout);
void shutdown();

// One caster draw of a shadow pass (shadow packets). Several passes of a frame name the same casters: the repeats are dropped.
void noteCaster(const Caster& c);

// Builds what the casters noted since the last call need (a budget per call), then the top-level structure for this view: the instances are transformed into eye space
// (view * model), the space the lighting shaders work in. Recorded into the current frame outside any render pass. view: 16 floats column-major.
void buildScene(const F32* view);

// the set to bind as set 3 for the frame slot (always valid once active())
VkDescriptorSet descriptorSet(U32 frame_slot);

// one log line: structures, memory, builds, refits, time
std::string report(double frames);
void resetStats();

} // namespace Rt
} // namespace LLVk

#endif // LL_VULKAN
#endif // LL_LLVKRT_H
