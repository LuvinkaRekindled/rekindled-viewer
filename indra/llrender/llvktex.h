/**
 * @file llvktex.h
 * @brief F1.2: texture edits that name their texture.
 *
 * The GL way was "bind a texture on the active unit, then call glTexParameteri / glTexImage2D / glGenerateMipmap ...": the command meant whatever was bound on the
 * thread that issued it, so every edit had to replay the bindings (and the active unit, and the pixel-store state) in order. Here an edit names its texture
 * (the id is the name the producer allocated: it is the handle), carries the unpack state it needs, and does not depend on any binding. The records are
 * independent of the texture slots, so they no longer force pending slot changes out as records of their own.
 *
 * Compiled only with -DUSE_VULKAN=ON.
 */

#ifndef LL_LLVKTEX_H
#define LL_LLVKTEX_H

#if defined(LL_VULKAN)

#include "linden_common.h"
#include "llvkasync.h"
#include "llvkformat.h"

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
    struct TexOp { U32 mKind; U32 mTex; LLVkFormat::Target mTarget; U8 mStore; U8 mSwz[4]; U8 mPad8; S32 mLevel; S32 mX0, mY0, mZ0, mX, mY, mW, mH; U32 mSource; };

    // ---- handles: the producer allocates the id, the worker makes and destroys the texture in stream order ----
    void create(S32 count, U32* ids);
    void destroy(S32 count, const U32* ids);
    // the unpack state the next uploads of this thread carry (producer-side only: no record)
    void setUnpackAlignment(S32 alignment);
    void setUnpackRowLength(S32 row_length);

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
    // copies from a render target (source: its id, 0 = the back buffer; colour attachment 0) into the texture
    void copySubImage(U32 id, U32 target, S32 level, S32 xoffset, S32 yoffset, S32 zoffset, S32 x, S32 y, S32 width, S32 height, U32 source);
    void copyImage2D(U32 id, U32 target, S32 level, U32 internal_format, S32 x, S32 y, S32 width, S32 height, S32 border, U32 source);
    // false: a read back now would make the main thread wait for a worker that is far behind (or too many were made this frame): ask again in a later frame. Counts the read back when it says yes.
    bool readbackAffordable();
    // answers (the caller waits for the worker)
    S32 levelParameter(U32 id, U32 target, S32 level, U32 pname);
    void getImage(U32 id, U32 target, S32 level, U32 format, U32 type, void* out);
    void getCompressedImage(U32 id, U32 target, S32 level, void* out);

    // a texture slot (LLTexUnit): what a draw samples from the unit. Typed record on the main producer; applied directly in inline mode; other threads render nothing, so nothing.
    void bindSlot(S32 unit, U32 target, U32 id);
}

namespace LLVkGL
{
    // worker side (or the calling thread in inline mode): the typed handlers, acting on the named texture
    void tex_image2D(unsigned id, const LLVkTex::PixelStore& ps, LLVkFormat::Target tg, int level, LLVkFormat::Store st, int width, int height, LLVkFormat::Transfer tr, const void* pixels);
    void tex_image3D(unsigned id, const LLVkTex::PixelStore& ps, LLVkFormat::Target tg, int level, LLVkFormat::Store st, int width, int height, int depth, LLVkFormat::Transfer tr, const void* pixels);
    void tex_subimage2D(unsigned id, const LLVkTex::PixelStore& ps, LLVkFormat::Target tg, int level, int xoffset, int yoffset, int width, int height, LLVkFormat::Transfer tr, const void* pixels);
    void tex_subimage3D(unsigned id, const LLVkTex::PixelStore& ps, LLVkFormat::Target tg, int level, int xoffset, int yoffset, int zoffset, int width, int height, int depth, LLVkFormat::Transfer tr, const void* pixels);
    void tex_compressed2D(unsigned id, const LLVkTex::PixelStore& ps, LLVkFormat::Target tg, int level, LLVkFormat::Store st, int width, int height, int imageSize, const void* data);
    void tex_params(unsigned id, const LLVkFormat::Setting* settings, unsigned count);
    void tex_swizzle(unsigned id, const LLVkFormat::Swizzle* rgba);
    void tex_mipmap(unsigned id);
    void tex_copy_sub(unsigned id, LLVkFormat::Target tg, int level, int xoffset, int yoffset, int zoffset, int x, int y, int width, int height, unsigned source);
    void tex_copy_image(unsigned id, LLVkFormat::Target tg, int level, LLVkFormat::Store st, int x, int y, int width, int height, unsigned source);
    int tex_level_parameter(unsigned id, LLVkFormat::Target tg, int level, LLVkFormat::Level q);
    void tex_get_image(unsigned id, LLVkFormat::Target tg, int level, LLVkFormat::Transfer tr, void* out);
    void tex_get_compressed_image(unsigned id, LLVkFormat::Target tg, int level, void* out);
    // dim: the LLVkFormat::Dim of the slot
    void slot_bind(unsigned unit, unsigned dim, unsigned name);
    // the calling thread's unpack state in inline mode (before the queue starts)
    LLVkTex::PixelStore inline_pixel_store();
}

#endif // LL_VULKAN

#endif // LL_LLVKTEX_H
