/**
 * @file llvkgl.cpp
 * @brief Translation of the viewer's OpenGL call stream into Vulkan (see llvkgl.h).
 *
 * Every hook runs after the real GL call. The hooks keep a copy of the GL state that matters (textures, framebuffer
 * attachments, buffers, vertex arrays, draw state) and, at each draw, translate it into Vulkan state and replay the
 * draw into the mirror frame.
 */

#include "linden_common.h"

#define LL_VK_NO_INTERCEPT      // this file talks about GL types and constants only; it never calls GL
#include "llglheaders.h"

#if defined(LL_VULKAN)

#include "llvkmirror.h"
#include "llvkverify.h"
#include "llglslshader.h"
#include "llshadermgr.h"
#include "llvkshader.h"
#include "llvkcontext.h"
#include "llvkrenderer.h"
#include "llvkdlss.h"

#include <chrono>
#include "llvkgl_hooks.inc"      // generated: storage and names for the per-hook statistics
#include "llvkasync.h"
#include <map>
#include <mutex>
#include <set>
#include <cstdio>
#include <tuple>
#include <memory>
#include <unordered_map>
#include <cstring>
#include <thread>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <string>

// constants that older GL headers may not define
#ifndef GL_READ_FRAMEBUFFER_BINDING
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#endif
#ifndef GL_BGR
#define GL_BGR 0x80E0
#endif
#ifndef GL_PACK_ROW_LENGTH
#define GL_PACK_ROW_LENGTH 0x0D02
#endif
#ifndef GL_PACK_ALIGNMENT
#define GL_PACK_ALIGNMENT 0x0D05
#endif
#ifndef GL_HALF_FLOAT
#define GL_HALF_FLOAT 0x140B
#endif
#ifndef GL_TEXTURE_BINDING_2D
#define GL_TEXTURE_BINDING_2D 0x8069
#endif
#ifndef GL_TEXTURE_CUBE_MAP_ARRAY
#define GL_TEXTURE_CUBE_MAP_ARRAY 0x9009
#endif
#ifndef GL_TEXTURE_2D_ARRAY
#define GL_TEXTURE_2D_ARRAY 0x8C1A
#endif
#ifndef GL_PIXEL_UNPACK_BUFFER
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#endif
#ifndef GL_UNIFORM_BUFFER
#define GL_UNIFORM_BUFFER 0x8A11
#endif
#ifndef GL_COMPARE_REF_TO_TEXTURE
#define GL_COMPARE_REF_TO_TEXTURE 0x884E
#endif
#ifndef GL_TEXTURE_MAX_ANISOTROPY
#define GL_TEXTURE_MAX_ANISOTROPY 0x84FE
#endif
#ifndef GL_TEXTURE_SWIZZLE_R
#define GL_TEXTURE_SWIZZLE_R 0x8E42
#define GL_TEXTURE_SWIZZLE_G 0x8E43
#define GL_TEXTURE_SWIZZLE_B 0x8E44
#define GL_TEXTURE_SWIZZLE_A 0x8E45
#define GL_TEXTURE_SWIZZLE_RGBA 0x8E46
#endif
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif

namespace LLVkGL
{

bool gActive = false;
bool gOwn = false;      // LL_VK_OWN: the mirror owns the GL resources (see llvkgl.h)
LLVkMirror::Mutex gMutex;

// CPU time spent inside the translator's draw/clear hooks (the cost of recording the work into Vulkan), in ticks of the performance counter
static long long gHookTicks = 0;

namespace
{

struct HookTimer
{
    LARGE_INTEGER mStart;
    HookTimer() { QueryPerformanceCounter(&mStart); }
    ~HookTimer()
    {
        LARGE_INTEGER end;
        QueryPerformanceCounter(&end);
        gHookTicks += end.QuadPart - mStart.QuadPart;
    }
};

using namespace LLVk;

//-----------------------------------------------------------------------------------------------------
// logging helpers
//-----------------------------------------------------------------------------------------------------

void warn_once(const char* what, U32 value)
{
    static std::map<std::string, bool> seen;
    const std::string key = std::string(what) + ":" + std::to_string(value);
    if (!seen[key])
    {
        seen[key] = true;
        LL_WARNS("VkGL") << "unsupported: " << what << " 0x" << std::hex << value << std::dec << LL_ENDL;
    }
}

//-----------------------------------------------------------------------------------------------------
// formats
//-----------------------------------------------------------------------------------------------------

struct FormatMap
{
    VkFormat mVk = VK_FORMAT_UNDEFINED;
    U32 mTexelBytes = 4;                    // bytes per texel in the Vulkan image
    bool mDepth = false;
    bool mCompressed = false;
    VkComponentMapping mSwizzle = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
};

const VkComponentMapping SWZ_ALPHA = { VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R };
const VkComponentMapping SWZ_LUM = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ONE };
const VkComponentMapping SWZ_LUM_ALPHA = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G };

bool map_internal_format(GLint f, FormatMap& m)
{
    m = FormatMap();
    switch ((GLenum)f)
    {
    case GL_RGBA: case GL_RGBA8: case 4: m.mVk = VK_FORMAT_R8G8B8A8_UNORM; return true;
    case GL_RGB: case GL_RGB8: case 3: m.mVk = VK_FORMAT_R8G8B8A8_UNORM; return true;
    case 0x8C43: case 0x8C42: m.mVk = VK_FORMAT_R8G8B8A8_SRGB; return true;        // SRGB8_ALPHA8, SRGB_ALPHA
    case 0x8C41: case 0x8C40: m.mVk = VK_FORMAT_R8G8B8A8_SRGB; return true;        // SRGB8, SRGB
    case GL_ALPHA: case GL_ALPHA8: m.mVk = VK_FORMAT_R8_UNORM; m.mTexelBytes = 1; m.mSwizzle = SWZ_ALPHA; return true;
    case GL_LUMINANCE: case GL_LUMINANCE8: case 1: m.mVk = VK_FORMAT_R8_UNORM; m.mTexelBytes = 1; m.mSwizzle = SWZ_LUM; return true;
    case GL_LUMINANCE_ALPHA: case GL_LUMINANCE8_ALPHA8: case 2: m.mVk = VK_FORMAT_R8G8_UNORM; m.mTexelBytes = 2; m.mSwizzle = SWZ_LUM_ALPHA; return true;
    case 0x822F: m.mVk = VK_FORMAT_R16G16_SFLOAT; m.mTexelBytes = 4; return true;  // RG16F
    case 0x8230: m.mVk = VK_FORMAT_R32G32_SFLOAT; m.mTexelBytes = 8; return true;  // RG32F
    case 0x822C: m.mVk = VK_FORMAT_R16G16_UNORM; m.mTexelBytes = 4; return true;   // RG16
    case 0x8229: m.mVk = VK_FORMAT_R8_UNORM; m.mTexelBytes = 1; return true;       // R8
    case 0x822B: m.mVk = VK_FORMAT_R8G8_UNORM; m.mTexelBytes = 2; return true;     // RG8
    case 0x822E: m.mVk = VK_FORMAT_R32_SFLOAT; m.mTexelBytes = 4; return true;     // R32F
    case 0x822D: m.mVk = VK_FORMAT_R16_SFLOAT; m.mTexelBytes = 2; return true;     // R16F
    case 0x881A: case 0x881B: m.mVk = VK_FORMAT_R16G16B16A16_SFLOAT; m.mTexelBytes = 8; return true;   // RGBA16F, RGB16F
    case 0x8814: case 0x8815: m.mVk = VK_FORMAT_R32G32B32A32_SFLOAT; m.mTexelBytes = 16; return true;  // RGBA32F, RGB32F
    case 0x805B: m.mVk = VK_FORMAT_R16G16B16A16_UNORM; m.mTexelBytes = 8; return true;                 // RGBA16
    case 0x8C3A: m.mVk = VK_FORMAT_B10G11R11_UFLOAT_PACK32; return true;           // R11F_G11F_B10F
    case 0x8059: m.mVk = VK_FORMAT_A2B10G10R10_UNORM_PACK32; return true;          // RGB10_A2
    case GL_DEPTH_COMPONENT: case GL_DEPTH_COMPONENT16: case GL_DEPTH_COMPONENT24: case GL_DEPTH_COMPONENT32:
    case 0x8CAC: case 0x88F0:                                                       // DEPTH_COMPONENT32F, DEPTH24_STENCIL8
        m.mVk = VK_FORMAT_D32_SFLOAT; m.mTexelBytes = 4; m.mDepth = true; return true;
    case 0x83F0: m.mVk = VK_FORMAT_BC1_RGB_UNORM_BLOCK; m.mTexelBytes = 8; m.mCompressed = true; return true;
    case 0x83F1: m.mVk = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; m.mTexelBytes = 8; m.mCompressed = true; return true;
    case 0x83F2: m.mVk = VK_FORMAT_BC2_UNORM_BLOCK; m.mTexelBytes = 16; m.mCompressed = true; return true;
    case 0x83F3: m.mVk = VK_FORMAT_BC3_UNORM_BLOCK; m.mTexelBytes = 16; m.mCompressed = true; return true;
    case 0x8E8C: m.mVk = VK_FORMAT_BC7_UNORM_BLOCK; m.mTexelBytes = 16; m.mCompressed = true; return true;
    // generic compressed formats: the driver chooses; the mirror stores plain RGBA8
    case 0x84EE: case 0x84ED: m.mVk = VK_FORMAT_R8G8B8A8_UNORM; return true;
    default:
        warn_once("internal format", (U32)f);
        return false;
    }
}

U32 type_bytes(GLenum type)
{
    switch (type)
    {
    case GL_UNSIGNED_BYTE: case GL_BYTE: return 1;
    case GL_UNSIGNED_SHORT: case GL_SHORT: case 0x140B: return 2;
    case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT: return 4;
    default: return 0;
    }
}

U32 format_components(GLenum f)
{
    switch (f)
    {
    case GL_RGBA: case 0x80E1: return 4;            // RGBA, BGRA
    case GL_RGB: case 0x80E0: return 3;             // RGB, BGR
    case GL_LUMINANCE_ALPHA: case 0x8227: return 2; // LUMINANCE_ALPHA, RG
    case GL_ALPHA: case GL_LUMINANCE: case 0x1903: case GL_DEPTH_COMPONENT: return 1;   // RED
    default: return 0;
    }
}

F32 half_to_float(U16 h)
{
    const U32 sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
    U32 f;
    if (exp == 0)
    {
        if (mant == 0) f = sign << 31;
        else
        {
            U32 e = 127 - 15 + 1, m = mant;
            while (!(m & 0x400)) { m <<= 1; --e; }
            f = (sign << 31) | (e << 23) | ((m & 0x3FF) << 13);
        }
    }
    else if (exp == 31) f = (sign << 31) | 0x7F800000 | (mant << 13);
    else f = (sign << 31) | ((exp - 15 + 127) << 23) | (mant << 13);
    F32 out;
    memcpy(&out, &f, 4);
    return out;
}

U16 float_to_half(F32 v)
{
    U32 f;
    memcpy(&f, &v, 4);
    const U32 sign = (f >> 16) & 0x8000;
    S32 exp = (S32)((f >> 23) & 0xFF) - 127 + 15;
    U32 mant = f & 0x7FFFFF;
    if (exp <= 0) return (U16)sign;
    if (exp >= 31) return (U16)(sign | 0x7C00);
    return (U16)(sign | (exp << 10) | (mant >> 13));
}

// Converts GL client pixel data into the tightly packed texels of the Vulkan format. Returns false if the
// combination is not handled.
bool convert_pixels(const FormatMap& fm, GLenum format, GLenum type, const void* src, U32 width, U32 height, U32 depth,
                    U32 alignment, U32 row_length, std::vector<U8>& out)
{
    const U32 comps = format_components(format);
    const U32 tb = type_bytes(type);
    if (!comps || !tb)
    {
        warn_once("pixel format/type", (format << 16) | type);
        return false;
    }
    const U32 src_row_texels = row_length ? row_length : width;
    U32 src_row_bytes = src_row_texels * comps * tb;
    src_row_bytes = (src_row_bytes + alignment - 1) / alignment * alignment;
    const U32 dst_texel = fm.mTexelBytes;

    // the common case, an 8-bit RGBA or RGB image into an RGBA8 texture: rows are copied or widened without any per-texel decisions
    if (!fm.mDepth && (fm.mVk == VK_FORMAT_R8G8B8A8_UNORM || fm.mVk == VK_FORMAT_R8G8B8A8_SRGB) && type == GL_UNSIGNED_BYTE
        && ((comps == 4 && format != 0x80E1) || (comps == 3 && format != 0x80E0)))
    {
        out.resize((size_t)width * height * depth * 4);
        const U8* fbase = (const U8*)src;
        for (U32 z = 0; z < depth; ++z)
        {
            for (U32 y = 0; y < height; ++y)
            {
                const U8* row = fbase + ((size_t)z * height + y) * src_row_bytes;
                U8* dst = out.data() + (((size_t)z * height + y) * width) * 4;
                if (comps == 4)
                {
                    memcpy(dst, row, (size_t)width * 4);
                }
                else
                {
                    for (U32 x = 0; x < width; ++x)
                    {
                        dst[x * 4 + 0] = row[x * 3 + 0];
                        dst[x * 4 + 1] = row[x * 3 + 1];
                        dst[x * 4 + 2] = row[x * 3 + 2];
                        dst[x * 4 + 3] = 255;
                    }
                }
            }
        }
        return true;
    }

    out.assign((size_t)width * height * depth * dst_texel, 0);

    const U8* base = (const U8*)src;
    for (U32 z = 0; z < depth; ++z)
    {
        for (U32 y = 0; y < height; ++y)
        {
            const U8* row = base + ((size_t)z * height + y) * src_row_bytes;
            U8* dst = out.data() + (((size_t)z * height + y) * width) * dst_texel;
            for (U32 x = 0; x < width; ++x)
            {
                const U8* p = row + (size_t)x * comps * tb;
                U8* d = dst + (size_t)x * dst_texel;
                if (fm.mDepth)
                {
                    F32 v = 0.f;
                    if (type == GL_FLOAT) memcpy(&v, p, 4);
                    else if (type == GL_UNSIGNED_INT) { U32 u; memcpy(&u, p, 4); v = (F32)((double)u / 4294967295.0); }
                    else if (type == GL_UNSIGNED_SHORT) { U16 u; memcpy(&u, p, 2); v = (F32)u / 65535.f; }
                    memcpy(d, &v, 4);
                    continue;
                }
                if (fm.mVk == VK_FORMAT_R8G8B8A8_UNORM || fm.mVk == VK_FORMAT_R8G8B8A8_SRGB)
                {
                    if (type != GL_UNSIGNED_BYTE) { warn_once("rgba8 source type", type); return false; }
                    switch (comps)
                    {
                    case 4: d[0] = p[0]; d[1] = p[1]; d[2] = p[2]; d[3] = p[3]; if (format == 0x80E1) std::swap(d[0], d[2]); break;
                    case 3: d[0] = p[0]; d[1] = p[1]; d[2] = p[2]; d[3] = 255; if (format == 0x80E0) std::swap(d[0], d[2]); break;
                    case 2: d[0] = p[0]; d[1] = p[0]; d[2] = p[0]; d[3] = p[1]; break;
                    default: d[0] = d[1] = d[2] = p[0]; d[3] = 255; break;
                    }
                }
                else if (fm.mVk == VK_FORMAT_R8_UNORM)
                {
                    d[0] = p[0];
                }
                else if (fm.mVk == VK_FORMAT_R8G8_UNORM)
                {
                    d[0] = p[0]; d[1] = comps > 1 ? p[1] : 255;
                }
                else if (fm.mVk == VK_FORMAT_R16G16B16A16_SFLOAT)
                {
                    U16 h[4] = { 0, 0, 0, 0x3C00 };
                    for (U32 c = 0; c < comps && c < 4; ++c)
                    {
                        if (type == GL_FLOAT) { F32 v; memcpy(&v, p + c * 4, 4); h[c] = float_to_half(v); }
                        else if (type == 0x140B) memcpy(&h[c], p + c * 2, 2);
                        else if (type == GL_UNSIGNED_BYTE) h[c] = float_to_half(p[c] / 255.f);
                        else { warn_once("rgba16f source type", type); return false; }
                    }
                    memcpy(d, h, 8);
                }
                else if (fm.mVk == VK_FORMAT_R32G32B32A32_SFLOAT)
                {
                    F32 v[4] = { 0, 0, 0, 1 };
                    for (U32 c = 0; c < comps && c < 4; ++c)
                    {
                        if (type == GL_FLOAT) memcpy(&v[c], p + c * 4, 4);
                        else if (type == 0x140B) { U16 h; memcpy(&h, p + c * 2, 2); v[c] = half_to_float(h); }
                        else if (type == GL_UNSIGNED_BYTE) v[c] = p[c] / 255.f;
                        else { warn_once("rgba32f source type", type); return false; }
                    }
                    memcpy(d, v, 16);
                }
                else if (fm.mVk == VK_FORMAT_R32_SFLOAT || fm.mVk == VK_FORMAT_R16_SFLOAT)
                {
                    F32 v = 0.f;
                    if (type == GL_FLOAT) memcpy(&v, p, 4);
                    else if (type == GL_UNSIGNED_BYTE) v = p[0] / 255.f;
                    if (fm.mVk == VK_FORMAT_R32_SFLOAT) memcpy(d, &v, 4);
                    else { U16 h = float_to_half(v); memcpy(d, &h, 2); }
                }
                else
                {
                    memcpy(d, p, std::min<U32>(dst_texel, comps * tb));     // packed formats: copy as is
                }
            }
        }
    }
    return true;
}

//-----------------------------------------------------------------------------------------------------
// state
//-----------------------------------------------------------------------------------------------------

struct SamplerParams
{
    GLenum mMinFilter = GL_NEAREST_MIPMAP_LINEAR;
    GLenum mMagFilter = GL_LINEAR;
    GLenum mWrapS = GL_REPEAT;
    GLenum mWrapT = GL_REPEAT;
    GLenum mWrapR = GL_REPEAT;
    F32 mAnisotropy = 1.f;
    bool mCompare = false;
    GLenum mCompareFunc = GL_LEQUAL;
    S32 mBaseLevel = 0;
    S32 mMaxLevel = 1000;
};

struct Tex
{
    Image mImage;
    GLenum mTarget = 0;                 // GL_TEXTURE_2D, GL_TEXTURE_CUBE_MAP, ...
    U32 mWidth = 0, mHeight = 0, mDepth = 0;
    U32 mLevels = 0;
    GLint mInternalFormat = 0;
    FormatMap mFormat;
    SamplerParams mSampler;
    U32 mValidLevels = 0;               // contiguous mip levels from 0 that hold data (the rest would sample as black)
    GLenum mSwizzle[4] = { GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA };      // GL_TEXTURE_SWIZZLE_R..A
    bool mSwizzleSet = false;
    std::map<std::pair<U32, U32>, std::unique_ptr<Image>> mViews;     // (level, layer) -> render-target view
    VkSampler mVkSampler = VK_NULL_HANDLE;      // cached sampler for the parameters hashed in mSamplerSig
    U64 mSamplerSig = 0;
};

VkComponentSwizzle swizzle_component(GLenum s)
{
    switch (s)
    {
    case GL_RED: return VK_COMPONENT_SWIZZLE_R;
    case GL_GREEN: return VK_COMPONENT_SWIZZLE_G;
    case GL_BLUE: return VK_COMPONENT_SWIZZLE_B;
    case GL_ALPHA: return VK_COMPONENT_SWIZZLE_A;
    case GL_ZERO: return VK_COMPONENT_SWIZZLE_ZERO;
    default: return VK_COMPONENT_SWIZZLE_ONE;
    }
}

VkComponentMapping swizzle_mapping(const Tex& t)
{
    return { swizzle_component(t.mSwizzle[0]), swizzle_component(t.mSwizzle[1]), swizzle_component(t.mSwizzle[2]), swizzle_component(t.mSwizzle[3]) };
}

// the sampling view of a texture whose swizzle changed: a Vulkan swizzle is part of the view, so make a new one
void rebuild_sampling_view(Tex& t)
{
    if (!t.mImage.valid() || !t.mImage.mView)
    {
        return;
    }
    Context& ctx = Context::instance();
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = t.mImage.mImage;
    vi.viewType = t.mImage.mViewType;
    vi.format = t.mImage.mFormat;
    vi.components = swizzle_mapping(t);
    vi.subresourceRange = { t.mImage.mAspect, 0, t.mImage.mMipLevels, 0, t.mImage.mLayers };
    VkImageView fresh = VK_NULL_HANDLE;
    if (vkCreateImageView(ctx.getDevice(), &vi, nullptr, &fresh) != VK_SUCCESS)
    {
        return;
    }
    VkImageView old = t.mImage.mView;
    VkDevice dev = ctx.getDevice();
    ctx.destroyLater([=]() { vkDestroyImageView(dev, old, nullptr); });
    t.mImage.mView = fresh;
}

struct Buf
{
    Buffer mVk;
    U64 mSize = 0;
    U64 mLastUseFrame = ~0ull;
    // Large vertex / index buffers live in plain video memory and are changed with GPU copies (Renderer::updateBuffer). Small and streaming
    // buffers (and uniform / pixel buffers) keep the host-visible path below.
    bool mDevicePath = false;
    bool mPending = false;              // device path: a write has not been recorded into the command stream yet
    // The Vulkan buffer lives in write-combined video memory, which is very slow to read from the CPU. This copy in ordinary memory
    // is what a renamed buffer is refilled from when only part of it is rewritten.
    std::vector<U8> mShadow;
};

struct VaoAttrib
{
    bool mEnabled = false;
    GLuint mBuffer = 0;
    GLint mSize = 4;
    GLenum mType = GL_FLOAT;
    bool mNormalized = false;
    bool mInteger = false;
    GLsizei mStride = 0;
    U64 mOffset = 0;
};

struct Vao
{
    VaoAttrib mAttrib[16];
    GLuint mElementBuffer = 0;
};

struct FboAttachment
{
    GLuint mTexture = 0;
    GLint mLevel = 0;
    U32 mLayer = 0;
};

struct Fbo
{
    FboAttachment mColor[8];
    FboAttachment mDepth;
    GLenum mDrawBuffers[8] = { GL_COLOR_ATTACHMENT0, 0, 0, 0, 0, 0, 0, 0 };
    U32 mDrawCount = 1;
    GLenum mReadBuffer = GL_COLOR_ATTACHMENT0;
};

// state that belongs to one GL context (the viewer has a texture worker thread with its own, sharing objects)
struct PerContext
{
    GLuint mBound[32][8] = {};                  // texture per unit and target class
    U32 mActiveUnit = 0;
    GLuint mDrawFbo = 0, mReadFbo = 0;
    GLuint mArrayBuffer = 0;
    GLuint mUniformBuffer = 0;
    GLuint mPixelUnpackBuffer = 0;
    GLuint mVao = 0;
    GLint mUnpackAlignment = 4;
    GLint mUnpackRowLength = 0;
    GLint mPackAlignment = 4;
    GLint mPackRowLength = 0;
    GLuint mPixelPackBuffer = 0;
};
thread_local PerContext sCtxTls;
thread_local PerContext* sCtxOverride = nullptr;      // set on the async worker: the context of the thread whose call is being replayed
inline PerContext& current_ctx() { return sCtxOverride ? *sCtxOverride : sCtxTls; }
#define sCtx (current_ctx())

struct GLState
{
    // objects (shared between contexts)
    std::unordered_map<GLuint, Tex> mTextures;
    std::unordered_map<GLuint, Fbo> mFbos;
    std::unordered_map<GLuint, Buf> mBuffers;
    std::unordered_map<GLuint, Vao> mVaos;
    F32 mConstAttrib[16][4];

    // draw state
    bool mDepthClamp = false;
    bool mBlend = false, mDepthTest = false, mCull = false, mScissorTest = false, mStencilTest = false, mPolyOffset = false;
    GLenum mBlendSrcRGB = GL_ONE, mBlendDstRGB = GL_ZERO, mBlendSrcA = GL_ONE, mBlendDstA = GL_ZERO;
    GLenum mDepthFunc = GL_LESS;
    bool mDepthMask = true;
    bool mColorMask[4] = { true, true, true, true };
    GLenum mCullFace = GL_BACK;
    F32 mOffsetFactor = 0.f, mOffsetUnits = 0.f;
    GLenum mPolygonMode = GL_FILL;
    F32 mLineWidth = 1.f;
    std::map<GLenum, bool> mOtherCaps;      // every other glEnable / glDisable capability the viewer has set
    S32 mViewport[4] = { 0, 0, 0, 0 };
    bool mViewportSet = false;
    S32 mScissor[4] = { 0, 0, 0, 0 };
    F32 mClearColor[4] = { 0, 0, 0, 0 };

    GLState()
    {
        for (auto& a : mConstAttrib) { a[0] = a[1] = a[2] = 0.f; a[3] = 1.f; }
    }
};

GLState sGL;
std::set<GLuint> sFrameTargets;         // textures rendered into during the current frame (for the debug dump)
GLuint sUboBinding[4] = {};             // GL buffer bound to each named uniform block binding point
std::map<GLuint, U32> sFrameSampled;    // textures read by draws this frame, with use counts
std::set<GLuint> sDumpTargets;          // the set captured for the dump that is pending
bool sDumpPending = false;
std::map<GLuint, U32> sFrameSampledLast;
U32 sDraws = 0, sDrawsSkipped = 0, sCopies = 0;
U64 sDrawsTotal = 0;
std::map<std::string, U32> sPhaseProgramDraws;      // LL_VK_PHASES: draws by program name, reported with the phase line
std::map<std::string, long long> sPhaseProgramTicks; // LL_VK_PHASES: the thread's time from the previous draw to this one (the state calls in between included), by program
std::map<std::string, long long> sPhaseTargetTicks;  // the same, by the render target being drawn into (one entry per shadow cascade, the G-buffer, ...)
std::map<std::string, U32> sPhaseTargetDraws;
long long sPhaseLastDrawTick = 0;

// LL_VK_PHASES: charges the time since the previous draw to the program of this one. Gaps over 2 ms (a frame boundary, a wait) are not counted.
void note_draw_time(const std::string& program_name)
{
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    if (sPhaseLastDrawTick)
    {
        const long long d = now.QuadPart - sPhaseLastDrawTick;
        if (d >= 0 && d < freq.QuadPart / 500)
        {
            sPhaseProgramTicks[program_name] += d;
            const std::string target = Renderer::instance().describeTarget();
            sPhaseTargetTicks[target] += d;
        }
    }
    sPhaseLastDrawTick = now.QuadPart;
}
std::map<std::pair<int, GLenum>, U64> sGetterCounts;    // LL_VK_PHASES: state getter calls by function and parameter
// draw outcomes of the current frame, by reason (and by program for the skipped ones)
struct DrawStats
{
    U32 mDone = 0, mNoProgram = 0, mNoTarget = 0, mNoVertexInput = 0, mNoElement = 0, mRendererRefused = 0, mNotInFrame = 0;
    std::map<std::string, U32> mDonePerProgram;
    std::map<std::string, U32> mSkippedPerProgram;
    std::map<std::string, std::string> mSamplerInfo;    // program + sampler -> what is bound there (lighting related programs only)
    std::map<std::string, U32> mFeedback;           // draws that sample a texture that is also a render target of the draw
    std::map<std::string, U32> mGraph;              // draws per "program -> targets <- sampled textures" (the frame's passes)
    std::vector<std::string> mGraphOrder;           // the keys of mGraph in order of first appearance
    std::vector<std::string> mBackbufferEvents;     // draws and clears that target the default framebuffer, in order
};
DrawStats sStats, sStatsLast;
bool sVerifySnapshotPending = false;   // a snapshot render is being mirrored: report on it when its off-screen frame ends
std::vector<GLuint> sCurTargets;        // textures attached to the current draw framebuffer
GLuint sRecentFrameTargets[16] = {};    // cheap filter in front of sFrameTargets: ids inserted recently this frame
U32 sCurTargetW = 1, sCurTargetH = 1;   // size of the first colour target of the current draw framebuffer
// the per-draw statistics / pass graph are only gathered when asked for (they cost string work on every draw)
const bool sStatsEnabled = getenv("LL_VK_STATS") != nullptr || getenv("LL_VK_DEBUG_SHADERS") != nullptr || getenv("LL_VK_VALIDATION") != nullptr;
bool sDbgMatch = false;                 // the draw being prepared belongs to a program the debug filter selects
bool sDbgNoDepth = false, sDbgNoDepthWrite = false, sDbgNoDepthTest = false, sDbgNoBlend = false, sDbgNoCull = false, sDbgNoScissor = false,
     sDbgFullMask = false, sDbgNoFeedback = false;      // debug draw state overrides from D:\claude\vk-debug.txt
int sDbgMode = 0;                       // debug shader mode from D:\claude\vk-debug.txt (0 = off)
std::string sDbgFilter;                 // programs whose name contains this get the mode
std::string sDbgSampler;                // sampler shown by mode 2
bool sHasDepth = false;             // does the current render target have a depth attachment

Vao& current_vao()
{
    return sGL.mVaos[sCtx.mVao];
}

U32 target_class(GLenum target)
{
    switch (target)
    {
    case GL_TEXTURE_2D: return 0;
    case GL_TEXTURE_CUBE_MAP: case 0x8515: case 0x8516: case 0x8517: case 0x8518: case 0x8519: case 0x851A: return 1;
    case GL_TEXTURE_CUBE_MAP_ARRAY: return 2;
    case GL_TEXTURE_2D_ARRAY: return 3;
    case GL_TEXTURE_3D: return 4;
    case 0x9100: return 5;      // TEXTURE_2D_MULTISAMPLE
    default: return 7;
    }
}

