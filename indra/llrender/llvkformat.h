/**
 * @file llvkformat.h
 * @brief F2.2: typed descriptions of textures and pixel data, and the one place that decodes the viewer's GL enum numbers.
 *
 * The viewer keeps GL enum numbers as constants (LLImageTex's formats, LLTexUnit's filter and wrap modes ...). They are turned into the types below once, where the producer
 * writes a record (llvktex.h, llvktarget.h), so neither the command stream nor the worker carries a GL number for textures.
 *
 * Compiled only with -DUSE_VULKAN=ON.
 */

#ifndef LL_LLVKFORMAT_H
#define LL_LLVKFORMAT_H

#if defined(LL_VULKAN)

#include "linden_common.h"
#include "llrenderformat.h"
#include "llrenderstate.h"

namespace LLVkFormat
{
    // ---- dimensionality (the values are the slot classes of the texture slot table) ----
    enum Dim : U8 { DIM_2D = 0, DIM_CUBE = 1, DIM_CUBE_ARRAY = 2, DIM_2D_ARRAY = 3, DIM_3D = 4, DIM_MS = 5, DIM_OTHER = 7 };
    struct Target
    {
        U8 mDim = DIM_OTHER;
        S8 mFace = -1;          // 0-5 when the target named one face of a cube map, else -1
    };
    Target target(U32 gl_target);               // (F3.6f: the typed form below is what the producer uses; this one stays for the migration tables)
    inline Target target(TextureTarget t)
    {
        Target r;
        const U8 v = (U8)t;
        r.mDim = v >= 8 ? (U8)DIM_CUBE : v;
        r.mFace = v >= 8 ? (S8)(v - 8) : (S8)-1;
        return r;
    }

    // ---- client pixel data: components and component type ----
    enum Comps : U8 { C_NONE = 0, C_R, C_RG, C_RGB, C_BGR, C_RGBA, C_BGRA, C_LUM, C_LUM_ALPHA, C_ALPHA, C_DEPTH };
    enum PType : U8 { T_NONE = 0, T_U8, T_S8, T_U16, T_S16, T_U32, T_S32, T_F32, T_F16 };
    struct Transfer
    {
        U8 mComps = C_NONE;
        U8 mType = T_NONE;
        bool valid() const { return mComps != C_NONE && mType != T_NONE; }
    };
    Transfer transfer(U32 gl_format, U32 gl_type);
    U32 compCount(U8 comps);
    U32 typeBytes(U8 type);

    // ---- geometry: draw modes, vertex attribute / index component types (the PType values), buffer kinds ----
    enum Topology : U8 { TOP_POINTS = 0, TOP_LINES, TOP_LINE_STRIP, TOP_TRIANGLES, TOP_TRIANGLE_STRIP, TOP_TRIANGLE_FAN, TOP_OTHER };
    Topology topology(U32 gl_mode);
    PType componentType(U32 gl_type);           // T_NONE for a type that is not a component type
    enum BufKind : U8 { BK_VERTEX = 0, BK_INDEX, BK_UNIFORM, BK_OTHER };
    BufKind bufKind(U32 gl_target);

    // ---- how a texture stores its texels (the internal formats the viewer uses; one entry per distinct treatment) ----
    enum Store : U8
    {
        S_NONE = 0,
        S_RGBA8, S_RGB8, S_SRGBA8, S_SRGB8, S_ALPHA8, S_LUM8, S_LUM_ALPHA8,
        S_RG16F, S_RG32F, S_RG16, S_R8, S_RG8, S_R32F, S_R16F, S_RGBA16F, S_RGBA32F, S_RGBA16, S_R11G11B10F, S_RGB10A2,
        S_DEPTH,
        S_BC1_RGB, S_BC1_RGBA, S_BC2, S_BC3, S_BC7,
        S_GENERIC_COMPRESSED,
        S_BC1_SRGBA, S_BC2_SRGBA, S_BC3_SRGBA
    };
    Store store(S32 gl_internal_format);        // S_NONE: no Vulkan format for it

    // ---- F3.6e: the typed vocabulary of llrenderformat.h IS these values (static_asserts in llvkformat.cpp) ----
    inline Store store(TexelFormat f) { return (Store)f; }
    inline Transfer transfer(PixelFormat f, ComponentType t) { Transfer r; r.mComps = (U8)f; r.mType = (U8)t; return r; }

