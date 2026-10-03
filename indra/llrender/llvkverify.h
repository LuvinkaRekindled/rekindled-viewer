/**
 * @file llvkverify.h
 * @brief Cross-checks of what the Vulkan mirror holds against what OpenGL just returned from a readback.
 *
 * Active only with LL_VK_VERIFY=1 while the mirror runs. After the viewer's own glReadPixels / glGetTexImage the
 * same region is read back from the Vulkan image and compared; the result goes to the log (tag VkVerify). This is the
 * test bed for answering those calls from Vulkan alone, which a Vulkan-only renderer will have to do.
 */

#ifndef LL_LLVKVERIFY_H
#define LL_LLVKVERIFY_H

#if defined(LL_VULKAN)

namespace LLVkVerify
{
    extern bool gEnabled;

    // GL has just filled gl_data with the (x, y, w, h) rectangle of the current read framebuffer
    void readPixels(int x, int y, int w, int h, unsigned int format, unsigned int type, const void* gl_data);

    // a snapshot is about to render: compare every pass of it between GL and Vulkan (SNAP lines in the log)
    void armSnapshot();

    // called where the viewer swaps buffers: compares the whole default framebuffer (every 300th frame or on request)
    void presentFrame();

    // GL has just filled gl_data with mip 'level' of the texture bound to the active unit
    void texImage(unsigned int target, int level, unsigned int format, unsigned int type, const void* gl_data);
}

#define LL_VK_VERIFY_READPIXELS(x, y, w, h, format, type, data) \
    do { if (LLVkVerify::gEnabled) LLVkVerify::readPixels((x), (y), (w), (h), (format), (type), (data)); } while (0)
#define LL_VK_VERIFY_TEXIMAGE(target, level, format, type, data) \
    do { if (LLVkVerify::gEnabled) LLVkVerify::texImage((target), (level), (format), (type), (data)); } while (0)
#define LL_VK_VERIFY_ARM_SNAPSHOT()     do { if (LLVkVerify::gEnabled) LLVkVerify::armSnapshot(); } while (0)

#else

#define LL_VK_VERIFY_READPIXELS(x, y, w, h, format, type, data) do {} while (0)
#define LL_VK_VERIFY_TEXIMAGE(target, level, format, type, data) do {} while (0)
#define LL_VK_VERIFY_ARM_SNAPSHOT() do {} while (0)

#endif

#endif // LL_LLVKVERIFY_H