Tex* bound_texture(GLenum target)
{
    const GLuint name = sCtx.mBound[sCtx.mActiveUnit][target_class(target)];
    auto it = sGL.mTextures.find(name);
    return it == sGL.mTextures.end() ? nullptr : &it->second;
}

// which threads feed the mirror (LL_VK_VERIFY): counts by thread and hook name
std::map<std::thread::id, std::map<std::string, U64>> sThreadCalls;
std::thread::id sMainThread;

void note_thread(const char* what)
{
    if (!LLVkVerify::gEnabled) return;
    if (sMainThread == std::thread::id()) sMainThread = std::this_thread::get_id();      // the first hook to run is the main thread's
    ++sThreadCalls[std::this_thread::get_id()][what];
}

//-----------------------------------------------------------------------------------------------------
// textures
//-----------------------------------------------------------------------------------------------------

void destroy_tex_images(Tex& t)
{
    Context& ctx = Context::instance();
    VkDevice dev = ctx.getDevice();
    for (auto& kv : t.mViews)
    {
        VkImageView v = kv.second->mView;
        ctx.destroyLater([=]() { vkDestroyImageView(dev, v, nullptr); });
    }
    t.mViews.clear();
    ctx.destroyImage(t.mImage);
}

// (re)creates the Vulkan image for a texture whose level 0 was just specified
bool ensure_image(Tex& t, GLenum target, GLint internal_format, U32 w, U32 h, U32 depth)
{
    FormatMap fm;
    if (!map_internal_format(internal_format, fm))
    {
        if (sVerifySnapshotPending)
        {
            LL_INFOS("VkVerify") << "snapshot: texture " << (S32)w << "x" << (S32)h << " with GL internal format " << internal_format << " has no Vulkan format; it will not exist in Vulkan" << LL_ENDL;
        }
        return false;
    }
    const U32 cls = target_class(target);
    TexType type = TexType::Tex2D;
    U32 d = 1;
    switch (cls)
    {
    case 1: type = TexType::Cube; break;
    case 2: type = TexType::CubeArray; d = std::max<U32>(1, depth / 6); break;
    case 3: type = TexType::Tex2DArray; d = std::max<U32>(1, depth); break;
    case 4: type = TexType::Tex3D; d = std::max<U32>(1, depth); break;
    default: break;
    }
    const U32 gl_target_class = cls;
    GLenum canonical = GL_TEXTURE_2D;
    if (gl_target_class == 1) canonical = GL_TEXTURE_CUBE_MAP;
    else if (gl_target_class == 2) canonical = GL_TEXTURE_CUBE_MAP_ARRAY;
    else if (gl_target_class == 3) canonical = GL_TEXTURE_2D_ARRAY;
    else if (gl_target_class == 4) canonical = GL_TEXTURE_3D;

    if (t.mImage.valid() && t.mTarget == canonical && t.mWidth == w && t.mHeight == h && t.mDepth == depth
        && t.mInternalFormat == internal_format)
    {
        return true;        // same specification again: keep the image and just upload into it
    }
    if (t.mImage.valid())
    {
        if (sVerifySnapshotPending)
        {
            LL_INFOS("VkVerify") << "snapshot: texture re-specified from " << t.mWidth << "x" << t.mHeight << " fmt " << t.mInternalFormat << " to " << w << "x" << h << " fmt " << internal_format
                                 << ", Vulkan image recreated (anything drawn into it so far is lost)" << LL_ENDL;
        }
        destroy_tex_images(t);
    }

    TextureDesc td;
    td.mType = type;
    td.mWidth = w;
    td.mHeight = h;
    td.mDepth = d;
    td.mMips = cls == 4 ? mipLevelCount(std::max(w, h), depth) : mipLevelCount(w, h);
    td.mFormat = fm.mVk;
    td.mSwizzle = t.mSwizzleSet ? swizzle_mapping(t) : fm.mSwizzle;
    if (fm.mDepth)
    {
        td.mUsage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    else if (!fm.mCompressed)
    {
        td.mUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (LLVkDlss::ready() && (fm.mVk == VK_FORMAT_R8G8B8A8_UNORM || fm.mVk == VK_FORMAT_R16G16B16A16_SFLOAT))
        {
            td.mUsage |= VK_IMAGE_USAGE_STORAGE_BIT;        // DLSS writes its output into an image like this
        }
    }
    if (!Renderer::instance().createTexture(td, t.mImage))
    {
        return false;
    }
    for (auto& kv : sGL.mTextures)
    {
        if (&kv.second == &t)
        {
            Context::instance().setObjectName(VK_OBJECT_TYPE_IMAGE, (U64)t.mImage.mImage, "GL texture " + std::to_string(kv.first));
            break;
        }
    }
    t.mTarget = canonical;
    t.mWidth = w;
    t.mHeight = h;
    t.mDepth = depth;
    t.mValidLevels = 0;
    t.mLevels = td.mMips;
    t.mInternalFormat = internal_format;
    t.mFormat = fm;
    return true;
}

// source of client pixel data: either a pointer or an offset into the bound pixel unpack buffer
const void* pixel_source(const void* pixels)
{
    if (sCtx.mPixelUnpackBuffer)
    {
        auto it = sGL.mBuffers.find(sCtx.mPixelUnpackBuffer);
        if (it == sGL.mBuffers.end() || !it->second.mVk.mMapped)
        {
            return nullptr;
        }
        return (const U8*)it->second.mVk.mMapped + (size_t)(U64)pixels;
    }
    return pixels;
}

void upload_converted(Tex& t, GLenum target, GLint level, GLint x, GLint y, GLint z, U32 w, U32 h, U32 d, bool converted, std::vector<U8>& texels);

void upload_level(Tex& t, GLenum target, GLint level, GLint x, GLint y, GLint z, U32 w, U32 h, U32 d, GLenum format, GLenum type, const void* pixels)
{
    if (!t.mImage.valid() || level < 0 || (U32)level >= t.mLevels)
    {
        return;
    }
    if ((U32)level == t.mValidLevels)
    {
        ++t.mValidLevels;       // levels are normally defined in order, level 0 first
    }
    const void* src = pixel_source(pixels);
    if (!src)
    {
        return;     // nothing to upload (allocation only)
    }
    static thread_local std::vector<U8> texels;       // reused between uploads of one thread
    {
        // A large conversion takes milliseconds; the viewer's texture thread and its main thread both feed this lock, so a big image
        // is converted without holding it. Nothing the conversion reads is shared: the pixels belong to the caller, who is waiting.
        const FormatMap fm = t.mFormat;
        const U32 alignment = (U32)sCtx.mUnpackAlignment, row_length = (U32)sCtx.mUnpackRowLength;
        const VkImage handle = t.mImage.mImage;
        const GLuint name = sCtx.mBound[sCtx.mActiveUnit][target_class(target)];
        const bool unlocked = (U64)w * h * d >= 16384 && !sCtx.mPixelUnpackBuffer && !LLVkAsync::gEnabled;      // (the worker does not take the lock)
        unsigned depth_held = 0;
        if (unlocked)
        {
            depth_held = LLVkGL::gMutex.releaseAll();
        }
        const bool converted = convert_pixels(fm, format, type, src, w, h, d, alignment, row_length, texels);
        if (unlocked)
        {
            LLVkGL::gMutex.reacquire(depth_held);
            // the texture may have been deleted or replaced while the lock was free
            auto it = sGL.mTextures.find(name);
            if (it == sGL.mTextures.end() || it->second.mImage.mImage != handle)
            {
                return;
            }
            return upload_converted(it->second, target, level, x, y, z, w, h, d, converted, texels);
        }
        if (!converted)
        {
            return;
        }
    }
    upload_converted(t, target, level, x, y, z, w, h, d, true, texels);
}

void upload_converted(Tex& t, GLenum target, GLint level, GLint x, GLint y, GLint z, U32 w, U32 h, U32 d, bool converted, std::vector<U8>& texels)
{
    if (!converted || !t.mImage.valid())
    {
        return;
    }
    U32 layer = 0;
    if (target >= 0x8515 && target <= 0x851A)
    {
        layer = target - 0x8515;            // cube face
    }
    else if (t.mTarget == GL_TEXTURE_2D_ARRAY || t.mTarget == GL_TEXTURE_CUBE_MAP_ARRAY || t.mTarget == GL_TEXTURE_3D)
    {
        layer = (U32)z;
    }
    const U32 slices = t.mTarget == GL_TEXTURE_3D ? d : 1;
    if (t.mTarget == GL_TEXTURE_2D_ARRAY || t.mTarget == GL_TEXTURE_CUBE_MAP_ARRAY)
    {
        // several layers at once: upload one layer at a time
        const size_t layer_bytes = (size_t)w * h * t.mFormat.mTexelBytes;
        for (U32 l = 0; l < d; ++l)
        {
            Renderer::instance().uploadTexture(t.mImage, (U32)level, layer + l, (U32)x, (U32)y, w, h, 1, texels.data() + l * layer_bytes, layer_bytes);
        }
        return;
    }
    Renderer::instance().uploadTexture(t.mImage, (U32)level, layer, (U32)x, (U32)y, w, h, slices, texels.data(), texels.size());
}

// view of one level / layer of a texture, usable as a render target
Image* texture_view(GLuint name, GLint level, U32 layer)
{
    auto it = sGL.mTextures.find(name);
    if (it == sGL.mTextures.end() || !it->second.mImage.valid())
    {
        return nullptr;
    }
    Tex& t = it->second;
    auto key = std::make_pair((U32)level, layer);
    auto vit = t.mViews.find(key);
    if (vit != t.mViews.end())
    {
        return vit->second.get();
    }
    Context& ctx = Context::instance();
    auto view = std::make_unique<Image>();
    view->mImage = t.mImage.mImage;
    view->mFormat = t.mImage.mFormat;
    view->mAspect = t.mImage.mAspect;
    view->mExtent = { std::max<U32>(1, t.mImage.mExtent.width >> level), std::max<U32>(1, t.mImage.mExtent.height >> level), 1 };
    view->mMipLevels = 1;
    view->mLayers = 1;
    view->mOwner = &t.mImage;
    view->mBaseMip = (U32)level;
    view->mBaseLayer = layer;
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = t.mImage.mImage;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = t.mImage.mFormat;
    vi.subresourceRange = { t.mImage.mAspect, (U32)level, 1, layer, 1 };
    if (vkCreateImageView(ctx.getDevice(), &vi, nullptr, &view->mView) != VK_SUCCESS)
    {
        return nullptr;
    }
    view->mLayout = t.mImage.mLayout;
    Image* raw = view.get();
    t.mViews[key] = std::move(view);
    return raw;
}

//-----------------------------------------------------------------------------------------------------
// framebuffer / render target
//-----------------------------------------------------------------------------------------------------

Image* attachment_image(const FboAttachment& a)
{
    return a.mTexture ? texture_view(a.mTexture, a.mLevel, a.mLayer) : nullptr;
}

// the color image reads (copies, readbacks) come from
Image* read_image()
{
    if (sCtx.mReadFbo == 0)
    {
        return LLVkMirror::backbufferColor();
    }
    auto it = sGL.mFbos.find(sCtx.mReadFbo);
    if (it == sGL.mFbos.end())
    {
        return nullptr;
    }
    const U32 index = it->second.mReadBuffer >= GL_COLOR_ATTACHMENT0 ? it->second.mReadBuffer - GL_COLOR_ATTACHMENT0 : 0;
    return index < 8 ? attachment_image(it->second.mColor[index]) : nullptr;
}

// makes the renderer's render target match the GL draw framebuffer. Returns false if there is nothing to draw to.
// remembers that a texture was rendered into this frame; the same few textures come back on every draw, so look in a small cache first
static inline void note_frame_target(GLuint texture)
{
    GLuint& slot = sRecentFrameTargets[texture & 15];
    if (slot != texture)
    {
        slot = texture;
        sFrameTargets.insert(texture);
    }
}

bool sync_target()
{
    Renderer& r = Renderer::instance();
    sCurTargets.clear();
    if (sCtx.mDrawFbo == 0)
    {
        Image* color = LLVkMirror::backbufferColor();
        Image* depth = LLVkMirror::backbufferDepth();
        if (!color)
        {
            return false;
        }
        {
            // LL_VK_SIZELOG: what the viewer draws into the default framebuffer, logged when it changes
            static const bool size_log = getenv("LL_VK_SIZELOG") != nullptr;
            static S32 last[9] = { -1, -1, -1, -1, -1, -1, -1, -1, -1 };
            if (size_log)
            {
                // the scissor changes constantly (every UI element); only the viewport and the target size are worth a line
                const S32 now[9] = { sGL.mViewport[0], sGL.mViewport[1], sGL.mViewport[2], sGL.mViewport[3], 0, 0, 0, 0, (S32)color->mExtent.width };
                if (memcmp(now, last, sizeof(now)) != 0)
                {
                    memcpy(last, now, sizeof(now));
                    LL_INFOS("VkSize") << "drawing to the backbuffer " << color->mExtent.width << "x" << color->mExtent.height << ": viewport (" << now[0] << "," << now[1] << "," << now[2] << "," << now[3]
                                       << ") scissor (" << now[4] << "," << now[5] << "," << now[6] << "," << now[7] << ")" << LL_ENDL;
                }
            }
        }
        Image* colors[1] = { color };
        r.setRenderTarget(colors, 1, depth);
        sHasDepth = depth != nullptr;
        sCurTargetW = color->mExtent.width;
        sCurTargetH = color->mExtent.height;
        return true;
    }
    auto it = sGL.mFbos.find(sCtx.mDrawFbo);
    if (it == sGL.mFbos.end())
    {
        return false;
    }
    Fbo& fbo = it->second;
    Image* colors[8] = {};
    U32 count = 0;
    for (U32 i = 0; i < fbo.mDrawCount && i < 8; ++i)
    {
        const GLenum buf = fbo.mDrawBuffers[i];
        if (buf < GL_COLOR_ATTACHMENT0 || buf >= GL_COLOR_ATTACHMENT0 + 8)
        {
            break;
        }
        Image* img = attachment_image(fbo.mColor[buf - GL_COLOR_ATTACHMENT0]);
        if (!img)
        {
            break;
        }
        colors[count++] = img;
        note_frame_target(fbo.mColor[buf - GL_COLOR_ATTACHMENT0].mTexture);
        sCurTargets.push_back(fbo.mColor[buf - GL_COLOR_ATTACHMENT0].mTexture);
    }
    if (fbo.mDepth.mTexture)
    {
        note_frame_target(fbo.mDepth.mTexture);
        sCurTargets.push_back(fbo.mDepth.mTexture);
    }
    Image* depth = attachment_image(fbo.mDepth);
    if (sDbgMatch && sDbgNoDepth)
    {
        depth = nullptr;
    }
    if (!count && !depth)
    {
        return false;
    }
    r.setRenderTarget(colors, count, depth);
    sHasDepth = depth != nullptr;
    if (count)
    {
        sCurTargetW = colors[0]->mExtent.width;
        sCurTargetH = colors[0]->mExtent.height;
    }
    return true;
}

//-----------------------------------------------------------------------------------------------------
// state translation
//-----------------------------------------------------------------------------------------------------

VkCompareOp compare_op(GLenum f)
{
    switch (f)
    {
    case GL_NEVER: return VK_COMPARE_OP_NEVER;
    case GL_LESS: return VK_COMPARE_OP_LESS;
    case GL_EQUAL: return VK_COMPARE_OP_EQUAL;
    case GL_LEQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case GL_GREATER: return VK_COMPARE_OP_GREATER;
    case GL_NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
    case GL_GEQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

VkBlendFactor blend_factor(GLenum f)
{
    switch (f)
    {
    case GL_ZERO: return VK_BLEND_FACTOR_ZERO;
    case GL_ONE: return VK_BLEND_FACTOR_ONE;
    case GL_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
    case GL_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case GL_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
    case GL_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case GL_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
    case GL_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case GL_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
    case GL_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case GL_SRC_ALPHA_SATURATE: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: warn_once("blend factor", f); return VK_BLEND_FACTOR_ONE;
    }
}

VkPrimitiveTopology topology(GLenum mode)
{
    switch (mode)
    {
    case GL_POINTS: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case GL_LINES: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case GL_LINE_STRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case GL_TRIANGLES: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case GL_TRIANGLE_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case GL_TRIANGLE_FAN: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    default: warn_once("primitive mode", mode); return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

VkSamplerAddressMode wrap_mode(GLenum w)
{
    switch (w)
    {
    case GL_CLAMP_TO_EDGE: case GL_CLAMP: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 0x8370: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 0x812D: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

SamplerDesc sampler_for(const Tex& t)
{
    const SamplerParams& sp = t.mSampler;
    SamplerDesc d;
    d.mMag = sp.mMagFilter == GL_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    bool mipmapped = true;
    switch (sp.mMinFilter)
    {
    case GL_NEAREST: d.mMin = VK_FILTER_NEAREST; mipmapped = false; break;
    case GL_LINEAR: d.mMin = VK_FILTER_LINEAR; mipmapped = false; break;
    case GL_NEAREST_MIPMAP_NEAREST: d.mMin = VK_FILTER_NEAREST; d.mMip = VK_SAMPLER_MIPMAP_MODE_NEAREST; break;
    case GL_LINEAR_MIPMAP_NEAREST: d.mMin = VK_FILTER_LINEAR; d.mMip = VK_SAMPLER_MIPMAP_MODE_NEAREST; break;
    case GL_NEAREST_MIPMAP_LINEAR: d.mMin = VK_FILTER_NEAREST; d.mMip = VK_SAMPLER_MIPMAP_MODE_LINEAR; break;
    default: d.mMin = VK_FILTER_LINEAR; d.mMip = VK_SAMPLER_MIPMAP_MODE_LINEAR; break;
    }
    d.mAddressU = wrap_mode(sp.mWrapS);
    d.mAddressV = wrap_mode(sp.mWrapT);
    d.mAddressW = wrap_mode(sp.mWrapR);
    d.mAnisotropy = sp.mAnisotropy;
    d.mCompare = sp.mCompare;
    d.mCompareOp = compare_op(sp.mCompareFunc);
    const F32 last = (F32)std::max<S32>(0, (S32)std::min(t.mLevels, std::max<U32>(1, t.mValidLevels)) - 1);
    d.mMinLod = (F32)std::max(0, sp.mBaseLevel);
    d.mMaxLod = mipmapped ? std::min(last, (F32)sp.mMaxLevel) : d.mMinLod + 0.25f;
    if (!mipmapped)
    {
        // maxLod 0.25 with linear mip mode would blend a quarter of the next (unwritten) level into every minified sample
        d.mMip = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    }
    if (d.mMaxLod < d.mMinLod) d.mMaxLod = d.mMinLod;
    return d;
}

// the Vulkan sampler for a texture, rebuilt only when one of the inputs of sampler_for() changed
VkSampler texture_sampler(Renderer& r, Tex& t)
{
    const SamplerParams& sp = t.mSampler;
    U64 h = 1469598103934665603ull;
    auto mix = [&h](U64 v) { h = (h ^ v) * 1099511628211ull; };
    mix(sp.mMinFilter); mix(sp.mMagFilter); mix(sp.mWrapS); mix(sp.mWrapT); mix(sp.mWrapR); mix(sp.mCompare ? 1 : 0); mix(sp.mCompareFunc);
    mix((U64)(S64)sp.mBaseLevel); mix((U64)(S64)sp.mMaxLevel);
    U32 aniso_bits;
    memcpy(&aniso_bits, &sp.mAnisotropy, 4);
    mix(aniso_bits); mix(t.mLevels); mix(t.mValidLevels);
    if (!t.mVkSampler || h != t.mSamplerSig)
    {
        t.mVkSampler = r.getSampler(sampler_for(t));
        t.mSamplerSig = h;
    }
    return t.mVkSampler;
}

// translates the tracked GL state into renderer state for the next draw
void sync_draw_state(Renderer& r, bool has_depth)
{
    RasterState rs;
    if (sGL.mCull)
    {
        rs.mCull = sGL.mCullFace == GL_FRONT ? VK_CULL_MODE_FRONT_BIT : (sGL.mCullFace == GL_FRONT_AND_BACK ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_BACK_BIT);
    }
    rs.mPolygonMode = sGL.mPolygonMode == GL_LINE ? VK_POLYGON_MODE_LINE : (sGL.mPolygonMode == GL_POINT ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_FILL);
    rs.mLineWidth = sGL.mLineWidth;
    rs.mDepthBias = sGL.mPolyOffset;
    rs.mDepthClamp = sGL.mDepthClamp;
    rs.mBiasConstant = sGL.mOffsetUnits;
    rs.mBiasSlope = sGL.mOffsetFactor;
    // Vulkan's face-orientation area has the opposite sign of GL's for the same framebuffer coordinates (its y axis points down),
    // and the mirror keeps GL's positive viewport, so GL's counter-clockwise front faces are clockwise here
    rs.mFrontFace = VK_FRONT_FACE_CLOCKWISE;
    if (sDbgMatch && sDbgNoCull)
    {
        rs.mCull = VK_CULL_MODE_NONE;
    }
    r.setRasterState(rs);

    DepthStencilState ds;
    ds.mDepthTest = sGL.mDepthTest && has_depth;
    // GL never writes depth while the depth test is disabled; Vulkan does when writes are enabled
    ds.mDepthWrite = sGL.mDepthMask && sGL.mDepthTest && has_depth;
    ds.mDepthOp = compare_op(sGL.mDepthFunc);
    if (sDbgMatch)
    {
        if (sDbgNoDepthWrite) ds.mDepthWrite = false;
        if (sDbgNoDepthTest) ds.mDepthTest = false;
    }
    ds.mStencilTest = false;        // the mirror's depth images carry no stencil yet
    r.setDepthStencilState(ds);

    BlendState bs;
    bs.mEnable = sGL.mBlend;
    bs.mSrcColor = blend_factor(sGL.mBlendSrcRGB);
    bs.mDstColor = blend_factor(sGL.mBlendDstRGB);
    bs.mSrcAlpha = blend_factor(sGL.mBlendSrcA);
    bs.mDstAlpha = blend_factor(sGL.mBlendDstA);
    bs.mWriteMask = (sGL.mColorMask[0] ? VK_COLOR_COMPONENT_R_BIT : 0) | (sGL.mColorMask[1] ? VK_COLOR_COMPONENT_G_BIT : 0)
        | (sGL.mColorMask[2] ? VK_COLOR_COMPONENT_B_BIT : 0) | (sGL.mColorMask[3] ? VK_COLOR_COMPONENT_A_BIT : 0);
    if (sDbgMatch)
    {
        if (sDbgNoBlend) bs.mEnable = false;
        if (sDbgFullMask) bs.mWriteMask = 0xF;
    }
    r.setBlendState(bs);

    if (sGL.mViewportSet)
    {
        r.setViewport(sGL.mViewport[0], sGL.mViewport[1], sGL.mViewport[2], sGL.mViewport[3]);
    }
    const bool scissor_on = sGL.mScissorTest && !(sDbgMatch && sDbgNoScissor);
    r.setScissorEnabled(scissor_on);
    if (scissor_on)
    {
        r.setScissor(sGL.mScissor[0], sGL.mScissor[1], sGL.mScissor[2], sGL.mScissor[3]);
    }
}

// binds the textures the program's samplers read
void sync_textures(Renderer& r, Program& program)
{
    const ProgramInfo& info = program.getInfo();
    for (size_t k = 0; k < info.mSamplers.size(); ++k)
    {
        const U32 unit = program.getSamplerUnit(k);
        if (unit >= 32)
        {
            continue;
        }
        const U32 cls = program.getSamplerClass(k);
        auto it = sGL.mTextures.find(sCtx.mBound[unit][cls]);
        if (sStatsEnabled)
        {
            const std::string& type = info.mSamplers[k].mType;
            static const char* const watch[] = { "Soften", "Windlight Sky", "Deferred Sun", "Deferred Light Shader", "Irradiance Gen", "Radiance Gen", "HUD", "Copy Depth", "Haze", "Tonemap", "Reflection Mip", "Glow" };
            bool wanted = false;
            for (const char* w : watch) if (program.getName().find(w) != std::string::npos) wanted = true;
            if (wanted)
            {
                std::string what = "unit " + std::to_string(unit) + " tex " + std::to_string(sCtx.mBound[unit][cls]) + " ";
                if (it == sGL.mTextures.end()) what += "UNBOUND";
                else if (!it->second.mImage.valid()) what += "NO IMAGE";
                else
                {
                    const Tex& tx = it->second;
                    what += std::to_string(tx.mImage.mExtent.width) + "x" + std::to_string(tx.mImage.mExtent.height) + " vkfmt " + std::to_string((S32)tx.mImage.mFormat)
                          + " levels " + std::to_string(tx.mValidLevels) + "/" + std::to_string(tx.mLevels) + " layers " + std::to_string(tx.mDepth)
                          + " compare " + std::to_string(tx.mSampler.mCompare ? 1 : 0);
                    const SamplerDesc sd = sampler_for(tx);
                    what += std::string(" mag ") + (sd.mMag == VK_FILTER_LINEAR ? "lin" : "nearest") + " min " + (sd.mMin == VK_FILTER_LINEAR ? "lin" : "nearest")
                          + " mip " + (sd.mMip == VK_SAMPLER_MIPMAP_MODE_LINEAR ? "lin" : "nearest") + " lod " + std::to_string(sd.mMinLod) + ".." + std::to_string(sd.mMaxLod)
                          + " wrap " + std::to_string((S32)sd.mAddressU) + "," + std::to_string((S32)sd.mAddressV) + " glminfilter " + std::to_string(tx.mSampler.mMinFilter);
                }
                sStats.mSamplerInfo[program.getName() + " :: " + info.mSamplers[k].mName + " (" + type + ")"] = what;
            }
        }
        if (it != sGL.mTextures.end() && it->second.mImage.valid())
        {
            r.setTexture(unit, &it->second.mImage, texture_sampler(r, it->second));
            if (sStatsEnabled)
            {
                ++sFrameSampled[it->first];
                if (std::find(sCurTargets.begin(), sCurTargets.end(), it->first) != sCurTargets.end())
                {
                    ++sStats.mFeedback[program.getName() + " samples " + info.mSamplers[k].mName + " = tex " + std::to_string(it->first)];
                }
            }
        }
        else
        {
            r.setTexture(unit, nullptr, VK_NULL_HANDLE);
        }
    }
}

//-----------------------------------------------------------------------------------------------------
// buffers and vertex input
//-----------------------------------------------------------------------------------------------------

// numeric_class: 'f' float, 'i' signed integer, 'u' unsigned integer (what the vertex shader declares)
VkFormat vertex_format(GLint size, GLenum type, bool normalized, bool integer, char numeric_class)
{
    const U32 n = (U32)std::max(1, std::min(size, 4));
    if (numeric_class == 'i' || numeric_class == 'u')
    {
        // the shader reads integers: pick the integer format of the right signedness for the stored width
        const bool sign = numeric_class == 'i';
        const VkFormat b8s[4] = { VK_FORMAT_R8_SINT, VK_FORMAT_R8G8_SINT, VK_FORMAT_R8G8B8_SINT, VK_FORMAT_R8G8B8A8_SINT };
        const VkFormat b8u[4] = { VK_FORMAT_R8_UINT, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8A8_UINT };
        const VkFormat b16s[4] = { VK_FORMAT_R16_SINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16B16_SINT, VK_FORMAT_R16G16B16A16_SINT };
        const VkFormat b16u[4] = { VK_FORMAT_R16_UINT, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16A16_UINT };
        const VkFormat b32s[4] = { VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32A32_SINT };
        const VkFormat b32u[4] = { VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT };
        switch (type)
        {
        case GL_BYTE: case GL_UNSIGNED_BYTE: return sign ? b8s[n - 1] : b8u[n - 1];
        case GL_SHORT: case GL_UNSIGNED_SHORT: return sign ? b16s[n - 1] : b16u[n - 1];
        default: return sign ? b32s[n - 1] : b32u[n - 1];
        }
    }
    switch (type)
    {
    case GL_FLOAT:
    {
        const VkFormat f[4] = { VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT };
        return f[n - 1];
    }
    case 0x140B:
    {
        const VkFormat f[4] = { VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT };
        return f[n - 1];
    }
    case GL_UNSIGNED_BYTE:
    {
        const VkFormat un[4] = { VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };
        const VkFormat ui[4] = { VK_FORMAT_R8_UINT, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8A8_UINT };
        const VkFormat us[4] = { VK_FORMAT_R8_USCALED, VK_FORMAT_R8G8_USCALED, VK_FORMAT_R8G8B8_USCALED, VK_FORMAT_R8G8B8A8_USCALED };
        return integer ? ui[n - 1] : (normalized ? un[n - 1] : us[n - 1]);
    }
    case GL_BYTE:
    {
        const VkFormat sn[4] = { VK_FORMAT_R8_SNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8A8_SNORM };
        const VkFormat si[4] = { VK_FORMAT_R8_SINT, VK_FORMAT_R8G8_SINT, VK_FORMAT_R8G8B8_SINT, VK_FORMAT_R8G8B8A8_SINT };
        return integer ? si[n - 1] : sn[n - 1];
    }
    case GL_UNSIGNED_SHORT:
    {
        const VkFormat un[4] = { VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16A16_UNORM };
        const VkFormat ui[4] = { VK_FORMAT_R16_UINT, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16A16_UINT };
        const VkFormat us[4] = { VK_FORMAT_R16_USCALED, VK_FORMAT_R16G16_USCALED, VK_FORMAT_R16G16B16_USCALED, VK_FORMAT_R16G16B16A16_USCALED };
        return integer ? ui[n - 1] : (normalized ? un[n - 1] : us[n - 1]);
    }
    case GL_SHORT:
    {
        const VkFormat sn[4] = { VK_FORMAT_R16_SNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16A16_SNORM };
        const VkFormat si[4] = { VK_FORMAT_R16_SINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16B16_SINT, VK_FORMAT_R16G16B16A16_SINT };
        const VkFormat ss[4] = { VK_FORMAT_R16_SSCALED, VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R16G16B16_SSCALED, VK_FORMAT_R16G16B16A16_SSCALED };
        return integer ? si[n - 1] : (normalized ? sn[n - 1] : ss[n - 1]);
    }
    case GL_UNSIGNED_INT:
    {
        const VkFormat ui[4] = { VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT };
        return ui[n - 1];
    }
    case GL_INT:
    {
        const VkFormat si[4] = { VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32A32_SINT };
        return si[n - 1];
    }
    default:
        warn_once("vertex attribute type", type);
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

U32 attrib_type_bytes(GLenum type)
{
    switch (type)
    {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: case 0x140B: return 2;
    default: return 4;
    }
}

// Builds the vertex input for the current VAO and the program's attributes.
bool build_vertex_input(Renderer& r, Program& program, VertexInput& vi, std::vector<GLuint>& used_buffers)
{
    Vao& vao = current_vao();
    const ProgramInfo& info = program.getInfo();
    // bindings made so far: buffer, stride, byte offset of the binding inside the buffer
    struct BindingKey { GLuint mBuffer; GLsizei mStride; U64 mBase; U32 mBinding; };
    BindingKey made[16];
    U32 made_count = 0;

    for (size_t a = 0; a < info.mAttributes.size(); ++a)
    {
        const U32 loc = info.mAttributeLocations[a];
        if (loc >= 16)
        {
            continue;
        }
        VaoAttrib& at = vao.mAttrib[loc];
        auto bit = sGL.mBuffers.find(at.mBuffer);
        if (at.mEnabled && at.mBuffer && bit != sGL.mBuffers.end() && bit->second.mVk.valid())
        {
            const U32 tb = attrib_type_bytes(at.mType);
            const GLsizei stride = at.mStride ? at.mStride : (GLsizei)(tb * (U32)at.mSize);
            // Vulkan limits attribute offsets to a couple of KB. An offset at least as large as the stride is the
            // start of a separate array inside the buffer: it becomes the binding's offset instead.
            const bool separate_array = at.mOffset >= (U64)stride;
            const U64 base = separate_array ? at.mOffset : 0;
            const U32 attr_offset = separate_array ? 0 : (U32)at.mOffset;
            U32 binding = ~0u;
            for (U32 m = 0; m < made_count; ++m)
            {
                if (made[m].mBuffer == at.mBuffer && made[m].mStride == stride && made[m].mBase == base)
                {
                    binding = made[m].mBinding;
                    break;
                }
            }
            if (binding == ~0u)
            {
                binding = (U32)vi.mBindings.size();
                VertexInput::Binding b;
                b.mBuffer = bit->second.mVk.mBuffer;
                b.mOffset = base;
                b.mStride = (U32)stride;
                vi.mBindings.push_back(b);
                if (made_count < 16) made[made_count++] = { at.mBuffer, stride, base, binding };
                used_buffers.push_back(at.mBuffer);
            }
            VertexInput::Attribute va;
            va.mLocation = loc;
            va.mBinding = binding;
            const std::string& stype = info.mAttributeTypes[a];
            const char numeric = stype.empty() ? 'f' : (stype[0] == 'u' ? 'u' : (stype[0] == 'i' ? 'i' : 'f'));
            va.mFormat = vertex_format(at.mSize, at.mType, at.mNormalized, at.mInteger, numeric);
            va.mOffset = attr_offset;
            vi.mAttributes.push_back(va);
        }
        else
        {
            // attribute array disabled: the shader reads the constant set with glVertexAttrib4f
            Renderer::ScratchAlloc sa = r.allocVertexScratch(16);
            if (!sa.mPtr)
            {
                return false;
            }
            memcpy(sa.mPtr, sGL.mConstAttrib[loc], 16);
            VertexInput::Binding b;
            b.mBuffer = sa.mBuffer;
            b.mOffset = sa.mOffset;
            b.mStride = 0;
            const U32 binding = (U32)vi.mBindings.size();
            vi.mBindings.push_back(b);
            VertexInput::Attribute va;
            va.mLocation = loc;
            va.mBinding = binding;
            va.mFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
            va.mOffset = 0;
            vi.mAttributes.push_back(va);
        }
    }
    return vi.mBindings.size() <= 16;
}


//-----------------------------------------------------------------------------------------------------
// per-pass snapshots (debug): after the passes that matter, sampled rows of their render targets are read back from OpenGL and
// recorded for Vulkan, so the first pass where the two disagree can be named
//-----------------------------------------------------------------------------------------------------

struct Snap
{
    U32 mIndex = 0;
    std::string mLabel;
    GLuint mTex = 0;
    U32 mW = 0, mH = 0;
    VkFormat mFmt = VK_FORMAT_UNDEFINED;
    Buffer mBuf;
    std::vector<F32> mGl;       // rows * w * 4 floats
    U32 mRows = 0;
};

std::vector<Snap> sSnaps;
bool sSnapActive = false;
U32 sSnapStage = 0;
U32 sSnapCounter = 0;
std::string sSnapKey;
std::map<GLuint, std::string> sSnapPending;    // colour targets of the pass in progress -> programs that drew into them

std::vector<std::string> sSnapExtra;    // extra program name substrings from D:\claude\vk-snap-filter.txt ("*" = every program)

bool snap_watched(const std::string& name)
{
    for (const std::string& e : sSnapExtra)
    {
        if (e == "*" ? (name != "UI Shader") : (name.find(e) != std::string::npos)) return true;
    }
    static const char* const watch[] = { "Soften", "Sun", "Light", "Windlight", "Tonemap", "Exposure", "Luminance", "Haze", "Blur", "Gaussian", "Copy",
                                         "Reflection Mip", "Radiance", "Irradiance", "SMAA", "Glow", "Post", "Sharpening", "Deferred Emissive", "PBR Opaque", "Material Shader 8", "HUD" };
    for (const char* w : watch) if (name.find(w) != std::string::npos) return true;
    return false;
}

bool decode_texel(VkFormat f, const U8* p, double rgb[4])
{
    rgb[0] = rgb[1] = rgb[2] = 0;
    rgb[3] = 1;
    switch (f)
    {
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
        for (int c = 0; c < 4; ++c) rgb[c] = p[c] / 255.0;
        return true;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        for (int c = 0; c < 4; ++c) { U16 h; memcpy(&h, p + c * 2, 2); rgb[c] = half_to_float(h); }
        return true;
    case VK_FORMAT_R16G16B16A16_UNORM:
        for (int c = 0; c < 4; ++c) { U16 h; memcpy(&h, p + c * 2, 2); rgb[c] = h / 65535.0; }
        return true;
    case VK_FORMAT_R16G16_SFLOAT:
        for (int c = 0; c < 2; ++c) { U16 h; memcpy(&h, p + c * 2, 2); rgb[c] = half_to_float(h); }
        return true;
    case VK_FORMAT_R16_SFLOAT:
        { U16 h; memcpy(&h, p, 2); rgb[0] = half_to_float(h); }
        return true;
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    {
        U32 packed; memcpy(&packed, p, 4);
        for (int c = 0; c < 3; ++c)
        {
            const U32 mant_bits = c == 2 ? 5 : 6;
            const U32 field = c == 0 ? (packed & 0x7FF) : c == 1 ? ((packed >> 11) & 0x7FF) : ((packed >> 22) & 0x3FF);
            const U32 e = field >> mant_bits, m = field & ((1u << mant_bits) - 1);
            rgb[c] = e == 0 ? (m / (double)(1u << mant_bits)) * pow(2.0, -14) : (1.0 + m / (double)(1u << mant_bits)) * pow(2.0, (double)e - 15);
        }
        return true;
    }
    default:
        return false;
    }
}

void snap_flush_pending()
{
    if (sSnapPending.empty())
    {
        return;
    }
    Context& ctx = Context::instance();
    Renderer& r = Renderer::instance();
    for (auto& kv : sSnapPending)
    {
        if (sSnaps.size() >= 80 || !glGetTextureSubImage)
        {
            break;
        }
        auto it = sGL.mTextures.find(kv.first);
        if (it == sGL.mTextures.end() || !it->second.mImage.valid())
        {
            continue;
        }
        Tex& t = it->second;
        const VkFormat vf = t.mImage.mFormat;
        double probe[4];
        U8 zero[16] = {};
        const FormatInfo fi = formatInfo(vf);
        if (!decode_texel(vf, zero, probe) || !fi.mKnown || t.mImage.mExtent.width == 0)
        {
            continue;
        }
        Snap s;
        s.mIndex = ++sSnapCounter;
        s.mLabel = kv.second;
        s.mTex = kv.first;
        s.mW = t.mImage.mExtent.width;
        s.mH = t.mImage.mExtent.height;
        s.mFmt = vf;
        s.mRows = (U64)s.mW * s.mH <= 700 * 700 ? s.mH : 48;      // small targets in full, large ones as a sparse set of rows
        std::vector<U32> rows;
        for (U32 i = 0; i < s.mRows; ++i) rows.push_back(std::min(s.mH - 1, (U32)((U64)i * s.mH / s.mRows + s.mH / (2 * s.mRows))));
        if (!ctx.createBuffer((VkDeviceSize)s.mRows * s.mW * fi.mBytesPerBlock, VK_BUFFER_USAGE_TRANSFER_DST_BIT, Context::Memory::Readback, s.mBuf))
        {
            continue;
        }
        if (!r.recordRowCopies(t.mImage, rows, s.mBuf.mBuffer))
        {
            ctx.destroyBuffer(s.mBuf);
            continue;
        }
        s.mGl.assign((size_t)s.mRows * s.mW * 4, 0.f);
        for (U32 i = 0; i < s.mRows; ++i)
        {
            glGetTextureSubImage(kv.first, 0, 0, (GLint)rows[i], 0, (GLsizei)s.mW, 1, 1, GL_RGBA, GL_FLOAT, (GLsizei)(s.mW * 16), &s.mGl[(size_t)i * s.mW * 4]);
        }
        sSnaps.push_back(std::move(s));
    }
    sSnapPending.clear();
}

// debug: compares the draw state the mirror tracked with what OpenGL really has right now (finds state calls the mirror does not see)
void state_check(const std::string& program)
{
    static std::set<std::string> reported;
    auto report = [&](const char* what, long tracked, long real) {
        if (tracked == real) return;
        const std::string key = program + "|" + what;
        if (!reported.insert(key).second) return;
        LL_INFOS("VkGL") << "STATE MISMATCH in '" << program << "': " << what << " tracked " << tracked << " GL " << real << LL_ENDL;
    };
    GLint v[4];
    report("blend", sGL.mBlend, glIsEnabled(GL_BLEND) ? 1 : 0);
    glGetIntegerv(GL_BLEND_SRC_RGB, v); report("blend src rgb", sGL.mBlendSrcRGB, v[0]);
    glGetIntegerv(GL_BLEND_DST_RGB, v); report("blend dst rgb", sGL.mBlendDstRGB, v[0]);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, v); report("blend src alpha", sGL.mBlendSrcA, v[0]);
    glGetIntegerv(GL_BLEND_DST_ALPHA, v); report("blend dst alpha", sGL.mBlendDstA, v[0]);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, v); report("blend equation rgb (tracked: add)", GL_FUNC_ADD, v[0]);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, v); report("blend equation alpha (tracked: add)", GL_FUNC_ADD, v[0]);
    GLboolean cm[4];
    glGetBooleanv(GL_COLOR_WRITEMASK, cm);
    for (int i = 0; i < 4; ++i) report("color mask", sGL.mColorMask[i] ? 1 : 0, cm[i] ? 1 : 0);
    report("depth test", sGL.mDepthTest, glIsEnabled(GL_DEPTH_TEST) ? 1 : 0);
    GLboolean dm;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &dm); report("depth mask", sGL.mDepthMask ? 1 : 0, dm ? 1 : 0);
    glGetIntegerv(GL_DEPTH_FUNC, v); report("depth func", sGL.mDepthFunc, v[0]);
    report("cull", sGL.mCull, glIsEnabled(GL_CULL_FACE) ? 1 : 0);
    glGetIntegerv(GL_CULL_FACE_MODE, v); report("cull mode", sGL.mCullFace, v[0]);
    report("scissor test", sGL.mScissorTest, glIsEnabled(GL_SCISSOR_TEST) ? 1 : 0);
    report("depth clamp", sGL.mDepthClamp, glIsEnabled(GL_DEPTH_CLAMP) ? 1 : 0);
    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    for (int i = 0; i < 4; ++i) report("viewport", sGL.mViewport[i], vp[i]);
    glGetIntegerv(GL_SCISSOR_BOX, vp);
    for (int i = 0; i < 4; ++i) report("scissor box", sGL.mScissor[i], vp[i]);
    GLint fbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    report("draw framebuffer", sCtx.mDrawFbo, fbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &fbo);
}

// called for every mirrored draw
void snap_track(const std::string& program)
{
    if (snap_watched(program))
    {
        state_check(program);
    }
    std::string key = sCtx.mDrawFbo == 0 ? "S" : "F";
    for (GLuint n : sCurTargets) key += std::to_string(n) + ",";
    if (key != sSnapKey)
    {
        snap_flush_pending();
        sSnapKey = key;
    }
    if (!snap_watched(program) || sCtx.mDrawFbo == 0)
    {
        return;
    }
    for (GLuint n : sCurTargets)
    {
        auto it = sGL.mTextures.find(n);
        if (it == sGL.mTextures.end() || !it->second.mImage.valid() || it->second.mImage.mAspect != VK_IMAGE_ASPECT_COLOR_BIT)
        {
            continue;
        }
        std::string& label = sSnapPending[n];
        if (label.find(program) == std::string::npos && label.size() < 120)
        {
            label += (label.empty() ? "" : " + ") + program;
        }
    }
}

void snap_evaluate()
{
    if (sSnaps.empty())
    {
        return;
    }
    Context::instance().waitIdle();
    for (Snap& s : sSnaps)
    {
        const FormatInfo fi = formatInfo(s.mFmt);
        double gsum[4] = {}, vsum[4] = {}, diff = 0;
        size_t n = 0;
        for (U32 i = 0; i < s.mRows; ++i)
        {
            for (U32 x = 0; x < s.mW; x += (s.mRows == s.mH ? 1 : 4))
            {
                double rgb[4];
                if (!decode_texel(s.mFmt, (const U8*)s.mBuf.mMapped + ((size_t)i * s.mW + x) * fi.mBytesPerBlock, rgb)) continue;
                const F32* g = &s.mGl[((size_t)i * s.mW + x) * 4];
                for (int c = 0; c < 4; ++c)
                {
                    gsum[c] += g[c]; vsum[c] += rgb[c];
                    if (c < 3) diff += fabs(g[c] - rgb[c]);
                }
                ++n;
            }
        }
        if (n)
        {
            LL_INFOS("VkGL") << "SNAP #" << s.mIndex << " tex " << s.mTex << " " << s.mW << "x" << s.mH << " vkfmt " << (S32)s.mFmt << " [" << s.mLabel << "]: GL mean ("
                             << gsum[0] / n << "," << gsum[1] / n << "," << gsum[2] / n << " a " << gsum[3] / n << ") VK mean (" << vsum[0] / n << "," << vsum[1] / n << ","
                             << vsum[2] / n << " a " << vsum[3] / n << ") absdiff " << diff / (n * 3) << LL_ENDL;
        }
        Context::instance().destroyBuffer(s.mBuf);
    }
    sSnaps.clear();
}

} // anonymous namespace

// the mirror is about to close the frame: the last pass of the frame gets its snapshot
namespace { void flush_buffer_updates(); }

void before_end_frame()
{
    std::lock_guard<LLVkMirror::Mutex> lock(gMutex);
    flush_buffer_updates();
    if (sSnapActive)
    {
        snap_flush_pending();
        sSnapKey.clear();
    }
}

void after_offscreen()
{
    std::lock_guard<LLVkMirror::Mutex> lock(gMutex);
    if (sSnapActive)
    {
        snap_evaluate();
        if (++sSnapStage >= 2) { sSnapActive = false; sSnapStage = 0; }
    }
    if (sVerifySnapshotPending)
    {
        // the off-screen frame held the snapshot's render: make the next frame report on it (draw stats, pass graph,
        // texture comparisons incl. the probe arrays, render target dumps), as the vk-dump.trigger path would
        sVerifySnapshotPending = false;
        sStatsLast = sStats;
        sStats = DrawStats();
        sDumpTargets = sFrameTargets;
        sFrameSampledLast = sFrameSampled;
        sDumpPending = true;
        LL_INFOS("VkVerify") << "snapshot frame finished: full report armed (" << sDumpTargets.size() << " render targets)" << LL_ENDL;
    }
}


namespace
{

// hands the bound named uniform blocks to the renderer
void sync_uniform_blocks(Renderer& r, std::vector<GLuint>& used_buffers)
{
    for (U32 i = 0; i < 4; ++i)
    {
        auto it = sUboBinding[i] ? sGL.mBuffers.find(sUboBinding[i]) : sGL.mBuffers.end();
        if (it != sGL.mBuffers.end() && it->second.mVk.valid())
        {
            r.setUniformBuffer(i, it->second.mVk.mBuffer, it->second.mVk.mSize);
            used_buffers.push_back(it->first);
        }
        else
        {
            r.setUniformBuffer(i, VK_NULL_HANDLE, 0);
        }
    }
}

// common work before a draw; returns false if the draw cannot be mirrored
bool prepare_draw(VertexInput& vi, std::vector<GLuint>& used_buffers)
{
    LLVkMirror::gLastLap = 200;
    if (!LLVkMirror::inFrame())
    {
        ++sStats.mNotInFrame;
        return false;
    }
    Renderer& r = Renderer::instance();
    Program* program = r.getProgram();
    if (!program || !program->isValid())
    {
        ++sDrawsSkipped;
        ++sStats.mNoProgram;
        return false;
    }
    if (LLVkMirror::benchPhasesActive())
    {
        LLVkMirror::benchPass(program->getName().c_str());
    }
    sDbgMatch = !sDbgFilter.empty() && program->getName().find(sDbgFilter) != std::string::npos;
    LLVkMirror::gLastLap = 201;
    {
        LLVkMirror::PhaseTimer phase(LLVkMirror::PH_D_TARGET);
        if (!sync_target())
        {
            ++sDrawsSkipped;
            ++sStats.mNoTarget;
            ++sStats.mSkippedPerProgram[program->getName() + " (no target)"];
            return false;
        }
    }
    LLVkMirror::gLastLap = 202;
    {
        LLVkMirror::PhaseTimer phase(LLVkMirror::PH_D_STATE);
        sync_draw_state(r, sHasDepth);
    }
    LLVkMirror::gLastLap = 203;
    {
        LLVkMirror::PhaseTimer phase(LLVkMirror::PH_D_TEX);
        sync_textures(r, *program);
    }
    LLVkMirror::gLastLap = 204;
    if (sSnapActive)
    {
        snap_track(program->getName());
    }
    if (debugShadersEnabled() && program->hasUniform("ll_dbg_mode"))
    {
        S32 mode = 0, sampler = 0;
        if (sDbgMode && program->getName().find(sDbgFilter) != std::string::npos)
        {
            mode = sDbgMode;
            if (const std::vector<std::string>* names = findDebugSamplers(program->getName()))
            {
                auto sit = std::find(names->begin(), names->end(), sDbgSampler);
                sampler = sit == names->end() ? 0 : (S32)(sit - names->begin());
            }
        }
        const F32 size[2] = { (F32)sCurTargetW, (F32)sCurTargetH };
        program->setInts("ll_dbg_mode", &mode, 1);
        program->setInts("ll_dbg_sampler", &sampler, 1);
        program->setFloats("ll_dbg_size", size, 2);
    }
    if (sStatsEnabled)
    {
        LLVkMirror::PhaseTimer phase(LLVkMirror::PH_D_STATS);
        std::string key = program->getName() + " -> ";
        if (sCtx.mDrawFbo == 0) key += "SCREEN";
        for (GLuint tname : sCurTargets) key += std::to_string(tname) + " ";
        key += "<- ";
        const ProgramInfo& gi = program->getInfo();
        for (size_t k = 0; k < gi.mSamplers.size(); ++k)
        {
            const U32 unit = program->getSamplerUnit(k);
            if (unit >= 32) continue;
            GLuint bound = 0;
            for (U32 c = 0; c < 8 && !bound; ++c)
            {
                const std::string& ty = gi.mSamplers[k].mType;
                const bool cube = ty.find("Cube") != std::string::npos, arr = ty.find("Array") != std::string::npos;
                const U32 want = ty.find("CubeArray") != std::string::npos ? 2 : cube ? 1 : arr ? 3 : ty.find("3D") != std::string::npos ? 4 : 0;
                bound = sCtx.mBound[unit][want];
                break;
            }
            if (bound && sFrameTargets.count(bound)) key += std::to_string(bound) + " ";
        }
        auto git = sStats.mGraph.find(key);
        if (git == sStats.mGraph.end())
        {
            sStats.mGraphOrder.push_back(key);
            sStats.mGraph[key] = 1;
        }
        else
        {
            ++git->second;
        }
    }
    if (sStatsEnabled && sCtx.mDrawFbo == 0 && sStats.mBackbufferEvents.size() < 4000)
    {
        std::string e = program->getName();
        if (e != "UI Shader" && e.compare(0, 3, "HUD") != 0)
        {
            const ProgramInfo& pi = program->getInfo();
            for (size_t k = 0; k < pi.mSamplers.size(); ++k)
            {
                const U32 unit = program->getSamplerUnit(k);
                if (unit >= 32) continue;
                U32 cls = 0;
                const std::string& ty = pi.mSamplers[k].mType;
                if (ty.find("CubeArray") != std::string::npos) cls = 2;
                else if (ty.find("Cube") != std::string::npos) cls = 1;
                else if (ty.find("2DArray") != std::string::npos) cls = 3;
                e += " [" + pi.mSamplers[k].mName + "=" + std::to_string(sCtx.mBound[unit][cls]) + "]";
            }
        }
        sStats.mBackbufferEvents.push_back(e);
    }
    {
        LLVkMirror::PhaseTimer phase(LLVkMirror::PH_D_UBO);
        sync_uniform_blocks(r, used_buffers);
    }
    LLVkMirror::gLastLap = 205;
    LLVkMirror::PhaseTimer phase(LLVkMirror::PH_D_VERTEX);
    if (!build_vertex_input(r, *program, vi, used_buffers))
    {
        ++sDrawsSkipped;
        ++sStats.mNoVertexInput;
        ++sStats.mSkippedPerProgram[program->getName() + " (vertex input)"];
        return false;
    }
    return true;
}

// ---- device-local buffers: writes become GPU copies, recorded when a draw is about to read the buffer ----
std::vector<GLuint> sPendingBufs;       // the buffers whose Buf::mPending is set

void flush_buffer_updates()
{
    if (sPendingBufs.empty())
    {
        return;
    }
    if (!Renderer::instance().flushBufferUpdates())
    {
        return;                         // no frame is being recorded: the writes stay queued
    }
    for (GLuint n : sPendingBufs)
    {
        auto it = sGL.mBuffers.find(n);
        if (it != sGL.mBuffers.end()) it->second.mPending = false;
    }
    sPendingBufs.clear();
}

// called before a draw is recorded: if it reads a buffer with an unrecorded write, record the writes first
void ensure_buffers_current(const std::vector<GLuint>& used, GLuint element)
{
    if (sPendingBufs.empty())
    {
        return;
    }
    bool stale = false;
    for (GLuint n : used)
    {
        auto it = sGL.mBuffers.find(n);
        if (it != sGL.mBuffers.end() && it->second.mPending) { stale = true; break; }
    }
    if (!stale && element)
    {
        auto it = sGL.mBuffers.find(element);
        stale = it != sGL.mBuffers.end() && it->second.mPending;
    }
    if (stale)
    {
        flush_buffer_updates();
    }
}

U64 device_buffer_min_bytes()
{
    static const U64 v = []() {
        const char* e = getenv("LL_VK_DEVICE_BUF_MIN");
        return e ? (U64)atoll(e) : (U64)(128 * 1024);
    }();
    return v;
}

void mark_used(const std::vector<GLuint>& buffers, GLuint element)
{
    const U64 frame = Context::instance().getFrameNumber();
    for (GLuint b : buffers)
    {
        auto it = sGL.mBuffers.find(b);
        if (it != sGL.mBuffers.end()) it->second.mLastUseFrame = frame;
    }
    auto eit = sGL.mBuffers.find(element);
    if (element && eit != sGL.mBuffers.end()) eit->second.mLastUseFrame = frame;
}

// writes into a GL buffer's host memory, renaming the Vulkan buffer first if a frame in flight may still read it
// how much time the buffer renames take (logged when it gets noticeable)
struct RenameStats
{
    U32 mCount = 0;
    U64 mBytes = 0;
    U64 mMicros = 0;
    U64 mShadowlessCopies = 0;
    std::chrono::steady_clock::time_point mWindowStart = std::chrono::steady_clock::now();
};
RenameStats sRenameStats;

void report_renames()
{
    const auto now = std::chrono::steady_clock::now();
    if (now - sRenameStats.mWindowStart < std::chrono::seconds(5))
    {
        return;
    }
    if (sRenameStats.mMicros > 20000)
    {
        U64 shadow_bytes = 0;
        for (const auto& kv : sGL.mBuffers) shadow_bytes += kv.second.mShadow.size();
        LL_INFOS("VkGL") << "buffer renames in the last 5 s: " << sRenameStats.mCount << " copies, " << (sRenameStats.mBytes >> 20) << " MB, "
                         << sRenameStats.mMicros / 1000 << " ms on the main thread (" << sRenameStats.mShadowlessCopies << " read from video memory); system memory copies hold "
                         << (shadow_bytes >> 20) << " MB" << LL_ENDL;
    }
    sRenameStats = RenameStats();
}

void write_buffer(GLuint name, Buf& b, U64 offset, U64 size, const void* data, bool whole)
{
    if (b.mDevicePath)
    {
        if (b.mVk.valid() && data && size && offset + size <= b.mVk.mSize)
        {
            Renderer::instance().updateBuffer(b.mVk.mBuffer, offset, size, data);
            if (!b.mPending)
            {
                b.mPending = true;
                sPendingBufs.push_back(name);
            }
        }
        return;
    }
    Context& ctx = Context::instance();
    const U64 frame = ctx.getFrameNumber();
    const bool in_flight = b.mLastUseFrame != ~0ull && frame < b.mLastUseFrame + Context::frameCount() + 1;
    if (in_flight && b.mVk.valid())
    {
        const auto t0 = std::chrono::steady_clock::now();
        Buffer fresh;
        if (ctx.createBuffer(b.mSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                             Context::Memory::UploadDevice, fresh))
        {
            const size_t keep = (size_t)std::min<U64>(b.mSize, b.mVk.mSize);
            if (!whole)
            {
                if (b.mShadow.size() >= keep)
                {
                    memcpy(fresh.mMapped, b.mShadow.data(), keep);
                }
                else
                {
                    memcpy(fresh.mMapped, b.mVk.mMapped, keep);     // no copy in system memory: the slow way
                    ++sRenameStats.mShadowlessCopies;
                }
            }
            ctx.destroyBuffer(b.mVk);
            b.mVk = fresh;
            b.mLastUseFrame = ~0ull;
            ++sRenameStats.mCount;
            sRenameStats.mBytes += keep;
        }
        sRenameStats.mMicros += (U64)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
        report_renames();
    }
    if (b.mVk.valid() && data && offset + size <= b.mVk.mSize)
    {
        memcpy((U8*)b.mVk.mMapped + offset, data, (size_t)size);
        if (b.mShadow.size() >= offset + size)
        {
            memcpy(b.mShadow.data() + offset, data, (size_t)size);
        }
    }
}

} // anonymous namespace

//-----------------------------------------------------------------------------------------------------
// hooks: textures
//-----------------------------------------------------------------------------------------------------

void on_glGenTextures(GLsizei n, GLuint* textures)
{
    for (GLsizei i = 0; i < n; ++i)
    {
        sGL.mTextures[textures[i]];
    }
}

void on_glDeleteTextures(GLsizei n, const GLuint* textures)
{
    for (GLsizei i = 0; i < n; ++i)
    {
        auto it = sGL.mTextures.find(textures[i]);
        if (it != sGL.mTextures.end())
        {
            destroy_tex_images(it->second);
            sGL.mTextures.erase(it);
        }
        for (auto& unit : sCtx.mBound)
        {
            for (auto& b : unit)
            {
                if (b == textures[i]) b = 0;
            }
        }
        for (auto& f : sGL.mFbos)
        {
            for (auto& a : f.second.mColor) if (a.mTexture == textures[i]) a.mTexture = 0;
            if (f.second.mDepth.mTexture == textures[i]) f.second.mDepth.mTexture = 0;
        }
    }
}

void on_glBindTexture(GLenum target, GLuint texture)
{
    sCtx.mBound[sCtx.mActiveUnit][target_class(target)] = texture;
    if (texture)
    {
        sGL.mTextures[texture];     // GL allows binding a name that was never generated
    }
}

void on_glActiveTexture(GLenum texture)
{
    if (texture >= GL_TEXTURE0 && texture < GL_TEXTURE0 + 32)
    {
        sCtx.mActiveUnit = texture - GL_TEXTURE0;
    }
}

void on_glPixelStorei(GLenum pname, GLint param)
{
    if (pname == GL_UNPACK_ALIGNMENT) sCtx.mUnpackAlignment = param;
    else if (pname == GL_UNPACK_ROW_LENGTH) sCtx.mUnpackRowLength = param;
    else if (pname == 0x0D05) sCtx.mPackAlignment = param;          // PACK_ALIGNMENT
    else if (pname == 0x0D02) sCtx.mPackRowLength = param;          // PACK_ROW_LENGTH
}

void on_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void* pixels)
{
    note_thread("glTexImage2D");
    Tex* t = bound_texture(target);
    if (!t || width <= 0 || height <= 0)
    {
        return;
    }
    if (level == 0 && !ensure_image(*t, target, internalformat, (U32)width, (U32)height, target_class(target) == 1 ? 6 : 1))
    {
        return;
    }
    if (level == 0 && !pixel_source(pixels) && t->mImage.valid() && t->mImage.mLayout == VK_IMAGE_LAYOUT_UNDEFINED)
    {
        t->mImage.mZeroOnFirstUse = true;       // allocation only: GL would hand back zeros
    }
    upload_level(*t, target, level, 0, 0, 0, (U32)width, (U32)height, 1, format, type, pixels);
}

void on_glTexImage3D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type, const void* pixels)
{
    Tex* t = bound_texture(target);
    if (!t || width <= 0 || height <= 0 || depth <= 0)
    {
        return;
    }
    if (level == 0 && !ensure_image(*t, target, internalformat, (U32)width, (U32)height, (U32)depth))
    {
        return;
    }
    if (level == 0 && !pixel_source(pixels) && t->mImage.valid() && t->mImage.mLayout == VK_IMAGE_LAYOUT_UNDEFINED)
    {
        t->mImage.mZeroOnFirstUse = true;
    }
    upload_level(*t, target, level, 0, 0, 0, (U32)width, (U32)height, (U32)depth, format, type, pixels);
}

void on_glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void* pixels)
{
    note_thread("glTexSubImage2D");
    Tex* t = bound_texture(target);
    if (t)
    {
        upload_level(*t, target, level, xoffset, yoffset, 0, (U32)width, (U32)height, 1, format, type, pixels);
    }
}

void on_glTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* pixels)
{
    Tex* t = bound_texture(target);
    if (t)
    {
        upload_level(*t, target, level, xoffset, yoffset, zoffset, (U32)width, (U32)height, (U32)depth, format, type, pixels);
    }
}