    // LL_VK_FORMAT_VERIFY=1: every texture data call names its call site and what it carries; the first time a (site, entry, store, comps, type) combination is seen it is logged
    // ("FormatVerify"), so two builds can be compared by the sets of lines.
    bool formatVerify();
    void noteFormat(const char* what, const char* file, int line, U8 store, U8 comps, U8 type);
    // F3.6f: the same for the decoded targets, settings, swizzles and level queries (one line per distinct combination: LL_VK_FORMAT_VERIFY)
    void noteKey(const char* what, int a = 0, int b = 0, int c = 0, int d = 0, float f = 0.f);

    // ---- sampler and level settings ----
    enum Param : U8
    {
        P_NONE = 0,
        P_MIN_FILTER, P_MAG_FILTER, P_WRAP_S, P_WRAP_T, P_WRAP_R, P_COMPARE_MODE, P_COMPARE_FUNC, P_ANISOTROPY, P_BASE_LEVEL, P_MAX_LEVEL,
        P_SWIZZLE_R, P_SWIZZLE_G, P_SWIZZLE_B, P_SWIZZLE_A,
        P_GENERATE_MIPMAP
    };
    enum Filter : U8 { F_NEAREST = 0, F_LINEAR, F_NEAREST_MIP_NEAREST, F_LINEAR_MIP_NEAREST, F_NEAREST_MIP_LINEAR, F_LINEAR_MIP_LINEAR };
    enum Wrap : U8 { W_REPEAT = 0, W_CLAMP_EDGE, W_MIRRORED_REPEAT, W_CLAMP_BORDER };
    enum Compare : U8 { CMP_NEVER = 0, CMP_LESS, CMP_EQUAL, CMP_LEQUAL, CMP_GREATER, CMP_NOTEQUAL, CMP_GEQUAL, CMP_ALWAYS };      // the order of VkCompareOp
    enum Swizzle : U8 { SW_R = 0, SW_G, SW_B, SW_A, SW_ZERO, SW_ONE };
    struct Setting
    {
        U8 mParam = P_NONE;
        U8 mValue = 0;          // a Filter / Wrap / Compare / Swizzle, or 0/1 for the compare mode and mipmap generation
        S32 mInt = 0;           // levels
        F32 mFloat = 0.f;       // anisotropy
    };
    // a setting as the viewer states it (F3.6f: typed builders; setting(U32 ...) decodes GL numbers for the migration tables and glTF files only)
    Setting setting(U32 gl_pname, bool is_float, S32 int_value, F32 float_value);
    Swizzle swizzle(S32 gl_value);
    inline Setting setMinFilter(TextureFilter f) { Setting s; s.mParam = P_MIN_FILTER; s.mValue = (U8)f; return s; }
    inline Setting setMagFilter(TextureFilter f) { Setting s; s.mParam = P_MAG_FILTER; s.mValue = (U8)f; return s; }
    inline Setting setWrapS(TextureWrap w) { Setting s; s.mParam = P_WRAP_S; s.mValue = (U8)w; return s; }
    inline Setting setWrapT(TextureWrap w) { Setting s; s.mParam = P_WRAP_T; s.mValue = (U8)w; return s; }
    inline Setting setWrapR(TextureWrap w) { Setting s; s.mParam = P_WRAP_R; s.mValue = (U8)w; return s; }
    inline Setting setCompareMode(bool on) { Setting s; s.mParam = P_COMPARE_MODE; s.mValue = on ? 1 : 0; return s; }
    inline Setting setCompareFunc(LLRenderState::CompareOp f) { Setting s; s.mParam = P_COMPARE_FUNC; s.mValue = (U8)f; return s; }
    inline Setting setAnisotropy(F32 a) { Setting s; s.mParam = P_ANISOTROPY; s.mFloat = a; return s; }
    inline Setting setBaseLevel(S32 l) { Setting s; s.mParam = P_BASE_LEVEL; s.mInt = l; return s; }
    inline Setting setMaxLevel(S32 l) { Setting s; s.mParam = P_MAX_LEVEL; s.mInt = l; return s; }

    // ---- what a level query asks for ----
    enum Level : U8 { LQ_NONE = 0, LQ_WIDTH, LQ_HEIGHT, LQ_DEPTH, LQ_COMPRESSED, LQ_COMPRESSED_SIZE };
    Level level(U32 gl_pname);
}

#endif // LL_VULKAN

#endif // LL_LLVKFORMAT_H
