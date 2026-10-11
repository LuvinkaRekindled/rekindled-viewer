/**
 * @file llvkformat.h
 * @brief F2.2: typed descriptions of textures and pixel data, and the one place that decodes the viewer's GL enum numbers.
 *
 * The viewer keeps GL enum numbers as constants (LLImageGL's formats, LLTexUnit's filter and wrap modes ...). They are turned into the types below once, where the producer
 * writes a record (llvktex.h, llvktarget.h), so neither the command stream nor the worker carries a GL number for textures.
 *
 * Compiled only with -DUSE_VULKAN=ON.
 */

#ifndef LL_LLVKFORMAT_H
#define LL_LLVKFORMAT_H

#if defined(LL_VULKAN)

#include "linden_common.h"

namespace LLVkFormat
{
    // ---- dimensionality (the values are the slot classes of the texture slot table) ----
    enum Dim : U8 { DIM_2D = 0, DIM_CUBE = 1, DIM_CUBE_ARRAY = 2, DIM_2D_ARRAY = 3, DIM_3D = 4, DIM_MS = 5, DIM_OTHER = 7 };
    struct Target
    {
        U8 mDim = DIM_OTHER;
        S8 mFace = -1;          // 0-5 when the target named one face of a cube map, else -1
    };
    Target target(U32 gl_target);

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

    // ---- how a texture stores its texels (the internal formats the viewer uses; one entry per distinct treatment) ----
    enum Store : U8
    {
        S_NONE = 0,
        S_RGBA8, S_RGB8, S_SRGBA8, S_SRGB8, S_ALPHA8, S_LUM8, S_LUM_ALPHA8,
        S_RG16F, S_RG32F, S_RG16, S_R8, S_RG8, S_R32F, S_R16F, S_RGBA16F, S_RGBA32F, S_RGBA16, S_R11G11B10F, S_RGB10A2,
        S_DEPTH,
        S_BC1_RGB, S_BC1_RGBA, S_BC2, S_BC3, S_BC7,
        S_GENERIC_COMPRESSED
    };
    Store store(S32 gl_internal_format);        // S_NONE: no Vulkan format for it

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
    // a setting as the viewer states it (GL parameter name, value as an integer or a float)
    Setting setting(U32 gl_pname, bool is_float, S32 int_value, F32 float_value);
    Swizzle swizzle(S32 gl_value);

    // ---- what a level query asks for ----
    enum Level : U8 { LQ_NONE = 0, LQ_WIDTH, LQ_HEIGHT, LQ_DEPTH, LQ_COMPRESSED, LQ_COMPRESSED_SIZE };
    Level level(U32 gl_pname);
}

#endif // LL_VULKAN

#endif // LL_LLVKFORMAT_H
