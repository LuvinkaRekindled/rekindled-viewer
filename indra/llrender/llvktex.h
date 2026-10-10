/**
 * @file llvktex.h
 * @brief F1.2: texture edits that name their texture.
 *
 * The GL way was "bind a texture on the active unit, then call glTexParameteri / glTexImage2D / glGenerateMipmap ...": the command meant whatever was bound on the
 * thread that issued it, so every edit had to replay the bindings (and the active unit, and the pixel-store state) in order. Here an edit names its texture
 * (the id is the name the producer allocated: it is the handle), carries the unpack state it needs, and does not depend on any binding. The records are
 * independent of the texture slots, so they no longer force pending slot changes out as records of their own.
 *
 * The producer-side slot table below is only what the old-style helpers (LLRender::texParameteri and friends, which act on "the bound texture") need to find
 * their id; it is written by the same calls that write the slot records and goes away with those helpers.
 *
 * Compiled only with -DUSE_VULKAN=ON.
 */

#ifndef LL_LLVKTEX_H
#define LL_LLVKTEX_H

#if defined(LL_VULKAN)

#include "linden_common.h"
#include "llvkasync.h"

namespace LLVkTex
{
    // the unpack state an upload reads, taken from the producer when the record is written
    struct PixelStore
    {
        S32 mAlignment = 4;
        S32 mRowLength = 0;
        U32 mUnpackBuffer = 0;      // a pixel unpack buffer is bound: the 'pixels' pointer is an offset into it
    };

    // the records (llvkasync.cpp writes and reads them)
    enum TexKind : U32 { K_IMAGE2D, K_IMAGE3D, K_SUB2D, K_SUB3D, K_COMPRESSED2D };
    enum TexOpKind : U32 { OPK_MIPMAP, OPK_COPY_SUB, OPK_COPY_IMAGE, OPK_SWIZZLE };
    struct TexOp { U32 mKind; U32 mTex; U32 mTarget; S32 mLevel; S32 mX0, mY0, mZ0, mX, mY, mW, mH; U32 mFormat; S32 mBorder; S32 mRgba[4]; };

    // LL_VK_TEX_VERIFY=1: an edit on the main thread that names a texture other than the one the caller bound on the active unit is logged (first 40)
    void verifyBound(const char* what, U32 id, U32 target);

    // ---- producer (any thread; the calling thread's pixel-store state applies) ----
    void image2D(U32 id, U32 target, S32 level, S32 internal_format, S32 width, S32 height, S32 border, U32 format, U32 type, const void* pixels);
    void image3D(U32 id, U32 target, S32 level, S32 internal_format, S32 width, S32 height, S32 depth, S32 border, U32 format, U32 type, const void* pixels);
    void subImage2D(U32 id, U32 target, S32 level, S32 x, S32 y, S32 width, S32 height, U32 format, U32 type, const void* pixels);
    void subImage3D(U32 id, U32 target, S32 level, S32 x, S32 y, S32 z, S32 width, S32 height, S32 depth, U32 format, U32 type, const void* pixels);
    void compressedImage2D(U32 id, U32 target, S32 level, U32 internal_format, S32 width, S32 height, S32 border, S32 image_size, const void* data);
    // sampler and level parameters (wrap, filter, anisotropy, levels, compare, swizzle): written as one record
    void params(U32 id, U32 target, const LLVkAsync::TexParam* p, U32 count);
    inline void paramI(U32 id, U32 target, U32 pname, S32 v) { LLVkAsync::TexParam p = { pname, 0, v, 0.f }; params(id, target, &p, 1); }
    void swizzle(U32 id, U32 target, const S32* rgba);
    void generateMipmap(U32 id, U32 target);
    // copies from the current read target (a render target or the back buffer) into the texture
    void copySubImage(U32 id, U32 target, S32 level, S32 xoffset, S32 yoffset, S32 zoffset, S32 x, S32 y, S32 width, S32 height);
    void copyImage2D(U32 id, U32 target, S32 level, U32 internal_format, S32 x, S32 y, S32 width, S32 height, S32 border);
    // answers (the caller waits for the worker)
    S32 levelParameter(U32 id, U32 target, S32 level, U32 pname);
    void getImage(U32 id, U32 target, S32 level, U32 format, U32 type, void* out);
    void getCompressedImage(U32 id, U32 target, S32 level, void* out);

