/**
 * @file llvkgl.cpp
 * @brief Translation of the viewer's OpenGL call stream into Vulkan (see llvkgl.h).
 *
 * Every hook runs after the real GL call. The hooks keep a copy of the GL state that matters (textures, framebuffer
 * attachments, buffers, vertex arrays, draw state) and, at each draw, translate it into Vulkan state and replay the
 * draw into the mirror frame.
 */

#include "linden_common.h"

#include "llglheaders.h"

#if defined(LL_VULKAN)

#include "llvkmirror.h"
#include "llglslshader.h"
#include "llshadermgr.h"
#include "llvkshader.h"
#include "llvkcontext.h"
#include "llvkrenderer.h"
#include "llvkstate.h"
#include "llvktex.h"
#include "llvkbuf.h"
#include "llvktarget.h"
#include "llvkquery.h"
#include "llvkformat.h"
#include "llvkdlss.h"
#include "llvkrt.h"

#include <chrono>
#include "llvkgl.h"
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

bool store_info(LLVkFormat::Store st, FormatMap& m)
{
    using namespace LLVkFormat;
    m = FormatMap();
    switch (st)
    {
    case S_RGBA8: case S_RGB8: m.mVk = VK_FORMAT_R8G8B8A8_UNORM; return true;
    case S_SRGBA8: case S_SRGB8: m.mVk = VK_FORMAT_R8G8B8A8_SRGB; return true;
    case S_ALPHA8: m.mVk = VK_FORMAT_R8_UNORM; m.mTexelBytes = 1; m.mSwizzle = SWZ_ALPHA; return true;
    case S_LUM8: m.mVk = VK_FORMAT_R8_UNORM; m.mTexelBytes = 1; m.mSwizzle = SWZ_LUM; return true;
    case S_LUM_ALPHA8: m.mVk = VK_FORMAT_R8G8_UNORM; m.mTexelBytes = 2; m.mSwizzle = SWZ_LUM_ALPHA; return true;
    case S_RG16F: m.mVk = VK_FORMAT_R16G16_SFLOAT; m.mTexelBytes = 4; return true;
    case S_RG32F: m.mVk = VK_FORMAT_R32G32_SFLOAT; m.mTexelBytes = 8; return true;
    case S_RG16: m.mVk = VK_FORMAT_R16G16_UNORM; m.mTexelBytes = 4; return true;
    case S_R8: m.mVk = VK_FORMAT_R8_UNORM; m.mTexelBytes = 1; return true;
    case S_RG8: m.mVk = VK_FORMAT_R8G8_UNORM; m.mTexelBytes = 2; return true;
    case S_R32F: m.mVk = VK_FORMAT_R32_SFLOAT; m.mTexelBytes = 4; return true;
    case S_R16F: m.mVk = VK_FORMAT_R16_SFLOAT; m.mTexelBytes = 2; return true;
    case S_RGBA16F: m.mVk = VK_FORMAT_R16G16B16A16_SFLOAT; m.mTexelBytes = 8; return true;
    case S_RGBA32F: m.mVk = VK_FORMAT_R32G32B32A32_SFLOAT; m.mTexelBytes = 16; return true;
    case S_RGBA16: m.mVk = VK_FORMAT_R16G16B16A16_UNORM; m.mTexelBytes = 8; return true;
    case S_R11G11B10F: m.mVk = VK_FORMAT_B10G11R11_UFLOAT_PACK32; return true;
    case S_RGB10A2: m.mVk = VK_FORMAT_A2B10G10R10_UNORM_PACK32; return true;
    case S_DEPTH: m.mVk = VK_FORMAT_D32_SFLOAT; m.mTexelBytes = 4; m.mDepth = true; return true;
    case S_BC1_RGB: m.mVk = VK_FORMAT_BC1_RGB_UNORM_BLOCK; m.mTexelBytes = 8; m.mCompressed = true; return true;
    case S_BC1_RGBA: m.mVk = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; m.mTexelBytes = 8; m.mCompressed = true; return true;
    case S_BC2: m.mVk = VK_FORMAT_BC2_UNORM_BLOCK; m.mTexelBytes = 16; m.mCompressed = true; return true;
    case S_BC3: m.mVk = VK_FORMAT_BC3_UNORM_BLOCK; m.mTexelBytes = 16; m.mCompressed = true; return true;
    case S_BC7: m.mVk = VK_FORMAT_BC7_UNORM_BLOCK; m.mTexelBytes = 16; m.mCompressed = true; return true;
    // generic compressed formats: the driver chose; the mirror stores plain RGBA8
    case S_GENERIC_COMPRESSED: m.mVk = VK_FORMAT_R8G8B8A8_UNORM; return true;
    default: return false;
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
bool is_rgba8_fast(const FormatMap& fm, LLVkFormat::Transfer tr)
{
    using namespace LLVkFormat;
    const U32 comps = compCount(tr.mComps);
    return !fm.mDepth && (fm.mVk == VK_FORMAT_R8G8B8A8_UNORM || fm.mVk == VK_FORMAT_R8G8B8A8_SRGB) && tr.mType == T_U8
        && ((comps == 4 && tr.mComps != C_BGRA) || (comps == 3 && tr.mComps != C_BGR));
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
bool convert_pixels(const FormatMap& fm, LLVkFormat::Transfer tr, const void* src, U32 width, U32 height, U32 depth,
                    U32 alignment, U32 row_length, std::vector<U8>& out)
{
    using namespace LLVkFormat;
    const U32 comps = compCount(tr.mComps);
    const U32 tb = typeBytes(tr.mType);
    if (!comps || !tb)
    {
        warn_once("pixel format/type", ((U32)tr.mComps << 8) | tr.mType);
        return false;
    }
    const U32 src_row_texels = row_length ? row_length : width;
    U32 src_row_bytes = src_row_texels * comps * tb;
    src_row_bytes = (src_row_bytes + alignment - 1) / alignment * alignment;
    const U32 dst_texel = fm.mTexelBytes;

    // the common case, an 8-bit RGBA or RGB image into an RGBA8 texture: rows are copied or widened without any per-texel decisions
    if (is_rgba8_fast(fm, tr))
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
                    if (tr.mType == T_F32) memcpy(&v, p, 4);
                    else if (tr.mType == T_U32) { U32 u; memcpy(&u, p, 4); v = (F32)((double)u / 4294967295.0); }
                    else if (tr.mType == T_U16) { U16 u; memcpy(&u, p, 2); v = (F32)u / 65535.f; }
                    memcpy(d, &v, 4);
                    continue;
                }
                if (fm.mVk == VK_FORMAT_R8G8B8A8_UNORM || fm.mVk == VK_FORMAT_R8G8B8A8_SRGB)
                {
                    if (tr.mType != T_U8) { warn_once("rgba8 source type", tr.mType); return false; }
                    switch (comps)
                    {
                    case 4: d[0] = p[0]; d[1] = p[1]; d[2] = p[2]; d[3] = p[3]; if (tr.mComps == C_BGRA) std::swap(d[0], d[2]); break;
                    case 3: d[0] = p[0]; d[1] = p[1]; d[2] = p[2]; d[3] = 255; if (tr.mComps == C_BGR) std::swap(d[0], d[2]); break;
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
                        if (tr.mType == T_F32) { F32 v; memcpy(&v, p + c * 4, 4); h[c] = float_to_half(v); }
                        else if (tr.mType == T_F16) memcpy(&h[c], p + c * 2, 2);
                        else if (tr.mType == T_U8) h[c] = float_to_half(p[c] / 255.f);
                        else { warn_once("rgba16f source type", tr.mType); return false; }
                    }
                    memcpy(d, h, 8);
                }
                else if (fm.mVk == VK_FORMAT_R32G32B32A32_SFLOAT)
                {
                    F32 v[4] = { 0, 0, 0, 1 };
                    for (U32 c = 0; c < comps && c < 4; ++c)
                    {
                        if (tr.mType == T_F32) memcpy(&v[c], p + c * 4, 4);
                        else if (tr.mType == T_F16) { U16 h; memcpy(&h, p + c * 2, 2); v[c] = half_to_float(h); }
                        else if (tr.mType == T_U8) v[c] = p[c] / 255.f;
                        else { warn_once("rgba32f source type", tr.mType); return false; }
                    }
                    memcpy(d, v, 16);
                }
                else if (fm.mVk == VK_FORMAT_R32_SFLOAT || fm.mVk == VK_FORMAT_R16_SFLOAT)
                {
                    F32 v = 0.f;
                    if (tr.mType == T_F32) memcpy(&v, p, 4);
                    else if (tr.mType == T_U8) v = p[0] / 255.f;
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
    LLVkFormat::Filter mMin = LLVkFormat::F_NEAREST_MIP_LINEAR;
    LLVkFormat::Filter mMag = LLVkFormat::F_LINEAR;
    LLVkFormat::Wrap mWrapS = LLVkFormat::W_REPEAT;
    LLVkFormat::Wrap mWrapT = LLVkFormat::W_REPEAT;
    LLVkFormat::Wrap mWrapR = LLVkFormat::W_REPEAT;
    F32 mAnisotropy = 1.f;
    bool mCompare = false;
    LLVkFormat::Compare mCompareFunc = LLVkFormat::CMP_LEQUAL;
    S32 mBaseLevel = 0;
    S32 mMaxLevel = 1000;
};

struct Tex
{
    Image mImage;
    U8 mDim = LLVkFormat::DIM_OTHER;    // LLVkFormat::Dim
    U32 mWidth = 0, mHeight = 0, mDepth = 0;
    U32 mLevels = 0;
    U8 mStore = LLVkFormat::S_NONE;     // LLVkFormat::Store
    FormatMap mFormat;
    SamplerParams mSampler;
    U32 mValidLevels = 0;               // contiguous mip levels from 0 that hold data (the rest would sample as black)
    LLVkFormat::Swizzle mSwizzle[4] = { LLVkFormat::SW_R, LLVkFormat::SW_G, LLVkFormat::SW_B, LLVkFormat::SW_A };
    bool mSwizzleSet = false;
    std::map<std::pair<U32, U32>, std::unique_ptr<Image>> mViews;     // (level, layer) -> render-target view
    VkSampler mVkSampler = VK_NULL_HANDLE;      // cached sampler for the parameters hashed in mSamplerSig
    U64 mSamplerSig = 0;
    U32 mBindlessEpoch = 0;                     // <FS> the slot this texture holds in the bindless table, for the frame (epoch) and image view it was taken for
    U32 mBindlessSlot = 0;
    VkImageView mBindlessView = VK_NULL_HANDLE;
};

VkComponentSwizzle swizzle_component(LLVkFormat::Swizzle s)
{
    switch (s)
    {
    case LLVkFormat::SW_R: return VK_COMPONENT_SWIZZLE_R;
    case LLVkFormat::SW_G: return VK_COMPONENT_SWIZZLE_G;
    case LLVkFormat::SW_B: return VK_COMPONENT_SWIZZLE_B;
    case LLVkFormat::SW_A: return VK_COMPONENT_SWIZZLE_A;
    case LLVkFormat::SW_ZERO: return VK_COMPONENT_SWIZZLE_ZERO;
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
    t.mImage.mViewId = 0;       // a different view: the bindless table must not take it for the old one
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
    unsigned mRtName = 0;               // its GL name, for the list of changed watched buffers
    bool mRtWatched = false;            // the ray scene reads this buffer at trace time (alpha records): a change of its storage is counted (sBufferTopology)
    // The Vulkan buffer lives in write-combined video memory, which is very slow to read from the CPU. This copy in ordinary memory
    // is what a renamed buffer is refilled from when only part of it is rewritten.
    std::vector<U8> mShadow;
};

struct VaoAttrib
{
    bool mEnabled = false;
    U32 mBuffer = 0;
    S32 mSize = 4;
    U8 mType = LLVkFormat::T_F32;       // LLVkFormat::PType
    bool mNormalized = false;
    bool mInteger = false;
    S32 mStride = 0;
    U64 mOffset = 0;
};

struct VertexInputState
{
    VaoAttrib mAttrib[16];
    U32 mArrayBuffer = 0;        // the buffer the next attribute pointers refer to
    U32 mElementBuffer = 0;
};
VertexInputState sVin;

struct FboAttachment
{
    U32 mTexture = 0;
    S32 mLevel = 0;
    U32 mLayer = 0;
};

struct Fbo
{
    FboAttachment mColor[8];
    FboAttachment mDepth;
    U32 mDrawCount = 1;
};

// state that belongs to one GL context (the viewer has a texture worker thread with its own, sharing objects)
struct PerContext
{
    U32 mPixelUnpackBuffer = 0;
    S32 mUnpackAlignment = 4;
    S32 mUnpackRowLength = 0;
    U32 mPixelPackBuffer = 0;
};
thread_local PerContext sCtxTls;
thread_local PerContext* sCtxOverride = nullptr;      // set on the async worker: the context of the thread whose call is being replayed
inline PerContext& current_ctx() { return sCtxOverride ? *sCtxOverride : sCtxTls; }
#define sCtx (current_ctx())

// F1.2: the texture slots a draw samples from (unit, target class): written by the typed slot records only
U32 sSlots[32][8] = {};
// F1.4: the render target draws go to (0: the back buffer): set by the typed target state record only
U32 sDrawFbo = 0;

struct GLState
{
    // objects (shared between contexts)
    std::unordered_map<U32, Tex> mTextures;
    std::unordered_map<U32, Fbo> mFbos;
    std::unordered_map<U32, Buf> mBuffers;
    F32 mConstAttrib[16][4];

    // draw state: the render state lives in LLVkState (F1.1); only the clear colour is still kept here

    GLState()
    {
        for (auto& a : mConstAttrib) { a[0] = a[1] = a[2] = 0.f; a[3] = 1.f; }
    }
};

// never destroyed: the viewer's own statics (gPipeline's render targets, the textures of the image lists) release their GL objects when the process exits, after this file's statics would
// be gone, and every one of those calls lands here (the shutdown of the mirror itself is LLVkMirror::shutdown). A destroyed map under that call was the crash at exit.
GLState& sGL = *new GLState;

// <FS> flat lookup of the entries of sGL.mTextures / mBuffers by GL name (the names are small integers from the viewer's name pools). A hash lookup for every sampler of
// every draw cost more than the rest of the draw's bookkeeping. The pointers are to the nodes of the maps (stable until erased); erasing forgets them.
std::vector<Tex*> sTexFlat;
std::vector<Buf*> sBufFlat;

inline Tex* tex_ptr(U32 name)
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
inline void tex_forget(U32 name) { if (name < sTexFlat.size()) sTexFlat[name] = nullptr; }

inline Buf* buf_ptr(U32 name)
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
inline void buf_forget(U32 name) { if (name < sBufFlat.size()) sBufFlat[name] = nullptr; }

// <FS> what sync_target() resolved for the current draw framebuffer, valid until something that can change it happens (bumps sTargetEpoch)
U64 sTargetEpoch = 1;
const void* sTargetNamesOwner = nullptr;      // whose list of attachment names sCurTargets holds
std::set<U32> sFrameTargets;         // textures rendered into during the current frame (for the debug dump)
U32 sUboBinding[4] = {};             // GL buffer bound to each named uniform block binding point
std::map<U32, U32> sFrameSampled;    // textures read by draws this frame, with use counts
std::map<U32, U32> sFrameSampledLast;
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
std::vector<U32> sCurTargets;        // textures attached to the current draw framebuffer
U32 sRecentFrameTargets[16] = {};    // cheap filter in front of sFrameTargets: ids inserted recently this frame
U32 sCurTargetW = 1, sCurTargetH = 1;   // size of the first colour target of the current draw framebuffer
// the per-draw statistics / pass graph are only gathered when asked for (they cost string work on every draw)
const bool sStatsEnabled = getenv("LL_VK_STATS") != nullptr || getenv("LL_VK_DEBUG_SHADERS") != nullptr || getenv("LL_VK_VALIDATION") != nullptr;
bool sDbgMatch = false;                 // the draw being prepared belongs to a program the debug filter selects
bool sDbgNoDepth = false, sDbgNoDepthWrite = false, sDbgNoDepthTest = false, sDbgNoBlend = false, sDbgNoCull = false, sDbgNoScissor = false,
     sDbgFullMask = false, sDbgNoFeedback = false;      // debug draw state overrides from <work>\vk-debug.txt
int sDbgMode = 0;                       // debug shader mode from <work>\vk-debug.txt (0 = off)
std::string sDbgFilter;                 // programs whose name contains this get the mode
std::string sDbgSampler;                // sampler shown by mode 2
bool sHasDepth = false;             // does the current render target have a depth attachment

// <FS> F1.2: a texture edit names its texture (LLVkTex): while a scope is open the edit functions below act on that texture and read the unpack state
// from the record, not from the GL bound-texture / pixel-store emulation. thread_local: the worker replays one record at a time, inline mode runs on the caller.
thread_local U32 tlsEditTex = 0;
thread_local const LLVkTex::PixelStore* tlsEditPS = nullptr;

Tex* edit_texture()
{
    if (tlsEditTex)
    {
        auto it = sGL.mTextures.find(tlsEditTex);
        return it == sGL.mTextures.end() ? nullptr : &it->second;
    }
    return nullptr;         // every texture edit names its texture (LLVkTex)
}

inline U32 edit_unpack_buffer() { return tlsEditPS ? tlsEditPS->mUnpackBuffer : sCtx.mPixelUnpackBuffer; }
inline S32 edit_unpack_alignment() { return tlsEditPS ? tlsEditPS->mAlignment : sCtx.mUnpackAlignment; }
inline S32 edit_unpack_row_length() { return tlsEditPS ? tlsEditPS->mRowLength : sCtx.mUnpackRowLength; }

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
bool ensure_image(Tex& t, LLVkFormat::Target tg, LLVkFormat::Store st, U32 w, U32 h, U32 depth)
{
    using namespace LLVkFormat;
    FormatMap fm;
    if (!store_info(st, fm))
    {
        if (sVerifySnapshotPending)
        {
            LL_INFOS("VkVerify") << "snapshot: texture " << (S32)w << "x" << (S32)h << " with storage format " << (S32)st << " has no Vulkan format; it will not exist in Vulkan" << LL_ENDL;
        }
        return false;
    }
    TexType type = TexType::Tex2D;
    U32 d = 1;
    switch (tg.mDim)
    {
    case DIM_CUBE: type = TexType::Cube; break;
    case DIM_CUBE_ARRAY: type = TexType::CubeArray; d = std::max<U32>(1, depth / 6); break;
    case DIM_2D_ARRAY: type = TexType::Tex2DArray; d = std::max<U32>(1, depth); break;
    case DIM_3D: type = TexType::Tex3D; d = std::max<U32>(1, depth); break;
    default: break;
    }

    if (t.mImage.valid() && t.mDim == tg.mDim && t.mWidth == w && t.mHeight == h && t.mDepth == depth && t.mStore == st)
    {
        return true;        // same specification again: keep the image and just upload into it
    }
    ++sTargetEpoch;
    if (t.mImage.valid())
    {
        if (sVerifySnapshotPending)
        {
            LL_INFOS("VkVerify") << "snapshot: texture re-specified from " << t.mWidth << "x" << t.mHeight << " fmt " << (S32)t.mStore << " to " << w << "x" << h << " fmt " << (S32)st
                                 << ", Vulkan image recreated (anything drawn into it so far is lost)" << LL_ENDL;
        }
        destroy_tex_images(t);
    }

    TextureDesc td;
    td.mType = type;
    td.mWidth = w;
    td.mHeight = h;
    td.mDepth = d;
    // a plain 2D texture starts with its base level only: the rest of the chain is allocated when something needs it (grow_levels). Most of the viewer's textures get it at once
    // (they are mipmapped right after the upload), but the UI images, the glyph pages and most render targets never do and would carry a third more memory for nothing.
    static const bool full_chain = getenv("LL_VK_FULL_MIPS") != nullptr;       // LL_VK_FULL_MIPS=1: every texture owns its whole chain from the start, as before (to compare)
    td.mMips = (tg.mDim == DIM_2D && !full_chain) ? 1 : (tg.mDim == DIM_3D ? mipLevelCount(std::max(w, h), depth) : mipLevelCount(w, h));
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
    t.mDim = tg.mDim;
    t.mWidth = w;
    t.mHeight = h;
    t.mDepth = depth;
    t.mValidLevels = 0;
    t.mLevels = td.mMips;
    t.mStore = st;
    t.mFormat = fm;
    return true;
}

// The rest of the mip chain of a 2D texture that started with its base level only: a new image with every level, level 0 copied over, the old image released. Called when something
// uploads or renders into a level above 0, or generates the chain. Arrays, cubes and 3D textures own their chain from the start.
bool grow_levels(Tex& t)
{
    using namespace LLVkFormat;
    if (!t.mImage.valid() || t.mDim != DIM_2D)
    {
        return t.mImage.valid();
    }
    const U32 full = mipLevelCount(t.mWidth, t.mHeight);
    if (t.mLevels >= full)
    {
        return true;
    }
    TextureDesc td;
    td.mType = TexType::Tex2D;
    td.mWidth = t.mWidth;
    td.mHeight = t.mHeight;
    td.mDepth = 1;
    td.mMips = full;
    td.mFormat = t.mFormat.mVk;
    td.mSwizzle = t.mSwizzleSet ? swizzle_mapping(t) : t.mFormat.mSwizzle;
    if (t.mFormat.mDepth)
    {
        td.mUsage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    else if (!t.mFormat.mCompressed)
    {
        td.mUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (LLVkDlss::ready() && (t.mFormat.mVk == VK_FORMAT_R8G8B8A8_UNORM || t.mFormat.mVk == VK_FORMAT_R16G16B16A16_SFLOAT))
        {
            td.mUsage |= VK_IMAGE_USAGE_STORAGE_BIT;
        }
    }
    Image fresh;
    if (!Renderer::instance().createTexture(td, fresh))
    {
        return false;
    }
    Renderer::instance().copyBaseLevel(t.mImage, fresh);
    destroy_tex_images(t);
    t.mImage = fresh;
    t.mLevels = full;
    for (auto& kv : sGL.mTextures)
    {
        if (&kv.second == &t)
        {
            Context::instance().setObjectName(VK_OBJECT_TYPE_IMAGE, (U64)t.mImage.mImage, "GL texture " + std::to_string(kv.first));
            break;
        }
    }
    return true;
}

// source of client pixel data: either a pointer or an offset into the bound pixel unpack buffer
const void* pixel_source(const void* pixels)
{
    if (edit_unpack_buffer())
    {
        auto it = sGL.mBuffers.find(edit_unpack_buffer());
        if (it == sGL.mBuffers.end() || !it->second.mVk.mMapped)
        {
            return nullptr;
        }
        return (const U8*)it->second.mVk.mMapped + (size_t)(U64)pixels;
    }
    return pixels;
}

void upload_converted(Tex& t, LLVkFormat::Target tg, S32 level, S32 x, S32 y, S32 z, U32 w, U32 h, U32 d, bool converted, std::vector<U8>& texels);

void upload_level(Tex& t, LLVkFormat::Target tg, S32 level, S32 x, S32 y, S32 z, U32 w, U32 h, U32 d, LLVkFormat::Transfer tr, const void* pixels)
{
    using namespace LLVkFormat;
    if (t.mImage.valid() && level > 0 && (U32)level >= t.mLevels)
    {
        grow_levels(t);
    }
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
    if (LLVkAsync::gTexOneCopy && LLVkAsync::gEnabled && d == 1 && t.mDim != DIM_2D_ARRAY && t.mDim != DIM_CUBE_ARRAY && t.mDim != DIM_3D
        && is_rgba8_fast(t.mFormat, tr) && w && h)
    {
        // C4: the common upload (8-bit RGB or RGBA into an RGBA8 texture) is read from where the viewer left it and written straight into the staging buffer: one copy
        struct Fast { const U8* mSrc; U32 mW, mH, mComps, mRowBytes; } fast;
        const U32 alignment = edit_unpack_alignment() > 0 ? (U32)edit_unpack_alignment() : 4, row_length = (U32)edit_unpack_row_length();
        fast.mSrc = (const U8*)src;
        fast.mW = w;
        fast.mH = h;
        fast.mComps = compCount(tr.mComps);
        fast.mRowBytes = ((row_length ? row_length : w) * fast.mComps + alignment - 1) / alignment * alignment;
        const U32 layer = tg.mFace >= 0 ? (U32)tg.mFace : 0;       // cube face
        Renderer::instance().uploadTextureDirect(t.mImage, (U32)level, layer, (U32)x, (U32)y, w, h, 1, (VkDeviceSize)w * h * 4,
                                                 [](void* p, U8* dst, VkDeviceSize) { const Fast* f = (const Fast*)p; convert_rgba8_rows(f->mSrc, f->mW, f->mH, 1, f->mComps, f->mRowBytes, dst); }, &fast);
        return;
    }
    static thread_local std::vector<U8> texels;       // reused between uploads of one thread
    {
        // A large conversion takes milliseconds; the viewer's texture thread and its main thread both feed this lock, so a big image
        // is converted without holding it. Nothing the conversion reads is shared: the pixels belong to the caller, who is waiting.
        const FormatMap fm = t.mFormat;
        const U32 alignment = (U32)edit_unpack_alignment(), row_length = (U32)edit_unpack_row_length();
        const VkImage handle = t.mImage.mImage;
        const U32 name = tlsEditTex;
        const bool unlocked = (U64)w * h * d >= 16384 && !edit_unpack_buffer() && !LLVkAsync::gEnabled;      // (the worker does not take the lock)
        unsigned depth_held = 0;
        if (unlocked)
        {
            depth_held = LLVkGL::gMutex.releaseAll();
        }
        const auto t_conv = std::chrono::steady_clock::now();
        const bool converted = convert_pixels(fm, tr, src, w, h, d, alignment, row_length, texels);
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
            return upload_converted(it->second, tg, level, x, y, z, w, h, d, converted, texels);
        }
        if (!converted)
        {
            return;
        }
    }
    upload_converted(t, tg, level, x, y, z, w, h, d, true, texels);
}

void upload_converted(Tex& t, LLVkFormat::Target tg, S32 level, S32 x, S32 y, S32 z, U32 w, U32 h, U32 d, bool converted, std::vector<U8>& texels)
{
    using namespace LLVkFormat;
    if (!converted || !t.mImage.valid())
    {
        return;
    }
    U32 layer = 0;
    if (tg.mFace >= 0)
    {
        layer = (U32)tg.mFace;              // cube face
    }
    else if (t.mDim == DIM_2D_ARRAY || t.mDim == DIM_CUBE_ARRAY || t.mDim == DIM_3D)
    {
        layer = (U32)z;
    }
    const U32 slices = t.mDim == DIM_3D ? d : 1;
    if (t.mDim == DIM_2D_ARRAY || t.mDim == DIM_CUBE_ARRAY)
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
Image* texture_view(U32 name, S32 level, U32 layer)
{
    Tex* tp = tex_ptr(name);
    if (!tp || !tp->mImage.valid())
    {
        return nullptr;
    }
    Tex& t = *tp;
    if (level > 0 && !grow_levels(t))
    {
        return nullptr;
    }
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

// the color image reads (copies, readbacks) come from: colour attachment 0 of a render target (0: the back buffer)
Image* read_image(U32 fbo)
{
    if (fbo == 0)
    {
        return LLVkMirror::backbufferColor();
    }
    auto it = sGL.mFbos.find(fbo);
    if (it == sGL.mFbos.end())
    {
        return nullptr;
    }
    return attachment_image(it->second.mColor[0]);         // reads come from colour attachment 0
}

Image* read_image()
{
    return read_image(sDrawFbo);        // readbacks (glReadPixels) read the target that was last made current
}

// makes the renderer's render target match the GL draw framebuffer. Returns false if there is nothing to draw to.
// remembers that a texture was rendered into this frame; the same few textures come back on every draw, so look in a small cache first
static inline void note_frame_target(U32 texture)
{
    U32& slot = sRecentFrameTargets[texture & 15];
    if (slot != texture)
    {
        slot = texture;
        sFrameTargets.insert(texture);
    }
}

const LLVkState::Block* state_block();

bool sync_target()
{
    Renderer& r = Renderer::instance();
    if (sDrawFbo == 0)
    {
        sCurTargets.clear();
        sTargetNamesOwner = nullptr;
    }
    if (sDrawFbo == 0)
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
                const LLVkState::Block* vb = state_block();
                const S32 now[9] = { vb ? vb->mViewport[0] : 0, vb ? vb->mViewport[1] : 0, vb ? vb->mViewport[2] : 0, vb ? vb->mViewport[3] : 0, 0, 0, 0, 0, (S32)color->mExtent.width };
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
    struct TargetCache { U64 mEpoch = 0; U32 mFbo = ~0u; U64 mFrame = ~0ull; Image* mColors[8] = {}; U32 mCount = 0; Image* mDepth = nullptr; bool mOk = false; std::vector<U32> mNames; };
    static TargetCache cache;
    const U64 frame_now = Context::instance().getFrameNumber();
    if (cache.mEpoch == sTargetEpoch && cache.mFbo == sDrawFbo && cache.mFrame == frame_now && !sDbgMatch && cache.mOk)
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
    auto it = sGL.mFbos.find(sDrawFbo);
    if (it == sGL.mFbos.end())
    {
        return false;
    }
    Fbo& fbo = it->second;
    Image* colors[8] = {};
    U32 count = 0;
    for (U32 i = 0; i < fbo.mDrawCount && i < 8; ++i)
    {
        Image* img = attachment_image(fbo.mColor[i]);
        if (!img)
        {
            break;
        }
        colors[count++] = img;
        note_frame_target(fbo.mColor[i].mTexture);
        sCurTargets.push_back(fbo.mColor[i].mTexture);
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
    cache.mFbo = sDrawFbo;
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

VkPrimitiveTopology topology(LLVkFormat::Topology t)
{
    switch (t)
    {
    case LLVkFormat::TOP_POINTS: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case LLVkFormat::TOP_LINES: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case LLVkFormat::TOP_LINE_STRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case LLVkFormat::TOP_TRIANGLES: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case LLVkFormat::TOP_TRIANGLE_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case LLVkFormat::TOP_TRIANGLE_FAN: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    default: warn_once("primitive mode", t); return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

VkSamplerAddressMode wrap_mode(LLVkFormat::Wrap w)
{
    switch (w)
    {
    case LLVkFormat::W_CLAMP_EDGE: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case LLVkFormat::W_MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case LLVkFormat::W_CLAMP_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

SamplerDesc sampler_for(const Tex& t)
{
    const SamplerParams& sp = t.mSampler;
    SamplerDesc d;
    d.mMag = sp.mMag == LLVkFormat::F_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    bool mipmapped = true;
    switch (sp.mMin)
    {
    case LLVkFormat::F_NEAREST: d.mMin = VK_FILTER_NEAREST; mipmapped = false; break;
    case LLVkFormat::F_LINEAR: d.mMin = VK_FILTER_LINEAR; mipmapped = false; break;
    case LLVkFormat::F_NEAREST_MIP_NEAREST: d.mMin = VK_FILTER_NEAREST; d.mMip = VK_SAMPLER_MIPMAP_MODE_NEAREST; break;
    case LLVkFormat::F_LINEAR_MIP_NEAREST: d.mMin = VK_FILTER_LINEAR; d.mMip = VK_SAMPLER_MIPMAP_MODE_NEAREST; break;
    case LLVkFormat::F_NEAREST_MIP_LINEAR: d.mMin = VK_FILTER_NEAREST; d.mMip = VK_SAMPLER_MIPMAP_MODE_LINEAR; break;
    default: d.mMin = VK_FILTER_LINEAR; d.mMip = VK_SAMPLER_MIPMAP_MODE_LINEAR; break;
    }
    d.mAddressU = wrap_mode(sp.mWrapS);
    d.mAddressV = wrap_mode(sp.mWrapT);
    d.mAddressW = wrap_mode(sp.mWrapR);
    d.mAnisotropy = sp.mAnisotropy;
    d.mCompare = sp.mCompare;
    d.mCompareOp = (VkCompareOp)sp.mCompareFunc;        // LLVkFormat::Compare has the order of VkCompareOp
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
    mix(sp.mMin); mix(sp.mMag); mix(sp.mWrapS); mix(sp.mWrapT); mix(sp.mWrapR); mix(sp.mCompare ? 1 : 0); mix(sp.mCompareFunc);
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

// <FS> F1.1: the render state comes from the typed block the producer wrote (LLVkState), not from a replayed GL state machine
const LLVkState::Block* state_block()
{
    if (!LLVkAsync::gEnabled)
    {
        // inline mode (before the queue starts): the producer and this code are one thread; the GL hooks wrote the context directly
        LLVkState::Block b;
        if (LLVkState::takeChanged(b))
        {
            LLVkState::receive(b);
        }
    }
    return LLVkState::received();
}

// translates the state block into renderer state for the next draw
void sync_draw_state(Renderer& r, bool has_depth)
{
    const LLVkState::Block* blk = state_block();
    if (!blk)
    {
        return;
    }
    // the translation is a function of the block, has_depth and the renderer's own state generation: when none changed it is skipped
    static LLVkState::Block last;
    static U32 last_gen = 0;
    static bool last_has_depth = false;
    static bool last_valid = false;
    if (!sDbgMatch && last_valid && last_gen == r.stateGeneration() && last_has_depth == has_depth && last == *blk)
    {
        return;
    }
    RasterState rs;
    DepthStencilState ds;
    BlendState bs;
    LLVkState::toRenderer(*blk, has_depth, rs, ds, bs);
    if (sDbgMatch)
    {
        if (sDbgNoCull) rs.mCull = VK_CULL_MODE_NONE;
        if (sDbgNoDepthWrite) ds.mDepthWrite = false;
        if (sDbgNoDepthTest) ds.mDepthTest = false;
        if (sDbgNoBlend) bs.mEnable = false;
        if (sDbgFullMask) bs.mWriteMask = 0xF;
    }
    r.setRasterState(rs);
    r.setDepthStencilState(ds);
    r.setBlendState(bs);

    if (blk->mViewportSet)
    {
        r.setViewport(blk->mViewport[0], blk->mViewport[1], blk->mViewport[2], blk->mViewport[3]);
    }
    const bool scissor_on = blk->mScissorOn && !(sDbgMatch && sDbgNoScissor);
    r.setScissorEnabled(scissor_on);
    if (scissor_on)
    {
        r.setScissor(blk->mScissor[0], blk->mScissor[1], blk->mScissor[2], blk->mScissor[3]);
    }
    last = *blk;
    last_gen = r.stateGeneration();
    last_has_depth = has_depth;
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
        Tex* tex = tex_ptr(sSlots[unit][cls]);
        if (sStatsEnabled)
        {
            const std::string& type = info.mSamplers[k].mType;
            static const char* const watch[] = { "Soften", "Windlight Sky", "Deferred Sun", "Deferred Light Shader", "Irradiance Gen", "Radiance Gen", "HUD", "Copy Depth", "Haze", "Tonemap", "Reflection Mip", "Glow" };
            bool wanted = false;
            for (const char* w : watch) if (program.getName().find(w) != std::string::npos) wanted = true;
            if (wanted)
            {
                std::string what = "unit " + std::to_string(unit) + " tex " + std::to_string(sSlots[unit][cls]) + " ";
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
                          + " wrap " + std::to_string((S32)sd.mAddressU) + "," + std::to_string((S32)sd.mAddressV) + " glminfilter " + std::to_string((S32)tx.mSampler.mMin);
                }
                sStats.mSamplerInfo[program.getName() + " :: " + info.mSamplers[k].mName + " (" + type + ")"] = what;
            }
        }
        if (tex && tex->mImage.valid())
        {
            r.setTexture(unit, &tex->mImage, texture_sampler(r, *tex));
            if (sStatsEnabled)
            {
                const U32 tname = sSlots[unit][cls];
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
VkFormat vertex_format(S32 size, U8 type, bool normalized, bool integer, char numeric_class)
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
        case LLVkFormat::T_S8: case LLVkFormat::T_U8: return sign ? b8s[n - 1] : b8u[n - 1];
        case LLVkFormat::T_S16: case LLVkFormat::T_U16: return sign ? b16s[n - 1] : b16u[n - 1];
        default: return sign ? b32s[n - 1] : b32u[n - 1];
        }
    }
    switch (type)
    {
    case LLVkFormat::T_F32:
    {
        const VkFormat f[4] = { VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT };
        return f[n - 1];
    }
    case LLVkFormat::T_F16:
    {
        const VkFormat f[4] = { VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT };
        return f[n - 1];
    }
    case LLVkFormat::T_U8:
    {
        const VkFormat un[4] = { VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };
        const VkFormat ui[4] = { VK_FORMAT_R8_UINT, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8A8_UINT };
        const VkFormat us[4] = { VK_FORMAT_R8_USCALED, VK_FORMAT_R8G8_USCALED, VK_FORMAT_R8G8B8_USCALED, VK_FORMAT_R8G8B8A8_USCALED };
        return integer ? ui[n - 1] : (normalized ? un[n - 1] : us[n - 1]);
    }
    case LLVkFormat::T_S8:
    {
        const VkFormat sn[4] = { VK_FORMAT_R8_SNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8A8_SNORM };
        const VkFormat si[4] = { VK_FORMAT_R8_SINT, VK_FORMAT_R8G8_SINT, VK_FORMAT_R8G8B8_SINT, VK_FORMAT_R8G8B8A8_SINT };
        return integer ? si[n - 1] : sn[n - 1];
    }
    case LLVkFormat::T_U16:
    {
        const VkFormat un[4] = { VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16A16_UNORM };
        const VkFormat ui[4] = { VK_FORMAT_R16_UINT, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16A16_UINT };
        const VkFormat us[4] = { VK_FORMAT_R16_USCALED, VK_FORMAT_R16G16_USCALED, VK_FORMAT_R16G16B16_USCALED, VK_FORMAT_R16G16B16A16_USCALED };
        return integer ? ui[n - 1] : (normalized ? un[n - 1] : us[n - 1]);
    }
    case LLVkFormat::T_S16:
    {
        const VkFormat sn[4] = { VK_FORMAT_R16_SNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16A16_SNORM };
        const VkFormat si[4] = { VK_FORMAT_R16_SINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16B16_SINT, VK_FORMAT_R16G16B16A16_SINT };
        const VkFormat ss[4] = { VK_FORMAT_R16_SSCALED, VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R16G16B16_SSCALED, VK_FORMAT_R16G16B16A16_SSCALED };
        return integer ? si[n - 1] : (normalized ? sn[n - 1] : ss[n - 1]);
    }
    case LLVkFormat::T_U32:
    {
        const VkFormat ui[4] = { VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT };
        return ui[n - 1];
    }
    case LLVkFormat::T_S32:
    {
        const VkFormat si[4] = { VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32A32_SINT };
        return si[n - 1];
    }
    default:
        warn_once("vertex attribute type", type);
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

U32 attrib_type_bytes(U8 type)
{
    switch (type)
    {
    case LLVkFormat::T_S8: case LLVkFormat::T_U8: return 1;
    case LLVkFormat::T_S16: case LLVkFormat::T_U16: case LLVkFormat::T_F16: return 2;
    default: return 4;
    }
}

// Builds the vertex input for the current VAO and the program's attributes.
bool build_vertex_input(Renderer& r, Program& program, VertexInput& vi, std::vector<U32>& used_buffers)
{
    const VertexInputState& vao = sVin;
    const ProgramInfo& info = program.getInfo();
    // bindings made so far: buffer, stride, byte offset of the binding inside the buffer
    struct BindingKey { U32 mBuffer; S32 mStride; U64 mBase; U32 mBinding; };
    BindingKey made[16];
    U32 made_count = 0;
    // <FS> the attributes of a draw mostly come from one buffer: it is looked up once, and marked as used right here (mark_used used to look every buffer up again)
    U32 last_name = 0;
    auto last_it = sGL.mBuffers.end();
    const U64 frame_now = Context::instance().getFrameNumber();

    for (size_t a = 0; a < info.mAttributes.size(); ++a)
    {
        const U32 loc = info.mAttributeLocations[a];
        if (loc >= 16)
        {
            continue;
        }
        const VaoAttrib& at = vao.mAttrib[loc];
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
            const S32 stride = at.mStride ? at.mStride : (S32)(tb * (U32)at.mSize);
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
void sync_uniform_blocks(Renderer& r, std::vector<U32>& used_buffers)
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
bool prepare_draw(VertexInput& vi, std::vector<U32>& used_buffers)
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
        if (sDrawFbo == 0) key += "SCREEN";
        for (U32 tname : sCurTargets) key += std::to_string(tname) + " ";
        key += "<- ";
        const ProgramInfo& gi = program->getInfo();
        for (size_t k = 0; k < gi.mSamplers.size(); ++k)
        {
            const U32 unit = program->getSamplerUnit(k);
            if (unit >= 32) continue;
            U32 bound = 0;
            for (U32 c = 0; c < 8 && !bound; ++c)
            {
                const std::string& ty = gi.mSamplers[k].mType;
                const bool cube = ty.find("Cube") != std::string::npos, arr = ty.find("Array") != std::string::npos;
                const U32 want = ty.find("CubeArray") != std::string::npos ? 2 : cube ? 1 : arr ? 3 : ty.find("3D") != std::string::npos ? 4 : 0;
                bound = sSlots[unit][want];
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
    if (sStatsEnabled && sDrawFbo == 0 && sStats.mBackbufferEvents.size() < 4000)
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
                e += " [" + pi.mSamplers[k].mName + "=" + std::to_string(sSlots[unit][cls]) + "]";
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
std::vector<U32> sPendingBufs;       // the buffers whose Buf::mPending is set
U64 sBufferTopology = 0;                // see Buf::mRtWatched
std::vector<unsigned> sTopologyNames;  // the names behind the count (cleared when taken; sTopologyOverflow: too many)
bool sTopologyOverflow = false;
void note_topology_change(const Buf& b)
{
    ++sBufferTopology;
    if (sTopologyNames.size() < 4096)
    {
        sTopologyNames.push_back(b.mRtName);
    }
    else
    {
        sTopologyOverflow = true;
    }
}

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
    for (U32 n : sPendingBufs)
    {
        auto it = sGL.mBuffers.find(n);
        if (it != sGL.mBuffers.end()) it->second.mPending = false;
    }
    sPendingBufs.clear();
}

// called before a draw is recorded: if it reads a buffer with an unrecorded write, record the writes first
void ensure_buffers_current(const std::vector<U32>& used, U32 element)
{
    if (sPendingBufs.empty())
    {
        return;
    }
    bool stale = false;
    for (U32 n : used)
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
    if (b.mRtWatched)
    {
        note_topology_change(b);
    }
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
    if (b.mRtWatched)
    {
        note_topology_change(b);
    }
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
void mark_used(const std::vector<U32>& buffers, U32 element)
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
std::unordered_map<U32, WriterStat> sWriters;
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

void write_buffer(U32 name, Buf& b, U64 offset, U64 size, const void* data, bool whole)
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
                if (b.mRtWatched)
                {
                    note_topology_change(b);
                }
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

void create_textures(S32 n, U32* textures)
{
    for (S32 i = 0; i < n; ++i)
    {
        sGL.mTextures[textures[i]];
    }
}

static U64 sPacketTextureGeneration = 0;     // <FS> bumped when a texture entry goes: the packet records' lookup cache starts over

void destroy_textures(S32 n, const U32* textures)
{
    for (S32 i = 0; i < n; ++i)
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
        for (auto& unit : sSlots)
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

void slot_bind(unsigned unit, unsigned dim, unsigned name)
{
    if (unit < 32 && dim < 8)
    {
        sSlots[unit][dim] = name;
        if (name)
        {
            sGL.mTextures[name];     // a name that was never generated is still a texture (the first edit makes it)
        }
    }
}


void edit_image2D(LLVkFormat::Target tg, S32 level, LLVkFormat::Store st, S32 width, S32 height, LLVkFormat::Transfer tr, const void* pixels)
{
    Tex* t = edit_texture();
    if (!t || width <= 0 || height <= 0)
    {
        return;
    }
    if (level == 0 && !ensure_image(*t, tg, st, (U32)width, (U32)height, tg.mDim == LLVkFormat::DIM_CUBE ? 6 : 1))
    {
        return;
    }
    if (level == 0 && !pixel_source(pixels) && t->mImage.valid() && t->mImage.mLayout == VK_IMAGE_LAYOUT_UNDEFINED)
    {
        t->mImage.mZeroOnFirstUse = true;       // allocation only: GL would hand back zeros
    }
    upload_level(*t, tg, level, 0, 0, 0, (U32)width, (U32)height, 1, tr, pixels);
}

void edit_image3D(LLVkFormat::Target tg, S32 level, LLVkFormat::Store st, S32 width, S32 height, S32 depth, LLVkFormat::Transfer tr, const void* pixels)
{
    Tex* t = edit_texture();
    if (!t || width <= 0 || height <= 0 || depth <= 0)
    {
        return;
    }
    if (level == 0 && !ensure_image(*t, tg, st, (U32)width, (U32)height, (U32)depth))
    {
        return;
    }
    if (level == 0 && !pixel_source(pixels) && t->mImage.valid() && t->mImage.mLayout == VK_IMAGE_LAYOUT_UNDEFINED)
    {
        t->mImage.mZeroOnFirstUse = true;
    }
    upload_level(*t, tg, level, 0, 0, 0, (U32)width, (U32)height, (U32)depth, tr, pixels);
}

void edit_subimage2D(LLVkFormat::Target tg, S32 level, S32 xoffset, S32 yoffset, S32 width, S32 height, LLVkFormat::Transfer tr, const void* pixels)
{
    Tex* t = edit_texture();
    if (t)
    {
        upload_level(*t, tg, level, xoffset, yoffset, 0, (U32)width, (U32)height, 1, tr, pixels);
    }
}

void edit_subimage3D(LLVkFormat::Target tg, S32 level, S32 xoffset, S32 yoffset, S32 zoffset, S32 width, S32 height, S32 depth, LLVkFormat::Transfer tr, const void* pixels)
{
    Tex* t = edit_texture();
    if (t)
    {
        upload_level(*t, tg, level, xoffset, yoffset, zoffset, (U32)width, (U32)height, (U32)depth, tr, pixels);
    }
}

void edit_compressed2D(LLVkFormat::Target tg, S32 level, LLVkFormat::Store st, S32 width, S32 height, S32 imageSize, const void* data)
{
    Tex* t = edit_texture();
    if (!t || width <= 0 || height <= 0)
    {
        return;
    }
    if (level == 0 && !ensure_image(*t, tg, st, (U32)width, (U32)height, tg.mDim == LLVkFormat::DIM_CUBE ? 6 : 1))
    {
        return;
    }
    const void* src = pixel_source(data);
    if (src && t->mImage.valid() && level > 0 && (U32)level >= t->mLevels)
    {
        grow_levels(*t);
    }
    if (!src || !t->mImage.valid() || level < 0 || (U32)level >= t->mLevels)
    {
        return;
    }
    const U32 layer = tg.mFace >= 0 ? (U32)tg.mFace : 0;
    Renderer::instance().uploadTexture(t->mImage, (U32)level, layer, 0, 0, (U32)width, (U32)height, 1, src, (VkDeviceSize)imageSize);
}

// one sampler / level / swizzle setting of the texture being edited
void edit_setting(const LLVkFormat::Setting& st)
{
    using namespace LLVkFormat;
    Tex* t = edit_texture();
    if (!t)
    {
        return;
    }
    SamplerParams& sp = t->mSampler;
    switch (st.mParam)
    {
    case P_MIN_FILTER: sp.mMin = (Filter)st.mValue; break;
    case P_MAG_FILTER: sp.mMag = (Filter)st.mValue; break;
    case P_WRAP_S: sp.mWrapS = (Wrap)st.mValue; break;
    case P_WRAP_T: sp.mWrapT = (Wrap)st.mValue; break;
    case P_WRAP_R: sp.mWrapR = (Wrap)st.mValue; break;
    case P_COMPARE_MODE: sp.mCompare = st.mValue != 0; break;
    case P_COMPARE_FUNC: sp.mCompareFunc = (Compare)st.mValue; break;
    case P_ANISOTROPY: sp.mAnisotropy = st.mFloat; break;
    case P_BASE_LEVEL: sp.mBaseLevel = st.mInt; break;
    case P_MAX_LEVEL: sp.mMaxLevel = st.mInt; break;
    case P_SWIZZLE_R: case P_SWIZZLE_G: case P_SWIZZLE_B: case P_SWIZZLE_A:
        t->mSwizzle[st.mParam - P_SWIZZLE_R] = (Swizzle)st.mValue;
        t->mSwizzleSet = true;
        rebuild_sampling_view(*t);
        break;
    default: break;
    }
}

void edit_mipmap()
{
    Tex* t = edit_texture();
    if (t && t->mImage.valid() && grow_levels(*t))
    {
        Renderer::instance().generateMipmaps(t->mImage);
        t->mValidLevels = t->mLevels;
    }
}

static void copy_to_texture(LLVkFormat::Target tg, S32 level, S32 xoffset, S32 yoffset, S32 zoffset, S32 x, S32 y, S32 width, S32 height, U32 source_fbo)
{
    Tex* t = edit_texture();
    Image* src = read_image(source_fbo);
    if (!t || !t->mImage.valid() || !src || !LLVkMirror::inFrame())
    {
        return;
    }
    if (level > 0 && !grow_levels(*t))
    {
        return;
    }
    U32 layer = 0;
    if (tg.mFace >= 0) layer = (U32)tg.mFace;
    else if (t->mDim == LLVkFormat::DIM_2D_ARRAY || t->mDim == LLVkFormat::DIM_CUBE_ARRAY) layer = (U32)zoffset;
    Renderer::instance().copyRegion(*src, x, y, t->mImage, (U32)level, layer, xoffset, yoffset, width, height);
    ++sCopies;
}



void copy_image_2d(LLVkFormat::Target tg, S32 level, LLVkFormat::Store st, S32 x, S32 y, S32 width, S32 height, U32 source_fbo)
{
    Tex* t = edit_texture();
    if (!t || width <= 0 || height <= 0)
    {
        return;
    }
    if (level == 0 && !ensure_image(*t, tg, st, (U32)width, (U32)height, 1))
    {
        return;
    }
    copy_to_texture(tg, level, 0, 0, 0, x, y, width, height, source_fbo);
}

//-----------------------------------------------------------------------------------------------------
// F1.2: texture edits that name their texture (LLVkTex). The scope replaces "the texture bound on the active unit".
//-----------------------------------------------------------------------------------------------------

namespace
{
struct EditScope
{
    U32 mPrev;
    const LLVkTex::PixelStore* mPrevPS;
    EditScope(U32 id, const LLVkTex::PixelStore* ps = nullptr) : mPrev(tlsEditTex), mPrevPS(tlsEditPS)
    {
        if (id)
        {
            sGL.mTextures[id];      // the object is made on first use
        }
        tlsEditTex = id;
        tlsEditPS = ps ? ps : tlsEditPS;
    }
    ~EditScope() { tlsEditTex = mPrev; tlsEditPS = mPrevPS; }
};
}

void tex_image2D(unsigned id, const LLVkTex::PixelStore& ps, LLVkFormat::Target tg, int level, LLVkFormat::Store st, int width, int height, LLVkFormat::Transfer tr, const void* pixels)
{
    EditScope s(id, &ps);
    edit_image2D(tg, level, st, width, height, tr, pixels);
}

void tex_image3D(unsigned id, const LLVkTex::PixelStore& ps, LLVkFormat::Target tg, int level, LLVkFormat::Store st, int width, int height, int depth, LLVkFormat::Transfer tr, const void* pixels)
{
    EditScope s(id, &ps);
    edit_image3D(tg, level, st, width, height, depth, tr, pixels);
}

void tex_subimage2D(unsigned id, const LLVkTex::PixelStore& ps, LLVkFormat::Target tg, int level, int xoffset, int yoffset, int width, int height, LLVkFormat::Transfer tr, const void* pixels)
{
    EditScope s(id, &ps);
    edit_subimage2D(tg, level, xoffset, yoffset, width, height, tr, pixels);
}

void tex_subimage3D(unsigned id, const LLVkTex::PixelStore& ps, LLVkFormat::Target tg, int level, int xoffset, int yoffset, int zoffset, int width, int height, int depth, LLVkFormat::Transfer tr, const void* pixels)
{
    EditScope s(id, &ps);
    edit_subimage3D(tg, level, xoffset, yoffset, zoffset, width, height, depth, tr, pixels);
}

void tex_compressed2D(unsigned id, const LLVkTex::PixelStore& ps, LLVkFormat::Target tg, int level, LLVkFormat::Store st, int width, int height, int imageSize, const void* data)
{
    EditScope s(id, &ps);
    edit_compressed2D(tg, level, st, width, height, imageSize, data);
}

void tex_params(unsigned id, const LLVkFormat::Setting* settings, unsigned count)
{
    EditScope s(id);
    for (unsigned i = 0; i < count; ++i)
    {
        edit_setting(settings[i]);
    }
}

void tex_swizzle(unsigned id, const LLVkFormat::Swizzle* rgba)
{
    EditScope s(id);
    Tex* t = edit_texture();
    if (t)
    {
        for (U32 i = 0; i < 4; ++i)
        {
            t->mSwizzle[i] = rgba[i];
        }
        t->mSwizzleSet = true;
        rebuild_sampling_view(*t);
    }
}

void tex_mipmap(unsigned id)
{
    EditScope s(id);
    edit_mipmap();
}

void tex_copy_sub(unsigned id, LLVkFormat::Target tg, int level, int xoffset, int yoffset, int zoffset, int x, int y, int width, int height, unsigned source)
{
    EditScope s(id);
    copy_to_texture(tg, level, xoffset, yoffset, zoffset, x, y, width, height, source);
}

void tex_copy_image(unsigned id, LLVkFormat::Target tg, int level, LLVkFormat::Store st, int x, int y, int width, int height, unsigned source)
{
    EditScope s(id);
    copy_image_2d(tg, level, st, x, y, width, height, source);
}

//-----------------------------------------------------------------------------------------------------
// hooks: framebuffers
//-----------------------------------------------------------------------------------------------------

void create_targets(S32 n, U32* framebuffers)
{
    for (S32 i = 0; i < n; ++i)
    {
        sGL.mFbos[framebuffers[i]];
    }
}

void destroy_targets(S32 n, const U32* framebuffers)
{
    for (S32 i = 0; i < n; ++i)
    {
        sGL.mFbos.erase(framebuffers[i]);
        ++sTargetEpoch;
        if (sDrawFbo == framebuffers[i]) sDrawFbo = 0;
    }
}

void target_attach(unsigned fbo_name, unsigned slot, unsigned texture)
{
    if (!fbo_name)
    {
        return;
    }
    ++sTargetEpoch;
    Fbo& fbo = sGL.mFbos[fbo_name];
    FboAttachment a;
    a.mTexture = texture;
    a.mLevel = 0;
    a.mLayer = 0;
    if (slot < 8)
    {
        fbo.mColor[slot] = a;
    }
    else if (slot == LLVkTarget::DEPTH_SLOT)
    {
        fbo.mDepth = a;
    }
}

void target_state(unsigned fbo_name, int draw_buffers)
{
    sDrawFbo = fbo_name;
    ++sTargetEpoch;
    if (!fbo_name)
    {
        return;
    }
    Fbo& fbo = sGL.mFbos[fbo_name];
    if (draw_buffers > 0)
    {
        fbo.mDrawCount = (U32)std::min(draw_buffers, 4);
    }
    else if (draw_buffers == 0)
    {
        fbo.mDrawCount = 0;         // depth only
    }
}

//-----------------------------------------------------------------------------------------------------
// hooks: buffers and vertex arrays
//-----------------------------------------------------------------------------------------------------

void create_buffers(S32 n, U32* buffers)
{
    for (S32 i = 0; i < n; ++i)
    {
        sGL.mBuffers[buffers[i]];
    }
}

static U64 sPacketBufferGeneration = 0;      // <FS> bumped when a buffer entry goes: the packet records' lookup cache starts over

void destroy_buffers(S32 n, const U32* buffers)
{
    Context& ctx = Context::instance();
    for (S32 i = 0; i < n; ++i)
    {
        auto it = sGL.mBuffers.find(buffers[i]);
        if (it != sGL.mBuffers.end())
        {
            release_storage(it->second);
            buf_forget(it->first);
            sGL.mBuffers.erase(it);
            ++sPacketBufferGeneration;
        }
        if (sVin.mArrayBuffer == buffers[i]) sVin.mArrayBuffer = 0;
        if (sVin.mElementBuffer == buffers[i]) sVin.mElementBuffer = 0;
        if (sCtx.mPixelUnpackBuffer == buffers[i]) sCtx.mPixelUnpackBuffer = 0;
        if (sCtx.mPixelPackBuffer == buffers[i]) sCtx.mPixelPackBuffer = 0;
    }
}

void on_bufferData(U32 name, unsigned kind, S64 size, const void* data);
void on_bufferSubData(U32 name, S64 offset, S64 size, const void* data);

void on_bufferData(U32 name, unsigned kind, S64 size, const void* data)
{
    auto it = sGL.mBuffers.find(name);
    if (it == sGL.mBuffers.end() || size < 0)
    {
        return;
    }
    Buf& b = it->second;
    Context& ctx = Context::instance();
    // large vertex / index buffers take the device path (GPU copies, no CPU access, no shadow copy)
    const bool want_device = (kind == LLVkFormat::BK_VERTEX || kind == LLVkFormat::BK_INDEX) && (U64)size >= device_buffer_min_bytes() && !getenv("LL_VK_NO_DEVICE_BUFFERS");
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
    const bool geometry = kind == LLVkFormat::BK_VERTEX || kind == LLVkFormat::BK_INDEX;
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
void on_bufferRecycle(U32 name)
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
    // the system-memory copy (reading the mapped video memory from the CPU is very slow); only when it covers the whole buffer
    out.mMapped = b.mArena.valid() ? b.mArena.mMapped : nullptr;
    out.mCpu = (!b.mShadow.empty() && b.mShadow.size() >= b.mSize) ? b.mShadow.data() : nullptr;
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

void flushPendingBufferWrites()
{
    flush_buffer_updates();
}

unsigned long long bufferTopologyVersion()
{
    return sBufferTopology;
}

bool takeTopologyChanges(std::vector<unsigned>& names)
{
    names.swap(sTopologyNames);
    sTopologyNames.clear();
    const bool complete = !sTopologyOverflow;
    sTopologyOverflow = false;
    return complete;
}

void on_bufferSubData(U32 name, S64 offset, S64 size, const void* data)
{
    auto it = sGL.mBuffers.find(name);
    if (it != sGL.mBuffers.end() && data && size > 0 && offset >= 0)
    {
        write_buffer(name, it->second, (U64)offset, (U64)size, data, false);
    }
}

// F1.3: a buffer becomes the contents of a numbered uniform block
void uniform_block_bind(unsigned index, unsigned buffer)
{
    if (buffer)
    {
        sGL.mBuffers[buffer];
    }
    if (index < 4)
    {
        sUboBinding[index] = buffer;
    }
}

void vertex_input_apply(bool bind_array, unsigned array_buffer, bool bind_elem, unsigned elem_buffer, const LLVkAsync::VertexAttrib* attribs, unsigned count)
{
    if (bind_array)
    {
        sVin.mArrayBuffer = array_buffer;
        if (array_buffer)
        {
            sGL.mBuffers[array_buffer];
        }
    }
    for (unsigned i = 0; i < count; ++i)
    {
        const LLVkAsync::VertexAttrib& e = attribs[i];
        if (e.mIndex >= 16)
        {
            continue;
        }
        VaoAttrib& a = sVin.mAttrib[e.mIndex];
        a.mBuffer = sVin.mArrayBuffer;
        a.mSize = e.mSize;
        a.mType = e.mType;
        a.mNormalized = e.mKind == 0 && e.mNormalized != 0;
        a.mInteger = e.mKind == 1;
        a.mStride = e.mStride;
        a.mOffset = (U64)e.mPointer;
    }
    if (bind_elem)
    {
        sVin.mElementBuffer = elem_buffer;
        if (elem_buffer)
        {
            sGL.mBuffers[elem_buffer];
        }
    }
}

void shader_bind(void* program, unsigned enable_mask, unsigned disable_mask)
{
    sVin.mArrayBuffer = 0;
    sVin.mElementBuffer = 0;
    if (program && LLVkMirror::isReady())
    {
        Renderer::instance().setProgram((Program*)program);
    }
    for (unsigned i = 0; i < 16; ++i)
    {
        if (enable_mask & (1u << i)) sVin.mAttrib[i].mEnabled = true;
        if (disable_mask & (1u << i)) sVin.mAttrib[i].mEnabled = false;
    }
}

void constant_attribute(unsigned location, float x, float y, float z, float w)
{
    if (location < 16)
    {
        sGL.mConstAttrib[location][0] = x; sGL.mConstAttrib[location][1] = y; sGL.mConstAttrib[location][2] = z; sGL.mConstAttrib[location][3] = w;
    }
}

//-----------------------------------------------------------------------------------------------------
// hooks: draws
//-----------------------------------------------------------------------------------------------------

namespace
{
bool probe_on();
void probe_note(U32 vb_name, U32 ib_name, const Buf& vb, const Buf& ib, U32 first, U32 count, U32 max_vertex);
}

void draw_arrays(unsigned topo, int first, int count)
{
    HookTimer hook_timer;
    LLVkMirror::PhaseTimer draw_phase(LLVkMirror::PH_DRAW);
    static thread_local VertexInput vi_cache;
    static thread_local std::vector<U32> used_cache;
    VertexInput& vi = vi_cache;
    std::vector<U32>& used = used_cache;
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
    r.setTopology(topology((LLVkFormat::Topology)topo));
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

void draw_elements(unsigned topo, unsigned start, unsigned end, int count, unsigned type, const void* indices)
{
    HookTimer hook_timer;
    LLVkMirror::PhaseTimer draw_phase(LLVkMirror::PH_DRAW);
    static thread_local VertexInput vi_cache;
    static thread_local std::vector<U32> used_cache;
    VertexInput& vi = vi_cache;
    std::vector<U32>& used = used_cache;
    vi.mBindings.clear();
    vi.mAttributes.clear();
    used.clear();
    if (count <= 0 || !prepare_draw(vi, used))
    {
        return;
    }
    const VertexInputState& vao = sVin;
    auto eit = sGL.mBuffers.find(vao.mElementBuffer);
    if (eit == sGL.mBuffers.end() || !eit->second.mVk.valid())
    {
        ++sDrawsSkipped;
        ++sStats.mNoElement;
        ++sStats.mSkippedPerProgram[Renderer::instance().getProgram()->getName() + " (no index buffer)"];
        return;
    }
    const VkIndexType itype = type == LLVkFormat::T_U32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    const U32 index_bytes = type == LLVkFormat::T_U32 ? 4 : 2;
    Renderer& r = Renderer::instance();
    if (probe_on() && type == LLVkFormat::T_U16)
    {
        // BLAS probe: positions are attribute 0 (vec4 array at the start of the vertex buffer); rigged draws (weights, joints enabled) and the UI are left out
        const VaoAttrib& pos = vao.mAttrib[0];
        auto vit = sGL.mBuffers.find(pos.mBuffer);
        if (pos.mEnabled && pos.mBuffer && vit != sGL.mBuffers.end() && pos.mType == LLVkFormat::T_F32 && pos.mSize >= 3 && pos.mOffset == 0 && (pos.mStride == 16 || (pos.mStride == 0 && pos.mSize == 4))
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
    r.setTopology(topology((LLVkFormat::Topology)topo));
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
static Buf* packet_buffer(U32 name)
{
    static Buf* cache_buf[1024];
    static U32 cache_name[1024];
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

static Tex* packet_texture(U32 name)
{
    static Tex* cache_tex[8192];
    static U32 cache_name[8192];
    static U64 cache_generation = ~0ull;
    if (cache_generation != sPacketTextureGeneration)
    {
        memset(cache_tex, 0, sizeof(cache_tex));
        memset(cache_name, 0, sizeof(cache_name));
        cache_generation = sPacketTextureGeneration;
    }
    const U32 slot = (name * 2654435761u) >> 19;
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
static U32 packet_texture_slot(Renderer& r, U32 name)
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
    // the slot this texture held last time is tried first: slots stay with their texture from frame to frame (the table keeps them), the table only checks that the slot still holds this
    // view and sampler and that this frame's set has it written
    const U32 slot = r.bindlessSlotIn(0, &t->mImage, texture_sampler(r, *t), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, t->mBindlessSlot);
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
    U32 mVb = 0, mIb = 0;
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
void probe_note(U32 vb_name, U32 ib_name, const Buf& vb, const Buf& ib, U32 first, U32 count, U32 max_vertex)
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
static void buffer_info_of(const Buf& b, BufferInfo& out)
{
    out = BufferInfo();
    out.mAddress = b.mArena.valid() ? (unsigned long long)b.mVk.mAddress : 0;
    out.mSize = b.mSize;
    out.mEpoch = b.mEpoch;
    out.mVersion = b.mVersion;
    out.mMoves = b.mMoves;
    out.mDevice = b.mDevicePath;
    out.mMapped = b.mArena.valid() ? b.mArena.mMapped : nullptr;
    out.mCpu = (!b.mShadow.empty() && b.mShadow.size() >= b.mSize) ? b.mShadow.data() : nullptr;
    if (!b.mVk.valid())
    {
        out.mAddress = 0;
    }
}

static void note_rt_caster(U32 pass, U32 vb, U32 ib, const Buf& vbuf, const Buf& ibuf, U32 first, U32 count, U32 matrix_id, U32 kind, const F32* model, const Rt::AlphaInfo* alpha = nullptr)
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
    if (kind == 1)
    {
        // the alpha test reads these buffers while the lighting runs: watched for moves, and in use this frame (a CPU rewrite of a host-written range has to rename it first)
        const U64 frame_now = Context::instance().getFrameNumber();
        Buf& vb_rw = const_cast<Buf&>(vbuf);
        Buf& ib_rw = const_cast<Buf&>(ibuf);
        vb_rw.mRtWatched = ib_rw.mRtWatched = true;
        vb_rw.mRtName = vb;
        ib_rw.mRtName = ib;
        vb_rw.mLastUseFrame = ib_rw.mLastUseFrame = frame_now;
    }
    buffer_info_of(vbuf, c.mVi);
    buffer_info_of(ibuf, c.mIi);
    Rt::noteCaster(c);
}

// a rigged caster draw for the ray scene: skinned on the GPU into a copy of its vertices (Rt::noteSkinned)
static void note_rt_skinned(U32 pass, const LLVkMirror::ShadowSkinEntry& e, const Buf& vbuf, const Buf& ibuf, U64 palette_address, const F32* model)
{
    Rt::SkinCaster c;
    c.mAvatarId = e.mAvatarId;
    c.mSkinId = e.mSkinId;
    c.mVb = e.mVertexBuffer;
    c.mIb = e.mIndexBuffer;
    c.mFirst = e.mFirstIndex;
    c.mCount = e.mIndexCount;
    c.mVertexFirst = e.mVertexFirst;
    c.mVertexCount = e.mVertexCount;
    c.mWeightOffset = e.mWeightOffset;
    c.mJoints = e.mJoints;
    c.mPalette = palette_address;
    c.mModel = model;
    c.mPass = pass;
    memcpy(c.mAvatarPos, e.mAvatarPos, sizeof(c.mAvatarPos));
    c.mAvatarKind = e.mAvatarKind;
    buffer_info_of(vbuf, c.mVi);
    buffer_info_of(ibuf, c.mIi);
    Rt::noteSkinned(c);
}

// the same for a rigged alpha masked draw: the structure of its avatar gets a geometry with the alpha test
static void note_rt_skinned_alpha(U32 pass, const LLVkMirror::ShadowAlphaEntry& e, const Buf& vbuf, const Buf& ibuf, U64 palette_address, const F32* model)
{
    Rt::AlphaInfo alpha;
    alpha.mFlags = e.mFlags & 31u;
    alpha.mTexcoordOffset = e.mTexcoordOffset;
    alpha.mColorOffset = e.mColorOffset;
    memcpy(alpha.mTextures, e.mTextures, sizeof(alpha.mTextures));
    alpha.mCutoff = e.mCutoff;
    Rt::SkinCaster c;
    c.mAvatarId = e.mAvatarId;
    c.mSkinId = e.mSkinId;
    c.mVb = e.mVertexBuffer;
    c.mIb = e.mIndexBuffer;
    c.mFirst = e.mFirstIndex;
    c.mCount = e.mIndexCount;
    c.mVertexFirst = e.mVertexFirst;
    c.mVertexCount = e.mVertexCount;
    c.mWeightOffset = e.mWeightOffset;
    c.mJoints = e.mJoints;
    c.mPalette = palette_address;
    c.mModel = model;
    c.mPass = pass;
    memcpy(c.mAvatarPos, e.mAvatarPos, sizeof(c.mAvatarPos));
    c.mAvatarKind = e.mAvatarKind;
    c.mKind = 1;
    c.mAlpha = &alpha;
    buffer_info_of(vbuf, c.mVi);
    buffer_info_of(ibuf, c.mIi);
    Rt::noteSkinned(c);
}

// the worker side of a record of casters the old path draws (kind 5): nothing is drawn, the ray scene gets them (opaque, or with the alpha test and the texture transform)
static void on_rt_only_packets(const LLVkMirror::ShadowPacketsHeader& h, const U8* blob)
{
    if (!Rt::active() || h.mEntries == 0)
    {
        return;
    }
    const LLVkMirror::ShadowAlphaEntry* entries = (const LLVkMirror::ShadowAlphaEntry*)blob;
    const F32* matrices = (const F32*)(blob + sizeof(LLVkMirror::ShadowAlphaEntry) * h.mEntries);
    for (U32 i = 0; i < h.mEntries; ++i)
    {
        const LLVkMirror::ShadowAlphaEntry& e = entries[i];
        Buf* vb = packet_buffer(e.mVertexBuffer);
        Buf* ib = packet_buffer(e.mIndexBuffer);
        if (!vb || !ib || !vb->mVk.valid() || !ib->mVk.valid() || !vb->mVk.mAddress || !ib->mVk.mAddress)
        {
            continue;
        }
        const F32* model = e.mMatrix ? matrices + 16 * (size_t)e.mMatrix : nullptr;
        if (e.mFlags & LLVkMirror::ALPHA_RAY_ONLY)
        {
            note_rt_caster(h.mPass, e.mVertexBuffer, e.mIndexBuffer, *vb, *ib, e.mFirstIndex, e.mIndexCount, e.mMatrixId, 0, model);
            continue;
        }
        Rt::AlphaInfo alpha;
        alpha.mFlags = e.mFlags & (31u | 64u);
        alpha.mTexcoordOffset = e.mTexcoordOffset;
        alpha.mColorOffset = e.mColorOffset;
        memcpy(alpha.mTextures, e.mTextures, sizeof(alpha.mTextures));
        alpha.mCutoff = e.mCutoff;
        memcpy(alpha.mUv, e.mUv, sizeof(alpha.mUv));
        note_rt_caster(h.mPass, e.mVertexBuffer, e.mIndexBuffer, *vb, *ib, e.mFirstIndex, e.mIndexCount, e.mMatrixId, 1, model, &alpha);
    }
}

// the worker side of kinds 6 and 7: rigged draws of the main view, only named to the ray scene (palettes are copied into the frame's scratch memory the scene call reads)
static void on_rigged_note_packets(const LLVkMirror::ShadowPacketsHeader& h, const U8* blob)
{
    if (!Rt::active() || h.mEntries == 0 || h.mPaletteFloats == 0)
    {
        return;
    }
    const bool alpha_entries = h.mKind == 7;
    const size_t entry_bytes = alpha_entries ? sizeof(LLVkMirror::ShadowAlphaEntry) : sizeof(LLVkMirror::ShadowSkinEntry);
    const F32* matrices = (const F32*)(blob + entry_bytes * h.mEntries);
    Renderer& r = Renderer::instance();
    Renderer::ScratchAlloc palettes = r.allocVertexScratch(4 * (size_t)h.mPaletteFloats);
    if (!palettes.mPtr || !palettes.mAddress)
    {
        return;
    }
    memcpy(palettes.mPtr, matrices + 16 * (size_t)h.mMatrices, 4 * (size_t)h.mPaletteFloats);
    for (U32 i = 0; i < h.mEntries; ++i)
    {
        if (alpha_entries)
        {
            const LLVkMirror::ShadowAlphaEntry& e = ((const LLVkMirror::ShadowAlphaEntry*)blob)[i];
            Buf* vb = packet_buffer(e.mVertexBuffer);
            Buf* ib = packet_buffer(e.mIndexBuffer);
            if (!vb || !ib || !vb->mVk.valid() || !ib->mVk.valid() || !vb->mVk.mAddress || !ib->mVk.mAddress || !(e.mFlags & LLVkMirror::ALPHA_RIGGED))
            {
                continue;
            }
            note_rt_skinned_alpha(h.mPass, e, *vb, *ib, (U64)palettes.mAddress + 4ull * e.mPaletteOffset, e.mMatrix ? matrices + 16 * (size_t)e.mMatrix : nullptr);
        }
        else
        {
            const LLVkMirror::ShadowSkinEntry& e = ((const LLVkMirror::ShadowSkinEntry*)blob)[i];
            Buf* vb = packet_buffer(e.mVertexBuffer);
            Buf* ib = packet_buffer(e.mIndexBuffer);
            if (!vb || !ib || !vb->mVk.valid() || !ib->mVk.valid() || !vb->mVk.mAddress || !ib->mVk.mAddress)
            {
                continue;
            }
            note_rt_skinned(h.mPass, e, *vb, *ib, (U64)palettes.mAddress + 4ull * e.mPaletteOffset, e.mMatrix ? matrices + 16 * (size_t)e.mMatrix : nullptr);
        }
    }
}

// the worker side of a record of the body meshes of avatars (kind 4): nothing is drawn, the ray scene skins them on the GPU
static void on_body_packets(const LLVkMirror::ShadowPacketsHeader& h, const U8* blob)
{
    if (!Rt::active() || h.mEntries == 0)
    {
        return;
    }
    const LLVkMirror::ShadowBodyEntry* entries = (const LLVkMirror::ShadowBodyEntry*)blob;
    const F32* palette_source = (const F32*)(blob + sizeof(LLVkMirror::ShadowBodyEntry) * h.mEntries);
    Renderer& r = Renderer::instance();
    Renderer::ScratchAlloc palettes;
    if (h.mPaletteFloats)
    {
        palettes = r.allocVertexScratch(4 * (size_t)h.mPaletteFloats);       // the memory of the frame: the scene call of this frame reads it
        if (!palettes.mPtr || !palettes.mAddress)
        {
            return;
        }
        memcpy(palettes.mPtr, palette_source, 4 * (size_t)h.mPaletteFloats);
    }
    for (U32 i = 0; i < h.mEntries; ++i)
    {
        const LLVkMirror::ShadowBodyEntry& e = entries[i];
        Buf* vb = packet_buffer(e.mVertexBuffer);
        Buf* ib = packet_buffer(e.mIndexBuffer);
        if (!vb || !ib || !vb->mVk.valid() || !ib->mVk.valid() || !vb->mVk.mAddress || !ib->mVk.mAddress)
        {
            continue;
        }
        Rt::SkinCaster c;
        c.mAvatarId = e.mAvatarId;
        c.mSkinId = e.mMeshId;
        c.mVb = e.mVertexBuffer;
        c.mIb = e.mIndexBuffer;
        c.mFirst = e.mFirstIndex;
        c.mCount = e.mIndexCount;
        c.mVertexFirst = e.mVertexFirst;
        c.mVertexCount = e.mVertexCount;
        c.mWeightOffset = e.mWeightOffset;
        c.mJoints = e.mJoints;
        c.mMode = e.mMode;
        memcpy(c.mAvatarPos, e.mAvatarPos, sizeof(c.mAvatarPos));
        c.mSelf = e.mSelf;
        c.mPalette = e.mMode == 1 ? (U64)palettes.mAddress : 0;
        c.mModel = e.mMode == 2 ? e.mModel : nullptr;
        c.mPass = h.mPass;
        buffer_info_of(*vb, c.mVi);
        buffer_info_of(*ib, c.mIi);
        Rt::noteSkinned(c);
    }
}

// the worker side of a record of alpha masked casters (shadowAlphaPullV.glsl / shadowAlphaPullF.glsl): the entries also name their textures
static void on_shadow_alpha_packets(const LLVkMirror::ShadowPacketsHeader& h, const U8* blob)
{
    struct AlphaGpu { U32 mPositions[2]; U32 mIndices[2]; U32 mTexcoords[2]; U32 mColors[2]; U32 mFirst, mCount, mModel, mFlags; U32 mSlots[2]; F32 mCutoff; U32 mPad; U32 mWeights[2]; U32 mPalette[2]; };
    static_assert(sizeof(AlphaGpu) == 80, "alpha packet entry");
    static thread_local VertexInput vi_cache;
    static thread_local std::vector<U32> used_cache, packet_used;
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
            note_rt_caster(h.mPass, e.mVertexBuffer, e.mIndexBuffer, *vb, *ib, e.mFirstIndex, e.mIndexCount, e.mMatrixId, 1, e.mMatrix ? matrices + 16 * (size_t)e.mMatrix : nullptr, &alpha);
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
            if (Rt::active())
            {
                note_rt_skinned_alpha(h.mPass, e, *vb, *ib, pal, e.mMatrix ? matrices + 16 * (size_t)e.mMatrix : nullptr);
            }
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
    r.setTopology(topology(LLVkFormat::TOP_TRIANGLES));
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
    static thread_local std::vector<U32> used_cache, packet_used;
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
    r.setTopology(topology(LLVkFormat::TOP_TRIANGLES));
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
    static thread_local std::vector<U32> used_cache, packet_used;
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
        if (Rt::active())
        {
            note_rt_skinned(h.mPass, e, *vb, *ib, pal, e.mMatrix ? matrices + 16 * (size_t)e.mMatrix : nullptr);
        }
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
    r.setTopology(topology(LLVkFormat::TOP_TRIANGLES));

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
    if (h.mKind == 4)
    {
        on_body_packets(h, blob);
        return;
    }
    if (h.mKind == 5)
    {
        on_rt_only_packets(h, blob);
        return;
    }
    if (h.mKind == 6 || h.mKind == 7)
    {
        on_rigged_note_packets(h, blob);
        return;
    }
    struct PacketGpu { U32 mPositions[2]; U32 mIndices[2]; U32 mFirst, mCount, mModel, mPad; };
    static_assert(sizeof(PacketGpu) == 32, "packet entry");
    static thread_local VertexInput vi_cache;
    static thread_local std::vector<U32> used_cache, packet_used;
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
    static U32 cache_name[1024];
    static U64 cache_generation = ~0ull;
    if (cache_generation != sPacketBufferGeneration)
    {
        memset(cache_buf, 0, sizeof(cache_buf));
        memset(cache_name, 0, sizeof(cache_name));
        cache_generation = sPacketBufferGeneration;
    }
    auto lookup = [&](U32 name) -> Buf*
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
    U32 last_vb = 0, last_ib = 0;
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
            note_rt_caster(h.mPass, e.mVertexBuffer, e.mIndexBuffer, *vb, *ib, e.mFirstIndex, e.mIndexCount, e.mMatrixId, 0, e.mMatrix ? matrices + 16 * (size_t)e.mMatrix : nullptr);
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
    r.setTopology(topology(LLVkFormat::TOP_TRIANGLES));

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

// F1.4: clears the current target (the colour and depth write masks and the scissor of the state block apply, like a GL clear)
void clear_target(bool color, bool depth, const float* rgba)
{
    HookTimer hook_timer;
    if (!LLVkMirror::inFrame() || !sync_target())
    {
        return;
    }
    U32 bits = 0;
    const LLVkState::Block* blk = state_block();
    const bool color_mask = !blk || blk->mBlend.mWriteMask != 0;
    const bool depth_mask = !blk || blk->mDepthMask;
    if (color && color_mask) bits |= CLEAR_COLOR;
    if (depth && depth_mask) bits |= CLEAR_DEPTH;
    if (bits)
    {
        Renderer::instance().clear(bits, rgba, 1.f, 0);
    }
}

//-----------------------------------------------------------------------------------------------------
// debug dump of the render targets of a frame (trigger: create the file <work>\vk-dump.trigger)
//-----------------------------------------------------------------------------------------------------

namespace
{

} // anonymous namespace

// what the translator currently holds in Vulkan, for the benchmark log
std::string resource_summary()
{
    std::lock_guard<LLVkMirror::Mutex> lock(gMutex);
    U64 tex_bytes = 0, rt_bytes = 0, buf_bytes = 0, unused_mip_bytes = 0;
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
        // the mip levels above the ones that ever received data (a texture the viewer uses without mips still owns the whole chain)
        for (U32 m = std::max<U32>(1, t.mValidLevels); m < t.mImage.mMipLevels; ++m)
        {
            const U64 w = std::max<U32>(1, t.mImage.mExtent.width >> m), h = std::max<U32>(1, t.mImage.mExtent.height >> m);
            unused_mip_bytes += ((w + fi.mBlockWidth - 1) / fi.mBlockWidth) * ((h + fi.mBlockHeight - 1) / fi.mBlockHeight) * fi.mBytesPerBlock * std::max<U32>(1, t.mImage.mLayers) * std::max<U32>(1, t.mImage.mExtent.depth);
        }
    }
    for (auto& kv : sGL.mBuffers)
    {
        if (kv.second.mVk.valid()) { buf_bytes += kv.second.mVk.mSize; ++buf_count; }
    }
    return "mirrored textures " + std::to_string(tex_count) + " (" + std::to_string(tex_bytes / (1024 * 1024)) + " MB), render targets " + std::to_string(rt_count) + " ("
        + std::to_string(rt_bytes / (1024 * 1024)) + " MB, " + std::to_string(views) + " extra views), buffers " + std::to_string(buf_count) + " (" + std::to_string(buf_bytes / (1024 * 1024))
        + " MB), mip levels that hold no data " + std::to_string(unused_mip_bytes / (1024 * 1024)) + " MB, GL texture objects " + std::to_string(sGL.mTextures.size()) + ", pending destroys " + std::to_string(Context::instance().pendingDestroys());
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
        // <FS> the video memory and what the textures take of it, every 1800 frames (about 30 seconds): after a teleport this shows where the memory went and whether a texture was refused
        static U32 frames = 0;
        if (++frames >= 1800)
        {
            frames = 0;
            const Renderer::TexStats ts = Renderer::instance().takeTexStats();
            LL_INFOS("VkTex") << "textures: " << resource_summary() << " | " << Context::instance().memoryBudgetText() << "| bindless slots in use at most " << ts.mBindlessPeak << " of "
                << Renderer::instance().bindlessCapacity() << ", " << ts.mBindlessWrites / 1800.0 << " descriptors written per frame, refused " << ts.mBindlessRefused << " | texture images that could not be allocated " << ts.mAllocFailed << " ("
                << ts.mAllocFailedBytes / (1024 * 1024) << " MB, last " << ts.mAllocFailedLast[0] << "x" << ts.mAllocFailedLast[1] << ")" << LL_ENDL;
            const Renderer::OccStats os = Renderer::instance().takeOccStats();
            LL_INFOS("VkTex") << "occlusion queries in this period: " << os.mPassed << " with samples, " << os.mNone << " with none (the box was hidden), " << os.mNeverDrawn
                << " whose box draw never reached the GPU (counted as hidden)" << LL_ENDL;
        }
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
                            std::vector<std::pair<U32, WriterStat>> top(sWriters.begin(), sWriters.end());
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
            if (FILE* f = fopen("<work>/vk-debug.txt", "rb"))
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

// ---- queries ----
struct QueryRec
{
    U32 mName = 0;                   // the GL name the query was begun under
    U64 mGen = 0;                       // the async generation of that begin (0 in inline mode)
    U32 mKind = 0;                      // LLVkQuery::Kind
    bool mOcc = false, mTime = false;
    bool mVkHave = false, mHaveStart = false, mHaveEnd = false;
    U64 mVkValue = 0, mTsStart = 0, mTsEnd = 0;
};
std::map<U64, QueryRec> sQueries;               // by serial, oldest first
std::unordered_map<U32, U64> sQueryLatest;   // GL query name -> serial of its latest begin
std::map<U32, U64> sQueryActive;                 // kind -> serial of the query in progress
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
            if (rec.mOcc && rec.mKind != LLVkQuery::SAMPLES_PASSED) v = v ? 1 : 0;
            LLVkAsync::queryPublish(rec.mName, rec.mGen, v);
        }
    }
    while (sQueries.size() > 40000)         // results that never found their partner
    {
        sQueries.erase(sQueries.begin());
    }
}


} // anonymous namespace







void create_queries(S32 n, U32* ids)
{
    (void)n; (void)ids;
}

void destroy_queries(S32 n, const U32* ids)
{
    if (!queries_on() || !ids) return;
    for (S32 i = 0; i < n; ++i) sQueryLatest.erase(ids[i]);
}

void query_begin(unsigned kind, unsigned id)
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
    rec.mKind = kind;
    const U64 serial = ++sQuerySerial;
    bool ok = false;
    if (kind == LLVkQuery::ANY_SAMPLES || kind == LLVkQuery::SAMPLES_PASSED)
    {
        rec.mOcc = true;
        ok = r.queryBegin(serial);
    }
    else if (kind == LLVkQuery::TIME_ELAPSED)
    {
        rec.mTime = true;
        ok = r.queryTimestamp(serial, false);
    }
    if (!ok)
    {
        sQueryActive.erase(kind);
        if (LLVkAsync::gEnabled) LLVkAsync::queryIdle(id, LLVkAsync::workerQueryGen(id));
        return;
    }
    sQueries[serial] = rec;
    sQueryLatest[id] = serial;
    sQueryActive[kind] = serial;
}

void query_end(unsigned kind)
{
    if (!queries_on()) return;
    auto act = sQueryActive.find(kind);
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





// the running totals, for the log


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
bool gl_channels(U8 comps, std::vector<int>& ch)
{
    switch (comps)
    {
    case LLVkFormat::C_RGBA: ch = { 0, 1, 2, 3 }; return true;
    case LLVkFormat::C_BGRA: ch = { 2, 1, 0, 3 }; return true;
    case LLVkFormat::C_RGB: ch = { 0, 1, 2 }; return true;
    case LLVkFormat::C_BGR: ch = { 2, 1, 0 }; return true;
    case LLVkFormat::C_RG: ch = { 0, 1 }; return true;
    case LLVkFormat::C_R: case LLVkFormat::C_DEPTH: ch = { 0 }; return true;
    default: return false;
    }
}

// the component types a readback can write
U32 gl_type_bytes(U8 type)
{
    switch (type)
    {
    case LLVkFormat::T_U8: return 1;
    case LLVkFormat::T_F16: return 2;
    case LLVkFormat::T_F32: return 4;
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

void own_gen(int kind, S32 n, U32* out)
{
    static std::atomic<U32> next[5] = { 1, 1, 1, 1, 1 };       // textures, framebuffers, buffers, vertex arrays, queries; 0 is never a name
    for (S32 i = 0; i < n; ++i)
    {
        out[i] = next[kind].fetch_add(1);
    }
}

void resource_create(unsigned kind, int n, const unsigned* ids)
{
    U32* names = const_cast<U32*>(ids);
    switch (kind)
    {
    case LLVkAsync::RES_TEXTURE: create_textures(n, names); break;
    case LLVkAsync::RES_TARGET: create_targets(n, names); break;
    case LLVkAsync::RES_BUFFER: create_buffers(n, names); break;
    case LLVkAsync::RES_QUERY: create_queries(n, names); break;
    default: break;
    }
}

void resource_destroy(unsigned kind, int n, const unsigned* ids)
{
    switch (kind)
    {
    case LLVkAsync::RES_TEXTURE: destroy_textures(n, ids); break;
    case LLVkAsync::RES_TARGET: destroy_targets(n, ids); break;
    case LLVkAsync::RES_BUFFER: destroy_buffers(n, ids); break;
    case LLVkAsync::RES_QUERY: destroy_queries(n, ids); break;
    default: break;
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

U32 pack_alignment() { return 1; }        // readbacks hand back tightly packed rows

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
bool encode_to_gl(const std::vector<float>& rgba, S32 w, S32 h, LLVkFormat::Transfer tr, U8* dst, S32 row_length = 0)
{
    std::vector<int> ch;
    const U32 tb = gl_type_bytes(tr.mType);
    if (!dst || !gl_channels(tr.mComps, ch) || !tb)
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
                if (tr.mType == LLVkFormat::T_U8)
                {
                    *p = (U8)std::lround(std::min(1.f, std::max(0.f, v)) * 255.f);
                }
                else if (tr.mType == LLVkFormat::T_F16)
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
bool own_query_value(U32 id, bool want_result, U64& value)
{
    drain_query_results();
    auto lat = sQueryLatest.find(id);
    auto it = lat == sQueryLatest.end() ? sQueries.end() : sQueries.find(lat->second);
    if (!want_result)
    {
        value = (it == sQueries.end() || it->second.mVkHave) ? 1 : 0;
        return true;
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
        if (it->second.mOcc && it->second.mKind != LLVkQuery::SAMPLES_PASSED) value = value ? 1 : 0;      // ANY_SAMPLES is a boolean
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

int tex_level_parameter(unsigned id, LLVkFormat::Target tg, int level, LLVkFormat::Level q)
{
    EditScope s(id);
    Tex* t = edit_texture();
    int v = 0;
    if (t && t->mImage.valid() && level >= 0 && (U32)level < std::max<U32>(1, t->mDim == LLVkFormat::DIM_2D ? mipLevelCount(t->mWidth, t->mHeight) : t->mLevels))
    {
        switch (q)
        {
        case LLVkFormat::LQ_WIDTH: v = (int)std::max<U32>(1, t->mWidth >> level); break;
        case LLVkFormat::LQ_HEIGHT: v = (int)std::max<U32>(1, t->mHeight >> level); break;
        case LLVkFormat::LQ_DEPTH: v = (int)(t->mDim == LLVkFormat::DIM_3D ? std::max<U32>(1, t->mDepth >> level) : std::max<U32>(1, t->mDepth)); break;
        case LLVkFormat::LQ_COMPRESSED: v = formatInfo(t->mImage.mFormat).mCompressed ? 1 : 0; break;
        case LLVkFormat::LQ_COMPRESSED_SIZE: v = (int)Renderer::compressedMipBytes(t->mImage, (U32)level); break;
        default: break;
        }
    }
    return v;
}

bool query_value(unsigned id, bool want_result, unsigned long long& value)
{
    return own_query_value(id, want_result, value);
}

void read_pixels(unsigned fbo, int x, int y, int width, int height, LLVkFormat::Transfer tr, void* pixels)
{
    U8* dst = pack_destination(pixels);
    Image* img = read_image(fbo);
    std::vector<U8> texels;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    std::vector<float> rgba;
    if (width <= 0 || height <= 0)
    {
        return;
    }
    if (!dst)
    {
        return;
    }
    if (!img || !img->valid() || !fetch_rect_cached(*img, x, y, width, height, texels, fmt) || !decode_vk_texels(fmt, texels, (size_t)width * height, rgba)
        || !encode_to_gl(rgba, width, height, tr, dst, 0))
    {
        static U32 reported = 0;
        if (reported++ < 5)
        {
            LL_WARNS("VkGL") << "a readback could not be answered from Vulkan (" << width << "x" << height << " components " << (S32)tr.mComps << " type " << (S32)tr.mType << ")" << LL_ENDL;
        }
        // GL would have written something; hand back zeros so the caller does not read garbage
        std::vector<int> ch;
        if (gl_channels(tr.mComps, ch) && gl_type_bytes(tr.mType))
        {
            const size_t stride = (((size_t)width * ch.size() * gl_type_bytes(tr.mType) + pack_alignment() - 1) / pack_alignment()) * pack_alignment();
            memset(dst, 0, stride * (size_t)height);
        }
    }
    return;
}

void tex_get_image(unsigned id, LLVkFormat::Target tg, int level, LLVkFormat::Transfer tr, void* pixels)
{
    EditScope s(id);
    U8* dst = pack_destination(pixels);
    Tex* t = edit_texture();
    if (!dst || !t || !t->mImage.valid() || level < 0 || (U32)level >= t->mImage.mMipLevels)
    {
        return;
    }
    Renderer& r = Renderer::instance();
    if (LLVkMirror::inFrame() && !r.flushAndRestart())
    {
        return;
    }
    const S32 w = (S32)std::max<U32>(1, t->mImage.mExtent.width >> level), h = (S32)std::max<U32>(1, t->mImage.mExtent.height >> level);
    std::vector<int> ch;
    if (!gl_channels(tr.mComps, ch) || !gl_type_bytes(tr.mType))
    {
        return;
    }
    const size_t stride = (((size_t)w * ch.size() * gl_type_bytes(tr.mType) + pack_alignment() - 1) / pack_alignment()) * pack_alignment();
    // 2D: one image; cube map face: that face; arrays and cube maps: every layer one after the other
    U32 first_layer = 0, layers = 1;
    if (tg.mFace >= 0) first_layer = (U32)tg.mFace;
    else if (tg.mDim == LLVkFormat::DIM_CUBE_ARRAY || tg.mDim == LLVkFormat::DIM_2D_ARRAY) layers = t->mImage.mLayers;
    else if (tg.mDim == LLVkFormat::DIM_CUBE) layers = 6;
    for (U32 l = 0; l < layers; ++l)
    {
        std::vector<U8> texels;
        std::vector<float> rgba;
        if (!r.readbackRegion(t->mImage, (U32)level, first_layer + l, 0, 0, (U32)w, (U32)h, texels) || !decode_vk_texels(t->mImage.mFormat, texels, (size_t)w * h, rgba))
        {
            continue;
        }
        encode_to_gl(rgba, w, h, tr, dst + (size_t)l * stride * h, 0);
    }
}

void tex_get_compressed_image(unsigned id, LLVkFormat::Target tg, int level, void* img)
{
    EditScope s(id);
    U8* dst = pack_destination(img);
    Tex* t = edit_texture();
    if (!dst || !t || !t->mImage.valid() || level < 0 || (U32)level >= t->mImage.mMipLevels)
    {
        return;
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
            LL_WARNS("VkGL") << "a compressed readback on a texture the mirror stores uncompressed (generic compressed format)" << LL_ENDL;
        }
        return;
    }
    if (LLVkMirror::inFrame() && !r.flushAndRestart())
    {
        return;
    }
    const U32 layer = tg.mFace >= 0 ? (U32)tg.mFace : 0;
    if (r.readbackCompressedMip(t->mImage, (U32)level, layer, blocks))
    {
        memcpy(dst, blocks.data(), blocks.size());
    }
}

LLVkTex::PixelStore inline_pixel_store()
{
    LLVkTex::PixelStore ps;
    ps.mAlignment = sCtx.mUnpackAlignment;
    ps.mRowLength = sCtx.mUnpackRowLength;
    ps.mUnpackBuffer = sCtx.mPixelUnpackBuffer;
    return ps;
}




void on_upscale(const LLVkMirror::UpscaleArgs& a)
{
    static U32 failures = 0;
    if (!gActive || !LLVkDlss::ready())
    {
        return;
    }
    auto find = [](U32 name) -> Tex* {
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

