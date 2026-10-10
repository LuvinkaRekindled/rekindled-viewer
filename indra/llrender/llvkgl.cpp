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
#include "llglslshader.h"
#include "llshadermgr.h"
#include "llvkshader.h"
#include "llvkcontext.h"
#include "llvkrenderer.h"
#include "llvkdlss.h"
#include "llvkrt.h"

#include <chrono>
#include "llvkgl_hooks.inc"      // generated: storage and names for the per-hook statistics

namespace LLVkGL
{
const char* hook_name(int i) { return (i >= 0 && i < HOOK_COUNT) ? kHookNames[i] : "?"; }
}
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
    const bool mOn = LLVkMirror::gPhaseTiming;      // the total only goes into the LL_VK_PHASES report: no clock reads (two per draw) otherwise
    HookTimer() { if (mOn) QueryPerformanceCounter(&mStart); }
    ~HookTimer()
    {
        if (mOn)
        {
            LARGE_INTEGER end;
            QueryPerformanceCounter(&end);
            gHookTicks += end.QuadPart - mStart.QuadPart;
        }
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

// 8-bit RGB / RGBA client rows into an RGBA8 or RGBA8 sRGB texture: nothing to decide per texel
bool is_rgba8_fast(const FormatMap& fm, GLenum format, GLenum type)
{
    const U32 comps = format_components(format);
    return !fm.mDepth && (fm.mVk == VK_FORMAT_R8G8B8A8_UNORM || fm.mVk == VK_FORMAT_R8G8B8A8_SRGB) && type == GL_UNSIGNED_BYTE
        && ((comps == 4 && format != 0x80E1) || (comps == 3 && format != 0x80E0));
}

// writes the texels front to back and never reads them back, so 'out' may be write-combined staging memory
void convert_rgba8_rows(const U8* fbase, U32 width, U32 height, U32 depth, U32 comps, U32 src_row_bytes, U8* out)
{
    for (U32 z = 0; z < depth; ++z)
    {
        for (U32 y = 0; y < height; ++y)
        {
            const U8* row = fbase + ((size_t)z * height + y) * src_row_bytes;
            U8* dst = out + (((size_t)z * height + y) * width) * 4;
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
    if (is_rgba8_fast(fm, format, type))
    {
        out.resize((size_t)width * height * depth * 4);
        convert_rgba8_rows((const U8*)src, width, height, depth, comps, src_row_bytes, out.data());
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
    U32 mBindlessEpoch = 0;                     // <FS> the slot this texture holds in the bindless table, for the frame (epoch) and image view it was taken for
    U32 mBindlessSlot = 0;
    VkImageView mBindlessView = VK_NULL_HANDLE;
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
    // Device path with the buffer arena (LL_VK_ARENA, default on): mVk is a view of mArena's range (mBuffer = the block's buffer, mAddress = the range's address,
    // mSize = the data size; it owns nothing) and draws bind mVk.mBuffer at mArena.mOffset. Without the arena mArena is empty and mVk is a buffer of its own.
    ArenaRef mArena;
    // mEpoch changes when the name gets other contents altogether (re-specified with another size, deleted, recycled by the vertex buffer pool); mVersion on every write;
    // mMoves when the same contents were copied to another range (a rename of a host-written range). A built acceleration structure keeps its own copy of the triangles, so it
    // is keyed on (name, epoch) and refitted on version; the address only matters when something reads the buffer (builds, shaders), and is looked up then.
    U64 mEpoch = 0;
    U64 mVersion = 0;
    U64 mMoves = 0;
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

// <FS> flat lookup of the entries of sGL.mTextures / mBuffers by GL name (the names are small integers from the viewer's name pools). A hash lookup for every sampler of
// every draw cost more than the rest of the draw's bookkeeping. The pointers are to the nodes of the maps (stable until erased); erasing forgets them.
std::vector<Tex*> sTexFlat;
std::vector<Buf*> sBufFlat;

inline Tex* tex_ptr(GLuint name)
{
    if (name < sTexFlat.size())
    {
        if (Tex* t = sTexFlat[name])
        {
            return t;
        }
    }
    auto it = sGL.mTextures.find(name);
    if (it == sGL.mTextures.end())
    {
        return nullptr;
    }
    if (name < (1u << 22))
    {
        if (name >= sTexFlat.size())
        {
            sTexFlat.resize(std::max<size_t>((size_t)name + 1, sTexFlat.size() * 2), nullptr);
        }
        sTexFlat[name] = &it->second;
    }
    return &it->second;
}
inline void tex_forget(GLuint name) { if (name < sTexFlat.size()) sTexFlat[name] = nullptr; }

inline Buf* buf_ptr(GLuint name)
{
    if (name < sBufFlat.size())
    {
        if (Buf* b = sBufFlat[name])
        {
            return b;
        }
    }
    auto it = sGL.mBuffers.find(name);
    if (it == sGL.mBuffers.end())
    {
        return nullptr;
    }
    if (name < (1u << 22))
    {
        if (name >= sBufFlat.size())
        {
            sBufFlat.resize(std::max<size_t>((size_t)name + 1, sBufFlat.size() * 2), nullptr);
        }
        sBufFlat[name] = &it->second;
    }
    return &it->second;
}
inline void buf_forget(GLuint name) { if (name < sBufFlat.size()) sBufFlat[name] = nullptr; }

// <FS> what sync_target() resolved for the current draw framebuffer, valid until something that can change it happens (bumps sTargetEpoch)
U64 sTargetEpoch = 1;
const void* sTargetNamesOwner = nullptr;      // whose list of attachment names sCurTargets holds
std::set<GLuint> sFrameTargets;         // textures rendered into during the current frame (for the debug dump)
GLuint sUboBinding[4] = {};             // GL buffer bound to each named uniform block binding point
std::map<GLuint, U32> sFrameSampled;    // textures read by draws this frame, with use counts
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

//-----------------------------------------------------------------------------------------------------
// textures
//-----------------------------------------------------------------------------------------------------

void destroy_tex_images(Tex& t)
{
    ++sTargetEpoch;
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
    ++sTargetEpoch;
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
    if (LLVkAsync::gTexOneCopy && LLVkAsync::gEnabled && d == 1 && t.mTarget != GL_TEXTURE_2D_ARRAY && t.mTarget != GL_TEXTURE_CUBE_MAP_ARRAY && t.mTarget != GL_TEXTURE_3D
        && is_rgba8_fast(t.mFormat, format, type) && w && h)
    {
        // C4: the common upload (8-bit RGB or RGBA into an RGBA8 texture) is read from where the viewer left it and written straight into the staging buffer: one copy
        struct Fast { const U8* mSrc; U32 mW, mH, mComps, mRowBytes; } fast;
        const U32 alignment = sCtx.mUnpackAlignment > 0 ? (U32)sCtx.mUnpackAlignment : 4, row_length = (U32)sCtx.mUnpackRowLength;
        fast.mSrc = (const U8*)src;
        fast.mW = w;
        fast.mH = h;
        fast.mComps = format_components(format);
        fast.mRowBytes = ((row_length ? row_length : w) * fast.mComps + alignment - 1) / alignment * alignment;
        U32 layer = 0;
        if (target >= 0x8515 && target <= 0x851A) layer = target - 0x8515;       // cube face
        Renderer::instance().uploadTextureDirect(t.mImage, (U32)level, layer, (U32)x, (U32)y, w, h, 1, (VkDeviceSize)w * h * 4,
                                                 [](void* p, U8* dst, VkDeviceSize) { const Fast* f = (const Fast*)p; convert_rgba8_rows(f->mSrc, f->mW, f->mH, 1, f->mComps, f->mRowBytes, dst); }, &fast);
        return;
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
        const auto t_conv = std::chrono::steady_clock::now();
        const bool converted = convert_pixels(fm, format, type, src, w, h, d, alignment, row_length, texels);
        LLVkAsync::noteTexWorker((U64)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t_conv).count(), 0, 0, 0);
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
    Tex* tp = tex_ptr(name);
    if (!tp || !tp->mImage.valid())
    {
        return nullptr;
    }
    Tex& t = *tp;
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
    if (sCtx.mDrawFbo == 0)
    {
        sCurTargets.clear();
        sTargetNamesOwner = nullptr;
    }
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
    // the framebuffer's attachments were resolved for an earlier draw and nothing that could change them has happened since (and the frame did not turn over)
    struct TargetCache { U64 mEpoch = 0; GLuint mFbo = ~0u; U64 mFrame = ~0ull; Image* mColors[8] = {}; U32 mCount = 0; Image* mDepth = nullptr; bool mOk = false; std::vector<GLuint> mNames; };
    static TargetCache cache;
    const U64 frame_now = Context::instance().getFrameNumber();
    if (cache.mEpoch == sTargetEpoch && cache.mFbo == sCtx.mDrawFbo && cache.mFrame == frame_now && !sDbgMatch && cache.mOk)
    {
        if (sTargetNamesOwner != &cache)
        {
            sCurTargets = cache.mNames;
            sTargetNamesOwner = &cache;
        }
        r.setRenderTarget(cache.mColors, cache.mCount, cache.mDepth);
        sHasDepth = cache.mDepth != nullptr;
        if (cache.mCount)
        {
            sCurTargetW = cache.mColors[0]->mExtent.width;
            sCurTargetH = cache.mColors[0]->mExtent.height;
        }
        return true;
    }
    sTargetNamesOwner = nullptr;
    sCurTargets.clear();
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
    cache.mEpoch = sTargetEpoch;
    cache.mFbo = sCtx.mDrawFbo;
    cache.mFrame = frame_now;
    memcpy(cache.mColors, colors, sizeof(colors));
    cache.mCount = count;
    cache.mDepth = depth;
    cache.mNames = sCurTargets;
    cache.mOk = true;
    sTargetNamesOwner = &cache;
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
    {
        const float bias = LLVkDlss::lodBias();      // a new DLSS mode changes it: the sampler is made again
        U32 bias_bits;
        memcpy(&bias_bits, &bias, 4);
        mix(bias_bits);
    }
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
    // <FS> the whole translation below is a function of these GL state values: when none changed (and the renderer's state was not set by anything else) it is skipped
    struct DrawStateKey
    {
        U32 mFlags;
        GLenum mCullFace, mPolygonMode, mDepthFunc, mBlendSrcRGB, mBlendDstRGB, mBlendSrcA, mBlendDstA;
        F32 mLineWidth, mOffsetUnits, mOffsetFactor;
        S32 mViewport[4], mScissor[4];
    };
    static DrawStateKey last;
    static U32 last_gen = 0;
    static bool last_valid = false;
    DrawStateKey key;
    memset(&key, 0, sizeof(key));
    key.mFlags = (sGL.mCull ? 1u : 0) | (sGL.mPolyOffset ? 2u : 0) | (sGL.mDepthClamp ? 4u : 0) | (sGL.mDepthTest ? 8u : 0) | (sGL.mDepthMask ? 16u : 0) | (sGL.mBlend ? 32u : 0)
        | (sGL.mColorMask[0] ? 64u : 0) | (sGL.mColorMask[1] ? 128u : 0) | (sGL.mColorMask[2] ? 256u : 0) | (sGL.mColorMask[3] ? 512u : 0) | (sGL.mViewportSet ? 1024u : 0)
        | (sGL.mScissorTest ? 2048u : 0) | (has_depth ? 4096u : 0);
    key.mCullFace = sGL.mCullFace; key.mPolygonMode = sGL.mPolygonMode; key.mDepthFunc = sGL.mDepthFunc;
    key.mBlendSrcRGB = sGL.mBlendSrcRGB; key.mBlendDstRGB = sGL.mBlendDstRGB; key.mBlendSrcA = sGL.mBlendSrcA; key.mBlendDstA = sGL.mBlendDstA;
    key.mLineWidth = sGL.mLineWidth; key.mOffsetUnits = sGL.mOffsetUnits; key.mOffsetFactor = sGL.mOffsetFactor;
    for (int i = 0; i < 4; ++i) { key.mViewport[i] = sGL.mViewport[i]; key.mScissor[i] = sGL.mScissor[i]; }
    if (!sDbgMatch && last_valid && last_gen == r.stateGeneration() && memcmp(&key, &last, sizeof(key)) == 0)
    {
        return;
    }
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
    last = key;
    last_gen = r.stateGeneration();
    last_valid = true;
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
        Tex* tex = tex_ptr(sCtx.mBound[unit][cls]);
        if (sStatsEnabled)
        {
            const std::string& type = info.mSamplers[k].mType;
            static const char* const watch[] = { "Soften", "Windlight Sky", "Deferred Sun", "Deferred Light Shader", "Irradiance Gen", "Radiance Gen", "HUD", "Copy Depth", "Haze", "Tonemap", "Reflection Mip", "Glow" };
            bool wanted = false;
            for (const char* w : watch) if (program.getName().find(w) != std::string::npos) wanted = true;
            if (wanted)
            {
                std::string what = "unit " + std::to_string(unit) + " tex " + std::to_string(sCtx.mBound[unit][cls]) + " ";
                if (!tex) what += "UNBOUND";
                else if (!tex->mImage.valid()) what += "NO IMAGE";
                else
                {
                    const Tex& tx = *tex;
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
        if (tex && tex->mImage.valid())
        {
            r.setTexture(unit, &tex->mImage, texture_sampler(r, *tex));
            if (sStatsEnabled)
            {
                const GLuint tname = sCtx.mBound[unit][cls];
                ++sFrameSampled[tname];
                if (std::find(sCurTargets.begin(), sCurTargets.end(), tname) != sCurTargets.end())
                {
                    ++sStats.mFeedback[program.getName() + " samples " + info.mSamplers[k].mName + " = tex " + std::to_string(tname)];
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
    // <FS> the attributes of a draw mostly come from one buffer: it is looked up once, and marked as used right here (mark_used used to look every buffer up again)
    GLuint last_name = 0;
    auto last_it = sGL.mBuffers.end();
    const U64 frame_now = Context::instance().getFrameNumber();

    for (size_t a = 0; a < info.mAttributes.size(); ++a)
    {
        const U32 loc = info.mAttributeLocations[a];
        if (loc >= 16)
        {
            continue;
        }
        VaoAttrib& at = vao.mAttrib[loc];
        auto bit = sGL.mBuffers.end();
        bool from_buffer = false;
        if (at.mEnabled && at.mBuffer)
        {
            if (at.mBuffer != last_name)
            {
                last_name = at.mBuffer;
                last_it = sGL.mBuffers.find(at.mBuffer);
            }
            bit = last_it;
            from_buffer = bit != sGL.mBuffers.end() && bit->second.mVk.valid();
        }
        if (from_buffer)
        {
            bit->second.mLastUseFrame = frame_now;
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
                b.mOffset = base + bit->second.mArena.mOffset;
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


} // anonymous namespace

namespace { void flush_buffer_updates(bool frame_end = false); }


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
            it->second.mLastUseFrame = Context::instance().getFrameNumber();      // <FS> (mark_used no longer walks the buffers of a draw)
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

struct BufTraffic;
void note_flush(bool frame_end, size_t buffers);

void flush_buffer_updates(bool frame_end)
{
    if (sPendingBufs.empty())
    {
        return;
    }
    const size_t pending = sPendingBufs.size();
    if (!Renderer::instance().flushBufferUpdates())
    {
        return;                         // no frame is being recorded: the writes stay queued
    }
    note_flush(frame_end, pending);
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

bool arena_enabled()
{
    static const bool on = []() { const char* e = getenv("LL_VK_ARENA"); return !(e && e[0] == '0'); }();
    return on;
}

// LL_VK_ARENA_POISON=1: a range that is given up is filled with 0xDEADBEEF in command order, so a draw or packet that still reads it shows garbage (not a stale picture)
bool arena_poison()
{
    static const bool on = []() { const char* e = getenv("LL_VK_ARENA_POISON"); return e && e[0] == '1'; }();
    return on;
}

// gives an arena range back (reusable once the GPU has finished the frames in flight)
void retire_range(ArenaRef& r)
{
    if (arena_poison())
    {
        Renderer::instance().fillBuffer(r.mBuffer, r.mOffset, (r.mSize + 3) & ~(VkDeviceSize)3, 0xDEADBEEFu);
        // the fill must be recorded before the range can be handed out again (a host range is rewritten by the CPU): name 0 makes the frame-end flush run
        sPendingBufs.push_back(0);
    }
    Context::instance().arenaFree(r);
}

// gives the storage of a buffer back: its arena range or its own Vulkan buffer
void release_storage(Buf& b)
{
    Context& ctx = Context::instance();
    if (b.mArena.valid())
    {
        retire_range(b.mArena);
        b.mVk = Buffer();
        ++b.mEpoch;
    }
    else if (b.mVk.valid())
    {
        ctx.destroyBuffer(b.mVk);
        ++b.mEpoch;
    }
}

// takes a range of the arena for a device-path buffer of 'size' bytes; true: b.mVk and b.mArena describe it
// kind Host: CPU-written memory (b.mVk.mMapped points at the range), for the small buffers
bool acquire_arena_storage(Buf& b, U64 size, ArenaKind kind)
{
    Context& ctx = Context::instance();
    ArenaRef ref;
    if (!ctx.arenaAlloc((VkDeviceSize)size, 16, kind, ref))
    {
        return false;
    }
    b.mArena = ref;
    b.mVk = Buffer();
    b.mVk.mBuffer = ref.mBuffer;
    b.mVk.mSize = (VkDeviceSize)size;
    b.mVk.mAddress = ref.mAddress;
    b.mVk.mMapped = ref.mMapped;
    ++b.mEpoch;
    return true;
}

// small vertex / index buffers (not uniform or pixel buffers) are ranges of host-written arena blocks; LL_VK_ARENA_HOST=0 gives each its own VkBuffer again
bool arena_host_enabled()
{
    static const bool on = []() { const char* e = getenv("LL_VK_ARENA_HOST"); return arena_enabled() && !(e && e[0] == '0'); }();
    return on;
}

U64 device_buffer_min_bytes()
{
    static const U64 v = []() {
        const char* e = getenv("LL_VK_DEVICE_BUF_MIN");
        return e ? (U64)atoll(e) : (U64)(128 * 1024);
    }();
    return v;
}

// <FS> the vertex and uniform buffers of a draw are marked as they are found (build_vertex_input, sync_uniform_blocks); only the element buffer is left, and only
// when the draw has one (glDrawArrays passes 0: it used to look up buffer 0 for nothing)
void mark_used(const std::vector<GLuint>& buffers, GLuint element)
{
    (void)buffers;
    if (element)
    {
        auto eit = sGL.mBuffers.find(element);
        if (eit != sGL.mBuffers.end()) eit->second.mLastUseFrame = Context::instance().getFrameNumber();
    }
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

// buffer traffic per VkPhases interval (arena baseline: how hot would the allocator and the retire list be)
struct BufTraffic
{
    U32 mSpecDevSame = 0, mSpecDevNew = 0, mSpecHost = 0;      // glBufferData: device path same size (kept), device path new allocation, host-visible path
    U64 mSpecDevNewBytes = 0, mSpecHostBytes = 0;
    U32 mSubDev = 0, mSubHost = 0;                             // glBufferSubData / write_buffer calls
    U64 mSubDevBytes = 0, mSubHostBytes = 0;
    U32 mFlushDraw = 0, mFlushFrameEnd = 0;                    // forced flushes of device-buffer writes (each ends the render pass and adds two full barriers)
    U64 mFlushBuffers = 0;                                     // buffers with pending writes, summed over the flushes
};
BufTraffic sBufTraffic;

void note_flush(bool frame_end, size_t buffers)
{
    if (frame_end) ++sBufTraffic.mFlushFrameEnd; else ++sBufTraffic.mFlushDraw;
    sBufTraffic.mFlushBuffers += buffers;
}

// LL_VK_PHASES: bytes written per buffer name in the interval, to find who writes every frame
struct WriterStat { U32 mWrites = 0; U64 mBytes = 0; U32 mFrames = 0; U64 mLastFrame = ~0ull; };      // mFrames: in how many different frames it was written
std::unordered_map<GLuint, WriterStat> sWriters;
const bool sWriterStatsOn = getenv("LL_VK_PHASES") != nullptr;

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
    if (sWriterStatsOn)
    {
        WriterStat& w = sWriters[name];
        ++w.mWrites;
        w.mBytes += size;
        const U64 wf = Context::instance().getFrameNumber();
        if (w.mLastFrame != wf)
        {
            w.mLastFrame = wf;
            ++w.mFrames;
        }
    }
    if (b.mDevicePath)
    {
        ++sBufTraffic.mSubDev;
        sBufTraffic.mSubDevBytes += size;
        ++b.mVersion;
        if (b.mVk.valid() && data && size && offset + size <= b.mVk.mSize)
        {
            Renderer::instance().updateBuffer(b.mVk.mBuffer, b.mArena.mOffset + offset, size, data);
            if (!b.mPending)
            {
                b.mPending = true;
                sPendingBufs.push_back(name);
            }
        }
        return;
    }
    ++sBufTraffic.mSubHost;
    sBufTraffic.mSubHostBytes += size;
    ++b.mVersion;
    Context& ctx = Context::instance();
    const U64 frame = ctx.getFrameNumber();
    const bool in_flight = b.mLastUseFrame != ~0ull && frame < b.mLastUseFrame + Context::frameCount() + 1;
    if (in_flight && b.mVk.valid())
    {
        const auto t0 = std::chrono::steady_clock::now();
        Buffer fresh;
        ArenaRef fresh_range;
        if (b.mArena.valid())
        {
            // a new range of the arena instead of a new VkBuffer; the old range goes back once the frames in flight are done
            if (ctx.arenaAlloc(b.mSize, 16, ArenaKind::Host, fresh_range))
            {
                const size_t keep = (size_t)std::min<U64>(b.mSize, b.mVk.mSize);
                if (!whole)
                {
                    if (b.mShadow.size() >= keep)
                    {
                        memcpy(fresh_range.mMapped, b.mShadow.data(), keep);
                    }
                    else
                    {
                        memcpy(fresh_range.mMapped, b.mVk.mMapped, keep);
                        ++sRenameStats.mShadowlessCopies;
                    }
                }
                retire_range(b.mArena);
                b.mArena = fresh_range;
                b.mVk = Buffer();
                b.mVk.mBuffer = fresh_range.mBuffer;
                b.mVk.mSize = (VkDeviceSize)b.mSize;
                b.mVk.mAddress = fresh_range.mAddress;
                b.mVk.mMapped = fresh_range.mMapped;
                ++b.mMoves;
                b.mLastUseFrame = ~0ull;
                ++sRenameStats.mCount;
                sRenameStats.mBytes += keep;
            }
        }
        else if (ctx.createBuffer(b.mSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
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

static U64 sPacketTextureGeneration = 0;     // <FS> bumped when a texture entry goes: the packet records' lookup cache starts over

void on_glDeleteTextures(GLsizei n, const GLuint* textures)
{
    for (GLsizei i = 0; i < n; ++i)
    {
        auto it = sGL.mTextures.find(textures[i]);
        if (it != sGL.mTextures.end())
        {
            destroy_tex_images(it->second);
            tex_forget(it->first);
            sGL.mTextures.erase(it);
            ++sTargetEpoch;
            ++sPacketTextureGeneration;
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
        ++sTargetEpoch;
        if (sCtx.mDrawFbo == framebuffers[i]) sCtx.mDrawFbo = 0;
        if (sCtx.mReadFbo == framebuffers[i]) sCtx.mReadFbo = 0;
    }
}

void on_glBindFramebuffer(GLenum target, GLuint framebuffer)
{
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) sCtx.mDrawFbo = framebuffer;
    ++sTargetEpoch;
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
    ++sTargetEpoch;
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
    ++sTargetEpoch;
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
    ++sTargetEpoch;
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

static U64 sPacketBufferGeneration = 0;      // <FS> bumped when a buffer entry goes: the packet records' lookup cache starts over

void on_glDeleteBuffers(GLsizei n, const GLuint* buffers)
{
    Context& ctx = Context::instance();
    for (GLsizei i = 0; i < n; ++i)
    {
        auto it = sGL.mBuffers.find(buffers[i]);
        if (it != sGL.mBuffers.end())
        {
            release_storage(it->second);
            buf_forget(it->first);
            sGL.mBuffers.erase(it);
            ++sPacketBufferGeneration;
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

void on_bufferData(GLuint name, GLenum target, GLsizeiptr size, const void* data, GLenum usage);
void on_bufferSubData(GLuint name, GLintptr offset, GLsizeiptr size, const void* data);

void on_glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage)
{
    on_bufferData(buffer_for_target(target), target, size, data, usage);
}

// <FS> N1b/N4: the buffer is named by the record, not by what is bound (LLVertexBuffer's typed uploads)
void on_bufferData(GLuint name, GLenum target, GLsizeiptr size, const void* data, GLenum usage)
{
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
        ++sBufTraffic.mSpecDevSame;
        ++b.mVersion;
        if (data)
        {
            Renderer::instance().updateBuffer(b.mVk.mBuffer, b.mArena.mOffset, (VkDeviceSize)size, data);
            if (!b.mPending)
            {
                b.mPending = true;
                sPendingBufs.push_back(name);
            }
        }
        return;
    }
    release_storage(b);
    b.mSize = (U64)size;
    b.mLastUseFrame = ~0ull;
    b.mShadow.clear();
    b.mDevicePath = want_device;
    ++b.mVersion;
    if (want_device)
    {
        ++sBufTraffic.mSpecDevNew;
        sBufTraffic.mSpecDevNewBytes += (U64)size;
        const bool created = arena_enabled() ? acquire_arena_storage(b, (U64)size, ArenaKind::Device)
            : ctx.createBuffer((VkDeviceSize)size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               Context::Memory::Device, b.mVk);
        if (created && data)
        {
            Renderer::instance().updateBuffer(b.mVk.mBuffer, b.mArena.mOffset, (VkDeviceSize)size, data);
            if (!b.mPending)
            {
                b.mPending = true;
                sPendingBufs.push_back(name);
            }
        }
        return;
    }
    const bool geometry = target == GL_ARRAY_BUFFER || target == GL_ELEMENT_ARRAY_BUFFER;
    const bool stored = size != 0 && (geometry && arena_host_enabled() ? acquire_arena_storage(b, (U64)size, ArenaKind::Host)
        : ctx.createBuffer((VkDeviceSize)size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                           Context::Memory::UploadDevice, b.mVk));
    if (!stored)
    {
        return;
    }
    ++sBufTraffic.mSpecHost;
    sBufTraffic.mSpecHostBytes += (U64)size;
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

// the vertex buffer pool hands the name to another owner (or takes it back) without glBufferData: what was known about its contents is stale
void on_bufferRecycle(GLuint name)
{
    auto it = sGL.mBuffers.find(name);
    if (it != sGL.mBuffers.end())
    {
        ++it->second.mEpoch;
    }
}

bool getBufferInfo(unsigned int name, BufferInfo& out)
{
    out = BufferInfo();
    auto it = sGL.mBuffers.find(name);
    if (it == sGL.mBuffers.end())
    {
        return false;
    }
    const Buf& b = it->second;
    out.mAddress = b.mArena.valid() ? (unsigned long long)b.mVk.mAddress : 0;
    out.mSize = b.mSize;
    out.mEpoch = b.mEpoch;
    out.mVersion = b.mVersion;
    out.mMoves = b.mMoves;
    out.mDevice = b.mDevicePath;
    return b.mVk.valid();
}

void markBufferUsed(unsigned int name)
{
    auto it = sGL.mBuffers.find(name);
    if (it != sGL.mBuffers.end())
    {
        it->second.mLastUseFrame = Context::instance().getFrameNumber();
    }
}

void on_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data)
{
    on_bufferSubData(buffer_for_target(target), offset, size, data);
}

void on_bufferSubData(GLuint name, GLintptr offset, GLsizeiptr size, const void* data)
{
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

namespace
{
bool probe_on();
void probe_note(GLuint vb_name, GLuint ib_name, const Buf& vb, const Buf& ib, U32 first, U32 count, U32 max_vertex);
}

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
    if (probe_on() && type == GL_UNSIGNED_SHORT)
    {
        // BLAS probe: positions are attribute 0 (vec4 array at the start of the vertex buffer); rigged draws (weights, joints enabled) and the UI are left out
        const VaoAttrib& pos = vao.mAttrib[0];
        auto vit = sGL.mBuffers.find(pos.mBuffer);
        if (pos.mEnabled && pos.mBuffer && vit != sGL.mBuffers.end() && pos.mType == GL_FLOAT && pos.mSize >= 3 && pos.mOffset == 0 && (pos.mStride == 16 || (pos.mStride == 0 && pos.mSize == 4))
            && !vao.mAttrib[10].mEnabled && !vao.mAttrib[12].mEnabled)
        {
            const std::string& program_name = r.getProgram()->getName();
            if (program_name.find("UI") == std::string::npos && program_name.find("HUD") == std::string::npos)
            {
                probe_note(pos.mBuffer, vao.mElementBuffer, vit->second, eit->second, (U32)((U64)indices / 2), (U32)count, (U32)end);
            }
        }
    }
    ensure_buffers_current(used, vao.mElementBuffer);
    LLVkMirror::PhaseTimer apply_phase(LLVkMirror::PH_D_APPLY);
    r.setVertexInput(vi);
    r.setTopology(topology(mode));
    r.setIndexBuffer(eit->second.mVk.mBuffer, eit->second.mArena.mOffset, itype);
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

// <FS> Render packets with textures. The buffers and textures of the draws, found through direct-mapped caches of the entries of sGL.mBuffers and sGL.mTextures
// (their nodes keep their place until erased, and erasing starts the cache over).
static Buf* packet_buffer(GLuint name)
{
    static Buf* cache_buf[1024];
    static GLuint cache_name[1024];
    static U64 cache_generation = ~0ull;
    if (cache_generation != sPacketBufferGeneration)
    {
        memset(cache_buf, 0, sizeof(cache_buf));
        memset(cache_name, 0, sizeof(cache_name));
        cache_generation = sPacketBufferGeneration;
    }
    const U32 slot = (name * 2654435761u) >> 22;
    if (cache_name[slot] == name && cache_buf[slot])
    {
        return cache_buf[slot];
    }
    auto it = sGL.mBuffers.find(name);
    if (it == sGL.mBuffers.end())
    {
        return nullptr;
    }
    cache_name[slot] = name;
    cache_buf[slot] = &it->second;
    return &it->second;
}

static Tex* packet_texture(GLuint name)
{
    static Tex* cache_tex[1024];
    static GLuint cache_name[1024];
    static U64 cache_generation = ~0ull;
    if (cache_generation != sPacketTextureGeneration)
    {
        memset(cache_tex, 0, sizeof(cache_tex));
        memset(cache_name, 0, sizeof(cache_name));
        cache_generation = sPacketTextureGeneration;
    }
    const U32 slot = (name * 2654435761u) >> 22;
    if (cache_name[slot] == name && cache_tex[slot])
    {
        return cache_tex[slot];
    }
    auto it = sGL.mTextures.find(name);
    if (it == sGL.mTextures.end())
    {
        return nullptr;
    }
    cache_name[slot] = name;
    cache_tex[slot] = &it->second;
    return &it->second;
}

// the slot of a texture in the frame's bindless table; 0 (white) for one that has no usable image, which is what the bound-texture path samples then too
static U32 packet_texture_slot(Renderer& r, GLuint name)
{
    Tex* t = packet_texture(name);
    if (!t || !t->mImage.valid() || t->mImage.mViewType != VK_IMAGE_VIEW_TYPE_2D)
    {
        return 0;
    }
    if (t->mImage.mLayout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
        const char* prev_cause = r.setBarrierCause("packet texture");
        r.ensureLayout(t->mImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        r.setBarrierCause(prev_cause);
    }
    const U32 epoch = r.bindlessEpoch();
    if (t->mBindlessEpoch == epoch && t->mBindlessView == t->mImage.mView)
    {
        return t->mBindlessSlot;
    }
    const U32 slot = r.bindlessSlot(&t->mImage, texture_sampler(r, *t));
    t->mBindlessEpoch = epoch;
    t->mBindlessView = t->mImage.mView;
    t->mBindlessSlot = slot;
    return slot;
}

//-----------------------------------------------------------------------------------------------------
// Arena phase 3: BLAS probe (LL_VK_BLAS_PROBE=1, needs LL_VK_RT=1 and the arena). One bottom-level acceleration structure per (vertex buffer, index buffer) pair, one geometry per
// distinct drawn index range, built or refitted at the end of the frame the buffer was drawn in. Nothing traces: the probe only measures what per-buffer structures cost
// (count, memory, build and refit rates, GPU time of the block, CPU time) while walking around and during a teleport. The positions are the vec4 array at the start of the
// vertex buffer (stride 16), the indices 16 bit.
//-----------------------------------------------------------------------------------------------------
namespace
{

bool probe_on()
{
    static const bool on = []() { const char* e = getenv("LL_VK_BLAS_PROBE"); return e && e[0] == '1'; }();
    return on;
}

U64 probe_env(const char* name, U64 fallback)
{
    const char* e = getenv(name);
    return e ? (U64)atoll(e) : fallback;
}

struct ProbeGeom
{
    U32 mFirst, mCount, mMaxVertex;
    bool operator<(const ProbeGeom& o) const { return mFirst != o.mFirst ? mFirst < o.mFirst : mCount < o.mCount; }
    bool operator==(const ProbeGeom& o) const { return mFirst == o.mFirst && mCount == o.mCount && mMaxVertex == o.mMaxVertex; }
};

struct ProbeBlas
{
    GLuint mVb = 0, mIb = 0;
    U64 mVbEpoch = 0, mIbEpoch = 0;                 // the storage the geometry list and the structure belong to
    U64 mGeomVersion = ~0ull;                       // vertex buffer version the geometry list was collected under (a rewrite starts the list over)
    U64 mVbVersion = ~0ull, mIbVersion = ~0ull;     // contents at the last build
    std::vector<ProbeGeom> mGeoms;                  // drawn ranges, this version of the contents
    std::vector<ProbeGeom> mBuilt;                  // what the structure was built over (sorted)
    VkAccelerationStructureKHR mAs = VK_NULL_HANDLE;
    Buffer mStorage;
    U64 mPrims = 0;
    bool mQueued = false;
};

struct ProbeStats
{
    U64 mBuilds = 0, mRefits = 0, mGeomChanged = 0, mEvicted = 0, mPrimsBuilt = 0, mPrimsRefit = 0, mDeferred = 0, mOverBudget = 0, mRuns = 0, mFailed = 0;
    U64 mBuildBytes = 0;
    double mCpuMs = 0, mCpuMsMax = 0;
};

std::unordered_map<U64, ProbeBlas> sProbe;
std::vector<U64> sProbeTouched;         // pairs drawn since the last run
ProbeStats sProbeStats;
Buffer sProbeScratch;
U64 sProbeStorageBytes = 0;             // live structure storage
U64 sProbeRunCount = 0;

void probe_drop(ProbeBlas& p)
{
    if (p.mAs != VK_NULL_HANDLE)
    {
        Context& ctx = Context::instance();
        VkDevice dev = ctx.getDevice();
        VkAccelerationStructureKHR as = p.mAs;
        ctx.destroyLater([=]() { vkDestroyAccelerationStructureKHR(dev, as, nullptr); });     // before its storage: deferred calls run in order
        sProbeStorageBytes -= std::min<U64>(sProbeStorageBytes, p.mStorage.mSize);
        ctx.destroyBuffer(p.mStorage);
    }
    p.mAs = VK_NULL_HANDLE;
    p.mStorage = Buffer();
    p.mBuilt.clear();
    p.mVbVersion = p.mIbVersion = ~0ull;
    p.mPrims = 0;
}

// one drawn index range of a vertex / index buffer pair (called by the packet handlers for the opaque colour passes)
void probe_note(GLuint vb_name, GLuint ib_name, const Buf& vb, const Buf& ib, U32 first, U32 count, U32 max_vertex)
{
    if (!vb.mArena.valid() || !ib.mArena.valid() || count < 3)
    {
        return;
    }
    const U64 key = ((U64)vb_name << 32) | ib_name;
    ProbeBlas& p = sProbe[key];
    if (p.mVb == 0)
    {
        p.mVb = vb_name;
        p.mIb = ib_name;
        p.mVbEpoch = vb.mEpoch;
        p.mIbEpoch = ib.mEpoch;
    }
    else if (p.mVbEpoch != vb.mEpoch || p.mIbEpoch != ib.mEpoch)
    {
        probe_drop(p);                  // the name holds something else now
        p.mGeoms.clear();
        p.mVbEpoch = vb.mEpoch;
        p.mIbEpoch = ib.mEpoch;
        p.mGeomVersion = ~0ull;
        ++sProbeStats.mEvicted;
    }
    if (p.mGeomVersion != vb.mVersion)
    {
        p.mGeoms.clear();               // rewritten: the old ranges may not mean anything any more
        p.mGeomVersion = vb.mVersion;
    }
    bool found = false;
    for (const ProbeGeom& g : p.mGeoms)
    {
        if (g.mFirst == first && g.mCount == count)
        {
            found = true;
            break;
        }
    }
    if (!found && p.mGeoms.size() < 256)
    {
        p.mGeoms.push_back({ first, count, max_vertex });
    }
    if (!p.mQueued)
    {
        p.mQueued = true;
        sProbeTouched.push_back(key);
    }
}

struct ProbeJob
{
    ProbeBlas* mBlas = nullptr;
    bool mUpdate = false;
    std::vector<ProbeGeom> mSet;
    std::vector<VkAccelerationStructureGeometryKHR> mGeometries;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> mRanges;
    std::vector<U32> mCounts;
    VkAccelerationStructureBuildGeometryInfoKHR mInfo = {};
    VkDeviceSize mScratchSize = 0, mScratchOffset = 0;
    VkAccelerationStructureKHR mNewAs = VK_NULL_HANDLE;
    Buffer mNewStorage;
    U64 mPrims = 0;
    U64 mVbVersion = 0, mIbVersion = 0;
};

// builds and refits what the frame drew, outside any render pass (the frame end)
void probe_run()
{
    if (!probe_on() || sProbeTouched.empty())
    {
        return;
    }
    Context& ctx = Context::instance();
    const Capabilities& caps = ctx.getCaps();
    if (!caps.mRt.mEnabled || !caps.mRt.mUsable || !vkCmdBuildAccelerationStructuresKHR || !vkCreateAccelerationStructureKHR)
    {
        static bool said = false;
        if (!said)
        {
            said = true;
            LL_WARNS("VkGL") << "LL_VK_BLAS_PROBE: the device has no ray tracing enabled (LL_VK_RT=1)" << LL_ENDL;
        }
        for (U64 k : sProbeTouched) sProbe[k].mQueued = false;
        sProbeTouched.clear();
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    VkDevice dev = ctx.getDevice();
    static const U64 max_jobs = probe_env("LL_VK_BLAS_PROBE_MAX_BUILDS", 64);
    static const U64 max_storage = probe_env("LL_VK_BLAS_PROBE_MB", 2048) << 20;
    const VkDeviceSize align = std::max<VkDeviceSize>(caps.mRt.mScratchAlignment, 128);
    const U64 frame_now = ctx.getFrameNumber();

    // forget structures over names that are gone or hold something else (now and then: a name that is drawn again is checked when it is noted)
    if (++sProbeRunCount % 300 == 0)
    {
        for (auto it = sProbe.begin(); it != sProbe.end();)
        {
            auto vit = sGL.mBuffers.find(it->second.mVb);
            auto iit = sGL.mBuffers.find(it->second.mIb);
            if (vit == sGL.mBuffers.end() || iit == sGL.mBuffers.end() || vit->second.mEpoch != it->second.mVbEpoch || iit->second.mEpoch != it->second.mIbEpoch)
            {
                probe_drop(it->second);
                if (it->second.mQueued)
                {
                    ++it;           // still in the touched list; the next sweep takes it
                    continue;
                }
                it = sProbe.erase(it);
                ++sProbeStats.mEvicted;
            }
            else
            {
                ++it;
            }
        }
    }

    std::vector<ProbeJob> jobs;
    jobs.reserve((size_t)max_jobs);
    U64 scratch_total = 0;
    for (U64 key : sProbeTouched)
    {
        auto pit = sProbe.find(key);
        if (pit == sProbe.end())
        {
            continue;
        }
        ProbeBlas& p = pit->second;
        p.mQueued = false;
        auto vit = sGL.mBuffers.find(p.mVb);
        auto iit = sGL.mBuffers.find(p.mIb);
        if (vit == sGL.mBuffers.end() || iit == sGL.mBuffers.end() || vit->second.mEpoch != p.mVbEpoch || iit->second.mEpoch != p.mIbEpoch
            || !vit->second.mVk.mAddress || !iit->second.mVk.mAddress || p.mGeoms.empty())
        {
            continue;
        }
        Buf& vb = vit->second;
        Buf& ib = iit->second;
        std::vector<ProbeGeom> set = p.mGeoms;
        std::sort(set.begin(), set.end());
        const bool same_set = p.mAs != VK_NULL_HANDLE && set == p.mBuilt;
        if (same_set && p.mVbVersion == vb.mVersion && p.mIbVersion == ib.mVersion)
        {
            continue;               // nothing changed
        }
        if (jobs.size() >= max_jobs)
        {
            ++sProbeStats.mDeferred;
            continue;
        }
        ProbeJob job;
        job.mBlas = &p;
        job.mUpdate = same_set;
        job.mVbVersion = vb.mVersion;
        job.mIbVersion = ib.mVersion;
        for (const ProbeGeom& g : set)
        {
            if (((U64)g.mFirst + g.mCount) * 2 > ib.mVk.mSize || vb.mVk.mSize < 16)
            {
                continue;           // outside the data (a stale range): not part of the structure
            }
            const U32 max_vertex = std::min<U32>(g.mMaxVertex, (U32)((vb.mVk.mSize - 12) / 16));
            VkAccelerationStructureGeometryKHR geo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
            geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            geo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
            VkAccelerationStructureGeometryTrianglesDataKHR& t = geo.geometry.triangles;
            t.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            t.vertexData.deviceAddress = vb.mVk.mAddress;
            t.vertexStride = 16;
            t.maxVertex = max_vertex;
            t.indexType = VK_INDEX_TYPE_UINT16;
            t.indexData.deviceAddress = ib.mVk.mAddress + 2ull * g.mFirst;
            job.mGeometries.push_back(geo);
            VkAccelerationStructureBuildRangeInfoKHR range = {};
            range.primitiveCount = g.mCount / 3;
            job.mRanges.push_back(range);
            job.mCounts.push_back(range.primitiveCount);
            job.mPrims += range.primitiveCount;
            job.mSet.push_back(g);
        }
        if (job.mGeometries.empty())
        {
            continue;
        }
        job.mInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        job.mInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        job.mInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        job.mInfo.mode = job.mUpdate ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        job.mInfo.geometryCount = (U32)job.mGeometries.size();
        job.mInfo.pGeometries = job.mGeometries.data();
        VkAccelerationStructureBuildSizesInfoKHR sizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
        vkGetAccelerationStructureBuildSizesKHR(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &job.mInfo, job.mCounts.data(), &sizes);
        if (job.mUpdate)
        {
            job.mInfo.srcAccelerationStructure = p.mAs;
            job.mInfo.dstAccelerationStructure = p.mAs;
            job.mScratchSize = sizes.updateScratchSize;
        }
        else
        {
            if (sProbeStorageBytes + sizes.accelerationStructureSize > max_storage)
            {
                ++sProbeStats.mOverBudget;
                continue;
            }
            if (!ctx.createBuffer(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, Context::Memory::Device, job.mNewStorage))
            {
                ++sProbeStats.mFailed;
                continue;
            }
            VkAccelerationStructureCreateInfoKHR ci = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
            ci.buffer = job.mNewStorage.mBuffer;
            ci.size = sizes.accelerationStructureSize;
            ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            if (vkCreateAccelerationStructureKHR(dev, &ci, nullptr, &job.mNewAs) != VK_SUCCESS)
            {
                ctx.destroyBuffer(job.mNewStorage);
                ++sProbeStats.mFailed;
                continue;
            }
            sProbeStorageBytes += job.mNewStorage.mSize;
            job.mInfo.dstAccelerationStructure = job.mNewAs;
            job.mScratchSize = sizes.buildScratchSize;
        }
        job.mScratchOffset = (scratch_total + align - 1) / align * align;
        scratch_total = job.mScratchOffset + job.mScratchSize;
        vb.mLastUseFrame = frame_now;       // the GPU reads these buffers this frame: a CPU rewrite of a host-written range must rename first
        ib.mLastUseFrame = frame_now;
        jobs.push_back(std::move(job));
    }
    sProbeTouched.clear();
    if (jobs.empty())
    {
        return;
    }

    auto undo_jobs = [&]() {
        for (ProbeJob& j : jobs)
        {
            if (j.mNewAs != VK_NULL_HANDLE)
            {
                VkAccelerationStructureKHR as = j.mNewAs;
                ctx.destroyLater([=]() { vkDestroyAccelerationStructureKHR(dev, as, nullptr); });
                sProbeStorageBytes -= std::min<U64>(sProbeStorageBytes, j.mNewStorage.mSize);
                ctx.destroyBuffer(j.mNewStorage);
            }
        }
    };

    // one scratch buffer for all builds of the frame (the barrier at the start of the block orders it after the previous frame's builds)
    if (!sProbeScratch.valid() || sProbeScratch.mSize < scratch_total + align)
    {
        ctx.destroyBuffer(sProbeScratch);
        if (!ctx.createBuffer((scratch_total + align) * 3 / 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Context::Memory::Device, sProbeScratch))
        {
            undo_jobs();
            ++sProbeStats.mFailed;
            return;
        }
    }
    const VkDeviceAddress scratch_base = (sProbeScratch.mAddress + align - 1) / align * align;
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos;
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> ranges;
    for (ProbeJob& j : jobs)
    {
        j.mInfo.scratchData.deviceAddress = scratch_base + j.mScratchOffset;
        infos.push_back(j.mInfo);
        ranges.push_back(j.mRanges.data());
    }
    const bool recorded = Renderer::instance().recordExternal({}, [&](VkCommandBuffer cmd) {
        ctx.gpuStamp(Context::stampTag("blas probe"));
        VkMemoryBarrier2 before = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        before.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        before.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        before.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        before.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &before;
        vkCmdPipelineBarrier2(cmd, &dep);
        vkCmdBuildAccelerationStructuresKHR(cmd, (U32)infos.size(), infos.data(), ranges.data());
        VkMemoryBarrier2 after = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        after.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        after.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        after.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        after.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        dep.pMemoryBarriers = &after;
        vkCmdPipelineBarrier2(cmd, &dep);
    });
    if (!recorded)
    {
        undo_jobs();            // no frame is being recorded; the ranges are noted again when they are drawn
        return;
    }
    for (ProbeJob& j : jobs)
    {
        ProbeBlas& p = *j.mBlas;
        if (j.mUpdate)
        {
            ++sProbeStats.mRefits;
            sProbeStats.mPrimsRefit += j.mPrims;
        }
        else
        {
            if (p.mAs != VK_NULL_HANDLE)
            {
                ++sProbeStats.mGeomChanged;
                probe_drop(p);
            }
            p.mAs = j.mNewAs;
            p.mStorage = j.mNewStorage;
            ++sProbeStats.mBuilds;
            sProbeStats.mPrimsBuilt += j.mPrims;
            sProbeStats.mBuildBytes += j.mNewStorage.mSize;
        }
        p.mBuilt = j.mSet;
        p.mVbVersion = j.mVbVersion;
        p.mIbVersion = j.mIbVersion;
        p.mPrims = j.mPrims;
    }
    ++sProbeStats.mRuns;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    sProbeStats.mCpuMs += ms;
    sProbeStats.mCpuMsMax = std::max(sProbeStats.mCpuMsMax, ms);
}

std::string probe_report(double frames)
{
    U32 live = 0;
    U64 prims = 0;
    for (const auto& kv : sProbe)
    {
        if (kv.second.mAs != VK_NULL_HANDLE)
        {
            ++live;
            prims += kv.second.mPrims;
        }
    }
    const ProbeStats& s = sProbeStats;
    char buf[900];
    snprintf(buf, sizeof(buf),
             "blas probe: %u live structures over %zu buffer pairs (%.0f MB storage, %.1f MB scratch, %.2f M triangles); per frame: %.2f builds (%.0f tri), %.2f refits (%.0f tri), geometry-set rebuilds %.3f, "
             "evicted %.3f, deferred by the per-frame cap %.3f, over the memory cap %.3f, failed %.3f; blocks recorded %.2f/frame, CPU %.3f ms/frame (worst block %.2f ms)",
             live, sProbe.size(), (double)sProbeStorageBytes / 1048576.0, (double)sProbeScratch.mSize / 1048576.0, (double)prims / 1e6,
             (double)s.mBuilds / frames, (double)s.mPrimsBuilt / frames, (double)s.mRefits / frames, (double)s.mPrimsRefit / frames, (double)s.mGeomChanged / frames,
             (double)s.mEvicted / frames, (double)s.mDeferred / frames, (double)s.mOverBudget / frames, (double)s.mFailed / frames, (double)s.mRuns / frames, s.mCpuMs / frames, s.mCpuMsMax);
    return buf;
}

} // anonymous namespace

// ray query shadows: a caster draw of a shadow pass, for the scene the lighting traces (llvkrt.cpp)
static void note_rt_caster(U32 pass, U32 vb, U32 ib, U32 first, U32 count, U32 matrix_id, U32 kind, const F32* model, const Rt::AlphaInfo* alpha = nullptr)
{
    Rt::Caster c;
    c.mVb = vb;
    c.mIb = ib;
    c.mFirst = first;
    c.mCount = count;
    c.mMatrixId = matrix_id;
    c.mKind = kind;
    c.mModel = model;
    c.mAlpha = alpha;
    c.mPass = pass;
    Rt::noteCaster(c);
}

// the worker side of a record of alpha masked casters (shadowAlphaPullV.glsl / shadowAlphaPullF.glsl): the entries also name their textures
static void on_shadow_alpha_packets(const LLVkMirror::ShadowPacketsHeader& h, const U8* blob)
{
    struct AlphaGpu { U32 mPositions[2]; U32 mIndices[2]; U32 mTexcoords[2]; U32 mColors[2]; U32 mFirst, mCount, mModel, mFlags; U32 mSlots[2]; F32 mCutoff; U32 mPad; U32 mWeights[2]; U32 mPalette[2]; };
    static_assert(sizeof(AlphaGpu) == 80, "alpha packet entry");
    static thread_local VertexInput vi_cache;
    static thread_local std::vector<GLuint> used_cache, packet_used;
    static thread_local std::vector<U32> counts;
    VertexInput& vi = vi_cache;
    vi.mBindings.clear();
    vi.mAttributes.clear();
    used_cache.clear();
    packet_used.clear();

    Renderer& r = Renderer::instance();
    Program* program = r.getProgram();
    if (h.mEntries == 0 || !program || !program->isValid() || !program->hasUniform("ll_packet_table") || !r.bindlessAvailable())
    {
        ++sStats.mNoProgram;
        return;
    }
    const LLVkMirror::ShadowAlphaEntry* entries = (const LLVkMirror::ShadowAlphaEntry*)blob;
    const F32* matrices = (const F32*)(blob + sizeof(LLVkMirror::ShadowAlphaEntry) * h.mEntries);
    Renderer::ScratchAlloc table = r.allocVertexScratch(sizeof(AlphaGpu) * (size_t)h.mEntries);
    Renderer::ScratchAlloc models = r.allocVertexScratch(64 * (size_t)std::max<U32>(h.mMatrices, 1));
    if (!table.mPtr || !models.mPtr || !table.mAddress || !models.mAddress)
    {
        return;
    }
    const U64 frame_now = Context::instance().getFrameNumber();
    counts.assign(h.mEntries, 0);
    AlphaGpu* out = (AlphaGpu*)table.mPtr;
    Renderer::ScratchAlloc palettes;
    if (h.mPaletteFloats)
    {
        palettes = r.allocVertexScratch(4 * (size_t)h.mPaletteFloats);
        if (!palettes.mPtr || !palettes.mAddress)
        {
            return;
        }
        memcpy(palettes.mPtr, matrices + 16 * (size_t)h.mMatrices, 4 * (size_t)h.mPaletteFloats);
    }
    for (U32 i = 0; i < h.mEntries; ++i)
    {
        const LLVkMirror::ShadowAlphaEntry& e = entries[i];
        AlphaGpu& g = out[i];
        memset(&g, 0, sizeof(g));
        Buf* vb = packet_buffer(e.mVertexBuffer);
        Buf* ib = packet_buffer(e.mIndexBuffer);
        if (!vb || !ib || !vb->mVk.valid() || !ib->mVk.valid() || !vb->mVk.mAddress || !ib->mVk.mAddress)
        {
            continue;       // not created (yet): the draw is skipped, as a draw from an empty buffer is
        }
        if (Rt::active() && !(e.mFlags & LLVkMirror::ALPHA_RIGGED))
        {
            Rt::AlphaInfo alpha;
            alpha.mFlags = e.mFlags & 31u;      // fullbright, tree, one texture, texcoords, colours: the bits mean the same in the ray query shader
            alpha.mTexcoordOffset = e.mTexcoordOffset;
            alpha.mColorOffset = e.mColorOffset;
            memcpy(alpha.mTextures, e.mTextures, sizeof(alpha.mTextures));
            alpha.mCutoff = e.mCutoff;
            note_rt_caster(h.mPass, e.mVertexBuffer, e.mIndexBuffer, e.mFirstIndex, e.mIndexCount, e.mMatrixId, 1, e.mMatrix ? matrices + 16 * (size_t)e.mMatrix : nullptr, &alpha);
        }
        const U64 pa = (U64)vb->mVk.mAddress, ia = (U64)ib->mVk.mAddress;
        g.mPositions[0] = (U32)pa;
        g.mPositions[1] = (U32)(pa >> 32);
        g.mIndices[0] = (U32)ia;
        g.mIndices[1] = (U32)(ia >> 32);
        U32 flags = e.mFlags;
        if (flags & LLVkMirror::ALPHA_HAS_TEXCOORD)
        {
            const U64 a = pa + e.mTexcoordOffset;
            g.mTexcoords[0] = (U32)a;
            g.mTexcoords[1] = (U32)(a >> 32);
        }
        if (flags & LLVkMirror::ALPHA_HAS_COLOR)
        {
            const U64 a = pa + e.mColorOffset;
            g.mColors[0] = (U32)a;
            g.mColors[1] = (U32)(a >> 32);
        }
        g.mFirst = e.mFirstIndex;
        g.mCount = e.mIndexCount;
        g.mModel = e.mMatrix;
        g.mFlags = flags;
        if (flags & LLVkMirror::ALPHA_RIGGED)
        {
            if (!palettes.mAddress)
            {
                continue;
            }
            const U64 wa = pa + e.mWeightOffset, pal = (U64)palettes.mAddress + 4ull * e.mPaletteOffset;
            g.mWeights[0] = (U32)wa;
            g.mWeights[1] = (U32)(wa >> 32);
            g.mPalette[0] = (U32)pal;
            g.mPalette[1] = (U32)(pal >> 32);
            g.mCount = e.mJoints;       // the shader's joint count (the draw's index count is in the indirect command)
        }
        U32 slot[4] = {};
        for (U32 t = 0; t < 4; ++t)
        {
            if (e.mTextures[t])
            {
                slot[t] = packet_texture_slot(r, e.mTextures[t]);
            }
        }
        g.mSlots[0] = slot[0] | (slot[1] << 16);
        g.mSlots[1] = slot[2] | (slot[3] << 16);
        g.mCutoff = e.mCutoff;
        counts[i] = e.mIndexCount;
        vb->mLastUseFrame = frame_now;
        ib->mLastUseFrame = frame_now;
        if (packet_used.size() < 2 || packet_used[packet_used.size() - 2] != e.mVertexBuffer || packet_used.back() != e.mIndexBuffer)
        {
            packet_used.push_back(e.mVertexBuffer);
            packet_used.push_back(e.mIndexBuffer);
        }
    }
    memcpy(models.mPtr, matrices, 64 * (size_t)h.mMatrices);

    const U64 table_address = (U64)table.mAddress, models_address = (U64)models.mAddress;
    const S32 table_words[2] = { (S32)(U32)table_address, (S32)(U32)(table_address >> 32) };
    const S32 models_words[2] = { (S32)(U32)models_address, (S32)(U32)(models_address >> 32) };
    program->setInts("ll_packet_table", table_words, 2);
    program->setInts("ll_packet_models", models_words, 2);
    program->setMatrices("ll_view_proj", h.mViewProj, 4, 4, 1);
    program->setFloats("ll_target_width", &h.mTargetWidth, 1);

    if (!prepare_draw(vi, used_cache))
    {
        return;
    }
    ensure_buffers_current(packet_used, 0);
    r.setVertexInput(vi);
    r.setTopology(topology(GL_TRIANGLES));
    r.bindlessFlush();

    Renderer::ScratchAlloc commands;
    const auto& caps = Context::instance().getCaps();
    if (caps.mMultiDrawIndirect && caps.mDrawIndirectFirstInstance)
    {
        commands = r.allocVertexScratch(sizeof(VkDrawIndirectCommand) * (size_t)h.mEntries);
        if (commands.mPtr)
        {
            VkDrawIndirectCommand* c = (VkDrawIndirectCommand*)commands.mPtr;
            for (U32 i = 0; i < h.mEntries; ++i)
            {
                c[i] = { counts[i], 1u, 0u, i };
            }
        }
    }
    if (r.drawPackets(counts.data(), h.mEntries, commands.mPtr ? commands.mBuffer : VK_NULL_HANDLE, commands.mOffset))
    {
        if (sStatsEnabled)
        {
            sStats.mDone += h.mEntries;
        }
    }
    else
    {
        ++sStats.mRendererRefused;
    }
    sDraws += h.mEntries;
    sDrawsTotal += h.mEntries;
}

// the worker side of a record of deferred geometry draws (diffusePullV.glsl): the draw's matrices are in the record, its textures get slots in the bindless table
static void on_geometry_packets(const LLVkMirror::ShadowPacketsHeader& h, const U8* blob)
{
    struct GeoGpu
    {
        U32 mPositions[2]; U32 mNormals[2]; U32 mTexcoords[2]; U32 mColors[2]; U32 mIndices[2]; U32 mTangents[2]; U32 mTexcoords1[2]; U32 mTexcoords2[2];
        U32 mFirst, mModel, mFlags, mMapSlots;
        U32 mSlots[2];
        U32 mPad[2];
        F32 mSpecular[4];
        F32 mEnvIntensity, mCutoff, mBrightness, mPad2;
    };
    static_assert(sizeof(GeoGpu) == 128, "geometry packet entry");
    static thread_local VertexInput vi_cache;
    static thread_local std::vector<GLuint> used_cache, packet_used;
    static thread_local std::vector<U32> counts;
    VertexInput& vi = vi_cache;
    vi.mBindings.clear();
    vi.mAttributes.clear();
    used_cache.clear();
    packet_used.clear();

    Renderer& r = Renderer::instance();
    Program* program = r.getProgram();
    if (h.mEntries == 0 || !program || !program->isValid() || !program->hasUniform("ll_packet_table") || !r.bindlessAvailable())
    {
        ++sStats.mNoProgram;
        return;
    }
    const LLVkMirror::GeoEntry* entries = (const LLVkMirror::GeoEntry*)blob;
    const F32* matrices = (const F32*)(blob + sizeof(LLVkMirror::GeoEntry) * h.mEntries);
    Renderer::ScratchAlloc table = r.allocVertexScratch(sizeof(GeoGpu) * (size_t)h.mEntries);
    Renderer::ScratchAlloc models = r.allocVertexScratch(176 * (size_t)std::max<U32>(h.mMatrices, 1));
    if (!table.mPtr || !models.mPtr || !table.mAddress || !models.mAddress)
    {
        return;
    }
    const U64 frame_now = Context::instance().getFrameNumber();
    counts.assign(h.mEntries, 0);
    GeoGpu* out = (GeoGpu*)table.mPtr;
    for (U32 i = 0; i < h.mEntries; ++i)
    {
        const LLVkMirror::GeoEntry& e = entries[i];
        GeoGpu& g = out[i];
        memset(&g, 0, sizeof(g));
        Buf* vb = packet_buffer(e.mVertexBuffer);
        Buf* ib = packet_buffer(e.mIndexBuffer);
        if (!vb || !ib || !vb->mVk.valid() || !ib->mVk.valid() || !vb->mVk.mAddress || !ib->mVk.mAddress)
        {
            continue;       // not created (yet): the draw is skipped, as a draw from an empty buffer is
        }
        const U64 pa = (U64)vb->mVk.mAddress, ia = (U64)ib->mVk.mAddress;
        const U64 na = pa + e.mNormalOffset, ta = pa + e.mTexcoordOffset, ca = pa + e.mColorOffset;
        const U64 tga = pa + e.mTangentOffset, t1a = pa + e.mTexcoord1Offset, t2a = pa + e.mTexcoord2Offset;
        g.mPositions[0] = (U32)pa;
        g.mPositions[1] = (U32)(pa >> 32);
        g.mNormals[0] = (U32)na;
        g.mNormals[1] = (U32)(na >> 32);
        g.mTexcoords[0] = (U32)ta;
        g.mTexcoords[1] = (U32)(ta >> 32);
        g.mColors[0] = (U32)ca;
        g.mColors[1] = (U32)(ca >> 32);
        g.mIndices[0] = (U32)ia;
        g.mIndices[1] = (U32)(ia >> 32);
        g.mTangents[0] = (U32)tga;
        g.mTangents[1] = (U32)(tga >> 32);
        g.mTexcoords1[0] = (U32)t1a;
        g.mTexcoords1[1] = (U32)(t1a >> 32);
        g.mTexcoords2[0] = (U32)t2a;
        g.mTexcoords2[1] = (U32)(t2a >> 32);
        g.mFirst = e.mFirstIndex;
        g.mModel = e.mMatrix;
        g.mFlags = e.mFlags;
        if (probe_on())
        {
            // positions are the vec4 array at the start of the vertex buffer, the normals (or the next array) follow it
            const U32 next = e.mNormalOffset ? e.mNormalOffset : e.mTexcoordOffset;
            const U32 verts = next ? next / 16 : (U32)(vb->mVk.mSize / 16);
            probe_note(e.mVertexBuffer, e.mIndexBuffer, *vb, *ib, e.mFirstIndex, e.mIndexCount, verts ? verts - 1 : 0);
        }
        U32 slot[4] = {};
        for (U32 t = 0; t < 4; ++t)
        {
            if (e.mTextures[t])
            {
                slot[t] = packet_texture_slot(r, e.mTextures[t]);
            }
        }
        g.mSlots[0] = slot[0] | (slot[1] << 16);
        g.mSlots[1] = slot[2] | (slot[3] << 16);
        const U32 normal_slot = e.mNormalMap ? packet_texture_slot(r, e.mNormalMap) : 0;
        const U32 specular_slot = e.mSpecularMap ? packet_texture_slot(r, e.mSpecularMap) : 0;
        g.mMapSlots = normal_slot | (specular_slot << 16);
        memcpy(g.mSpecular, e.mSpecular, sizeof(g.mSpecular));
        g.mEnvIntensity = e.mEnvIntensity;
        g.mCutoff = e.mCutoff;
        g.mBrightness = e.mBrightness;
        counts[i] = e.mIndexCount;
        vb->mLastUseFrame = frame_now;
        ib->mLastUseFrame = frame_now;
        if (packet_used.size() < 2 || packet_used[packet_used.size() - 2] != e.mVertexBuffer || packet_used.back() != e.mIndexBuffer)
        {
            packet_used.push_back(e.mVertexBuffer);
            packet_used.push_back(e.mIndexBuffer);
        }
    }
    memcpy(models.mPtr, matrices, 176 * (size_t)h.mMatrices);

    const U64 table_address = (U64)table.mAddress, models_address = (U64)models.mAddress;
    const S32 table_words[2] = { (S32)(U32)table_address, (S32)(U32)(table_address >> 32) };
    const S32 models_words[2] = { (S32)(U32)models_address, (S32)(U32)(models_address >> 32) };
    program->setInts("ll_packet_table", table_words, 2);
    program->setInts("ll_packet_models", models_words, 2);

    if (!prepare_draw(vi, used_cache))
    {
        return;
    }
    ensure_buffers_current(packet_used, 0);
    r.setVertexInput(vi);
    r.setTopology(topology(GL_TRIANGLES));
    r.bindlessFlush();

    Renderer::ScratchAlloc commands;
    const auto& caps = Context::instance().getCaps();
    if (caps.mMultiDrawIndirect && caps.mDrawIndirectFirstInstance)
    {
        commands = r.allocVertexScratch(sizeof(VkDrawIndirectCommand) * (size_t)h.mEntries);
        if (commands.mPtr)
        {
            VkDrawIndirectCommand* c = (VkDrawIndirectCommand*)commands.mPtr;
            for (U32 i = 0; i < h.mEntries; ++i)
            {
                c[i] = { counts[i], 1u, 0u, i };
            }
        }
    }
    if (r.drawPackets(counts.data(), h.mEntries, commands.mPtr ? commands.mBuffer : VK_NULL_HANDLE, commands.mOffset))
    {
        if (sStatsEnabled)
        {
            sStats.mDone += h.mEntries;
        }
    }
    else
    {
        ++sStats.mRendererRefused;
    }
    sDraws += h.mEntries;
    sDrawsTotal += h.mEntries;
}

// the worker side of a record of rigged untextured casters (shadowSkinPullV.glsl)
static void on_shadow_skin_packets(const LLVkMirror::ShadowPacketsHeader& h, const U8* blob)
{
    struct SkinGpu { U32 mPositions[2]; U32 mIndices[2]; U32 mWeights[2]; U32 mPalette[2]; U32 mFirst, mJoints, mModel, mPad; };
    static_assert(sizeof(SkinGpu) == 48, "skin packet entry");
    static thread_local VertexInput vi_cache;
    static thread_local std::vector<GLuint> used_cache, packet_used;
    static thread_local std::vector<U32> counts;
    VertexInput& vi = vi_cache;
    vi.mBindings.clear();
    vi.mAttributes.clear();
    used_cache.clear();
    packet_used.clear();

    Renderer& r = Renderer::instance();
    Program* program = r.getProgram();
    if (h.mEntries == 0 || h.mPaletteFloats == 0 || !program || !program->isValid() || !program->hasUniform("ll_packet_table"))
    {
        ++sStats.mNoProgram;
        return;
    }
    const LLVkMirror::ShadowSkinEntry* entries = (const LLVkMirror::ShadowSkinEntry*)blob;
    const F32* matrices = (const F32*)(blob + sizeof(LLVkMirror::ShadowSkinEntry) * h.mEntries);
    Renderer::ScratchAlloc table = r.allocVertexScratch(sizeof(SkinGpu) * (size_t)h.mEntries);
    Renderer::ScratchAlloc models = r.allocVertexScratch(64 * (size_t)std::max<U32>(h.mMatrices, 1));
    Renderer::ScratchAlloc palettes = r.allocVertexScratch(4 * (size_t)h.mPaletteFloats);
    if (!table.mPtr || !models.mPtr || !palettes.mPtr || !table.mAddress || !models.mAddress || !palettes.mAddress)
    {
        return;
    }
    memcpy(palettes.mPtr, matrices + 16 * (size_t)h.mMatrices, 4 * (size_t)h.mPaletteFloats);
    const U64 frame_now = Context::instance().getFrameNumber();
    counts.assign(h.mEntries, 0);
    SkinGpu* out = (SkinGpu*)table.mPtr;
    for (U32 i = 0; i < h.mEntries; ++i)
    {
        const LLVkMirror::ShadowSkinEntry& e = entries[i];
        SkinGpu& g = out[i];
        memset(&g, 0, sizeof(g));
        Buf* vb = packet_buffer(e.mVertexBuffer);
        Buf* ib = packet_buffer(e.mIndexBuffer);
        if (!vb || !ib || !vb->mVk.valid() || !ib->mVk.valid() || !vb->mVk.mAddress || !ib->mVk.mAddress)
        {
            continue;       // not created (yet): the draw is skipped, as a draw from an empty buffer is
        }
        const U64 pa = (U64)vb->mVk.mAddress, ia = (U64)ib->mVk.mAddress;
        const U64 wa = pa + e.mWeightOffset, pal = (U64)palettes.mAddress + 4ull * e.mPaletteOffset;
        g.mPositions[0] = (U32)pa;
        g.mPositions[1] = (U32)(pa >> 32);
        g.mIndices[0] = (U32)ia;
        g.mIndices[1] = (U32)(ia >> 32);
        g.mWeights[0] = (U32)wa;
        g.mWeights[1] = (U32)(wa >> 32);
        g.mPalette[0] = (U32)pal;
        g.mPalette[1] = (U32)(pal >> 32);
        g.mFirst = e.mFirstIndex;
        g.mJoints = e.mJoints;
        g.mModel = e.mMatrix;
        counts[i] = e.mIndexCount;
        vb->mLastUseFrame = frame_now;
        ib->mLastUseFrame = frame_now;
        if (packet_used.size() < 2 || packet_used[packet_used.size() - 2] != e.mVertexBuffer || packet_used.back() != e.mIndexBuffer)
        {
            packet_used.push_back(e.mVertexBuffer);
            packet_used.push_back(e.mIndexBuffer);
        }
    }
    memcpy(models.mPtr, matrices, 64 * (size_t)h.mMatrices);

    const U64 table_address = (U64)table.mAddress, models_address = (U64)models.mAddress;
    const S32 table_words[2] = { (S32)(U32)table_address, (S32)(U32)(table_address >> 32) };
    const S32 models_words[2] = { (S32)(U32)models_address, (S32)(U32)(models_address >> 32) };
    program->setInts("ll_packet_table", table_words, 2);
    program->setInts("ll_packet_models", models_words, 2);
    program->setMatrices("ll_view_proj", h.mViewProj, 4, 4, 1);

    if (!prepare_draw(vi, used_cache))
    {
        return;
    }
    ensure_buffers_current(packet_used, 0);
    r.setVertexInput(vi);
    r.setTopology(topology(GL_TRIANGLES));

    Renderer::ScratchAlloc commands;
    const auto& caps = Context::instance().getCaps();
    if (caps.mMultiDrawIndirect && caps.mDrawIndirectFirstInstance)
    {
        commands = r.allocVertexScratch(sizeof(VkDrawIndirectCommand) * (size_t)h.mEntries);
        if (commands.mPtr)
        {
            VkDrawIndirectCommand* c = (VkDrawIndirectCommand*)commands.mPtr;
            for (U32 i = 0; i < h.mEntries; ++i)
            {
                c[i] = { counts[i], 1u, 0u, i };
            }
        }
    }
    if (r.drawPackets(counts.data(), h.mEntries, commands.mPtr ? commands.mBuffer : VK_NULL_HANDLE, commands.mOffset))
    {
        if (sStatsEnabled)
        {
            sStats.mDone += h.mEntries;
        }
    }
    else
    {
        ++sStats.mRendererRefused;
    }
    sDraws += h.mEntries;
    sDrawsTotal += h.mEntries;
}

// <FS> Render packets: the worker side of LLVkMirror::shadowPackets. The program bound is the one of shadowPullV.glsl: it has no vertex input and finds the position of
// every vertex itself, from the buffer addresses in the table written here (one 32 byte entry per draw; the entry number is the draw's instance index).
void on_shadow_packets(const LLVkMirror::ShadowPacketsHeader& h, const U8* blob)
{
    if (h.mKind == 1)
    {
        on_shadow_alpha_packets(h, blob);
        return;
    }
    if (h.mKind == 2)
    {
        on_shadow_skin_packets(h, blob);
        return;
    }
    if (h.mKind == 3)
    {
        on_geometry_packets(h, blob);
        return;
    }
    struct PacketGpu { U32 mPositions[2]; U32 mIndices[2]; U32 mFirst, mCount, mModel, mPad; };
    static_assert(sizeof(PacketGpu) == 32, "packet entry");
    static thread_local VertexInput vi_cache;
    static thread_local std::vector<GLuint> used_cache, packet_used;
    static thread_local std::vector<U32> counts;
    VertexInput& vi = vi_cache;
    vi.mBindings.clear();
    vi.mAttributes.clear();
    used_cache.clear();
    packet_used.clear();

    Renderer& r = Renderer::instance();
    Program* program = r.getProgram();
    if (h.mEntries == 0 || !program || !program->isValid() || !program->hasUniform("ll_packet_table"))
    {
        ++sStats.mNoProgram;
        return;
    }
    const LLVkMirror::ShadowPacketEntry* entries = (const LLVkMirror::ShadowPacketEntry*)blob;
    const F32* matrices = (const F32*)(blob + sizeof(LLVkMirror::ShadowPacketEntry) * h.mEntries);
    Renderer::ScratchAlloc table = r.allocVertexScratch(sizeof(PacketGpu) * (size_t)h.mEntries);
    Renderer::ScratchAlloc models = r.allocVertexScratch(64 * (size_t)std::max<U32>(h.mMatrices, 1));
    if (!table.mPtr || !models.mPtr || !table.mAddress || !models.mAddress)
    {
        return;
    }
    const U64 frame_now = Context::instance().getFrameNumber();
    counts.assign(h.mEntries, 0);
    PacketGpu* out = (PacketGpu*)table.mPtr;
    // the buffers of the draws: a direct-mapped cache of the entries of sGL.mBuffers (their nodes keep their place until erased, and erasing starts the cache over)
    static Buf* cache_buf[1024];
    static GLuint cache_name[1024];
    static U64 cache_generation = ~0ull;
    if (cache_generation != sPacketBufferGeneration)
    {
        memset(cache_buf, 0, sizeof(cache_buf));
        memset(cache_name, 0, sizeof(cache_name));
        cache_generation = sPacketBufferGeneration;
    }
    auto lookup = [&](GLuint name) -> Buf*
    {
        const U32 slot = (name * 2654435761u) >> 22;
        if (cache_name[slot] == name && cache_buf[slot])
        {
            return cache_buf[slot];
        }
        auto it = sGL.mBuffers.find(name);
        if (it == sGL.mBuffers.end())
        {
            return nullptr;
        }
        cache_name[slot] = name;
        cache_buf[slot] = &it->second;
        return &it->second;
    };
    GLuint last_vb = 0, last_ib = 0;
    for (U32 i = 0; i < h.mEntries; ++i)
    {
        const LLVkMirror::ShadowPacketEntry& e = entries[i];
        PacketGpu& g = out[i];
        memset(&g, 0, sizeof(g));
        Buf* vb = lookup(e.mVertexBuffer);
        Buf* ib = lookup(e.mIndexBuffer);
        last_vb = e.mVertexBuffer;
        last_ib = e.mIndexBuffer;
        if (!vb || !ib || !vb->mVk.valid() || !ib->mVk.valid() || !vb->mVk.mAddress || !ib->mVk.mAddress)
        {
            continue;       // not created (yet): the draw is skipped, as a draw from an empty buffer is
        }
        if (Rt::active())
        {
            note_rt_caster(h.mPass, e.mVertexBuffer, e.mIndexBuffer, e.mFirstIndex, e.mIndexCount, e.mMatrixId, 0, e.mMatrix ? matrices + 16 * (size_t)e.mMatrix : nullptr);
        }
        const U64 pa = (U64)vb->mVk.mAddress, ia = (U64)ib->mVk.mAddress;
        g.mPositions[0] = (U32)pa;
        g.mPositions[1] = (U32)(pa >> 32);
        g.mIndices[0] = (U32)ia;
        g.mIndices[1] = (U32)(ia >> 32);
        g.mFirst = e.mFirstIndex;
        g.mCount = e.mIndexCount;
        g.mModel = e.mMatrix;
        counts[i] = e.mIndexCount;
        vb->mLastUseFrame = frame_now;
        ib->mLastUseFrame = frame_now;
        if (packet_used.size() < 2 || packet_used[packet_used.size() - 2] != last_vb || packet_used.back() != last_ib)
        {
            packet_used.push_back(last_vb);
            packet_used.push_back(last_ib);
        }
    }
    memcpy(models.mPtr, matrices, 64 * (size_t)h.mMatrices);

    const U64 table_address = (U64)table.mAddress, models_address = (U64)models.mAddress;
    const S32 table_words[2] = { (S32)(U32)table_address, (S32)(U32)(table_address >> 32) };
    const S32 models_words[2] = { (S32)(U32)models_address, (S32)(U32)(models_address >> 32) };
    program->setInts("ll_packet_table", table_words, 2);
    program->setInts("ll_packet_models", models_words, 2);
    program->setMatrices("ll_view_proj", h.mViewProj, 4, 4, 1);

    if (!prepare_draw(vi, used_cache))
    {
        return;
    }
    ensure_buffers_current(packet_used, 0);
    r.setVertexInput(vi);
    r.setTopology(topology(GL_TRIANGLES));

    // one indirect command for the record when the device allows the first instance in indirect draws (the draw number is the first instance)
    Renderer::ScratchAlloc commands;
    const auto& caps = Context::instance().getCaps();
    if (caps.mMultiDrawIndirect && caps.mDrawIndirectFirstInstance)
    {
        commands = r.allocVertexScratch(sizeof(VkDrawIndirectCommand) * (size_t)h.mEntries);
        if (commands.mPtr)
        {
            VkDrawIndirectCommand* c = (VkDrawIndirectCommand*)commands.mPtr;
            for (U32 i = 0; i < h.mEntries; ++i)
            {
                c[i] = { counts[i], 1u, 0u, i };
            }
        }
    }
    if (r.drawPackets(counts.data(), h.mEntries, commands.mPtr ? commands.mBuffer : VK_NULL_HANDLE, commands.mOffset))
    {
        if (sStatsEnabled)
        {
            sStats.mDone += h.mEntries;
        }
    }
    else
    {
        ++sStats.mRendererRefused;
    }
    sDraws += h.mEntries;
    sDrawsTotal += h.mEntries;
}

// <FS> Ray query shadows: the viewer asks for the frame's scene before the lighting that samples the sun shadow (LLVkMirror::rtScene)
void on_rt_scene(const LLVkMirror::RtSceneArgs& args)
{
    if (!Rt::active())
    {
        return;
    }
    flush_buffer_updates();         // the structures are built over the vertex data: writes that are still pending are recorded first
    Rt::buildScene(args.mView, [](U32 name) { return packet_texture_slot(Renderer::instance(), name); });
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

// <FS> the mirror is about to close the frame: the writes to device-local buffers that no draw has read yet are recorded now (they stage in the frame's own memory,
// which a later frame reuses)
void before_end_frame()
{
    std::lock_guard<LLVkMirror::Mutex> lock(gMutex);
    flush_buffer_updates(true);
    probe_run();
}

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
                        {
                            const ArenaStats as = Context::instance().arenaStats();
                            LL_INFOS("VkPhases") << "buffer arena (" << (arena_enabled() ? "on" : "off, LL_VK_ARENA=0") << "): " << as.mBlocks << " blocks, " << (as.mBlockBytes >> 20) << " MB reserved, "
                                                 << as.mAllocs << " ranges, " << (as.mUsedBytes >> 20) << " MB in use, largest free range " << (as.mLargestFree >> 20) << " MB; host-written (small buffers): "
                                                 << as.mHostBlocks << " blocks, " << (as.mHostBlockBytes >> 20) << " MB reserved, " << as.mHostAllocs << " ranges, " << (as.mHostUsedBytes >> 20)
                                                 << " MB in use; blocks created " << as.mBlocksCreated << ", released " << as.mBlocksReleased << LL_ENDL;
                        }
                        if (probe_on())
                        {
                            LL_INFOS("VkPhases") << probe_report(std::max(1.0, (double)frames)) << LL_ENDL;
                            sProbeStats = ProbeStats();
                        }
                        if (Rt::active())
                        {
                            LL_INFOS("VkPhases") << Rt::report(std::max(1.0, (double)frames)) << LL_ENDL;
                            Rt::resetStats();
                        }
                        const BufTraffic& t = sBufTraffic;
                        LL_INFOS("VkPhases") << "buffer traffic per frame: glBufferData device same size " << (double)t.mSpecDevSame / frames << ", device new allocation "
                                             << (double)t.mSpecDevNew / frames << " (" << (double)t.mSpecDevNewBytes / frames / 1024 << " KB), host-visible " << (double)t.mSpecHost / frames
                                             << " (" << (double)t.mSpecHostBytes / frames / 1024 << " KB); sub-writes device " << (double)t.mSubDev / frames << " ("
                                             << (double)t.mSubDevBytes / frames / 1024 << " KB), host-visible " << (double)t.mSubHost / frames << " (" << (double)t.mSubHostBytes / frames / 1024 << " KB)" << LL_ENDL;
                        LL_INFOS("VkPhases") << "buffer flushes per frame: by a draw " << (double)t.mFlushDraw / frames << ", at frame end " << (double)t.mFlushFrameEnd / frames
                                             << "; buffers with pending writes per flush " << ((t.mFlushDraw + t.mFlushFrameEnd) ? (double)t.mFlushBuffers / (t.mFlushDraw + t.mFlushFrameEnd) : 0.0) << LL_ENDL;
                        {
                            std::vector<std::pair<GLuint, WriterStat>> top(sWriters.begin(), sWriters.end());
                            const size_t n = std::min<size_t>(top.size(), 6);
                            std::partial_sort(top.begin(), top.begin() + n, top.end(), [](const auto& a, const auto& b) { return a.second.mBytes > b.second.mBytes; });
                            std::string text;
                            for (size_t i = 0; i < n; ++i)
                            {
                                const auto bit = sGL.mBuffers.find(top[i].first);
                                char line[200];
                                snprintf(line, sizeof(line), "%s#%u %s %llu KB x%u (%llu KB/write)", i ? "; " : "", (unsigned)top[i].first,
                                         bit != sGL.mBuffers.end() ? (bit->second.mDevicePath ? "device" : "host") : "gone",
                                         (unsigned long long)(top[i].second.mBytes >> 10), (unsigned)top[i].second.mWrites,
                                         (unsigned long long)(top[i].second.mBytes / top[i].second.mWrites >> 10));
                                text += line;
                                if (bit != sGL.mBuffers.end()) text += " size " + std::to_string(bit->second.mSize >> 10) + " KB";
                            }
                            LL_INFOS("VkPhases") << "buffers written most in the interval (" << sWriters.size() << " distinct): " << text << LL_ENDL;
                            // who writes continuously: buffers written in at least half of the frames that wrote anything (the candidates for a per-frame stream path, as opposed to geometry that stays)
                            U32 most_frames = 0;
                            for (const auto& kv : sWriters) most_frames = std::max(most_frames, kv.second.mFrames);
                            U32 cont_dev = 0, cont_host = 0, once_dev = 0, once_host = 0;
                            U64 cont_dev_b = 0, cont_host_b = 0, once_dev_b = 0, once_host_b = 0;
                            U32 cont_host_sizes[4] = {};
                            for (const auto& kv : sWriters)
                            {
                                const auto bit = sGL.mBuffers.find(kv.first);
                                if (bit == sGL.mBuffers.end()) continue;
                                const bool dev = bit->second.mDevicePath, cont = kv.second.mFrames * 2 >= most_frames;
                                U32& n = dev ? (cont ? cont_dev : once_dev) : (cont ? cont_host : once_host);
                                U64& by = dev ? (cont ? cont_dev_b : once_dev_b) : (cont ? cont_host_b : once_host_b);
                                ++n;
                                by += kv.second.mBytes;
                                if (!dev && cont)
                                {
                                    const U64 sz = bit->second.mSize;
                                    ++cont_host_sizes[sz < 4096 ? 0 : sz < 16384 ? 1 : sz < 65536 ? 2 : 3];
                                }
                            }
                            LL_INFOS("VkPhases") << "continuous writers (written in at least half of " << most_frames << " frames): device " << cont_dev << " buffers " << (cont_dev_b >> 10) << " KB, host-visible "
                                                 << cont_host << " buffers " << (cont_host_b >> 10) << " KB (by size <4K " << cont_host_sizes[0] << ", <16K " << cont_host_sizes[1] << ", <64K " << cont_host_sizes[2]
                                                 << ", larger " << cont_host_sizes[3] << "); occasional writers: device " << once_dev << " buffers " << (once_dev_b >> 10) << " KB, host-visible " << once_host
                                                 << " buffers " << (once_host_b >> 10) << " KB" << LL_ENDL;
                            sWriters.clear();
                        }
                        sBufTraffic = BufTraffic();
                    }
                    if (LLVkAsync::gEnabled)
                    {
                        LL_INFOS("VkPhases") << "frame pacing: " << LLVkAsync::pacingReport() << LL_ENDL;
                        LL_INFOS("VkPhases") << "worker operations: " << LLVkAsync::opStatsReport((size_t)frames) << LL_ENDL;
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
                    LL_INFOS("VkPhases") << "texture upload costs per frame: " << LLVkAsync::texUploadReport((size_t)frames) << LL_ENDL;
                    LL_INFOS("VkPhases") << "texture uploads per frame: " << (double)up.mCount / frames << " uploads, " << (double)up.mBytes / frames / 1024.0 << " KB, "
                                         << (double)up.mPassSplits / frames << " ended a render pass in progress" << LL_ENDL;
                    {
                        static U64 last_barriers = 0, last_built = 0, last_micros = 0;
                        const U64 barriers = gSyncStats.mImageBarriers.load(std::memory_order_relaxed);
                        const U64 built = gSyncStats.mPipelinesBuilt.load(std::memory_order_relaxed);
                        const U64 micros = gSyncStats.mPipelineBuildMicros.load(std::memory_order_relaxed);
                        const U64 slowest = gSyncStats.mPipelineBuildMaxMicros.exchange(0, std::memory_order_relaxed);
                        LL_INFOS("VkPhases") << "barrier causes: " << Renderer::instance().takeBarrierReport((size_t)frames) << LL_ENDL;
                        LL_INFOS("VkPhases") << "image barriers per frame: " << (double)(barriers - last_barriers) / frames << "; graphics pipelines built in this period: "
                                             << (built - last_built) << " (" << (double)(micros - last_micros) / 1000.0 << " ms in the driver in total, slowest "
                                             << (double)slowest / 1000.0 << " ms)" << LL_ENDL;
                        {
                            static U64 l_miss = 0, l_skip = 0, l_bg = 0, l_bgus = 0, l_pq = 0, l_pd = 0, l_pus = 0;
                            const U64 miss = gSyncStats.mPipeMisses.load(), skip = gSyncStats.mPipeSkippedDraws.load(), bg = gSyncStats.mPipeBgDone.load(), bgus = gSyncStats.mPipeBgMicros.load();
                            const U64 pq = gSyncStats.mPipePrewarmQueued.load(), pd = gSyncStats.mPipePrewarmDone.load(), pus = gSyncStats.mPipePrewarmMicros.load();
                            const U64 bgmax = gSyncStats.mPipeBgMaxMicros.exchange(0);
                            LL_INFOS("VkPhases") << "pipelines off the draw path in this period: misses queued " << (miss - l_miss) << ", draws dropped while one was building " << (skip - l_skip)
                                                 << ", background builds done for a draw " << (bg - l_bg) << " (" << (double)(bgus - l_bgus) / 1000.0 << " ms of thread time, slowest " << (double)bgmax / 1000.0
                                                 << " ms); pre-warm queued " << (pq - l_pq) << ", done " << (pd - l_pd) << " (" << (double)(pus - l_pus) / 1000.0 << " ms of thread time)" << LL_ENDL;
                            l_miss = miss; l_skip = skip; l_bg = bg; l_bgus = bgus; l_pq = pq; l_pd = pd; l_pus = pus;
                        }
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
                            for (size_t k = 0; k < by_time.size() && k < 60; ++k)
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
    sFrameTargets.clear();
    memset(sRecentFrameTargets, 0, sizeof(sRecentFrameTargets));
    sFrameSampledLast = sFrameSampled;
    sFrameSampled.clear();
    sStatsLast = sStats;
    sStats = DrawStats();
}

//-----------------------------------------------------------------------------------------------------
// hooks: calls that return data or state, answered from the mirror's own state (or the Vulkan device)
//-----------------------------------------------------------------------------------------------------

namespace
{

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
        // <FS> what a GL driver reports (32 per stage, 192 combined), not the descriptor limit of the device (a million): the viewer loops over this many texture units
        // (display() unbinds every one of them each frame: 2.6 ms for a million).
        out[0] = (GLint)std::min<U32>((U32)caps.mMaxSampledImagesPerStage, pname == 0x8B4D ? 192u : 32u); return true;
    case 0x8D57: out[0] = (GLint)caps.mMaxSampleCount; return true;                 // MAX_SAMPLES
    // C6: the limits that used to read as 0 (nothing answered them)
    case 0x80E8: out[0] = (GLint)caps.mRecommendedMaxVertices; return true;         // MAX_ELEMENTS_VERTICES
    case 0x80E9: out[0] = (GLint)caps.mRecommendedMaxIndices; return true;          // MAX_ELEMENTS_INDICES
    case 0x910E: out[0] = (GLint)caps.mMaxColorTextureSamples; return true;         // MAX_COLOR_TEXTURE_SAMPLES
    case 0x910F: out[0] = (GLint)caps.mMaxDepthTextureSamples; return true;         // MAX_DEPTH_TEXTURE_SAMPLES
    case 0x9110: out[0] = (GLint)caps.mMaxIntegerSamples; return true;              // MAX_INTEGER_SAMPLES
    case 0x8E59: out[0] = (GLint)caps.mMaxSampleMaskWords; return true;             // MAX_SAMPLE_MASK_WORDS
    case 0x8DFC: out[0] = (GLint)caps.mMaxVaryingVectors; return true;              // MAX_VARYING_VECTORS
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


// ---- queries ----
struct QueryRec
{
    GLuint mName = 0;                   // the GL name the query was begun under
    U64 mGen = 0;                       // the async generation of that begin (0 in inline mode)
    GLenum mTarget = 0;
    bool mOcc = false, mTime = false;
    bool mVkHave = false, mHaveStart = false, mHaveEnd = false;
    U64 mVkValue = 0, mTsStart = 0, mTsEnd = 0;
};
std::map<U64, QueryRec> sQueries;               // by serial, oldest first
std::unordered_map<GLuint, U64> sQueryLatest;   // GL query name -> serial of its latest begin
std::map<GLenum, U64> sQueryActive;             // target -> serial of the query in progress
U64 sQuerySerial = 0;

bool queries_on()
{
    static const bool disabled = getenv("LL_VK_NO_QUERIES") != nullptr;
    return gOwn && !disabled;
}

// pulls finished Vulkan query results and hands them to the main thread
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
    }
    while (sQueries.size() > 40000)         // results that never found their partner
    {
        sQueries.erase(sQueries.begin());
    }
}


} // anonymous namespace







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
        sQueryActive.erase(target);
        if (LLVkAsync::gEnabled) LLVkAsync::queryIdle(id, LLVkAsync::workerQueryGen(id));
        return;
    }
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




// the running totals, for the log


// ---- sync objects and which threads feed the mirror ----
namespace
{
struct SyncRec { U64 mFrame = 0; bool mInFrame = false; U64 mFlush = 0; };
std::unordered_map<GLsync, SyncRec> sSyncs;
}



void on_glWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout)
{
    (void)sync; (void)flags; (void)timeout;      // server-side waits have no meaning here: the mirror owns the fences
}

void on_glDeleteSync(GLsync sync)
{
    sSyncs.erase(sync);
}



//-----------------------------------------------------------------------------------------------------
// texel decoding and read-back helpers for glReadPixels / glGetTexImage answers
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

struct VerifyCounters
{
    U32 mChecks = 0, mMismatching = 0, mSkipped = 0;
    std::map<std::string, U32> mSkipReasons;
};
VerifyCounters sVerify;

} // anonymous namespace


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
    env.mBindless = bindlessShaders();      // <FS> this is the compile every program takes while the mirror owns the resources
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

#endif // LL_VULKAN