    // a texture slot (LLTexUnit): what a draw samples from the unit. Typed record on the main producer; applied directly in inline mode; other threads render nothing, so nothing.
    void bindSlot(S32 unit, U32 target, U32 id);

    // ---- slot table for the helpers that act on 'the bound texture' (main thread) ----
    inline U32 targetClass(U32 target)
    {
        switch (target)
        {
        case 0x0DE1: return 0;                                  // TEXTURE_2D
        case 0x8513: case 0x8515: case 0x8516: case 0x8517: case 0x8518: case 0x8519: case 0x851A: return 1;     // cube map and its faces
        case 0x9009: return 2;                                  // CUBE_MAP_ARRAY
        case 0x8C1A: return 3;                                  // 2D_ARRAY
        case 0x806F: return 4;                                  // 3D
        case 0x9100: return 5;                                  // 2D_MULTISAMPLE
        default: return 7;
        }
    }
    struct Slots { U32 mBound[32][8] = {}; U32 mActive = 0; };
    inline Slots gSlots;
    inline void noteActive(S32 unit) { if ((U32)unit < 32) gSlots.mActive = (U32)unit; }
    inline void noteBind(S32 unit, U32 target, U32 id) { if ((U32)unit < 32) gSlots.mBound[unit][targetClass(target)] = id; }
    inline U32 boundOnActive(U32 target) { return gSlots.mBound[gSlots.mActive][targetClass(target)]; }
}

namespace LLVkGL
{
    // worker side (or the calling thread in inline mode): the same handlers the GL calls reached, acting on the named texture
    void tex_image2D(unsigned id, const LLVkTex::PixelStore& ps, unsigned target, int level, int internalformat, int width, int height, int border, unsigned format, unsigned type, const void* pixels);
    void tex_image3D(unsigned id, const LLVkTex::PixelStore& ps, unsigned target, int level, int internalformat, int width, int height, int depth, int border, unsigned format, unsigned type, const void* pixels);
    void tex_subimage2D(unsigned id, const LLVkTex::PixelStore& ps, unsigned target, int level, int xoffset, int yoffset, int width, int height, unsigned format, unsigned type, const void* pixels);
    void tex_subimage3D(unsigned id, const LLVkTex::PixelStore& ps, unsigned target, int level, int xoffset, int yoffset, int zoffset, int width, int height, int depth, unsigned format, unsigned type, const void* pixels);
    void tex_compressed2D(unsigned id, const LLVkTex::PixelStore& ps, unsigned target, int level, unsigned internalformat, int width, int height, int border, int imageSize, const void* data);
    void tex_params(unsigned id, unsigned target, const LLVkAsync::TexParam* params, unsigned count);
    void tex_swizzle(unsigned id, unsigned target, const int* rgba);
    void tex_mipmap(unsigned id, unsigned target);
    void tex_copy_sub(unsigned id, unsigned target, int level, int xoffset, int yoffset, int zoffset, int x, int y, int width, int height);
    void tex_copy_image(unsigned id, unsigned target, int level, unsigned internalformat, int x, int y, int width, int height, int border);
    int tex_level_parameter(unsigned id, unsigned target, int level, unsigned pname);
    void tex_get_image(unsigned id, unsigned target, int level, unsigned format, unsigned type, void* out);
    void tex_get_compressed_image(unsigned id, unsigned target, int level, void* out);
    void slot_bind(unsigned unit, unsigned target, unsigned name);
    // the calling thread's unpack state in inline mode (before the queue starts)
    LLVkTex::PixelStore inline_pixel_store();
}

#endif // LL_VULKAN

#endif // LL_LLVKTEX_H