void on_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void* data)
{
    note_thread("glCompressedTexImage2D");
    Tex* t = bound_texture(target);
    if (!t || width <= 0 || height <= 0)
    {
        return;
    }
    if (level == 0 && !ensure_image(*t, target, (GLint)internalformat, (U32)width, (U32)height, target_class(target) == 1 ? 6 : 1))
    {
        return;
    }
    const void* src = pixel_source(data);
    if (!src || !t->mImage.valid() || level < 0 || (U32)level >= t->mLevels)
    {
        return;
    }
    U32 layer = 0;
    if (target >= 0x8515 && target <= 0x851A) layer = target - 0x8515;
    Renderer::instance().uploadTexture(t->mImage, (U32)level, layer, 0, 0, (U32)width, (U32)height, 1, src, (VkDeviceSize)imageSize);
}

void on_glTexParameteri(GLenum target, GLenum pname, GLint param)
{
    Tex* t = bound_texture(target);
    if (!t)
    {
        return;
    }
    SamplerParams& sp = t->mSampler;
    switch (pname)
    {
    case GL_TEXTURE_MIN_FILTER: sp.mMinFilter = (GLenum)param; break;
    case GL_TEXTURE_MAG_FILTER: sp.mMagFilter = (GLenum)param; break;
    case GL_TEXTURE_WRAP_S: sp.mWrapS = (GLenum)param; break;
    case GL_TEXTURE_WRAP_T: sp.mWrapT = (GLenum)param; break;
    case GL_TEXTURE_WRAP_R: sp.mWrapR = (GLenum)param; break;
    case GL_TEXTURE_COMPARE_MODE: sp.mCompare = (GLenum)param == GL_COMPARE_REF_TO_TEXTURE; break;
    case GL_TEXTURE_COMPARE_FUNC: sp.mCompareFunc = (GLenum)param; break;
    case GL_TEXTURE_BASE_LEVEL: sp.mBaseLevel = param; break;
    case GL_TEXTURE_MAX_LEVEL: sp.mMaxLevel = param; break;
    case GL_TEXTURE_SWIZZLE_R: case GL_TEXTURE_SWIZZLE_G: case GL_TEXTURE_SWIZZLE_B: case GL_TEXTURE_SWIZZLE_A:
        t->mSwizzle[pname - GL_TEXTURE_SWIZZLE_R] = (GLenum)param;
        t->mSwizzleSet = true;
        rebuild_sampling_view(*t);
        break;
    default: break;
    }
}

