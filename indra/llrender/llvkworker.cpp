/**
 * @file llvkworker.cpp
 * @brief Translation of the viewer's OpenGL call stream into Vulkan (see llvkworker.h).
 *
 * Every hook runs after the real GL call. The hooks keep a copy of the GL state that matters (textures, framebuffer
 * attachments, buffers, vertex arrays, draw state) and, at each draw, translate it into Vulkan state and replay the
 * draw into the mirror frame.
 */

#include "linden_common.h"
#include <xmmintrin.h>

#include "llrenderheaders.h"

#if defined(LL_VULKAN)

#include "llvkbridge.h"
#include "llvktexanim.h"
#include "llshaderprogram.h"
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
#include "llvkupscale.h"
#include "llvkdlssnr.h"
#include "llvkrt.h"
#include "llvkpasscensus.h"
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <chrono>
#include <deque>
#include "llvkworker.h"
#include "llvkasync.h"
#include <map>
#include <mutex>
#include <set>
#include <cstdio>
#include <tuple>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <cstring>
#include <thread>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <string>

std::atomic<int> gFsWorkerCpu{ -1 };
std::atomic<unsigned> gFsWorkerTid{ 0 };

namespace LLVkWorker
{

bool gActive = false;
LLVkBridge::Mutex gMutex;

// CPU time spent inside the translator's draw/clear hooks (the cost of recording the work into Vulkan), in ticks of the performance counter
static long long gHookTicks = 0;

namespace
{

struct HookTimer
{
    LARGE_INTEGER mStart;
    const bool mOn = LLVkBridge::gPhaseTiming;      // the total only goes into the LL_VK_PHASES report: no clock reads (two per draw) otherwise
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
    case S_BC1_SRGBA: m.mVk = VK_FORMAT_BC1_RGBA_SRGB_BLOCK; m.mTexelBytes = 8; m.mCompressed = true; return true;
    case S_BC2_SRGBA: m.mVk = VK_FORMAT_BC2_SRGB_BLOCK; m.mTexelBytes = 16; m.mCompressed = true; return true;
    case S_BC3_SRGBA: m.mVk = VK_FORMAT_BC3_SRGB_BLOCK; m.mTexelBytes = 16; m.mCompressed = true; return true;
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
    U32 mHandle = 0;                            // <FS> texture handles (doc/vulkan/PLAN-texture-handles.md, H3): its handle in the 2D table, made when the texture first has an image with data and kept until the texture goes:
                                                //   rows, pins and records carry it; what it shows follows the image (texture_handle)
    U64 mHandleViewId = 0;                      // ... the view (id) and sampler the handle shows now
    VkSampler mHandleSampler = VK_NULL_HANDLE;
    bool mHandleDirty = false;                  // ... in sHandleDirty
    bool mSeenSampled = false;                  // <FS> diagnostic (blink after load): a draw has sampled this texture with data in it
    U32 mName = 0;                              // <FS> its GL name, which is its id (set when the entry is made)
    U32 mPinIndex = 0;                          // <FS> the ray scene's pin of this texture (0 = none): a change of the texture asks for it to be looked at again
};

// <FS> something about this texture that a bindless descriptor depends on changed (image, view, sampler parameters, levels): a pin of it is looked at again at the next ray scene call
void pin_touch(Tex& t);
void pin_wake_name(U32 name);

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
    ctx.noteDestroyedView(t.mImage.mViewId);        // (the destroyed list of the reports)
    ctx.forgetView(old);            // (the handles made for this view by handleForImage go with it)
    t.mImage.mView = fresh;
    t.mImage.mViewId = nextViewId();       // a different view: whatever keys on the view id must not take it for the old one
    pin_touch(t);                   // (the handle of this texture shows the new view from the next refresh: the descriptor of the old one stays valid until the old view is destroyed, several frames from now)
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
    bool mResWatched = false;           // S2: a resident fragment of the ray scene reads it: a change (epoch, moves, version) is logged by name (sResChanged)
    U32 mResName = 0;
    U32 mRenamesInFrame = 0;            // the renames of this buffer in the frame mRenamedFrame (a chain: every write of a frame renamed again)
    U64 mRenamedFrame = ~0ull;          // the frame of the last rename of this buffer (LL_VK_RENAME_ONCE: a range allocated this frame has not been listed by any fragment yet)
    bool mGeoWatched = false;           // S3: a scene geometry fragment reads it: a change of its storage (epoch, moves) is logged by name (sGeoChanged), and the write path asks whether the fragments listed it lately
    bool mRtWatched = false;            // the ray scene reads this buffer at trace time (alpha records): a change of its storage is counted (sBufferTopology)
    // The Vulkan buffer lives in write-combined video memory, which is very slow to read from the CPU. This copy in ordinary memory
    // is what a renamed buffer is refilled from when only part of it is rewritten.
    std::vector<U8> mShadow;
};

struct BoundAttrib
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

struct BoundVertices
{
    BoundAttrib mAttrib[16];
    U32 mArrayBuffer = 0;        // the buffer the next attribute pointers refer to
    U32 mElementBuffer = 0;
};
// ---- S6.4: the worker's state, by owner (doc/vulkan/S6-design.md section 7) ----
// PassState: what the records of a pass read and write that the pass before left behind. Passes are closed (every pass announces again what it reads: LL_VK_PASSCLOSE), poison mode forgets all of it at
// every pass (LLVkWorker::pass_poison). One instance: the stream is played by one thread; passes decoded on several threads would each have their own.
struct PassState
{
    U32 mUnitTex[32] = {};              // F2.6: the texture each unit holds, by id only; a sampler of dimension 'cls' samples it when the texture has that dimension
    U32 mDrawFbo = 0;                   // F1.4: the render target draws go to (0: the back buffer): set by the typed pass record only
    U32 mUboBinding[4] = {};            // GL buffer bound to each named uniform block binding point
    BoundVertices mBound;               // the vertex arrays the next draw reads (a draw record carries it when it changed)
    // what sync_target() resolved for the current draw framebuffer, valid until something that can change it happens (bumps mTargetEpoch)
    U64 mTargetEpoch = 1;
    const void* mTargetNamesOwner = nullptr;     // whose list of attachment names mCurTargets holds
    std::vector<U32> mCurTargets;       // textures attached to the current draw framebuffer
    bool mHasDepth = false;             // does the current render target have a depth attachment
    U32 mCurTargetW = 1, mCurTargetH = 1;        // size of the first colour target of the current draw framebuffer
};
PassState gPassState;
// FrameState: tables that live for one frame (the palette list the frame's records name by offset, the targets and samplers the debug dump reports), reset where the frame turns over.
struct PalSeg { U32 mStart, mFloats; U64 mAddress; };
struct FrameState
{
    std::set<U32> mTargets;             // textures rendered into during the current frame (for the debug dump)
    U32 mRecentTargets[16] = {};        // cheap filter in front of mTargets: ids inserted recently this frame
    std::map<U32, U32> mSampled;        // textures read by draws this frame, with use counts
    std::map<U32, U32> mSampledLast;
    std::vector<PalSeg> mPalSegs;       // S4 A2: the palette list of the viewer's frame mPalFrame, by float offset
    U32 mPalFrame = ~0u;
};
FrameState gFrameState;
// What is left at file level is owned by the resource tables (sGL: textures, buffers, targets; the bindless table and pins; the arena; the resident and scene fragments; queries), which only the
// worker writes, or is diagnostics. The Renderer singleton holds the draw state the recorder takes (program, textures, raster / depth / blend, vertex input) and moves with the recorder split.

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
};
thread_local PerContext sCtxTls;
thread_local PerContext* sCtxOverride = nullptr;      // set on the async worker: the context of the thread whose call is being replayed
inline PerContext& current_ctx() { return sCtxOverride ? *sCtxOverride : sCtxTls; }
#define sCtx (current_ctx())


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
// be gone, and every one of those calls lands here (the shutdown of the mirror itself is LLVkBridge::shutdown). A destroyed map under that call was the crash at exit.
GLState& sGL = *new GLState;

// <FS> flat lookup of the entries of sGL.mTextures / mBuffers by GL name (the names are small integers from the viewer's name pools). A hash lookup for every sampler of
// every draw cost more than the rest of the draw's bookkeeping. The pointers are to the nodes of the maps (stable until erased); erasing forgets them.
std::vector<Tex*> sTexFlat;

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


U32 sDraws = 0, sDrawsSkipped = 0, sCopies = 0;
std::unordered_set<U32> sSeenNames;       // <FS> diagnostic: GL names of textures drawn with a picture at least once
U64 sCopyDropFrame = 0, sCopyDropTex = 0, sCopyDropSrc = 0, sRespecs = 0, sGrows = 0, sLevelDrops = 0, sLevelDropsRefused = 0, sSampleNoName = 0, sSampleNoImage = 0, sSampleBlank = 0, sPinNoSlot = 0, sCommits = 0;     // <FS> texture copies from targets that were dropped (and why), images re-specified, mip chains grown: the downscale of textures shows in these
U64 sOutOfRangeDraws = 0;           // indexed draws left out because their index or vertex range was not inside their buffers (since the start)
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
// the per-draw statistics / pass graph are only gathered when asked for (they cost string work on every draw)
const bool sStatsEnabled = getenv("LL_VK_STATS") != nullptr || getenv("LL_VK_DEBUG_SHADERS") != nullptr || getenv("LL_VK_VALIDATION") != nullptr;
bool sDbgMatch = false;                 // the draw being prepared belongs to a program the debug filter selects
bool sDbgNoDepth = false, sDbgNoDepthWrite = false, sDbgNoDepthTest = false, sDbgNoBlend = false, sDbgNoCull = false, sDbgNoScissor = false,
     sDbgFullMask = false, sDbgNoFeedback = false;      // debug draw state overrides from <work>\vk-debug.txt
int sDbgMode = 0;                       // debug shader mode from <work>\vk-debug.txt (0 = off)
std::string sDbgFilter;                 // programs whose name contains this get the mode
std::string sDbgSampler;                // sampler shown by mode 2

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

inline S32 edit_unpack_alignment() { return tlsEditPS ? tlsEditPS->mAlignment : 1; }
inline S32 edit_unpack_row_length() { return tlsEditPS ? tlsEditPS->mRowLength : 0; }

//-----------------------------------------------------------------------------------------------------
// textures
//-----------------------------------------------------------------------------------------------------

void destroy_tex_images(Tex& t)
{
    pin_touch(t);
    ++gPassState.mTargetEpoch;
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
    ++gPassState.mTargetEpoch;
    if (t.mImage.valid())
    {
        ++sRespecs;
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
    td.mMips = (tg.mDim == DIM_2D) ? 1 : (tg.mDim == DIM_3D ? mipLevelCount(std::max(w, h), depth) : mipLevelCount(w, h));
    td.mFormat = fm.mVk;
    td.mSwizzle = t.mSwizzleSet ? swizzle_mapping(t) : fm.mSwizzle;
    if (fm.mDepth)
    {
        td.mUsage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    else if (!fm.mCompressed)
    {
        td.mUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (LLVkUpscale::ready() && (fm.mVk == VK_FORMAT_R8G8B8A8_UNORM || fm.mVk == VK_FORMAT_R16G16B16A16_SFLOAT))
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
            pin_wake_name(kv.first);        // a pin that has been waiting for this texture's image is looked at now, not when its backoff runs out
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
    pin_touch(t);
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
    ++sGrows;
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
        if (LLVkUpscale::ready() && (t.mFormat.mVk == VK_FORMAT_R8G8B8A8_UNORM || t.mFormat.mVk == VK_FORMAT_R16G16B16A16_SFLOAT))
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
    pin_touch(t);
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

// <FS> native-vk: a plain 2D texture that holds its whole mip chain gives up its top k levels: a new image with the levels below them (a copy, the old image released). The viewer shrinks a texture this way
// (LLImageTex::scaleDown) instead of drawing it small into a target and copying that. False (nothing changed) when the texture does not hold all of its levels.
bool drop_levels(Tex& t, U32 k)
{
    using namespace LLVkFormat;
    if (!k || !t.mImage.valid() || t.mDim != DIM_2D || t.mFormat.mDepth || t.mFormat.mCompressed)
    {
        return false;
    }
    const U32 full = mipLevelCount(t.mWidth, t.mHeight);
    if (t.mLevels < full || t.mValidLevels < t.mLevels || t.mLevels <= k)
    {
        return false;
    }
    TextureDesc td;
    td.mType = TexType::Tex2D;
    td.mWidth = std::max(1u, t.mWidth >> k);
    td.mHeight = std::max(1u, t.mHeight >> k);
    td.mDepth = 1;
    td.mMips = t.mLevels - k;
    td.mFormat = t.mFormat.mVk;
    td.mSwizzle = t.mSwizzleSet ? swizzle_mapping(t) : t.mFormat.mSwizzle;
    td.mUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (LLVkUpscale::ready() && (t.mFormat.mVk == VK_FORMAT_R8G8B8A8_UNORM || t.mFormat.mVk == VK_FORMAT_R16G16B16A16_SFLOAT))
    {
        td.mUsage |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    Image fresh;
    if (!Renderer::instance().createTexture(td, fresh))
    {
        return false;
    }
    if (!Renderer::instance().copyMipLevels(t.mImage, k, fresh))
    {
        Context::instance().destroyImage(fresh);
        return false;
    }
    destroy_tex_images(t);
    t.mImage = fresh;
    t.mWidth = td.mWidth;
    t.mHeight = td.mHeight;
    t.mLevels = td.mMips;
    t.mValidLevels = td.mMips;
    pin_touch(t);
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
        pin_touch(t);
    }
    const void* src = pixels;
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
        const bool unlocked = (U64)w * h * d >= 16384 && !LLVkAsync::gEnabled;      // (the worker does not take the lock)
        unsigned depth_held = 0;
        if (unlocked)
        {
            depth_held = LLVkWorker::gMutex.releaseAll();
        }
        const auto t_conv = std::chrono::steady_clock::now();
        const bool converted = convert_pixels(fm, tr, src, w, h, d, alignment, row_length, texels);
        LLVkAsync::noteTexWorker((U64)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t_conv).count(), 0, 0, 0);
        if (unlocked)
        {
            LLVkWorker::gMutex.reacquire(depth_held);
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
        return LLVkBridge::backbufferColor();
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
    return read_image(gPassState.mDrawFbo);        // readbacks (glReadPixels) read the target that was last made current
}

// makes the renderer's render target match the GL draw framebuffer. Returns false if there is nothing to draw to.
// remembers that a texture was rendered into this frame; the same few textures come back on every draw, so look in a small cache first
static inline void note_frame_target(U32 texture)
{
    U32& slot = gFrameState.mRecentTargets[texture & 15];
    if (slot != texture)
    {
        slot = texture;
        gFrameState.mTargets.insert(texture);
    }
}

const LLVkState::Block* state_block();

bool sync_target()
{
    Renderer& r = Renderer::instance();
    if (gPassState.mDrawFbo == 0)
    {
        gPassState.mCurTargets.clear();
        gPassState.mTargetNamesOwner = nullptr;
    }
    if (gPassState.mDrawFbo == 0)
    {
        Image* color = LLVkBridge::backbufferColor();
        Image* depth = LLVkBridge::backbufferDepth();
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
        gPassState.mHasDepth = depth != nullptr;
        gPassState.mCurTargetW = color->mExtent.width;
        gPassState.mCurTargetH = color->mExtent.height;
        return true;
    }
    // the framebuffer's attachments were resolved for an earlier draw and nothing that could change them has happened since (and the frame did not turn over)
    struct TargetCache { U64 mEpoch = 0; U32 mFbo = ~0u; U64 mFrame = ~0ull; Image* mColors[8] = {}; U32 mCount = 0; Image* mDepth = nullptr; bool mOk = false; std::vector<U32> mNames; };
    static TargetCache cache;
    const U64 frame_now = Context::instance().getFrameNumber();
    if (cache.mEpoch == gPassState.mTargetEpoch && cache.mFbo == gPassState.mDrawFbo && cache.mFrame == frame_now && !sDbgMatch && cache.mOk)
    {
        if (gPassState.mTargetNamesOwner != &cache)
        {
            gPassState.mCurTargets = cache.mNames;
            gPassState.mTargetNamesOwner = &cache;
        }
        r.setRenderTarget(cache.mColors, cache.mCount, cache.mDepth);
        gPassState.mHasDepth = cache.mDepth != nullptr;
        if (cache.mCount)
        {
            gPassState.mCurTargetW = cache.mColors[0]->mExtent.width;
            gPassState.mCurTargetH = cache.mColors[0]->mExtent.height;
        }
        return true;
    }
    gPassState.mTargetNamesOwner = nullptr;
    gPassState.mCurTargets.clear();
    auto it = sGL.mFbos.find(gPassState.mDrawFbo);
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
        gPassState.mCurTargets.push_back(fbo.mColor[i].mTexture);
    }
    if (fbo.mDepth.mTexture)
    {
        note_frame_target(fbo.mDepth.mTexture);
        gPassState.mCurTargets.push_back(fbo.mDepth.mTexture);
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
    gPassState.mHasDepth = depth != nullptr;
    if (count)
    {
        gPassState.mCurTargetW = colors[0]->mExtent.width;
        gPassState.mCurTargetH = colors[0]->mExtent.height;
    }
    cache.mEpoch = gPassState.mTargetEpoch;
    cache.mFbo = gPassState.mDrawFbo;
    cache.mFrame = frame_now;
    memcpy(cache.mColors, colors, sizeof(colors));
    cache.mCount = count;
    cache.mDepth = depth;
    cache.mNames = gPassState.mCurTargets;
    cache.mOk = true;
    gPassState.mTargetNamesOwner = &cache;
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
        const float bias = LLVkUpscale::lodBias();      // a new DLSS mode changes it: the sampler is made again
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
    if (LLVkPassCensus::sOn) LLVkPassCensus::stateRead();
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

// the texture a sampler of dimension 'cls' reads from a unit, through the unit's id alone (a texture of another dimension is not what a sampler of this dimension samples)
inline U32 unit_texture(U32 unit, U32 cls)
{
    const U32 name = gPassState.mUnitTex[unit];
    if (!name)
    {
        return 0;
    }
    Tex* t = tex_ptr(name);
    return (t && t->mDim == cls) ? name : 0;
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
        if (LLVkPassCensus::sOn) LLVkPassCensus::unitRead(unit);
        const U32 slot_name = unit_texture(unit, cls);
        Tex* tex = tex_ptr(slot_name);
        if (sStatsEnabled)
        {
            const std::string& type = info.mSamplers[k].mType;
            static const char* const watch[] = { "Soften", "Windlight Sky", "Deferred Sun", "Deferred Light Shader", "Irradiance Gen", "Radiance Gen", "HUD", "Copy Depth", "Haze", "Tonemap", "Reflection Mip", "Glow" };
            bool wanted = false;
            for (const char* w : watch) if (program.getName().find(w) != std::string::npos) wanted = true;
            if (wanted)
            {
                std::string what = "unit " + std::to_string(unit) + " tex " + std::to_string(slot_name) + " ";
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
                const U32 tname = slot_name;
                ++gFrameState.mSampled[tname];
                if (std::find(gPassState.mCurTargets.begin(), gPassState.mCurTargets.end(), tname) != gPassState.mCurTargets.end())
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

// F3.1: vertices that travel inside a draw record (immediate mode): set around prepare_draw by draw_scratch, read by build_vertex_input
struct ScratchVerts
{
    S32 mCount = 0;
    U32 mMask = 0;                  // LLVkAsync::ScratchAttr bits (bit = attribute location)
    const U8* mBytes = nullptr;     // positions (16 bytes each), texcoords (8), colours (4), one run after the other
};
thread_local const ScratchVerts* sScratchVerts = nullptr;

// the vertex input of a scratch draw: the vertices are copied to the frame's vertex scratch; the locations the record supplies read it, the others read the constant attribute
bool build_scratch_input(Renderer& r, Program& program, VertexInput& vi)
{
    const ScratchVerts& sv = *sScratchVerts;
    const ProgramInfo& info = program.getInfo();
    const size_t n = (size_t)sv.mCount;
    const size_t pos_bytes = n * 16, uv_bytes = (sv.mMask & LLVkAsync::SCRATCH_TEXCOORD0) ? n * 8 : 0, col_bytes = (sv.mMask & LLVkAsync::SCRATCH_COLOR) ? n * 4 : 0;
    Renderer::ScratchAlloc sa = r.allocVertexScratch(pos_bytes + uv_bytes + col_bytes);
    if (!sa.mPtr)
    {
        return false;
    }
    memcpy(sa.mPtr, sv.mBytes, pos_bytes + uv_bytes + col_bytes);
    // the attribute locations are the vertex buffer's attribute types (position 0, texcoord0 2, colour 6); one binding each, stride = element size
    struct Src { U32 mLocation; U32 mBit; VkDeviceSize mOffset; U32 mStride; S32 mSize; U8 mType; bool mNormalized; };
    const Src srcs[3] = {
        { 0, LLVkAsync::SCRATCH_POSITION, 0, 16, 3, LLVkFormat::T_F32, false },
        { 2, LLVkAsync::SCRATCH_TEXCOORD0, pos_bytes, 8, 2, LLVkFormat::T_F32, false },
        { 6, LLVkAsync::SCRATCH_COLOR, pos_bytes + uv_bytes, 4, 4, LLVkFormat::T_U8, true },
    };
    for (size_t a = 0; a < info.mAttributes.size(); ++a)
    {
        const U32 loc = info.mAttributeLocations[a];
        if (loc >= 16)
        {
            continue;
        }
        const Src* src = nullptr;
        for (const Src& s : srcs)
        {
            if (s.mLocation == loc && (sv.mMask & s.mBit))
            {
                src = &s;
            }
        }
        VertexInput::Binding b;
        VertexInput::Attribute va;
        va.mLocation = loc;
        va.mOffset = 0;
        if (src)
        {
            b.mBuffer = sa.mBuffer;
            b.mOffset = sa.mOffset + src->mOffset;
            b.mStride = src->mStride;
            const std::string& stype = info.mAttributeTypes[a];
            const char numeric = stype.empty() ? 'f' : (stype[0] == 'u' ? 'u' : (stype[0] == 'i' ? 'i' : 'f'));
            va.mFormat = vertex_format(src->mSize, src->mType, src->mNormalized, false, numeric);
        }
        else
        {
            Renderer::ScratchAlloc ca = r.allocVertexScratch(16);
            if (!ca.mPtr)
            {
                return false;
            }
            memcpy(ca.mPtr, sGL.mConstAttrib[loc], 16);
            b.mBuffer = ca.mBuffer;
            b.mOffset = ca.mOffset;
            b.mStride = 0;
            va.mFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
        }
        va.mBinding = (U32)vi.mBindings.size();
        vi.mBindings.push_back(b);
        vi.mAttributes.push_back(va);
    }
    return vi.mBindings.size() <= 16;
}

// Builds the vertex input for the current VAO and the program's attributes.
bool build_vertex_input(Renderer& r, Program& program, VertexInput& vi, std::vector<U32>& used_buffers)
{
    if (sScratchVerts)
    {
        return build_scratch_input(r, program, vi);
    }
    const BoundVertices& vao = gPassState.mBound;
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
        const BoundAttrib& at = vao.mAttrib[loc];
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
        if (LLVkPassCensus::sOn && gPassState.mUboBinding[i]) LLVkPassCensus::uboRead(i);
        auto it = gPassState.mUboBinding[i] ? sGL.mBuffers.find(gPassState.mUboBinding[i]) : sGL.mBuffers.end();
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
    LLVkBridge::gLastLap = 200;
    if (!LLVkBridge::inFrame())
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
    sDbgMatch = !sDbgFilter.empty() && program->getName().find(sDbgFilter) != std::string::npos;
    LLVkBridge::gLastLap = 201;
    {
        LLVkBridge::PhaseTimer phase(LLVkBridge::PH_D_TARGET);
        if (!sync_target())
        {
            ++sDrawsSkipped;
            ++sStats.mNoTarget;
            ++sStats.mSkippedPerProgram[program->getName() + " (no target)"];
            return false;
        }
    }
    LLVkBridge::gLastLap = 202;
    {
        LLVkBridge::PhaseTimer phase(LLVkBridge::PH_D_STATE);
        sync_draw_state(r, gPassState.mHasDepth);
    }
    LLVkBridge::gLastLap = 203;
    {
        LLVkBridge::PhaseTimer phase(LLVkBridge::PH_D_TEX);
        sync_textures(r, *program);
    }
    LLVkBridge::gLastLap = 204;
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
        const F32 size[2] = { (F32)gPassState.mCurTargetW, (F32)gPassState.mCurTargetH };
        program->setInts("ll_dbg_mode", &mode, 1);
        program->setInts("ll_dbg_sampler", &sampler, 1);
        program->setFloats("ll_dbg_size", size, 2);
    }
    if (sStatsEnabled)
    {
        LLVkBridge::PhaseTimer phase(LLVkBridge::PH_D_STATS);
        std::string key = program->getName() + " -> ";
        if (gPassState.mDrawFbo == 0) key += "SCREEN";
        for (U32 tname : gPassState.mCurTargets) key += std::to_string(tname) + " ";
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
                bound = unit_texture(unit, want);
                break;
            }
            if (bound && gFrameState.mTargets.count(bound)) key += std::to_string(bound) + " ";
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
    if (sStatsEnabled && gPassState.mDrawFbo == 0 && sStats.mBackbufferEvents.size() < 4000)
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
                e += " [" + pi.mSamplers[k].mName + "=" + std::to_string(unit_texture(unit, cls)) + "]";
            }
        }
        sStats.mBackbufferEvents.push_back(e);
    }
    {
        LLVkBridge::PhaseTimer phase(LLVkBridge::PH_D_UBO);
        sync_uniform_blocks(r, used_buffers);
    }
    LLVkBridge::gLastLap = 205;
    LLVkBridge::PhaseTimer phase(LLVkBridge::PH_D_VERTEX);
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
std::vector<U32> sResChanged;           // names of the buffers a resident fragment reads that changed since the ray membership was last looked at
U64 sResCause[3] = {};                  // writes of watched buffers by what changed: 0 epoch (storage given up or made again), 1 version (data written), 2 moves (same data in another range)
static U64 sGeoPinEpoch = 0;                 // <FS> S3: bumped when a pin a scene geometry fragment uses gets another bindless slot
void geo_buffer_gone_impl(U32 name);      // (defined with the scene geometry table below)
bool geo_in_flight_impl(U32 name, U64 frame, U64 min_listed = 0);
std::string geo_explain_address(unsigned long long at);
// <FS> the two switches of the host-block race (doc/vulkan/crash-brief/ANALYSIS.md, doc/vulkan/PLAN-texture-handles.md 17.23); both default ON since 2026-10-26 (LL_VK_RENAME_ONCE=0 / LL_VK_GEO_ROWCOPY=0)
// LL_VK_RENAME_ONCE=1: a buffer renamed in this frame is not renamed again by every further write of the frame unless a fragment listed it after the rename (the chains of 5 to 53 renames that emptied the host arena in one frame)
// LL_VK_GEO_ROWCOPY=1: the static table draws (the G-buffer types and the alpha run) read rows copied for this frame, as the rigged ones do, not the one table the worker rewrites in place while frames are in flight
static bool env_default_on(const char* name)
{
    char b[8] = {};
    return GetEnvironmentVariableA(name, b, sizeof(b)) == 0 || atoi(b) != 0;
}
static bool geo_rowcopy_on()
{
    static const bool on = env_default_on("LL_VK_GEO_ROWCOPY");       // on since 2026-10-26 (0 turns it off)
    return on;
}
static bool rename_once_on()
{
    // (not without the row copies: alone, the second write of a frame goes in place into a range an in-flight frame can see through the live table row, which is the race itself)
    static const bool on = env_default_on("LL_VK_RENAME_ONCE") && env_default_on("LL_VK_GEO_ROWCOPY");       // on since 2026-10-26 (0 turns it off)
    return on;
}
std::vector<U32> sGeoChanged;           // S3: names of the buffers a scene geometry fragment reads whose storage changed (causes 0 and 2) since the last prepare
inline void res_touched(const Buf& b, int cause)
{
    if (b.mResWatched) { sResChanged.push_back(b.mResName); ++sResCause[cause]; }
    if (b.mGeoWatched && cause != 1) { sGeoChanged.push_back(b.mResName); }
}
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
    Context::instance().flight(Context::FL_FLUSH, (unsigned long long)pending, frame_end ? 1ull : 0ull);
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
        ++b.mEpoch; res_touched(b, 0);
    }
    else if (b.mVk.valid())
    {
        ctx.destroyBuffer(b.mVk);
        ++b.mEpoch; res_touched(b, 0);
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
    ++b.mEpoch; res_touched(b, 0);
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
    U32 mChained = 0;                   // renames of a buffer that was already renamed in the same frame (the chain's second link and on)
    U32 mMaxChain = 0;                  // the longest chain of one buffer in one frame
    U32 mBlocks = 0;                    // host-written arena blocks created in the window
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
    // (every 5 s, always: the exposure of the host-block race of doc/vulkan/PLAN-texture-handles.md 17.23 is the chains: a buffer renamed again and again in one frame empties the host arena and forces a new block)
    LL_INFOS("VkGL") << "rename chains in the last 5 s: " << sRenameStats.mCount << " renames, " << sRenameStats.mChained << " of them a repeat in the same frame, the longest chain " << sRenameStats.mMaxChain
                     << (rename_once_on() ? " (LL_VK_RENAME_ONCE on)" : "") << (geo_rowcopy_on() ? " (LL_VK_GEO_ROWCOPY on)" : "") << LL_ENDL;
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
        ++b.mVersion; res_touched(b, 1);
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
    ++b.mVersion; res_touched(b, 1);
    Context& ctx = Context::instance();
    const U64 frame = ctx.getFrameNumber();
    const bool in_flight = (b.mLastUseFrame != ~0ull && frame < b.mLastUseFrame + Context::frameCount() + 1)
        || (b.mGeoWatched && geo_in_flight_impl(b.mResName, frame, (rename_once_on() && b.mRenamedFrame != ~0ull) ? b.mRenamedFrame : 0ull));
    ctx.flight(Context::FL_BUF_WRITE, (unsigned long long)name, offset, size, (unsigned long long)b.mVk.mAddress | (in_flight ? 1ull : 0ull));
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
                ctx.flight(Context::FL_BUF_RENAME, (unsigned long long)name, (unsigned long long)b.mArena.mAddress, (unsigned long long)fresh_range.mAddress, (unsigned long long)b.mSize);
                retire_range(b.mArena);
                b.mArena = fresh_range;
                b.mVk = Buffer();
                b.mVk.mBuffer = fresh_range.mBuffer;
                b.mVk.mSize = (VkDeviceSize)b.mSize;
                b.mVk.mAddress = fresh_range.mAddress;
                b.mVk.mMapped = fresh_range.mMapped;
                ++b.mMoves; res_touched(b, 2);
                if (b.mRtWatched)
                {
                    note_topology_change(b);
                }
                b.mLastUseFrame = ~0ull;
                b.mRenamesInFrame = (b.mRenamedFrame == frame) ? b.mRenamesInFrame + 1 : 1;
                sRenameStats.mChained += b.mRenamesInFrame > 1;
                sRenameStats.mMaxChain = std::max(sRenameStats.mMaxChain, b.mRenamesInFrame);
                b.mRenamedFrame = frame;
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
        sGL.mTextures[textures[i]].mName = textures[i];
    }
}

static U64 sPacketTextureGeneration = 0;     // <FS> bumped when a texture entry goes: the packet records' lookup cache starts over

namespace { void pin_lost(Tex& t, U32 name); }

static std::vector<U32> sHandleName;       // H6: the name of the texture of each handle of the 2D table (0: none), to give the readings of the draws to the texture

void destroy_textures(S32 n, const U32* textures)
{
    for (S32 i = 0; i < n; ++i)
    {
        auto it = sGL.mTextures.find(textures[i]);
        if (it != sGL.mTextures.end())
        {
            pin_lost(it->second, it->first);
            if (it->second.mHandle)
            {
                Renderer::instance().handleRelease(0, it->second.mHandle);      // (rows that still name it see the placeholder; the number comes back when the frames that may read it have finished)
                if (it->second.mHandle < sHandleName.size())
                {
                    sHandleName[it->second.mHandle] = 0;
                }
                it->second.mHandle = 0;
            }
            LLVkBridge::textureNeedForget(textures[i]);
            destroy_tex_images(it->second);
            tex_forget(it->first);
            sGL.mTextures.erase(it);
            ++gPassState.mTargetEpoch;
            ++sPacketTextureGeneration;
        }
        for (U32& n : gPassState.mUnitTex)
        {
            if (n == textures[i]) n = 0;
        }
        for (auto& f : sGL.mFbos)
        {
            for (auto& a : f.second.mColor) if (a.mTexture == textures[i]) a.mTexture = 0;
            if (f.second.mDepth.mTexture == textures[i]) f.second.mDepth.mTexture = 0;
        }
    }
}

void slot_set(unsigned unit, unsigned name)
{
    if (unit < 32)
    {
        if (LLVkPassCensus::sOn) LLVkPassCensus::unitSet(unit);
        gPassState.mUnitTex[unit] = name;
        if (name)
        {
            sGL.mTextures[name].mName = name;     // a name that was never generated is still a texture (the first edit makes it)
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
    if (level == 0 && !pixels && t->mImage.valid() && t->mImage.mLayout == VK_IMAGE_LAYOUT_UNDEFINED)
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
    if (level == 0 && !pixels && t->mImage.valid() && t->mImage.mLayout == VK_IMAGE_LAYOUT_UNDEFINED)
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
    const void* src = data;
    if (src && t->mImage.valid() && level > 0 && (U32)level >= t->mLevels)
    {
        grow_levels(*t);
    }
    if (!t->mImage.valid() || level < 0 || (U32)level >= t->mLevels)
    {
        return;
    }
    // <FS> BC7 at decode: the same bookkeeping as upload_level, which this path skipped (a chain uploaded level by level counted as holding no data and drew white, 2026-10-29)
    if ((U32)level == t->mValidLevels)
    {
        ++t->mValidLevels;
        pin_touch(*t);
    }
    if (!src)
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
    pin_touch(*t);
}

void edit_mipmap()
{
    Tex* t = edit_texture();
    if (t && t->mImage.valid() && grow_levels(*t))
    {
        Renderer::instance().generateMipmaps(t->mImage);
        t->mValidLevels = t->mLevels;
        pin_touch(*t);
    }
}

static void copy_to_texture(LLVkFormat::Target tg, S32 level, S32 xoffset, S32 yoffset, S32 zoffset, S32 x, S32 y, S32 width, S32 height, U32 source_fbo)
{
    Tex* t = edit_texture();
    Image* src = read_image(source_fbo);
    if (!t || !t->mImage.valid() || !src || !LLVkBridge::inFrame())
    {
        ++((!t || !t->mImage.valid()) ? sCopyDropTex : !src ? sCopyDropSrc : sCopyDropFrame);
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
    pin_touch(*t);
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
            sGL.mTextures[id].mName = id;      // the object is made on first use
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

// <FS> native-vk (texture handles H2): the image prepared under `staged` becomes the image of the texture `id`. The texture keeps its entry, its pin, its slot hints and its place in every
// record; what changes is the image (the old one goes through destroy_tex_images) and the state that describes it; its handle shows the new image from the next refresh (pin_touch, below).
void tex_commit_image(unsigned id, unsigned staged)
{
    if (!id || !staged || id == staged)
    {
        return;
    }
    auto sit = sGL.mTextures.find(staged);
    if (sit == sGL.mTextures.end())
    {
        return;                 // (nothing was prepared: a failed upload)
    }
    auto dit = sGL.mTextures.find(id);
    if (dit == sGL.mTextures.end())
    {
        const U32 gone = staged;
        destroy_textures(1, &gone);         // (the texture was deleted while its new image was prepared)
        return;
    }
    Tex& d = dit->second;
    Tex& st = sit->second;
    destroy_tex_images(d);
    d.mImage = st.mImage;
    d.mViews = std::move(st.mViews);
    d.mDim = st.mDim;
    d.mWidth = st.mWidth; d.mHeight = st.mHeight; d.mDepth = st.mDepth;
    d.mLevels = st.mLevels;
    d.mStore = st.mStore;
    d.mFormat = st.mFormat;
    d.mSampler = st.mSampler;
    d.mValidLevels = st.mValidLevels;
    for (int k = 0; k < 4; ++k) d.mSwizzle[k] = st.mSwizzle[k];
    d.mSwizzleSet = st.mSwizzleSet;
    d.mVkSampler = VK_NULL_HANDLE;
    d.mSamplerSig = 0;
    st.mImage = Image();                // (the image moved: the staged entry goes without destroying it)
    st.mViews.clear();
    const U32 staged_name = staged;
    destroy_textures(1, &staged_name);
    ++sCommits;
    ++gPassState.mTargetEpoch;
    ++sPacketTextureGeneration;
    pin_touch(d);
}

void tex_drop_levels(unsigned id, int levels)
{
    EditScope s(id);
    Tex* t = edit_texture();
    if (!t || levels <= 0 || !drop_levels(*t, (U32)levels))
    {
        ++sLevelDropsRefused;
        return;
    }
    ++sLevelDrops;
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
        ++gPassState.mTargetEpoch;
        if (gPassState.mDrawFbo == framebuffers[i]) gPassState.mDrawFbo = 0;
    }
}

void target_attach(unsigned fbo_name, unsigned slot, unsigned texture)
{
    if (!fbo_name)
    {
        return;
    }
    ++gPassState.mTargetEpoch;
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
    gPassState.mDrawFbo = fbo_name;
    ++gPassState.mTargetEpoch;
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
static void res_buffer_gone(U32 name);       // <FS> S2: the resident fragments' cached pointers to it are dropped

void destroy_buffers(S32 n, const U32* buffers)
{
    Context& ctx = Context::instance();
    for (S32 i = 0; i < n; ++i)
    {
        auto it = sGL.mBuffers.find(buffers[i]);
        if (it != sGL.mBuffers.end())
        {
            release_storage(it->second);
            res_buffer_gone(it->first);
            geo_buffer_gone_impl(it->first);
            sGL.mBuffers.erase(it);
            ++sPacketBufferGeneration;
        }
        if (gPassState.mBound.mArrayBuffer == buffers[i]) gPassState.mBound.mArrayBuffer = 0;
        if (gPassState.mBound.mElementBuffer == buffers[i]) gPassState.mBound.mElementBuffer = 0;
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
        ++b.mVersion; res_touched(b, 1);
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
    ++b.mVersion; res_touched(b, 1);
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
        ++it->second.mEpoch; res_touched(it->second, 0);
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
        gPassState.mUboBinding[index] = buffer;
    }
}

//-----------------------------------------------------------------------------------------------------
// F2.4: draw records (doc/vulkan/F2-draw-records.md)
//-----------------------------------------------------------------------------------------------------

void refusals(unsigned long long out[6])
{
    out[0] = sStats.mNoProgram; out[1] = sStats.mNoTarget; out[2] = sStats.mNoVertexInput; out[3] = sStats.mNoElement; out[4] = sStats.mRendererRefused; out[5] = sStats.mNotInFrame;
}

void pass_poison()
{
    const int mask = LLVkPassCensus::sPoisonMask;
    if (mask & 1) Program::poisonAll();
    if (mask & 2) for (U32 u = 0; u < 32; ++u) gPassState.mUnitTex[u] = 0;
    if (mask & 4) for (U32 i = 0; i < 4; ++i) gPassState.mUboBinding[i] = 0;
    if (mask & 8) for (auto& a : sGL.mConstAttrib) { a[0] = a[1] = a[2] = 0.f; a[3] = 1.f; }
    if (mask & 16) gPassState.mBound = BoundVertices();
    if (const LLVkState::Block* cur = (mask & 32) ? LLVkState::received() : nullptr)
    {
        LLVkState::Block b = *cur;
        b.mRaster.mCull = VK_CULL_MODE_FRONT_BIT;
        b.mDepth.mDepthTest = false;
        b.mDepth.mDepthWrite = false;
        b.mBlend.mEnable = true;
        b.mBlend.mWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_B_BIT;
        b.mScissorOn = true;
        b.mScissor[0] = b.mScissor[1] = 0;
        b.mScissor[2] = b.mScissor[3] = 1;
        LLVkState::receive(b);
    }
    if ((mask & 64) && LLVkBridge::isReady())
    {
        Renderer::instance().setProgram(nullptr);
    }
}

// CMD_DRAW2: the draw names its program and carries its vertex binding when it changed; the binding is written into the vertex input state the draw functions read
void draw_context_apply(void* program, const LLVkAsync::DrawBinding* binding)
{
    if (LLVkBridge::isReady())
    {
        Renderer& r = Renderer::instance();
        if (r.getProgram() != (Program*)program)
        {
            r.setProgram((Program*)program);
        }
    }
    if (!binding)
    {
        return;
    }
    gPassState.mBound.mArrayBuffer = binding->mArray;
    if (binding->mArray)
    {
        sGL.mBuffers[binding->mArray];
    }
    gPassState.mBound.mElementBuffer = binding->mElem;
    if (binding->mElem)
    {
        sGL.mBuffers[binding->mElem];
    }
    for (unsigned i = 0; i < 16; ++i)
    {
        BoundAttrib& a = gPassState.mBound.mAttrib[i];
        if (!(binding->mPresent & (1u << i)))
        {
            a.mEnabled = false;
            continue;
        }
        const LLVkAsync::BindAttr& e = binding->mAttr[i];
        a.mEnabled = true;
        a.mBuffer = binding->mArray;
        a.mSize = e.mSize;
        a.mType = e.mType;
        a.mNormalized = e.mNormalized != 0;
        a.mInteger = e.mInteger != 0;
        a.mStride = e.mStride;
        a.mOffset = e.mOffset;
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
    LLVkBridge::PhaseTimer draw_phase(LLVkBridge::PH_DRAW);
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
    LLVkBridge::gLastLap = 210;
    LLVkBridge::PhaseTimer apply_phase(LLVkBridge::PH_D_APPLY);
    r.setVertexInput(vi);
    r.setTopology(topology((LLVkFormat::Topology)topo));
    LLVkBridge::gLastLap = 211;
    if (r.draw((U32)count, (U32)first))
    {
        if (LLVkBridge::gPhaseTiming)
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


// <FS> native-vk diagnostic (LL_VK_UIDIFF=1): the immediate-mode draws of every frame (the UI, the name tags and floating text, the crosshair) are kept as a list of hashes (program, texture, vertex
// count, positions, texture coordinates, colours, box); a frame that differs from the frames on both sides of it (one frame A B A, or two frames A B B A) is logged with the draws that differ, tag VkUiDiff.
// Equal data with a different picture means the renderer, different data means the viewer sent it.
namespace
{
const bool sUiDiff = getenv("LL_VK_UIDIFF") != nullptr;
struct UiRec
{
    U64 mProg = 0, mPos = 0, mUv = 0, mCol = 0;
    U32 mTex0 = 0, mTex1 = 0, mCount = 0, mTopo = 0, mMask = 0, mCol0 = 0;
    F32 mMinX = 0, mMinY = 0, mMaxX = 0, mMaxY = 0;
    bool mOk = true;
};
std::vector<UiRec> sUiCur;
std::vector<UiRec> sUiHist[4];          // 0 = the frame just ended, 1 = the one before, ...
U64 sUiHash[4] = {};
U64 sUiFrames = 0, sUiEvents = 0;
std::unordered_map<U64, std::string> sUiNames;

U64 ui_fnv(const void* p, size_t n, U64 h = 1469598103934665603ull)
{
    const U8* b = (const U8*)p;
    for (size_t i = 0; i < n; ++i)
    {
        h = (h ^ b[i]) * 1099511628211ull;
    }
    return h;
}

void uidiff_note(int count, unsigned topo, unsigned mask, const void* bytes, const std::string& program, bool ok)
{
    UiRec r;
    r.mProg = std::hash<std::string>()(program);
    sUiNames.emplace(r.mProg, program);
    r.mTex0 = gPassState.mUnitTex[0];
    r.mTex1 = gPassState.mUnitTex[1];
    r.mCount = (U32)count;
    r.mTopo = topo;
    r.mMask = mask;
    r.mOk = ok;
    const U8* b = (const U8*)bytes;
    const size_t n = (size_t)count;
    r.mPos = ui_fnv(b, n * 16);
    size_t off = n * 16;
    if (mask & LLVkAsync::SCRATCH_TEXCOORD0)
    {
        r.mUv = ui_fnv(b + off, n * 8);
        off += n * 8;
    }
    if (mask & LLVkAsync::SCRATCH_COLOR)
    {
        r.mCol = ui_fnv(b + off, n * 4);
        if (n)
        {
            memcpy(&r.mCol0, b + off, 4);
        }
    }
    r.mMinX = r.mMinY = 1e30f;
    r.mMaxX = r.mMaxY = -1e30f;
    for (size_t i = 0; i < n; ++i)
    {
        const F32* v = (const F32*)(b + i * 16);
        r.mMinX = std::min(r.mMinX, v[0]); r.mMaxX = std::max(r.mMaxX, v[0]);
        r.mMinY = std::min(r.mMinY, v[1]); r.mMaxY = std::max(r.mMaxY, v[1]);
    }
    sUiCur.push_back(r);
}

U64 ui_frame_hash(const std::vector<UiRec>& v)
{
    U64 h = 1469598103934665603ull;
    for (const UiRec& r : v)
    {
        h = ui_fnv(&r.mProg, sizeof(U64), h);
        h = ui_fnv(&r.mPos, sizeof(U64), h);
        h = ui_fnv(&r.mUv, sizeof(U64), h);
        h = ui_fnv(&r.mCol, sizeof(U64), h);
        h = ui_fnv(&r.mTex0, sizeof(U32), h);
        h = ui_fnv(&r.mTex1, sizeof(U32), h);
        h = ui_fnv(&r.mCount, sizeof(U32), h);
        h = ui_fnv(&r.mOk, sizeof(bool), h);
    }
    return h;
}

void ui_describe_diff(const char* what, const std::vector<UiRec>& base, const std::vector<UiRec>& odd)
{
    std::string text;
    if (base.size() != odd.size())
    {
        text += " draw count " + std::to_string(base.size()) + " -> " + std::to_string(odd.size()) + ";";
    }
    if (base.size() != odd.size())
    {
        const std::vector<UiRec>& big = base.size() > odd.size() ? base : odd;
        const std::vector<UiRec>& small = base.size() > odd.size() ? odd : base;
        // the first position where the two lists stop matching is where the extra draw sits
        size_t k = 0;
        while (k < small.size() && big[k].mProg == small[k].mProg && big[k].mPos == small[k].mPos && big[k].mCol == small[k].mCol && big[k].mTex0 == small[k].mTex0 && big[k].mCount == small[k].mCount)
        {
            ++k;
        }
        if (k < big.size())
        {
            const UiRec& x = big[k];
            char buf[360];
            snprintf(buf, sizeof(buf), " extra draw in the %s frame at #%zu: %s tex %u/%u, %u vertices, mask %u, box (%.1f,%.1f)-(%.1f,%.1f), first colour %08x;", base.size() > odd.size() ? "base" : "odd", k,
                     sUiNames.count(x.mProg) ? sUiNames[x.mProg].c_str() : "?", x.mTex0, x.mTex1, x.mCount, x.mMask, x.mMinX, x.mMinY, x.mMaxX, x.mMaxY, x.mCol0);
            text += buf;
        }
    }
    U32 shown = 0;
    const size_t n = std::min(base.size(), odd.size());
    for (size_t i = 0; i < n && shown < 10; ++i)
    {
        const UiRec& a = base[i];
        const UiRec& b = odd[i];
        if (a.mProg == b.mProg && a.mPos == b.mPos && a.mUv == b.mUv && a.mCol == b.mCol && a.mTex0 == b.mTex0 && a.mTex1 == b.mTex1 && a.mCount == b.mCount && a.mOk == b.mOk)
        {
            continue;
        }
        ++shown;
        char buf[480];
        snprintf(buf, sizeof(buf), " [#%zu %s tex %u/%u -> %u/%u, %u -> %u vertices, %s%s%s%s%s%s box (%.0f,%.0f)-(%.0f,%.0f) -> (%.0f,%.0f)-(%.0f,%.0f), first colour %08x -> %08x]", i,
                 sUiNames.count(a.mProg) ? sUiNames[a.mProg].c_str() : "?", a.mTex0, a.mTex1, b.mTex0, b.mTex1, a.mCount, b.mCount,
                 a.mProg != b.mProg ? "program " : "", a.mPos != b.mPos ? "positions " : "", a.mUv != b.mUv ? "texcoords " : "", a.mCol != b.mCol ? "colours " : "",
                 a.mCount != b.mCount ? "count " : "", a.mOk != b.mOk ? "refused-or-not " : "", a.mMinX, a.mMinY, a.mMaxX, a.mMaxY, b.mMinX, b.mMinY, b.mMaxX, b.mMaxY, a.mCol0, b.mCol0);
        text += buf;
    }
    LL_INFOS("VkUiDiff") << what << " (frame " << sUiFrames << ", " << base.size() << " immediate draws):" << text << LL_ENDL;
}

void uidiff_frame_end()
{
    ++sUiFrames;
    sUiHist[3].swap(sUiHist[2]);
    sUiHist[2].swap(sUiHist[1]);
    sUiHist[1].swap(sUiHist[0]);
    sUiHist[0].swap(sUiCur);
    sUiCur.clear();
    sUiHash[3] = sUiHash[2]; sUiHash[2] = sUiHash[1]; sUiHash[1] = sUiHash[0];
    sUiHash[0] = ui_frame_hash(sUiHist[0]);
    if (sUiEvents < 300 && sUiFrames >= 4)
    {
        if (sUiHash[0] == sUiHash[2] && sUiHash[0] != sUiHash[1])
        {
            ++sUiEvents;
            ui_describe_diff("one frame differs", sUiHist[0], sUiHist[1]);
        }
        else if (sUiHash[0] == sUiHash[3] && sUiHash[1] == sUiHash[2] && sUiHash[0] != sUiHash[1])
        {
            ++sUiEvents;
            ui_describe_diff("two frames differ", sUiHist[0], sUiHist[1]);
        }
    }
    if (sUiFrames % 1800 == 0)
    {
        LL_INFOS("VkUiDiff") << sUiFrames << " frames seen, " << sUiEvents << " events logged" << LL_ENDL;
    }
}
}

// F3.1: a non-indexed draw with its vertices given (immediate mode); same path as draw_arrays, the vertex input comes from the given bytes
void draw_scratch(unsigned topo, int count, unsigned mask, const void* bytes)
{
    HookTimer hook_timer;
    LLVkBridge::PhaseTimer draw_phase(LLVkBridge::PH_DRAW);
    static thread_local VertexInput vi_cache;
    static thread_local std::vector<U32> used_cache;
    VertexInput& vi = vi_cache;
    std::vector<U32>& used = used_cache;
    vi.mBindings.clear();
    vi.mAttributes.clear();
    used.clear();
    ScratchVerts sv;
    sv.mCount = count;
    sv.mMask = mask;
    sv.mBytes = (const U8*)bytes;
    sScratchVerts = &sv;
    const bool ok = count > 0 && prepare_draw(vi, used);
    sScratchVerts = nullptr;
    if (!ok)
    {
        return;
    }
    Renderer& r = Renderer::instance();
    LLVkBridge::PhaseTimer apply_phase(LLVkBridge::PH_D_APPLY);
    r.setVertexInput(vi);
    r.setTopology(topology((LLVkFormat::Topology)topo));
    const bool drawn = r.draw((U32)count, 0);
    if (sUiDiff)
    {
        uidiff_note(count, topo, mask, bytes, r.getProgram()->getName(), drawn);
    }
    if (drawn)
    {
        if (LLVkBridge::gPhaseTiming)
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
    ++sDraws;
    ++sDrawsTotal;
}

void draw_elements(unsigned topo, unsigned start, unsigned end, int count, unsigned type, const void* indices)
{
    HookTimer hook_timer;
    LLVkBridge::PhaseTimer draw_phase(LLVkBridge::PH_DRAW);
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
    const BoundVertices& vao = gPassState.mBound;
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
        const BoundAttrib& pos = vao.mAttrib[0];
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
    {
        // A draw reads the index range and the vertices up to `end` from the buffers it names; a range that is not inside them (a name that was recycled or deleted since the draw was recorded, a
        // record from before a buffer changed) would read memory that belongs to something else, and a bad index there is a page fault: the device is lost. Left out and reported instead.
        static const bool check_on = getenv("LL_VK_DRAW_RANGE_CHECK") ? atoi(getenv("LL_VK_DRAW_RANGE_CHECK")) != 0 : (getenv("LL_VK_DIAG") && atoi(getenv("LL_VK_DIAG")) != 0);       // (on with LL_VK_DIAG)
        if (check_on)
        {
            const U64 index_end = (U64)indices + (U64)count * index_bytes;
            const char* why = nullptr;
            if (index_end > eit->second.mSize)
            {
                why = "index range outside the index buffer";
            }
            for (int a = 0; !why && a < 16; ++a)
            {
                const BoundAttrib& at = vao.mAttrib[a];
                if (!at.mEnabled || !at.mBuffer)
                {
                    continue;
                }
                auto vit = sGL.mBuffers.find(at.mBuffer);
                if (vit == sGL.mBuffers.end() || !vit->second.mVk.valid())
                {
                    continue;                   // (prepare_draw decides what a missing buffer means)
                }
                const U64 element = (U64)std::max<S32>(at.mSize, 1) * LLVkFormat::typeBytes(at.mType);
                const U64 stride = at.mStride ? (U64)at.mStride : element;
                if (at.mOffset + (U64)end * stride + element > vit->second.mSize)
                {
                    why = "vertex range outside a vertex buffer";
                }
            }
            if (!why && Renderer::instance().detailPass() && eit->second.mShadow.size() >= index_end && index_bytes == 2)
            {
                // the index VALUES: what the caller says is the highest vertex is not what the data holds when the buffer was recycled or is not yet written. An impostor pass is small (a few hundred
                // draws): the indices of the draw are read from the system-memory copy and the largest one is held against what every enabled vertex buffer can supply.
                const U16* idx = (const U16*)(eit->second.mShadow.data() + (U64)indices);
                U32 largest = 0;
                for (S32 i = 0; i < count; ++i)
                {
                    largest = std::max<U32>(largest, idx[i]);
                }
                for (int a = 0; !why && a < 16; ++a)
                {
                    const BoundAttrib& at = vao.mAttrib[a];
                    auto vit = at.mEnabled && at.mBuffer ? sGL.mBuffers.find(at.mBuffer) : sGL.mBuffers.end();
                    if (vit == sGL.mBuffers.end() || !vit->second.mVk.valid())
                    {
                        continue;
                    }
                    const U64 element = (U64)std::max<S32>(at.mSize, 1) * LLVkFormat::typeBytes(at.mType);
                    const U64 stride = at.mStride ? (U64)at.mStride : element;
                    const U64 capacity = vit->second.mSize > at.mOffset + element ? (vit->second.mSize - at.mOffset - element) / stride + 1 : 0;
                    if ((U64)largest >= capacity)
                    {
                        why = "an index value past the end of a vertex buffer";
                        static U32 logged_values = 0;
                        if (logged_values++ < 30)
                        {
                            LL_WARNS("Vulkan") << "impostor draw: largest index " << largest << " but attribute " << a << " (buffer " << at.mBuffer << ") holds " << capacity << " vertices (program "
                                               << Renderer::instance().getProgram()->getName() << ", " << count << " indices)" << LL_ENDL;
                        }
                    }
                }
            }
            if (why)
            {
                static U32 logged = 0;
                if (logged++ < 30)
                {
                    LL_WARNS("Vulkan") << "draw left out: " << why << " (program " << Renderer::instance().getProgram()->getName() << ", " << count << " indices at byte " << (U64)indices << " of a buffer of "
                                       << eit->second.mSize << " bytes, highest vertex " << end << ")" << LL_ENDL;
                }
                ++sDrawsSkipped;
                ++sOutOfRangeDraws;
                return;
            }
        }
    }
    ensure_buffers_current(used, vao.mElementBuffer);
    LLVkBridge::PhaseTimer apply_phase(LLVkBridge::PH_D_APPLY);
    r.setVertexInput(vi);
    r.setTopology(topology((LLVkFormat::Topology)topo));
    r.setIndexBuffer(eit->second.mVk.mBuffer, eit->second.mArena.mOffset, itype);
    if (r.drawIndexed((U32)count, (U32)((U64)indices / index_bytes), 0))
    {
        if (LLVkBridge::gPhaseTiming)
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
    static Tex* cache_tex[32768];
    static U32 cache_name[32768];
    static U64 cache_generation = ~0ull;
    if (cache_generation != sPacketTextureGeneration)
    {
        memset(cache_tex, 0, sizeof(cache_tex));
        memset(cache_name, 0, sizeof(cache_name));
        cache_generation = sPacketTextureGeneration;
    }
    const U32 slot = (name * 2654435761u) >> 17;
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

// <FS> texture handles (doc/vulkan/PLAN-texture-handles.md, H3): the handle of a texture. It is made when the texture first has an image with data (before that a row names 0, the placeholder) and then
// belongs to the texture until it is deleted: rows, pins and records keep it whatever happens to the image. What it shows follows the image: a new view is set at once (the caller sees it in the
// view id), the sampler (the levels with data, the parameters) is looked at every 16 frames, spread by name. The old descriptor stays valid until the old image is destroyed, several frames later.
static U32 texture_handle(Renderer& r, Tex& t, bool force)
{
    const bool shows = t.mImage.valid() && t.mImage.mViewType == VK_IMAGE_VIEW_TYPE_2D && t.mValidLevels > 0;
    if (!t.mHandle)
    {
        if (!shows)
        {
            return 0;
        }
        t.mHandle = r.handleNew(0);
        if (!t.mHandle)
        {
            return 0;               // (the table is full: the texture shows the placeholder, counted as refused)
        }
        if (sHandleName.size() <= t.mHandle)
        {
            sHandleName.resize(t.mHandle + 1024, 0);
        }
        sHandleName[t.mHandle] = t.mName;
    }
    if (!shows)
    {
        if (t.mHandleViewId && !(t.mImage.valid() && t.mImage.mViewType != VK_IMAGE_VIEW_TYPE_2D))
        {
            r.handleClear(0, t.mHandle);        // its image is gone and the new one has no data yet: the placeholder, not a descriptor of an image that is about to be destroyed
            t.mHandleViewId = 0;
            t.mHandleSampler = VK_NULL_HANDLE;
        }
        return t.mHandle;
    }
    if (!t.mImage.mViewId)
    {
        t.mImage.mViewId = nextViewId();
    }
    const U64 frame_now = Context::instance().getFrameNumber();
    if (!force && t.mHandleViewId == t.mImage.mViewId && ((frame_now + t.mName) & 15u) != 0)
    {
        return t.mHandle;
    }
    const VkSampler sampler = texture_sampler(r, t);
    if (t.mHandleViewId != t.mImage.mViewId || t.mHandleSampler != sampler)
    {
        if (t.mImage.mLayout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        {
            const char* prev_cause = r.setBarrierCause("texture handle");
            r.ensureLayout(t.mImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            r.setBarrierCause(prev_cause);
        }
        if (r.handleSet(0, t.mHandle, &t.mImage, sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
        {
            t.mHandleViewId = t.mImage.mViewId;
            t.mHandleSampler = sampler;
        }
    }
    return t.mHandle;
}

static std::vector<U32> sHandleDirty;       // names of textures whose handle has to be looked at again (their image, view, levels or sampler changed: pin_touch)

// once a frame (and before a scene call): the handles of the textures that changed show their new image
static void handles_refresh()
{
    if (sHandleDirty.empty())
    {
        return;
    }
    Renderer& r = Renderer::instance();
    std::vector<U32> names;
    names.swap(sHandleDirty);
    for (U32 n : names)
    {
        if (Tex* t = packet_texture(n))
        {
            t->mHandleDirty = false;
            texture_handle(r, *t, true);
        }
    }
}

// the slot of a texture in the frame's bindless table; 0 (white) for one that has no usable image, which is what the bound-texture path samples then too
static U32 packet_texture_slot(Renderer& r, U32 name)
{
    Tex* t = packet_texture(name);
    if (!t || !t->mImage.valid() || t->mImage.mViewType != VK_IMAGE_VIEW_TYPE_2D)
    {
        // diagnostic (blink after load): a texture that was drawn with its picture before and now has none (deleted name, or no image)
        if (name)
        {
            if (!t)
            {
                if (sSeenNames.erase(name)) ++sSampleNoName;
            }
            else if (!t->mImage.valid() && t->mSeenSampled)
            {
                ++sSampleNoImage;
            }
        }
        return t && t->mHandle ? t->mHandle : 0;       // (a texture whose image is being replaced keeps its handle: it shows the placeholder meanwhile)
    }
    if (!t->mSeenSampled && t->mValidLevels > 0)
    {
        t->mSeenSampled = true;
        sSeenNames.insert(name);
    }
    else if (t->mSeenSampled && t->mValidLevels == 0)
    {
        ++sSampleBlank;     // drawn with its picture before; now an image that has had no level defined: it samples as zeros (transparent black)
    }
    return texture_handle(r, *t, false);
}

//-----------------------------------------------------------------------------------------------------
// Pinned textures for the ray scene (doc/vulkan/PLAN-rt-materials.md). The material and geometry rows name a texture by PIN INDEX; a pin owns a bindless slot for as long as anything refers
// to it (the handle of the texture, which stays), and the GPU reads index -> handle from a table the ray scene keeps (Rt::setPinSlot): nothing is looked up
// per frame. What can make a slot wrong is a change of the texture: the places that change one call pin_touch, the pin is looked at again before the next scene is built. Missed
// changes are caught by checking a slice of all pins every frame (every pin once in about 32 frames).
//-----------------------------------------------------------------------------------------------------
namespace
{
struct Pin
{
    U32 mName = 0;              // GL name (0 = free entry)
    U32 mSlot = 0;              // the pinned bindless slot (0 = none yet: the texture has no image, or is not a plain 2D one)
    U32 mRefs = 0;
    U32 mGeoRefs = 0;           // S3: how many of the refs are scene geometry fragment textures (their entries are written again when the slot changes)
    bool mQueued = false;       // in sPinDirty
    bool mWaiting = false;      // in sPinWaiting
    U64 mNextTry = 0;           // a pin with no image is tried again from this frame on (the wait doubles each time, up to 256 frames)
    U32 mBackoff = 8;
};
std::vector<Pin> sPins(1);
std::unordered_map<U32, U32> sPinByName;
std::vector<U32> sPinFree;
std::vector<U32> sPinDirty;
std::vector<U32> sPinWaiting;       // pins without a slot: looked at again in turns
size_t sPinCheckCursor = 0;
U64 sPinResolves = 0, sPinEvents = 0;

void pin_queue(U32 index)
{
    if (index && index < sPins.size() && sPins[index].mName && !sPins[index].mQueued)
    {
        sPins[index].mQueued = true;
        sPinDirty.push_back(index);
    }
}

void pin_touch(Tex& t)
{
    if (t.mHandle && !t.mHandleDirty && t.mName)
    {
        t.mHandleDirty = true;
        sHandleDirty.push_back(t.mName);
    }
    if (t.mPinIndex)
    {
        ++sPinEvents;
        pin_queue(t.mPinIndex);
    }
}

// the image of a texture appeared and a pin waits for it: no backoff, resolved in this frame's pin pass
void pin_wake_name(U32 name)
{
    auto it = sPinByName.find(name);
    if (it != sPinByName.end() && it->second < sPins.size())
    {
        Pin& p = sPins[it->second];
        if (p.mName)
        {
            p.mBackoff = 8;
            p.mNextTry = 0;
            pin_queue(it->second);
        }
    }
}

// the texture of a pin as it is now: its handle (made when the texture first has data; it stays), told to the ray scene when it was not known yet
void pin_resolve(U32 index)
{
    Pin& p = sPins[index];
    p.mQueued = false;
    if (!p.mName)
    {
        return;
    }
    ++sPinResolves;
    Renderer& r = Renderer::instance();
    U32 slot = 0;
    Tex* t = packet_texture(p.mName);
    if (t)
    {
        slot = texture_handle(r, *t, true);
        if (slot)
        {
            t->mPinIndex = index;
        }
    }
    if (slot)
    {
        p.mBackoff = 8;
    }
    else
    {
        p.mNextTry = Context::instance().getFrameNumber() + p.mBackoff;
        p.mBackoff = std::min<U32>(p.mBackoff * 2, 256);
    }
    if (slot != p.mSlot)
    {
        p.mSlot = slot;
        Rt::setPinSlot(index, slot);
        if (p.mGeoRefs)
        {
            ++sGeoPinEpoch;
        }
    }
    if (!slot)
    {
        ++sPinNoSlot;
    }
    if (!slot && !p.mWaiting)
    {
        p.mWaiting = true;
        sPinWaiting.push_back(index);
    }
}

// the texture entry goes away: the pin falls back to waiting (its handle goes with the texture: rows that still name it see the placeholder)
void pin_lost(Tex& t, U32 name)
{
    const U32 index = t.mPinIndex;
    t.mPinIndex = 0;
    if (index && index < sPins.size() && sPins[index].mName == name && sPins[index].mSlot)
    {
        sPins[index].mSlot = 0;
        Rt::setPinSlot(index, 0);
        if (sPins[index].mGeoRefs)
        {
            ++sGeoPinEpoch;
        }
        if (!sPins[index].mWaiting)
        {
            sPins[index].mWaiting = true;
            sPinWaiting.push_back(index);
        }
    }
}

} // anonymous namespace (pins)

// the texture behind a view id, for the device lost report (read after the loss, on the thread that found it: no lock, the table is only read)
std::string describe_texture_of_view(U64 view_id)
{
    if (!view_id)
    {
        return "no view id";
    }
    for (const auto& kv : sGL.mTextures)
    {
        const Tex& t = kv.second;
        if (t.mImage.mViewId == view_id)
        {
            char buf[300];
            snprintf(buf, sizeof(buf), "GL texture %u: %ux%u, %u levels allocated, %u levels hold data, dim %u, layers %u, bindless slot %u, pin %u, sampler 0x%llx, swizzle %s",
                     kv.first, t.mWidth, t.mHeight, t.mLevels, t.mValidLevels, (unsigned)t.mDim, t.mImage.mLayers, t.mHandle, t.mPinIndex, (unsigned long long)(uintptr_t)t.mVkSampler, t.mSwizzleSet ? "set" : "default");
            return buf;
        }
    }
    return "no GL texture has this view (the texture was deleted or its image replaced)";
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

static void note_rt_caster_ref(U32 pass, U32 vb, U32 ib, const Buf& vbuf, const Buf& ibuf, U32 first, U32 count, U32 matrix_id, U32 kind, const F32* model, LLVk::Rt::CasterRef* ref);
static void note_rt_caster(U32 pass, U32 vb, U32 ib, const Buf& vbuf, const Buf& ibuf, U32 first, U32 count, U32 matrix_id, U32 kind, const F32* model, const Rt::AlphaInfo* alpha = nullptr, LLVk::Rt::CasterRef* ref = nullptr)
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
    if (kind == 1 || alpha)
    {
        // the alpha test (and the material tables' hit shading) reads these buffers while the lighting runs: watched for moves, and in use this frame (a CPU rewrite of a host-written range has to rename it first)
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
    Rt::noteCaster(c, ref);
}

static void note_rt_caster_ref(U32 pass, U32 vb, U32 ib, const Buf& vbuf, const Buf& ibuf, U32 first, U32 count, U32 matrix_id, U32 kind, const F32* model, LLVk::Rt::CasterRef* ref)
{
    note_rt_caster(pass, vb, ib, vbuf, ibuf, first, count, matrix_id, kind, model, nullptr, ref);
}

// a rigged caster draw for the ray scene: skinned on the GPU into a copy of its vertices (Rt::noteSkinned)
static void note_rt_skinned(U32 pass, const LLVkBridge::ShadowSkinEntry& e, const Buf& vbuf, const Buf& ibuf, U64 palette_address, const F32* model)
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
    Rt::AlphaInfo info;
    if (Rt::materialsOn() && e.mMaterial)
    {
        // ray scene materials: an opaque draw names its material and the arrays the hit shading reads (no alpha test: kind 0)
        info.mFlags = (e.mTexcoordOffset ? 8u : 0u) | (e.mColorOffset ? 16u : 0u);
        info.mTexcoordOffset = e.mTexcoordOffset;
        info.mColorOffset = e.mColorOffset;
        info.mNormalOffset = e.mNormalOffset;
        info.mMaterial = e.mMaterial;
        c.mAlpha = &info;
    }
    buffer_info_of(vbuf, c.mVi);
    buffer_info_of(ibuf, c.mIi);
    Rt::noteSkinned(c);
}

// the same for a rigged alpha masked draw: the structure of its avatar gets a geometry with the alpha test
static void note_rt_skinned_alpha(U32 pass, const LLVkBridge::ShadowAlphaEntry& e, const Buf& vbuf, const Buf& ibuf, U64 palette_address, const F32* model)
{
    Rt::AlphaInfo alpha;
    alpha.mFlags = e.mFlags & (31u | LLVkBridge::ALPHA_HARD);
    alpha.mTexcoordOffset = e.mTexcoordOffset;
    alpha.mColorOffset = e.mColorOffset;
    memcpy(alpha.mTextures, e.mTextures, sizeof(alpha.mTextures));
    alpha.mCutoff = e.mCutoff;
    if (e.mFlags & LLVkBridge::ALPHA_HARD)
    {
        memcpy(alpha.mUv, e.mUv, sizeof(alpha.mUv));         // the glTF texture transform
    }
    if (Rt::materialsOn())
    {
        alpha.mNormalOffset = e.mNormalOffset;
        alpha.mMaterial = e.mMaterial;
    }
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
static void on_rt_only_packets(const LLVkBridge::ShadowPacketsHeader& h, const U8* blob)
{
    if (!Rt::active() || h.mEntries == 0)
    {
        return;
    }
    const LLVkBridge::ShadowAlphaEntry* entries = (const LLVkBridge::ShadowAlphaEntry*)blob;
    const F32* matrices = (const F32*)(blob + sizeof(LLVkBridge::ShadowAlphaEntry) * h.mEntries);
    for (U32 i = 0; i < h.mEntries; ++i)
    {
        const LLVkBridge::ShadowAlphaEntry& e = entries[i];
        Buf* vb = packet_buffer(e.mVertexBuffer);
        Buf* ib = packet_buffer(e.mIndexBuffer);
        if (!vb || !ib || !vb->mVk.valid() || !ib->mVk.valid() || !vb->mVk.mAddress || !ib->mVk.mAddress)
        {
            continue;
        }
        const F32* model = e.mMatrix ? matrices + 16 * (size_t)e.mMatrix : nullptr;
        if (e.mFlags & LLVkBridge::ALPHA_RAY_ONLY)
        {
            Rt::AlphaInfo info;
            if (Rt::materialsOn())
            {
                info.mFlags = (e.mTexcoordOffset ? 8u : 0u) | (e.mColorOffset ? 16u : 0u);
                info.mTexcoordOffset = e.mTexcoordOffset;
                info.mColorOffset = e.mColorOffset;
                info.mNormalOffset = e.mNormalOffset;
                info.mMaterial = e.mMaterial;
            }
            note_rt_caster(h.mPass, e.mVertexBuffer, e.mIndexBuffer, *vb, *ib, e.mFirstIndex, e.mIndexCount, e.mMatrixId, 0, model, Rt::materialsOn() ? &info : nullptr);
            continue;
        }
        Rt::AlphaInfo alpha;
        alpha.mFlags = e.mFlags & (31u | 64u);
        alpha.mTexcoordOffset = e.mTexcoordOffset;
        alpha.mColorOffset = e.mColorOffset;
        memcpy(alpha.mTextures, e.mTextures, sizeof(alpha.mTextures));
        alpha.mCutoff = e.mCutoff;
        memcpy(alpha.mUv, e.mUv, sizeof(alpha.mUv));
        alpha.mNormalOffset = e.mNormalOffset;
        alpha.mMaterial = e.mMaterial;
        note_rt_caster(h.mPass, e.mVertexBuffer, e.mIndexBuffer, *vb, *ib, e.mFirstIndex, e.mIndexCount, e.mMatrixId, 1, model, &alpha);
    }
}

// S4 A2: the frame's shared palette list (kind 27): segments of whole palettes, each in scratch memory of this frame, found by the absolute float offset the entries carry
static U64 palette_address(U32 offset)
{
    for (size_t k = gFrameState.mPalSegs.size(); k-- > 0; )
    {
        const PalSeg& s = gFrameState.mPalSegs[k];
        if (offset >= s.mStart && offset < s.mStart + s.mFloats)
        {
            return s.mAddress + 4ull * (offset - s.mStart);
        }
    }
    return 0;
}

static void on_palette_segment(const LLVkBridge::ShadowPacketsHeader& h, const U8* blob)
{
    if (h.mPass != gFrameState.mPalFrame)
    {
        gFrameState.mPalFrame = h.mPass;
        gFrameState.mPalSegs.clear();
    }
    if (h.mPaletteFloats == 0)
    {
        return;
    }
    Renderer::ScratchAlloc a = Renderer::instance().allocVertexScratch(4 * (size_t)h.mPaletteFloats);
    if (!a.mPtr || !a.mAddress)
    {
        return;
    }
    memcpy(a.mPtr, blob, 4 * (size_t)h.mPaletteFloats);
    gFrameState.mPalSegs.push_back({ h.mFragment, h.mPaletteFloats, (U64)a.mAddress });
    Context::instance().flight(Context::FL_PAL_SEG, (unsigned long long)h.mFragment, (unsigned long long)h.mPaletteFloats, (unsigned long long)a.mAddress, (unsigned long long)h.mPass);
}

// the worker side of kinds 6 and 7: rigged draws of the main view, only named to the ray scene (palettes are copied into the frame's scratch memory the scene call reads)
static void on_rigged_note_packets(const LLVkBridge::ShadowPacketsHeader& h, const U8* blob)
{
    Context::instance().flight(Context::FL_RT_NOTE, (unsigned long long)h.mKind, (unsigned long long)h.mEntries, (unsigned long long)h.mPaletteFloats, (unsigned long long)h.mPass);
    const bool shared_palettes = h.mKind == 6 && h.mFragment == 1;       // S4 A2: the palettes are in the frame's shared list
    if (!Rt::active() || h.mEntries == 0 || (h.mPaletteFloats == 0 && !shared_palettes))
    {
        return;
    }
    const bool alpha_entries = h.mKind == 7;
    const size_t entry_bytes = alpha_entries ? sizeof(LLVkBridge::ShadowAlphaEntry) : sizeof(LLVkBridge::ShadowSkinEntry);
    const F32* matrices = (const F32*)(blob + entry_bytes * h.mEntries);
    Renderer& r = Renderer::instance();
    Renderer::ScratchAlloc palettes;
    if (!shared_palettes)
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
        if (alpha_entries)
        {
            const LLVkBridge::ShadowAlphaEntry& e = ((const LLVkBridge::ShadowAlphaEntry*)blob)[i];
            Buf* vb = packet_buffer(e.mVertexBuffer);
            Buf* ib = packet_buffer(e.mIndexBuffer);
            // debug: why a rigged alpha entry of the main view is turned away here (counted, logged about once in 600 packets)
            static U64 why[6] = {}, packets = 0;
            static std::string examples;
            if (i == 0 && ++packets % 600 == 0)
            {
                LL_INFOS("VkRt") << "rigged alpha packets (main view naming): no vertex buffer " << why[0] << ", no index buffer " << why[1] << ", vertex buffer not valid " << why[2] << ", index buffer not valid " << why[3]
                    << ", no address " << why[4] << ", noted " << why[5] << "; skipped examples (triangles vb/ib name):" << examples << LL_ENDL;
                memset(why, 0, sizeof(why));
                examples.clear();
            }
            int reason = -1;
            if (!vb) reason = 0;
            else if (!ib) reason = 1;
            else if (!vb->mVk.valid()) reason = 2;
            else if (!ib->mVk.valid()) reason = 3;
            else if (!vb->mVk.mAddress || !ib->mVk.mAddress) reason = 4;
            if (reason >= 0)
            {
                ++why[reason];
                if (examples.size() < 400)
                {
                    examples += " " + std::to_string(e.mIndexCount / 3) + " " + std::to_string(e.mVertexBuffer) + "/" + std::to_string(e.mIndexBuffer) + "(" + std::to_string(reason) + ")";
                }
            }
            else
            {
                ++why[5];
            }
            if (!vb || !ib || !vb->mVk.valid() || !ib->mVk.valid() || !vb->mVk.mAddress || !ib->mVk.mAddress || !(e.mFlags & LLVkBridge::ALPHA_RIGGED))
            {
                continue;
            }
            note_rt_skinned_alpha(h.mPass, e, *vb, *ib, (U64)palettes.mAddress + 4ull * e.mPaletteOffset, e.mMatrix ? matrices + 16 * (size_t)e.mMatrix : nullptr);
        }
        else
        {
            const LLVkBridge::ShadowSkinEntry& e = ((const LLVkBridge::ShadowSkinEntry*)blob)[i];
            Buf* vb = packet_buffer(e.mVertexBuffer);
            Buf* ib = packet_buffer(e.mIndexBuffer);
            if (!vb || !ib || !vb->mVk.valid() || !ib->mVk.valid() || !vb->mVk.mAddress || !ib->mVk.mAddress)
            {
                continue;
            }
            const U64 pal = shared_palettes ? palette_address(e.mPaletteOffset) : (U64)palettes.mAddress + 4ull * e.mPaletteOffset;
            if (!pal)
            {
                continue;
            }
            note_rt_skinned(h.mPass, e, *vb, *ib, pal, e.mMatrix ? matrices + 16 * (size_t)e.mMatrix : nullptr);
        }
    }
}

// the worker side of a record of the body meshes of avatars (kind 4): nothing is drawn, the ray scene skins them on the GPU
static void on_body_packets(const LLVkBridge::ShadowPacketsHeader& h, const U8* blob)
{
    if (!Rt::active() || h.mEntries == 0)
    {
        return;
    }
    const LLVkBridge::ShadowBodyEntry* entries = (const LLVkBridge::ShadowBodyEntry*)blob;
    const F32* palette_source = (const F32*)(blob + sizeof(LLVkBridge::ShadowBodyEntry) * h.mEntries);
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
        const LLVkBridge::ShadowBodyEntry& e = entries[i];
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
        Rt::AlphaInfo alpha;
        if (e.mAlphaFlags)
        {
            // hair, eyelashes, skirt: one texture, the alpha cut at the cutoff (the old shadow shader's semi-transparent dithering is dropped)
            c.mKind = 1;
            alpha.mFlags = e.mAlphaFlags;
            alpha.mTexcoordOffset = e.mTexcoordOffset;
            alpha.mTextures[0] = e.mTexture;
            alpha.mCutoff = e.mCutoff;
            c.mAlpha = &alpha;
        }
        if (Rt::materialsOn() && e.mMaterial)
        {
            // ray scene materials: the arrays the hit shading reads (the texture is in the material); an alpha masked part keeps its alpha test fields
            if (!e.mAlphaFlags)
            {
                alpha.mFlags = 0;
            }
            alpha.mFlags |= e.mTexcoordOffset ? 8u : 0u;
            alpha.mTexcoordOffset = e.mTexcoordOffset;
            alpha.mNormalOffset = e.mNormalOffset;
            alpha.mMaterial = e.mMaterial;
            c.mAlpha = &alpha;
        }
        buffer_info_of(*vb, c.mVi);
        buffer_info_of(*ib, c.mIi);
        Rt::noteSkinned(c);
    }
}

// the worker side of a record of alpha masked casters (shadowAlphaPullV.glsl / shadowAlphaPullF.glsl): the entries also name their textures
static void on_shadow_alpha_packets(const LLVkBridge::ShadowPacketsHeader& h, const U8* blob)
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
    const LLVkBridge::ShadowAlphaEntry* entries = (const LLVkBridge::ShadowAlphaEntry*)blob;
    const F32* matrices = (const F32*)(blob + sizeof(LLVkBridge::ShadowAlphaEntry) * h.mEntries);
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
        const LLVkBridge::ShadowAlphaEntry& e = entries[i];
        AlphaGpu& g = out[i];
        memset(&g, 0, sizeof(g));
        Buf* vb = packet_buffer(e.mVertexBuffer);
        Buf* ib = packet_buffer(e.mIndexBuffer);
        if (!vb || !ib || !vb->mVk.valid() || !ib->mVk.valid() || !vb->mVk.mAddress || !ib->mVk.mAddress)
        {
            continue;       // not created (yet): the draw is skipped, as a draw from an empty buffer is
        }
        if (Rt::active() && !(e.mFlags & LLVkBridge::ALPHA_RIGGED))
        {
            Rt::AlphaInfo alpha;
            alpha.mFlags = e.mFlags & 31u;      // fullbright, tree, one texture, texcoords, colours: the bits mean the same in the ray query shader
            alpha.mTexcoordOffset = e.mTexcoordOffset;
            alpha.mColorOffset = e.mColorOffset;
            memcpy(alpha.mTextures, e.mTextures, sizeof(alpha.mTextures));
            alpha.mCutoff = e.mCutoff;
            alpha.mNormalOffset = e.mNormalOffset;
            alpha.mMaterial = e.mMaterial;
            note_rt_caster(h.mPass, e.mVertexBuffer, e.mIndexBuffer, *vb, *ib, e.mFirstIndex, e.mIndexCount, e.mMatrixId, 1, e.mMatrix ? matrices + 16 * (size_t)e.mMatrix : nullptr, &alpha);
        }
        const U64 pa = (U64)vb->mVk.mAddress, ia = (U64)ib->mVk.mAddress;
        g.mPositions[0] = (U32)pa;
        g.mPositions[1] = (U32)(pa >> 32);
        g.mIndices[0] = (U32)ia;
        g.mIndices[1] = (U32)(ia >> 32);
        U32 flags = e.mFlags;
        if (flags & LLVkBridge::ALPHA_HAS_TEXCOORD)
        {
            const U64 a = pa + e.mTexcoordOffset;
            g.mTexcoords[0] = (U32)a;
            g.mTexcoords[1] = (U32)(a >> 32);
        }
        if (flags & LLVkBridge::ALPHA_HAS_COLOR)
        {
            const U64 a = pa + e.mColorOffset;
            g.mColors[0] = (U32)a;
            g.mColors[1] = (U32)(a >> 32);
        }
        g.mFirst = e.mFirstIndex;
        g.mCount = e.mIndexCount;
        g.mModel = e.mMatrix;
        g.mFlags = flags;
        if (flags & LLVkBridge::ALPHA_RIGGED)
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
static void on_geometry_packets(const LLVkBridge::ShadowPacketsHeader& h, const U8* blob, bool rigged = false, bool indexed = false, bool alpha = false)
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
    // S3 G0 (LL_VK_SCENE_TIMES=1): what this record costs the worker: the table build (entries, buffer and texture lookups), and the rest (matrices, state, draw)
    static const bool timed = getenv("LL_VK_SCENE_TIMES") != nullptr;
    static U64 ns_table_a[2] = {}, ns_rest_a[2] = {}, records_a[2] = {}, entries_total_a[2] = {};       // [0] static, [1] rigged (S4 A1, kind 25)
    const auto t_begin = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
    std::chrono::steady_clock::time_point t_table_end = t_begin;
    struct Account
    {
        bool mOn; std::chrono::steady_clock::time_point mBegin, &mTableEnd; U32 mEntries; bool mRigged;
        ~Account()
        {
            if (!mOn) return;
            const auto now = std::chrono::steady_clock::now();
            const int k = mRigged ? 1 : 0;
            U64& ns_table = ns_table_a[k]; U64& ns_rest = ns_rest_a[k]; U64& records = records_a[k]; U64& entries_total = entries_total_a[k];
            ns_table += (U64)std::chrono::duration_cast<std::chrono::nanoseconds>(mTableEnd - mBegin).count();
            ns_rest += (U64)std::chrono::duration_cast<std::chrono::nanoseconds>(now - mTableEnd).count();
            entries_total += mEntries;
            if (++records % 3000 == 0)
            {
                LL_INFOS("VkScene") << (mRigged ? "rigged " : "") << "geometry packets, worker time per record: table build " << (double)ns_table / 1000.0 / records << " us, matrices + state + draw " << (double)ns_rest / 1000.0 / records
                    << " us, " << (double)entries_total / records << " entries a record (" << records << " records)" << LL_ENDL;
            }
        }
    } account = { timed, t_begin, t_table_end, h.mEntries, rigged };
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
        static std::set<std::string> told;
        const std::string why = program ? program->getName() + (!program->isValid() ? ": program not valid" : !program->hasUniform("ll_packet_table") ? ": no packet table" : ": bindless not available") : std::string("no program");
        if (h.mEntries && told.insert(why).second)
        {
            LL_WARNS("VkScene") << "a geometry packet record is not drawn (" << why << ")" << LL_ENDL;
        }
        return;
    }
    const LLVkBridge::GeoEntry* entries = (const LLVkBridge::GeoEntry*)blob;
    const F32* matrices = (const F32*)(blob + sizeof(LLVkBridge::GeoEntry) * h.mEntries);
    // S4 A1: a rigged entry is the 128 bytes of GeoGpu and 32 more (weights, palette, joints), the palettes follow the matrices in the blob
    const size_t stride = 160;       // (S4 B: every record entry has the rigged tail, the pull shaders read one layout)
    const bool own_palettes = rigged && !alpha && h.mPaletteFloats > 0;       // else (S4 A2) they are in the frame's shared list (always for the alpha kind 32: mPaletteFloats counts its PBR rows)
    const U32 texmat_count = (!alpha && !own_palettes) ? h.mFragment : 0;       // S6.6b: kinds 3 / 25 / 26 carry the texture matrices of their draws after the matrices (kind 32: its own layout below)
    Renderer::ScratchAlloc table = r.allocVertexScratch(stride * (size_t)h.mEntries);
    Renderer::ScratchAlloc models = r.allocVertexScratch(176 * (size_t)std::max<U32>(h.mMatrices, 1));
    Renderer::ScratchAlloc palettes;
    if (own_palettes)
    {
        palettes = r.allocVertexScratch(4 * (size_t)h.mPaletteFloats);
    }
    if (!table.mPtr || !models.mPtr || !table.mAddress || !models.mAddress || (own_palettes && (!palettes.mPtr || !palettes.mAddress)))
    {
        return;
    }
    if (own_palettes)
    {
        memcpy(palettes.mPtr, matrices + 44 * (size_t)h.mMatrices, 4 * (size_t)h.mPaletteFloats);
    }
    const U64 frame_now = Context::instance().getFrameNumber();
    counts.assign(h.mEntries, 0);
    static thread_local std::vector<Buf*> index_buffers;
    index_buffers.assign(h.mEntries, nullptr);
    for (U32 i = 0; i < h.mEntries; ++i)
    {
        const LLVkBridge::GeoEntry& e = entries[i];
        U8* raw = (U8*)table.mPtr + stride * i;
        memset(raw, 0, stride);
        GeoGpu& g = *(GeoGpu*)raw;
        const bool no_index = (e.mFlags & LLVkBridge::GEO_NO_INDEX) != 0;       // S6.6: a drawArrays: no index buffer
        Buf* vb = packet_buffer(e.mVertexBuffer);
        Buf* ib = no_index ? nullptr : packet_buffer(e.mIndexBuffer);
        if (!vb || !vb->mVk.valid() || !vb->mVk.mAddress || (!no_index && (!ib || !ib->mVk.valid() || !ib->mVk.mAddress)))
        {
            // (said once per program and reason: a draw that goes missing is not a silent one)
            static std::set<std::string> told;
            const std::string why = program->getName() + (!vb ? ": vertex buffer unknown" : !vb->mVk.valid() ? ": vertex buffer not created" : !vb->mVk.mAddress ? ": vertex buffer without an address"
                : (!no_index && !ib) ? ": index buffer unknown" : (!no_index && !ib->mVk.valid()) ? ": index buffer not created" : ": index buffer without an address");
            if (told.insert(why).second)
            {
                LL_WARNS("VkScene") << "a geometry packet entry is skipped (" << why << ")" << LL_ENDL;
            }
            continue;       // not created (yet): the draw is skipped, as a draw from an empty buffer is
        }
        const U64 pa = (U64)vb->mVk.mAddress, ia = no_index ? 0 : (U64)ib->mVk.mAddress;
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
        if (alpha)
        {
            g.mPad[0] = e.mPad3;                                // S5f F3: the texture matrix (0 = none)
            g.mPad[1] = e.mPbrRow;                              // ... the PBR row
        }
        else if (texmat_count)
        {
            g.mPad[0] = e.mPad3;                                // S6.6b: the texture matrix of the draw (0 = none)
        }
        memcpy(&g.mPad2, &e.mEmissiveOffset, sizeof(U32));       // ... the emissive array (the glow draw; S6 X2: the glow pass's own draws too)
        if (!rigged && e.mWeightOffset)
        {
            U32* x = (U32*)(raw + 128);         // S6.6d: a body mesh: the weights are read by address, its palette is a uniform of the program
            const U64 wa = pa + e.mWeightOffset;
            x[0] = (U32)wa;
            x[1] = (U32)(wa >> 32);
        }
        if (rigged)
        {
            U32* x = (U32*)(raw + 128);
            const U64 wa = pa + e.mWeightOffset, pal = own_palettes ? (U64)palettes.mAddress + 4ull * e.mPaletteOffset : palette_address(e.mPaletteOffset);
            if (!pal)
            {
                continue;       // (a palette the frame's list does not hold: not drawn; counts[i] stays 0)
            }
            x[0] = (U32)wa;
            x[1] = (U32)(wa >> 32);
            x[2] = (U32)pal;
            x[3] = (U32)(pal >> 32);
            x[4] = e.mJoints;
        }
        if (probe_on())
        {
            // positions are the vec4 array at the start of the vertex buffer, the normals (or the next array) follow it
            const U32 next = e.mNormalOffset ? e.mNormalOffset : e.mTexcoordOffset;
            const U32 verts = next ? next / 16 : (U32)(vb->mVk.mSize / 16);
            if (ib)
            {
                probe_note(e.mVertexBuffer, e.mIndexBuffer, *vb, *ib, e.mFirstIndex, e.mIndexCount, verts ? verts - 1 : 0);
            }
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
        index_buffers[i] = ib;
        vb->mLastUseFrame = frame_now;
        if (ib)
        {
            ib->mLastUseFrame = frame_now;
        }
        if (packet_used.size() < 2 || packet_used[packet_used.size() - 2] != e.mVertexBuffer || packet_used.back() != e.mIndexBuffer)
        {
            packet_used.push_back(e.mVertexBuffer);
            packet_used.push_back(e.mIndexBuffer);
        }
    }
    if (timed)
    {
        t_table_end = std::chrono::steady_clock::now();
    }
    memcpy(models.mPtr, matrices, 176 * (size_t)h.mMatrices);

    const U64 table_address = (U64)table.mAddress, models_address = (U64)models.mAddress;
    const S32 table_words[2] = { (S32)(U32)table_address, (S32)(U32)(table_address >> 32) };
    const S32 models_words[2] = { (S32)(U32)models_address, (S32)(U32)(models_address >> 32) };
    program->setInts("ll_packet_table", table_words, 2);
    program->setInts("ll_packet_models", models_words, 2);
    if (texmat_count && program->hasUniform("ll_packet_texmats"))
    {
        // S6.6b: the texture matrices of the draws of this record (number 0 = identity)
        const F32* texmat_src = matrices + 44 * (size_t)h.mMatrices;
        Renderer::ScratchAlloc texmats = r.allocVertexScratch(64 * ((size_t)texmat_count + 1));
        if (!texmats.mPtr || !texmats.mAddress)
        {
            return;
        }
        F32* tm = (F32*)texmats.mPtr;
        LLVkTexAnim::writeSlot0(tm);
        memcpy(tm + 16, texmat_src, 64 * (size_t)texmat_count);
        const U64 texmats_address = (U64)texmats.mAddress;
        const S32 texmats_words[2] = { (S32)(U32)texmats_address, (S32)(U32)(texmats_address >> 32) };
        program->setInts("ll_packet_texmats", texmats_words, 2);
    }
    if (alpha)
    {
        // S5f F3: the record's texture matrices (number 0 = identity) and PBR rows, in scratch memory of their own
        const U32 texmat_count = h.mFragment, pbr_count = h.mPaletteFloats;
        const F32* texmat_src = matrices + 44 * (size_t)h.mMatrices;
        const F32* pbr_src = texmat_src + 16 * (size_t)texmat_count;
        Renderer::ScratchAlloc texmats = r.allocVertexScratch(64 * ((size_t)texmat_count + 1));
        Renderer::ScratchAlloc pbr_rows = r.allocVertexScratch(160 * (size_t)std::max<U32>(pbr_count, 1));
        if (!texmats.mPtr || !texmats.mAddress || !pbr_rows.mPtr || !pbr_rows.mAddress)
        {
            return;
        }
        F32* tm = (F32*)texmats.mPtr;
        LLVkTexAnim::writeSlot0(tm);
        memcpy(tm + 16, texmat_src, 64 * (size_t)texmat_count);
        memset(pbr_rows.mPtr, 0, 160 * (size_t)std::max<U32>(pbr_count, 1));
        memcpy(pbr_rows.mPtr, pbr_src, 160 * (size_t)pbr_count);
        const U64 texmats_address = (U64)texmats.mAddress, pbr_address = (U64)pbr_rows.mAddress;
        const S32 texmats_words[2] = { (S32)(U32)texmats_address, (S32)(U32)(texmats_address >> 32) };
        const S32 pbr_words[2] = { (S32)(U32)pbr_address, (S32)(U32)(pbr_address >> 32) };
        if (program->hasUniform("ll_packet_texmats"))
        {
            program->setInts("ll_packet_texmats", texmats_words, 2);
        }
        if (program->hasUniform("ll_packet_pbr"))
        {
            program->setInts("ll_packet_pbr", pbr_words, 2);
        }
    }

    if (!prepare_draw(vi, used_cache))
    {
        static std::set<std::string> told;
        if (told.insert(program->getName()).second)
        {
            LL_WARNS("VkScene") << "a geometry packet record is not drawn (" << program->getName() << ": the draw could not be prepared)" << LL_ENDL;
        }
        return;
    }
    ensure_buffers_current(packet_used, 0);
    r.setVertexInput(vi);
    r.setTopology(topology(h.mEntries && (entries[0].mFlags & LLVkBridge::GEO_STRIP) ? LLVkFormat::TOP_TRIANGLE_STRIP : LLVkFormat::TOP_TRIANGLES));      // (S6.6: a strip is a record of one entry)
    r.bindlessFlush();

    if (indexed)
    {
        // S4 A1: the hardware reads the indices (the vertex shader gets the index as its vertex number: post-transform cache reuse); one indirect draw per run of entries that share an index buffer
        static thread_local std::vector<VkDrawIndexedIndirectCommand> icmds;
        icmds.assign(h.mEntries, VkDrawIndexedIndirectCommand{ 0, 1, 0, 0, 0 });
        for (U32 i = 0; i < h.mEntries; ++i)
        {
            icmds[i] = { counts[i], 1u, entries[i].mFirstIndex, 0, i };
        }
        const auto& icaps = Context::instance().getCaps();
        Renderer::ScratchAlloc idraw;
        if (icaps.mMultiDrawIndirect && icaps.mDrawIndirectFirstInstance)
        {
            idraw = r.allocVertexScratch(sizeof(VkDrawIndexedIndirectCommand) * (size_t)h.mEntries);
            if (idraw.mPtr)
            {
                memcpy(idraw.mPtr, icmds.data(), sizeof(VkDrawIndexedIndirectCommand) * (size_t)h.mEntries);
            }
        }
        U32 drawn = 0, runs = 0;
        for (U32 a = 0; a < h.mEntries; )
        {
            if (!index_buffers[a] || !counts[a])
            {
                ++a;
                continue;
            }
            U32 b = a + 1;
            while (b < h.mEntries && index_buffers[b] == index_buffers[a])
            {
                ++b;        // (an entry with nothing to draw has a zero count: the draw is empty)
            }
            r.setIndexBuffer(index_buffers[a]->mVk.mBuffer, index_buffers[a]->mArena.mOffset, VK_INDEX_TYPE_UINT16);
            if (r.drawPacketsIndexed(icmds.data() + a, b - a, idraw.mPtr ? idraw.mBuffer : VK_NULL_HANDLE, idraw.mPtr ? idraw.mOffset + (VkDeviceSize)a * sizeof(VkDrawIndexedIndirectCommand) : 0))
            {
                drawn += b - a;
                ++runs;
            }
            a = b;
        }
        if (drawn)
        {
            if (sStatsEnabled)
            {
                sStats.mDone += drawn;
            }
        }
        else
        {
            ++sStats.mRendererRefused;
        }
        sDraws += runs;
        sDrawsTotal += runs;
        return;
    }

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
static void on_shadow_skin_packets(const LLVkBridge::ShadowPacketsHeader& h, const U8* blob)
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
    const bool shared_palettes = h.mFragment == 1;       // S4 A2: the palettes are in the frame's shared list (kind 27)
    if (h.mEntries == 0 || (h.mPaletteFloats == 0 && !shared_palettes) || !program || !program->isValid() || !program->hasUniform("ll_packet_table"))
    {
        ++sStats.mNoProgram;
        return;
    }
    const LLVkBridge::ShadowSkinEntry* entries = (const LLVkBridge::ShadowSkinEntry*)blob;
    const F32* matrices = (const F32*)(blob + sizeof(LLVkBridge::ShadowSkinEntry) * h.mEntries);
    Renderer::ScratchAlloc table = r.allocVertexScratch(sizeof(SkinGpu) * (size_t)h.mEntries);
    Renderer::ScratchAlloc models = r.allocVertexScratch(64 * (size_t)std::max<U32>(h.mMatrices, 1));
    Renderer::ScratchAlloc palettes;
    if (!shared_palettes)
    {
        palettes = r.allocVertexScratch(4 * (size_t)h.mPaletteFloats);
    }
    if (!table.mPtr || !models.mPtr || !table.mAddress || !models.mAddress || (!shared_palettes && (!palettes.mPtr || !palettes.mAddress)))
    {
        return;
    }
    if (!shared_palettes)
    {
        memcpy(palettes.mPtr, matrices + 16 * (size_t)h.mMatrices, 4 * (size_t)h.mPaletteFloats);
    }
    const U64 frame_now = Context::instance().getFrameNumber();
    counts.assign(h.mEntries, 0);
    SkinGpu* out = (SkinGpu*)table.mPtr;
    for (U32 i = 0; i < h.mEntries; ++i)
    {
        const LLVkBridge::ShadowSkinEntry& e = entries[i];
        SkinGpu& g = out[i];
        memset(&g, 0, sizeof(g));
        Buf* vb = packet_buffer(e.mVertexBuffer);
        Buf* ib = packet_buffer(e.mIndexBuffer);
        if (!vb || !ib || !vb->mVk.valid() || !ib->mVk.valid() || !vb->mVk.mAddress || !ib->mVk.mAddress)
        {
            continue;       // not created (yet): the draw is skipped, as a draw from an empty buffer is
        }
        const U64 pa = (U64)vb->mVk.mAddress, ia = (U64)ib->mVk.mAddress;
        const U64 wa = pa + e.mWeightOffset, pal = shared_palettes ? palette_address(e.mPaletteOffset) : (U64)palettes.mAddress + 4ull * e.mPaletteOffset;
        if (!pal)
        {
            continue;       // (a palette the list does not hold: not drawn)
        }
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

// <FS> Avatar motion vectors (doc/vulkan/PLAN-ray-tracing.md, item 6): the worker side of a record of rigged draws for the velocity pass (kind 8, avatarVelocityPullV.glsl). Entries as kind 2
// (ShadowSkinEntry; no model matrices), then the palettes of this frame, then the palettes of the frame before, at the same offsets. The viewer sets the other uniforms the normal way.
static void on_velocity_packets(const LLVkBridge::ShadowPacketsHeader& h, const U8* blob)
{
    struct VelocityGpu { U32 mPositions[2]; U32 mIndices[2]; U32 mWeights[2]; U32 mPalette[2]; U32 mPrevPalette[2]; U32 mFirst, mJoints; };
    static_assert(sizeof(VelocityGpu) == 48, "velocity packet entry");
    static thread_local VertexInput vi_cache;
    static thread_local std::vector<U32> used_cache, packet_used;
    static thread_local std::vector<U32> counts;
    static thread_local std::vector<Buf*> velocity_index_buffers;
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
    const LLVkBridge::ShadowSkinEntry* entries = (const LLVkBridge::ShadowSkinEntry*)blob;
    const F32* palette_data = (const F32*)(blob + sizeof(LLVkBridge::ShadowSkinEntry) * h.mEntries);
    Renderer::ScratchAlloc table = r.allocVertexScratch(sizeof(VelocityGpu) * (size_t)h.mEntries);
    Renderer::ScratchAlloc palettes = r.allocVertexScratch(8 * (size_t)h.mPaletteFloats);      // this frame's, then last frame's
    if (!table.mPtr || !palettes.mPtr || !table.mAddress || !palettes.mAddress)
    {
        return;
    }
    memcpy(palettes.mPtr, palette_data, 8 * (size_t)h.mPaletteFloats);
    const U64 frame_now = Context::instance().getFrameNumber();
    counts.assign(h.mEntries, 0);
    velocity_index_buffers.assign(h.mEntries, nullptr);
    VelocityGpu* out = (VelocityGpu*)table.mPtr;
    for (U32 i = 0; i < h.mEntries; ++i)
    {
        const LLVkBridge::ShadowSkinEntry& e = entries[i];
        VelocityGpu& g = out[i];
        memset(&g, 0, sizeof(g));
        Buf* vb = packet_buffer(e.mVertexBuffer);
        Buf* ib = packet_buffer(e.mIndexBuffer);
        if (!vb || !ib || !vb->mVk.valid() || !ib->mVk.valid() || !vb->mVk.mAddress || !ib->mVk.mAddress)
        {
            continue;       // not created (yet): the draw is skipped, as a draw from an empty buffer is
        }
        const U64 pa = (U64)vb->mVk.mAddress, ia = (U64)ib->mVk.mAddress;
        const U64 wa = pa + e.mWeightOffset;
        const U64 pal = (U64)palettes.mAddress + 4ull * e.mPaletteOffset;
        const U64 prev = pal + 4ull * h.mPaletteFloats;
        g.mPositions[0] = (U32)pa;
        g.mPositions[1] = (U32)(pa >> 32);
        g.mIndices[0] = (U32)ia;
        g.mIndices[1] = (U32)(ia >> 32);
        g.mWeights[0] = (U32)wa;
        g.mWeights[1] = (U32)(wa >> 32);
        g.mPalette[0] = (U32)pal;
        g.mPalette[1] = (U32)(pal >> 32);
        g.mPrevPalette[0] = (U32)prev;
        g.mPrevPalette[1] = (U32)(prev >> 32);
        g.mFirst = e.mFirstIndex;
        g.mJoints = e.mJoints;
        counts[i] = e.mIndexCount;
        velocity_index_buffers[i] = ib;
        vb->mLastUseFrame = frame_now;
        ib->mLastUseFrame = frame_now;
        if (packet_used.size() < 2 || packet_used[packet_used.size() - 2] != e.mVertexBuffer || packet_used.back() != e.mIndexBuffer)
        {
            packet_used.push_back(e.mVertexBuffer);
            packet_used.push_back(e.mIndexBuffer);
        }
    }

    const U64 table_address = (U64)table.mAddress;
    const S32 table_words[2] = { (S32)(U32)table_address, (S32)(U32)(table_address >> 32) };
    program->setInts("ll_packet_table", table_words, 2);
    program->setMatrices("ll_view_proj", h.mViewProj, 4, 4, 1);
    // rt-debug.txt velidx=1: the entries are drawn indexed, a run for each index buffer (the hardware reads the indices: the vertex shader runs once for a vertex, not once for each corner of a triangle)
    const bool indexed_draw = LLVk::Rt::velIdx() != 0;
    if (program->hasUniform("vel_indexed"))
    {
        const S32 flag = indexed_draw ? 1 : 0;
        program->setInts("vel_indexed", &flag, 1);
    }

    if (!prepare_draw(vi, used_cache))
    {
        return;
    }
    ensure_buffers_current(packet_used, 0);
    r.setVertexInput(vi);
    r.setTopology(topology(LLVkFormat::TOP_TRIANGLES));
    if (r.bindlessAvailable())
    {
        r.bindlessFlush();      // the depth texture the fragment shader reads
    }

    Renderer::ScratchAlloc commands;
    const auto& caps = Context::instance().getCaps();
    if (indexed_draw)
    {
        static thread_local std::vector<VkDrawIndexedIndirectCommand> icmds;
        icmds.assign(h.mEntries, VkDrawIndexedIndirectCommand{ 0, 1, 0, 0, 0 });
        for (U32 i = 0; i < h.mEntries; ++i)
        {
            icmds[i] = { counts[i], 1u, entries[i].mFirstIndex, 0, i };
        }
        Renderer::ScratchAlloc idraw;
        if (caps.mMultiDrawIndirect && caps.mDrawIndirectFirstInstance)
        {
            idraw = r.allocVertexScratch(sizeof(VkDrawIndexedIndirectCommand) * (size_t)h.mEntries);
            if (idraw.mPtr)
            {
                memcpy(idraw.mPtr, icmds.data(), sizeof(VkDrawIndexedIndirectCommand) * (size_t)h.mEntries);
            }
        }
        U32 drawn = 0;
        for (U32 a = 0; a < h.mEntries; )
        {
            if (!velocity_index_buffers[a] || !counts[a])
            {
                ++a;
                continue;
            }
            U32 b = a + 1;
            while (b < h.mEntries && velocity_index_buffers[b] == velocity_index_buffers[a])
            {
                ++b;
            }
            r.setIndexBuffer(velocity_index_buffers[a]->mVk.mBuffer, velocity_index_buffers[a]->mArena.mOffset, VK_INDEX_TYPE_UINT16);
            if (r.drawPacketsIndexed(icmds.data() + a, b - a, idraw.mPtr ? idraw.mBuffer : VK_NULL_HANDLE, idraw.mPtr ? idraw.mOffset + (VkDeviceSize)a * sizeof(VkDrawIndexedIndirectCommand) : 0))
            {
                drawn += b - a;
            }
            a = b;
        }
        if (drawn)
        {
            if (sStatsEnabled)
            {
                sStats.mDone += drawn;
            }
        }
        else
        {
            ++sStats.mRendererRefused;
        }
        sDraws += h.mEntries;
        sDrawsTotal += h.mEntries;
        return;
    }
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

// <FS> Avatar motion vectors, step 1b (doc/vulkan/PLAN-ray-tracing.md, item 6): the worker side of a record of draws that move as a whole (kind 9, objectVelocityPullV.glsl). Entries as the static
// shadow casters (ShadowPacketEntry), then the model matrices of this frame (mMatrices of them, 16 floats), then the ones of the frame before (same numbers).
static void on_object_velocity_packets(const LLVkBridge::ShadowPacketsHeader& h, const U8* blob)
{
    struct ObjectGpu { U32 mPositions[2]; U32 mIndices[2]; U32 mFirst, mCount, mModel, mPad; };
    static_assert(sizeof(ObjectGpu) == 32, "object velocity packet entry");
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
    if (h.mEntries == 0 || h.mMatrices == 0 || !program || !program->isValid() || !program->hasUniform("ll_packet_table"))
    {
        ++sStats.mNoProgram;
        return;
    }
    const LLVkBridge::ShadowPacketEntry* entries = (const LLVkBridge::ShadowPacketEntry*)blob;
    const F32* matrices = (const F32*)(blob + sizeof(LLVkBridge::ShadowPacketEntry) * h.mEntries);
    Renderer::ScratchAlloc table = r.allocVertexScratch(sizeof(ObjectGpu) * (size_t)h.mEntries);
    Renderer::ScratchAlloc models = r.allocVertexScratch(128 * (size_t)h.mMatrices);      // this frame's, then last frame's
    if (!table.mPtr || !models.mPtr || !table.mAddress || !models.mAddress)
    {
        return;
    }
    memcpy(models.mPtr, matrices, 128 * (size_t)h.mMatrices);
    const U64 frame_now = Context::instance().getFrameNumber();
    counts.assign(h.mEntries, 0);
    ObjectGpu* out = (ObjectGpu*)table.mPtr;
    for (U32 i = 0; i < h.mEntries; ++i)
    {
        const LLVkBridge::ShadowPacketEntry& e = entries[i];
        ObjectGpu& g = out[i];
        memset(&g, 0, sizeof(g));
        Buf* vb = packet_buffer(e.mVertexBuffer);
        Buf* ib = packet_buffer(e.mIndexBuffer);
        if (!vb || !ib || !vb->mVk.valid() || !ib->mVk.valid() || !vb->mVk.mAddress || !ib->mVk.mAddress)
        {
            continue;       // not created (yet): the draw is skipped, as a draw from an empty buffer is
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
        if (packet_used.size() < 2 || packet_used[packet_used.size() - 2] != e.mVertexBuffer || packet_used.back() != e.mIndexBuffer)
        {
            packet_used.push_back(e.mVertexBuffer);
            packet_used.push_back(e.mIndexBuffer);
        }
    }

    const U64 table_address = (U64)table.mAddress, models_address = (U64)models.mAddress, prev_address = models_address + 64ull * h.mMatrices;
    const S32 table_words[2] = { (S32)(U32)table_address, (S32)(U32)(table_address >> 32) };
    const S32 models_words[2] = { (S32)(U32)models_address, (S32)(U32)(models_address >> 32) };
    const S32 prev_words[2] = { (S32)(U32)prev_address, (S32)(U32)(prev_address >> 32) };
    program->setInts("ll_packet_table", table_words, 2);
    program->setInts("ll_packet_models", models_words, 2);
    program->setInts("ll_packet_prev_models", prev_words, 2);
    program->setMatrices("ll_view_proj", h.mViewProj, 4, 4, 1);

    if (!prepare_draw(vi, used_cache))
    {
        return;
    }
    ensure_buffers_current(packet_used, 0);
    r.setVertexInput(vi);
    r.setTopology(topology(LLVkFormat::TOP_TRIANGLES));
    if (r.bindlessAvailable())
    {
        r.bindlessFlush();      // the depth texture the fragment shader reads
    }

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


// <FS> S2 resident fragments (doc/vulkan/S2-handoff.md): the entries of static groups live in a GPU table (the same 32 byte entries the packet shader reads, plus a bounding sphere each), kept by
// slot; a compute pass tests every entry against the cascade's frustum planes and writes the draw commands (vertex count 0 for the culled ones); the draw is one indirect record. The main thread
// only tells the worker what changed (RETAIN_RESIDENT / RELEASE, kinds 14 / 12), the region matrices (15) and which pass to draw (16).
struct ResGpu32 { U32 mPositions[2]; U32 mIndices[2]; U32 mFirst, mCount, mModel, mPad; };
static_assert(sizeof(ResGpu32) == 32, "resident entry");
struct ResFrag;
struct ResPair
{
    ResFrag* mFrag = nullptr; U32 mIndex = 0;                  // its fragment and its place in the fragment's pairs (the events of its buffers find the entries by them)
    U32 mVb = 0, mIb = 0; Buf* mPvb = nullptr; Buf* mPib = nullptr; U64 mGen = ~0ull, mEvb = 0, mMvb = 0, mEib = 0, mMib = 0; bool mOk = false;
    U64 mNote[6] = {};                                          // epoch, moves, version of both buffers at the last full ray note (the cheap note is valid while they are the same)
};
struct ResFrag
{
    std::vector<U32> mSlots;                                    // the table slot of each entry
    std::vector<LLVkBridge::ShadowPacketEntry> mEntries;
    std::vector<U32> mPairOf;                                   // per entry: index into mPairs
    std::vector<ResPair> mPairs;
    F32 mBounds[4] = {};                                        // the sphere (the table copy is write-combined memory: never read back by the CPU)
    std::vector<LLVk::Rt::CasterRef> mRefs;                     // ray scene: the group each entry was filed under (LL_VK_SCENE_RAYFAST)
    std::vector<U32> mRefMat;                                   // the matrix version of the entry's region slot at its last full note
    U64 mNoteFrame = ~0ull;                                     // the frame a pass of which noted this fragment (the other passes of the frame need not)
    size_t mCullIndex = 0;                                      // its row in sRes.mCull
    bool mInCull = false;
    // ray scene alpha casters (kind 18): mMatrix = region slot, mTextures = GL names as last told (kind 19 changes them), ALPHA_NOT_READY keeps an entry out of the scene
    std::vector<LLVkBridge::ShadowAlphaEntry> mAlpha;
    std::vector<LLVk::Rt::CasterRef> mAlphaRefs;
    std::vector<U32> mAlphaPairOf;
    std::vector<U8> mAlphaHeld;
};
// one row per fragment, contiguous, for the sphere test of the ray notes (a sweep over 5,000 fragments must not touch their scattered nodes): centre in region space, radius, the region matrix slot
struct ResCullItem { F32 mC[3]; F32 mR; U32 mSlot; U32 mId; ResFrag* mFrag; bool mHeld; bool mHasAlpha; };
static std::unordered_multimap<U32, struct ResPair*> sResPairByName;     // the fragments' buffer pairs by the names of their buffers (to drop their cached pointers when a buffer goes)
static const U32 kResCapacity = 131072;
static const U32 kResMatrixSlots = 256;
static struct ResState
{
    Buffer mTable, mBounds, mModels;
    ResGpu32* mT = nullptr;
    F32* mB = nullptr;
    F32* mM = nullptr;
    bool mReady = false, mFailed = false;
    U32 mHigh = 0;                                              // slots used so far (the cull covers 0..mHigh)
    std::vector<U32> mFree;
    std::vector<std::pair<U32, U64>> mRetired;                  // slot, frame it was given up (reusable once the frames in flight are done)
    std::unordered_map<U32, ResFrag> mFrags;
    std::vector<ResCullItem> mCull;
    F32 mPlanes[4][24] = {};                                    // ray range mode: the frustum planes each sun cascade pass last named (6 planes of 4 floats)
    U32 mPlaneMask = 0;                                         // which of them are set
    std::vector<bool> mMatrixDirty = std::vector<bool>(256, false);     // slots whose region matrix changed value since the membership was last looked at
    U64 mEntered = 0, mLeft = 0, mEventNotes = 0, mMembershipFrames = 0, mMembershipNs = 0, mHeldNow = 0;
    U64 mEvBufNames = 0, mEvBufNotes = 0, mEvMatNotes = 0, mEvMatFrames = 0;
    U64 mHandleFirst[2] = {}, mHandleReplaced[2] = {}, mHandleSame[2] = {};       // full notes by kind: the first of an entry, one that found its group gone (a new one made), and the rest
    U64 mRefreshFrame = ~0ull;
    U64 mRefreshNs = 0;
    std::vector<F32> mMatrixCpu = std::vector<F32>(16 * 256, 0.f);     // the region matrices as sent (the ray notes read them: the table copy is write-combined)
    std::vector<U32> mMatrixVersion = std::vector<U32>(256, 1);         // bumped when a slot's matrix changes value (a region shift)
    U64 mPasses = 0, mDrawn = 0, mReport = 0, mOverflow = 0, mUnresolved = 0, mRecheck = 0;
} sRes;

static bool res_init()
{
    if (sRes.mReady)
    {
        return true;
    }
    if (sRes.mFailed)
    {
        return false;
    }
    Context& ctx = Context::instance();
    const auto& caps = ctx.getCaps();
    sRes.mFailed = true;
    if (!caps.mBufferDeviceAddress || !caps.mMultiDrawIndirect || !caps.mDrawIndirectFirstInstance)
    {
        LL_WARNS("VkScene") << "resident shadow entries: the device lacks buffer addresses or first-instance indirect draws" << LL_ENDL;
        return false;
    }
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (!ctx.createBuffer((VkDeviceSize)sizeof(ResGpu32) * kResCapacity, usage, Context::Memory::UploadDevice, sRes.mTable)
        || !ctx.createBuffer(16ull * kResCapacity, usage, Context::Memory::UploadDevice, sRes.mBounds)
        || !ctx.createBuffer(64ull * kResMatrixSlots, usage, Context::Memory::UploadDevice, sRes.mModels)
        || !sRes.mTable.mMapped || !sRes.mBounds.mMapped || !sRes.mModels.mMapped || !sRes.mTable.mAddress || !sRes.mBounds.mAddress || !sRes.mModels.mAddress)
    {
        LL_WARNS("VkScene") << "resident shadow entries: the tables could not be created" << LL_ENDL;
        return false;
    }
    sRes.mT = (ResGpu32*)sRes.mTable.mMapped;
    sRes.mB = (F32*)sRes.mBounds.mMapped;
    sRes.mM = (F32*)sRes.mModels.mMapped;
    memset(sRes.mT, 0, sizeof(ResGpu32) * kResCapacity);
    memset(sRes.mB, 0, 16ull * kResCapacity);
    memset(sRes.mM, 0, 64ull * kResMatrixSlots);
    for (U32 i = 0; i < kResMatrixSlots; ++i)
    {
        sRes.mM[16 * i] = sRes.mM[16 * i + 5] = sRes.mM[16 * i + 10] = sRes.mM[16 * i + 15] = 1.f;
        sRes.mMatrixCpu[16 * i] = sRes.mMatrixCpu[16 * i + 5] = sRes.mMatrixCpu[16 * i + 10] = sRes.mMatrixCpu[16 * i + 15] = 1.f;
    }
    sRes.mFailed = false;
    sRes.mReady = true;
    LL_INFOS("VkScene") << "resident shadow entries: tables ready (" << kResCapacity << " entries)" << LL_ENDL;
    return true;
}

static void res_reclaim(U64 frame)
{
    const U64 lag = (U64)Context::frameCount() + 1;
    size_t keep = 0;
    for (size_t i = 0; i < sRes.mRetired.size(); ++i)
    {
        if (frame >= sRes.mRetired[i].second + lag)
        {
            sRes.mFree.push_back(sRes.mRetired[i].first);
        }
        else
        {
            sRes.mRetired[keep++] = sRes.mRetired[i];
        }
    }
    sRes.mRetired.resize(keep);
}

// 0: nothing changed, 1: the pair has other addresses now (or lost them), 2: not there (yet)
static int res_pair_check(ResPair& p)
{
    if (p.mGen != sPacketBufferGeneration)
    {
        auto v = sGL.mBuffers.find(p.mVb);
        auto i = sGL.mBuffers.find(p.mIb);
        p.mPvb = v == sGL.mBuffers.end() ? nullptr : &v->second;
        p.mPib = i == sGL.mBuffers.end() ? nullptr : &i->second;
        p.mGen = sPacketBufferGeneration;
        p.mOk = false;
        if (!p.mPvb || !p.mPib)
        {
            return 2;
        }
        // looked up again: the recorded versions decide below
        p.mEvb = ~0ull;
    }
    if (!p.mPvb || !p.mPib || !p.mPvb->mVk.valid() || !p.mPib->mVk.valid() || !p.mPvb->mVk.mAddress || !p.mPib->mVk.mAddress)
    {
        const bool was = p.mOk;
        p.mOk = false;
        return was ? 1 : 2;
    }
    if (p.mOk && p.mEvb == p.mPvb->mEpoch && p.mMvb == p.mPvb->mMoves && p.mEib == p.mPib->mEpoch && p.mMib == p.mPib->mMoves)
    {
        return 0;
    }
    p.mEvb = p.mPvb->mEpoch; p.mMvb = p.mPvb->mMoves; p.mEib = p.mPib->mEpoch; p.mMib = p.mPib->mMoves;
    p.mOk = true;
    return 1;
}

static void res_write_entries(ResFrag& f, size_t pair_index)
{
    const ResPair& p = f.mPairs[pair_index];
    for (size_t i = 0; i < f.mEntries.size(); ++i)
    {
        if (f.mPairOf[i] != pair_index)
        {
            continue;
        }
        const LLVkBridge::ShadowPacketEntry& e = f.mEntries[i];
        ResGpu32& g = sRes.mT[f.mSlots[i]];
        if (!p.mOk)
        {
            g.mCount = 0;
            continue;
        }
        const U64 pa = (U64)p.mPvb->mVk.mAddress, ia = (U64)p.mPib->mVk.mAddress;
        g.mPositions[0] = (U32)pa; g.mPositions[1] = (U32)(pa >> 32);
        g.mIndices[0] = (U32)ia; g.mIndices[1] = (U32)(ia >> 32);
        g.mFirst = e.mFirstIndex;
        g.mModel = e.mMatrix;
        g.mPad = 0;
        g.mCount = e.mIndexCount;           // last: a count of 0 is a culled entry
    }
}

static void res_register_pairs(ResFrag& f)
{
    for (size_t k = 0; k < f.mPairs.size(); ++k)
    {
        ResPair& p = f.mPairs[k];
        p.mFrag = &f;
        p.mIndex = (U32)k;
        sResPairByName.emplace(p.mVb, &p);
        sResPairByName.emplace(p.mIb, &p);
    }
}

static void res_unregister_pairs(ResFrag& f)
{
    for (ResPair& p : f.mPairs)
    {
        for (U32 name : { p.mVb, p.mIb })
        {
            auto range = sResPairByName.equal_range(name);
            for (auto m = range.first; m != range.second; ++m)
            {
                if (m->second == &p)
                {
                    sResPairByName.erase(m);
                    break;
                }
            }
        }
    }
}

static void res_buffer_gone(U32 name)
{
    auto range = sResPairByName.equal_range(name);
    for (auto it = range.first; it != range.second; ++it)
    {
        ResPair* p = it->second;
        if (p->mVb == name) { p->mPvb = nullptr; }
        if (p->mIb == name) { p->mPib = nullptr; }
        p->mOk = false;
        p->mGen = ~0ull;
        p->mNote[0] = ~0ull;                // the cheap note is not valid until a full one
    }
}

// the pair's buffers, found (again) if the cached pointer was dropped, and usable by the ray scene
static bool res_pair_ready(ResPair& p)
{
    if (!p.mPvb)
    {
        auto v = sGL.mBuffers.find(p.mVb);
        if (v == sGL.mBuffers.end())
        {
            return false;
        }
        p.mPvb = &v->second;
    }
    if (!p.mPib)
    {
        auto i = sGL.mBuffers.find(p.mIb);
        if (i == sGL.mBuffers.end())
        {
            return false;
        }
        p.mPib = &i->second;
    }
    p.mPvb->mResWatched = true;
    p.mPvb->mResName = p.mVb;
    p.mPib->mResWatched = true;
    p.mPib->mResName = p.mIb;
    return p.mPvb->mVk.valid() && p.mPib->mVk.valid() && p.mPvb->mVk.mAddress && p.mPib->mVk.mAddress;
}

// the entry named to the ray scene in full (creates or updates its group, ranges and buffer info, and refreshes the handle)
static void res_full_note(ResFrag& f, size_t i)
{
    ResPair& pair = f.mPairs[f.mPairOf[i]];
    const LLVkBridge::ShadowPacketEntry& e = f.mEntries[i];
    void* before = f.mRefs[i].mGroup;
    note_rt_caster_ref(0, e.mVertexBuffer, e.mIndexBuffer, *pair.mPvb, *pair.mPib, e.mFirstIndex, e.mIndexCount, e.mMatrixId, 0, e.mMatrix ? sRes.mMatrixCpu.data() + 16 * (size_t)e.mMatrix : nullptr, &f.mRefs[i]);
    ++(!before ? sRes.mHandleFirst[0] : (before != f.mRefs[i].mGroup ? sRes.mHandleReplaced[0] : sRes.mHandleSame[0]));
    f.mRefMat[i] = sRes.mMatrixVersion[e.mMatrix];
}

// the fragment comes into range: all its pairs have to be there, then every entry is named in full and its group held
// an alpha caster named to the ray scene in full (kind 1: the alpha test data with it)
static void res_full_note_alpha(ResFrag& f, size_t i)
{
    ResPair& pair = f.mPairs[f.mAlphaPairOf[i]];
    const LLVkBridge::ShadowAlphaEntry& e = f.mAlpha[i];
    Rt::AlphaInfo a;
    a.mFlags = e.mFlags & (31u | 64u);
    a.mTexcoordOffset = e.mTexcoordOffset;
    a.mColorOffset = e.mColorOffset;
    memcpy(a.mTextures, e.mTextures, sizeof(a.mTextures));
    a.mCutoff = e.mCutoff;
    memcpy(a.mUv, e.mUv, sizeof(a.mUv));
    a.mNormalOffset = e.mNormalOffset;
    a.mMaterial = e.mMaterial;
    void* before = f.mAlphaRefs[i].mGroup;
    note_rt_caster(0, e.mVertexBuffer, e.mIndexBuffer, *pair.mPvb, *pair.mPib, e.mFirstIndex, e.mIndexCount, e.mMatrixId, 1, e.mMatrix ? sRes.mMatrixCpu.data() + 16 * (size_t)e.mMatrix : nullptr, &a, &f.mAlphaRefs[i]);
    ++(!before ? sRes.mHandleFirst[1] : (before != f.mAlphaRefs[i].mGroup ? sRes.mHandleReplaced[1] : sRes.mHandleSame[1]));
}

static void res_alpha_hold(ResFrag& f, size_t i)
{
    if (!f.mAlphaHeld[i] && !(f.mAlpha[i].mFlags & LLVkBridge::ALPHA_NOT_READY))
    {
        res_full_note_alpha(f, i);
        LLVk::Rt::holdRange(f.mAlphaRefs[i], f.mAlpha[i].mFirstIndex, f.mAlpha[i].mIndexCount, 1);
        f.mAlphaHeld[i] = 1;
    }
}

static void res_alpha_unhold(ResFrag& f, size_t i)
{
    if (f.mAlphaHeld[i])
    {
        LLVk::Rt::holdRange(f.mAlphaRefs[i], f.mAlpha[i].mFirstIndex, f.mAlpha[i].mIndexCount, -1);
        f.mAlphaHeld[i] = 0;
    }
}

static bool res_enter(ResFrag& f)
{
    for (ResPair& p : f.mPairs)
    {
        if (!res_pair_ready(p))
        {
            return false;
        }
    }
    for (size_t i = 0; i < f.mEntries.size(); ++i)
    {
        res_full_note(f, i);
        LLVk::Rt::holdRange(f.mRefs[i], f.mEntries[i].mFirstIndex, f.mEntries[i].mIndexCount, 1);
    }
    for (size_t i = 0; i < f.mAlpha.size(); ++i)
    {
        res_alpha_hold(f, i);
    }
    return true;
}

static void res_leave(ResFrag& f)
{
    for (size_t i = 0; i < f.mEntries.size(); ++i)
    {
        LLVk::Rt::holdRange(f.mRefs[i], f.mEntries[i].mFirstIndex, f.mEntries[i].mIndexCount, -1);
    }
    for (size_t i = 0; i < f.mAlpha.size(); ++i)
    {
        res_alpha_unhold(f, i);
    }
}

static void res_release(U32 id)
{
    auto it = sRes.mFrags.find(id);
    if (it == sRes.mFrags.end())
    {
        return;
    }
    if (it->second.mInCull && sRes.mCull[it->second.mCullIndex].mHeld)
    {
        res_leave(it->second);
        sRes.mCull[it->second.mCullIndex].mHeld = false;
    }
    res_unregister_pairs(it->second);
    const U64 frame = Context::instance().getFrameNumber();
    for (U32 slot : it->second.mSlots)
    {
        sRes.mT[slot].mCount = 0;
        sRes.mRetired.push_back({ slot, frame });
    }
    if (it->second.mInCull)
    {
        const size_t index = it->second.mCullIndex;
        sRes.mCull[index] = sRes.mCull.back();
        sRes.mCull[index].mFrag->mCullIndex = index;
        sRes.mCull.pop_back();
    }
    sRes.mFrags.erase(it);
}

static void res_retain(const LLVkBridge::ShadowPacketsHeader& h, const U8* blob)
{
    if (!res_init())
    {
        return;
    }
    res_release(h.mFragment);
    res_reclaim(Context::instance().getFrameNumber());
    const LLVkBridge::ShadowPacketEntry* entries = (const LLVkBridge::ShadowPacketEntry*)blob;
    ResFrag f;
    memcpy(f.mBounds, h.mViewProj, sizeof(f.mBounds));
    f.mEntries.assign(entries, entries + h.mEntries);
    f.mRefs.assign(h.mEntries, LLVk::Rt::CasterRef());
    f.mRefMat.assign(h.mEntries, 0);
    for (U32 i = 0; i < h.mEntries; ++i)
    {
        U32 slot;
        if (!sRes.mFree.empty())
        {
            slot = sRes.mFree.back();
            sRes.mFree.pop_back();
        }
        else if (sRes.mHigh < kResCapacity)
        {
            slot = sRes.mHigh++;
        }
        else
        {
            if (sRes.mOverflow++ == 0)
            {
                LL_WARNS("VkScene") << "resident shadow entries: the table is full (" << kResCapacity << " entries): the rest are not drawn" << LL_ENDL;
            }
            for (U32 s : f.mSlots) { sRes.mRetired.push_back({ s, Context::instance().getFrameNumber() }); }
            return;
        }
        f.mSlots.push_back(slot);
        const LLVkBridge::ShadowPacketEntry& e = f.mEntries[i];
        size_t k = 0;
        while (k < f.mPairs.size() && !(f.mPairs[k].mVb == e.mVertexBuffer && f.mPairs[k].mIb == e.mIndexBuffer))
        {
            ++k;
        }
        if (k == f.mPairs.size())
        {
            ResPair p;
            p.mVb = e.mVertexBuffer;
            p.mIb = e.mIndexBuffer;
            f.mPairs.push_back(p);
        }
        f.mPairOf.push_back((U32)k);
        sRes.mT[slot].mCount = 0;
        F32* b = sRes.mB + 4 * (size_t)slot;
        memcpy(b, h.mViewProj, 4 * sizeof(F32));
        sRes.mT[slot].mModel = e.mMatrix;
    }
    ResFrag& stored = sRes.mFrags[h.mFragment] = std::move(f);
    res_register_pairs(stored);
    if (!stored.mSlots.empty())
    {
        ResCullItem item = { { stored.mBounds[0], stored.mBounds[1], stored.mBounds[2] }, stored.mBounds[3], stored.mEntries[0].mMatrix, h.mFragment, &stored, false, false };
        stored.mCullIndex = sRes.mCull.size();
        stored.mInCull = true;
        sRes.mCull.push_back(item);
    }
    for (size_t k = 0; k < stored.mPairs.size(); ++k)
    {
        if (res_pair_check(stored.mPairs[k]) == 1)
        {
            res_write_entries(stored, k);
        }
    }
}

// once a frame: the pairs whose buffers moved (or appeared) get their entries written again; every buffer a resident entry reads counts as used this frame (the buffer rename path asks)
static void res_refresh(U64 frame)
{
    if (sRes.mRefreshFrame == frame)
    {
        return;
    }
    sRes.mRefreshFrame = frame;
    const auto t_refresh = std::chrono::steady_clock::now();
    struct RefreshTimer { std::chrono::steady_clock::time_point mT; ~RefreshTimer() { sRes.mRefreshNs += (U64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - mT).count(); } } refresh_timer = { t_refresh };
    res_reclaim(frame);
    U64 unresolved = 0;
    const bool pending = !sPendingBufs.empty();
    bool flush = false;
    for (auto& kv : sRes.mFrags)
    {
        ResFrag& f = kv.second;
        for (size_t k = 0; k < f.mPairs.size(); ++k)
        {
            ResPair& p = f.mPairs[k];
            const int c = res_pair_check(p);
            if (c == 1)
            {
                res_write_entries(f, k);
                ++sRes.mRecheck;
            }
            if (p.mOk)
            {
                p.mPvb->mLastUseFrame = frame;
                p.mPib->mLastUseFrame = frame;
                flush = flush || (pending && (p.mPvb->mPending || p.mPib->mPending));
            }
            else
            {
                ++unresolved;
            }
        }
    }
    sRes.mUnresolved = unresolved;
    if (flush)
    {
        flush_buffer_updates();     // data written by the main thread this frame is recorded before anything reads it
    }
}

// kind 18: the alpha masked static casters of a fragment (merges with its kind 14 record, which comes first)
static void res_retain_alpha(const LLVkBridge::ShadowPacketsHeader& h, const U8* blob)
{
    if (!res_init() || h.mEntries == 0)
    {
        return;
    }
    const LLVkBridge::ShadowAlphaEntry* entries = (const LLVkBridge::ShadowAlphaEntry*)blob;
    auto found = sRes.mFrags.find(h.mFragment);
    ResFrag* f;
    if (found == sRes.mFrags.end())
    {
        f = &sRes.mFrags[h.mFragment];
        memcpy(f->mBounds, h.mViewProj, sizeof(f->mBounds));
    }
    else
    {
        f = &found->second;
    }
    res_unregister_pairs(*f);
    f->mAlpha.assign(entries, entries + h.mEntries);
    f->mAlphaRefs.assign(h.mEntries, LLVk::Rt::CasterRef());
    f->mAlphaHeld.assign(h.mEntries, 0);
    f->mAlphaPairOf.clear();
    for (const LLVkBridge::ShadowAlphaEntry& e : f->mAlpha)
    {
        size_t k = 0;
        while (k < f->mPairs.size() && !(f->mPairs[k].mVb == e.mVertexBuffer && f->mPairs[k].mIb == e.mIndexBuffer))
        {
            ++k;
        }
        if (k == f->mPairs.size())
        {
            ResPair p;
            p.mVb = e.mVertexBuffer;
            p.mIb = e.mIndexBuffer;
            f->mPairs.push_back(p);
        }
        f->mAlphaPairOf.push_back((U32)k);
    }
    res_register_pairs(*f);
    if (!f->mInCull)
    {
        ResCullItem item = { { f->mBounds[0], f->mBounds[1], f->mBounds[2] }, f->mBounds[3], f->mAlpha[0].mMatrix, h.mFragment, f, false, true };
        f->mCullIndex = sRes.mCull.size();
        f->mInCull = true;
        sRes.mCull.push_back(item);
    }
    else
    {
        sRes.mCull[f->mCullIndex].mHasAlpha = true;
    }
}

// kind 19: entries whose textures changed name or readiness: { entry index, 4 names, ready }
static void res_tex_update(const LLVkBridge::ShadowPacketsHeader& h, const U8* blob)
{
    auto it = sRes.mFrags.find(h.mFragment);
    if (it == sRes.mFrags.end())
    {
        return;
    }
    ResFrag& f = it->second;
    const U32* u = (const U32*)blob;
    const bool held = f.mInCull && sRes.mCull[f.mCullIndex].mHeld;
    for (U32 j = 0; j < h.mEntries; ++j)
    {
        const U32 index = u[6 * j];
        if (index >= f.mAlpha.size())
        {
            continue;
        }
        LLVkBridge::ShadowAlphaEntry& e = f.mAlpha[index];
        memcpy(e.mTextures, u + 6 * j + 1, sizeof(e.mTextures));
        if (u[6 * j + 5])
        {
            e.mFlags &= ~(U32)LLVkBridge::ALPHA_NOT_READY;
        }
        else
        {
            e.mFlags |= LLVkBridge::ALPHA_NOT_READY;
        }
        if (!held)
        {
            continue;
        }
        if (e.mFlags & LLVkBridge::ALPHA_NOT_READY)
        {
            res_alpha_unhold(f, index);
        }
        else if (res_pair_ready(f.mPairs[f.mAlphaPairOf[index]]))
        {
            if (f.mAlphaHeld[index])
            {
                res_full_note_alpha(f, index);          // the group's alpha test data changed
            }
            else
            {
                res_alpha_hold(f, index);
            }
        }
    }
}

static void res_matrices(const LLVkBridge::ShadowPacketsHeader& h, const U8* blob)
{
    if (!res_init())
    {
        return;
    }
    const U32 n = std::min<U32>(h.mMatrices, kResMatrixSlots);
    memcpy(sRes.mM, blob, 64 * (size_t)n);
    for (U32 s = 0; s < n; ++s)
    {
        if (memcmp(sRes.mMatrixCpu.data() + 16 * (size_t)s, blob + 64 * (size_t)s, 64) != 0)
        {
            ++sRes.mMatrixVersion[s];
            sRes.mMatrixDirty[s] = true;
        }
    }
    memcpy(sRes.mMatrixCpu.data(), blob, 64 * (size_t)n);
}

namespace
{
const char* kCullShader = R"GLSL(#version 450
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
layout(local_size_x = 64) in;
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer Table { uint t[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Bounds { vec4 b[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Models { mat4 m[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Params { vec4 planes[6]; uint count; uint planeCount; };
layout(buffer_reference, std430, buffer_reference_align = 4) writeonly buffer Cmds { uint c[]; };
layout(push_constant) uniform PC { uvec2 table; uvec2 bounds; uvec2 models; uvec2 params; uvec2 cmds; } pc;
void main()
{
    Params p = Params(pc.params);
    uint i = gl_GlobalInvocationID.x;
    if (i >= p.count) return;
    Table tb = Table(pc.table);
    Bounds bd = Bounds(pc.bounds);
    Models md = Models(pc.models);
    Cmds cm = Cmds(pc.cmds);
    uint count = tb.t[i * 8u + 5u];
    uint model = tb.t[i * 8u + 6u];
    bool visible = count != 0u;
    if (visible)
    {
        vec4 b = bd.b[i];
        vec3 c = (md.m[model] * vec4(b.xyz, 1.0)).xyz;
        for (uint k = 0u; k < p.planeCount; ++k)
        {
            if (dot(p.planes[k].xyz, c) + p.planes[k].w < -b.w) { visible = false; break; }
        }
    }
    cm.c[i * 4u + 0u] = visible ? count : 0u;
    cm.c[i * 4u + 1u] = visible ? 1u : 0u;
    cm.c[i * 4u + 2u] = 0u;
    cm.c[i * 4u + 3u] = i;
}
)GLSL";
struct CullPipe { VkShaderModule mModule = VK_NULL_HANDLE; VkPipelineLayout mLayout = VK_NULL_HANDLE; VkPipeline mPipe = VK_NULL_HANDLE; bool mTried = false; } sCullPipe;
}

static bool res_cull_pipeline()
{
    if (sCullPipe.mPipe != VK_NULL_HANDLE)
    {
        return true;
    }
    if (sCullPipe.mTried)
    {
        return false;
    }
    sCullPipe.mTried = true;
    static const bool glslang_ready = glslang::InitializeProcess();
    if (!glslang_ready)
    {
        return false;
    }
    glslang::TShader shader(EShLangCompute);
    const char* strings[1] = { kCullShader };
    shader.setStrings(strings, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, EShLangCompute, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);
    const EShMessages msgs = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader.parse(GetDefaultResources(), 450, ECoreProfile, false, false, msgs))
    {
        LL_WARNS("VkScene") << "resident cull: compile failed: " << shader.getInfoLog() << LL_ENDL;
        return false;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(msgs))
    {
        LL_WARNS("VkScene") << "resident cull: link failed: " << program.getInfoLog() << LL_ENDL;
        return false;
    }
    std::vector<unsigned int> spirv;
    spv::SpvBuildLogger logger;
    glslang::SpvOptions options;
    options.validate = false;
    glslang::GlslangToSpv(*program.getIntermediate(EShLangCompute), spirv, &logger, &options);
    if (spirv.empty())
    {
        LL_WARNS("VkScene") << "resident cull: no SPIR-V: " << logger.getAllMessages() << LL_ENDL;
        return false;
    }
    VkDevice dev = Context::instance().getDevice();
    VkShaderModuleCreateInfo mi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    mi.codeSize = spirv.size() * sizeof(unsigned int);
    mi.pCode = spirv.data();
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 40 };
    VkPipelineLayoutCreateInfo li = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &pcr;
    if (vkCreateShaderModule(dev, &mi, nullptr, &sCullPipe.mModule) != VK_SUCCESS || vkCreatePipelineLayout(dev, &li, nullptr, &sCullPipe.mLayout) != VK_SUCCESS)
    {
        return false;
    }
    VkComputePipelineCreateInfo pi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    pi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pi.stage.module = sCullPipe.mModule;
    pi.stage.pName = "main";
    pi.layout = sCullPipe.mLayout;
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pi, nullptr, &sCullPipe.mPipe) != VK_SUCCESS)
    {
        sCullPipe.mPipe = VK_NULL_HANDLE;
        return false;
    }
    LL_INFOS("VkScene") << "resident cull pipeline ready" << LL_ENDL;
    return true;
}

// kind 17 (ray shadows): the casters of a cascade pass for the ray scene, from the resident fragments: the same frustum test as the GPU cull, on the CPU, per fragment (its entries share its bounds)
static void res_note(const LLVkBridge::ShadowPacketsHeader& h, const U8* blob)
{
    if (!Rt::active() || !sRes.mReady || sRes.mFrags.empty())
    {
        return;
    }
    static const bool range = getenv("LL_VK_SCENE_RAYRANGE") && getenv("LL_VK_SCENE_RAYRANGE")[0] == '1';
    if (range)
    {
        // membership by residency: the pass only tells where its frustum is; which fragments are in the scene is decided once a frame, before the scene call (res_ray_membership)
        if (h.mPass < 4)
        {
            memcpy(sRes.mPlanes[h.mPass], blob, 24 * sizeof(F32));
            sRes.mPlaneMask |= 1u << h.mPass;
        }
        return;
    }
    static const bool fast = getenv("LL_VK_SCENE_RAYFAST") && getenv("LL_VK_SCENE_RAYFAST")[0] == '1';
    const U64 frame_now = Context::instance().getFrameNumber();
    if (fast)
    {
        // nothing reads the table's addresses in ray shadow mode (nothing is drawn): the pairs are checked when they are named, and dropped by res_buffer_gone; only the slots are given back
        res_reclaim(frame_now);
    }
    else
    {
        res_refresh(frame_now);
    }
    if (fast)
    {
        Rt::passRendered(h.mPass);      // the groups' stamps for this pass are read against it, whichever fragments it ends up naming
    }
    static const U32 plane_count = []() { const char* e = getenv("LL_VK_SCENE_NEARFAR"); return e && e[0] == '1' ? 6u : 4u; }();
    const F32* planes = (const F32*)blob;
    static U64 passes = 0, noted = 0, fragments_in = 0, again = 0, full = 0;
    static U64 ns_refresh = 0, ns_sweep = 0, ns_notes = 0;
    static const bool timed = getenv("LL_VK_SCENE_TIMES") != nullptr;
    const auto t_start = std::chrono::steady_clock::now();
    for (const ResCullItem& item : sRes.mCull)
    {
        // the sphere: the centre is in region space, add the region matrix translation
        const F32* m0 = item.mSlot ? sRes.mMatrixCpu.data() + 16 * (size_t)item.mSlot : nullptr;
        const F32 c[3] = { item.mC[0] + (m0 ? m0[12] : 0.f), item.mC[1] + (m0 ? m0[13] : 0.f), item.mC[2] + (m0 ? m0[14] : 0.f) };
        bool inside = true;
        for (U32 p = 0; p < plane_count && inside; ++p)
        {
            inside = planes[4 * p] * c[0] + planes[4 * p + 1] * c[1] + planes[4 * p + 2] * c[2] + planes[4 * p + 3] >= -item.mR;
        }
        if (!inside)
        {
            continue;
        }
        ResFrag& f = *item.mFrag;
        if (fast && f.mNoteFrame == frame_now)
        {
            continue;                   // an earlier pass of this frame named it (the pass stamp of that pass keeps its groups alive)
        }
        ++fragments_in;
        f.mNoteFrame = frame_now;
        for (size_t i = 0; i < f.mEntries.size(); ++i)
        {
            ResPair& pair = f.mPairs[f.mPairOf[i]];
            if (fast ? !res_pair_ready(pair) : !pair.mOk)
            {
                continue;
            }
            const LLVkBridge::ShadowPacketEntry& e = f.mEntries[i];
            if (fast)
            {
                // the cheap note: the group was filed at a full note, and nothing it was made from changed since (buffers: epoch, moves, version; the region matrix: its version)
                const U64 stamp[6] = { pair.mPvb->mEpoch, pair.mPvb->mMoves, pair.mPvb->mVersion, pair.mPib->mEpoch, pair.mPib->mMoves, pair.mPib->mVersion };
                if (f.mRefs[i].mGroup && f.mRefMat[i] == sRes.mMatrixVersion[e.mMatrix] && memcmp(stamp, pair.mNote, sizeof(stamp)) == 0 && Rt::noteAgain(f.mRefs[i], h.mPass))
                {
                    ++again;
                    ++noted;
                    continue;
                }
                ++full;
                note_rt_caster_ref(h.mPass, e.mVertexBuffer, e.mIndexBuffer, *pair.mPvb, *pair.mPib, e.mFirstIndex, e.mIndexCount, e.mMatrixId, 0, e.mMatrix ? sRes.mMatrixCpu.data() + 16 * (size_t)e.mMatrix : nullptr, &f.mRefs[i]);
                f.mRefMat[i] = sRes.mMatrixVersion[e.mMatrix];
                memcpy(pair.mNote, stamp, sizeof(stamp));
                ++noted;
                continue;
            }
            note_rt_caster(h.mPass, e.mVertexBuffer, e.mIndexBuffer, *pair.mPvb, *pair.mPib, e.mFirstIndex, e.mIndexCount, e.mMatrixId, 0, e.mMatrix ? sRes.mMatrixCpu.data() + 16 * (size_t)e.mMatrix : nullptr, nullptr);
            ++noted;
        }
    }
    if (timed)
    {
        ns_notes += (U64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t_start).count();
    }
    if (++passes % 600 == 0 && timed)
    {
        LL_INFOS("VkScene") << "resident ray notes, worker time per pass: sweep + notes " << (double)ns_notes / 1000.0 / passes << " us, refresh " << (double)sRes.mRefreshNs / 1000.0 / passes << " us (once a frame)" << LL_ENDL;
    }
    if (passes % 2400 == 0)
    {
        LL_INFOS("VkScene") << "resident ray notes: " << passes << " passes, " << noted << " casters named (" << again << " cheap, " << full << " full), " << fragments_in << " fragments inside the frustum" << LL_ENDL;
    }
}

// S2 stage 2 (LL_VK_SCENE_RAYRANGE=1): once a frame, before the scene call: a resident fragment is in the ray scene while its sphere is inside the frustum of any sun cascade pass (the planes
// each pass last named: a cascade that does not render this frame keeps its last frustum, as its cached shadow map kept its casters). Only the fragments that change sides are touched
// (their groups are held or released); the groups of unchanged fragments are not named at all. Changes of what a held fragment was made from arrive as events: a buffer it reads changed (the
// worker's write sites log its name), or a region matrix changed value (a region shift).
static void res_ray_membership(U64 frame)
{
    static const bool range = getenv("LL_VK_SCENE_RAYRANGE") && getenv("LL_VK_SCENE_RAYRANGE")[0] == '1';
    static U64 last = ~0ull;
    if (!range || !Rt::active() || !sRes.mReady || last == frame)
    {
        return;
    }
    last = frame;
    const auto t0 = std::chrono::steady_clock::now();
    res_reclaim(frame);
    static const U32 plane_count = []() { const char* e = getenv("LL_VK_SCENE_NEARFAR"); return e && e[0] == '1' ? 6u : 4u; }();
    const U32 mask = sRes.mPlaneMask;
    U64 held = 0;
    for (ResCullItem& item : sRes.mCull)
    {
        bool inside = false;
        if (mask)
        {
            const F32* m0 = item.mSlot ? sRes.mMatrixCpu.data() + 16 * (size_t)item.mSlot : nullptr;
            const F32 c[3] = { item.mC[0] + (m0 ? m0[12] : 0.f), item.mC[1] + (m0 ? m0[13] : 0.f), item.mC[2] + (m0 ? m0[14] : 0.f) };
            for (U32 pass = 0; pass < 4 && !inside; ++pass)
            {
                if (!(mask & (1u << pass)))
                {
                    continue;
                }
                const F32* planes = sRes.mPlanes[pass];
                bool in = true;
                for (U32 p = 0; p < plane_count && in; ++p)
                {
                    in = planes[4 * p] * c[0] + planes[4 * p + 1] * c[1] + planes[4 * p + 2] * c[2] + planes[4 * p + 3] >= -item.mR;
                }
                inside = in;
            }
        }
        if (inside && !item.mHeld)
        {
            if (res_enter(*item.mFrag))
            {
                item.mHeld = true;
                ++sRes.mEntered;
            }
        }
        else if (!inside && item.mHeld)
        {
            res_leave(*item.mFrag);
            item.mHeld = false;
            ++sRes.mLeft;
        }
        held += item.mHeld ? 1 : 0;
    }
    sRes.mHeldNow = held;
    // the alpha test reads the vertex data of a held alpha caster while the lighting traces: its buffers count as in use this frame (a CPU rewrite of a host-written range renames it first)
    for (const ResCullItem& item : sRes.mCull)
    {
        if (item.mHeld && item.mHasAlpha)
        {
            for (ResPair& p : item.mFrag->mPairs)
            {
                if (p.mPvb && p.mPib)
                {
                    p.mPvb->mLastUseFrame = frame;
                    p.mPib->mLastUseFrame = frame;
                }
            }
        }
    }
    // events: a buffer a held fragment reads changed (its group's buffer info and ranges are made again by a full note)
    if (!sResChanged.empty())
    {
        std::sort(sResChanged.begin(), sResChanged.end());
        sResChanged.erase(std::unique(sResChanged.begin(), sResChanged.end()), sResChanged.end());
        sRes.mEvBufNames += sResChanged.size();
        for (U32 name : sResChanged)
        {
            auto range_it = sResPairByName.equal_range(name);
            for (auto it = range_it.first; it != range_it.second; ++it)
            {
                ResPair* p = it->second;
                ResFrag* f = p->mFrag;
                if (!f || !f->mInCull || !sRes.mCull[f->mCullIndex].mHeld || !res_pair_ready(*p))
                {
                    continue;
                }
                // a changed buffer refreshes the buffer info of the groups its entries belong to: the entries that share a group need one note, not one each
                static const bool dedupe = !(getenv("LL_VK_SCENE_EVDEDUPE") && getenv("LL_VK_SCENE_EVDEDUPE")[0] == '0');
                void* noted[8];
                size_t noted_count = 0;
                auto already = [&](void* group) -> bool
                {
                    for (size_t k = 0; dedupe && k < noted_count; ++k)
                    {
                        if (noted[k] == group)
                        {
                            return true;
                        }
                    }
                    return false;
                };
                for (size_t i = 0; i < f->mEntries.size(); ++i)
                {
                    if (f->mPairOf[i] == p->mIndex)
                    {
                        if (f->mRefs[i].mGroup && already(f->mRefs[i].mGroup))
                        {
                            continue;
                        }
                        res_full_note(*f, i);
                        ++sRes.mEventNotes;
                        ++sRes.mEvBufNotes;
                        if (noted_count < 8) { noted[noted_count++] = f->mRefs[i].mGroup; }
                    }
                }
                for (size_t i = 0; i < f->mAlpha.size(); ++i)
                {
                    if (f->mAlphaPairOf[i] == p->mIndex && f->mAlphaHeld[i])
                    {
                        if (f->mAlphaRefs[i].mGroup && already(f->mAlphaRefs[i].mGroup))
                        {
                            continue;
                        }
                        res_full_note_alpha(*f, i);
                        ++sRes.mEventNotes;
                        ++sRes.mEvBufNotes;
                        if (noted_count < 8) { noted[noted_count++] = f->mAlphaRefs[i].mGroup; }
                    }
                }
            }
        }
        sResChanged.clear();
    }
    // events: a region matrix changed value (the scene moved with a region shift): the entries that use it are named again
    bool any_matrix = false;
    for (U32 s = 1; s < 256; ++s)
    {
        any_matrix = any_matrix || sRes.mMatrixDirty[s];
    }
    if (any_matrix)
    {
        ++sRes.mEvMatFrames;
        for (ResCullItem& item : sRes.mCull)
        {
            if (!item.mHeld)
            {
                continue;
            }
            ResFrag& f = *item.mFrag;
            for (size_t i = 0; i < f.mEntries.size(); ++i)
            {
                if (f.mEntries[i].mMatrix && sRes.mMatrixDirty[f.mEntries[i].mMatrix] && res_pair_ready(f.mPairs[f.mPairOf[i]]))
                {
                    res_full_note(f, i);
                    ++sRes.mEventNotes;
                }
            }
            for (size_t i = 0; i < f.mAlpha.size(); ++i)
            {
                if (f.mAlphaHeld[i] && f.mAlpha[i].mMatrix && sRes.mMatrixDirty[f.mAlpha[i].mMatrix] && res_pair_ready(f.mPairs[f.mAlphaPairOf[i]]))
                {
                    res_full_note_alpha(f, i);
                    ++sRes.mEventNotes;
                    ++sRes.mEvMatNotes;
                }
            }
        }
        std::fill(sRes.mMatrixDirty.begin(), sRes.mMatrixDirty.end(), false);
    }
    sRes.mMembershipNs += (U64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    if (++sRes.mMembershipFrames % 600 == 0)
    {
        LL_INFOS("VkScene") << "resident ray membership: " << sRes.mHeldNow << " of " << sRes.mCull.size() << " fragments in the scene, " << sRes.mEntered << " entered and " << sRes.mLeft << " left in total, "
            << "full notes (kind 0 / alpha): first " << sRes.mHandleFirst[0] << " / " << sRes.mHandleFirst[1] << ", group gone and made again " << sRes.mHandleReplaced[0] << " / " << sRes.mHandleReplaced[1] << ", same group " << sRes.mHandleSame[0] << " / " << sRes.mHandleSame[1] << "; " << sRes.mEventNotes << " event notes (" << sRes.mEvBufNotes << " from " << sRes.mEvBufNames << " changed buffer names; writes of watched buffers: " << sResCause[1] << " data, " << sResCause[0] << " storage made again, " << sResCause[2] << " moved, " << sRes.mEvMatNotes << " from matrix changes in " << sRes.mEvMatFrames << " frames), " << (double)sRes.mMembershipNs / 1000.0 / sRes.mMembershipFrames << " us a frame" << LL_ENDL;
    }
}

void on_shadow_packets(const LLVkBridge::ShadowPacketsHeader& h, const U8* blob);
// kind 16: cull the resident entries against the pass's planes on the GPU, then draw the commands it wrote with the packet program (already bound by the viewer: the same one as kind 0)
static void res_draw(const LLVkBridge::ShadowPacketsHeader& h, const U8* blob)
{
    if (!res_init() || !res_cull_pipeline())
    {
        return;
    }
    Renderer& r = Renderer::instance();
    Program* program = r.getProgram();
    if (!program || !program->isValid() || !program->hasUniform("ll_packet_table"))
    {
        ++sStats.mNoProgram;
        return;
    }
    const U64 frame_now = Context::instance().getFrameNumber();
    res_refresh(frame_now);
    const U32 n = sRes.mHigh;
    if (n == 0 || sRes.mFrags.empty())
    {
        return;
    }
    Renderer::ScratchAlloc params = r.allocVertexScratch(112);
    Renderer::ScratchAlloc commands = r.allocVertexScratch(16ull * n);
    if (!params.mPtr || !commands.mPtr || !params.mAddress || !commands.mAddress)
    {
        return;
    }
    static const U32 plane_count = []() { const char* e = getenv("LL_VK_SCENE_NEARFAR"); return e && e[0] == '1' ? 6u : 4u; }();
    memcpy(params.mPtr, blob, 96);
    ((U32*)params.mPtr)[24] = n;
    ((U32*)params.mPtr)[25] = plane_count;
    const U64 ta = (U64)sRes.mTable.mAddress, ba = (U64)sRes.mBounds.mAddress, ma = (U64)sRes.mModels.mAddress, pa = (U64)params.mAddress, ca = (U64)commands.mAddress;
    const U32 push[10] = { (U32)ta, (U32)(ta >> 32), (U32)ba, (U32)(ba >> 32), (U32)ma, (U32)(ma >> 32), (U32)pa, (U32)(pa >> 32), (U32)ca, (U32)(ca >> 32) };
    r.recordExternal({}, [&](VkCommandBuffer cmd) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sCullPipe.mPipe);
        vkCmdPushConstants(cmd, sCullPipe.mLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
        vkCmdDispatch(cmd, (n + 63) / 64, 1, 1);
        VkMemoryBarrier2 mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
        mb.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
        VkDependencyInfo di = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        di.memoryBarrierCount = 1;
        di.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &di);
    });

    static thread_local VertexInput vi_cache;
    static thread_local std::vector<U32> used_cache;
    VertexInput& vi = vi_cache;
    vi.mBindings.clear();
    vi.mAttributes.clear();
    used_cache.clear();
    const S32 table_words[2] = { (S32)(U32)ta, (S32)(U32)(ta >> 32) };
    const S32 models_words[2] = { (S32)(U32)ma, (S32)(U32)(ma >> 32) };
    program->setInts("ll_packet_table", table_words, 2);
    program->setInts("ll_packet_models", models_words, 2);
    program->setMatrices("ll_view_proj", h.mViewProj, 4, 4, 1);
    if (!prepare_draw(vi, used_cache))
    {
        return;
    }
    r.setVertexInput(vi);
    r.setTopology(topology(LLVkFormat::TOP_TRIANGLES));
    if (r.drawPackets(nullptr, n, commands.mBuffer, commands.mOffset))
    {
        sDraws += n;
        sDrawsTotal += n;
    }
    ++sRes.mPasses;
    sRes.mDrawn += n;
    if (sRes.mPasses - sRes.mReport >= 2400)
    {
        sRes.mReport = sRes.mPasses;
        LL_INFOS("VkScene") << "resident shadow entries: " << sRes.mFrags.size() << " fragments, " << n << " table slots (" << sRes.mFree.size() << " free, " << sRes.mRetired.size() << " retired), "
            << sRes.mUnresolved << " buffer pairs unresolved, " << sRes.mRecheck << " pair re-resolves, " << sRes.mPasses << " passes drawn, " << sRes.mOverflow << " overflows" << LL_ENDL;
    }
}

// <FS> S3 G2 / G3 (doc/vulkan/S3-design.md): the deferred geometry entries of the scene fragments, kept in a persistent GPU table (the 128 byte entry diffusePullV.glsl / materialPullV.glsl read), drawn
// from the list of fragments the viewer names once a frame (kind 21) and the pass it draws (kind 22). Nothing is looked at per entry per frame: a fragment is checked only when an EVENT says
// so (a buffer it reads got other storage or went, one of its textures was named otherwise by the viewer (kind 24), a pin of its textures got another bindless slot) and its entries are then
// written again, with its draw commands. The per-frame work is per listed fragment: one dirty test and a copy of its commands. The textures of the entries are PINNED slots (permanent in every
// frame's descriptor set, rtPinAcquire), so the single buffered, write-combined table never names a slot an earlier frame in flight cannot see. A buffer a fragment reads is marked used by the
// fragment's listed frame (geo_buffer_in_flight, asked by the write path), not by touching it every frame. LL_VK_SCENE_GEO_STALECHECK (default on, every 16th prepare) compares what the table
// holds against a fresh lookup and heals it, counting what the events missed.
namespace
{
const U32 kGeoCapacity = 131072;
const U32 kGeoTypes = LLVkBridge::kGeoPassTypes;
const U32 kGeoAlphaType = 2 * kGeoTypes;       // S5a: the item type of the alpha items
const U32 kGeoAlphaRiggedType = 2 * kGeoTypes + 1;       // S5d: ... of the rigged alpha items (weights and palette, indexed draws)
const U32 kPbrCapacity = 16384;                // S5c: rows of PBR values (160 bytes each) for the alpha items with a glTF material
struct PbrWire { U32 mItem; U32 mPad[3]; F32 mV[40]; };        // kind 20, after the texture names: the PBR values of the wire item mItem
struct GeoGpuG
{
    U32 mPositions[2]; U32 mNormals[2]; U32 mTexcoords[2]; U32 mColors[2]; U32 mIndices[2]; U32 mTangents[2]; U32 mTexcoords1[2]; U32 mTexcoords2[2];
    U32 mFirst, mModel, mFlags, mMapSlots;
    U32 mSlots[2];
    U32 mPad[2];
    F32 mSpecular[4];
    F32 mEnvIntensity, mCutoff, mBrightness;
    U32 mEmissiveOffset;        // S5e: the byte offset of the emissive array (alpha items; the glow draw)
    U32 mWeights[2]; U32 mPalette[2]; U32 mJoints; U32 mSkinPad[3];     // S4 B: rigged items (the viewer's per-frame binding writes the palette and the joints)
};
static_assert(sizeof(GeoGpuG) == 160, "geometry table entry");
struct GeoFrag
{
    std::vector<LLVkBridge::GeoWire> mItems;
    std::vector<U32> mSlots;            // the table slot of each item
    std::vector<U32> mPairOf;           // each item's index into mPairs
    std::vector<ResPair> mPairs;        // the distinct (vertex, index) buffers of its items
    std::vector<U64> mAddr;             // per pair: vertex and index buffer address as last written into the entries (the stale check reads these, never the write-combined table)
    std::vector<U32> mTexName;          // the GL name the viewer last told for each local texture
    std::vector<U32> mPin;              // its pin (0 = none)
    std::vector<U32> mTexSlot;          // the slot the entries were last written with
    std::vector<VkDrawIndirectCommand> mCmds;   // the draws of its usable items, sorted by pass type
    std::vector<U32> mPbrOfItem;                // S5c: per item, its PBR row + 1 (0 = none)
    std::vector<VkDrawIndexedIndirectCommand> mARCmds;      // S5d: the draws of its rigged alpha items (type 27), in item order: indexed, the count 0 until the viewer bound a palette this frame
    std::vector<U32> mARCount0, mARItem, mARItemCmd;        // ... their index counts, the item of each command, the command of each item (~0u: none)
    std::vector<VkDrawIndirectCommand> mACmds;  // S5a: the draws of its alpha items (type 26), in item order, one per item (count 0 when its buffers are not usable): a run names a range of them
    U32 mTypeFirst[kGeoTypes + 1] = {};
    // S4 B: the rigged items (types 13-25, drawn indexed: the hardware reads the indices): commands sorted by rigged type and buffer pair, runs of one pair, the item -> command map
    struct RRun { U32 mPair, mFirst, mCount; };
    std::vector<VkDrawIndexedIndirectCommand> mRCmds;
    std::vector<U32> mRCount0;                  // the index count of each (a skin without a palette this frame draws nothing: count 0)
    std::vector<RRun> mRRuns;
    U32 mRTypeFirst[kGeoTypes + 1] = {};
    std::vector<U32> mRItemCmd;                 // per item: its command in mRCmds (~0u: none)
    U32 mMaxModel = 0;                          // the highest matrix slot its items name: the pass matrices must cover it whether or not the viewer sent that slot this frame (a fragment released by the viewer during the frame is still listed: an item must never index past the matrix scratch)
    std::vector<U64> mPalAddr;                  // per item: the palette address last bound (0: never): an item draws only with one
    std::vector<U32> mPalJoints;
    bool mDirtyPairs = true, mDirtyEntries = true;
    U64 mPinEpochSeen = ~0ull;
    U64 mListedFrame = ~0ull;           // the frame it was last listed (the buffers it reads count as in flight for the frames after)
    U64 mSafetyFrame = ~0ull;           // S5 safety: the frame an alpha record last checked its buffers against the live ones
    U32 mHandle = 0;
};
struct GeoStats
{
    U64 mRetains = 0, mReleases = 0, mReleaseUnknown = 0, mOverflow = 0, mPrepares = 0, mDrawRecords = 0, mDrawStale = 0, mDrawNoProgram = 0, mScratchFail = 0, mNotInFrame = 0;
    U64 mFragsListed = 0, mFragsUnknown = 0, mItemsListed = 0, mItemsSkippedPair = 0, mCommandsTotal = 0;
    U64 mRebuilds = 0, mRebuildFirst = 0, mRebuildPairs = 0, mRebuildTex = 0, mRebuildPin = 0, mItemsRewritten = 0;
    U64 mBufEvents = 0, mBufEventFrags = 0, mBufGone = 0, mBufGoneFrags = 0, mInFlightAsks = 0, mInFlightYes = 0;
    U64 mTexUpdates = 0, mTexUpdateEntries = 0, mPinAcquires = 0, mPinReleases = 0, mPinSlotZero = 0, mFlushes = 0;
    U64 mCheckRuns = 0, mCheckFrags = 0, mStaleAddr = 0, mStaleSlot = 0, mStaleCmd = 0;
    U64 mCommands[kGeoTypes] = {}, mCommandsMax[kGeoTypes] = {}, mDrawNs[kGeoTypes] = {}, mDrawCalls[kGeoTypes] = {};
    U64 mMaxFragsFrame = 0, mMaxItemsFrame = 0;
    U64 mNsPrepare = 0, mNsEvents = 0, mNsPins = 0, mNsRebuild = 0, mNsList = 0, mNsCopy = 0, mNsCheck = 0, mNsRetain = 0, mNsRelease = 0, mNsTexUpdate = 0, mNsMatrices = 0;
    U64 mNsDraw = 0, mNsDrawMatrices = 0, mNsDrawState = 0, mNsDrawCall = 0;
    U64 mMatricesSent = 0;
    U64 mLastLog = 0;
};
static struct GeoState
{
    Buffer mTable;
    GeoGpuG* mT = nullptr;
    std::vector<U32> mRowFrame;         // the frame each row was last written in (the device lost report: which rows name the faulting address, and when they were written)
    std::vector<GeoGpuG> mTCpu;         // the CPU's copy of every row (the table is write-combined memory: reading it costs about a microsecond a float; the per-frame row copies read this)
    bool mReady = false, mFailed = false;
    U32 mHigh = 0;
    std::vector<U32> mFree;
    std::vector<std::pair<U32, U64>> mRetired;
    Buffer mPbrTable;                                   // S5c: the PBR rows
    F32* mPbrT = nullptr;
    U32 mPbrHigh = 0;
    std::vector<U32> mPbrFree;
    std::vector<std::pair<U32, U64>> mPbrRetired;
    std::vector<GeoFrag*> mBy;                          // by the viewer's dense handle
    std::unordered_multimap<U32, U32> mByBuf;           // buffer name -> handle of a fragment that reads it
    U64 mPreparedFrame = ~0ull;
    Renderer::ScratchAlloc mCommands;
    U32 mFirst[kGeoTypes] = {}, mCount[kGeoTypes] = {};
    // S4 B: the frame's rigged draws by rigged type: a run = the commands of one buffer pair of one fragment (index buffer bound, one indirect indexed draw)
    struct RDraw { U32 mIbName; U32 mFirst, mCount; };       // (the index buffer by its GL name: looked up again at the draw, a buffer destroyed since the prepare must not be used)
    std::vector<RDraw> mRDraws[kGeoTypes];
    Renderer::ScratchAlloc mRCommands;
    Renderer::ScratchAlloc mSTable;         // LL_VK_GEO_ROWCOPY: the rows of the static items of this frame (copies of the table's rows), the static commands index it
    Renderer::ScratchAlloc mRTable;         // the rows of the rigged items of this frame (copies of the table's rows with the frame's palette), the rigged commands index it
    U64 mRBindings = 0, mRDead = 0, mRCmdsFrame = 0;
    F32 mRegionM[2048][16] = {};       // S4 C: slots 1-255 region matrices, 256-2047 the moving ones (the viewer sends the values of the slots the listed fragments use, every frame)
    U8 mHaveM[2048] = {};
    U32 mMaxSlot = 0;
    U32 mMaxSlotAll = 0;           // the highest matrix slot any fragment ever named (the alpha passes draw fragments the G-buffer list does not name: their tables must cover them)
    size_t mFragCount = 0;
    GeoStats mS;
} sGeo;

bool geo_timed()
{
    static const bool on = getenv("LL_VK_SCENE_TIMES") != nullptr;
    return on;
}
bool geo_stale_check_on()
{
    static const bool on = !(getenv("LL_VK_SCENE_GEO_STALECHECK") && getenv("LL_VK_SCENE_GEO_STALECHECK")[0] == '0');
    return on;
}
U64 geo_ns(const std::chrono::steady_clock::time_point& since)
{
    return (U64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - since).count();
}

bool geo_init()
{
    if (sGeo.mReady)
    {
        return true;
    }
    if (sGeo.mFailed)
    {
        return false;
    }
    sGeo.mFailed = true;
    Context& ctx = Context::instance();
    const auto& caps = ctx.getCaps();
    if (!caps.mBufferDeviceAddress || !caps.mMultiDrawIndirect || !caps.mDrawIndirectFirstInstance)
    {
        LL_WARNS("VkScene") << "scene geometry table: the device lacks buffer addresses or first-instance indirect draws" << LL_ENDL;
        return false;
    }
    if (!ctx.createBuffer((VkDeviceSize)sizeof(GeoGpuG) * kGeoCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Context::Memory::UploadDevice, sGeo.mTable) || !sGeo.mTable.mMapped || !sGeo.mTable.mAddress)
    {
        LL_WARNS("VkScene") << "scene geometry table: the table could not be created" << LL_ENDL;
        return false;
    }
    sGeo.mT = (GeoGpuG*)sGeo.mTable.mMapped;
    memset(sGeo.mT, 0, sizeof(GeoGpuG) * kGeoCapacity);
    sGeo.mTCpu.assign(kGeoCapacity, GeoGpuG());
    memset(sGeo.mTCpu.data(), 0, sizeof(GeoGpuG) * kGeoCapacity);
    sGeo.mRowFrame.assign(kGeoCapacity, 0u);
    ctx.addAddressDescriber(geo_explain_address);
    LL_INFOS("VkScene") << "host-block race fixes: static table draws read " << (geo_rowcopy_on() ? "rows copied for the frame (LL_VK_GEO_ROWCOPY, on)" : "the live table (LL_VK_GEO_ROWCOPY=0)") << "; a buffer renamed this frame is "
                        << (rename_once_on() ? "not renamed again (LL_VK_RENAME_ONCE, on)" : "renamed again by every write (LL_VK_RENAME_ONCE off)") << LL_ENDL;
    if (ctx.createBuffer((VkDeviceSize)160 * kPbrCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Context::Memory::UploadDevice, sGeo.mPbrTable) && sGeo.mPbrTable.mMapped && sGeo.mPbrTable.mAddress)
    {
        sGeo.mPbrT = (F32*)sGeo.mPbrTable.mMapped;
        memset(sGeo.mPbrT, 0, (size_t)160 * kPbrCapacity);
    }
    else
    {
        LL_WARNS("VkScene") << "scene geometry table: the PBR rows could not be created: PBR alpha draws are not served" << LL_ENDL;
    }
    ctx.addAddressDescriber([](unsigned long long at) -> std::string
    {
        const unsigned long long base = (unsigned long long)sGeo.mTable.mAddress, size = (unsigned long long)sizeof(GeoGpuG) * kGeoCapacity;
        if (!base || at + 0x10000 < base || at > base + size + 0x100000)
        {
            return std::string();
        }
        const unsigned long long offset = at - base;
        return "the scene geometry table, offset " + std::to_string(offset) + " = row " + std::to_string(offset / sizeof(GeoGpuG)) + " of " + std::to_string((unsigned long long)kGeoCapacity) + (offset >= size ? " (PAST THE END)" : "");
    });
    sGeo.mFailed = false;
    sGeo.mReady = true;
    LL_INFOS("VkScene") << "scene geometry table ready (" << kGeoCapacity << " entries, " << sizeof(GeoGpuG) * kGeoCapacity / (1024 * 1024) << " MB)" << LL_ENDL;
    return true;
}

void geo_reclaim(U64 frame)
{
    const U64 lag = (U64)Context::frameCount() + 1;
    size_t keep = 0;
    for (size_t i = 0; i < sGeo.mRetired.size(); ++i)
    {
        if (frame >= sGeo.mRetired[i].second + lag)
        {
            sGeo.mFree.push_back(sGeo.mRetired[i].first);
        }
        else
        {
            sGeo.mRetired[keep++] = sGeo.mRetired[i];
        }
    }
    sGeo.mRetired.resize(keep);
    keep = 0;
    for (size_t i = 0; i < sGeo.mPbrRetired.size(); ++i)
    {
        if (frame >= sGeo.mPbrRetired[i].second + lag)
        {
            sGeo.mPbrFree.push_back(sGeo.mPbrRetired[i].first);
        }
        else
        {
            sGeo.mPbrRetired[keep++] = sGeo.mPbrRetired[i];
        }
    }
    sGeo.mPbrRetired.resize(keep);
}

GeoFrag* geo_find(U32 handle)
{
    return handle < sGeo.mBy.size() ? sGeo.mBy[handle] : nullptr;
}

void geo_pin_set(GeoFrag& f, size_t k, U32 name)
{
    if (f.mPin[k])
    {
        if (sPins[f.mPin[k]].mGeoRefs)
        {
            --sPins[f.mPin[k]].mGeoRefs;
        }
        rtPinRelease(f.mPin[k]);
        ++sGeo.mS.mPinReleases;
        f.mPin[k] = 0;
    }
    f.mTexName[k] = name;
    if (name)
    {
        const U32 pin = rtPinAcquire(name);
        f.mPin[k] = pin;
        if (pin)
        {
            ++sPins[pin].mGeoRefs;
            ++sGeo.mS.mPinAcquires;
        }
    }
}

void geo_release(U32 handle)
{
    GeoFrag* f = geo_find(handle);
    if (!f)
    {
        return;
    }
    const U64 frame = Context::instance().getFrameNumber();
    for (U32 slot : f->mSlots)
    {
        sGeo.mRetired.push_back({ slot, frame });
    }
    for (U32 slot1 : f->mPbrOfItem)
    {
        if (slot1)
        {
            sGeo.mPbrRetired.push_back({ slot1 - 1, frame });
        }
    }
    for (size_t k = 0; k < f->mPin.size(); ++k)
    {
        geo_pin_set(*f, k, 0);
    }
    for (const ResPair& p : f->mPairs)
    {
        for (U32 name : { p.mVb, p.mIb })
        {
            auto range = sGeo.mByBuf.equal_range(name);
            for (auto m = range.first; m != range.second; ++m)
            {
                if (m->second == handle)
                {
                    sGeo.mByBuf.erase(m);
                    break;
                }
            }
        }
    }
    sGeo.mBy[handle] = nullptr;
    --sGeo.mFragCount;
    Context::instance().flight(Context::FL_GEO_RELEASE, (unsigned long long)handle, (unsigned long long)f->mItems.size());
    delete f;
}

// kind 20: mFragment = the viewer's handle, mEntries x GeoWire, then mPaletteFloats GL names (the fragment's textures in list order)
void geo_retain(const LLVkBridge::ShadowPacketsHeader& h, const U8* blob)
{
    if (!geo_init())
    {
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    geo_release(h.mFragment);
    geo_reclaim(Context::instance().getFrameNumber());
    const LLVkBridge::GeoWire* items = (const LLVkBridge::GeoWire*)blob;
    const U32* names = (const U32*)(items + h.mEntries);
    GeoFrag* f = new GeoFrag();
    f->mHandle = h.mFragment;
    f->mItems.assign(items, items + h.mEntries);
    f->mPalAddr.assign(h.mEntries, 0);
    f->mPalJoints.assign(h.mEntries, 0);
    for (U32 i = 0; i < h.mEntries; ++i)
    {
        f->mMaxModel = std::max(f->mMaxModel, items[i].mE.mMatrix);
        if (items[i].mType == kGeoAlphaType || items[i].mType == kGeoAlphaRiggedType)
        {
            f->mMaxModel = std::max(f->mMaxModel, items[i].mE.mPad3);      // S5a: the texture matrix slot indexes the pass's tables too
        }
    }
    sGeo.mMaxSlotAll = std::max(sGeo.mMaxSlotAll, std::min<U32>(f->mMaxModel, 2047u));
    for (U32 i = 0; i < h.mEntries; ++i)
    {
        U32 slot;
        if (!sGeo.mFree.empty())
        {
            slot = sGeo.mFree.back();
            sGeo.mFree.pop_back();
        }
        else if (sGeo.mHigh < kGeoCapacity)
        {
            slot = sGeo.mHigh++;
        }
        else
        {
            if (sGeo.mS.mOverflow++ == 0)
            {
                LL_WARNS("VkScene") << "scene geometry table is full (" << kGeoCapacity << " entries): the rest are not drawn from it" << LL_ENDL;
            }
            for (U32 s : f->mSlots)
            {
                sGeo.mRetired.push_back({ s, Context::instance().getFrameNumber() });
            }
            delete f;
            return;
        }
        f->mSlots.push_back(slot);
        const LLVkBridge::GeoEntry& e = f->mItems[i].mE;
        size_t k = 0;
        while (k < f->mPairs.size() && !(f->mPairs[k].mVb == e.mVertexBuffer && f->mPairs[k].mIb == e.mIndexBuffer))
        {
            ++k;
        }
        if (k == f->mPairs.size())
        {
            ResPair p;
            p.mVb = e.mVertexBuffer;
            p.mIb = e.mIndexBuffer;
            f->mPairs.push_back(p);
            sGeo.mByBuf.emplace(p.mVb, h.mFragment);
            sGeo.mByBuf.emplace(p.mIb, h.mFragment);
        }
        f->mPairOf.push_back((U32)k);
    }
    f->mAddr.assign(2 * f->mPairs.size(), 0);
    f->mPbrOfItem.assign(h.mEntries, 0);
    if (h.mMatrices && sGeo.mPbrT)
    {
        const PbrWire* pbr = (const PbrWire*)(names + h.mPaletteFloats);
        for (U32 k = 0; k < h.mMatrices; ++k)
        {
            U32 slot;
            if (!sGeo.mPbrFree.empty())
            {
                slot = sGeo.mPbrFree.back();
                sGeo.mPbrFree.pop_back();
            }
            else if (sGeo.mPbrHigh < kPbrCapacity)
            {
                slot = sGeo.mPbrHigh++;
            }
            else
            {
                continue;       // (the table is full: that item has no PBR row and is not drawn)
            }
            if (pbr[k].mItem < h.mEntries)
            {
                memcpy(sGeo.mPbrT + (size_t)40 * slot, pbr[k].mV, 160);
                f->mPbrOfItem[pbr[k].mItem] = slot + 1;
            }
            else
            {
                sGeo.mPbrRetired.push_back({ slot, Context::instance().getFrameNumber() });
            }
        }
    }
    const size_t textures = h.mPaletteFloats;
    f->mTexName.assign(textures, 0);
    f->mPin.assign(textures, 0);
    f->mTexSlot.assign(textures, ~0u);
    for (size_t k = 0; k < textures; ++k)
    {
        geo_pin_set(*f, k, names[k]);
    }
    if (sGeo.mBy.size() <= h.mFragment)
    {
        sGeo.mBy.resize(h.mFragment + 1 + h.mFragment / 4, nullptr);
    }
    sGeo.mBy[h.mFragment] = f;
    ++sGeo.mFragCount;
    ++sGeo.mS.mRetains;
    Context::instance().flight(Context::FL_GEO_RETAIN, (unsigned long long)h.mFragment, (unsigned long long)h.mEntries, (unsigned long long)f->mPairs.size(), (unsigned long long)f->mMaxModel);
    sGeo.mS.mNsRetain += geo_ns(t0);
}

// kind 24: mFragment = handle, mEntries x { local texture number (0 based), GL name }
void geo_tex_update(const LLVkBridge::ShadowPacketsHeader& h, const U8* blob)
{
    const auto t0 = std::chrono::steady_clock::now();
    GeoFrag* f = geo_find(h.mFragment);
    ++sGeo.mS.mTexUpdates;
    if (!f)
    {
        return;
    }
    const U32* u = (const U32*)blob;
    for (U32 i = 0; i < h.mEntries; ++i)
    {
        const U32 k = u[2 * i], name = u[2 * i + 1];
        if (k < f->mTexName.size() && f->mTexName[k] != name)
        {
            geo_pin_set(*f, k, name);
            f->mDirtyEntries = true;
            ++sGeo.mS.mTexUpdateEntries;
        }
    }
    sGeo.mS.mNsTexUpdate += geo_ns(t0);
}

// the matrices a pass needs for its region matrices: modelview, modelview * projection, the normal matrix (columns of the inverse transpose of the modelview's upper 3x3), as LLRender::syncMatrices()
// would give the shader (the builder computes them with glm: float rounding differs in the last bits)
void geo_matrix(const F32* view, const F32* proj, const F32* model, F32* out)
{
    auto mul = [](const F32* a, const F32* b, F32* r)
    {
        for (int c = 0; c < 4; ++c)
        {
            for (int row = 0; row < 4; ++row)
            {
                r[c * 4 + row] = a[0 * 4 + row] * b[c * 4 + 0] + a[1 * 4 + row] * b[c * 4 + 1] + a[2 * 4 + row] * b[c * 4 + 2] + a[3 * 4 + row] * b[c * 4 + 3];
            }
        }
    };
    F32 mv[16];
    if (model)
    {
        mul(view, model, mv);
    }
    else
    {
        memcpy(mv, view, sizeof(mv));
    }
    memcpy(out, mv, 64);
    mul(proj, mv, out + 16);
    const F32* a0 = mv; const F32* a1 = mv + 4; const F32* a2 = mv + 8;
    auto cross = [](const F32* x, const F32* y, F32* r) { r[0] = x[1] * y[2] - x[2] * y[1]; r[1] = x[2] * y[0] - x[0] * y[2]; r[2] = x[0] * y[1] - x[1] * y[0]; };
    F32 c12[3], c20[3], c01[3];
    cross(a1, a2, c12);
    cross(a2, a0, c20);
    cross(a0, a1, c01);
    const F32 det = a0[0] * c12[0] + a0[1] * c12[1] + a0[2] * c12[2];
    const F32 inv = det != 0.f ? 1.f / det : 0.f;
    F32* n = out + 32;
    for (int k = 0; k < 3; ++k)
    {
        n[0 + k] = c12[k] * inv;
        n[4 + k] = c20[k] * inv;
        n[8 + k] = c01[k] * inv;
    }
    n[3] = n[7] = n[11] = 0.f;
}

U32 geo_pin_slot(U32 pin)
{
    return pin && pin < sPins.size() ? sPins[pin].mSlot : 0u;
}

// the entries of a fragment from the pairs and slots as they are now (the pairs have been looked at)
// The device lost report asks this for every data fault address (Context::addAddressDescriber): the GL buffers whose arena range holds the address or ends just below it (name, size, renames and the frame of the last
// one), and the table rows (the CPU's copy) that have a pointer within 2 MB below it (the pull shaders read up to a megabyte past an array base), with the frame each row was last written in.
std::string geo_explain_address(unsigned long long at)
{
    std::string text;
    int shown = 0;
    for (const auto& kv : sGL.mBuffers)
    {
        const Buf& b = kv.second;
        const U64 base = (U64)b.mVk.mAddress;
        if (!base || !b.mArena.valid())
        {
            continue;
        }
        const U64 size = b.mSize;
        if (at >= base && at < base + size + 0x1000)
        {
            char buf[320];
            snprintf(buf, sizeof(buf), "GL buffer %u (%llu bytes at 0x%llx, offset %llu in it): renamed %llu times in all, last rename in frame %lld, renames in that frame %u, last used by a draw in frame %lld; ", kv.first,
                     (unsigned long long)size, (unsigned long long)base, (unsigned long long)(at - base), (unsigned long long)b.mMoves, (long long)(b.mRenamedFrame == ~0ull ? -1 : (long long)b.mRenamedFrame), b.mRenamesInFrame,
                     (long long)(b.mLastUseFrame == ~0ull ? -1 : (long long)b.mLastUseFrame));
            text += buf;
            if (++shown >= 3)
            {
                break;
            }
        }
    }
    if (shown == 0)
    {
        text += "no GL buffer holds it; ";
    }
    struct Field { const char* mName; const U32* mPtr; };
    int rows = 0;
    for (U32 slot = 0; slot < (U32)sGeo.mTCpu.size() && rows < 6; ++slot)
    {
        const GeoGpuG& g = sGeo.mTCpu[slot];
        const Field fields[] = { { "positions", g.mPositions }, { "normals", g.mNormals }, { "texcoords", g.mTexcoords }, { "colors", g.mColors }, { "indices", g.mIndices }, { "tangents", g.mTangents },
                                 { "texcoords1", g.mTexcoords1 }, { "texcoords2", g.mTexcoords2 }, { "weights", g.mWeights }, { "palette", g.mPalette } };
        for (const Field& f : fields)
        {
            const U64 p = (U64)f.mPtr[0] | ((U64)f.mPtr[1] << 32);
            if (p && at >= p && at - p < 0x200000)
            {
                char buf[240];
                snprintf(buf, sizeof(buf), "table row %u, %s at 0x%llx (the address is %llu bytes past it), row written in frame %u (first %u, model %u, flags 0x%x); ", slot, f.mName, (unsigned long long)p,
                         (unsigned long long)(at - p), sGeo.mRowFrame.size() > slot ? sGeo.mRowFrame[slot] : 0u, g.mFirst, g.mModel, g.mFlags);
                text += buf;
                ++rows;
                break;
            }
        }
    }
    if (rows == 0)
    {
        text += "no table row has a pointer within 2 MB below it (the live table's CPU copy)";
    }
    return text;
}

void geo_write_items(GeoFrag& f)
{
    for (size_t i = 0; i < f.mItems.size(); ++i)
    {
        const LLVkBridge::GeoEntry& e = f.mItems[i].mE;
        const ResPair& p = f.mPairs[f.mPairOf[i]];
        if (!p.mOk)
        {
            continue;
        }
        GeoGpuG g;
        memset(&g, 0, sizeof(g));
        const U64 pa = (U64)p.mPvb->mVk.mAddress, ia = (U64)p.mPib->mVk.mAddress;
        const U64 na = pa + e.mNormalOffset, ta = pa + e.mTexcoordOffset, ca = pa + e.mColorOffset;
        const U64 tga = pa + e.mTangentOffset, t1a = pa + e.mTexcoord1Offset, t2a = pa + e.mTexcoord2Offset;
        g.mPositions[0] = (U32)pa; g.mPositions[1] = (U32)(pa >> 32);
        g.mNormals[0] = (U32)na; g.mNormals[1] = (U32)(na >> 32);
        g.mTexcoords[0] = (U32)ta; g.mTexcoords[1] = (U32)(ta >> 32);
        g.mColors[0] = (U32)ca; g.mColors[1] = (U32)(ca >> 32);
        g.mIndices[0] = (U32)ia; g.mIndices[1] = (U32)(ia >> 32);
        g.mTangents[0] = (U32)tga; g.mTangents[1] = (U32)(tga >> 32);
        g.mTexcoords1[0] = (U32)t1a; g.mTexcoords1[1] = (U32)(t1a >> 32);
        g.mTexcoords2[0] = (U32)t2a; g.mTexcoords2[1] = (U32)(t2a >> 32);
        g.mFirst = e.mFirstIndex;
        g.mModel = e.mMatrix;
        g.mFlags = e.mFlags;
        if (f.mItems[i].mType == kGeoAlphaType || f.mItems[i].mType == kGeoAlphaRiggedType)
        {
            g.mPad[0] = e.mPad3;            // S5a: the slot of the draw's texture matrix (0 = none)
        }
        if (i < f.mPbrOfItem.size() && f.mPbrOfItem[i])
        {
            g.mPad[1] = f.mPbrOfItem[i] - 1;       // S5c: its PBR row (S6 X4: the glTF items of the G passes have one too)
        }
        g.mEmissiveOffset = e.mEmissiveOffset;      // S5e (the alpha items' glow draw), S6 X2 (the glow pass items: the emissive array; 0 for everything else)
        if (e.mFlags & LLVkBridge::GEO_RIGGED)
        {
            const U64 wa = pa + e.mWeightOffset;
            g.mWeights[0] = (U32)wa;
            g.mWeights[1] = (U32)(wa >> 32);
            g.mPalette[0] = (U32)f.mPalAddr[i];            // (the last palette bound: see the shadow in GeoFrag)
            g.mPalette[1] = (U32)(f.mPalAddr[i] >> 32);
            g.mJoints = f.mPalJoints[i];
        }
        auto slot_of = [&](U32 n) -> U32 { return n ? f.mTexSlot[n - 1] : 0u; };
        g.mSlots[0] = slot_of(e.mTextures[0]) | (slot_of(e.mTextures[1]) << 16);
        g.mSlots[1] = slot_of(e.mTextures[2]) | (slot_of(e.mTextures[3]) << 16);
        g.mMapSlots = slot_of(e.mNormalMap) | (slot_of(e.mSpecularMap) << 16);
        memcpy(g.mSpecular, e.mSpecular, sizeof(g.mSpecular));
        g.mEnvIntensity = e.mEnvIntensity;
        g.mCutoff = e.mCutoff;
        g.mBrightness = e.mBrightness;
        sGeo.mT[f.mSlots[i]] = g;
        sGeo.mTCpu[f.mSlots[i]] = g;
        sGeo.mRowFrame[f.mSlots[i]] = (U32)Context::instance().getFrameNumber();
    }
}

// An item's index range has to lie inside its index buffer (16-bit indices). Nothing checked that: a range left over from before a buffer was rewritten smaller (or a buffer given a new allocation under
// the same name) made the pull shader read indices past the end and so vertices anywhere in memory. Giant triangles over a whole alpha pass, 100-700 ms frames inside the alpha run, and the hangs
// past the driver's 2 s limit that ended five of today's runs are what that would look like. A failing item is not drawn (count 0), counted, and the first ones are logged.
static U64 sGeoBadRange = 0;
static bool geo_item_ok(const GeoFrag& f, size_t i)
{
    const ResPair& p = f.mPairs[f.mPairOf[i]];
    if (!p.mOk)
    {
        return false;
    }
    const LLVkBridge::GeoEntry& e = f.mItems[i].mE;
    const U64 end = ((U64)e.mFirstIndex + (U64)e.mIndexCount) * 2ull;
    if (p.mPib && end > (U64)p.mPib->mVk.mSize)
    {
        if (++sGeoBadRange <= 30)
        {
            LL_WARNS("VkScene") << "scene table item left out: its index range is past the end of its index buffer: first index " << e.mFirstIndex << " count " << e.mIndexCount << " (" << end << " bytes of indices) in a buffer of "
                                << (U64)p.mPib->mVk.mSize << " bytes, index buffer name " << p.mIb << ", vertex buffer name " << p.mVb << ", type " << f.mItems[i].mType << " (" << sGeoBadRange << " so far)" << LL_ENDL;
        }
        return false;
    }
    return true;
}

// the draw commands of the usable items, sorted by pass type
void geo_build_commands(GeoFrag& f)
{
    f.mCmds.clear();
    for (U32 t = 0; t < kGeoTypes; ++t)
    {
        f.mTypeFirst[t] = (U32)f.mCmds.size();
        for (size_t i = 0; i < f.mItems.size(); ++i)
        {
            if (f.mItems[i].mType == t && geo_item_ok(f, i))
            {
                f.mCmds.push_back({ f.mItems[i].mE.mIndexCount, 1u, 0u, f.mSlots[i] });
            }
        }
    }
    f.mTypeFirst[kGeoTypes] = (U32)f.mCmds.size();
    // S4 B: the rigged items, by rigged type and buffer pair
    f.mRCmds.clear();
    f.mRCount0.clear();
    f.mRRuns.clear();
    f.mRItemCmd.assign(f.mItems.size(), ~0u);
    for (U32 rt = 0; rt < kGeoTypes; ++rt)
    {
        f.mRTypeFirst[rt] = (U32)f.mRRuns.size();
        for (size_t k = 0; k < f.mPairs.size(); ++k)
        {
            const U32 first = (U32)f.mRCmds.size();
            for (size_t i = 0; i < f.mItems.size(); ++i)
            {
                if (f.mItems[i].mType == kGeoTypes + rt && f.mPairOf[i] == k && geo_item_ok(f, i))
                {
                    f.mRItemCmd[i] = (U32)f.mRCmds.size();
                    f.mRCmds.push_back({ f.mPalAddr[i] ? f.mItems[i].mE.mIndexCount : 0u, 1u, f.mItems[i].mE.mFirstIndex, 0, f.mSlots[i] });
                    f.mRCount0.push_back(f.mItems[i].mE.mIndexCount);
                }
            }
            if (f.mRCmds.size() > first)
            {
                f.mRRuns.push_back({ (U32)k, first, (U32)f.mRCmds.size() - first });
            }
        }
    }
    f.mRTypeFirst[kGeoTypes] = (U32)f.mRRuns.size();
    // S5d: the rigged alpha items, in item order (indexed draws; the count is 0 until a palette is bound this frame)
    f.mARCmds.clear();
    f.mARCount0.clear();
    f.mARItem.clear();
    f.mARItemCmd.assign(f.mItems.size(), ~0u);
    for (size_t i = 0; i < f.mItems.size(); ++i)
    {
        if (f.mItems[i].mType == kGeoAlphaRiggedType)
        {
            const bool ok = geo_item_ok(f, i);
            f.mARItemCmd[i] = (U32)f.mARCmds.size();
            f.mARItem.push_back((U32)i);
            f.mARCmds.push_back({ ok && f.mPalAddr[i] ? f.mItems[i].mE.mIndexCount : 0u, 1u, f.mItems[i].mE.mFirstIndex, 0, f.mSlots[i] });
            f.mARCount0.push_back(ok ? f.mItems[i].mE.mIndexCount : 0u);
        }
    }
    // S5a: the alpha items, in item order
    f.mACmds.clear();
    for (size_t i = 0; i < f.mItems.size(); ++i)
    {
        if (f.mItems[i].mType == kGeoAlphaType)
        {
            f.mACmds.push_back({ geo_item_ok(f, i) ? f.mItems[i].mE.mIndexCount : 0u, 1u, 0u, f.mSlots[i] });
        }
    }
}

// the fragment was named by an event (or is new): look at its buffers and textures again, write its entries and commands
void geo_rebuild(GeoFrag& f, U64 frame_now, bool first)
{
    GeoStats& s = sGeo.mS;
    ++s.mRebuilds;
    bool pairs_changed = false;
    if (f.mDirtyPairs)
    {
        for (size_t k = 0; k < f.mPairs.size(); ++k)
        {
            ResPair& p = f.mPairs[k];
            const int c = res_pair_check(p);
            if (c == 1)
            {
                pairs_changed = true;
            }
            if (p.mOk)
            {
                p.mPvb->mGeoWatched = true;
                p.mPvb->mResName = p.mVb;
                p.mPib->mGeoWatched = true;
                p.mPib->mResName = p.mIb;
                f.mAddr[2 * k] = (U64)p.mPvb->mVk.mAddress;
                f.mAddr[2 * k + 1] = (U64)p.mPib->mVk.mAddress;
            }
        }
        // a pair that is not usable yet stays dirty (looked at again next frame: its buffer appears without an event we can see)
        bool all_ok = true;
        for (const ResPair& p : f.mPairs)
        {
            all_ok = all_ok && p.mOk;
        }
        f.mDirtyPairs = !all_ok;
    }
    bool slots_changed = false;
    for (size_t k = 0; k < f.mTexSlot.size(); ++k)
    {
        const U32 slot = geo_pin_slot(f.mPin[k]);
        if (f.mTexSlot[k] != slot)
        {
            f.mTexSlot[k] = slot;
            slots_changed = true;
            s.mPinSlotZero += slot == 0 ? 1 : 0;
        }
    }
    s.mRebuildFirst += first ? 1 : 0;
    s.mRebuildPairs += pairs_changed ? 1 : 0;
    s.mRebuildTex += (slots_changed && !first) ? 1 : 0;
    s.mRebuildPin += f.mPinEpochSeen != sGeoPinEpoch ? 1 : 0;
    if (first || pairs_changed || slots_changed || f.mDirtyEntries)
    {
        s.mItemsRewritten += f.mItems.size();
        geo_write_items(f);
        geo_build_commands(f);
        f.mDirtyEntries = false;
    }
    f.mPinEpochSeen = sGeoPinEpoch;
    (void)frame_now;
}

// a watched buffer got other storage (called by the write path through res_touched, causes 0 and 2): the fragments reading it look at their pairs when next listed
void geo_buffer_changed(U32 name)
{
    ++sGeo.mS.mBufEvents;
    Context::instance().flight(Context::FL_GEO_BUF, (unsigned long long)name, 0ull);
    auto range = sGeo.mByBuf.equal_range(name);
    for (auto m = range.first; m != range.second; ++m)
    {
        if (GeoFrag* f = geo_find(m->second))
        {
            f->mDirtyPairs = true;
            ++sGeo.mS.mBufEventFrags;
        }
    }
}

// a buffer entry is erased: no pair of ours may keep a pointer to it
void geo_buffer_gone_impl(U32 name)
{
    ++sGeo.mS.mBufGone;
    Context::instance().flight(Context::FL_GEO_BUF, (unsigned long long)name, 1ull);
    auto range = sGeo.mByBuf.equal_range(name);
    for (auto m = range.first; m != range.second; ++m)
    {
        GeoFrag* f = geo_find(m->second);
        if (!f)
        {
            continue;
        }
        ++sGeo.mS.mBufGoneFrags;
        f->mDirtyPairs = true;
        for (ResPair& p : f->mPairs)
        {
            if (p.mVb == name)
            {
                p.mPvb = nullptr;
                p.mOk = false;
                p.mGen = ~0ull;
            }
            if (p.mIb == name)
            {
                p.mPib = nullptr;
                p.mOk = false;
                p.mGen = ~0ull;
            }
        }
    }
}

bool geo_in_flight_impl(U32 name, U64 frame, U64 min_listed)
{
    ++sGeo.mS.mInFlightAsks;
    auto range = sGeo.mByBuf.equal_range(name);
    for (auto m = range.first; m != range.second; ++m)
    {
        const GeoFrag* f = geo_find(m->second);
        // (min_listed: only a listing at or after the buffer's last rename counts: the new range has not been read by any frame before that)
        if (f && f->mListedFrame != ~0ull && f->mListedFrame >= min_listed && frame < f->mListedFrame + Context::frameCount() + 1)
        {
            ++sGeo.mS.mInFlightYes;
            return true;
        }
    }
    return false;
}

// writes by the main thread to a buffer the table reads are recorded before the draw that reads them
void geo_flush_if_needed()
{
    if (sPendingBufs.empty())
    {
        return;
    }
    for (U32 name : sPendingBufs)
    {
        if (sGeo.mByBuf.find(name) != sGeo.mByBuf.end())
        {
            ++sGeo.mS.mFlushes;
            flush_buffer_updates();
            return;
        }
    }
}

// the safety net, every 16th prepare: what the table holds against a fresh look (buffer addresses by a plain lookup of the names, texture slots by the pins and by the per-frame path), healed
void geo_stale_check(const std::vector<GeoFrag*>& listed)
{
    GeoStats& s = sGeo.mS;
    Renderer& r = Renderer::instance();
    ++s.mCheckRuns;
    for (GeoFrag* f : listed)
    {
        ++s.mCheckFrags;
        bool stale = false;
        for (size_t k = 0; k < f->mPairs.size(); ++k)
        {
            const ResPair& p = f->mPairs[k];
            auto v = sGL.mBuffers.find(p.mVb);
            auto i = sGL.mBuffers.find(p.mIb);
            const bool ok = v != sGL.mBuffers.end() && i != sGL.mBuffers.end() && v->second.mVk.valid() && i->second.mVk.valid() && v->second.mVk.mAddress && i->second.mVk.mAddress;
            if (ok != p.mOk || (ok && (f->mAddr[2 * k] != (U64)v->second.mVk.mAddress || f->mAddr[2 * k + 1] != (U64)i->second.mVk.mAddress)))
            {
                ++s.mStaleAddr;
                stale = true;
                static U32 said = 0;
                if (said++ < 8)
                {
                    LL_WARNS("VkScene") << "scene geometry stale check: fragment " << f->mHandle << " pair " << k << " (vb " << p.mVb << ", ib " << p.mIb << ") table " << (p.mOk ? "usable" : "not usable") << ", now "
                        << (ok ? "usable" : "not usable") << (ok ? "" : "") << LL_ENDL;
                }
            }
        }
        for (size_t k = 0; k < f->mTexSlot.size(); ++k)
        {
            // the pin's slot against the slot the per-frame lookup gives for the same GL name (0 = no image yet: both 0)
            const U32 expect = f->mTexName[k] ? packet_texture_slot(r, f->mTexName[k]) : 0u;
            if (expect != f->mTexSlot[k])
            {
                ++s.mStaleSlot;
                stale = true;
                static U32 said = 0;
                if (said++ < 8)
                {
                    LL_WARNS("VkScene") << "scene geometry stale check: fragment " << f->mHandle << " texture " << k << " (GL " << f->mTexName[k] << ") table slot " << f->mTexSlot[k] << ", per-frame lookup " << expect << LL_ENDL;
                }
            }
        }
        size_t usable = 0;
        for (size_t i = 0; i < f->mItems.size(); ++i)
        {
            usable += f->mPairs[f->mPairOf[i]].mOk ? 1 : 0;
        }
        if (usable != f->mCmds.size() + f->mRCmds.size() + f->mACmds.size() + f->mARCmds.size())
        {
            ++s.mStaleCmd;
            stale = true;
        }
        if (stale)
        {
            f->mDirtyPairs = true;
            f->mDirtyEntries = true;
        }
    }
}

// kind 21: once a frame, before the passes. Blob: mEntries handles (U32), then mMatrices x GeoSlotMatrix
void geo_prepare(const LLVkBridge::ShadowPacketsHeader& h, const U8* blob)
{
    if (!geo_init())
    {
        return;
    }
    if (!LLVkBridge::inFrame())
    {
        ++sGeo.mS.mNotInFrame;
        return;
    }
    const bool timed = geo_timed();
    const auto t_all = std::chrono::steady_clock::now();
    GeoStats& s = sGeo.mS;
    Renderer& r = Renderer::instance();
    const U64 frame_now = Context::instance().getFrameNumber();
    geo_reclaim(frame_now);
    const U32* handles = (const U32*)blob;
    const LLVkBridge::GeoSlotMatrix* matrices = (const LLVkBridge::GeoSlotMatrix*)(handles + h.mEntries);
    struct Binding { U32 mHandle, mItem, mOffset, mJoints; };
    const Binding* bindings = (const Binding*)(matrices + h.mMatrices);       // S4 B: mPaletteFloats = how many
    auto t0 = std::chrono::steady_clock::now();
    // pins: textures the viewer named since last frame are resolved (their slots are permanent from here on); a slot that changed makes the fragments look at their slots again
    rtPinUpdate();
    if (timed) s.mNsPins += geo_ns(t0);
    // matrices
    t0 = std::chrono::steady_clock::now();
    memset(sGeo.mHaveM, 0, sizeof(sGeo.mHaveM));
    sGeo.mMaxSlot = 0;
    for (U32 i = 0; i < h.mMatrices; ++i)
    {
        const U32 slot = matrices[i].mSlot;
        if (slot < 2048)
        {
            memcpy(sGeo.mRegionM[slot], matrices[i].mM, 64);
            sGeo.mHaveM[slot] = 1;
            sGeo.mMaxSlot = std::max(sGeo.mMaxSlot, slot);
        }
    }
    s.mMatricesSent += h.mMatrices;
    if (timed) s.mNsMatrices += geo_ns(t0);
    // events: the buffers whose storage changed
    t0 = std::chrono::steady_clock::now();
    if (!sGeoChanged.empty())
    {
        std::sort(sGeoChanged.begin(), sGeoChanged.end());
        sGeoChanged.erase(std::unique(sGeoChanged.begin(), sGeoChanged.end()), sGeoChanged.end());
        for (U32 name : sGeoChanged)
        {
            geo_buffer_changed(name);
        }
        sGeoChanged.clear();
    }
    if (timed) s.mNsEvents += geo_ns(t0);
    // the listed fragments: rebuilt when something named them, their commands counted by type
    static thread_local std::vector<GeoFrag*> listed;
    listed.clear();
    t0 = std::chrono::steady_clock::now();
    U32 counts[kGeoTypes] = {};
    U64 items_frame = 0;
    for (U32 i = 0; i < h.mEntries; ++i)
    {
        ++s.mFragsListed;
        GeoFrag* f = geo_find(handles[i]);
        if (!f)
        {
            ++s.mFragsUnknown;
            continue;
        }
        const bool first = f->mPinEpochSeen == ~0ull;
        if (first || f->mDirtyPairs || f->mDirtyEntries || f->mPinEpochSeen != sGeoPinEpoch)
        {
            const auto t1 = std::chrono::steady_clock::now();
            geo_rebuild(*f, frame_now, first);
            if (timed) s.mNsRebuild += geo_ns(t1);
        }
        if (f->mDirtyEntries)
        {
            geo_rebuild(*f, frame_now, false);
        }
        f->mListedFrame = frame_now;
        sGeo.mMaxSlot = std::max(sGeo.mMaxSlot, std::min<U32>(f->mMaxModel, 2047u));
        for (VkDrawIndexedIndirectCommand& c : f->mRCmds)
        {
            c.indexCount = 0;           // S4 B: a rigged item draws only when the viewer bound a palette for it in THIS frame (below): one that was not (its fragment replaced during the frame, ...) would use a palette address of an older frame
        }
        for (VkDrawIndexedIndirectCommand& c : f->mARCmds)
        {
            c.indexCount = 0;           // S5d: a rigged alpha item draws only when the viewer bound a palette for it in THIS frame (kind 31)
        }
        listed.push_back(f);
        items_frame += f->mItems.size();
        s.mItemsSkippedPair += f->mItems.size() - f->mCmds.size() - f->mRCmds.size() - f->mACmds.size() - f->mARCmds.size();
        for (U32 t = 0; t < kGeoTypes; ++t)
        {
            counts[t] += f->mTypeFirst[t + 1] - f->mTypeFirst[t];
        }
    }
    if (timed) s.mNsList += geo_ns(t0);
    // S4 B: this frame's palettes of the rigged items (the viewer sent the shared list before this record): the row's palette address and joints, and the command's count (a skin that has no palette draws nothing)
    for (U32 i = 0; i < h.mPaletteFloats; ++i)
    {
        const Binding& b = bindings[i];
        GeoFrag* f = geo_find(b.mHandle);
        ++sGeo.mRBindings;
        if (!f || b.mItem >= f->mItems.size() || b.mItem >= f->mRItemCmd.size() || f->mRItemCmd[b.mItem] == ~0u)
        {
            continue;
        }
        const U32 cmd = f->mRItemCmd[b.mItem];
        const U64 pal = b.mOffset != ~0u ? palette_address(b.mOffset) : 0;
        GeoGpuG& row = sGeo.mT[f->mSlots[b.mItem]];
        if (pal)
        {
            f->mPalAddr[b.mItem] = pal;
            f->mPalJoints[b.mItem] = b.mJoints;
            row.mPalette[0] = (U32)pal;
            row.mPalette[1] = (U32)(pal >> 32);
            row.mJoints = b.mJoints;
            GeoGpuG& cpu_row = sGeo.mTCpu[f->mSlots[b.mItem]];
            cpu_row.mPalette[0] = (U32)pal; cpu_row.mPalette[1] = (U32)(pal >> 32); cpu_row.mJoints = b.mJoints;
            f->mRCmds[cmd].indexCount = f->mRCount0[cmd];
        }
        else
        {
            f->mRCmds[cmd].indexCount = 0;
            ++sGeo.mRDead;
        }
    }
    s.mItemsListed += items_frame;
    s.mMaxFragsFrame = std::max<U64>(s.mMaxFragsFrame, h.mEntries);
    s.mMaxItemsFrame = std::max<U64>(s.mMaxItemsFrame, items_frame);
    if (geo_stale_check_on() && (s.mPrepares & 15u) == 0)
    {
        t0 = std::chrono::steady_clock::now();
        geo_stale_check(listed);
        if (timed) s.mNsCheck += geo_ns(t0);
    }
    // the commands of the frame, by type
    t0 = std::chrono::steady_clock::now();
    size_t total = 0;
    for (U32 t = 0; t < kGeoTypes; ++t)
    {
        sGeo.mFirst[t] = (U32)total;
        sGeo.mCount[t] = counts[t];
        total += counts[t];
        s.mCommands[t] += counts[t];
        s.mCommandsMax[t] = std::max<U64>(s.mCommandsMax[t], counts[t]);
    }
    s.mCommandsTotal += total;
    sGeo.mCommands = Renderer::ScratchAlloc();
    sGeo.mSTable = Renderer::ScratchAlloc();
    if (total)
    {
        sGeo.mCommands = r.allocVertexScratch(sizeof(VkDrawIndirectCommand) * total);
        if (!sGeo.mCommands.mPtr)
        {
            ++s.mScratchFail;
            std::fill(sGeo.mCount, sGeo.mCount + kGeoTypes, 0u);
        }
        else
        {
            VkDrawIndirectCommand* out = (VkDrawIndirectCommand*)sGeo.mCommands.mPtr;
            GeoGpuG* static_rows = nullptr;
            if (geo_rowcopy_on())
            {
                // the table is one buffer the GPU reads while the next frames are prepared, and the worker rewrites rows in place (a renamed buffer: the row names a new range): the static draws read rows of their own,
                // copied for this frame, as the rigged ones do (the commands are copied from the CPU's list, not read back from the write combined scratch)
                sGeo.mSTable = r.allocVertexScratch(sizeof(GeoGpuG) * total);
                if (sGeo.mSTable.mPtr && sGeo.mSTable.mAddress)
                {
                    static_rows = (GeoGpuG*)sGeo.mSTable.mPtr;
                }
                else
                {
                    sGeo.mSTable = Renderer::ScratchAlloc();
                    ++s.mScratchFail;
                }
            }
            U32 at[kGeoTypes];
            for (U32 t = 0; t < kGeoTypes; ++t)
            {
                at[t] = sGeo.mFirst[t];
            }
            for (const GeoFrag* f : listed)
            {
                for (U32 t = 0; t < kGeoTypes; ++t)
                {
                    const U32 n = f->mTypeFirst[t + 1] - f->mTypeFirst[t];
                    if (n && static_rows)
                    {
                        const VkDrawIndirectCommand* src = f->mCmds.data() + f->mTypeFirst[t];
                        for (U32 k = 0; k < n; ++k)
                        {
                            VkDrawIndirectCommand c = src[k];
                            static_rows[at[t] + k] = sGeo.mTCpu[c.firstInstance];
                            c.firstInstance = at[t] + k;
                            out[at[t] + k] = c;
                        }
                        at[t] += n;
                    }
                    else if (n)
                    {
                        memcpy(out + at[t], f->mCmds.data() + f->mTypeFirst[t], n * sizeof(VkDrawIndirectCommand));
                        at[t] += n;
                    }
                }
            }
        }
    }
    // S4 B: the rigged commands of the frame, by rigged type: runs of one buffer pair
    for (U32 rt = 0; rt < kGeoTypes; ++rt)
    {
        sGeo.mRDraws[rt].clear();
    }
    sGeo.mRCommands = Renderer::ScratchAlloc();
    sGeo.mRTable = Renderer::ScratchAlloc();
    size_t rtotal = 0;
    for (const GeoFrag* f : listed)
    {
        rtotal += f->mRCmds.size();
    }
    sGeo.mRCmdsFrame += rtotal;
    if (rtotal)
    {
        sGeo.mRCommands = r.allocVertexScratch(sizeof(VkDrawIndexedIndirectCommand) * rtotal);
        if (!sGeo.mRCommands.mPtr)
        {
            ++s.mScratchFail;
        }
        else
        {
            VkDrawIndexedIndirectCommand* rout = (VkDrawIndexedIndirectCommand*)sGeo.mRCommands.mPtr;
            U32 cursor = 0;
            for (const GeoFrag* f : listed)
            {
                for (U32 rt = 0; rt < kGeoTypes; ++rt)
                {
                    for (U32 k = f->mRTypeFirst[rt]; k < f->mRTypeFirst[rt + 1]; ++k)
                    {
                        const GeoFrag::RRun& run = f->mRRuns[k];
                        memcpy(rout + cursor, f->mRCmds.data() + run.mFirst, run.mCount * sizeof(VkDrawIndexedIndirectCommand));
                        sGeo.mRDraws[rt].push_back({ f->mPairs[run.mPair].mIb, cursor, run.mCount });
                        cursor += run.mCount;
                    }
                }
            }
            // The table is one buffer the GPU reads while the next frames are being prepared: a palette written into a row now would be read by the draws of the frames still in flight
            // (rigged items out of step with each other and with the camera). The rigged items draw from rows of their own, copied for this frame: the command's first instance is the copy.
            sGeo.mRTable = r.allocVertexScratch(sizeof(GeoGpuG) * (size_t)rtotal);
            if (sGeo.mRTable.mPtr && sGeo.mRTable.mAddress)
            {
                GeoGpuG* rows = (GeoGpuG*)sGeo.mRTable.mPtr;
                for (size_t i = 0; i < rtotal; ++i)
                {
                    rows[i] = sGeo.mTCpu[rout[i].firstInstance];
                    rout[i].firstInstance = (U32)i;
                }
            }
            else
            {
                sGeo.mRTable = Renderer::ScratchAlloc();
                ++s.mScratchFail;
            }
        }
    }
    if (timed) s.mNsCopy += geo_ns(t0);
    geo_flush_if_needed();
    sGeo.mPreparedFrame = frame_now;
    {
        Context& fc = Context::instance();
        fc.flight(Context::FL_GEO_PREPARE, (unsigned long long)h.mEntries, (unsigned long long)items_frame, (unsigned long long)total | ((unsigned long long)rtotal << 32), (unsigned long long)sGeo.mMaxSlot | ((unsigned long long)h.mMatrices << 32));
        fc.flight(Context::FL_GEO_CMDS, (unsigned long long)sGeo.mCommands.mAddress, (unsigned long long)(sizeof(VkDrawIndirectCommand) * total), (unsigned long long)sGeo.mRCommands.mAddress, (unsigned long long)(sizeof(VkDrawIndexedIndirectCommand) * rtotal));
    }
    ++s.mPrepares;
    if (timed) s.mNsPrepare += geo_ns(t_all);
    if (s.mPrepares - s.mLastLog >= 600)
    {
        s.mLastLog = s.mPrepares;
        const double n = (double)s.mPrepares;
        std::string per_type;
        for (U32 t = 0; t < kGeoTypes; ++t)
        {
            per_type += std::string(t ? "/" : "") + std::to_string((long long)((double)s.mCommands[t] / n + 0.5)) + "(max " + std::to_string(s.mCommandsMax[t]) + ")";
        }
        U64 pins = 0, resolves = 0, events = 0;
        LLVkWorker::rtPinCounts(pins, resolves, events);
        LL_INFOS("VkScene") << "scene geometry table, per frame over " << s.mPrepares << " prepares: " << (double)s.mFragsListed / n << " fragments listed (" << s.mFragsUnknown << " unknown handles in total, max "
            << s.mMaxFragsFrame << "), " << (double)s.mItemsListed / n << " entries (max " << s.mMaxItemsFrame << "), " << (double)s.mCommandsTotal / n << " commands (" << (double)s.mItemsSkippedPair / n
            << " entries without usable buffers), rebuilds " << (double)s.mRebuilds / n << " (first " << (double)s.mRebuildFirst / n << ", buffers changed " << (double)s.mRebuildPairs / n << ", texture slots changed "
            << (double)s.mRebuildTex / n << ", entries written " << (double)s.mItemsRewritten / n << "), pin epoch bumps " << sGeoPinEpoch << ", buffer events " << (double)s.mBufEvents / n << " naming "
            << (double)s.mBufEventFrags / n << " fragments, buffers gone " << (double)s.mBufGone / n << " (" << (double)s.mBufGoneFrags / n << " fragments), in-flight asks by the write path "
            << (double)s.mInFlightAsks / n << " (" << (double)s.mInFlightYes / n << " yes), texture update records " << (double)s.mTexUpdates / n << " (" << (double)s.mTexUpdateEntries / n << " entries), pins acquired "
            << (double)s.mPinAcquires / n << " released " << (double)s.mPinReleases / n << " (entries with slot 0: " << s.mPinSlotZero << "; all pins " << pins << ", resolves since last report " << resolves
            << ", events " << events << "), matrices sent " << (double)s.mMatricesSent / n << ", flushes " << s.mFlushes << ", commands by type (simple/material/mask/glow/spec/specmask/specglow/"
            << "norm/normmask/normglow/normspec/nsmask/nsglow) " << per_type << " | stale check: " << s.mCheckRuns << " runs over " << s.mCheckFrags << " fragments found " << s.mStaleAddr << " stale buffer pairs, "
            << s.mStaleSlot << " stale texture slots, " << s.mStaleCmd << " stale command lists | table: " << sGeo.mHigh << " slots used, " << sGeo.mFree.size() << " free, " << sGeo.mRetired.size()
            << " retired, " << sGeo.mFragCount << " fragments, " << s.mRetains << " retains, " << s.mReleases << " releases (" << s.mReleaseUnknown << " unknown), " << s.mOverflow << " overflows, "
            << s.mScratchFail << " scratch failures, " << s.mNotInFrame << " prepares outside a frame, " << s.mDrawStale << " draws without this frame's prepare, " << s.mDrawNoProgram << " draws without the program" << LL_ENDL;
        if (timed)
        {
            LL_INFOS("VkScene") << "scene geometry table, worker time per frame (us): prepare " << (double)s.mNsPrepare / 1000.0 / n << " = pins " << (double)s.mNsPins / 1000.0 / n << " + matrices "
                << (double)s.mNsMatrices / 1000.0 / n << " + buffer events " << (double)s.mNsEvents / 1000.0 / n << " + listing " << (double)s.mNsList / 1000.0 / n << " (of it rebuilds "
                << (double)s.mNsRebuild / 1000.0 / n << ") + stale check " << (double)s.mNsCheck / 1000.0 / n << " + command copy " << (double)s.mNsCopy / 1000.0 / n << "; retain " << (double)s.mNsRetain / 1000.0 / n
                << ", release " << (double)s.mNsRelease / 1000.0 / n << ", texture updates " << (double)s.mNsTexUpdate / 1000.0 / n << "; draw records " << (double)s.mNsDraw / 1000.0 / n << " (matrices "
                << (double)s.mNsDrawMatrices / 1000.0 / n << ", state " << (double)s.mNsDrawState / 1000.0 / n << ", draw call " << (double)s.mNsDrawCall / 1000.0 / n << ")" << LL_ENDL;
        }
    }
}

// kind 22: one pass type, with the program the viewer bound
void geo_draw(const LLVkBridge::ShadowPacketsHeader& h, const U8* blob)
{
    GeoStats& s = sGeo.mS;
    const U32 type = h.mPass;
    const bool rigged_draw = type >= kGeoTypes && type < 2 * kGeoTypes;       // S4 B: the rigged variants of the 13 types
    if (!sGeo.mReady || type >= 2 * kGeoTypes)
    {
        return;
    }
    const U64 frame_now = Context::instance().getFrameNumber();
    if (sGeo.mPreparedFrame != frame_now)
    {
        ++s.mDrawStale;
        return;
    }
    U32 count = 0;
    if (rigged_draw)
    {
        if (!sGeo.mRCommands.mPtr)
        {
            return;
        }
        for (const GeoState::RDraw& d : sGeo.mRDraws[type - kGeoTypes])
        {
            count += d.mCount;
        }
    }
    else
    {
        count = sGeo.mCount[type];
        if (count == 0 || !sGeo.mCommands.mPtr)
        {
            return;
        }
    }
    if (count == 0)
    {
        return;
    }
    const bool timed = geo_timed();
    const auto t_all = std::chrono::steady_clock::now();
    Renderer& r = Renderer::instance();
    Program* program = r.getProgram();
    if (!program || !program->isValid() || !r.bindlessAvailable())
    {
        ++s.mDrawNoProgram;
        return;
    }
    const Program::PacketUniforms& pu = program->packetUniforms();
    if (pu.mTable == ~0u)
    {
        ++s.mDrawNoProgram;
        return;
    }
    // the pass's matrices for the region matrix slots of this frame: made once for a view and projection (the draw records of a frame, 26 of them, share them: they were made again for each, 7 us a record)
    auto t0 = std::chrono::steady_clock::now();
    const F32* view = (const F32*)blob;
    const F32* proj = view + 16;
    const U32 slots = sGeo.mMaxSlot + 1;
    static struct SharedModels { U64 mFrame = ~0ull, mPrepares = ~0ull; U32 mSlots = 0; F32 mVP[32] = {}; Renderer::ScratchAlloc mModels; } shared;
    Renderer::ScratchAlloc models;
    const bool reuse = shared.mFrame == frame_now && shared.mPrepares == s.mPrepares && shared.mSlots == slots && memcmp(shared.mVP, view, sizeof(shared.mVP)) == 0 && shared.mModels.mPtr && shared.mModels.mAddress;
    if (reuse)
    {
        models = shared.mModels;
    }
    else
    {
        models = r.allocVertexScratch(176 * (size_t)slots);
        if (!models.mPtr || !models.mAddress)
        {
            ++s.mScratchFail;
            return;
        }
    }
    Context::instance().flight(Context::FL_GEO_DRAW, (unsigned long long)type, (unsigned long long)count, (unsigned long long)models.mAddress, (unsigned long long)slots);
    {
        // a timestamp stamp per table draw type: the device lost report ("the GPU is inside [...]") names the type when the stuck pass is the G-buffer pass (the pass was one lump)
        static U32 tags[2 * kGeoTypes] = {};
        static bool made[2 * kGeoTypes] = {};
        if (!made[type])
        {
            made[type] = true;
            tags[type] = Context::stampTag(std::string("scene table draw, type ") + std::to_string(type % kGeoTypes) + (rigged_draw ? " rigged" : ""));
        }
        Context::instance().gpuStamp(tags[type]);
    }
    if (!reuse)
    {
        F32* out = (F32*)models.mPtr;
        memset(out, 0, 176 * (size_t)slots);
        geo_matrix(view, proj, nullptr, out);               // slot 0: the view alone
        for (U32 slot = 1; slot < slots; ++slot)
        {
            if (sGeo.mHaveM[slot])
            {
                geo_matrix(view, proj, sGeo.mRegionM[slot], out + 44 * (size_t)slot);
            }
        }
        shared.mFrame = frame_now;
        shared.mPrepares = s.mPrepares;
        shared.mSlots = slots;
        memcpy(shared.mVP, view, sizeof(shared.mVP));
        shared.mModels = models;
    }
    if (timed) s.mNsDrawMatrices += geo_ns(t0);
    t0 = std::chrono::steady_clock::now();
    static thread_local VertexInput vi_cache;
    static thread_local std::vector<U32> used_cache;
    VertexInput& vi = vi_cache;
    vi.mBindings.clear();
    vi.mAttributes.clear();
    used_cache.clear();
    const U64 table_address = (rigged_draw && sGeo.mRTable.mAddress) ? (U64)sGeo.mRTable.mAddress : (!rigged_draw && sGeo.mSTable.mAddress) ? (U64)sGeo.mSTable.mAddress : (U64)sGeo.mTable.mAddress, models_address = (U64)models.mAddress;
    Context::instance().flight(Context::FL_GEO_DRAW, 100ull + type, table_address, (unsigned long long)((rigged_draw ? sGeo.mRTable.mAddress : sGeo.mSTable.mAddress) ? 1 : 0), (unsigned long long)count);       // (a static or rigged type draw: which table, 1 = rows copied for the frame)
    const S32 table_words[2] = { (S32)(U32)table_address, (S32)(U32)(table_address >> 32) };
    const S32 models_words[2] = { (S32)(U32)models_address, (S32)(U32)(models_address >> 32) };
    program->setFloatsAt(pu.mTable, reinterpret_cast<const F32*>(table_words), 2, 1);
    if (pu.mModels != ~0u)
    {
        program->setFloatsAt(pu.mModels, reinterpret_cast<const F32*>(models_words), 2, 1);
    }
    if (pu.mPbr != ~0u && sGeo.mPbrT)
    {   // S6 X4: the glTF passes read their PBR rows
        const U64 pbr_address = (U64)sGeo.mPbrTable.mAddress;
        const S32 pbr_words[2] = { (S32)(U32)pbr_address, (S32)(U32)(pbr_address >> 32) };
        program->setFloatsAt(pu.mPbr, reinterpret_cast<const F32*>(pbr_words), 2, 1);
    }
    if (!prepare_draw(vi, used_cache))
    {
        return;
    }
    geo_flush_if_needed();
    r.setVertexInput(vi);
    r.setTopology(topology(LLVkFormat::TOP_TRIANGLES));
    r.bindlessFlush();
    if (timed) s.mNsDrawState += geo_ns(t0);
    t0 = std::chrono::steady_clock::now();
    if (rigged_draw)
    {
        U32 drawn = 0, runs = 0;
        for (const GeoState::RDraw& d : sGeo.mRDraws[type - kGeoTypes])
        {
            Buf* ib = packet_buffer(d.mIbName);
            if (!ib || !ib->mVk.valid())
            {
                continue;
            }
            r.setIndexBuffer(ib->mVk.mBuffer, ib->mArena.mOffset, VK_INDEX_TYPE_UINT16);
            if (r.drawPacketsIndexed(nullptr, d.mCount, sGeo.mRCommands.mBuffer, sGeo.mRCommands.mOffset + (VkDeviceSize)d.mFirst * sizeof(VkDrawIndexedIndirectCommand)))
            {
                drawn += d.mCount;
                ++runs;
            }
        }
        if (drawn)
        {
            if (sStatsEnabled)
            {
                sStats.mDone += drawn;
            }
        }
        else
        {
            ++sStats.mRendererRefused;
        }
        sDraws += runs;
        sDrawsTotal += runs;
    }
    else
    {
        if (r.drawPackets(nullptr, count, sGeo.mCommands.mBuffer, sGeo.mCommands.mOffset + (VkDeviceSize)sGeo.mFirst[type] * sizeof(VkDrawIndirectCommand)))
        {
            if (sStatsEnabled)
            {
                sStats.mDone += count;
            }
        }
        else
        {
            ++sStats.mRendererRefused;
        }
        sDraws += count;
        sDrawsTotal += count;
    }
    if (timed) s.mNsDrawCall += geo_ns(t0);
    ++s.mDrawRecords;
    ++s.mDrawCalls[type % kGeoTypes];
    if (timed)
    {
        const U64 ns = geo_ns(t_all);
        s.mNsDraw += ns;
        s.mDrawNs[type % kGeoTypes] += ns;
    }
}

// kind 29 (S5a, doc/vulkan/S5-design.md): the alpha pass drawn from the scene table. mEntries == 0: the pass begins, blob = 32 floats (the view and the projection matrix of the pass): the matrices (modelview,
// modelview * projection, normal; and the raw texture matrices) of the slots the prepare of the frame knew are made once and shared by the runs that follow. mEntries > 0: one run, drawn with the pull
// program the viewer bound (alphaPullV with the lit or the fullbright alpha fragment shader); blob = mEntries x AlphaRange { fragment handle, first alpha item, count }: the ranges of the fragments'
// alpha items (their own command list, in item order) the run draws, in order.
struct AlphaStats
{
    U64 mRecords = 0, mRanges = 0, mCmds = 0, mStale = 0, mNoProgram = 0, mScratchFail = 0, mBadRange = 0, mUnknown = 0, mTables = 0, mRefused = 0, mSafetyChecks = 0, mSafetyHits = 0, mZeroStatic = 0, mZeroRigged = 0;
    U64 mNsAll = 0, mNsTables = 0, mNsCopy = 0, mNsState = 0, mNsDraw = 0, mLast = 0;
    U64 mNsRanges = 0, mNsStamp = 0, mNsUniforms = 0, mNsPrepare = 0, mNsBindless = 0, mNsProgram = 0;      // S6 census: the parts of 'ranges' and 'state'
} sAlphaS;
static struct AlphaPass
{
    U64 mFrame = ~0ull, mPrepares = ~0ull;
    F32 mVP[32] = {};
    Renderer::ScratchAlloc mModels, mTexmats;
} sAlphaPass;

void geo_alpha_draw(const LLVkBridge::ShadowPacketsHeader& h, const U8* blob)
{
    GeoStats& s = sGeo.mS;
    AlphaStats& a = sAlphaS;
    if (!sGeo.mReady)
    {
        return;
    }
    const U64 frame_now = Context::instance().getFrameNumber();
    if (sGeo.mPreparedFrame != frame_now)
    {
        ++a.mStale;
        return;
    }
    const bool timed = geo_timed();
    const auto t_all = std::chrono::steady_clock::now();
    Renderer& r = Renderer::instance();
    AlphaPass& pass = sAlphaPass;
    auto t0 = std::chrono::steady_clock::now();
    if (h.mEntries == 0)
    {
        // the pass begins: its tables
        const F32* view = (const F32*)blob;
        const F32* proj = view + 16;
        const U32 slots = std::max(sGeo.mMaxSlot, sGeo.mMaxSlotAll) + 1;       // (every slot a fragment names: the alpha groups are not all in the G-buffer list)
        pass.mModels = r.allocVertexScratch(176 * (size_t)slots);
        pass.mTexmats = r.allocVertexScratch(64 * (size_t)slots);
        if (!pass.mModels.mPtr || !pass.mModels.mAddress || !pass.mTexmats.mPtr || !pass.mTexmats.mAddress)
        {
            pass.mModels = Renderer::ScratchAlloc();
            pass.mTexmats = Renderer::ScratchAlloc();
            ++a.mScratchFail;
            return;
        }
        F32* out = (F32*)pass.mModels.mPtr;
        F32* tex = (F32*)pass.mTexmats.mPtr;
        memset(out, 0, 176 * (size_t)slots);
        geo_matrix(view, proj, nullptr, out);               // slot 0: the view alone
        for (U32 slot = 0; slot < slots; ++slot)
        {
            F32* t = tex + 16 * (size_t)slot;
            if (slot && sGeo.mHaveM[slot])
            {
                geo_matrix(view, proj, sGeo.mRegionM[slot], out + 44 * (size_t)slot);
                memcpy(t, sGeo.mRegionM[slot], 64);         // (a texture matrix slot holds the raw matrix the viewer sent)
            }
            else
            {
                memset(t, 0, 64);
                t[0] = t[5] = t[10] = t[15] = 1.f;
            }
        }
        LLVkTexAnim::writeSlot0(tex);       // (slot 0 is never a matrix: the clock of the texture animations whose slots hold parameters)
        pass.mFrame = frame_now;
        pass.mPrepares = s.mPrepares;
        memcpy(pass.mVP, view, sizeof(pass.mVP));
        ++a.mTables;
        if (timed) a.mNsTables += geo_ns(t0);
        return;
    }
    if (pass.mFrame != frame_now || pass.mPrepares != s.mPrepares || !pass.mModels.mPtr || !pass.mTexmats.mPtr)
    {
        ++a.mStale;         // (a run without the pass's begin record of this frame)
        return;
    }
    t0 = std::chrono::steady_clock::now();
    Program* program = r.getProgram();
    if (!program || !program->isValid() || !r.bindlessAvailable())
    {
        ++a.mNoProgram;
        return;
    }
    const Program::PacketUniforms& pu = program->packetUniforms();
    if (pu.mTable == ~0u || pu.mModels == ~0u)
    {
        ++a.mNoProgram;
        return;
    }
    if (timed) a.mNsProgram += geo_ns(t0);
    struct Range { U32 mHandle, mFirst, mCount; };
    const Range* ranges = (const Range*)blob;
    const bool rigged_run = h.mPass == 1;       // S5d: the ranges are of the rigged alpha items (indexed draws)
    t0 = std::chrono::steady_clock::now();
    static thread_local std::vector<std::pair<GeoFrag*, Range>> todo;
    todo.clear();
    size_t total = 0;
    for (U32 i = 0; i < h.mEntries; ++i)
    {
        GeoFrag* f = geo_find(ranges[i].mHandle);
        if (!f)
        {
            ++a.mUnknown;
            continue;
        }
        if (f->mSafetyFrame != frame_now)
        {
            // safety at the point of use (once a fragment a frame): the buffers of its pairs, looked up by NAME now, must be the ones its rows were written with. A buffer whose storage moved or went
            // without an event reaching us would otherwise be read from its old address until the stale check of every 16th prepare: the GPU reads freed memory (device lost).
            f->mSafetyFrame = frame_now;
            ++a.mSafetyChecks;
            bool stale = false;
            for (size_t k = 0; k < f->mPairs.size(); ++k)
            {
                ResPair& p = f->mPairs[k];
                auto v = sGL.mBuffers.find(p.mVb);
                auto ib = sGL.mBuffers.find(p.mIb);
                const bool ok = v != sGL.mBuffers.end() && ib != sGL.mBuffers.end() && v->second.mVk.valid() && ib->second.mVk.valid() && v->second.mVk.mAddress && ib->second.mVk.mAddress;
                if (ok != p.mOk || (ok && (f->mAddr[2 * k] != (U64)v->second.mVk.mAddress || f->mAddr[2 * k + 1] != (U64)ib->second.mVk.mAddress)))
                {
                    p.mGen = ~0ull;         // (looked up again by the rebuild: the pointers may be stale)
                    stale = true;
                }
            }
            if (stale)
            {
                f->mDirtyPairs = true;
                f->mDirtyEntries = true;
                ++a.mSafetyHits;
            }
        }
        const bool first = f->mPinEpochSeen == ~0ull;
        if (first || f->mDirtyPairs || f->mDirtyEntries || f->mPinEpochSeen != sGeoPinEpoch)
        {
            geo_rebuild(*f, frame_now, first);
        }
        if (f->mDirtyEntries)
        {
            geo_rebuild(*f, frame_now, false);   // (a slot given back: the rows get the stand-in now, before they are read)
        }
        f->mListedFrame = frame_now;
        Range rg = ranges[i];
        const size_t avail = rigged_run ? f->mARCmds.size() : f->mACmds.size();
        if (rg.mFirst >= avail)
        {
            ++a.mBadRange;
            continue;
        }
        rg.mCount = std::min<U32>(rg.mCount, (U32)(avail - rg.mFirst));
        todo.push_back({ f, rg });
        total += rg.mCount;
    }
    if (timed) a.mNsRanges += geo_ns(t0);
    if (total == 0)
    {
        return;
    }
    t0 = std::chrono::steady_clock::now();
    Renderer::ScratchAlloc cmds = r.allocVertexScratch((rigged_run ? sizeof(VkDrawIndexedIndirectCommand) : sizeof(VkDrawIndirectCommand)) * total);
    if (!cmds.mPtr)
    {
        ++a.mScratchFail;
        return;
    }
    Renderer::ScratchAlloc alpha_rows;
    if (rigged_run)
    {
        VkDrawIndexedIndirectCommand* out = (VkDrawIndexedIndirectCommand*)cmds.mPtr;
        for (const auto& t : todo)
        {
            const VkDrawIndexedIndirectCommand* src = t.first->mARCmds.data() + t.second.mFirst;
            memcpy(out, src, t.second.mCount * sizeof(VkDrawIndexedIndirectCommand));
            for (U32 k = 0; k < t.second.mCount; ++k)
            {
                a.mZeroRigged += src[k].indexCount == 0 ? 1 : 0;       // (an item that draws nothing: no palette this frame, or its buffers are not usable; counted from the source: the scratch is write combined memory, a read of it costs about a microsecond)
            }
            out += t.second.mCount;
        }
        // as the rigged G-buffer items: the rows the draw reads are copies made for this draw (the table is read by frames in flight)
        alpha_rows = r.allocVertexScratch(sizeof(GeoGpuG) * total);
        if (alpha_rows.mPtr && alpha_rows.mAddress)
        {
            VkDrawIndexedIndirectCommand* rc = (VkDrawIndexedIndirectCommand*)cmds.mPtr;
            GeoGpuG* rows = (GeoGpuG*)alpha_rows.mPtr;
            for (size_t i = 0; i < total; ++i)
            {
                rows[i] = sGeo.mTCpu[rc[i].firstInstance];
                rc[i].firstInstance = (U32)i;
            }
        }
        else
        {
            alpha_rows = Renderer::ScratchAlloc();
            ++a.mScratchFail;
        }
    }
    else
    {
        VkDrawIndirectCommand* out = (VkDrawIndirectCommand*)cmds.mPtr;
        GeoGpuG* static_rows = nullptr;
        if (geo_rowcopy_on())
        {
            alpha_rows = r.allocVertexScratch(sizeof(GeoGpuG) * total);        // as the rigged run: rows copied for this draw (LL_VK_GEO_ROWCOPY, see the switch)
            if (alpha_rows.mPtr && alpha_rows.mAddress)
            {
                static_rows = (GeoGpuG*)alpha_rows.mPtr;
            }
            else
            {
                alpha_rows = Renderer::ScratchAlloc();
                ++a.mScratchFail;
            }
        }
        size_t at = 0;
        for (const auto& t : todo)
        {
            const VkDrawIndirectCommand* src = t.first->mACmds.data() + t.second.mFirst;
            if (static_rows)
            {
                for (U32 k = 0; k < t.second.mCount; ++k)
                {
                    VkDrawIndirectCommand c = src[k];
                    static_rows[at + k] = sGeo.mTCpu[c.firstInstance];
                    c.firstInstance = (U32)(at + k);
                    out[k] = c;
                }
            }
            else
            {
                memcpy(out, src, t.second.mCount * sizeof(VkDrawIndirectCommand));
            }
            for (U32 k = 0; k < t.second.mCount; ++k)
            {
                a.mZeroStatic += src[k].vertexCount == 0 ? 1 : 0;      // (an item that draws nothing: its buffers are not usable: missing from the picture; counted from the source, see above)
            }
            out += t.second.mCount;
            at += t.second.mCount;
        }
    }
    if (timed) a.mNsCopy += geo_ns(t0);
    t0 = std::chrono::steady_clock::now();
    {
        static U32 tags[4] = {};
        static bool made = false;
        if (!made)
        {
            made = true;
            tags[0] = Context::stampTag("scene table draw, alpha run");
            tags[1] = Context::stampTag("scene table draw, alpha run rigged");       // (the passes of the alpha pool: their own lines in the GPU time by pass)
            tags[2] = Context::stampTag("scene table draw, alpha run rigged fringe");
            tags[3] = Context::stampTag("scene table draw, alpha run depth only");
        }
        Context::instance().gpuStamp(tags[h.mFragment == 2 ? 3 : (h.mFragment == 1 ? 2 : (h.mPass == 1 ? 1 : 0))]);
    }
    Context::instance().flight(Context::FL_GEO_DRAW, 99ull, (unsigned long long)total, (unsigned long long)pass.mModels.mAddress, (unsigned long long)(sGeo.mMaxSlot + 1));
    if (timed) { a.mNsStamp += geo_ns(t0); t0 = std::chrono::steady_clock::now(); }
    static thread_local VertexInput vi_cache;
    static thread_local std::vector<U32> used_cache;
    VertexInput& vi = vi_cache;
    vi.mBindings.clear();
    vi.mAttributes.clear();
    used_cache.clear();
    const U64 table_address = alpha_rows.mAddress ? (U64)alpha_rows.mAddress : (U64)sGeo.mTable.mAddress, models_address = (U64)pass.mModels.mAddress, texmats_address = (U64)pass.mTexmats.mAddress;
    Context::instance().flight(Context::FL_GEO_DRAW, 98ull, table_address, (unsigned long long)(alpha_rows.mAddress ? 1 : 0), (unsigned long long)total);       // (the alpha run: which table, 1 = rows copied for this draw)
    const S32 table_words[2] = { (S32)(U32)table_address, (S32)(U32)(table_address >> 32) };
    const S32 models_words[2] = { (S32)(U32)models_address, (S32)(U32)(models_address >> 32) };
    const S32 texmats_words[2] = { (S32)(U32)texmats_address, (S32)(U32)(texmats_address >> 32) };
    program->setFloatsAt(pu.mTable, reinterpret_cast<const F32*>(table_words), 2, 1);
    program->setFloatsAt(pu.mModels, reinterpret_cast<const F32*>(models_words), 2, 1);
    if (pu.mTexmats != ~0u)
    {
        program->setFloatsAt(pu.mTexmats, reinterpret_cast<const F32*>(texmats_words), 2, 1);
    }
    if (pu.mPbr != ~0u && sGeo.mPbrT)
    {
        const U64 pbr_address = (U64)sGeo.mPbrTable.mAddress;
        const S32 pbr_words[2] = { (S32)(U32)pbr_address, (S32)(U32)(pbr_address >> 32) };
        program->setFloatsAt(pu.mPbr, reinterpret_cast<const F32*>(pbr_words), 2, 1);
    }
    if (timed) { a.mNsUniforms += geo_ns(t0); t0 = std::chrono::steady_clock::now(); }
    if (!prepare_draw(vi, used_cache))
    {
        return;
    }
    geo_flush_if_needed();
    r.setVertexInput(vi);
    r.setTopology(topology(LLVkFormat::TOP_TRIANGLES));
    if (timed) { a.mNsPrepare += geo_ns(t0); t0 = std::chrono::steady_clock::now(); }
    r.bindlessFlush();
    if (timed) { a.mNsBindless += geo_ns(t0); }
    if (timed) a.mNsState += geo_ns(t0);
    t0 = std::chrono::steady_clock::now();
    if (rigged_run)
    {
        // runs of one buffer pair: the index buffer bound, one indirect indexed draw
        size_t cursor = 0;
        U32 drawn = 0;
        for (const auto& t : todo)
        {
            GeoFrag* f = t.first;
            U32 k = 0;
            while (k < t.second.mCount)
            {
                const U32 pair = f->mPairOf[f->mARItem[t.second.mFirst + k]];
                U32 e = k + 1;
                while (e < t.second.mCount && f->mPairOf[f->mARItem[t.second.mFirst + e]] == pair)
                {
                    ++e;
                }
                Buf* ib = packet_buffer(f->mPairs[pair].mIb);
                if (ib && ib->mVk.valid())
                {
                    r.setIndexBuffer(ib->mVk.mBuffer, ib->mArena.mOffset, VK_INDEX_TYPE_UINT16);
                    if (r.drawPacketsIndexed(nullptr, e - k, cmds.mBuffer, cmds.mOffset + (VkDeviceSize)(cursor + k) * sizeof(VkDrawIndexedIndirectCommand)))
                    {
                        drawn += e - k;
                    }
                    else
                    {
                        ++a.mRefused;
                        ++sStats.mRendererRefused;
                    }
                }
                k = e;
            }
            cursor += t.second.mCount;
        }
        if (drawn && sStatsEnabled)
        {
            sStats.mDone += drawn;
        }
        sDraws += drawn;
        sDrawsTotal += drawn;
    }
    else
    {
    if (r.drawPackets(nullptr, (U32)total, cmds.mBuffer, cmds.mOffset))
    {
        if (sStatsEnabled)
        {
            sStats.mDone += (U32)total;
        }
    }
    else
    {
        ++a.mRefused;
        ++sStats.mRendererRefused;
    }
    sDraws += (U32)total;
    sDrawsTotal += (U32)total;
    }
    if (timed) a.mNsDraw += geo_ns(t0);
    ++a.mRecords;
    a.mRanges += h.mEntries;
    a.mCmds += total;
    if (timed) a.mNsAll += geo_ns(t_all);
    if (a.mRecords - a.mLast >= 6000)
    {
        a.mLast = a.mRecords;
        const double n = (double)a.mRecords;
        std::string tail;
        if (timed)
        {
            tail = "; us per record: all " + std::to_string((double)a.mNsAll / 1000.0 / n) + " = tables " + std::to_string((double)a.mNsTables / 1000.0 / n) + " + ranges and copy " + std::to_string((double)a.mNsCopy / 1000.0 / n)
                + " + state " + std::to_string((double)a.mNsState / 1000.0 / n) + " + draw call " + std::to_string((double)a.mNsDraw / 1000.0 / n)
                + " (program check " + std::to_string((double)a.mNsProgram / 1000.0 / n) + ", ranges loop " + std::to_string((double)a.mNsRanges / 1000.0 / n) + ", stamp+flight " + std::to_string((double)a.mNsStamp / 1000.0 / n)
                + ", uniforms " + std::to_string((double)a.mNsUniforms / 1000.0 / n) + ", prepare_draw+vertex input " + std::to_string((double)a.mNsPrepare / 1000.0 / n) + ", bindless flush " + std::to_string((double)a.mNsBindless / 1000.0 / n) + ")";
        }
        LL_INFOS("VkScene") << "alpha runs (S5a), worker, over " << a.mRecords << " records: " << (double)a.mRanges / n << " ranges and " << (double)a.mCmds / n << " draws a record; pass tables made " << a.mTables
            << " times; stale (no prepare this frame) " << a.mStale << ", no program " << a.mNoProgram << ", scratch failures " << a.mScratchFail << ", unknown handles " << a.mUnknown << ", bad ranges " << a.mBadRange
            << ", refused " << a.mRefused << "; safety: " << a.mSafetyChecks << " fragment checks, " << a.mSafetyHits << " found a buffer moved or gone; items sent that draw nothing: " << a.mZeroStatic << " static (buffers not usable), " << a.mZeroRigged << " rigged (no palette or buffers not usable)" << tail << LL_ENDL;
    }
}

// kind 33 (S6 LV1): the light volumes of a deferred lighting pass. The program is the pull light program the viewer bound; each light is an 8 vertex triangle fan drawn as one indirect command whose instance is the
// light's row.
void on_light_volumes(const LLVkBridge::ShadowPacketsHeader& h, const U8* blob)
{
    Renderer& r = Renderer::instance();
    Program* program = r.getProgram();
    if (!h.mEntries || !program || !program->isValid())
    {
        return;
    }
    const Program::PacketUniforms& pu = program->packetUniforms();
    if (pu.mLights == ~0u)
    {
        return;
    }
    const bool spot = h.mPass == 1;
    const size_t row_bytes = spot ? sizeof(LLVkBridge::SpotVolumeRow) : sizeof(LLVkBridge::LightVolumeRow);
    Renderer::ScratchAlloc rows = r.allocVertexScratch(row_bytes * h.mEntries);
    Renderer::ScratchAlloc cmds = r.allocVertexScratch(sizeof(VkDrawIndirectCommand) * h.mEntries);
    if (!rows.mPtr || !rows.mAddress || !cmds.mPtr)
    {
        return;
    }
    memcpy(rows.mPtr, blob, row_bytes * h.mEntries);
    if (spot)
    {   // the projected image: the GL name in the row becomes this frame's bindless slot
        LLVkBridge::SpotVolumeRow* sr = (LLVkBridge::SpotVolumeRow*)rows.mPtr;
        const LLVkBridge::SpotVolumeRow* src = (const LLVkBridge::SpotVolumeRow*)blob;
        for (U32 i = 0; i < h.mEntries; ++i)
        {
            sr[i].mTexture = packet_texture_slot(r, src[i].mTexture);
        }
    }
    VkDrawIndirectCommand* c = (VkDrawIndirectCommand*)cmds.mPtr;
    for (U32 i = 0; i < h.mEntries; ++i)
    {
        c[i] = { 8u, 1u, 0u, i };
    }
    static thread_local VertexInput vi;
    static thread_local std::vector<U32> used;
    vi.mBindings.clear();
    vi.mAttributes.clear();
    used.clear();
    const U64 address = (U64)rows.mAddress;
    const S32 words[2] = { (S32)(U32)address, (S32)(U32)(address >> 32) };
    program->setFloatsAt(pu.mLights, reinterpret_cast<const F32*>(words), 2, 1);
    if (!prepare_draw(vi, used))
    {
        return;
    }
    r.setVertexInput(vi);
    r.setTopology(topology(LLVkFormat::TOP_TRIANGLE_FAN));
    if (spot)
    {
        r.bindlessFlush();
    }
    if (r.drawPackets(nullptr, h.mEntries, cmds.mBuffer, cmds.mOffset))
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

// kind 31 (S5d): the palettes of this frame for the rigged alpha items. Blob: mEntries x { fragment handle, item (its number in the fragment's wire list), the palette's offset in the frame's shared list
// (~0u: none), joints }. The row of the item gets the palette address and joint count, its command the index count: an item without a binding this frame draws nothing (as the rigged G-buffer items).
void geo_alpha_palettes(const LLVkBridge::ShadowPacketsHeader& h, const U8* blob)
{
    if (!sGeo.mReady || sGeo.mPreparedFrame != Context::instance().getFrameNumber())
    {
        return;
    }
    struct Binding { U32 mHandle, mItem, mOffset, mJoints; };
    const Binding* bindings = (const Binding*)blob;
    for (U32 i = 0; i < h.mEntries; ++i)
    {
        const Binding& b = bindings[i];
        GeoFrag* f = geo_find(b.mHandle);
        if (!f || b.mItem >= f->mItems.size() || b.mItem >= f->mARItemCmd.size() || f->mARItemCmd[b.mItem] == ~0u)
        {
            continue;
        }
        const U32 cmd = f->mARItemCmd[b.mItem];
        const U64 pal = b.mOffset != ~0u ? palette_address(b.mOffset) : 0;
        GeoGpuG& row = sGeo.mT[f->mSlots[b.mItem]];
        if (pal)
        {
            f->mPalAddr[b.mItem] = pal;
            f->mPalJoints[b.mItem] = b.mJoints;
            row.mPalette[0] = (U32)pal;
            row.mPalette[1] = (U32)(pal >> 32);
            row.mJoints = b.mJoints;
            GeoGpuG& cpu_row = sGeo.mTCpu[f->mSlots[b.mItem]];
            cpu_row.mPalette[0] = (U32)pal; cpu_row.mPalette[1] = (U32)(pal >> 32); cpu_row.mJoints = b.mJoints;
            f->mARCmds[cmd].indexCount = f->mARCount0[cmd];
        }
        else
        {
            f->mARCmds[cmd].indexCount = 0;
        }
    }
}

void geo_dispatch(const LLVkBridge::ShadowPacketsHeader& h, const U8* blob)
{
    if (h.mKind == 31)
    {
        geo_alpha_palettes(h, blob);
    }
    else if (h.mKind == 29)
    {
        geo_alpha_draw(h, blob);
    }
    else if (h.mKind == 20)
    {
        geo_retain(h, blob);
    }
    else if (h.mKind == 21)
    {
        geo_prepare(h, blob);
    }
    else if (h.mKind == 22)
    {
        geo_draw(h, blob);
    }
    else if (h.mKind == 24)
    {
        geo_tex_update(h, blob);
    }
    else
    {
        const auto t0 = std::chrono::steady_clock::now();
        const U32* ids = (const U32*)blob;
        for (U32 i = 0; i < h.mEntries; ++i)
        {
            ++sGeo.mS.mReleases;
            sGeo.mS.mReleaseUnknown += geo_find(ids[i]) ? 0 : 1;
            geo_release(ids[i]);
        }
        sGeo.mS.mNsRelease += geo_ns(t0);
    }
}
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// <FS> S7 H1 (doc/vulkan/S7-design.md section 4): Hi-Z occlusion. Kind 34 carries the boxes of the occlusion groups (HizBox, 32 bytes each) and the view-projection of the pass; the worker builds a max-depth
// pyramid of the depth target that is bound at that point of the frame (first level = half resolution; sizes are the floor of half like Vulkan's mip chain, texel p of a level covers texels 2p and 2p+1 of the one below and the last texel also the odd extra row / column), and tests every box
// with one thread: the nearest depth of its 8 corners against the largest depth of the pyramid texels its screen rectangle covers (a level where the rectangle spans at most 2 texels). One word a box
// goes to a slot of a ring of host-visible cached memory: bit 0 = occluded, bits 8-15 = the pyramid level used, bit 16 = cut by the near plane (visible), bit 17 = rectangle off the screen
// (occluded: no sample of it is drawn), bit 18 = beyond the far plane (visible), bit 19 = wholly behind the camera (occluded). The viewer reads the slot when the frame's fence has signalled (hiz_poll publishes the serial); nothing ever blocks.
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
namespace
{
const char* kHizDownShader = R"GLSL(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D uSrc;
layout(set = 0, binding = 1, r32f) uniform writeonly image2D uDst;
layout(push_constant) uniform PC { ivec2 srcSize; ivec2 dstSize; int srcLod; } pc;
void main()
{
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= pc.dstSize.x || p.y >= pc.dstSize.y) return;
    ivec2 b = p * 2;
    ivec2 m = pc.srcSize - 1;
    // texel p covers source texels 2p and 2p+1; the last one also takes the odd extra source row / column (the level sizes are the floor of half, as Vulkan's mip chain)
    int nx = (p.x == pc.dstSize.x - 1 && (pc.srcSize.x & 1) == 1 && pc.srcSize.x > 1) ? 3 : 2;
    int ny = (p.y == pc.dstSize.y - 1 && (pc.srcSize.y & 1) == 1 && pc.srcSize.y > 1) ? 3 : 2;
    float d = 0.0;
    for (int y = 0; y < ny; ++y)
        for (int x = 0; x < nx; ++x)
            d = max(d, texelFetch(uSrc, min(b + ivec2(x, y), m), pc.srcLod).r);
    imageStore(uDst, p, vec4(d));
}
)GLSL";
const char* kHizTestShader = R"GLSL(#version 450
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
layout(local_size_x = 64) in;
layout(set = 0, binding = 0) uniform sampler2D uPyr;
struct Box { vec4 c; vec4 h; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Boxes { Box b[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) writeonly buffer Out { uint o[]; };
layout(push_constant) uniform PC { mat4 vp; uvec2 boxes; uvec2 outp; uint count; uint levels; vec2 depthSize; uint fine; uint mode; uint first; uint pad2; } pc;
void main()
{
    uint i = gl_GlobalInvocationID.x + pc.first;
    if (i >= pc.count) return;
    if (pc.mode == 2u) { Out(pc.outp).o[3u * i] = 0u; return; }        // (S7 B0 census: no previous pyramid to test against)
    Box bx = Boxes(pc.boxes).b[i];
    bool water = (floatBitsToUint(bx.c.w) & 1u) != 0u;
    float minz = 1e9;
    vec2 mn = vec2(1e9), mx = vec2(-1e9);
    uint behind = 0u;
    bool near_cut = false;
    for (int k = 0; k < 8; ++k)
    {
        vec3 s = vec3((k & 1) != 0 ? 1.0 : -1.0, (k & 2) != 0 ? 1.0 : -1.0, (k & 4) != 0 ? 1.0 : -1.0);
        vec4 cl = pc.vp * vec4(bx.c.xyz + s * bx.h.xyz, 1.0);
        float z = (cl.z + cl.w) * 0.5;
        if (cl.w <= 1e-4) { ++behind; near_cut = true; continue; }
        if (!water && z < 0.0) near_cut = true;
        vec2 uv = cl.xy / cl.w * 0.5 + 0.5;
        mn = min(mn, uv);
        mx = max(mx, uv);
        minz = min(minz, clamp(z / cl.w, 0.0, 1.0));
    }
    uint result = 0u;
    Out(pc.outp).o[3u * i + 1u] = 0u;
    Out(pc.outp).o[3u * i + 2u] = 0u;
    if (behind == 8u) result = 1u | (1u << 19);         // wholly behind the camera: nothing of it can be drawn (what the query counted as no samples)
    else if (near_cut) result = 1u << 16;               // cut by the near plane: some of it is in front of everything, visible
    else
    {
        if (mx.x < 0.0 || mn.x > 1.0 || mx.y < 0.0 || mn.y > 1.0) result = 1u | (1u << 17);      // outside the view: no samples either
        else if (!water && minz >= 1.0) result = 1u << 18;
        else
        {
            mn = clamp(mn, vec2(0.0), vec2(1.0));
            mx = clamp(mx, vec2(0.0), vec2(1.0));
            vec2 pmn = mn * pc.depthSize, pmx = mx * pc.depthSize;
            vec2 ext = max(pmx - pmn, vec2(1.0));
            float s = max(ext.x, ext.y);
            int lod = clamp(int(ceil(log2(s))) - int(pc.fine), 0, int(pc.levels) - 1);
            float texel = exp2(float(lod + 1));
            ivec2 sz = textureSize(uPyr, lod);
            ivec2 t0 = min(ivec2(floor(pmn / texel)), sz - 1);
            ivec2 t1 = min(ivec2(floor(pmx / texel)), sz - 1);
            t1 = min(t1, t0 + 11);
            float maxd = 0.0;
            for (int y = t0.y; y <= t1.y; ++y)
                for (int x = t0.x; x <= t1.x; ++x)
                    maxd = max(maxd, texelFetch(uPyr, ivec2(x, y), lod).r);
            result = (minz > maxd ? 1u : 0u) | (uint(lod) << 8);
            Out(pc.outp).o[3u * i + 1u] = floatBitsToUint(minz);
            Out(pc.outp).o[3u * i + 2u] = floatBitsToUint(maxd);
        }
    }
    Out(pc.outp).o[3u * i] = result | (pc.mode == 1u ? 0x80000000u : 0u);       // mode 1 (S7 B0 census, the previous frame's pyramid): bit 31 = a valid verdict
}
)GLSL";

constexpr U32 kHizSlots = 8;                // ring slots (the viewer never has more than kHizSlots - 1 serials in flight)
constexpr U32 kHizCapacity = 32768;         // boxes a slot holds (S7 B0: the census sends the boxes of the table items as well; the second half of the results buffer holds their verdicts against the previous frame's pyramid)

struct HizState
{
    bool mTried = false, mReady = false;
    VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout mDownLayout = VK_NULL_HANDLE, mTestLayout = VK_NULL_HANDLE;
    VkPipeline mDown = VK_NULL_HANDLE, mTest = VK_NULL_HANDLE;
    VkSampler mSampler = VK_NULL_HANDLE;
    Buffer mResults;                        // kHizSlots x kHizCapacity words, cached host memory
    Image mPyramid;                         // R32_SFLOAT, mips, GENERAL for good
    std::vector<VkImageView> mMipViews;
    bool mPyramidInit = false;
    U32 mDepthW = 0, mDepthH = 0;
    struct Pending { U32 mSerial, mSlot; U64 mFrame; U32 mCount; bool mCensus; };
    std::deque<Pending> mPending;
    // counters (reported every 600 records)
    U64 mRecords = 0, mBoxes = 0, mNoDepth = 0, mNoPipeline = 0, mPyramidBuilds = 0, mTooMany = 0, mPublished = 0, mDispatches = 0, mLast = 0;
};
HizState sHiz;

VkShaderModule hiz_compile(const char* source, const char* what)
{
    static const bool glslang_ready = glslang::InitializeProcess();
    if (!glslang_ready)
    {
        return VK_NULL_HANDLE;
    }
    glslang::TShader shader(EShLangCompute);
    const char* strings[1] = { source };
    shader.setStrings(strings, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, EShLangCompute, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);
    const EShMessages msgs = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader.parse(GetDefaultResources(), 450, ECoreProfile, false, false, msgs))
    {
        LL_WARNS("VkHiz") << what << ": compile failed: " << shader.getInfoLog() << LL_ENDL;
        return VK_NULL_HANDLE;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(msgs))
    {
        LL_WARNS("VkHiz") << what << ": link failed: " << program.getInfoLog() << LL_ENDL;
        return VK_NULL_HANDLE;
    }
    std::vector<unsigned int> spirv;
    spv::SpvBuildLogger logger;
    glslang::SpvOptions options;
    options.validate = false;
    glslang::GlslangToSpv(*program.getIntermediate(EShLangCompute), spirv, &logger, &options);
    if (spirv.empty())
    {
        LL_WARNS("VkHiz") << what << ": no SPIR-V: " << logger.getAllMessages() << LL_ENDL;
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo mi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    mi.codeSize = spirv.size() * sizeof(unsigned int);
    mi.pCode = spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(Context::instance().getDevice(), &mi, nullptr, &module) != VK_SUCCESS)
    {
        return VK_NULL_HANDLE;
    }
    return module;
}

bool hiz_init()
{
    HizState& s = sHiz;
    if (s.mReady)
    {
        return true;
    }
    if (s.mTried)
    {
        return false;
    }
    s.mTried = true;
    Context& ctx = Context::instance();
    VkDevice dev = ctx.getDevice();
    VkShaderModule down = hiz_compile(kHizDownShader, "hiz down");
    VkShaderModule test = hiz_compile(kHizTestShader, "hiz test");
    if (down == VK_NULL_HANDLE || test == VK_NULL_HANDLE)
    {
        return false;
    }
    VkDescriptorSetLayoutBinding b[2] = {};
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[0].descriptorCount = 1;
    b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[1].binding = 1;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[1].descriptorCount = 1;
    b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo sl = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    sl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    sl.bindingCount = 2;
    sl.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev, &sl, nullptr, &s.mSetLayout) != VK_SUCCESS)
    {
        return false;
    }
    VkPushConstantRange down_pc = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 20 };
    VkPushConstantRange test_pc = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 112 };
    VkPipelineLayoutCreateInfo li = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    li.setLayoutCount = 1;
    li.pSetLayouts = &s.mSetLayout;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &down_pc;
    if (vkCreatePipelineLayout(dev, &li, nullptr, &s.mDownLayout) != VK_SUCCESS)
    {
        return false;
    }
    li.pPushConstantRanges = &test_pc;
    if (vkCreatePipelineLayout(dev, &li, nullptr, &s.mTestLayout) != VK_SUCCESS)
    {
        return false;
    }
    VkComputePipelineCreateInfo pi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    pi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pi.stage.pName = "main";
    pi.stage.module = down;
    pi.layout = s.mDownLayout;
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pi, nullptr, &s.mDown) != VK_SUCCESS)
    {
        return false;
    }
    pi.stage.module = test;
    pi.layout = s.mTestLayout;
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pi, nullptr, &s.mTest) != VK_SUCCESS)
    {
        return false;
    }
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 16.f;
    if (vkCreateSampler(dev, &sci, nullptr, &s.mSampler) != VK_SUCCESS)
    {
        return false;
    }
    if (!ctx.createBuffer((VkDeviceSize)kHizSlots * kHizCapacity * 12 * 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Context::Memory::Readback, s.mResults) || !s.mResults.mMapped || !s.mResults.mAddress)
    {
        return false;
    }
    memset(s.mResults.mMapped, 0, (size_t)kHizSlots * kHizCapacity * 12 * 2);
    LLVkBridge::hizSetResults((const U32*)s.mResults.mMapped, kHizCapacity);
    s.mReady = true;
    LL_INFOS("VkHiz") << "Hi-Z occlusion ready: " << kHizSlots << " result slots of " << kHizCapacity << " boxes" << LL_ENDL;
    return true;
}

void hiz_poll()
{
    HizState& s = sHiz;
    if (!s.mReady)
    {
        return;
    }
    Context& ctx = Context::instance();
    while (!s.mPending.empty() && ctx.frameFinished(s.mPending.front().mFrame))
    {
        const HizState::Pending p = s.mPending.front();
        s.mPending.pop_front();
        vmaInvalidateAllocation(ctx.getAllocator(), s.mResults.mAllocation, (VkDeviceSize)p.mSlot * kHizCapacity * 12, (VkDeviceSize)p.mCount * 12);
        if (p.mCensus)
        {
            vmaInvalidateAllocation(ctx.getAllocator(), s.mResults.mAllocation, ((VkDeviceSize)kHizSlots + p.mSlot) * kHizCapacity * 12, (VkDeviceSize)p.mCount * 12);       // (S7 B0: the census half)
        }
        LLVkBridge::hizPublish(p.mSlot, p.mSerial);
        ++s.mPublished;
    }
}

// the pyramid for a depth target of this size; false when it cannot be made
bool hiz_pyramid(U32 w, U32 h)
{
    HizState& s = sHiz;
    if (s.mPyramid.valid() && s.mDepthW == w && s.mDepthH == h)
    {
        return true;
    }
    Context& ctx = Context::instance();
    if (s.mPyramid.valid())
    {
        Image old = s.mPyramid;
        std::vector<VkImageView> views = s.mMipViews;
        s.mPyramid = Image();
        s.mMipViews.clear();
        ctx.destroyLater([old, views]() mutable {
            Context& c = Context::instance();
            for (VkImageView v : views) vkDestroyImageView(c.getDevice(), v, nullptr);
            c.destroyImage(old);
        });
    }
    const U32 w0 = std::max(1u, w / 2), h0 = std::max(1u, h / 2);
    U32 mips = 1;
    for (U32 m = std::max(w0, h0); m > 1; m /= 2) ++mips;
    if (!ctx.createImage2D(w0, h0, mips, 1, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, s.mPyramid))
    {
        s.mPyramid = Image();
        return false;
    }
    s.mMipViews.clear();
    for (U32 m = 0; m < mips; ++m)
    {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = s.mPyramid.mImage;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R32_SFLOAT;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, m, 1, 0, 1 };
        VkImageView v = VK_NULL_HANDLE;
        if (vkCreateImageView(ctx.getDevice(), &vi, nullptr, &v) != VK_SUCCESS)
        {
            return false;
        }
        s.mMipViews.push_back(v);
    }
    s.mDepthW = w;
    s.mDepthH = h;
    s.mPyramidInit = false;
    ++s.mPyramidBuilds;
    LL_INFOS("VkHiz") << "Hi-Z pyramid for a " << w << "x" << h << " depth target: " << w0 << "x" << h0 << ", " << mips << " levels" << LL_ENDL;
    return true;
}

// kind 34
void on_hiz_boxes(const LLVkBridge::ShadowPacketsHeader& h, const U8* blob)
{
    HizState& s = sHiz;
    ++s.mRecords;
    s.mBoxes += h.mEntries;
    Renderer& r = Renderer::instance();
    Context& ctx = Context::instance();
    hiz_poll();
    Image* depth = r.currentDepthTarget();
    if (!hiz_init() || !h.mEntries)
    {
        ++s.mNoPipeline;
        return;
    }
    if (h.mEntries > kHizCapacity)
    {
        ++s.mTooMany;
        return;
    }
    if (!depth || !depth->valid() || !(depth->mAspect & VK_IMAGE_ASPECT_DEPTH_BIT))
    {
        ++s.mNoDepth;
        return;
    }
    Image* root = depth->mOwner ? depth->mOwner : depth;
    const U32 w = depth->mExtent.width, hgt = depth->mExtent.height;
    if (!hiz_pyramid(w, hgt))
    {
        ++s.mNoPipeline;
        return;
    }
    Renderer::ScratchAlloc boxes = r.allocVertexScratch(32ull * h.mEntries);
    if (!boxes.mPtr || !boxes.mAddress)
    {
        ++s.mNoPipeline;
        return;
    }
    memcpy(boxes.mPtr, blob, 32ull * h.mEntries);
    const U32 slot = h.mPass % kHizSlots;
    const U64 out_address = (U64)s.mResults.mAddress + (U64)slot * kHizCapacity * 12;
    const U32 levels = s.mPyramid.mMipLevels;
    struct TestPc { F32 mVp[16]; U32 mBoxes[2]; U32 mOut[2]; U32 mCount; U32 mLevels; F32 mDepth[2]; U32 mFine; U32 mPad[3]; } tpc;
    static_assert(sizeof(TestPc) == 112, "hiz test push constants");
    memcpy(tpc.mVp, h.mViewProj, sizeof(tpc.mVp));
    tpc.mBoxes[0] = (U32)boxes.mAddress; tpc.mBoxes[1] = (U32)((U64)boxes.mAddress >> 32);
    tpc.mOut[0] = (U32)out_address; tpc.mOut[1] = (U32)(out_address >> 32);
    tpc.mCount = h.mEntries;
    tpc.mLevels = levels;
    tpc.mDepth[0] = (F32)w; tpc.mDepth[1] = (F32)hgt;
    static const U32 fine = []() { const char* e = getenv("LL_VK_HIZ_FINE"); return e ? (U32)atoi(e) : 5u; }();      // how many pyramid levels finer than "the rectangle spans two texels" the test reads (5: the rectangle spans up to about 9 x 9 texels of a level 4 finer; 3 lost 23 % of the queries' savings, 5 lost 15 %)
    tpc.mFine = fine;
    tpc.mPad[0] = tpc.mPad[1] = tpc.mPad[2] = 0;       // mode 0, first 0
    // S7 B0 (LL_VK_VISCENSUS): the boxes from h.mPaletteFloats on are table items; they are tested a second time, before the pyramid is rebuilt, against the previous frame's pyramid with the previous view-projection (the 64 bytes after the boxes)
    const bool census_prev = h.mMatrices == 1 && h.mPaletteFloats < h.mEntries;
    TestPc cpc = tpc;
    if (census_prev)
    {
        memcpy(cpc.mVp, blob + 32ull * h.mEntries, sizeof(cpc.mVp));
        const U64 half_address = out_address + (U64)kHizSlots * kHizCapacity * 12;
        cpc.mOut[0] = (U32)half_address; cpc.mOut[1] = (U32)(half_address >> 32);
        cpc.mPad[0] = s.mPyramidInit ? 1u : 2u;
        cpc.mPad[1] = h.mPaletteFloats;
    }
    const bool init = !s.mPyramidInit;
    VkImage pyramid_image = s.mPyramid.mImage;
    VkImageView pyramid_view = s.mPyramid.mView;
    const std::vector<VkImageView> mip_views = s.mMipViews;
    VkImageView depth_view = depth->mView;
    const VkSampler sampler = s.mSampler;
    const VkPipeline down = s.mDown, test = s.mTest;
    const VkPipelineLayout down_layout = s.mDownLayout, test_layout = s.mTestLayout;
    U64 dispatches = 0;
    const bool recorded = r.recordExternal({ { root, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL } }, [&](VkCommandBuffer cmd) {
        if (LLVkBridge::gPhaseTiming)
        {
            ctx.gpuStamp(Context::stampTag("hiz pyramid + box test"));
        }
        auto barrier = [&](VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
            VkMemoryBarrier2 mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
            mb.srcStageMask = src_stage; mb.srcAccessMask = src_access; mb.dstStageMask = dst_stage; mb.dstAccessMask = dst_access;
            VkDependencyInfo di = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            di.memoryBarrierCount = 1;
            di.pMemoryBarriers = &mb;
            vkCmdPipelineBarrier2(cmd, &di);
        };
        if (init)
        {
            VkImageMemoryBarrier2 ib = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            ib.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            ib.srcAccessMask = 0;
            ib.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            ib.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
            ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ib.image = pyramid_image;
            ib.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1 };
            VkDependencyInfo di = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            di.imageMemoryBarrierCount = 1;
            di.pImageMemoryBarriers = &ib;
            vkCmdPipelineBarrier2(cmd, &di);
        }
        // the depth writes of the passes before, the pyramid and results of the frames before
        barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        if (census_prev)
        {
            // S7 B0: the table items against the pyramid of the frame before (it is rebuilt below)
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, test);
            VkDescriptorImageInfo prev_info = { sampler, pyramid_view, VK_IMAGE_LAYOUT_GENERAL };
            VkWriteDescriptorSet pwr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            pwr.dstBinding = 0; pwr.descriptorCount = 1; pwr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; pwr.pImageInfo = &prev_info;
            vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, test_layout, 0, 1, &pwr);
            vkCmdPushConstants(cmd, test_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(cpc), &cpc);
            vkCmdDispatch(cmd, (cpc.mCount - cpc.mPad[1] + 63) / 64, 1, 1);
            ++dispatches;
            barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, down);
        U32 src_w = w, src_h = hgt;
        for (U32 m = 0; m < levels; ++m)
        {
            const U32 dst_w = std::max(1u, src_w / 2), dst_h = std::max(1u, src_h / 2);
            VkDescriptorImageInfo src_info = { sampler, m == 0 ? depth_view : pyramid_view, m == 0 ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL };
            VkDescriptorImageInfo dst_info = { VK_NULL_HANDLE, mip_views[m], VK_IMAGE_LAYOUT_GENERAL };
            VkWriteDescriptorSet wr[2] = {};
            wr[0].sType = wr[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr[0].dstBinding = 0; wr[0].descriptorCount = 1; wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[0].pImageInfo = &src_info;
            wr[1].dstBinding = 1; wr[1].descriptorCount = 1; wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; wr[1].pImageInfo = &dst_info;
            vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, down_layout, 0, 2, wr);
            const S32 dpc[5] = { (S32)src_w, (S32)src_h, (S32)dst_w, (S32)dst_h, m == 0 ? 0 : (S32)(m - 1) };
            vkCmdPushConstants(cmd, down_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(dpc), dpc);
            vkCmdDispatch(cmd, (dst_w + 7) / 8, (dst_h + 7) / 8, 1);
            ++dispatches;
            barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
            src_w = dst_w;
            src_h = dst_h;
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, test);
        VkDescriptorImageInfo pyr_info = { sampler, pyramid_view, VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        wr.dstBinding = 0; wr.descriptorCount = 1; wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = &pyr_info;
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, test_layout, 0, 1, &wr);
        vkCmdPushConstants(cmd, test_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(tpc), &tpc);
        vkCmdDispatch(cmd, (tpc.mCount + 63) / 64, 1, 1);
        ++dispatches;
        // the results are read by the host after the fence; the depth is written again by the passes after this (a write after our reads)
        barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    });
    if (!recorded)
    {
        ++s.mNoPipeline;
        return;
    }
    s.mPyramidInit = true;
    s.mDispatches += dispatches;
    s.mPending.push_back({ h.mFragment, slot, ctx.getFrameNumber(), h.mEntries, census_prev });
    if (s.mRecords - s.mLast >= 600)
    {
        s.mLast = s.mRecords;
        const double n = (double)s.mRecords;
        LL_INFOS("VkHiz") << "Hi-Z worker over " << s.mRecords << " records: " << (double)s.mBoxes / n << " boxes a record, " << (double)s.mDispatches / n << " dispatches a record, depth " << w << "x" << hgt << " (pyramid "
            << s.mPyramid.mExtent.width << "x" << s.mPyramid.mExtent.height << ", " << levels << " levels, " << s.mPyramidBuilds << " builds), published " << s.mPublished << ", still pending " << s.mPending.size()
            << "; refused: no depth target " << s.mNoDepth << ", no pipeline/other " << s.mNoPipeline << ", too many boxes " << s.mTooMany << LL_ENDL;
    }
}
}

// ---- <FS> S7 C1 (doc/vulkan/S7-design.md section 14), kind 35: the flat group table against the camera frustum, shadow mode: nothing reads the bits but the viewer's comparison log ----
const char* kCullTestShader = R"GLSL(#version 450
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
layout(local_size_x = 32) in;
struct Row { vec4 c; vec4 h; vec4 emin; vec4 emax; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Rows { Row r[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Params { vec4 v[12]; };
layout(buffer_reference, std430, buffer_reference_align = 4) writeonly buffer Out { uint o[]; };
layout(push_constant) uniform PC { uvec2 rows; uvec2 params; uvec2 outp; uint count; uint words; } pc;
shared uint sBits[4];
bool inside(Row r, uint flags, float m)
{
    Params P = Params(pc.params);
    uint planes = floatBitsToUint(P.v[10].x);
    vec3 c = r.c.xyz;
    vec3 hh = r.h.xyz + vec3(m);
    for (uint p = 0u; p < planes; ++p)
    {
        if (p == 5u) continue;                          // the far plane: the CPU's NoFarClip test leaves it out, the sphere test is the far limit
        vec4 pl = P.v[p];
        if (dot(pl.xyz, c) + pl.w - dot(abs(pl.xyz), hh) > 0.0) return false;
    }
    if ((flags & 8u) != 0u)
    {
        vec3 origin = P.v[7].xyz;
        float rad = P.v[7].w;
        vec3 dd = max(max(r.emin.xyz - vec3(m) - origin, origin - (r.emax.xyz + vec3(m))), vec3(0.0));
        if (dot(dd, dd) > rad * rad) return false;
    }
    return true;
}
void main()
{
    uint i = gl_GlobalInvocationID.x;
    uint lid = gl_LocalInvocationID.x;
    if (lid < 4u) sBits[lid] = 0u;
    barrier();
    if (i < pc.count)
    {
        Rows rows = Rows(pc.rows);
        Params P = Params(pc.params);
        Row r = rows.r[i];
        uint flags = floatBitsToUint(r.h.w);
        uint parent = floatBitsToUint(r.c.w);
        bool occ = (flags & 5u) == 4u;                  // occluded and not the root
        if ((flags & 1u) == 0u)
        {
            uint a = parent;
            for (int d = 0; d < 32 && !occ; ++d)
            {
                uint f = floatBitsToUint(rows.r[a].h.w);
                if ((f & 1u) != 0u) break;              // the root is never occluded
                if ((f & 4u) != 0u) occ = true;
                a = floatBitsToUint(rows.r[a].c.w);
            }
        }
        float dist = length(r.c.xyz - P.v[7].xyz);
        float margin = P.v[8].x + dist * P.v[8].y;
        for (uint v = 0u; v < 4u; ++v)
        {
            float m = v == 0u ? 0.0 : margin * P.v[9][v - 1u];
            bool pass = (flags & 32u) == 0u && inside(r, flags, m);            // (a free slot: dead) frustum only: the viewer keeps its own occlusion state on top (the Hi-Z verdicts are the CPU's)
            if (!pass && (flags & 3u) == 2u)            // SKIP_FRUSTUM_CHECK: taken with a parent that passed
            {
                Row pr = rows.r[parent];
                pass = inside(pr, floatBitsToUint(pr.h.w), m);
            }
            if (pass) atomicOr(sBits[v], 1u << lid);
        }
    }
    barrier();
    if (lid < 4u) Out(pc.outp).o[lid * pc.words + gl_WorkGroupID.x] = sBits[lid];
}
)GLSL";

constexpr U32 kCullSlots = 8;
constexpr U32 kCullSlotWords = 4 * 4096;        // 4 variants of 4096 words = 131072 rows a slot

struct CullShadowState
{
    bool mTried = false, mReady = false;
    VkPipelineLayout mLayout = VK_NULL_HANDLE;
    VkPipeline mPipe = VK_NULL_HANDLE;
    Buffer mResults;
    struct Pending { U32 mSerial, mSlot; U64 mFrame; U32 mWords; };
    std::deque<Pending> mPending;
    std::vector<LLVkBridge::CullRow> mTable;        // C1c: the worker's copy of the table, kept up to date by the delta rows of the records
    U64 mRecords = 0, mRows = 0, mDeltaRows = 0, mResets = 0, mBadSlots = 0, mTooMany = 0, mNoPipeline = 0, mPublished = 0, mLast = 0;
};
CullShadowState sCullShadow;

bool cull_init()
{
    CullShadowState& s = sCullShadow;
    if (s.mReady)
    {
        return true;
    }
    if (s.mTried)
    {
        return false;
    }
    s.mTried = true;
    Context& ctx = Context::instance();
    VkDevice dev = ctx.getDevice();
    VkShaderModule mod = hiz_compile(kCullTestShader, "cull test");
    if (mod == VK_NULL_HANDLE)
    {
        return false;
    }
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 32 };
    VkPipelineLayoutCreateInfo li = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(dev, &li, nullptr, &s.mLayout) != VK_SUCCESS)
    {
        return false;
    }
    VkComputePipelineCreateInfo pi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    pi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pi.stage.pName = "main";
    pi.stage.module = mod;
    pi.layout = s.mLayout;
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pi, nullptr, &s.mPipe) != VK_SUCCESS)
    {
        return false;
    }
    if (!ctx.createBuffer((VkDeviceSize)kCullSlots * kCullSlotWords * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Context::Memory::Readback, s.mResults) || !s.mResults.mMapped || !s.mResults.mAddress)
    {
        return false;
    }
    memset(s.mResults.mMapped, 0, (size_t)kCullSlots * kCullSlotWords * 4);
    LLVkBridge::cullSetResults((const U32*)s.mResults.mMapped, kCullSlotWords);
    s.mReady = true;
    LL_INFOS("VkCull") << "cull shadow ready: " << kCullSlots << " result slots of " << kCullSlotWords << " words" << LL_ENDL;
    return true;
}

void cull_poll()
{
    CullShadowState& s = sCullShadow;
    if (!s.mReady)
    {
        return;
    }
    Context& ctx = Context::instance();
    while (!s.mPending.empty() && ctx.frameFinished(s.mPending.front().mFrame))
    {
        const CullShadowState::Pending p = s.mPending.front();
        s.mPending.pop_front();
        vmaInvalidateAllocation(ctx.getAllocator(), s.mResults.mAllocation, (VkDeviceSize)p.mSlot * kCullSlotWords * 4, (VkDeviceSize)p.mWords * 4 * 4);
        LLVkBridge::cullPublish(p.mSlot, p.mSerial);
        ++s.mPublished;
    }
}

// kind 35: delta rows for the worker's copy of the table (blob: CullParams, slots[mEntries] padded to 16 bytes, rows[mEntries]); mMatrices = rows of the whole table, mPaletteFloats 1 = start the copy afresh
void on_cull_rows(const LLVkBridge::ShadowPacketsHeader& h, const U8* blob)
{
    CullShadowState& s = sCullShadow;
    ++s.mRecords;
    Renderer& r = Renderer::instance();
    Context& ctx = Context::instance();
    cull_poll();
    const U32 table = h.mMatrices;
    if (!table)
    {
        ++s.mNoPipeline;
        return;
    }
    // the delta goes into the copy first, whatever happens to this record's test: the next record builds on it
    {
        LLVkBridge::CullRow dead;
        memset(&dead, 0, sizeof(dead));
        dead.mFlags = 33u;
        if (h.mPaletteFloats == 1)
        {
            s.mTable.assign(table, dead);
            ++s.mResets;
        }
        else
        {
            s.mTable.resize(table, dead);
        }
        const U8* p = blob + sizeof(LLVkBridge::CullParams);
        const U32* slots = (const U32*)p;
        const size_t slot_bytes = ((size_t)h.mEntries * 4 + 15) & ~(size_t)15;
        const U8* rows = p + slot_bytes;
        for (U32 i = 0; i < h.mEntries; ++i)
        {
            U32 slot;
            memcpy(&slot, slots + i, 4);
            if (slot >= table)
            {
                ++s.mBadSlots;
                continue;
            }
            memcpy(&s.mTable[slot], rows + (size_t)i * sizeof(LLVkBridge::CullRow), sizeof(LLVkBridge::CullRow));
        }
        s.mDeltaRows += h.mEntries;
        s.mRows += table;
    }
    if (!cull_init())
    {
        ++s.mNoPipeline;
        return;
    }
    const U32 words = (table + 31) / 32;
    if (4 * words > kCullSlotWords)
    {
        ++s.mTooMany;
        return;
    }
    const size_t bytes = sizeof(LLVkBridge::CullParams) + (size_t)table * sizeof(LLVkBridge::CullRow);
    Renderer::ScratchAlloc sc = r.allocVertexScratch(bytes);
    if (!sc.mPtr || !sc.mAddress)
    {
        ++s.mNoPipeline;
        return;
    }
    memcpy(sc.mPtr, blob, sizeof(LLVkBridge::CullParams));
    memcpy((U8*)sc.mPtr + sizeof(LLVkBridge::CullParams), s.mTable.data(), (size_t)table * sizeof(LLVkBridge::CullRow));
    const U32 slot = h.mPass % kCullSlots;
    const U64 out_address = (U64)s.mResults.mAddress + (U64)slot * kCullSlotWords * 4;
    const U64 params_address = (U64)sc.mAddress;
    const U64 rows_address = params_address + sizeof(LLVkBridge::CullParams);
    struct CullPc { U32 mRows[2]; U32 mParams[2]; U32 mOut[2]; U32 mCount; U32 mWords; } pc;
    static_assert(sizeof(CullPc) == 32, "cull push constants");
    pc.mRows[0] = (U32)rows_address; pc.mRows[1] = (U32)(rows_address >> 32);
    pc.mParams[0] = (U32)params_address; pc.mParams[1] = (U32)(params_address >> 32);
    pc.mOut[0] = (U32)out_address; pc.mOut[1] = (U32)(out_address >> 32);
    pc.mCount = table;
    pc.mWords = words;
    const VkPipeline pipe = s.mPipe;
    const VkPipelineLayout layout = s.mLayout;
    const bool recorded = r.recordExternal({}, [&](VkCommandBuffer cmd) {
        if (LLVkBridge::gPhaseTiming)
        {
            ctx.gpuStamp(Context::stampTag("cull shadow test"));
        }
        VkMemoryBarrier2 mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        VkDependencyInfo di = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        di.memoryBarrierCount = 1;
        di.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &di);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, words, 1, 1);
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier2(cmd, &di);
    });
    if (!recorded)
    {
        ++s.mNoPipeline;
        return;
    }
    s.mPending.push_back({ h.mFragment, slot, ctx.getFrameNumber(), words });
    if (s.mRecords - s.mLast >= 600)
    {
        s.mLast = s.mRecords;
        LL_INFOS("VkCull") << "cull shadow worker over " << s.mRecords << " records: " << (double)s.mRows / (double)s.mRecords << " table rows a record, " << (double)s.mDeltaRows / (double)s.mRecords << " delta rows a record, " << s.mResets << " resets, " << s.mBadSlots << " bad slots, published " << s.mPublished << ", still pending " << s.mPending.size()
            << "; refused: too many rows " << s.mTooMany << ", no pipeline/other " << s.mNoPipeline << LL_ENDL;
    }
}

// <FS> S1 retained fragments: the table id -> entries (kinds 11-13 of LLVkBridge::shadowPackets). Ordered with the draws by the queue, so a release never overtakes a pass that uses it.
static std::unordered_map<U32, std::vector<LLVkBridge::ShadowPacketEntry>> sShadowFragments;
static U64 sFragmentMissing = 0;

void on_shadow_packets(const LLVkBridge::ShadowPacketsHeader& h, const U8* blob);
static void on_shadow_fragments(const LLVkBridge::ShadowPacketsHeader& h, const U8* blob)
{
    typedef LLVkBridge::ShadowPacketEntry Entry;
    if (h.mKind == 18) { res_retain_alpha(h, blob); return; }
    if (h.mKind == 19) { res_tex_update(h, blob); return; }
    if (h.mKind >= 14 && h.mKind <= 17)
    {
        // timing per kind (LL_VK_SCENE_TIMES=1): what the resident records cost the worker
        static const bool timed = getenv("LL_VK_SCENE_TIMES") != nullptr;
        static U64 ns[4] = {}, calls[4] = {}, last_log = 0;
        const auto t0 = std::chrono::steady_clock::now();
        if (h.mKind == 14) res_retain(h, blob);
        else if (h.mKind == 15) res_matrices(h, blob);
        else if (h.mKind == 16) res_draw(h, blob);
        else res_note(h, blob);
        if (timed)
        {
            ns[h.mKind - 14] += (U64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
            ++calls[h.mKind - 14];
            const U64 now = (U64)Context::instance().getFrameNumber();
            if (now >= last_log + 300)
            {
                last_log = now;
                LL_INFOS("VkScene") << "resident records, worker time: retain " << ns[0] / 1000000 << " ms in " << calls[0] << " calls, matrices " << ns[1] / 1000000 << " ms in " << calls[1] << ", draw "
                    << ns[2] / 1000000 << " ms in " << calls[2] << ", note " << ns[3] / 1000000 << " ms in " << calls[3] << " (totals)" << LL_ENDL;
            }
        }
        return;
    }
    if (h.mKind == 11)
    {
        sShadowFragments[h.mFragment].assign((const Entry*)blob, (const Entry*)blob + h.mEntries);
        return;
    }
    if (h.mKind == 12)
    {
        const U32* ids = (const U32*)blob;
        for (U32 i = 0; i < h.mEntries; ++i)
        {
            sShadowFragments.erase(ids[i]);
            res_release(ids[i]);
        }
        return;
    }
    struct Ref { U32 mId, mMapOffset; };
    const Ref* refs = (const Ref*)blob;
    const U32* map = (const U32*)(blob + sizeof(Ref) * h.mEntries);
    const U8* matrices = (const U8*)(map + h.mPaletteFloats);
    static thread_local std::vector<U8> expanded;
    size_t total = 0;
    for (U32 i = 0; i < h.mEntries; ++i)
    {
        auto it = sShadowFragments.find(refs[i].mId);
        total += it == sShadowFragments.end() ? 0 : it->second.size();
    }
    expanded.resize(total * sizeof(Entry) + 64 * (size_t)h.mMatrices);
    Entry* out = (Entry*)expanded.data();
    size_t n = 0;
    for (U32 i = 0; i < h.mEntries; ++i)
    {
        auto it = sShadowFragments.find(refs[i].mId);
        if (it == sShadowFragments.end())
        {
            ++sFragmentMissing;
            continue;
        }
        const U32* local = map + refs[i].mMapOffset;
        for (const Entry& e : it->second)
        {
            Entry& o = out[n++];
            o = e;
            o.mMatrix = e.mMatrix ? local[e.mMatrix - 1] : 0;
        }
    }
    if (n == 0)
    {
        return;
    }
    memcpy(expanded.data() + n * sizeof(Entry), matrices, 64 * (size_t)h.mMatrices);
    LLVkBridge::ShadowPacketsHeader plain = h;
    plain.mKind = 0;
    plain.mEntries = (U32)n;
    plain.mPaletteFloats = 0;
    on_shadow_packets(plain, expanded.data());
}

// <FS> Render packets: the worker side of LLVkBridge::shadowPackets. The program bound is the one of shadowPullV.glsl: it has no vertex input and finds the position of
// every vertex itself, from the buffer addresses in the table written here (one 32 byte entry per draw; the entry number is the draw's instance index).
void on_shadow_packets_impl(const LLVkBridge::ShadowPacketsHeader& h, const U8* blob);
// S6 census (LL_VK_OPSTATS): what the worker spends in each packet kind (calls and time per command buffer submit, which is 2 a presented frame; logged every 600 submits)
void on_shadow_packets(const LLVkBridge::ShadowPacketsHeader& h, const U8* blob)
{
    static const bool census = getenv("LL_VK_OPSTATS") != nullptr;
    if (!census || h.mKind >= 40)
    {
        on_shadow_packets_impl(h, blob);
        return;
    }
    static U64 ns[40] = {}, calls[40] = {}, entries[40] = {}, first_frame = 0, nested = 0;
    if (nested)
    {
        on_shadow_packets_impl(h, blob);        // (a kind that dispatches another through here: counted by the outer one)
        return;
    }
    const U64 now = (U64)Context::instance().getFrameNumber();
    if (!first_frame)
    {
        first_frame = now;
    }
    nested = 1;
    const auto t0 = std::chrono::steady_clock::now();
    on_shadow_packets_impl(h, blob);
    ns[h.mKind] += (U64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    ++calls[h.mKind];
    entries[h.mKind] += h.mEntries;
    nested = 0;
    if (now >= first_frame + 600)
    {
        const double frames = (double)(now - first_frame);
        std::string text;
        for (U32 k = 0; k < 40; ++k)
        {
            if (calls[k])
            {
                char buf[160];
                snprintf(buf, sizeof(buf), " | kind %u: %.1f calls, %.1f entries, %.3f ms (%.2f us each)", k, (double)calls[k] / frames, (double)entries[k] / frames, (double)ns[k] / 1.0e6 / frames, (double)ns[k] / 1000.0 / (double)calls[k]);
                text += buf;
                ns[k] = calls[k] = entries[k] = 0;
            }
        }
        first_frame = now;
        LL_INFOS("VkOps") << "packet kinds, worker, per command buffer submit (2 a frame: multiply by 2 for per frame):" << text << LL_ENDL;
    }
}

void on_shadow_packets_impl(const LLVkBridge::ShadowPacketsHeader& h, const U8* blob)
{
    if ((h.mKind >= 20 && h.mKind <= 24) || h.mKind == 29 || h.mKind == 31)
    {
        geo_dispatch(h, blob);      // <FS> S3 G2: the scene geometry table
        return;
    }
    if (h.mKind >= 11 && h.mKind <= 19)
    {
        on_shadow_fragments(h, blob);
        return;
    }
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
    if (h.mKind == 10)
    {
        Rt::updateMaterials((const LLVkBridge::RtMaterialDef*)blob, h.mEntries);
        return;
    }
    if (h.mKind == 8)
    {
        on_velocity_packets(h, blob);
        return;
    }
    if (h.mKind == 9)
    {
        on_object_velocity_packets(h, blob);
        return;
    }
    if (h.mKind == 27)
    {
        on_palette_segment(h, blob);
        return;
    }
    if (h.mKind == 33)
    {
        on_light_volumes(h, blob);
        return;
    }
    if (h.mKind == 34)
    {
        on_hiz_boxes(h, blob);       // <FS> S7 H1
        return;
    }
    if (h.mKind == 35)
    {
        on_cull_rows(h, blob);       // <FS> S7 C1
        return;
    }
    if (h.mKind == 3 || h.mKind == 25 || h.mKind == 26)
    {
        on_geometry_packets(h, blob, h.mKind >= 25, h.mKind == 26);
        return;
    }
    if (h.mKind == 32)
    {
        on_geometry_packets(h, blob, h.mPass == 1, h.mPass == 1, true);       // S5f F3: transient alpha draws (F4: mPass 1 = rigged: indexed draws, palettes of the shared list)
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
    const LLVkBridge::ShadowPacketEntry* entries = (const LLVkBridge::ShadowPacketEntry*)blob;
    const F32* matrices = (const F32*)(blob + sizeof(LLVkBridge::ShadowPacketEntry) * h.mEntries);
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
        const LLVkBridge::ShadowPacketEntry& e = entries[i];
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
        if (Rt::active() && h.mFragment != 1)       // (S6.6c: mFragment 1: the draws of this record are only drawn, they are not casters of the ray scene)
        {
            Rt::AlphaInfo info;
            if (Rt::materialsOn())
            {
                info.mFlags = (e.mTexcoordOffset ? 8u : 0u) | (e.mColorOffset ? 16u : 0u);
                info.mTexcoordOffset = e.mTexcoordOffset;
                info.mColorOffset = e.mColorOffset;
                info.mNormalOffset = e.mNormalOffset;
                info.mMaterial = e.mMaterial;
            }
            note_rt_caster(h.mPass, e.mVertexBuffer, e.mIndexBuffer, *vb, *ib, e.mFirstIndex, e.mIndexCount, e.mMatrixId, 0, e.mMatrix ? matrices + 16 * (size_t)e.mMatrix : nullptr, Rt::materialsOn() ? &info : nullptr);
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

unsigned int rtPinAcquire(unsigned int gl_name)
{
    if (!gl_name)
    {
        return 0;
    }
    auto it = sPinByName.find(gl_name);
    U32 index;
    if (it != sPinByName.end())
    {
        index = it->second;
    }
    else
    {
        if (!sPinFree.empty())
        {
            index = sPinFree.back();
            sPinFree.pop_back();
        }
        else
        {
            index = (U32)sPins.size();
            sPins.emplace_back();
        }
        sPins[index] = Pin();
        sPins[index].mName = gl_name;
        sPinByName[gl_name] = index;
    }
    ++sPins[index].mRefs;
    if (sPins[index].mRefs == 1)
    {
        pin_queue(index);
    }
    return index;
}

void rtPinRelease(unsigned int index)
{
    if (!index || index >= sPins.size() || !sPins[index].mName || !sPins[index].mRefs)
    {
        return;
    }
    Pin& p = sPins[index];
    if (--p.mRefs)
    {
        return;
    }
    if (p.mSlot)
    {
        p.mSlot = 0;
        Rt::setPinSlot(index, 0);
    }
    sPinByName.erase(p.mName);
    if (Tex* t = packet_texture(p.mName))
    {
        if (t->mPinIndex == index)
        {
            t->mPinIndex = 0;
        }
    }
    p.mName = 0;
    sPinFree.push_back(index);
}

void rtPinUpdate()
{
    handles_refresh();
    // 1. pins whose texture changed or that are new
    for (size_t i = 0; i < sPinDirty.size(); ++i)
    {
        pin_resolve(sPinDirty[i]);
    }
    sPinDirty.clear();
    // 2. pins still waiting for an image: a slice in turns, each one only when it is due
    const U64 frame = Context::instance().getFrameNumber();
    if (!sPinWaiting.empty())
    {
        const size_t n = std::min<size_t>(sPinWaiting.size(), 256);
        std::vector<U32> batch(sPinWaiting.begin(), sPinWaiting.begin() + n);
        sPinWaiting.erase(sPinWaiting.begin(), sPinWaiting.begin() + n);
        for (U32 index : batch)
        {
            if (index < sPins.size())
            {
                sPins[index].mWaiting = false;
                if (sPins[index].mName && !sPins[index].mSlot && sPins[index].mRefs)
                {
                    if (frame < sPins[index].mNextTry)
                    {
                        sPins[index].mWaiting = true;       // not due yet: back of the line, nothing looked at
                        sPinWaiting.push_back(index);
                    }
                    else
                    {
                        pin_resolve(index);         // (puts itself back into the waiting list when there is still nothing)
                    }
                }
            }
        }
    }
    // 3. the safety net: every pin is looked at again about every 32 frames
    const size_t total = sPins.size();
    if (total > 1)
    {
        const size_t n = total / 32 + 1;
        for (size_t k = 0; k < n; ++k)
        {
            if (++sPinCheckCursor >= total)
            {
                sPinCheckCursor = 1;
            }
            if (sPins[sPinCheckCursor].mName && sPins[sPinCheckCursor].mSlot)
            {
                pin_resolve((U32)sPinCheckCursor);
            }
        }
    }
}

void rtPinCounts(unsigned long long& pins, unsigned long long& resolves, unsigned long long& events)
{
    pins = sPinByName.size();
    resolves = sPinResolves;
    events = sPinEvents;
    sPinResolves = sPinEvents = 0;
}

// asks the memory system for what packet_texture_slot will read (see Rt::TexturePrefetchFn)
static void packet_texture_prefetch(U32 name, U32 stage)
{
    Tex* t = packet_texture(name);
    if (!t)
    {
        return;
    }
    if (stage == 0)
    {
        const char* p = (const char*)t;
        _mm_prefetch(p, _MM_HINT_T0);
        _mm_prefetch(p + 64, _MM_HINT_T0);
        _mm_prefetch(p + 128, _MM_HINT_T0);
        _mm_prefetch((const char*)&t->mVkSampler, _MM_HINT_T0);
        _mm_prefetch((const char*)&t->mHandle, _MM_HINT_T0);
        _mm_prefetch((const char*)&t->mHandleViewId, _MM_HINT_T0);
    }
    else
    {
        _mm_prefetch((const char*)&t->mImage, _MM_HINT_T0);
    }
}

// <FS> Ray query shadows: the viewer asks for the frame's scene before the lighting that samples the sun shadow (LLVkBridge::rtScene)
void on_rt_scene(const LLVkBridge::RtSceneArgs& args)
{
    if (!Rt::active())
    {
        return;
    }
    flush_buffer_updates();         // the structures are built over the vertex data: writes that are still pending are recorded first
    res_ray_membership(Context::instance().getFrameNumber());
    Rt::setDistanceScale(args.mDistanceScale);
    Rt::buildScene(args.mView, [](U32 name) { return packet_texture_slot(Renderer::instance(), name); }, packet_texture_prefetch);
}

//-----------------------------------------------------------------------------------------------------
// hooks: state and clears
//-----------------------------------------------------------------------------------------------------

// F1.4: clears the current target (the colour and depth write masks and the scissor of the state block apply, like a GL clear)
void clear_target(bool color, bool depth, const float* rgba)
{
    HookTimer hook_timer;
    if (!LLVkBridge::inFrame() || !sync_target())
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
    std::lock_guard<LLVkBridge::Mutex> lock(gMutex);
    U64 tex_bytes = 0, rt_bytes = 0, buf_bytes = 0, unused_mip_bytes = 0, bc_bytes = 0;
    U32 tex_count = 0, rt_count = 0, views = 0, buf_count = 0, bc_count = 0;      // <FS> bc_*: the block-compressed textures (BC7 at decode) within the mirrored ones
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
        const bool target = (t.mImage.mAspect != VK_IMAGE_ASPECT_COLOR_BIT) || !t.mViews.empty() || gFrameState.mTargets.count(kv.first);
        if (target) { rt_bytes += bytes; ++rt_count; } else { tex_bytes += bytes; ++tex_count; if (fi.mCompressed) { bc_bytes += bytes; ++bc_count; } }
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
    return "mirrored textures " + std::to_string(tex_count) + " (" + std::to_string(tex_bytes / (1024 * 1024)) + " MB, of which block compressed " + std::to_string(bc_count) + " (" + std::to_string(bc_bytes / (1024 * 1024)) + " MB)), render targets " + std::to_string(rt_count) + " ("
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
    std::lock_guard<LLVkBridge::Mutex> lock(gMutex);
    flush_buffer_updates(true);
    probe_run();
}

void after_frame()
{
    {
        // <FS> LL_VK_PIN_WORKER=<logical cpu>: a test of what a core of its own does for the render worker (doc/vulkan/PLAN-texture-handles.md section 17.22)
        static bool pinned = false;
        if (!pinned)
        {
            pinned = true;
            char pin[16] = {};
            if (GetEnvironmentVariableA("LL_VK_PIN_WORKER", pin, sizeof(pin)) > 0)
            {
                const int cpu = atoi(pin);
                const DWORD_PTR previous = SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << cpu);
                LL_INFOS("CpuProbe") << "render worker thread pinned to logical cpu " << cpu << (previous ? " (done)" : " (FAILED)") << LL_ENDL;
            }
        }
    }
    gFsWorkerCpu.store((int)GetCurrentProcessorNumber(), std::memory_order_relaxed);        // <FS> LL_VK_CPUPROBE core census (pipeline.cpp)
    gFsWorkerTid.store((unsigned)GetCurrentThreadId(), std::memory_order_relaxed);
    std::lock_guard<LLVkBridge::Mutex> lock(gMutex);
    if (sUiDiff)
    {
        uidiff_frame_end();
    }
    if (LLVkAsync::gEnabled)
    {
        drain_query_results();
    }
    hiz_poll();     // <FS> S7 H1: publish the Hi-Z results of the frames the GPU has finished
    {
        // <FS> H6: what the draws measured of the textures' need in the frames the GPU has finished, given to the textures by id
        static std::vector<Renderer::FbBatch> batches;
        Renderer::instance().feedbackTake(batches);
        for (const Renderer::FbBatch& b : batches)
        {
            for (const Renderer::FbReading& r : b.mReadings)
            {
                const U32 name = r.mHandle < sHandleName.size() ? sHandleName[r.mHandle] : 0;
                if (name)
                {
                    LLVkBridge::textureNeedReading(name, r.mArea);
                }
            }
        }
        batches.clear();
    }
    handles_refresh();      // the handles of the textures that changed in this frame show their new image from the next frame on
    {
        // <FS> the video memory and what the textures take of it, every 1800 frames (about 30 seconds): after a teleport this shows where the memory went and whether a texture was refused
        {
            // <FS> every 300 frames (about 5 s), when something changed: what the texture copies from targets and the re-specified images did in that time (cumulative totals, the difference between two lines is the interval)
            static U32 copy_frames = 0;
            static U64 last[14] = {};
            if (++copy_frames >= 300)
            {
                copy_frames = 0;
                const U64 now_v[14] = { (U64)sCopies, sCopyDropFrame, sCopyDropTex, sCopyDropSrc, sRespecs, sGrows, sLevelDrops, sLevelDropsRefused, sSampleNoName, sSampleNoImage, sSampleBlank, sPinNoSlot,
                                        Renderer::instance().handleSets(), sCommits };
                static const char* const names[14] = { "texture copies from targets done", "dropped: not in a frame", "no texture image", "no source image", "images re-specified", "mip chains grown", "levels dropped",
                                                       "drops refused", "draw samples of a deleted name", "of a texture without an image", "of an image with no level yet", "pin resolves that ended without a handle",
                                                       "descriptors written for handles (a new image or sampler)", "images committed under the same id (a new resolution)" };
                if (memcmp(now_v, last, sizeof(now_v)) != 0)
                {
                    std::ostringstream os;
                    for (int k = 0; k < 14; ++k)
                    {
                        os << (k ? (k == 1 || k == 5 || k == 8 || k == 11 ? "; " : ", ") : "") << names[k] << " " << now_v[k] << " (+" << now_v[k] - last[k] << ")";
                    }
                    LL_INFOS("VkTexCopy") << os.str() << LL_ENDL;
                    memcpy(last, now_v, sizeof(last));
                }
            }
        }
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
            if (!LLVkBridge::gPhaseTiming)
            {
                LLVkBridge::gPhaseTiming = true;
                QueryPerformanceCounter(&t_start);
            }
            if (++frames >= 600)
            {
                LARGE_INTEGER freq, now;
                QueryPerformanceFrequency(&freq);
                QueryPerformanceCounter(&now);
                const double wall_ms = (double)(now.QuadPart - t_start.QuadPart) * 1000.0 / (double)freq.QuadPart;
                std::string text;
                for (int p = 0; p < LLVkBridge::PH_COUNT; ++p)
                {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "%s %.2f | ", LLVkBridge::phaseName(p), (double)LLVkBridge::gPhaseTicks[p] * 1000.0 / (double)freq.QuadPart / frames);
                    text += buf;
                    LLVkBridge::gPhaseTicks[p] = 0;
                }
                LL_INFOS("VkPhases") << frames << " frames, " << wall_ms / frames << " ms/frame wall, " << (sDrawsTotal - draws_at_start) / frames
                                     << " draws/frame; CPU ms/frame by phase: " << text << LL_ENDL;
                {
                    {
                        const Renderer::DrawDiag dd = Renderer::instance().takeDrawDiag();
                        LL_INFOS("VkPhases") << "indexed draws per frame: " << (double)dd.mDraws / frames << ", of which changed nothing but the draw command: " << (double)dd.mUnchanged / frames
                                             << ", of which also continued the previous index range (mergeable): " << (double)dd.mMergeable / frames
                                             << "; draws left out for a range outside their buffers (since the start): " << sOutOfRangeDraws << LL_ENDL;
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
                            {
                                // <FS> the acceleration structure arena (the difference between two lines is the interval)
                                static ArenaStats prev;
                                const double dt = std::max(1.0, (double)frames);
                                const U64 made = as.mAccelCreateMicros - prev.mAccelCreateMicros;
                                const U64 dedicated_released = as.mAccelDedicatedReleased - prev.mAccelDedicatedReleased;
                                LL_INFOS("VkAsArena") << "acceleration structure arena: " << as.mAccelBlocks << " blocks alive (" << (as.mAccelBlockBytes >> 20) << " MB, " << (as.mAccelUsedBytes >> 20)
                                                      << " MB in use, largest free range " << (as.mAccelLargestFree >> 20) << " MB), of them " << as.mAccelDedicatedAlive << " dedicated (" << (as.mAccelDedicatedBytes >> 20)
                                                      << " MB); in this interval: " << as.mAccelAllocs - prev.mAccelAllocs << " ranges asked for (" << ((as.mAccelAllocBytes - prev.mAccelAllocBytes) >> 20)
                                                      << " MB; <1 MB " << as.mAccelBucket[0] - prev.mAccelBucket[0] << ", <8 " << as.mAccelBucket[1] - prev.mAccelBucket[1] << ", <32 " << as.mAccelBucket[2] - prev.mAccelBucket[2]
                                                      << ", <64 " << as.mAccelBucket[3] - prev.mAccelBucket[3] << ", <128 " << as.mAccelBucket[4] - prev.mAccelBucket[4] << ", more " << as.mAccelBucket[5] - prev.mAccelBucket[5]
                                                      << "), blocks made " << as.mAccelBlocksCreated - prev.mAccelBlocksCreated << " (dedicated " << as.mAccelDedicatedCreated - prev.mAccelDedicatedCreated << ", "
                                                      << ((as.mAccelCreatedBytes - prev.mAccelCreatedBytes) >> 20) << " MB, " << made / 1000.0 << " ms in vmaCreateBuffer = " << made / 1000.0 / dt << " ms/frame), blocks given back "
                                                      << as.mAccelBlocksReleased - prev.mAccelBlocksReleased << " (dedicated " << dedicated_released << ", lived "
                                                      << (dedicated_released ? (double)(as.mAccelDedicatedLifeFrames - prev.mAccelDedicatedLifeFrames) / (double)dedicated_released : 0.0) << " frames on average) | pool of empty dedicated blocks: reused " << as.mAccelPoolHits - prev.mAccelPoolHits << ", not found " << as.mAccelPoolMisses - prev.mAccelPoolMisses
                                                      << ", given back after a wait " << as.mAccelPoolTrimmed - prev.mAccelPoolTrimmed << ", holding now " << as.mAccelPooled << " blocks (" << (as.mAccelPooledBytes >> 20) << " MB)" << LL_ENDL;
                                LL_INFOS("VkAsArena") << Context::instance().arenaAccelCensus() << LL_ENDL;
                                prev = as;
                            }
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
                        LL_INFOS("VkPhases") << Renderer::instance().takeFlushReport(frames) << LL_ENDL;
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
                            {
                                U64 vram_used = 0, vram_budget = 0;
                                if (Context::instance().videoMemory(vram_used, vram_budget))
                                {
                                    LL_INFOS("VkPhases") << "video memory: " << (vram_used >> 20) << " MB used of a budget of " << (vram_budget >> 20) << " MB" << LL_ENDL;
                                }
                            }
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
    gFrameState.mTargets.clear();
    memset(gFrameState.mRecentTargets, 0, sizeof(gFrameState.mRecentTargets));
    gFrameState.mSampledLast = gFrameState.mSampled;
    gFrameState.mSampled.clear();
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
    return !disabled;
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
    if (!queries_on() || !LLVkBridge::inFrame() || !Renderer::instance().isReady())
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
    if (it != sQueries.end() && LLVkBridge::inFrame())
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
        if (LLVkBridge::inFrame() && !r.flushAndRestart(1))
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
    U8* dst = (U8*)pixels;
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
    U8* dst = (U8*)pixels;
    Tex* t = edit_texture();
    if (!dst || !t || !t->mImage.valid() || level < 0 || (U32)level >= t->mImage.mMipLevels)
    {
        return;
    }
    Renderer& r = Renderer::instance();
    if (LLVkBridge::inFrame() && !r.flushAndRestart(2))
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
    U8* dst = (U8*)img;
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
    if (LLVkBridge::inFrame() && !r.flushAndRestart(3))
    {
        return;
    }
    const U32 layer = tg.mFace >= 0 ? (U32)tg.mFace : 0;
    if (r.readbackCompressedMip(t->mImage, (U32)level, layer, blocks))
    {
        memcpy(dst, blocks.data(), blocks.size());
    }
}

// <FS> DLSS-NR (doc/vulkan/PLAN-dlss-nr.md step two): the finished window-size colour through the neural renderer; depth and motion at the render size
void on_neural_render(const LLVkBridge::UpscaleArgs& a)
{
    static U32 failures = 0;
    if (!gActive)
    {
        return;
    }
    if (a.mNeuralRender == 2)
    {
        LLVkDlssNr::release();
        return;
    }
    if (!LLVkDlssNr::ready())
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
            LL_WARNS("VkDlssNr") << "neural render: a texture is missing (colour " << (color != nullptr) << ", depth " << (depth != nullptr) << ", motion " << (motion != nullptr)
                                 << ", output " << (output != nullptr) << ")" << LL_ENDL;
        }
        return;
    }
    LLVkDlssNr::Params p;
    p.mColor = &color->mImage;
    p.mDepth = &depth->mImage;
    p.mMotion = &motion->mImage;
    p.mOutput = &output->mImage;
    p.mRenderWidth = a.mRenderWidth;
    p.mRenderHeight = a.mRenderHeight;
    p.mOutputWidth = a.mOutputWidth;
    p.mOutputHeight = a.mOutputHeight;
    p.mMotionScaleX = a.mMotionScaleX;
    p.mMotionScaleY = a.mMotionScaleY;
    p.mReset = a.mReset != 0;
    p.mQuality = (U32)llmax(a.mNrQuality, 0);
    p.mPreset = a.mPreset;
    p.mAutoMask = a.mNrAutoMask;
    p.mScalingRatio = a.mNrScalingRatio;
    p.mStyle = a.mNrStyle;
    p.mIntensity = a.mNrIntensity;
    p.mGlobalTone = a.mNrGlobalTone;
    p.mLocalTone = a.mNrLocalTone;
    p.mLocalStructure = a.mNrLocalStructure;
    p.mSkinStructure = a.mNrSkinStructure;
    std::vector<std::pair<Image*, VkImageLayout>> needs = {
        { p.mColor, LLVkDlss::inputLayout(*p.mColor) },
        { p.mDepth, LLVkDlss::inputLayout(*p.mDepth) },
        { p.mMotion, LLVkDlss::inputLayout(*p.mMotion) },
        { p.mOutput, LLVkDlss::outputLayout() },
    };
    bool done = false;
    Renderer::instance().recordExternal(needs, [&](VkCommandBuffer cmd) { done = LLVkDlssNr::evaluate(cmd, p); });
    if (done)
    {
        output->mValidLevels = 1;
    }
    else if (failures++ < 5)
    {
        LL_WARNS("VkDlssNr") << "the neural render did not run" << LL_ENDL;
    }
}

void on_upscale(const LLVkBridge::UpscaleArgs& a)
{
    static U32 failures = 0;
    if (a.mNeuralRender)
    {
        on_neural_render(a);
        return;
    }
    if (!gActive || !LLVkUpscale::ready())
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
    Tex* diffuse = a.mRayReconstruction ? find(a.mDiffuseAlbedo) : nullptr;
    Tex* specular = a.mRayReconstruction ? find(a.mSpecularAlbedo) : nullptr;
    Tex* normals = a.mRayReconstruction ? find(a.mNormals) : nullptr;
    Tex* exposure = a.mRayReconstruction && a.mExposure ? find(a.mExposure) : nullptr;
    Tex* hit_distance = a.mRayReconstruction && a.mSpecularHitDistance ? find(a.mSpecularHitDistance) : nullptr;
    Tex* refl_motion = a.mRayReconstruction && a.mReflectionMotion ? find(a.mReflectionMotion) : nullptr;
    if (!color || !depth || !motion || !output || (a.mRayReconstruction && (!diffuse || !specular || !normals)))
    {
        if (failures++ < 5)
        {
            LL_WARNS("VkDlss") << "upscale: a texture is missing (colour " << (color != nullptr) << ", depth " << (depth != nullptr) << ", motion " << (motion != nullptr)
                               << ", output " << (output != nullptr) << ", guides " << (diffuse != nullptr) << (specular != nullptr) << (normals != nullptr) << ")" << LL_ENDL;
        }
        return;
    }
    LLVkUpscale::Params p;
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
    p.mCameraNear = a.mCameraNear;
    p.mCameraFar = a.mCameraFar;
    p.mCameraFovY = a.mCameraFovY;
    p.mFrameTimeMs = a.mFrameTimeMs;
    std::vector<std::pair<Image*, VkImageLayout>> needs = {
        { p.mColor, LLVkUpscale::inputLayout(*p.mColor) },
        { p.mDepth, LLVkUpscale::inputLayout(*p.mDepth) },
        { p.mMotion, LLVkUpscale::inputLayout(*p.mMotion) },
        { p.mOutput, LLVkUpscale::outputLayout() },
    };
    if (a.mRayReconstruction)
    {
        p.mRayReconstruction = true;
        p.mDiffuseAlbedo = &diffuse->mImage;
        p.mSpecularAlbedo = &specular->mImage;
        p.mNormals = &normals->mImage;
        p.mPreset = a.mPreset;
        needs.push_back({ p.mDiffuseAlbedo, LLVkUpscale::inputLayout(*p.mDiffuseAlbedo) });
        needs.push_back({ p.mSpecularAlbedo, LLVkUpscale::inputLayout(*p.mSpecularAlbedo) });
        needs.push_back({ p.mNormals, LLVkUpscale::inputLayout(*p.mNormals) });
        if (refl_motion)
        {
            p.mReflectionMotion = &refl_motion->mImage;
            needs.push_back({ p.mReflectionMotion, LLVkUpscale::inputLayout(*p.mReflectionMotion) });
        }
        if (hit_distance)
        {
            p.mHitDistance = &hit_distance->mImage;
            memcpy(p.mWorldToView, a.mWorldToView, sizeof(p.mWorldToView));
            memcpy(p.mViewToClip, a.mViewToClip, sizeof(p.mViewToClip));
            needs.push_back({ p.mHitDistance, LLVkUpscale::inputLayout(*p.mHitDistance) });
        }
        if (exposure)
        {
            p.mExposure = &exposure->mImage;
            needs.push_back({ p.mExposure, LLVkUpscale::inputLayout(*p.mExposure) });
        }
    }
    bool done = false;
    Renderer::instance().recordExternal(needs, [&](VkCommandBuffer cmd) { done = LLVkUpscale::evaluate(cmd, p); });
    if (done)
    {
        output->mValidLevels = 1;       // what was written counts as the texture's content
    }
    else if (failures++ < 5)
    {
        LL_WARNS("VkDlss") << "the upscale did not run" << LL_ENDL;
    }
}


} // namespace LLVkWorker

#endif // LL_VULKAN

