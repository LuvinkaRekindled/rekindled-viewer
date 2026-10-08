/**
 * @file llvkverify.h
 * @brief What is left of the GL-versus-Vulkan verification: a flag that stays false.
 *
 * The comparisons (readbacks, snapshots, the whole-frame check) needed a real OpenGL context to compare against and were deleted with it.
 * Code that still asks LLVkVerify::gEnabled gets false; the macros the viewer's readback paths call do nothing.
 */

#ifndef LL_LLVKVERIFY_H
#define LL_LLVKVERIFY_H

#if defined(LL_VULKAN)
namespace LLVkVerify
{
    extern bool gEnabled;
}
#endif

#define LL_VK_VERIFY_READPIXELS(x, y, w, h, format, type, data) do {} while (0)
#define LL_VK_VERIFY_TEXIMAGE(target, level, format, type, data) do {} while (0)
#define LL_VK_VERIFY_ARM_SNAPSHOT() do {} while (0)

#endif // LL_LLVKVERIFY_H