void on_glTexParameterf(GLenum target, GLenum pname, GLfloat param)
{
    if (pname == GL_TEXTURE_MAX_ANISOTROPY || pname == GL_TEXTURE_MAX_ANISOTROPY_EXT)
    {
        if (Tex* t = bound_texture(target))
        {
            t->mSampler.mAnisotropy = param;
        }
        return;
    }
    on_glTexParameteri(target, pname, (GLint)param);
}

void on_glTexParameteriv(GLenum target, GLenum pname, const GLint* params)
{
    if (!params)
    {
        return;
    }
    if (pname == GL_TEXTURE_SWIZZLE_RGBA)
    {
        if (Tex* tex = bound_texture(target))
        {
            for (U32 i = 0; i < 4; ++i)
            {
                tex->mSwizzle[i] = (GLenum)params[i];
            }
            tex->mSwizzleSet = true;
            rebuild_sampling_view(*tex);
        }
        return;
    }
    on_glTexParameteri(target, pname, params[0]);
}

void on_glGenerateMipmap(GLenum target)
{
    Tex* t = bound_texture(target);
    if (t && t->mImage.valid())
    {
        Renderer::instance().generateMipmaps(t->mImage);
        t->mValidLevels = t->mLevels;
    }
}

static void copy_to_texture(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLint x, GLint y, GLsizei width, GLsizei height)
{
    Tex* t = bound_texture(target);
    Image* src = read_image();
    if (!t || !t->mImage.valid() || !src || !LLVkMirror::inFrame())
    {
        return;
    }
    U32 layer = 0;
    if (target >= 0x8515 && target <= 0x851A) layer = target - 0x8515;
    else if (t->mTarget == GL_TEXTURE_2D_ARRAY || t->mTarget == GL_TEXTURE_CUBE_MAP_ARRAY) layer = (U32)zoffset;
    Renderer::instance().copyRegion(*src, x, y, t->mImage, (U32)level, layer, xoffset, yoffset, width, height);
    ++sCopies;
}

void on_glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height)
{
    copy_to_texture(target, level, xoffset, yoffset, 0, x, y, width, height);
}

void on_glCopyTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLint x, GLint y, GLsizei width, GLsizei height)
{
    copy_to_texture(target, level, xoffset, yoffset, zoffset, x, y, width, height);
}

void on_glCopyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border)
{
    Tex* t = bound_texture(target);
    if (!t || width <= 0 || height <= 0)
    {
        return;
    }
    if (level == 0 && !ensure_image(*t, target, (GLint)internalformat, (U32)width, (U32)height, 1))
    {
        return;
    }
    copy_to_texture(target, level, 0, 0, 0, x, y, width, height);
}

//-----------------------------------------------------------------------------------------------------
// hooks: framebuffers
//-----------------------------------------------------------------------------------------------------

void on_glGenFramebuffers(GLsizei n, GLuint* framebuffers)
{
    for (GLsizei i = 0; i < n; ++i)
    {
        sGL.mFbos[framebuffers[i]];
    }
}

void on_glDeleteFramebuffers(GLsizei n, const GLuint* framebuffers)
{
    for (GLsizei i = 0; i < n; ++i)
    {
        sGL.mFbos.erase(framebuffers[i]);
        if (sCtx.mDrawFbo == framebuffers[i]) sCtx.mDrawFbo = 0;
        if (sCtx.mReadFbo == framebuffers[i]) sCtx.mReadFbo = 0;
    }
}

void on_glBindFramebuffer(GLenum target, GLuint framebuffer)
{
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) sCtx.mDrawFbo = framebuffer;
    if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER) sCtx.mReadFbo = framebuffer;
    if (framebuffer)
    {
        sGL.mFbos[framebuffer];
    }
}

void on_glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)
{
    const GLuint fbo_name = target == GL_READ_FRAMEBUFFER ? sCtx.mReadFbo : sCtx.mDrawFbo;
    if (!fbo_name)
    {
        return;
    }
    Fbo& fbo = sGL.mFbos[fbo_name];
    FboAttachment a;
    a.mTexture = texture;
    a.mLevel = level;
    a.mLayer = (textarget >= 0x8515 && textarget <= 0x851A) ? textarget - 0x8515 : 0;
    if (attachment >= GL_COLOR_ATTACHMENT0 && attachment < GL_COLOR_ATTACHMENT0 + 8)
    {
        fbo.mColor[attachment - GL_COLOR_ATTACHMENT0] = a;
    }
    else if (attachment == GL_DEPTH_ATTACHMENT || attachment == 0x821A)
    {
        fbo.mDepth = a;
    }
}

void on_glDrawBuffers(GLsizei n, const GLenum* bufs)
{
    if (sCtx.mDrawFbo == 0)
    {
        return;
    }
    Fbo& fbo = sGL.mFbos[sCtx.mDrawFbo];
    fbo.mDrawCount = (U32)std::min<GLsizei>(n, 8);
    for (U32 i = 0; i < fbo.mDrawCount; ++i)
    {
        fbo.mDrawBuffers[i] = bufs[i];
    }
}

void on_glDrawBuffer(GLenum buf)
{
    if (sCtx.mDrawFbo == 0)
    {
        return;
    }
    Fbo& fbo = sGL.mFbos[sCtx.mDrawFbo];
    if (buf >= GL_COLOR_ATTACHMENT0 && buf < GL_COLOR_ATTACHMENT0 + 8)
    {
        fbo.mDrawCount = 1;
        fbo.mDrawBuffers[0] = buf;
    }
    else
    {
        fbo.mDrawCount = 0;     // GL_NONE: depth only
    }
}

void on_glReadBuffer(GLenum src)
{
    if (sCtx.mReadFbo)
    {
        sGL.mFbos[sCtx.mReadFbo].mReadBuffer = src;
    }
}

//-----------------------------------------------------------------------------------------------------
// hooks: buffers and vertex arrays
//-----------------------------------------------------------------------------------------------------

void on_glGenBuffers(GLsizei n, GLuint* buffers)
{
    for (GLsizei i = 0; i < n; ++i)
    {
        sGL.mBuffers[buffers[i]];
    }
}

void on_glDeleteBuffers(GLsizei n, const GLuint* buffers)
{
    Context& ctx = Context::instance();
    for (GLsizei i = 0; i < n; ++i)
    {
        auto it = sGL.mBuffers.find(buffers[i]);
        if (it != sGL.mBuffers.end())
        {
            ctx.destroyBuffer(it->second.mVk);
            sGL.mBuffers.erase(it);
        }
        if (sCtx.mArrayBuffer == buffers[i]) sCtx.mArrayBuffer = 0;
        if (sCtx.mPixelUnpackBuffer == buffers[i]) sCtx.mPixelUnpackBuffer = 0;
        if (sCtx.mPixelPackBuffer == buffers[i]) sCtx.mPixelPackBuffer = 0;
    }
}

void on_glBindBuffer(GLenum target, GLuint buffer)
{
    if (buffer)
    {
        sGL.mBuffers[buffer];
    }
    if (target == GL_ARRAY_BUFFER) sCtx.mArrayBuffer = buffer;
    else if (target == GL_ELEMENT_ARRAY_BUFFER) current_vao().mElementBuffer = buffer;
    else if (target == GL_PIXEL_UNPACK_BUFFER) sCtx.mPixelUnpackBuffer = buffer;
    else if (target == 0x88EB) sCtx.mPixelPackBuffer = buffer;      // PIXEL_PACK_BUFFER
    else if (target == GL_UNIFORM_BUFFER) sCtx.mUniformBuffer = buffer;
}

static GLuint buffer_for_target(GLenum target)
{
    if (target == GL_ARRAY_BUFFER) return sCtx.mArrayBuffer;
    if (target == GL_ELEMENT_ARRAY_BUFFER) return current_vao().mElementBuffer;
    if (target == GL_PIXEL_UNPACK_BUFFER) return sCtx.mPixelUnpackBuffer;
    if (target == 0x88EB) return sCtx.mPixelPackBuffer;
    if (target == GL_UNIFORM_BUFFER) return sCtx.mUniformBuffer;
    return 0;
}

void on_glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage)
{
    const GLuint name = buffer_for_target(target);
    auto it = sGL.mBuffers.find(name);
    if (it == sGL.mBuffers.end() || size < 0)
    {
        return;
    }
    Buf& b = it->second;
    Context& ctx = Context::instance();
    // large vertex / index buffers take the device path (GPU copies, no CPU access, no shadow copy)
    const bool want_device = (target == GL_ARRAY_BUFFER || target == GL_ELEMENT_ARRAY_BUFFER) && (U64)size >= device_buffer_min_bytes() && !getenv("LL_VK_NO_DEVICE_BUFFERS");
    if (want_device && b.mDevicePath && b.mVk.valid() && b.mVk.mSize == (VkDeviceSize)size)
    {
        // the same size again (the viewer re-specifies a buffer to rewrite it): keep the Vulkan buffer, queue the data
        b.mSize = (U64)size;
        if (data)
        {
            Renderer::instance().updateBuffer(b.mVk.mBuffer, 0, (VkDeviceSize)size, data);
            if (!b.mPending)
            {
                b.mPending = true;
                sPendingBufs.push_back(name);
            }
        }
        return;
    }
    if (b.mVk.valid())
    {
        ctx.destroyBuffer(b.mVk);
    }
    b.mSize = (U64)size;
    b.mLastUseFrame = ~0ull;
    b.mShadow.clear();
    b.mDevicePath = want_device;
    if (want_device)
    {
        if (ctx.createBuffer((VkDeviceSize)size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             Context::Memory::Device, b.mVk) && data)
        {
            Renderer::instance().updateBuffer(b.mVk.mBuffer, 0, (VkDeviceSize)size, data);
            if (!b.mPending)
            {
                b.mPending = true;
                sPendingBufs.push_back(name);
            }
        }
        return;
    }
    if (size == 0 || !ctx.createBuffer((VkDeviceSize)size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                       Context::Memory::UploadDevice, b.mVk))
    {
        return;
    }
    b.mShadow.assign((size_t)size, 0);
    if (data)
    {
        memcpy(b.mVk.mMapped, data, (size_t)size);
        memcpy(b.mShadow.data(), data, (size_t)size);
    }
    else
    {
        memset(b.mVk.mMapped, 0, (size_t)size);
    }
}

void on_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data)
{
    const GLuint name = buffer_for_target(target);
    auto it = sGL.mBuffers.find(name);
    if (it != sGL.mBuffers.end() && data && size > 0 && offset >= 0)
    {
        write_buffer(name, it->second, (U64)offset, (U64)size, data, false);
    }
}

void on_glBindBufferBase(GLenum target, GLuint index, GLuint buffer)
{
    if (target == GL_UNIFORM_BUFFER)
    {
        sCtx.mUniformBuffer = buffer;
        if (buffer)
        {
            sGL.mBuffers[buffer];
        }
        if (index < 4)
        {
            sUboBinding[index] = buffer;
        }
    }
}

void on_glGenVertexArrays(GLsizei n, GLuint* arrays)
{
    for (GLsizei i = 0; i < n; ++i)
    {
        sGL.mVaos[arrays[i]];
    }
}

void on_glBindVertexArray(GLuint array)
{
    sCtx.mVao = array;
    sGL.mVaos[array];
}

void on_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer)
{
    if (index >= 16) return;
    VaoAttrib& a = current_vao().mAttrib[index];
    a.mBuffer = sCtx.mArrayBuffer;
    a.mSize = size;
    a.mType = type;
    a.mNormalized = normalized != GL_FALSE;
    a.mInteger = false;
    a.mStride = stride;
    a.mOffset = (U64)pointer;
}

void on_glVertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride, const void* pointer)
{
    if (index >= 16) return;
    VaoAttrib& a = current_vao().mAttrib[index];
    a.mBuffer = sCtx.mArrayBuffer;
    a.mSize = size;
    a.mType = type;
    a.mNormalized = false;
    a.mInteger = true;
    a.mStride = stride;
    a.mOffset = (U64)pointer;
}

void on_glEnableVertexAttribArray(GLuint index)
{
    if (index < 16) current_vao().mAttrib[index].mEnabled = true;
}

void on_glDisableVertexAttribArray(GLuint index)
{
    if (index < 16) current_vao().mAttrib[index].mEnabled = false;
}

void on_glVertexAttrib4f(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
    if (index < 16)
    {
        sGL.mConstAttrib[index][0] = x; sGL.mConstAttrib[index][1] = y; sGL.mConstAttrib[index][2] = z; sGL.mConstAttrib[index][3] = w;
    }
}

void on_glVertexAttrib4fv(GLuint index, const GLfloat* v)
{
    if (v)
    {
        on_glVertexAttrib4f(index, v[0], v[1], v[2], v[3]);
    }
}

//-----------------------------------------------------------------------------------------------------
// hooks: draws
//-----------------------------------------------------------------------------------------------------

void on_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    HookTimer hook_timer;
    LLVkMirror::PhaseTimer draw_phase(LLVkMirror::PH_DRAW);
    static thread_local VertexInput vi_cache;
    static thread_local std::vector<GLuint> used_cache;
    VertexInput& vi = vi_cache;
    std::vector<GLuint>& used = used_cache;
    vi.mBindings.clear();
    vi.mAttributes.clear();
    used.clear();
    if (count <= 0 || !prepare_draw(vi, used))
    {
        return;
    }
    Renderer& r = Renderer::instance();
    ensure_buffers_current(used, 0);
    LLVkMirror::gLastLap = 210;
    LLVkMirror::PhaseTimer apply_phase(LLVkMirror::PH_D_APPLY);
    r.setVertexInput(vi);
    r.setTopology(topology(mode));
    LLVkMirror::gLastLap = 211;
    if (r.draw((U32)count, (U32)first))
    {
        if (LLVkMirror::gPhaseTiming)
        {
            ++sPhaseProgramDraws[r.getProgram()->getName()];
            ++sPhaseTargetDraws[r.describeTarget()];
            note_draw_time(r.getProgram()->getName());
        }
        if (sStatsEnabled)
        {
            ++sStats.mDone;
            ++sStats.mDonePerProgram[r.getProgram()->getName()];
        }
    }
    else
    {
        ++sStats.mRendererRefused;
    }
    mark_used(used, 0);
    ++sDraws;
    ++sDrawsTotal;
}

void on_glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices)
{
    HookTimer hook_timer;
    LLVkMirror::PhaseTimer draw_phase(LLVkMirror::PH_DRAW);
    static thread_local VertexInput vi_cache;
    static thread_local std::vector<GLuint> used_cache;
    VertexInput& vi = vi_cache;
    std::vector<GLuint>& used = used_cache;
    vi.mBindings.clear();
    vi.mAttributes.clear();
    used.clear();
    if (count <= 0 || !prepare_draw(vi, used))
    {
        return;
    }
    Vao& vao = current_vao();
    auto eit = sGL.mBuffers.find(vao.mElementBuffer);
    if (eit == sGL.mBuffers.end() || !eit->second.mVk.valid())
    {
        ++sDrawsSkipped;
        ++sStats.mNoElement;
        ++sStats.mSkippedPerProgram[Renderer::instance().getProgram()->getName() + " (no index buffer)"];
        return;
    }
    const VkIndexType itype = type == GL_UNSIGNED_INT ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    const U32 index_bytes = type == GL_UNSIGNED_INT ? 4 : 2;
    Renderer& r = Renderer::instance();
    ensure_buffers_current(used, vao.mElementBuffer);
    LLVkMirror::PhaseTimer apply_phase(LLVkMirror::PH_D_APPLY);
    r.setVertexInput(vi);
    r.setTopology(topology(mode));
    r.setIndexBuffer(eit->second.mVk.mBuffer, 0, itype);
    if (r.drawIndexed((U32)count, (U32)((U64)indices / index_bytes), 0))
    {
        if (LLVkMirror::gPhaseTiming)
        {
            ++sPhaseProgramDraws[r.getProgram()->getName()];
            ++sPhaseTargetDraws[r.describeTarget()];
            note_draw_time(r.getProgram()->getName());
        }
        if (sStatsEnabled)
        {
            ++sStats.mDone;
            ++sStats.mDonePerProgram[r.getProgram()->getName()];
        }
    }
    else
    {
        ++sStats.mRendererRefused;
    }
    mark_used(used, vao.mElementBuffer);
    ++sDraws;
    ++sDrawsTotal;
}

//-----------------------------------------------------------------------------------------------------
// hooks: state and clears
//-----------------------------------------------------------------------------------------------------

static void set_cap(GLenum cap, bool on)
{
    switch (cap)
    {
    case GL_BLEND: sGL.mBlend = on; break;
    case GL_DEPTH_TEST: sGL.mDepthTest = on; break;
    case GL_CULL_FACE: sGL.mCull = on; break;
    case GL_SCISSOR_TEST: sGL.mScissorTest = on; break;
    case GL_STENCIL_TEST: sGL.mStencilTest = on; break;
    case GL_POLYGON_OFFSET_FILL: sGL.mPolyOffset = on; break;
    case GL_DEPTH_CLAMP: sGL.mDepthClamp = on; break;
    default: sGL.mOtherCaps[cap] = on; break;
    }
}

void on_glEnable(GLenum cap) { set_cap(cap, true); }
void on_glDisable(GLenum cap) { set_cap(cap, false); }

void on_glBlendFunc(GLenum sfactor, GLenum dfactor)
{
    sGL.mBlendSrcRGB = sGL.mBlendSrcA = sfactor;
    sGL.mBlendDstRGB = sGL.mBlendDstA = dfactor;
}

void on_glBlendFuncSeparate(GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha)
{
    sGL.mBlendSrcRGB = sfactorRGB; sGL.mBlendDstRGB = dfactorRGB;
    sGL.mBlendSrcA = sfactorAlpha; sGL.mBlendDstA = dfactorAlpha;
}

void on_glDepthFunc(GLenum func) { sGL.mDepthFunc = func; }
void on_glDepthMask(GLboolean flag) { sGL.mDepthMask = flag != GL_FALSE; }

void on_glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha)
{
    sGL.mColorMask[0] = red != GL_FALSE; sGL.mColorMask[1] = green != GL_FALSE;
    sGL.mColorMask[2] = blue != GL_FALSE; sGL.mColorMask[3] = alpha != GL_FALSE;
}

void on_glCullFace(GLenum mode) { sGL.mCullFace = mode; }
void on_glPolygonOffset(GLfloat factor, GLfloat units) { sGL.mOffsetFactor = factor; sGL.mOffsetUnits = units; }
void on_glPolygonMode(GLenum face, GLenum mode) { sGL.mPolygonMode = mode; }
void on_glLineWidth(GLfloat width) { sGL.mLineWidth = width; }
void on_glPointSize(GLfloat size) { (void)size; }
void on_glStencilFunc(GLenum, GLint, GLuint) {}
void on_glStencilOp(GLenum, GLenum, GLenum) {}
void on_glStencilMask(GLuint) {}

void on_glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
    sGL.mViewport[0] = x; sGL.mViewport[1] = y; sGL.mViewport[2] = width; sGL.mViewport[3] = height;
    sGL.mViewportSet = true;
}

void on_glScissor(GLint x, GLint y, GLsizei width, GLsizei height)
{
    sGL.mScissor[0] = x; sGL.mScissor[1] = y; sGL.mScissor[2] = width; sGL.mScissor[3] = height;
}

void on_glClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha)
{
    sGL.mClearColor[0] = red; sGL.mClearColor[1] = green; sGL.mClearColor[2] = blue; sGL.mClearColor[3] = alpha;
}

void on_glClear(GLbitfield mask)
{
    HookTimer hook_timer;
    if (!LLVkMirror::inFrame() || !sync_target())
    {
        return;
    }
    if (sCtx.mDrawFbo == 0 && sStats.mBackbufferEvents.size() < 4000)
    {
        sStats.mBackbufferEvents.push_back(std::string("CLEAR mask ") + std::to_string(mask) + " scissor " + (sGL.mScissorTest ? "on" : "off"));
    }
    U32 bits = 0;
    if ((mask & GL_COLOR_BUFFER_BIT) && (sGL.mColorMask[0] || sGL.mColorMask[1] || sGL.mColorMask[2] || sGL.mColorMask[3])) bits |= CLEAR_COLOR;
    if ((mask & GL_DEPTH_BUFFER_BIT) && sGL.mDepthMask) bits |= CLEAR_DEPTH;
    if (bits)
    {
        Renderer::instance().clear(bits, sGL.mClearColor, 1.f, 0);
    }
}

//-----------------------------------------------------------------------------------------------------
// debug dump of the render targets of a frame (trigger: create the file D:\claude\vk-dump.trigger)
//-----------------------------------------------------------------------------------------------------

namespace
{

U32 crc32_update(U32 crc, const U8* data, size_t n)
{
    static U32 table[256];
    static bool init = false;
    if (!init)
    {
        for (U32 i = 0; i < 256; ++i)
        {
            U32 c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void put_be32(std::vector<U8>& v, U32 x)
{
    v.push_back((U8)(x >> 24)); v.push_back((U8)(x >> 16)); v.push_back((U8)(x >> 8)); v.push_back((U8)x);
}

void png_chunk(std::vector<U8>& out, const char* type, const std::vector<U8>& data)
{
    put_be32(out, (U32)data.size());
    std::vector<U8> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    put_be32(out, crc32_update(0, body.data(), body.size()));
}

// minimal PNG writer: 8-bit RGBA, stored (uncompressed) deflate blocks
bool write_png(const std::string& path, U32 w, U32 h, const std::vector<U8>& rgba)
{
    std::vector<U8> raw;
    raw.reserve((size_t)(w * 4 + 1) * h);
    for (U32 y = 0; y < h; ++y)
    {
        raw.push_back(0);
        raw.insert(raw.end(), rgba.begin() + (size_t)y * w * 4, rgba.begin() + (size_t)(y + 1) * w * 4);
    }
    std::vector<U8> z = { 0x78, 0x01 };
    U32 a = 1, b = 0;
    for (U8 byte : raw) { a = (a + byte) % 65521; b = (b + a) % 65521; }
    for (size_t pos = 0; pos < raw.size();)
    {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        z.push_back(pos + n >= raw.size() ? 1 : 0);
        z.push_back((U8)(n & 0xFF)); z.push_back((U8)(n >> 8)); z.push_back((U8)(~n & 0xFF)); z.push_back((U8)((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    }
    put_be32(z, (b << 16) | a);

    std::vector<U8> png = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    std::vector<U8> ihdr;
    put_be32(ihdr, w); put_be32(ihdr, h);
    ihdr.push_back(8); ihdr.push_back(6); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    png_chunk(png, "IHDR", ihdr);
    png_chunk(png, "IDAT", z);
    png_chunk(png, "IEND", {});
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fwrite(png.data(), 1, png.size(), f);
    fclose(f);
    return true;
}

U8 to_byte(F32 v)
{
    return (U8)(std::max(0.f, std::min(1.f, v)) * 255.f + 0.5f);
}

// converts the texels of an image to 8-bit RGBA for viewing. Depth and scalar formats become gray.
bool to_rgba8(VkFormat f, U32 w, U32 h, const std::vector<U8>& in, std::vector<U8>& out)
{
    out.assign((size_t)w * h * 4, 255);
    const size_t n = (size_t)w * h;
    switch (f)
    {
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
        memcpy(out.data(), in.data(), n * 4);
        for (size_t i = 0; i < n; ++i) out[i * 4 + 3] = 255;       // alpha is data in the G-buffer; show colour only
        return true;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        for (size_t i = 0; i < n; ++i)
            for (int c = 0; c < 3; ++c) { U16 hv; memcpy(&hv, &in[(i * 4 + c) * 2], 2); out[i * 4 + c] = to_byte(half_to_float(hv)); }
        return true;
    case VK_FORMAT_R32G32B32A32_SFLOAT:
        for (size_t i = 0; i < n; ++i)
            for (int c = 0; c < 3; ++c) { F32 v; memcpy(&v, &in[(i * 4 + c) * 4], 4); out[i * 4 + c] = to_byte(v); }
        return true;
    case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_D32_SFLOAT:
        for (size_t i = 0; i < n; ++i) { F32 v; memcpy(&v, &in[i * 4], 4); const U8 g = to_byte(v); out[i * 4] = out[i * 4 + 1] = out[i * 4 + 2] = g; }
        return true;
    case VK_FORMAT_R16G16_SFLOAT:
        for (size_t i = 0; i < n; ++i)
        {
            U16 a, b; memcpy(&a, &in[i * 4], 2); memcpy(&b, &in[i * 4 + 2], 2);
            out[i * 4] = to_byte(half_to_float(a)); out[i * 4 + 1] = to_byte(half_to_float(b)); out[i * 4 + 2] = 0;
        }
        return true;
    case VK_FORMAT_R8_UNORM:
        for (size_t i = 0; i < n; ++i) out[i * 4] = out[i * 4 + 1] = out[i * 4 + 2] = in[i];
        return true;
    case VK_FORMAT_R8G8_UNORM:
        for (size_t i = 0; i < n; ++i) { out[i * 4] = in[i * 2]; out[i * 4 + 1] = in[i * 2 + 1]; out[i * 4 + 2] = 0; }
        return true;
    default:
        return false;
    }
}

} // anonymous namespace

// what the translator currently holds in Vulkan, for the benchmark log
std::string resource_summary()
{
    std::lock_guard<LLVkMirror::Mutex> lock(gMutex);
    U64 tex_bytes = 0, rt_bytes = 0, buf_bytes = 0;
    U32 tex_count = 0, rt_count = 0, views = 0, buf_count = 0;
    for (auto& kv : sGL.mTextures)
    {
        const Tex& t = kv.second;
        views += (U32)t.mViews.size();
        if (!t.mImage.valid()) continue;
        const FormatInfo fi = formatInfo(t.mImage.mFormat);
        U64 bytes = 0;
        for (U32 m = 0; m < t.mImage.mMipLevels; ++m)
        {
            const U64 w = std::max<U32>(1, t.mImage.mExtent.width >> m), h = std::max<U32>(1, t.mImage.mExtent.height >> m);
            bytes += ((w + fi.mBlockWidth - 1) / fi.mBlockWidth) * ((h + fi.mBlockHeight - 1) / fi.mBlockHeight) * fi.mBytesPerBlock * std::max<U32>(1, t.mImage.mLayers) * std::max<U32>(1, t.mImage.mExtent.depth);
        }
        const bool target = (t.mImage.mAspect != VK_IMAGE_ASPECT_COLOR_BIT) || !t.mViews.empty() || sFrameTargets.count(kv.first);
        if (target) { rt_bytes += bytes; ++rt_count; } else { tex_bytes += bytes; ++tex_count; }
    }
    for (auto& kv : sGL.mBuffers)
    {
        if (kv.second.mVk.valid()) { buf_bytes += kv.second.mVk.mSize; ++buf_count; }
    }
    return "mirrored textures " + std::to_string(tex_count) + " (" + std::to_string(tex_bytes / (1024 * 1024)) + " MB), render targets " + std::to_string(rt_count) + " ("
        + std::to_string(rt_bytes / (1024 * 1024)) + " MB, " + std::to_string(views) + " extra views), buffers " + std::to_string(buf_count) + " (" + std::to_string(buf_bytes / (1024 * 1024))
        + " MB), GL texture objects " + std::to_string(sGL.mTextures.size()) + ", pending destroys " + std::to_string(Context::instance().pendingDestroys());
}

// milliseconds of hook CPU time since the last call
double take_hook_ms()
{
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    const double ms = (double)gHookTicks * 1000.0 / (double)freq.QuadPart;
    gHookTicks = 0;
    return ms;
}

// called by the mirror once per frame after it presents: arms the dump if the trigger file exists
namespace { void drain_query_results(); }

void after_frame()
{
    std::lock_guard<LLVkMirror::Mutex> lock(gMutex);
    if (LLVkAsync::gEnabled)
    {
        drain_query_results();
    }
    {
        // LL_VK_PHASES=1: where the main thread's time goes inside the translation layer, averaged over 600 frames
        static const bool phases = getenv("LL_VK_PHASES") != nullptr;
        if (phases)
        {
            static U32 frames = 0;
            static U64 draws_at_start = 0;
            static LARGE_INTEGER t_start = {};
            if (!LLVkMirror::gPhaseTiming)
            {
                LLVkMirror::gPhaseTiming = true;
                QueryPerformanceCounter(&t_start);
            }
            if (++frames >= 600)
            {
                LARGE_INTEGER freq, now;
                QueryPerformanceFrequency(&freq);
                QueryPerformanceCounter(&now);
                const double wall_ms = (double)(now.QuadPart - t_start.QuadPart) * 1000.0 / (double)freq.QuadPart;
                std::string text;
                for (int p = 0; p < LLVkMirror::PH_COUNT; ++p)
                {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "%s %.2f | ", LLVkMirror::phaseName(p), (double)LLVkMirror::gPhaseTicks[p] * 1000.0 / (double)freq.QuadPart / frames);
                    text += buf;
                    LLVkMirror::gPhaseTicks[p] = 0;
                }
                LL_INFOS("VkPhases") << frames << " frames, " << wall_ms / frames << " ms/frame wall, " << (sDrawsTotal - draws_at_start) / frames
                                     << " draws/frame; CPU ms/frame by phase: " << text << LL_ENDL;
                {
                    // the sink hooks, most expensive first, for the main thread and for all other threads (time includes waiting for the lock;
                    // the draw hooks contain the draw phases above)
                    for (int part = 0; part < 2; ++part)
                    {
                        std::vector<std::pair<long long, int>> order;
                        for (int i = part * HOOK_COUNT; i < (part + 1) * HOOK_COUNT; ++i)
                        {
                            if (gHookStatCalls[i]) order.push_back({ gHookStatTicks[i], i });
                        }
                        std::sort(order.rbegin(), order.rend());
                        std::string top;
                        for (size_t k = 0; k < order.size() && k < (part ? 8u : 14u); ++k)
                        {
                            const int i = order[k].second;
                            const double ms = (double)gHookStatTicks[i] * 1000.0 / (double)freq.QuadPart;
                            char buf[160];
                            snprintf(buf, sizeof(buf), "%s %.1f calls %.2f ms %.0f ns/call | ", kHookNames[i - part * HOOK_COUNT], (double)gHookStatCalls[i] / frames, ms / frames, ms * 1e6 / (double)gHookStatCalls[i]);
                            top += buf;
                        }
                        LL_INFOS("VkPhases") << (part ? "hooks on OTHER threads per frame (time shown per viewer frame): " : "hooks on the MAIN thread per frame, most CPU time first: ") << top << LL_ENDL;
                    }
                    {
                        // the hooks that hand a value back to the viewer: each would be a wait if the work were queued for another thread
                        std::string answers;
                        for (int i = 0; i < HOOK_COUNT; ++i)
                        {
                            const char* n = kHookNames[i];
                            const bool value = strncmp(n, "glGet", 5) == 0 || strncmp(n, "glIs", 4) == 0 || strncmp(n, "glCheck", 7) == 0 || strncmp(n, "glFence", 7) == 0
                                               || strncmp(n, "glClientWait", 12) == 0 || strncmp(n, "glReadPixels", 12) == 0 || strncmp(n, "glCreate", 8) == 0;
                            if (value && gHookStatCalls[i])
                            {
                                char buf[120];
                                snprintf(buf, sizeof(buf), "%s %.1f calls | ", n, (double)gHookStatCalls[i] / frames);
                                answers += buf;
                            }
                        }
                        LL_INFOS("VkPhases") << "value-returning hooks per frame (main thread): " << answers << LL_ENDL;
                        static const char* const fn_names[] = { "glGetIntegerv", "glGetFloatv", "glGetBooleanv", "glIsEnabled", "glGetTexLevelParameteriv" };
                        std::vector<std::pair<U64, std::pair<int, GLenum>>> order;
                        for (const auto& kv : sGetterCounts) order.push_back({ kv.second, kv.first });
                        std::sort(order.rbegin(), order.rend());
                        std::string getters;
                        for (size_t k = 0; k < order.size() && k < 12; ++k)
                        {
                            char buf[120];
                            snprintf(buf, sizeof(buf), "%s(0x%X) %.1f | ", fn_names[order[k].second.first], order[k].second.second, (double)order[k].first / frames);
                            getters += buf;
                        }
                        LL_INFOS("VkPhases") << "state getters per frame by parameter: " << getters << LL_ENDL;
                        sGetterCounts.clear();
                    }
                    for (int i = 0; i < 2 * HOOK_COUNT; ++i) { gHookStatTicks[i] = 0; gHookStatCalls[i] = 0; }
                    {
                        const Renderer::DrawDiag dd = Renderer::instance().takeDrawDiag();
                        LL_INFOS("VkPhases") << "indexed draws per frame: " << (double)dd.mDraws / frames << ", of which changed nothing but the draw command: " << (double)dd.mUnchanged / frames
                                             << ", of which also continued the previous index range (mergeable): " << (double)dd.mMergeable / frames << LL_ENDL;
                    }
                    {
                        std::vector<std::pair<U32, std::string>> by_count;
                        for (const auto& kv : sPhaseProgramDraws) by_count.push_back({ kv.second, kv.first });
                        std::sort(by_count.rbegin(), by_count.rend());
                        std::string text;
                        for (size_t k = 0; k < by_count.size() && k < 16; ++k)
                        {
                            char buf[200];
                            snprintf(buf, sizeof(buf), "%s %.1f | ", by_count[k].second.c_str(), (double)by_count[k].first / frames);
                            text += buf;
                        }
                        LL_INFOS("VkPhases") << "draws per frame by program (top 16): " << text << LL_ENDL;
                        {
                            // the thread's time per frame by program (everything from the previous draw to this one), with the average cost of one draw
                            LARGE_INTEGER freq;
                            QueryPerformanceFrequency(&freq);
                            std::vector<std::pair<long long, std::string>> by_time;
                            long long total_ticks = 0;
                            for (const auto& kv : sPhaseProgramTicks) { by_time.push_back({ kv.second, kv.first }); total_ticks += kv.second; }
                            std::sort(by_time.rbegin(), by_time.rend());
                            std::string time_text;
                            for (size_t k = 0; k < by_time.size() && k < 14; ++k)
                            {
                                const auto draws_it = sPhaseProgramDraws.find(by_time[k].second);
                                const double draws = draws_it != sPhaseProgramDraws.end() ? (double)draws_it->second : 1.0;
                                char buf[220];
                                snprintf(buf, sizeof(buf), "%s %.2f ms (%.2f us/draw) | ", by_time[k].second.c_str(), (double)by_time[k].first * 1000.0 / (double)freq.QuadPart / frames,
                                         (double)by_time[k].first * 1e6 / (double)freq.QuadPart / draws);
                                time_text += buf;
                            }
                            LL_INFOS("VkPhases") << "time per frame between draws, by program (all draws " << (double)total_ticks * 1000.0 / (double)freq.QuadPart / frames << " ms): " << time_text << LL_ENDL;
                            sPhaseProgramTicks.clear();
                        }
                        {
                            LARGE_INTEGER freq;
                            QueryPerformanceFrequency(&freq);
                            std::vector<std::pair<long long, std::string>> by_time;
                            for (const auto& kv : sPhaseTargetTicks) by_time.push_back({ kv.second, kv.first });
                            std::sort(by_time.rbegin(), by_time.rend());
                            std::string text;
                            for (size_t k = 0; k < by_time.size() && k < 14; ++k)
                            {
                                const auto it = sPhaseTargetDraws.find(by_time[k].second);
                                const double draws = it != sPhaseTargetDraws.end() ? (double)it->second : 1.0;
                                char buf[200];
                                snprintf(buf, sizeof(buf), "[%s] %.2f ms, %.0f draws (%.2f us/draw) | ", by_time[k].second.c_str(), (double)by_time[k].first * 1000.0 / (double)freq.QuadPart / frames,
                                         draws / frames, (double)by_time[k].first * 1e6 / (double)freq.QuadPart / draws);
                                text += buf;
                            }
                            LL_INFOS("VkPhases") << "time per frame between draws, by render target: " << text << LL_ENDL;
                            sPhaseTargetTicks.clear();
                            sPhaseTargetDraws.clear();
                        }
                        sPhaseProgramDraws.clear();
                    }
                    {
                        unsigned long long v[2];
                        LLVkAsync::queryResultStats(v);
                        LL_INFOS("VkPhases") << "occlusion results handed to the viewer per frame: occluded (0) " << (double)v[0] / frames << ", visible " << (double)v[1] / frames << LL_ENDL;
                    }
                    {
                        U64 dev_bytes = 0, host_bytes = 0, shadow_bytes = 0;
                        U32 dev_count = 0, host_count = 0;
                        for (const auto& kv : sGL.mBuffers)
                        {
                            if (!kv.second.mVk.valid()) continue;
                            if (kv.second.mDevicePath) { dev_bytes += kv.second.mVk.mSize; ++dev_count; }
                            else { host_bytes += kv.second.mVk.mSize; ++host_count; }
                            shadow_bytes += kv.second.mShadow.size();
                        }
                        LL_INFOS("VkPhases") << "GL buffers: " << dev_count << " in video memory (copies, " << (dev_bytes >> 20) << " MB), " << host_count << " host-visible ("
                                             << (host_bytes >> 20) << " MB), system-memory shadow copies " << (shadow_bytes >> 20) << " MB" << LL_ENDL;
                    }
                    if (LLVkAsync::gEnabled)
                    {
                        LL_INFOS("VkPhases") << "frame pacing: " << LLVkAsync::pacingReport() << LL_ENDL;
                    }
                    if (LLVkAsync::gEnabled)
                    {
                        unsigned long long q[7];
                        LLVkAsync::queryStats(q);
                        LL_INFOS("VkPhases") << "occlusion queries per frame: marked pending " << (double)q[0] / frames << ", published " << (double)q[1] / frames << ", set idle " << (double)q[2] / frames
                                             << "; polls answered ready " << (double)q[3] / frames << ", pending " << (double)q[4] / frames << ", idle " << (double)q[5] / frames
                                             << ", result reads sent to the worker " << (double)q[6] / frames << LL_ENDL;
                    }
                    const Renderer::UploadStats up = Renderer::instance().takeUploadStats();
                    LL_INFOS("VkPhases") << "texture uploads per frame: " << (double)up.mCount / frames << " uploads, " << (double)up.mBytes / frames / 1024.0 << " KB, "
                                         << (double)up.mPassSplits / frames << " ended a render pass in progress" << LL_ENDL;
                    {
                        static U64 last_barriers = 0, last_built = 0, last_micros = 0;
                        const U64 barriers = gSyncStats.mImageBarriers.load(std::memory_order_relaxed);
                        const U64 built = gSyncStats.mPipelinesBuilt.load(std::memory_order_relaxed);
                        const U64 micros = gSyncStats.mPipelineBuildMicros.load(std::memory_order_relaxed);
                        const U64 slowest = gSyncStats.mPipelineBuildMaxMicros.exchange(0, std::memory_order_relaxed);
                        LL_INFOS("VkPhases") << "image barriers per frame: " << (double)(barriers - last_barriers) / frames << "; graphics pipelines built in this period: "
                                             << (built - last_built) << " (" << (double)(micros - last_micros) / 1000.0 << " ms in the driver in total, slowest "
                                             << (double)slowest / 1000.0 << " ms)" << LL_ENDL;
                        last_barriers = barriers;
                        last_built = built;
                        last_micros = micros;
                    }
                    {
                        static U64 last_gpu_frames = 0, last_gpu = 0, last_fence = 0, last_acquire = 0, last_present = 0;
                        const U64 gpu_frames = gSyncStats.mGpuFrames.load(std::memory_order_relaxed);
                        const U64 gpu = gSyncStats.mGpuMicros.load(std::memory_order_relaxed);
                        const U64 gpu_max = gSyncStats.mGpuMaxMicros.exchange(0, std::memory_order_relaxed);
                        const U64 fence = gSyncStats.mFenceWaitMicros.load(std::memory_order_relaxed);
                        const U64 acquire = gSyncStats.mAcquireWaitMicros.load(std::memory_order_relaxed);
                        const U64 present = gSyncStats.mPresentMicros.load(std::memory_order_relaxed);
                        const double n = (double)std::max<U64>(1, gpu_frames - last_gpu_frames);
                        (void)gpu_max;
                        LL_INFOS("VkPhases") << "GPU time averaged over every command buffer (" << n / frames << " per viewer frame): " << (double)(gpu - last_gpu) / n / 1000.0
                                             << " ms each; the Vulkan thread is blocked per frame: waiting for the GPU (frame fence) " << (double)(fence - last_fence) / frames / 1000.0
                                             << " ms, swapchain acquire " << (double)(acquire - last_acquire) / frames / 1000.0 << " ms, present " << (double)(present - last_present) / frames / 1000.0 << " ms" << LL_ENDL;
                        {
                            Context::GpuPassStats gp;
                            Context::instance().takeGpuPassStats(gp);
                            const double total = (double)(gp.mMainMicros + gp.mOffMicros);
                            LL_INFOS("VkPhases") << "GPU time per viewer frame: " << total / 1000.0 / frames << " ms in all (the frame that is presented " << (double)gp.mMainMicros / 1000.0 / std::max<double>(1, (double)gp.mMainFrames)
                                                 << " ms avg, slowest " << (double)gp.mMainMax / 1000.0 << " ms; the off-screen frame after it " << (double)gp.mOffMicros / 1000.0 / std::max<double>(1, (double)gp.mOffFrames)
                                                 << " ms avg, slowest " << (double)gp.mOffMax / 1000.0 << " ms). Timestamps between passes make this a little higher than an unprofiled run" << LL_ENDL;
                            std::vector<std::pair<U64, U32>> by_time;
                            for (const auto& kv : gp.mMicros) by_time.push_back({ kv.second, kv.first });
                            std::sort(by_time.rbegin(), by_time.rend());
                            std::string text;
                            for (size_t k = 0; k < by_time.size() && k < 16; ++k)
                            {
                                char buf[220];
                                snprintf(buf, sizeof(buf), "[%s] %.2f ms (%.0f%%) | ", Context::stampTagName(by_time[k].second).c_str(), (double)by_time[k].first / 1000.0 / frames,
                                         total > 0 ? 100.0 * (double)by_time[k].first / total : 0.0);
                                text += buf;
                            }
                            LL_INFOS("VkPhases") << "GPU time by render pass (per viewer frame): " << text << LL_ENDL;
                        }
                        last_gpu_frames = gpu_frames;
                        last_gpu = gpu;
                        last_fence = fence;
                        last_acquire = acquire;
                        last_present = present;
                    }
                }
                draws_at_start = sDrawsTotal;
                frames = 0;
                QueryPerformanceCounter(&t_start);
            }
        }
    }
    {
        static U32 counter = 0;
        if (debugShadersEnabled() && (++counter % 10) == 0)
        {
            int mode = 0;
            char filter[128] = {}, sampler[128] = {};
            std::string content;
            if (FILE* f = fopen("D:/claude/vk-debug.txt", "rb"))
            {
                char buffer[512] = {};
                const size_t n = fread(buffer, 1, sizeof(buffer) - 1, f);
                content.assign(buffer, n);
                fclose(f);
                sscanf(content.c_str(), "%d %127s %127s", &mode, filter, sampler);
            }
            // the words after the first line switch draw state overrides on for the selected programs
            const size_t eol = content.find('\n');
            const std::string flags = eol == std::string::npos ? std::string() : content.substr(eol);
            sDbgNoDepth = flags.find("nodepth") != std::string::npos;
            sDbgNoDepthWrite = flags.find("nodepthwrite") != std::string::npos;
            sDbgNoDepthTest = flags.find("nodepthtest") != std::string::npos;
            sDbgNoBlend = flags.find("noblend") != std::string::npos;
            sDbgNoCull = flags.find("nocull") != std::string::npos;
            sDbgNoScissor = flags.find("noscissor") != std::string::npos;
            sDbgFullMask = flags.find("fullmask") != std::string::npos;
            if (mode != sDbgMode || sDbgFilter != filter || sDbgSampler != sampler)
            {
                sDbgMode = mode;
                sDbgFilter = filter;
                sDbgSampler = sampler;
                LL_INFOS("VkGL") << "debug shader control: mode " << mode << " programs '" << filter << "' sampler '" << sampler << "'" << LL_ENDL;
            }
        }
    }
    if (!sDumpPending && GetFileAttributesA("D:\\claude\\vk-dump.trigger") != INVALID_FILE_ATTRIBUTES)
    {
        DeleteFileA("D:\\claude\\vk-dump.trigger");
        sDumpTargets = sFrameTargets;
        sDumpPending = true;
        LL_INFOS("VkGL") << "debug dump armed: " << sDumpTargets.size() << " render target textures of the last frame" << LL_ENDL;
    }
    if (sSnapActive)
    {
        snap_evaluate();
        if (++sSnapStage >= 2) { sSnapActive = false; sSnapStage = 0; }
    }
    if (sDumpPending && !sSnapActive)
    {
        sSnapActive = true;     // the next frame records per-pass snapshots
        sSnapStage = 0;
        sSnapExtra.clear();
        if (FILE* f = fopen("D:/claude/vk-snap-filter.txt", "rb"))
        {
            char line[256];
            while (fgets(line, sizeof(line), f))
            {
                std::string l = line;
                while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.pop_back();
                if (!l.empty()) sSnapExtra.push_back(l);
            }
            fclose(f);
        }
    }
    sFrameTargets.clear();
    memset(sRecentFrameTargets, 0, sizeof(sRecentFrameTargets));
    sFrameSampledLast = sFrameSampled;
    sFrameSampled.clear();
    sStatsLast = sStats;
    sStats = DrawStats();
}

// Reads a texture level from real OpenGL (the viewer's own context, DSA so no bindings change) and from the
// Vulkan mirror and logs the mean colour of each and how far apart they are.
void compare_with_gl()
{
    Context& ctx = Context::instance();
    ctx.waitIdle();
    // render targets and the most used sampled textures of the last frame
    std::vector<std::pair<U32, GLuint>> ranked;
    for (auto& kv : sFrameSampledLast) ranked.push_back({ kv.second, kv.first });
    std::sort(ranked.rbegin(), ranked.rend());
    std::vector<GLuint> names(sDumpTargets.begin(), sDumpTargets.end());
    for (size_t i = 0; i < ranked.size() && i < 14; ++i) names.push_back(ranked[i].second);
    for (auto& kv : sFrameSampledLast)      // every array texture and every depth texture that was sampled (reflection probes, irradiance, shadow maps)
    {
        auto at = sGL.mTextures.find(kv.first);
        if (at != sGL.mTextures.end() && at->second.mImage.valid()
            && (at->second.mTarget == GL_TEXTURE_CUBE_MAP_ARRAY || at->second.mTarget == GL_TEXTURE_2D_ARRAY || at->second.mImage.mFormat == VK_FORMAT_D32_SFLOAT)
            && std::find(names.begin(), names.end(), kv.first) == names.end())
        {
            names.push_back(kv.first);
        }
    }

    LL_INFOS("VkGL") << "GL vs Vulkan comparison of " << names.size() << " textures (mean RGB over a sparse grid, then mean abs difference)" << LL_ENDL;
    for (GLuint name : names)
    {
        auto it = sGL.mTextures.find(name);
        if (it == sGL.mTextures.end() || !it->second.mImage.valid())
        {
            continue;
        }
        Tex& t = it->second;
        const VkFormat vf = t.mImage.mFormat;
        GLenum gl_format = GL_RGBA, gl_type = GL_UNSIGNED_BYTE;
        U32 bpt = 4;
        bool ok = false;
        if (vf == VK_FORMAT_R8G8B8A8_UNORM || vf == VK_FORMAT_R8G8B8A8_SRGB) { ok = true; }
        else if (vf == VK_FORMAT_R16G16B16A16_SFLOAT) { gl_type = GL_FLOAT; bpt = 16; ok = true; }
        else if (vf == VK_FORMAT_D32_SFLOAT) { gl_format = GL_DEPTH_COMPONENT; gl_type = GL_FLOAT; bpt = 4; ok = true; }
        else if (vf == VK_FORMAT_B10G11R11_UFLOAT_PACK32) { gl_type = GL_FLOAT; bpt = 16; ok = true; }
        if (!ok || !glGetTextureImage)
        {
            if (t.mTarget == GL_TEXTURE_CUBE_MAP_ARRAY || t.mTarget == GL_TEXTURE_2D_ARRAY)
            {
                LL_INFOS("VkGL") << "cmp skipped array tex " << name << " " << t.mImage.mExtent.width << "x" << t.mImage.mExtent.height << " vk format " << (S32)vf
                                 << " layers " << t.mDepth << " levels " << t.mImage.mMipLevels << LL_ENDL;
            }
            continue;
        }
        const bool is_array = t.mTarget == GL_TEXTURE_CUBE_MAP_ARRAY || t.mTarget == GL_TEXTURE_2D_ARRAY;
        std::vector<U32> layer_list = { 0u };
        if (is_array)
        {
            layer_list = { 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 12u, 18u, 24u, 30u, 36u };
        }
        for (U32 layer : layer_list)
        for (U32 level : { 0u, 2u })
        {
            if (level >= t.mImage.mMipLevels || layer >= std::max<U32>(1, t.mDepth))
            {
                continue;
            }
            const U32 w = std::max<U32>(1, t.mImage.mExtent.width >> level), h = std::max<U32>(1, t.mImage.mExtent.height >> level);
            std::vector<U8> gl_texels((size_t)w * h * bpt, 0), vk_texels;
            if (!glGetTextureSubImage)
            {
                continue;
            }
            glGetTextureSubImage(name, (GLint)level, 0, 0, (GLint)layer, (GLsizei)w, (GLsizei)h, 1, gl_format, gl_type, (GLsizei)gl_texels.size(), gl_texels.data());
            if (!Renderer::instance().readbackImage(t.mImage, level, layer, vk_texels))
            {
                continue;
            }
            double gsum[3] = {}, vsum[3] = {}, diff = 0;
            size_t n = 0;
            const size_t step = std::max<size_t>(1, (size_t)w * h / 4000);
            for (size_t px = 0; px < (size_t)w * h; px += step)
            {
                for (int c = 0; c < 3; ++c)
                {
                    double gv, vv;
                    if (gl_type == GL_UNSIGNED_BYTE)
                    {
                        gv = gl_texels[px * 4 + (gl_format == GL_RGBA ? c : 0)] / 255.0;
                        vv = vk_texels[px * 4 + (gl_format == GL_RGBA ? c : 0)] / 255.0;
                    }
                    else if (vf == VK_FORMAT_B10G11R11_UFLOAT_PACK32)
                    {
                        F32 a; memcpy(&a, &gl_texels[(px * 4 + c) * 4], 4);
                        U32 packed; memcpy(&packed, &vk_texels[px * 4], 4);
                        const U32 mant_bits = c == 2 ? 5 : 6;
                        const U32 field = c == 0 ? (packed & 0x7FF) : c == 1 ? ((packed >> 11) & 0x7FF) : ((packed >> 22) & 0x3FF);
                        const U32 e = field >> mant_bits, m = field & ((1u << mant_bits) - 1);
                        gv = a;
                        vv = e == 0 ? (m / (double)(1u << mant_bits)) * pow(2.0, -14) : (1.0 + m / (double)(1u << mant_bits)) * pow(2.0, (double)e - 15);
                    }
                    else if (vf == VK_FORMAT_D32_SFLOAT)
                    {
                        F32 a, b; memcpy(&a, &gl_texels[px * 4], 4); memcpy(&b, &vk_texels[px * 4], 4); gv = a; vv = b;
                    }
                    else
                    {
                        // RGBA16F in Vulkan, RGBA32F read from GL
                        F32 a; memcpy(&a, &gl_texels[(px * 4 + c) * 4], 4);
                        U16 hv; memcpy(&hv, &vk_texels[(px * 4 + c) * 2], 2);
                        gv = a; vv = half_to_float(hv);
                    }
                    gsum[c] += gv; vsum[c] += vv; diff += fabs(gv - vv);
                }
                ++n;
            }
            if (!n) continue;
            LL_INFOS("VkGL") << "cmp tex " << name << " " << t.mImage.mExtent.width << "x" << t.mImage.mExtent.height << " fmt " << (S32)vf << " layer " << layer << "/" << t.mDepth << " level " << level
                             << (sFrameTargets.count(name) || sDumpTargets.count(name) ? " [target]" : " [sampled]") << ": GL mean ("
                             << gsum[0] / n << "," << gsum[1] / n << "," << gsum[2] / n << ") VK mean (" << vsum[0] / n << "," << vsum[1] / n << ","
                             << vsum[2] / n << ") absdiff " << diff / (n * 3) << " validLevels " << t.mValidLevels << "/" << t.mLevels << LL_ENDL;
        }
    }
}

// called by the mirror at the start of the next frame, outside any frame: writes the PNGs
void run_dump()
{
    std::lock_guard<LLVkMirror::Mutex> lock(gMutex);
    if (!sDumpPending)
    {
        return;
    }
    sDumpPending = false;
    {
        const DrawStats& d = sStatsLast;
        LL_INFOS("VkGL") << "draw stats of the last mirrored frame: drawn " << d.mDone << ", no program " << d.mNoProgram << ", no target " << d.mNoTarget
                         << ", no vertex input " << d.mNoVertexInput << ", no index buffer " << d.mNoElement << ", refused by renderer " << d.mRendererRefused
                         << ", outside frame " << d.mNotInFrame << LL_ENDL;
        std::vector<std::pair<U32, std::string>> skipped, done;
        for (auto& kv : d.mSkippedPerProgram) skipped.push_back({ kv.second, kv.first });
        for (auto& kv : d.mDonePerProgram) done.push_back({ kv.second, kv.first });
        std::sort(skipped.rbegin(), skipped.rend());
        std::sort(done.rbegin(), done.rend());
        for (size_t i = 0; i < skipped.size() && i < 40; ++i)
            LL_INFOS("VkGL") << "  skipped " << skipped[i].first << "x " << skipped[i].second << LL_ENDL;
        for (size_t i = 0; i < done.size() && i < 60; ++i)
            LL_INFOS("VkGL") << "  drawn   " << done[i].first << "x " << done[i].second << LL_ENDL;
        for (const std::string& key : d.mGraphOrder)
        {
            if (key.find("Shadow") == std::string::npos && key.find("UI Shader") == std::string::npos)
                LL_INFOS("VkGL") << "  pass " << d.mGraph.at(key) << "x " << key << LL_ENDL;
        }
        for (auto& kv : d.mSamplerInfo)
            LL_INFOS("VkGL") << "  sampler " << kv.first << " -> " << kv.second << LL_ENDL;
        for (auto& kv : d.mFeedback)
            LL_INFOS("VkGL") << "  feedback " << kv.second << "x " << kv.first << LL_ENDL;
        // the default framebuffer's history this frame, repeats collapsed
        {
            std::string line;
            size_t i = 0;
            while (i < d.mBackbufferEvents.size())
            {
                size_t j = i;
                while (j < d.mBackbufferEvents.size() && d.mBackbufferEvents[j] == d.mBackbufferEvents[i]) ++j;
                line += d.mBackbufferEvents[i] + (j - i > 1 ? " x" + std::to_string(j - i) : std::string()) + " | ";
                i = j;
            }
            LL_INFOS("VkGL") << "default framebuffer events (" << d.mBackbufferEvents.size() << "): " << line << LL_ENDL;
        }
    }
    LLGLSLShader::mirrorCompareAll();
    for (U32 i = 0; i < 4; ++i)
    {
        auto it = sUboBinding[i] ? sGL.mBuffers.find(sUboBinding[i]) : sGL.mBuffers.end();
        if (it == sGL.mBuffers.end() || !it->second.mVk.valid() || !glGetNamedBufferSubData)
        {
            LL_INFOS("VkGL") << "UBO " << i << ": nothing bound" << LL_ENDL;
            continue;
        }
        const U64 n = std::min<U64>(it->second.mSize, 65536);
        std::vector<U8> gl_bytes(n);
        glGetNamedBufferSubData(it->first, 0, (GLsizeiptr)n, gl_bytes.data());
        U64 differing = 0, first = ~0ull;
        for (U64 b = 0; b < n; ++b)
        {
            if (gl_bytes[b] != ((const U8*)it->second.mVk.mMapped)[b])
            {
                if (first == ~0ull) first = b;
                ++differing;
            }
        }
        U64 nonzero = 0;
        for (U64 b = 0; b < n; ++b) nonzero += gl_bytes[b] != 0;
        LL_INFOS("VkGL") << "UBO " << i << " (GL buffer " << it->first << ", " << it->second.mSize << " bytes): " << differing << " of " << n
                         << " bytes differ from OpenGL's copy (first at " << (S64)first << "), " << nonzero << " non-zero bytes in GL's copy" << LL_ENDL;
    }
    compare_with_gl();
    CreateDirectoryA("D:\\claude\\vk-dump", nullptr);
    Context& ctx = Context::instance();
    ctx.waitIdle();
    U32 written = 0;
    for (GLuint name : sDumpTargets)
    {
        auto it = sGL.mTextures.find(name);
        if (it == sGL.mTextures.end() || !it->second.mImage.valid())
        {
            continue;
        }
        Tex& t = it->second;
        std::vector<U8> texels, rgba;
        if (!Renderer::instance().readbackImage(t.mImage, 0, 0, texels))
        {
            continue;
        }
        if (!to_rgba8(t.mImage.mFormat, t.mImage.mExtent.width, t.mImage.mExtent.height, texels, rgba))
        {
            LL_INFOS("VkGL") << "dump: texture " << name << " has format " << (S32)t.mImage.mFormat << " (not converted)" << LL_ENDL;
            continue;
        }
        // GL images are stored bottom row first; flip for viewing
        const U32 w = t.mImage.mExtent.width, h = t.mImage.mExtent.height;
        std::vector<U8> flipped(rgba.size());
        for (U32 y = 0; y < h; ++y) memcpy(&flipped[(size_t)y * w * 4], &rgba[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
        char path[160];
        snprintf(path, sizeof(path), "D:\\claude\\vk-dump\\tex%u_%ux%u_fmt%d.png", name, w, h, (S32)t.mImage.mFormat);
        if (write_png(path, w, h, flipped)) ++written;
    }
    LL_INFOS("VkGL") << "debug dump: wrote " << written << " images to D:\\claude\\vk-dump" << LL_ENDL;
}

//-----------------------------------------------------------------------------------------------------
// hooks: calls that return data or state. With LL_VK_VERIFY set they compare OpenGL's answer with what the mirror's own
// state (or the Vulkan device) would answer, and tally which questions the viewer actually asks. The emulation here is
// what a Vulkan-only renderer will have to provide.
//-----------------------------------------------------------------------------------------------------

namespace
{

struct GetterTally
{
    U64 mCalls = 0, mHandled = 0, mMismatch = 0;
    double mGl[4] = {}, mEmu[4] = {};       // last mismatching pair
    U32 mCount = 0;
};
// kind: 0 glGetIntegerv, 1 glGetFloatv, 2 glGetBooleanv, 3 glIsEnabled, 4 glGetTexLevelParameteriv, 5 glCheckFramebufferStatus
std::map<std::pair<int, GLenum>, GetterTally> sGetterTally;
const char* const kGetterNames[] = { "glGetIntegerv", "glGetFloatv", "glGetBooleanv", "glIsEnabled", "glGetTexLevelParameteriv", "glCheckFramebufferStatus" };

// device limits: the Vulkan device must offer at least what OpenGL reports (the viewer sizes things from these)
bool emulate_limit(GLenum pname, GLint* out, U32& n)
{
    const Capabilities& caps = Context::instance().getCaps();
    n = 1;
    switch (pname)
    {
    case 0x0D33: out[0] = (GLint)caps.mMaxImageDim2D; return true;                  // MAX_TEXTURE_SIZE
    case 0x84E8: out[0] = (GLint)caps.mMaxImageDim2D; return true;                  // MAX_RENDERBUFFER_SIZE
    case 0x851C: out[0] = (GLint)caps.mMaxImageDimCube; return true;                // MAX_CUBE_MAP_TEXTURE_SIZE
    case 0x8073: out[0] = (GLint)caps.mMaxImageDim3D; return true;                  // MAX_3D_TEXTURE_SIZE
    case 0x88FF: out[0] = (GLint)caps.mMaxArrayLayers; return true;                 // MAX_ARRAY_TEXTURE_LAYERS
    case 0x8CDF: out[0] = (GLint)caps.mMaxColorAttachments; return true;           // MAX_COLOR_ATTACHMENTS
    case 0x8824: out[0] = (GLint)caps.mMaxColorAttachments; return true;           // MAX_DRAW_BUFFERS
    case 0x8869: out[0] = (GLint)caps.mMaxVertexAttribs; return true;               // MAX_VERTEX_ATTRIBS
    case 0x8A30: out[0] = (GLint)caps.mMaxUniformRange; return true;                // MAX_UNIFORM_BLOCK_SIZE
    case 0x8872: case 0x8B4C: case 0x8B4D:                                          // MAX_TEXTURE_IMAGE_UNITS, MAX_VERTEX_TEXTURE_IMAGE_UNITS, MAX_COMBINED_TEXTURE_IMAGE_UNITS
        out[0] = (GLint)caps.mMaxSampledImagesPerStage; return true;
    case 0x8D57: out[0] = (GLint)caps.mMaxSampleCount; return true;                 // MAX_SAMPLES
    case 0x0D3A: out[0] = (GLint)caps.mMaxViewport[0]; out[1] = (GLint)caps.mMaxViewport[1]; n = 2; return true;     // MAX_VIEWPORT_DIMS
    default: return false;
    }
}

// the value the mirror's tracked GL state holds. False if the mirror does not track this one.
bool emulate_integer(GLenum pname, GLint* out, U32& n)
{
    n = 1;
    switch (pname)
    {
    case 0x0BA2:    // VIEWPORT
        if (!sGL.mViewportSet) return false;
        for (int i = 0; i < 4; ++i) out[i] = sGL.mViewport[i];
        n = 4; return true;
    case 0x0C10:    // SCISSOR_BOX
        if (!sGL.mScissor[2] && !sGL.mScissor[3]) return false;
        for (int i = 0; i < 4; ++i) out[i] = sGL.mScissor[i];
        n = 4; return true;
    case 0x84E0: out[0] = (GLint)(0x84C0 + sCtx.mActiveUnit); return true;     // ACTIVE_TEXTURE
    case 0x8069: out[0] = (GLint)sCtx.mBound[sCtx.mActiveUnit][0]; return true;     // TEXTURE_BINDING_2D
    case 0x8514: out[0] = (GLint)sCtx.mBound[sCtx.mActiveUnit][1]; return true;     // TEXTURE_BINDING_CUBE_MAP
    case 0x900A: out[0] = (GLint)sCtx.mBound[sCtx.mActiveUnit][2]; return true;     // TEXTURE_BINDING_CUBE_MAP_ARRAY
    case 0x8C1D: out[0] = (GLint)sCtx.mBound[sCtx.mActiveUnit][3]; return true;     // TEXTURE_BINDING_2D_ARRAY
    case 0x806A: out[0] = (GLint)sCtx.mBound[sCtx.mActiveUnit][4]; return true;     // TEXTURE_BINDING_3D
    case 0x8CA6: out[0] = (GLint)sCtx.mDrawFbo; return true;                        // DRAW_FRAMEBUFFER_BINDING / FRAMEBUFFER_BINDING
    case 0x8CAA: out[0] = (GLint)sCtx.mReadFbo; return true;                        // READ_FRAMEBUFFER_BINDING
    case 0x8894: out[0] = (GLint)sCtx.mArrayBuffer; return true;                    // ARRAY_BUFFER_BINDING
    case 0x8895: out[0] = (GLint)current_vao().mElementBuffer; return true;         // ELEMENT_ARRAY_BUFFER_BINDING
    case 0x85B5: out[0] = (GLint)sCtx.mVao; return true;                            // VERTEX_ARRAY_BINDING
    case 0x8A28: out[0] = (GLint)sCtx.mUniformBuffer; return true;                  // UNIFORM_BUFFER_BINDING
    case 0x88EF: out[0] = (GLint)sCtx.mPixelUnpackBuffer; return true;              // PIXEL_UNPACK_BUFFER_BINDING
    case 0x0CF5: out[0] = sCtx.mUnpackAlignment; return true;                       // UNPACK_ALIGNMENT
    case 0x0CF2: out[0] = sCtx.mUnpackRowLength; return true;                       // UNPACK_ROW_LENGTH
    case 0x0B74: out[0] = (GLint)sGL.mDepthFunc; return true;                       // DEPTH_FUNC
    case 0x0B45: out[0] = (GLint)sGL.mCullFace; return true;                        // CULL_FACE_MODE
    case 0x80C9: out[0] = (GLint)sGL.mBlendSrcRGB; return true;                     // BLEND_SRC_RGB
    case 0x80C8: out[0] = (GLint)sGL.mBlendDstRGB; return true;                     // BLEND_DST_RGB
    case 0x80CB: out[0] = (GLint)sGL.mBlendSrcA; return true;                       // BLEND_SRC_ALPHA
    case 0x80CA: out[0] = (GLint)sGL.mBlendDstA; return true;                       // BLEND_DST_ALPHA
    default: return false;
    }
}

bool emulate_float(GLenum pname, GLfloat* out, U32& n)
{
    n = 1;
    switch (pname)
    {
    case 0x0BA2:
        if (!sGL.mViewportSet) return false;
        for (int i = 0; i < 4; ++i) out[i] = (GLfloat)sGL.mViewport[i];
        n = 4; return true;
    case 0x0B21: out[0] = sGL.mLineWidth; return true;                              // LINE_WIDTH
    case 0x0C22: for (int i = 0; i < 4; ++i) out[i] = sGL.mClearColor[i]; n = 4; return true;      // COLOR_CLEAR_VALUE
    case 0x8038: out[0] = sGL.mOffsetFactor; return true;                           // POLYGON_OFFSET_FACTOR
    case 0x2A00: out[0] = sGL.mOffsetUnits; return true;                            // POLYGON_OFFSET_UNITS
    case 0x84FF: out[0] = Context::instance().getCaps().mMaxAnisotropy; return true;               // MAX_TEXTURE_MAX_ANISOTROPY
    case 0x846E: case 0x0B22:                                                                       // ALIASED_LINE_WIDTH_RANGE, SMOOTH_LINE_WIDTH_RANGE
        out[0] = Context::instance().getCaps().mLineWidthRange[0]; out[1] = Context::instance().getCaps().mLineWidthRange[1]; n = 2; return true;
    default: return false;
    }
}

bool emulate_boolean(GLenum pname, GLboolean* out, U32& n)
{
    n = 1;
    switch (pname)
    {
    case 0x0B72: out[0] = sGL.mDepthMask ? 1 : 0; return true;                      // DEPTH_WRITEMASK
    case 0x0C23: for (int i = 0; i < 4; ++i) out[i] = sGL.mColorMask[i] ? 1 : 0; n = 4; return true;       // COLOR_WRITEMASK
    default: return false;
    }
}

bool emulate_enabled(GLenum cap, bool& value)
{
    switch (cap)
    {
    case 0x0BE2: value = sGL.mBlend; return true;
    case 0x0B71: value = sGL.mDepthTest; return true;
    case 0x0B44: value = sGL.mCull; return true;
    case 0x0C11: value = sGL.mScissorTest; return true;
    case 0x0B90: value = sGL.mStencilTest; return true;
    case 0x8037: value = sGL.mPolyOffset; return true;
    case 0x864F: value = sGL.mDepthClamp; return true;
    default:
    {
        auto it = sGL.mOtherCaps.find(cap);
        value = it != sGL.mOtherCaps.end() ? it->second : (cap == 0x0BD0 || cap == 0x809D);      // DITHER and MULTISAMPLE start enabled, the rest disabled
        return true;
    }
    }
}

void note_getter(int kind, GLenum pname, bool handled, bool mismatch, const double* gl_vals, const double* emu_vals, U32 n)
{
    GetterTally& t = sGetterTally[{ kind, pname }];
    ++t.mCalls;
    if (!handled) return;
    ++t.mHandled;
    if (mismatch)
    {
        ++t.mMismatch;
        for (U32 i = 0; i < 4; ++i) { t.mGl[i] = i < n ? gl_vals[i] : 0; t.mEmu[i] = i < n ? emu_vals[i] : 0; }
        if (++t.mCount <= 3)
        {
            LL_INFOS("VkVerify") << "GETTER MISMATCH " << kGetterNames[kind] << "(0x" << std::hex << pname << std::dec << "): GL (" << t.mGl[0] << "," << t.mGl[1] << "," << t.mGl[2] << "," << t.mGl[3]
                                 << ") mirror (" << t.mEmu[0] << "," << t.mEmu[1] << "," << t.mEmu[2] << "," << t.mEmu[3] << ")" << LL_ENDL;
        }
    }
}

// ---- queries ----
struct QueryRec
{
    GLuint mName = 0;                   // the GL name the query was begun under
    U64 mGen = 0;                       // the async generation of that begin (0 in inline mode)
    GLenum mTarget = 0;
    bool mOcc = false, mTime = false;
    bool mVkHave = false, mHaveStart = false, mHaveEnd = false, mGlHave = false;
    U64 mVkValue = 0, mTsStart = 0, mTsEnd = 0, mGlValue = 0;
};
std::map<U64, QueryRec> sQueries;               // by serial, oldest first
std::unordered_map<GLuint, U64> sQueryLatest;   // GL query name -> serial of its latest begin
std::map<GLenum, U64> sQueryActive;             // target -> serial of the query in progress
U64 sQuerySerial = 0;
struct QueryTally
{
    U64 mOcc = 0, mOccAgree = 0, mGlVisibleVkHidden = 0, mGlHiddenVkVisible = 0, mCountPairs = 0;
    double mCountRelErr = 0;
    U64 mTimePairs = 0;
    double mTimeRatioSum = 0;
    U64 mNotIssued = 0, mIssued = 0, mDiscarded = 0;
} sQT;

bool queries_on()
{
    static const bool disabled = getenv("LL_VK_NO_QUERIES") != nullptr;
    return (LLVkVerify::gEnabled || gOwn) && !disabled;
}

void compare_query(U64 serial, QueryRec& rec)
{
    if (rec.mOcc)
    {
        ++sQT.mOcc;
        const bool gv = rec.mGlValue != 0, vv = rec.mVkValue != 0;
        if (gv == vv)
        {
            ++sQT.mOccAgree;
        }
        else if (gv)
        {
            if (++sQT.mGlVisibleVkHidden <= 15)
                LL_INFOS("VkVerify") << "OCCLUSION: GL says visible (" << rec.mGlValue << " samples), Vulkan says hidden (query " << serial << ", target 0x" << std::hex << rec.mTarget << std::dec << ")" << LL_ENDL;
        }
        else
        {
            if (++sQT.mGlHiddenVkVisible <= 15)
                LL_INFOS("VkVerify") << "OCCLUSION: GL says hidden, Vulkan says visible (" << rec.mVkValue << " samples) (query " << serial << ", target 0x" << std::hex << rec.mTarget << std::dec << ")" << LL_ENDL;
        }
        if (rec.mTarget == 0x8914 && rec.mGlValue && rec.mVkValue)       // SAMPLES_PASSED: the counts themselves
        {
            ++sQT.mCountPairs;
            sQT.mCountRelErr += std::fabs((double)rec.mGlValue - (double)rec.mVkValue) / (double)std::max(rec.mGlValue, rec.mVkValue);
        }
    }
    else if (rec.mTime)
    {
        if (rec.mGlValue)
        {
            ++sQT.mTimePairs;
            const double ratio = (double)rec.mVkValue / (double)rec.mGlValue;
            sQT.mTimeRatioSum += ratio;
            if (sQT.mTimePairs <= 12)
                LL_INFOS("VkVerify") << "TIMER: GL " << rec.mGlValue << " ns, Vulkan " << rec.mVkValue << " ns (ratio " << ratio << ")" << LL_ENDL;
        }
    }
}

// pulls finished Vulkan query results and joins them with what OpenGL reported
void drain_query_results()
{
    if (!Renderer::instance().isReady())
    {
        return;
    }
    static std::vector<Renderer::QueryResult> results;
    results.clear();
    Renderer::instance().takeQueryResults(results);
    const Capabilities& caps = Context::instance().getCaps();
    const U64 mask = caps.mTimestampValidBits >= 64 ? ~0ull : ((1ull << caps.mTimestampValidBits) - 1);
    for (const Renderer::QueryResult& r : results)
    {
        auto it = sQueries.find(r.mSerial);
        if (it == sQueries.end())
        {
            continue;
        }
        QueryRec& rec = it->second;
        if (r.mKind == 0) { rec.mVkValue = r.mValue; rec.mVkHave = true; }
        else if (r.mKind == 1) { rec.mTsStart = r.mValue; rec.mHaveStart = true; }
        else { rec.mTsEnd = r.mValue; rec.mHaveEnd = true; }
        if (rec.mTime && rec.mHaveStart && rec.mHaveEnd && !rec.mVkHave)
        {
            rec.mVkValue = (U64)(((rec.mTsEnd - rec.mTsStart) & mask) * (double)caps.mTimestampPeriodNs);
            rec.mVkHave = true;
        }
        if (rec.mVkHave && LLVkAsync::gEnabled && rec.mGen)
        {
            // hand the result to the main thread, which polls without waiting for this thread
            U64 v = rec.mVkValue;
            if (rec.mOcc && rec.mTarget != 0x8914) v = v ? 1 : 0;
            LLVkAsync::queryPublish(rec.mName, rec.mGen, v);
        }
        if (rec.mVkHave && rec.mGlHave)
        {
            compare_query(it->first, rec);
            sQueries.erase(it);
        }
    }
    while (sQueries.size() > 40000)         // results that never found their partner
    {
        sQueries.erase(sQueries.begin());
        ++sQT.mDiscarded;
    }
}

void note_query_result(GLuint id, U64 gl_value)
{
    drain_query_results();
    auto lat = sQueryLatest.find(id);
    if (lat == sQueryLatest.end())
    {
        return;
    }
    auto it = sQueries.find(lat->second);
    if (it == sQueries.end())
    {
        return;
    }
    QueryRec& rec = it->second;
    rec.mGlValue = gl_value;
    rec.mGlHave = true;
    if (rec.mVkHave)
    {
        compare_query(it->first, rec);
        sQueries.erase(it);
    }
}

} // anonymous namespace

void on_glGetIntegerv(GLenum pname, GLint* params)
{
    if (!LLVkVerify::gEnabled || !params) return;
    GLint emu[4] = {};
    U32 n = 1;
    bool limit = false;
    bool handled = emulate_integer(pname, emu, n);
    if (!handled) { handled = emulate_limit(pname, emu, n); limit = handled; }
    double g[4] = {}, e[4] = {};
    bool mismatch = false;
    for (U32 i = 0; handled && i < n; ++i)
    {
        g[i] = params[i]; e[i] = emu[i];
        mismatch |= limit ? (emu[i] < params[i]) : (emu[i] != params[i]);
    }
    note_getter(0, pname, handled, mismatch, g, e, n);
}

void on_glGetFloatv(GLenum pname, GLfloat* params)
{
    if (!LLVkVerify::gEnabled || !params) return;
    GLfloat emu[4] = {};
    U32 n = 1;
    const bool handled = emulate_float(pname, emu, n);
    double g[4] = {}, e[4] = {};
    bool mismatch = false;
    for (U32 i = 0; handled && i < n; ++i)
    {
        g[i] = params[i]; e[i] = emu[i];
        const bool limit = pname == 0x84FF;
        mismatch |= limit ? (emu[i] < params[i]) : (std::fabs(emu[i] - params[i]) > 1e-4f * std::max(1.f, std::fabs(params[i])));
    }
    note_getter(1, pname, handled, mismatch, g, e, n);
}

void on_glGetBooleanv(GLenum pname, GLboolean* params)
{
    if (!LLVkVerify::gEnabled || !params) return;
    GLboolean emu[4] = {};
    U32 n = 1;
    const bool handled = emulate_boolean(pname, emu, n);
    double g[4] = {}, e[4] = {};
    bool mismatch = false;
    for (U32 i = 0; handled && i < n; ++i)
    {
        g[i] = params[i] ? 1 : 0; e[i] = emu[i];
        mismatch |= (params[i] != 0) != (emu[i] != 0);
    }
    note_getter(2, pname, handled, mismatch, g, e, n);
}

void on_glIsEnabled(GLenum cap, GLboolean result)
{
    if (!LLVkVerify::gEnabled) return;
    bool emu = false;
    const bool handled = emulate_enabled(cap, emu);
    const double g = result ? 1 : 0, e = emu ? 1 : 0;
    note_getter(3, cap, handled, handled && (result != 0) != emu, &g, &e, 1);
}

void on_glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint* params)
{
    if (!LLVkVerify::gEnabled || !params) return;
    Tex* t = bound_texture(target);
    GLint emu = 0;
    bool handled = false;
    if (t && t->mImage.valid() && level >= 0 && (U32)level < std::max<U32>(1, t->mLevels))
    {
        switch (pname)
        {
        case 0x1000: emu = (GLint)std::max<U32>(1, t->mWidth >> level); handled = true; break;       // TEXTURE_WIDTH
        case 0x1001: emu = (GLint)std::max<U32>(1, t->mHeight >> level); handled = true; break;      // TEXTURE_HEIGHT
        case 0x8071: emu = (GLint)(t->mTarget == GL_TEXTURE_3D ? std::max<U32>(1, t->mDepth >> level) : std::max<U32>(1, t->mDepth)); handled = true; break;    // TEXTURE_DEPTH
        case 0x1003: emu = t->mInternalFormat; handled = true; break;                                // TEXTURE_INTERNAL_FORMAT
        default: break;
        }
    }
    const double g = params[0], e = emu;
    note_getter(4, pname, handled, handled && emu != params[0], &g, &e, 1);
}

void on_glCheckFramebufferStatus(GLenum target, GLenum result)
{
    if (!LLVkVerify::gEnabled) return;
    const GLuint name = target == GL_READ_FRAMEBUFFER ? sCtx.mReadFbo : sCtx.mDrawFbo;
    GLenum emu = 0x8CD5;        // FRAMEBUFFER_COMPLETE
    if (name)
    {
        auto it = sGL.mFbos.find(name);
        bool any = false;
        if (it != sGL.mFbos.end())
        {
            for (const FboAttachment& a : it->second.mColor) any |= a.mTexture != 0;
            any |= it->second.mDepth.mTexture != 0;
        }
        if (!any) emu = 0x8CD7;     // INCOMPLETE_MISSING_ATTACHMENT
    }
    const double g = result, e = emu;
    note_getter(5, target, true, result != emu, &g, &e, 1);
}

void on_glGenQueries(GLsizei n, GLuint* ids)
{
    (void)n; (void)ids;
}

void on_glDeleteQueries(GLsizei n, const GLuint* ids)
{
    if (!queries_on() || !ids) return;
    for (GLsizei i = 0; i < n; ++i) sQueryLatest.erase(ids[i]);
}

void on_glBeginQuery(GLenum target, GLuint id)
{
    if (!queries_on() || !LLVkMirror::inFrame() || !Renderer::instance().isReady())
    {
        if (queries_on()) ++sQT.mNotIssued;
        if (LLVkAsync::gEnabled) LLVkAsync::queryIdle(id, LLVkAsync::workerQueryGen(id));
        return;
    }
    Renderer& r = Renderer::instance();
    QueryRec rec;
    rec.mName = id;
    rec.mGen = LLVkAsync::gEnabled ? LLVkAsync::workerQueryGen(id) : 0;
    rec.mTarget = target;
    const U64 serial = ++sQuerySerial;
    bool ok = false;
    if (target == 0x8914 || target == 0x8C2F || target == 0x8D6A)       // SAMPLES_PASSED, ANY_SAMPLES_PASSED(_CONSERVATIVE)
    {
        rec.mOcc = true;
        ok = r.queryBegin(serial);
    }
    else if (target == 0x88BF)                                           // TIME_ELAPSED
    {
        rec.mTime = true;
        ok = r.queryTimestamp(serial, false);
    }
    if (!ok)
    {
        ++sQT.mNotIssued;
        sQueryActive.erase(target);
        if (LLVkAsync::gEnabled) LLVkAsync::queryIdle(id, LLVkAsync::workerQueryGen(id));
        return;
    }
    ++sQT.mIssued;
    sQueries[serial] = rec;
    sQueryLatest[id] = serial;
    sQueryActive[target] = serial;
}

void on_glEndQuery(GLenum target)
{
    if (!queries_on()) return;
    auto act = sQueryActive.find(target);
    if (act == sQueryActive.end() || !Renderer::instance().isReady())
    {
        return;
    }
    auto it = sQueries.find(act->second);
    if (it != sQueries.end() && LLVkMirror::inFrame())
    {
        if (it->second.mOcc) Renderer::instance().queryEnd();
        else if (it->second.mTime) Renderer::instance().queryTimestamp(act->second, true);
    }
    sQueryActive.erase(act);
}

void on_glQueryCounter(GLuint id, GLenum target)
{
    (void)id; (void)target;
}

void on_glGetQueryObjectiv(GLuint id, GLenum pname, GLint* params)
{
    if (queries_on() && params && pname == 0x8866) note_query_result(id, (U64)(U32)params[0]);
}

void on_glGetQueryObjectuiv(GLuint id, GLenum pname, GLuint* params)
{
    if (queries_on() && params && pname == 0x8866) note_query_result(id, params[0]);
}

void on_glGetQueryObjectui64v(GLuint id, GLenum pname, GLuint64* params)
{
    if (queries_on() && params && pname == 0x8866) note_query_result(id, (U64)params[0]);
}

// the running totals, for the log
void report_syncs_and_threads();
void report_getters_and_queries()
{
    std::lock_guard<LLVkMirror::Mutex> lock(gMutex);
    if (queries_on())
    {
        drain_query_results();
        LL_INFOS("VkVerify") << "QUERIES: issued to Vulkan " << sQT.mIssued << ", not issued " << sQT.mNotIssued << ", occlusion pairs compared " << sQT.mOcc << " (agree " << sQT.mOccAgree
                             << ", GL visible/Vulkan hidden " << sQT.mGlVisibleVkHidden << ", GL hidden/Vulkan visible " << sQT.mGlHiddenVkVisible << "), sample-count pairs " << sQT.mCountPairs
                             << " (mean relative difference " << (sQT.mCountPairs ? sQT.mCountRelErr / sQT.mCountPairs : 0.0) << "), timer pairs " << sQT.mTimePairs
                             << " (mean Vulkan/GL " << (sQT.mTimePairs ? sQT.mTimeRatioSum / sQT.mTimePairs : 0.0) << "), waiting " << sQueries.size() << ", discarded " << sQT.mDiscarded << LL_ENDL;
    }
    std::vector<std::pair<U64, std::pair<int, GLenum>>> order;
    for (auto& kv : sGetterTally) order.push_back({ kv.second.mCalls, kv.first });
    std::sort(order.rbegin(), order.rend());
    std::string line;
    U32 shown = 0;
    for (auto& o : order)
    {
        if (shown++ >= 45) break;
        const GetterTally& t = sGetterTally[o.second];
        char buf[160];
        snprintf(buf, sizeof(buf), " %s(0x%X): %llu calls, %s%llu mismatch;", kGetterNames[o.second.first], (unsigned)o.second.second, (unsigned long long)t.mCalls,
                 t.mHandled == t.mCalls ? "all checked, " : (t.mHandled ? "some checked, " : "NOT EMULATED, "), (unsigned long long)t.mMismatch);
        line += buf;
        if (line.size() > 1500) { LL_INFOS("VkVerify") << "GETTERS:" << line << LL_ENDL; line.clear(); }
    }
    if (!line.empty()) LL_INFOS("VkVerify") << "GETTERS:" << line << LL_ENDL;
    report_syncs_and_threads();
}

// at start-up: the limits and identity the viewer reads once, OpenGL against the Vulkan device
void verify_startup()
{
    std::lock_guard<LLVkMirror::Mutex> lock(gMutex);
    const Capabilities& caps = Context::instance().getCaps();
    const char* gl_renderer = (const char*)glGetString(GL_RENDERER);
    const char* gl_vendor = (const char*)glGetString(GL_VENDOR);
    const char* gl_version = (const char*)glGetString(GL_VERSION);
    LL_INFOS("VkVerify") << "identity: OpenGL renderer '" << (gl_renderer ? gl_renderer : "?") << "' vendor '" << (gl_vendor ? gl_vendor : "?") << "' version '" << (gl_version ? gl_version : "?")
                         << "'; Vulkan device '" << caps.mDeviceName << "' vendor id 0x" << std::hex << caps.mVendorId << std::dec << " api " << VK_API_VERSION_MAJOR(caps.mApiVersion) << "."
                         << VK_API_VERSION_MINOR(caps.mApiVersion) << LL_ENDL;
    static const GLenum limits[] = { 0x0D33, 0x84E8, 0x851C, 0x8073, 0x88FF, 0x8CDF, 0x8824, 0x8869, 0x8A30, 0x8872, 0x8B4C, 0x8B4D, 0x8D57, 0x0D3A };
    std::string line;
    U32 bad = 0;
    for (GLenum p : limits)
    {
        GLint gl_vals[4] = {}, emu[4] = {};
        U32 n = 1;
        glGetIntegerv(p, gl_vals);
        if (!emulate_limit(p, emu, n)) continue;
        bool ok = true;
        for (U32 i = 0; i < n; ++i) ok &= emu[i] >= gl_vals[i];
        bad += ok ? 0 : 1;
        char buf[120];
        snprintf(buf, sizeof(buf), " 0x%X: GL %d, Vulkan %d%s;", (unsigned)p, gl_vals[0], emu[0], ok ? "" : " (VULKAN LOWER)");
        line += buf;
    }
    LL_INFOS("VkVerify") << "limits (Vulkan must be at least as large as OpenGL, " << bad << " fall short):" << line << LL_ENDL;
}


// ---- sync objects and which threads feed the mirror ----
namespace
{
struct SyncRec { U64 mFrame = 0; bool mInFrame = false; U64 mFlush = 0; };
std::unordered_map<GLsync, SyncRec> sSyncs;
struct SyncTally
{
    U64 mCreated = 0, mPolls = 0, mPollAgree = 0, mGlDoneVkNot = 0, mVkDoneGlNot = 0, mBlockingWaits = 0, mServerWaits = 0, mUnknown = 0;
} sST;
}

void on_glFenceSync(GLenum condition, GLbitfield flags, GLsync result)
{
    if (!LLVkVerify::gEnabled || !result) return;
    SyncRec rec;
    rec.mFrame = Context::instance().getFrameNumber();
    rec.mInFrame = LLVkMirror::inFrame();
    sSyncs[result] = rec;
    ++sST.mCreated;
}

void on_glClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout, GLenum result)
{
    if (!LLVkVerify::gEnabled) return;
    auto it = sSyncs.find(sync);
    if (it == sSyncs.end())
    {
        ++sST.mUnknown;
        return;
    }
    if (timeout != 0)
    {
        ++sST.mBlockingWaits;        // GL waited until its own work was done; Vulkan would have to wait for its frame
        return;
    }
    ++sST.mPolls;
    const bool gl_done = result == 0x911A || result == 0x911C;      // ALREADY_SIGNALED, CONDITION_SATISFIED
    const bool vk_done = !it->second.mInFrame || Context::instance().frameFinished(it->second.mFrame);
    if (gl_done == vk_done) ++sST.mPollAgree;
    else if (gl_done) ++sST.mGlDoneVkNot;
    else ++sST.mVkDoneGlNot;
}

void on_glWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout)
{
    if (LLVkVerify::gEnabled) ++sST.mServerWaits;
}

void on_glDeleteSync(GLsync sync)
{
    if (LLVkVerify::gEnabled) sSyncs.erase(sync);
}

void report_syncs_and_threads()
{
    LL_INFOS("VkVerify") << "SYNC: fences created " << sST.mCreated << ", polls " << sST.mPolls << " (GL and Vulkan agree " << sST.mPollAgree << ", GL done but Vulkan not " << sST.mGlDoneVkNot
                         << ", Vulkan done but GL not " << sST.mVkDoneGlNot << "), blocking waits " << sST.mBlockingWaits << ", server-side waits " << sST.mServerWaits << ", unknown fences " << sST.mUnknown << LL_ENDL;
    std::string line;
    for (auto& t : sThreadCalls)
    {
        line += (t.first == sMainThread ? " main thread:" : " other thread:");
        for (auto& c : t.second) line += " " + c.first + " x" + std::to_string(c.second) + ",";
        line += ";";
    }
    LL_INFOS("VkVerify") << "THREADS feeding texture uploads into the mirror:" << line << LL_ENDL;
}


//-----------------------------------------------------------------------------------------------------
// LL_VK_VERIFY: compare GL readbacks with the same region read back from Vulkan
//-----------------------------------------------------------------------------------------------------

namespace
{

// Vulkan texel layout -> float RGBA per pixel (missing channels 0, missing alpha 1). False if the format is not handled.
bool decode_vk_texels(VkFormat f, const std::vector<U8>& texels, size_t pixels, std::vector<float>& out)
{
    U32 comps = 0;
    enum { B8, H16, F32 } kind = B8;
    switch (f)
    {
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: comps = 4; kind = B8; break;
    case VK_FORMAT_R8G8_UNORM: comps = 2; kind = B8; break;
    case VK_FORMAT_R8_UNORM: comps = 1; kind = B8; break;
    case VK_FORMAT_R16G16B16A16_SFLOAT: comps = 4; kind = H16; break;
    case VK_FORMAT_R16_SFLOAT: comps = 1; kind = H16; break;
    case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_D32_SFLOAT: comps = 1; kind = F32; break;
    case VK_FORMAT_R32G32B32A32_SFLOAT: comps = 4; kind = F32; break;
    default: return false;
    }
    const size_t comp_bytes = kind == B8 ? 1 : kind == H16 ? 2 : 4;
    const size_t bytes_per = comps * comp_bytes;
    if (texels.size() < pixels * bytes_per)
    {
        return false;
    }
    out.assign(pixels * 4, 0.f);
    for (size_t i = 0; i < pixels; ++i)
    {
        float* o = &out[i * 4];
        o[3] = 1.f;
        for (U32 c = 0; c < comps; ++c)
        {
            const U8* src = &texels[i * bytes_per + c * comp_bytes];
            if (kind == B8) o[c] = *src / 255.f;
            else if (kind == H16) { U16 h; memcpy(&h, src, 2); o[c] = half_to_float(h); }
            else { memcpy(&o[c], src, 4); }
        }
    }
    return true;
}

// the channels (indices into RGBA) a GL pixel format stores, in order
bool gl_channels(GLenum format, std::vector<int>& ch)
{
    switch (format)
    {
    case GL_RGBA: ch = { 0, 1, 2, 3 }; return true;
    case GL_BGRA: ch = { 2, 1, 0, 3 }; return true;
    case GL_RGB: ch = { 0, 1, 2 }; return true;
    case GL_BGR: ch = { 2, 1, 0 }; return true;
    case GL_RG: ch = { 0, 1 }; return true;
    case GL_RED: case GL_DEPTH_COMPONENT: ch = { 0 }; return true;
    default: return false;
    }
}

U32 gl_type_bytes(GLenum type)
{
    switch (type)
    {
    case GL_UNSIGNED_BYTE: return 1;
    case GL_HALF_FLOAT: return 2;
    case GL_FLOAT: return 4;
    default: return 0;
    }
}

float read_gl_component(const U8* p, GLenum type)
{
    if (type == GL_UNSIGNED_BYTE) return *p / 255.f;
    if (type == GL_HALF_FLOAT) { U16 h; memcpy(&h, p, 2); return half_to_float(h); }
    float f; memcpy(&f, p, 4); return f;
}

struct VerifyCounters
{
    U32 mChecks = 0, mMismatching = 0, mSkipped = 0;
    std::map<std::string, U32> mSkipReasons;
};
VerifyCounters sVerify;

void verify_skip(const std::string& why)
{
    ++sVerify.mSkipped;
    if (sVerify.mSkipReasons[why]++ == 0)
    {
        LL_INFOS("VkVerify") << "skipped (first time): " << why << LL_ENDL;
    }
}

// Compare GL's bytes with the decoded Vulkan region. 'what' labels the log line.
double verify_compare(const std::string& what, VkFormat vf, const std::vector<U8>& vk_texels, S32 w, S32 h, GLenum format, GLenum type, const void* gl_data)
{
    std::vector<int> ch;
    const U32 tb = gl_type_bytes(type);
    if (!gl_channels(format, ch) || !tb)
    {
        verify_skip("GL format/type not handled: " + std::to_string((S32)format) + "/" + std::to_string((S32)type));
        return 0;
    }
    std::vector<float> src;
    if (!decode_vk_texels(vf, vk_texels, (size_t)w * h, src))
    {
        verify_skip("Vulkan format not handled: " + std::to_string((S32)vf));
        return 0;
    }
    GLint pack_align = 4, row_length = 0;
    glGetIntegerv(GL_PACK_ALIGNMENT, &pack_align);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &row_length);
    const size_t pixel_bytes = ch.size() * tb;
    const size_t row_pixels = row_length > 0 ? (size_t)row_length : (size_t)w;
    const size_t stride = ((row_pixels * pixel_bytes + pack_align - 1) / pack_align) * pack_align;
    const float tol = type == GL_UNSIGNED_BYTE ? 2.5f / 255.f : (format == GL_DEPTH_COMPONENT ? 0.002f : 0.02f);

    size_t compared = 0, bad = 0;
    double sum = 0, maxd = 0;
    S32 first_x = -1, first_y = -1;
    float first_gl = 0, first_vk = 0;
    for (S32 y = 0; y < h; ++y)
    {
        const U8* row = (const U8*)gl_data + (size_t)y * stride;
        for (S32 x = 0; x < w; ++x)
        {
            for (size_t c = 0; c < ch.size(); ++c)
            {
                const float g = read_gl_component(row + (size_t)x * pixel_bytes + c * tb, type);
                const float v = src[((size_t)y * w + x) * 4 + ch[c]];
                const double d = std::fabs((double)g - (double)v);
                ++compared;
                sum += d;
                maxd = std::max(maxd, d);
                if (d > tol * std::max(1.0f, std::fabs(g)))
                {
                    if (!bad) { first_x = x; first_y = y; first_gl = g; first_vk = v; }
                    ++bad;
                }
            }
        }
    }
    ++sVerify.mChecks;
    const bool mismatch = bad > 0;
    if (mismatch) ++sVerify.mMismatching;
    if (mismatch || sVerify.mChecks <= 30 || (sVerify.mChecks % 200) == 0)
    {
        LL_INFOS("VkVerify") << (mismatch ? "MISMATCH " : "match ") << what << ": " << w << "x" << h << " fmt " << (S32)format << " type " << (S32)type
                             << " vk " << (S32)vf << ": " << bad << " of " << compared << " values differ, max diff " << maxd << ", mean " << (sum / std::max<size_t>(1, compared))
                             << (mismatch ? (" first at (" + std::to_string(first_x) + "," + std::to_string(first_y) + ") GL " + std::to_string(first_gl) + " VK " + std::to_string(first_vk)) : std::string())
                             << " [checks " << sVerify.mChecks << ", mismatching " << sVerify.mMismatching << ", skipped " << sVerify.mSkipped << "]" << LL_ENDL;
    }
    return sum / std::max<size_t>(1, compared);
}

// writes the whole default framebuffer as GL has it and as Vulkan has it, for looking at (first few bad cases only)
void verify_dump_backbuffer(Image& view)
{
    static U32 dumped = 0;
    if (dumped >= 4 || sCtx.mReadFbo != 0)
    {
        return;
    }
    ++dumped;
    CreateDirectoryA("D:\\claude\\vk-dump", nullptr);
    const U32 w = view.mExtent.width, h = view.mExtent.height;
    std::vector<U8> gl_px((size_t)w * h * 4, 0), vk_px, vk_rgba;
    GLint align = 4;
    glGetIntegerv(GL_PACK_ALIGNMENT, &align);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, gl_px.data());
    glPixelStorei(GL_PACK_ALIGNMENT, align);
    Renderer& r = Renderer::instance();
    if (!r.readbackRegion(view, 0, 0, 0, 0, w, h, vk_px) || !to_rgba8(view.mFormat, w, h, vk_px, vk_rgba))
    {
        LL_INFOS("VkVerify") << "dump: could not read the Vulkan backbuffer" << LL_ENDL;
        return;
    }
    // GL images are stored bottom row first; flip for viewing
    std::vector<U8> gl_flip(gl_px.size()), vk_flip(vk_rgba.size());
    for (U32 y = 0; y < h; ++y)
    {
        memcpy(&gl_flip[(size_t)y * w * 4], &gl_px[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
        memcpy(&vk_flip[(size_t)y * w * 4], &vk_rgba[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
    }
    char path[160];
    snprintf(path, sizeof(path), "D:\\claude\\vk-dump\\verify%u_gl_%ux%u.png", dumped, w, h);
    write_png(path, w, h, gl_flip);
    snprintf(path, sizeof(path), "D:\\claude\\vk-dump\\verify%u_vk_%ux%u.png", dumped, w, h);
    write_png(path, w, h, vk_flip);
    LL_INFOS("VkVerify") << "dump " << dumped << ": wrote GL and Vulkan backbuffer images (" << w << "x" << h << ") to D:\\claude\\vk-dump, mirror inFrame " << LLVkMirror::inFrame() << LL_ENDL;
}

// the Vulkan side of a readback: make the frame's work visible, then copy the rectangle out
bool verify_fetch(Image& view, S32 x, S32 y, S32 w, S32 h, std::vector<U8>& texels)
{
    Renderer& r = Renderer::instance();
    if (LLVkMirror::inFrame() && !r.flushAndRestart())
    {
        return false;
    }
    const U32 mip = view.mOwner ? view.mBaseMip : 0, layer = view.mOwner ? view.mBaseLayer : 0;
    return r.readbackRegion(view, mip, layer, x, y, (U32)w, (U32)h, texels);
}

} // anonymous namespace

void verify_read_pixels(S32 x, S32 y, S32 w, S32 h, GLenum format, GLenum type, const void* gl_data)
{
    std::lock_guard<LLVkMirror::Mutex> lock(gMutex);
    if (!gActive || !gl_data || w <= 0 || h <= 0)
    {
        return;
    }
    Image* img = read_image();
    if (!img || !img->valid())
    {
        verify_skip("no Vulkan image behind the read framebuffer");
        return;
    }
    const VkFormat vf = img->mOwner ? img->mOwner->mFormat : img->mFormat;
    std::vector<U8> texels;
    if (!verify_fetch(*img, x, y, w, h, texels))
    {
        verify_skip("Vulkan readback failed (format " + std::to_string((S32)vf) + ")");
        return;
    }
    std::string what = "readPixels " + std::string(sCtx.mReadFbo == 0 ? "backbuffer" : ("fbo " + std::to_string(sCtx.mReadFbo))) + " @(" + std::to_string(x) + "," + std::to_string(y) + ")";
    const double mean = verify_compare(what, vf, texels, w, h, format, type, gl_data);
    if (mean > 0.05 && sCtx.mReadFbo == 0)
    {
        verify_dump_backbuffer(*img);
    }
}

// Whole-frame check at the moment the viewer swaps buffers: GL's default framebuffer against the mirror's backbuffer.
// Runs every 300th frame, or when the file D:\claude\vk-verify.trigger exists (it is deleted). Logs the share of pixels that
// differ per cell of a 8x6 grid, and writes both images when the frames differ a lot.
void verify_present_frame()
{
    static U64 counter = 0;
    static U32 dumps = 0;
    ++counter;
    if ((counter % 600) == 0)
    {
        report_getters_and_queries();
    }
    const bool triggered = GetFileAttributesA("D:\\claude\\vk-verify.trigger") != INVALID_FILE_ATTRIBUTES;
    if (triggered)
    {
        report_getters_and_queries();
    }
    if (!triggered && (counter % 300) != 0)
    {
        return;
    }
    if (triggered)
    {
        DeleteFileA("D:\\claude\\vk-verify.trigger");
    }
    std::lock_guard<LLVkMirror::Mutex> lock(gMutex);
    Image* img = LLVkMirror::backbufferColor();
    if (!gActive || !img || !img->valid() || !LLVkMirror::inFrame())
    {
        return;
    }
    const U32 w = img->mExtent.width, h = img->mExtent.height;
    std::vector<U8> gl_px((size_t)w * h * 4, 0), vk_px, vk_rgba;
    GLint prev_read = 0, align = 4;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
    glGetIntegerv(GL_PACK_ALIGNMENT, &align);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, gl_px.data());
    glPixelStorei(GL_PACK_ALIGNMENT, align);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read);

    Renderer& r = Renderer::instance();
    if (!r.flushAndRestart() || !r.readbackRegion(*img, 0, 0, 0, 0, w, h, vk_px) || !to_rgba8(img->mFormat, w, h, vk_px, vk_rgba))
    {
        verify_skip("present-frame readback failed");
        return;
    }
    const int GX = 8, GY = 6;
    std::vector<U32> cell_bad(GX * GY, 0), cell_total(GX * GY, 0);
    size_t bad = 0;
    for (U32 y = 0; y < h; ++y)
    {
        for (U32 x = 0; x < w; ++x)
        {
            const size_t i = ((size_t)y * w + x) * 4;
            const int d = std::max({ std::abs((int)gl_px[i] - (int)vk_rgba[i]), std::abs((int)gl_px[i + 1] - (int)vk_rgba[i + 1]), std::abs((int)gl_px[i + 2] - (int)vk_rgba[i + 2]) });
            const int cell = (int)(y * GY / h) * GX + (int)(x * GX / w);      // row 0 of the grid is the bottom of the image
            ++cell_total[cell];
            if (d > 40) { ++cell_bad[cell]; ++bad; }
        }
    }
    std::string grid;
    for (int gy = GY - 1; gy >= 0; --gy)        // print the top of the image first
    {
        grid += " [";
        for (int gx = 0; gx < GX; ++gx)
        {
            grid += std::to_string(cell_bad[gy * GX + gx] * 100 / std::max<U32>(1, cell_total[gy * GX + gx])) + (gx + 1 < GX ? " " : "");
        }
        grid += "]";
    }
    const double pct = 100.0 * bad / ((double)w * h);
    LL_INFOS("VkVerify") << (triggered ? "TRIGGERED " : "") << "present frame " << w << "x" << h << ": " << pct << "% of pixels differ by more than 40; percent per grid cell, top row first:" << grid << LL_ENDL;
    if (pct > 1.0 && dumps < 6)
    {
        ++dumps;
        CreateDirectoryA("D:\\claude\\vk-dump", nullptr);
        std::vector<U8> gl_flip(gl_px.size()), vk_flip(vk_rgba.size());
        for (U32 y = 0; y < h; ++y)
        {
            memcpy(&gl_flip[(size_t)y * w * 4], &gl_px[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
            memcpy(&vk_flip[(size_t)y * w * 4], &vk_rgba[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
        }
        char path[160];
        snprintf(path, sizeof(path), "D:\\claude\\vk-dump\\present%u_gl_%ux%u.png", dumps, w, h);
        write_png(path, w, h, gl_flip);
        snprintf(path, sizeof(path), "D:\\claude\\vk-dump\\present%u_vk_%ux%u.png", dumps, w, h);
        write_png(path, w, h, vk_flip);
        LL_INFOS("VkVerify") << "present frame dump " << dumps << " written to D:\\claude\\vk-dump" << LL_ENDL;
    }
}


// A snapshot is about to render: record per-pass GL-vs-Vulkan comparisons ("SNAP" lines, every program) for it
void verify_arm_snapshot()
{
    std::lock_guard<LLVkMirror::Mutex> lock(gMutex);
    if (!gActive)
    {
        return;
    }
    sSnaps.clear();
    sSnapActive = true;
    sSnapStage = 0;
    sSnapKey.clear();
    sSnapExtra.assign(1, "*");
    sVerifySnapshotPending = true;
    sStats = DrawStats();
    LL_INFOS("VkVerify") << "snapshot starting: per-pass GL/Vulkan comparison armed for every program" << LL_ENDL;
}

void verify_tex_image(GLenum target, S32 level, GLenum format, GLenum type, const void* gl_data)
{
    std::lock_guard<LLVkMirror::Mutex> lock(gMutex);
    if (!gActive || !gl_data)
    {
        return;
    }
    if (target != GL_TEXTURE_2D)
    {
        verify_skip("texImage target " + std::to_string((S32)target) + " not handled");
        return;
    }
    GLint bound = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    auto it = sGL.mTextures.find((GLuint)bound);
    if (it == sGL.mTextures.end() || !it->second.mImage.valid() || level < 0 || (U32)level >= it->second.mImage.mMipLevels)
    {
        verify_skip("texture " + std::to_string(bound) + " unknown to the mirror or level out of range");
        return;
    }
    Tex& t = it->second;
    const S32 w = (S32)std::max<U32>(1, t.mImage.mExtent.width >> level), h = (S32)std::max<U32>(1, t.mImage.mExtent.height >> level);
    std::vector<U8> texels;
    if (!verify_fetch(t.mImage, 0, 0, w, h, texels))
    {
        verify_skip("Vulkan texture readback failed");
        return;
    }
    std::string what = "getTexImage tex " + std::to_string(bound) + " level " + std::to_string(level);
    verify_compare(what, t.mImage.mFormat, texels, w, h, format, type, gl_data);
}

//-----------------------------------------------------------------------------------------------------
// LL_VK_OWN: the mirror owns the resources. The wrappers no longer make the sink calls in real OpenGL; the objects exist only
// in Vulkan, and the calls that return something are answered here.
//-----------------------------------------------------------------------------------------------------

void own_gen(int kind, GLsizei n, GLuint* out)
{
    static std::atomic<GLuint> next[5] = { 1, 1, 1, 1, 1 };       // textures, framebuffers, buffers, vertex arrays, queries; 0 is never a name
    for (GLsizei i = 0; i < n; ++i)
    {
        out[i] = next[kind].fetch_add(1);
    }
}

// the async worker: one context state per producing thread (0 is the viewer's main thread)
static PerContext* sCtxTable[256] = {};
void register_main_context()
{
    sCtxTable[0] = &sCtxTls;        // the main thread's state so far carries on in the worker
}
void set_worker_context(int id)
{
    PerContext*& slot = sCtxTable[id & 255];
    if (!slot)
    {
        slot = new PerContext;
    }
    sCtxOverride = slot;
}

namespace
{

U32 pack_alignment() { return sCtx.mPackAlignment > 0 ? (U32)sCtx.mPackAlignment : 4u; }

// the questions the mirror could not answer and left to OpenGL (logged once each)
void note_fallback(const char* fn, GLenum pname)
{
    static std::set<std::pair<std::string, GLenum>> seen;
    if (seen.insert({ fn, pname }).second)
    {
        LL_INFOS("VkOwn") << "not answered by the mirror, OpenGL answers (and its state is stale): " << fn << "(0x" << std::hex << pname << std::dec << ")" << LL_ENDL;
    }
}

// where a pack operation writes: the bound pixel pack buffer (the pointer is then an offset) or client memory
U8* pack_destination(void* pixels)
{
    if (sCtx.mPixelPackBuffer)
    {
        auto it = sGL.mBuffers.find(sCtx.mPixelPackBuffer);
        if (it == sGL.mBuffers.end() || !it->second.mVk.valid() || !it->second.mVk.mMapped)
        {
            return nullptr;
        }
        return (U8*)it->second.mVk.mMapped + (size_t)(uintptr_t)pixels;
    }
    return (U8*)pixels;
}

U16 float_to_half_bits(float f)
{
    U32 x;
    memcpy(&x, &f, 4);
    const U32 sign = (x >> 16) & 0x8000u;
    S32 exp = (S32)((x >> 23) & 0xFF) - 127 + 15;
    U32 man = x & 0x7FFFFF;
    if (exp <= 0) return (U16)sign;                              // flush denormals to zero
    if (exp >= 31) return (U16)(sign | 0x7C00u);
    return (U16)(sign | ((U32)exp << 10) | (man >> 13));
}

// writes decoded RGBA floats (w x h, row 0 first) into GL's pixel layout
bool encode_to_gl(const std::vector<float>& rgba, S32 w, S32 h, GLenum format, GLenum type, U8* dst, S32 row_length = 0)
{
    std::vector<int> ch;
    const U32 tb = gl_type_bytes(type);
    if (!dst || !gl_channels(format, ch) || !tb)
    {
        return false;
    }
    const size_t pixel_bytes = ch.size() * tb;
    const size_t row_pixels = row_length > 0 ? (size_t)row_length : (size_t)w;
    const U32 align = pack_alignment();
    const size_t stride = ((row_pixels * pixel_bytes + align - 1) / align) * align;
    for (S32 y = 0; y < h; ++y)
    {
        U8* row = dst + (size_t)y * stride;
        for (S32 x = 0; x < w; ++x)
        {
            for (size_t c = 0; c < ch.size(); ++c)
            {
                const float v = rgba[((size_t)y * w + x) * 4 + ch[c]];
                U8* p = row + (size_t)x * pixel_bytes + c * tb;
                if (type == GL_UNSIGNED_BYTE)
                {
                    *p = (U8)std::lround(std::min(1.f, std::max(0.f, v)) * 255.f);
                }
                else if (type == GL_HALF_FLOAT)
                {
                    const U16 hb = float_to_half_bits(v);
                    memcpy(p, &hb, 2);
                }
                else
                {
                    memcpy(p, &v, 4);
                }
            }
        }
    }
    return true;
}

// The last full mip level read back, so a snapshot that reads it row by row costs one readback
struct ReadCache
{
    const Image* mImage = nullptr;
    U32 mMip = 0, mLayer = 0;
    U64 mEpoch = ~0ull;
    U32 mW = 0, mH = 0;
    VkFormat mFormat = VK_FORMAT_UNDEFINED;
    std::vector<U8> mTexels;
} sReadCache;

// tightly packed texels of the rectangle (x, y, w, h) of 'view', as the image holds them
bool fetch_rect_cached(Image& view, S32 x, S32 y, S32 w, S32 h, std::vector<U8>& out, VkFormat& fmt)
{
    Renderer& r = Renderer::instance();
    Image* root = view.mOwner ? view.mOwner : &view;
    const U32 mip = view.mOwner ? view.mBaseMip : 0, layer = view.mOwner ? view.mBaseLayer : 0;
    const FormatInfo fi = formatInfo(root->mFormat);
    if (!fi.mKnown || fi.mCompressed)
    {
        return false;
    }
    if (sReadCache.mImage != &view || sReadCache.mMip != mip || sReadCache.mLayer != layer || sReadCache.mEpoch != r.modificationCount())
    {
        if (LLVkMirror::inFrame() && !r.flushAndRestart())
        {
            return false;
        }
        const U32 mw = std::max<U32>(1, root->mExtent.width >> mip), mh = std::max<U32>(1, root->mExtent.height >> mip);
        sReadCache.mTexels.clear();
        if (!r.readbackRegion(view, mip, layer, 0, 0, mw, mh, sReadCache.mTexels))
        {
            sReadCache.mImage = nullptr;
            return false;
        }
        sReadCache.mImage = &view;
        sReadCache.mMip = mip;
        sReadCache.mLayer = layer;
        sReadCache.mEpoch = r.modificationCount();
        sReadCache.mW = mw;
        sReadCache.mH = mh;
        sReadCache.mFormat = root->mFormat;
    }
    if (x < 0 || y < 0 || (U32)(x + w) > sReadCache.mW || (U32)(y + h) > sReadCache.mH)
    {
        return false;
    }
    fmt = sReadCache.mFormat;
    out.resize((size_t)w * h * fi.mBytesPerBlock);
    for (S32 row = 0; row < h; ++row)
    {
        memcpy(&out[(size_t)row * w * fi.mBytesPerBlock], &sReadCache.mTexels[((size_t)(y + row) * sReadCache.mW + x) * fi.mBytesPerBlock], (size_t)w * fi.mBytesPerBlock);
    }
    return true;
}

// ---- query answers ----
bool own_query_value(GLuint id, GLenum pname, U64& value)
{
    drain_query_results();
    auto lat = sQueryLatest.find(id);
    auto it = lat == sQueryLatest.end() ? sQueries.end() : sQueries.find(lat->second);
    if (pname == 0x8867)         // QUERY_RESULT_AVAILABLE
    {
        value = (it == sQueries.end() || it->second.mVkHave) ? 1 : 0;
        return true;
    }
    if (pname != 0x8866)         // QUERY_RESULT
    {
        return false;
    }
    if (it == sQueries.end())
    {
        value = 1;               // never issued to Vulkan: report 'visible' so nothing is wrongly hidden
        return true;
    }
    if (!it->second.mVkHave)
    {
        Renderer::instance().resolveQueries();
        drain_query_results();
        it = sQueries.find(lat->second);
    }
    if (it != sQueries.end() && it->second.mVkHave)
    {
        value = it->second.mVkValue;
        if (it->second.mOcc && it->second.mTarget != 0x8914) value = value ? 1 : 0;      // ANY_SAMPLES_PASSED is a boolean
        LLVkAsync::queryResultTally(value != 0);
        sQueries.erase(it);
        sQueryLatest.erase(lat);
        if (LLVkAsync::gEnabled) LLVkAsync::queryIdle(id, LLVkAsync::workerQueryGen(id));
        return true;
    }
    value = it != sQueries.end() && it->second.mOcc ? 1 : 0;
    return true;
}

} // anonymous namespace

// LL_VK_PHASES: which state getters the viewer asks for, by function and parameter name
void phase_getter(int fn, GLenum pname)
{
    if (LLVkMirror::gPhaseTiming && LLVkMirror::onMainThread())
    {
        ++sGetterCounts[{ fn, pname }];
    }
}

bool own_glGetIntegerv(GLenum pname, GLint* params)
{
    phase_getter(0, pname);
    GLint emu[4] = {};
    U32 n = 1;
    if (!params || !(emulate_integer(pname, emu, n) || emulate_limit(pname, emu, n)))
    {
        note_fallback("glGetIntegerv", pname);
        return false;
    }
    for (U32 i = 0; i < n; ++i) params[i] = emu[i];
    return true;
}

bool own_glGetFloatv(GLenum pname, GLfloat* params)
{
    phase_getter(1, pname);
    GLfloat emu[4] = {};
    U32 n = 1;
    if (!params || !emulate_float(pname, emu, n))
    {
        note_fallback("glGetFloatv", pname);
        return false;
    }
    for (U32 i = 0; i < n; ++i) params[i] = emu[i];
    return true;
}

bool own_glGetBooleanv(GLenum pname, GLboolean* params)
{
    phase_getter(2, pname);
    GLboolean emu[4] = {};
    U32 n = 1;
    if (!params || !emulate_boolean(pname, emu, n))
    {
        note_fallback("glGetBooleanv", pname);
        return false;
    }
    for (U32 i = 0; i < n; ++i) params[i] = emu[i];
    return true;
}

bool own_glIsEnabled(GLenum cap, GLboolean& out)
{
    phase_getter(3, cap);
    bool v = false;
    if (!emulate_enabled(cap, v))
    {
        note_fallback("glIsEnabled", cap);
        return false;
    }
    out = v ? GL_TRUE : GL_FALSE;
    return true;
}

bool own_glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint* params)
{
    phase_getter(4, pname);
    if (!params)
    {
        return true;
    }
    Tex* t = bound_texture(target);
    params[0] = 0;
    if (t && t->mImage.valid() && level >= 0 && (U32)level < std::max<U32>(1, t->mLevels))
    {
        switch (pname)
        {
        case 0x1000: params[0] = (GLint)std::max<U32>(1, t->mWidth >> level); break;
        case 0x1001: params[0] = (GLint)std::max<U32>(1, t->mHeight >> level); break;
        case 0x8071: params[0] = (GLint)(t->mTarget == GL_TEXTURE_3D ? std::max<U32>(1, t->mDepth >> level) : std::max<U32>(1, t->mDepth)); break;
        case 0x1003: params[0] = t->mInternalFormat; break;
        case 0x86A1: params[0] = formatInfo(t->mImage.mFormat).mCompressed ? 1 : 0; break;
        case 0x86A0: params[0] = (GLint)Renderer::compressedMipBytes(t->mImage, (U32)level); break;     // TEXTURE_COMPRESSED_IMAGE_SIZE
        default: break;
        }
    }
    return true;
}

bool own_glCheckFramebufferStatus(GLenum target, GLenum& out)
{
    const GLuint name = target == GL_READ_FRAMEBUFFER ? sCtx.mReadFbo : sCtx.mDrawFbo;
    out = 0x8CD5;       // FRAMEBUFFER_COMPLETE
    if (name)
    {
        auto it = sGL.mFbos.find(name);
        bool any = false;
        if (it != sGL.mFbos.end())
        {
            for (const FboAttachment& a : it->second.mColor) any |= a.mTexture != 0;
            any |= it->second.mDepth.mTexture != 0;
        }
        if (!any) out = 0x8CD7;     // INCOMPLETE_MISSING_ATTACHMENT
    }
    return true;
}

bool own_glGetQueryObjectiv(GLuint id, GLenum pname, GLint* params)
{
    U64 v = 0;
    if (!params || !own_query_value(id, pname, v)) return false;
    params[0] = (GLint)v;
    return true;
}

bool own_glGetQueryObjectuiv(GLuint id, GLenum pname, GLuint* params)
{
    U64 v = 0;
    if (!params || !own_query_value(id, pname, v)) return false;
    params[0] = (GLuint)v;
    return true;
}

bool own_glGetQueryObjectui64v(GLuint id, GLenum pname, GLuint64* params)
{
    U64 v = 0;
    if (!params || !own_query_value(id, pname, v)) return false;
    params[0] = v;
    return true;
}

bool own_glFenceSync(GLenum condition, GLbitfield flags, GLsync& out)
{
    static U64 counter = 1;
    out = (GLsync)(uintptr_t)(++counter << 4);
    SyncRec rec;
    rec.mFrame = Context::instance().getFrameNumber();
    rec.mInFrame = LLVkMirror::inFrame();
    rec.mFlush = Renderer::instance().flushCount();
    sSyncs[out] = rec;
    return true;
}

bool own_glClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout, GLenum& out)
{
    auto it = sSyncs.find(sync);
    if (it == sSyncs.end())
    {
        out = 0x911A;       // ALREADY_SIGNALED
        return true;
    }
    Context& ctx = Context::instance();
    Renderer& r = Renderer::instance();
    const SyncRec& rec = it->second;
    const bool done = !rec.mInFrame || ctx.frameFinished(rec.mFrame) || r.flushCount() != rec.mFlush;
    if (done)
    {
        out = 0x911A;
        return true;
    }
    if (timeout == 0)
    {
        out = 0x911B;       // TIMEOUT_EXPIRED
        return true;
    }
    // a blocking wait: finish what has been recorded so far (the frame goes on afterwards), or wait for the earlier frames
    if (rec.mFrame == ctx.getFrameNumber() && LLVkMirror::inFrame())
    {
        r.flushAndRestart();
    }
    else
    {
        ctx.waitIdle();
    }
    out = 0x911C;           // CONDITION_SATISFIED
    return true;
}

bool own_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels)
{
    U8* dst = pack_destination(pixels);
    Image* img = read_image();
    std::vector<U8> texels;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    std::vector<float> rgba;
    if (width <= 0 || height <= 0)
    {
        return true;
    }
    if (!dst)
    {
        return true;
    }
    if (!img || !img->valid() || !fetch_rect_cached(*img, x, y, width, height, texels, fmt) || !decode_vk_texels(fmt, texels, (size_t)width * height, rgba)
        || !encode_to_gl(rgba, width, height, format, type, dst, sCtx.mPackRowLength))
    {
        static U32 reported = 0;
        if (reported++ < 5)
        {
            LL_WARNS("VkGL") << "glReadPixels could not be answered from Vulkan (" << width << "x" << height << " format 0x" << std::hex << format << " type 0x" << type << std::dec << ")" << LL_ENDL;
        }
        // GL would have written something; hand back zeros so the caller does not read garbage
        std::vector<int> ch;
        if (gl_channels(format, ch) && gl_type_bytes(type))
        {
            const size_t stride = (((size_t)width * ch.size() * gl_type_bytes(type) + pack_alignment() - 1) / pack_alignment()) * pack_alignment();
            memset(dst, 0, stride * (size_t)height);
        }
    }
    return true;
}

bool own_glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, void* pixels)
{
    U8* dst = pack_destination(pixels);
    Tex* t = bound_texture(target);
    if (!dst || !t || !t->mImage.valid() || level < 0 || (U32)level >= t->mImage.mMipLevels)
    {
        return true;
    }
    Renderer& r = Renderer::instance();
    if (LLVkMirror::inFrame() && !r.flushAndRestart())
    {
        return true;
    }
    const S32 w = (S32)std::max<U32>(1, t->mImage.mExtent.width >> level), h = (S32)std::max<U32>(1, t->mImage.mExtent.height >> level);
    std::vector<int> ch;
    if (!gl_channels(format, ch) || !gl_type_bytes(type))
    {
        return true;
    }
    const size_t stride = (((size_t)w * ch.size() * gl_type_bytes(type) + pack_alignment() - 1) / pack_alignment()) * pack_alignment();
    // 2D: one image; cube map face: that face; arrays and cube maps: every layer one after the other
    U32 first_layer = 0, layers = 1;
    if (target >= 0x8515 && target <= 0x851A) first_layer = target - 0x8515;
    else if (target_class(target) == 2 || target_class(target) == 3) layers = t->mImage.mLayers;
    else if (target_class(target) == 1) layers = 6;
    for (U32 l = 0; l < layers; ++l)
    {
        std::vector<U8> texels;
        std::vector<float> rgba;
        if (!r.readbackRegion(t->mImage, (U32)level, first_layer + l, 0, 0, (U32)w, (U32)h, texels) || !decode_vk_texels(t->mImage.mFormat, texels, (size_t)w * h, rgba))
        {
            continue;
        }
        encode_to_gl(rgba, w, h, format, type, dst + (size_t)l * stride * h, 0);
    }
    return true;
}

bool own_glGetCompressedTexImage(GLenum target, GLint level, void* img)
{
    U8* dst = pack_destination(img);
    Tex* t = bound_texture(target);
    if (!dst || !t || !t->mImage.valid() || level < 0 || (U32)level >= t->mImage.mMipLevels)
    {
        return true;
    }
    Renderer& r = Renderer::instance();
    std::vector<U8> blocks;
    if (!formatInfo(t->mImage.mFormat).mCompressed)
    {
        // generic compressed formats are stored as plain RGBA8 here, so there are no blocks to hand back
        static bool said = false;
        if (!said)
        {
            said = true;
            LL_WARNS("VkGL") << "glGetCompressedTexImage on a texture the mirror stores uncompressed (generic compressed format)" << LL_ENDL;
        }
        return true;
    }
    if (LLVkMirror::inFrame() && !r.flushAndRestart())
    {
        return true;
    }
    U32 layer = 0;
    if (target >= 0x8515 && target <= 0x851A) layer = target - 0x8515;
    if (r.readbackCompressedMip(t->mImage, (U32)level, layer, blocks))
    {
        memcpy(dst, blocks.data(), blocks.size());
    }
    return true;
}

void on_glReadPixels(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*) {}
void on_glGetTexImage(GLenum, GLint, GLenum, GLenum, void*) {}
void on_glGetCompressedTexImage(GLenum, GLint, void*) {}


//-----------------------------------------------------------------------------------------------------
// LL_VK_OWN: shader and program objects. GLSL is compiled to SPIR-V with glslang at glLinkProgram; the object names, the list of
// active uniforms, uniform and attribute locations and the link status are made up here.
//-----------------------------------------------------------------------------------------------------

namespace
{

struct OwnShader { GLenum mType = 0; std::string mSource; };
struct OwnActive { std::string mName; GLenum mType = 0x1406; GLint mSize = 1; GLint mLocation = 0; };
struct OwnProgram
{
    std::vector<GLuint> mShaders;
    bool mLinked = false;
    std::string mLog;
    ProgramInfo mInfo;
    std::vector<OwnActive> mActive;
    std::unordered_map<std::string, std::pair<GLint, GLint>> mBase;     // uniform name (no [0]) -> (location, element count)
};
std::unordered_map<GLuint, OwnShader> sOwnShaders;
std::unordered_map<GLuint, OwnProgram> sOwnPrograms;
GLuint sNextOwnShader = 1, sNextOwnProgram = 1;

GLenum gl_enum_of_type(const std::string& t)
{
    static const std::unordered_map<std::string, GLenum> map = {
        { "float", 0x1406 }, { "vec2", 0x8B50 }, { "vec3", 0x8B51 }, { "vec4", 0x8B52 },
        { "int", 0x1404 }, { "ivec2", 0x8B53 }, { "ivec3", 0x8B54 }, { "ivec4", 0x8B55 },
        { "bool", 0x8B56 }, { "bvec2", 0x8B57 }, { "bvec3", 0x8B58 }, { "bvec4", 0x8B59 },
        { "mat2", 0x8B5A }, { "mat3", 0x8B5B }, { "mat4", 0x8B5C },
        { "mat2x3", 0x8B65 }, { "mat2x4", 0x8B66 }, { "mat3x2", 0x8B67 }, { "mat3x4", 0x8B68 }, { "mat4x2", 0x8B69 }, { "mat4x3", 0x8B6A },
        { "uint", 0x1405 }, { "uvec2", 0x8DC6 }, { "uvec3", 0x8DC7 }, { "uvec4", 0x8DC8 },
        { "sampler1D", 0x8B5D }, { "sampler2D", 0x8B5E }, { "sampler3D", 0x8B5F }, { "samplerCube", 0x8B60 },
        { "sampler1DShadow", 0x8B61 }, { "sampler2DShadow", 0x8B62 }, { "sampler2DRect", 0x8B63 }, { "sampler2DRectShadow", 0x8B64 },
        { "sampler2DArray", 0x8DC1 }, { "sampler2DArrayShadow", 0x8DC4 }, { "samplerCubeShadow", 0x8DC5 },
        { "samplerCubeArray", 0x900C }, { "sampler2DMS", 0x9108 },
    };
    auto it = map.find(t);
    return it == map.end() ? 0x1406 : it->second;
}

OwnProgram* own_program(GLuint id)
{
    auto it = sOwnPrograms.find(id);
    return it == sOwnPrograms.end() ? nullptr : &it->second;
}

void own_link(GLuint id)
{
    OwnProgram* p = own_program(id);
    if (!p)
    {
        return;
    }
    p->mLinked = false;
    p->mActive.clear();
    p->mBase.clear();
    std::vector<StageSource> stages;
    for (GLuint sh : p->mShaders)
    {
        auto it = sOwnShaders.find(sh);
        if (it == sOwnShaders.end())
        {
            continue;
        }
        StageSource s;
        s.mName = "shader " + std::to_string(sh);
        s.mSource = it->second.mSource;
        switch (it->second.mType)
        {
        case GL_VERTEX_SHADER: s.mStage = StageSource::Vertex; break;
        case GL_FRAGMENT_SHADER: s.mStage = StageSource::Fragment; break;
        case GL_GEOMETRY_SHADER: s.mStage = StageSource::Geometry; break;
        default: continue;
        }
        stages.push_back(std::move(s));
    }
    ShaderEnv env;
    env.mAttributeNames = LLShaderMgr::instance()->mReservedAttribs;
    env.mUniformBlockNames = { "ReflectionProbes", "GLTFJoints", "GLTFNodes", "GLTFMaterials" };
    const char* dump = getenv("LL_VK_SHADER_DUMP_DIR");
    ProgramInfo info;
    if (!compileProgram("program " + std::to_string(id), stages, env, info, dump ? dump : ""))
    {
        p->mLog = info.mError;
        return;
    }
    p->mInfo = std::move(info);
    GLint next_location = 0;
    for (const LooseUniform& u : p->mInfo.mUniforms)
    {
        OwnActive a;
        a.mName = u.mArraySize ? u.mName + "[0]" : u.mName;
        a.mType = gl_enum_of_type(u.mType);
        a.mSize = u.mArraySize ? (GLint)u.mArraySize : 1;
        a.mLocation = next_location;
        p->mBase[u.mName] = { a.mLocation, a.mSize };
        next_location += a.mSize;
        p->mActive.push_back(std::move(a));
    }
    for (const SamplerInfo& s : p->mInfo.mSamplers)
    {
        OwnActive a;
        a.mName = s.mName;
        a.mType = gl_enum_of_type(s.mType);
        a.mSize = 1;
        a.mLocation = next_location;
        p->mBase[s.mName] = { a.mLocation, 1 };
        next_location += 1;
        p->mActive.push_back(std::move(a));
    }
    p->mLinked = true;
    p->mLog.clear();
}

void copy_string(const std::string& s, GLsizei buf_size, GLsizei* length, GLchar* out)
{
    const GLsizei n = buf_size > 0 ? std::min<GLsizei>((GLsizei)s.size(), buf_size - 1) : 0;
    if (out && buf_size > 0)
    {
        memcpy(out, s.data(), (size_t)n);
        out[n] = 0;
    }
    if (length)
    {
        *length = n;
    }
}

} // anonymous namespace

const ProgramInfo* own_program_info(GLuint program)
{
    OwnProgram* p = own_program(program);
    return p && p->mLinked ? &p->mInfo : nullptr;
}

bool own_glCreateShader(GLenum type, GLuint& out)
{
    out = sNextOwnShader++;
    sOwnShaders[out].mType = type;
    return true;
}

bool own_glCreateProgram(GLuint& out)
{
    out = sNextOwnProgram++;
    sOwnPrograms[out];
    return true;
}

bool own_glIsShader(GLuint shader, GLboolean& out)
{
    out = sOwnShaders.count(shader) ? GL_TRUE : GL_FALSE;
    return true;
}

bool own_glIsProgram(GLuint program, GLboolean& out)
{
    out = sOwnPrograms.count(program) ? GL_TRUE : GL_FALSE;
    return true;
}

bool own_glGetShaderiv(GLuint shader, GLenum pname, GLint* params)
{
    if (!params)
    {
        return true;
    }
    switch (pname)
    {
    case 0x8B81: params[0] = 1; break;                                             // COMPILE_STATUS: errors show up when the program is linked
    case 0x8B4F: { auto it = sOwnShaders.find(shader); params[0] = it == sOwnShaders.end() ? 0 : (GLint)it->second.mType; } break;     // SHADER_TYPE
    default: params[0] = 0; break;                                                  // INFO_LOG_LENGTH, DELETE_STATUS, ...
    }
    return true;
}

bool own_glGetShaderInfoLog(GLuint, GLsizei buf_size, GLsizei* length, GLchar* log)
{
    copy_string(std::string(), buf_size, length, log);
    return true;
}

bool own_glGetProgramiv(GLuint program, GLenum pname, GLint* params)
{
    if (!params)
    {
        return true;
    }
    OwnProgram* p = own_program(program);
    params[0] = 0;
    if (!p)
    {
        return true;
    }
    switch (pname)
    {
    case 0x8B82: params[0] = p->mLinked ? 1 : 0; break;                             // LINK_STATUS
    case 0x8B83: params[0] = p->mLinked ? 1 : 0; break;                             // VALIDATE_STATUS
    case 0x8B84: params[0] = p->mLog.empty() ? 0 : (GLint)p->mLog.size() + 1; break;      // INFO_LOG_LENGTH
    case 0x8B85: params[0] = (GLint)p->mShaders.size(); break;                      // ATTACHED_SHADERS
    case 0x8B86: params[0] = (GLint)p->mActive.size(); break;                       // ACTIVE_UNIFORMS
    case 0x8B87: params[0] = 256; break;                                            // ACTIVE_UNIFORM_MAX_LENGTH
    case 0x8B89: params[0] = (GLint)p->mInfo.mAttributes.size(); break;             // ACTIVE_ATTRIBUTES
    case 0x8A36: params[0] = 0; break;                                              // ACTIVE_UNIFORM_BLOCKS
    default: break;                                                                 // PROGRAM_BINARY_LENGTH and the rest: 0
    }
    return true;
}

bool own_glGetProgramInfoLog(GLuint program, GLsizei buf_size, GLsizei* length, GLchar* log)
{
    OwnProgram* p = own_program(program);
    copy_string(p ? p->mLog : std::string(), buf_size, length, log);
    return true;
}

bool own_glGetAttachedShaders(GLuint program, GLsizei max_count, GLsizei* count, GLuint* shaders)
{
    OwnProgram* p = own_program(program);
    GLsizei n = 0;
    if (p && shaders)
    {
        for (GLuint s : p->mShaders)
        {
            if (n < max_count) shaders[n++] = s;
        }
    }
    if (count) *count = n;
    return true;
}

bool own_glGetActiveUniform(GLuint program, GLuint index, GLsizei buf_size, GLsizei* length, GLint* size, GLenum* type, GLchar* name)
{
    OwnProgram* p = own_program(program);
    if (!p || index >= p->mActive.size())
    {
        if (length) *length = 0;
        if (size) *size = 0;
        if (type) *type = 0;
        if (name && buf_size > 0) name[0] = 0;
        return true;
    }
    const OwnActive& a = p->mActive[index];
    copy_string(a.mName, buf_size, length, name);
    if (size) *size = a.mSize;
    if (type) *type = a.mType;
    return true;
}

bool own_glGetActiveUniformsiv(GLuint, GLsizei count, const GLuint*, GLenum pname, GLint* params)
{
    if (params)
    {
        for (GLsizei i = 0; i < count; ++i) params[i] = pname == 0x8A3A ? -1 : 0;       // UNIFORM_BLOCK_INDEX: none of these are block members
    }
    return true;
}

bool own_glGetUniformLocation(GLuint program, const GLchar* name, GLint& out)
{
    out = -1;
    OwnProgram* p = own_program(program);
    if (!p || !name)
    {
        return true;
    }
    std::string n(name);
    GLint element = 0;
    const size_t bracket = n.find('[');
    if (bracket != std::string::npos)
    {
        element = atoi(n.c_str() + bracket + 1);
        n.resize(bracket);
    }
    auto it = p->mBase.find(n);
    if (it != p->mBase.end() && element >= 0 && element < it->second.second)
    {
        out = it->second.first + element;
    }
    return true;
}

bool own_glGetAttribLocation(GLuint program, const GLchar* name, GLint& out)
{
    out = -1;
    OwnProgram* p = own_program(program);
    if (!p || !name)
    {
        return true;
    }
    for (size_t i = 0; i < p->mInfo.mAttributes.size() && i < p->mInfo.mAttributeLocations.size(); ++i)
    {
        if (p->mInfo.mAttributes[i] == name)
        {
            out = (GLint)p->mInfo.mAttributeLocations[i];
            break;
        }
    }
    return true;
}

bool own_glGetUniformBlockIndex(GLuint program, const GLchar* name, GLuint& out)
{
    static const char* const names[] = { "ReflectionProbes", "GLTFJoints", "GLTFNodes", "GLTFMaterials" };
    out = 0xFFFFFFFFu;      // INVALID_INDEX
    for (GLuint i = 0; name && i < 4; ++i)
    {
        if (strcmp(name, names[i]) == 0) out = i;
    }
    return true;
}

void on_glShaderSource(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length)
{
    if (!gOwn) return;
    auto it = sOwnShaders.find(shader);
    if (it == sOwnShaders.end()) return;
    std::string& src = it->second.mSource;
    src.clear();
    for (GLsizei i = 0; i < count; ++i)
    {
        if (!string[i]) continue;
        if (length && length[i] >= 0) src.append(string[i], (size_t)length[i]);
        else src += string[i];
    }
}

void on_glCompileShader(GLuint) {}
// the questions are only answered by the mirror when it owns the resources; with OpenGL running next to it there is nothing to do
void on_glCreateShader(GLenum, GLuint) {}
void on_glGetShaderiv(GLuint, GLenum, GLint*) {}
void on_glGetShaderInfoLog(GLuint, GLsizei, GLsizei*, GLchar*) {}
void on_glIsShader(GLuint, GLboolean) {}
void on_glCreateProgram(GLuint) {}
void on_glIsProgram(GLuint, GLboolean) {}
void on_glGetProgramiv(GLuint, GLenum, GLint*) {}
void on_glGetProgramInfoLog(GLuint, GLsizei, GLsizei*, GLchar*) {}
void on_glGetAttachedShaders(GLuint, GLsizei, GLsizei*, GLuint*) {}
void on_glGetActiveUniform(GLuint, GLuint, GLsizei, GLsizei*, GLint*, GLenum*, GLchar*) {}
void on_glGetActiveUniformsiv(GLuint, GLsizei, const GLuint*, GLenum, GLint*) {}
void on_glGetUniformLocation(GLuint, const GLchar*, GLint) {}
void on_glGetAttribLocation(GLuint, const GLchar*, GLint) {}
void on_glGetUniformBlockIndex(GLuint, const GLchar*, GLuint) {}
void on_glDeleteShader(GLuint shader) { if (gOwn) sOwnShaders.erase(shader); }

void on_glAttachShader(GLuint program, GLuint shader)
{
    if (!gOwn) return;
    if (OwnProgram* p = own_program(program)) p->mShaders.push_back(shader);
}

void on_glDetachShader(GLuint program, GLuint shader)
{
    if (!gOwn) return;
    if (OwnProgram* p = own_program(program)) p->mShaders.erase(std::remove(p->mShaders.begin(), p->mShaders.end(), shader), p->mShaders.end());
}

void on_glDeleteProgram(GLuint program) { if (gOwn) sOwnPrograms.erase(program); }
void on_glBindAttribLocation(GLuint, GLuint, const GLchar*) {}
void on_glLinkProgram(GLuint program) { if (gOwn) own_link(program); }
void on_glValidateProgram(GLuint) {}
void on_glUniformBlockBinding(GLuint, GLuint, GLuint) {}
void on_glUseProgram(GLuint) {}
void on_glProgramParameteri(GLuint, GLenum, GLint) {}
void on_glProgramBinary(GLuint, GLenum, const void*, GLsizei) {}

void on_upscale(const LLVkMirror::UpscaleArgs& a)
{
    static U32 failures = 0;
    if (!gActive || !LLVkDlss::ready())
    {
        return;
    }
    auto find = [](GLuint name) -> Tex* {
        auto it = sGL.mTextures.find(name);
        return it != sGL.mTextures.end() && it->second.mImage.valid() ? &it->second : nullptr;
    };
    Tex* color = find(a.mColor);
    Tex* depth = find(a.mDepth);
    Tex* motion = find(a.mMotion);
    Tex* output = find(a.mOutput);
    if (!color || !depth || !motion || !output)
    {
        if (failures++ < 5)
        {
            LL_WARNS("VkDlss") << "upscale: a texture is missing (colour " << (color != nullptr) << ", depth " << (depth != nullptr) << ", motion " << (motion != nullptr)
                               << ", output " << (output != nullptr) << ")" << LL_ENDL;
        }
        return;
    }
    LLVkDlss::Params p;
    p.mColor = &color->mImage;
    p.mDepth = &depth->mImage;
    p.mMotion = &motion->mImage;
    p.mOutput = &output->mImage;
    p.mRenderWidth = a.mRenderWidth;
    p.mRenderHeight = a.mRenderHeight;
    p.mOutputWidth = a.mOutputWidth;
    p.mOutputHeight = a.mOutputHeight;
    p.mJitterX = a.mJitterX;
    p.mJitterY = a.mJitterY;
    p.mMotionScaleX = a.mMotionScaleX;
    p.mMotionScaleY = a.mMotionScaleY;
    p.mReset = a.mReset != 0;
    p.mHdr = a.mHdr != 0;
    const std::vector<std::pair<Image*, VkImageLayout>> needs = {
        { p.mColor, LLVkDlss::inputLayout(*p.mColor) },
        { p.mDepth, LLVkDlss::inputLayout(*p.mDepth) },
        { p.mMotion, LLVkDlss::inputLayout(*p.mMotion) },
        { p.mOutput, LLVkDlss::outputLayout() },
    };
    bool done = false;
    Renderer::instance().recordExternal(needs, [&](VkCommandBuffer cmd) { done = LLVkDlss::evaluate(cmd, p); });
    if (done)
    {
        output->mValidLevels = 1;       // what was written counts as the texture's content
    }
    else if (failures++ < 5)
    {
        LL_WARNS("VkDlss") << "the upscale did not run" << LL_ENDL;
    }
}


} // namespace LLVkGL

#include "llvkverify.h"

namespace LLVkVerify
{
bool gEnabled = false;

void readPixels(int x, int y, int w, int h, unsigned int format, unsigned int type, const void* gl_data)
{
    LLVkGL::verify_read_pixels(x, y, w, h, format, type, gl_data);
}

void armSnapshot()
{
    LLVkGL::verify_arm_snapshot();
}

void presentFrame()
{
    LLVkGL::verify_present_frame();
}

void startup()
{
    LLVkGL::verify_startup();
}

void texImage(unsigned int target, int level, unsigned int format, unsigned int type, const void* gl_data)
{
    LLVkGL::verify_tex_image(target, level, format, type, gl_data);
}
}

#endif // LL_VULKAN

