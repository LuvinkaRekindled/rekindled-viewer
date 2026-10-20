/**
 * @file llvkformat.cpp
 * @brief F2.2: the decoders from the viewer's GL enum numbers to the typed texture descriptions (see llvkformat.h).
 */

#include "linden_common.h"
#include "llglheaders.h"

#if defined(LL_VULKAN)

#include "llvkformat.h"

#include <mutex>
#include <string>
#include <unordered_set>

namespace LLVkFormat
{

Target target(U32 gl)
{
    Target t;
    switch (gl)
    {
    case 0x0DE1: t.mDim = DIM_2D; break;                                // TEXTURE_2D
    case 0x8513: t.mDim = DIM_CUBE; break;                              // TEXTURE_CUBE_MAP
    case 0x8515: case 0x8516: case 0x8517: case 0x8518: case 0x8519: case 0x851A:       // TEXTURE_CUBE_MAP_POSITIVE_X .. NEGATIVE_Z
        t.mDim = DIM_CUBE;
        t.mFace = (S8)(gl - 0x8515);
        break;
    case 0x9009: t.mDim = DIM_CUBE_ARRAY; break;                        // TEXTURE_CUBE_MAP_ARRAY
    case 0x8C1A: t.mDim = DIM_2D_ARRAY; break;                          // TEXTURE_2D_ARRAY
    case 0x806F: t.mDim = DIM_3D; break;                                // TEXTURE_3D
    case 0x9100: t.mDim = DIM_MS; break;                                // TEXTURE_2D_MULTISAMPLE
    default: t.mDim = DIM_OTHER; break;
    }
    return t;
}

Transfer transfer(U32 gl_format, U32 gl_type)
{
    Transfer t;
    switch (gl_format)
    {
    case 0x1908: t.mComps = C_RGBA; break;                  // RGBA
    case 0x80E1: t.mComps = C_BGRA; break;                  // BGRA
    case 0x1907: t.mComps = C_RGB; break;                   // RGB
    case 0x80E0: t.mComps = C_BGR; break;                   // BGR
    case 0x190A: t.mComps = C_LUM_ALPHA; break;             // LUMINANCE_ALPHA
    case 0x8227: t.mComps = C_RG; break;                    // RG
    case 0x1906: t.mComps = C_ALPHA; break;                 // ALPHA
    case 0x1909: t.mComps = C_LUM; break;                   // LUMINANCE
    case 0x1903: t.mComps = C_R; break;                     // RED
    case 0x1902: t.mComps = C_DEPTH; break;                 // DEPTH_COMPONENT
    default: break;
    }
    switch (gl_type)
    {
    case 0x1401: t.mType = T_U8; break;
    case 0x1400: t.mType = T_S8; break;
    case 0x1403: t.mType = T_U16; break;
    case 0x1402: t.mType = T_S16; break;
    case 0x1405: t.mType = T_U32; break;
    case 0x1404: t.mType = T_S32; break;
    case 0x1406: t.mType = T_F32; break;
    case 0x140B: t.mType = T_F16; break;                    // HALF_FLOAT
    default: break;
    }
    return t;
}

static_assert((int)BK_VERTEX == (int)BufferKind::Vertex && (int)BK_INDEX == (int)BufferKind::Index && (int)BK_UNIFORM == (int)BufferKind::Uniform, "BufferKind");
static_assert((int)T_U8 == (int)ComponentType::U8 && (int)T_U16 == (int)ComponentType::U16 && (int)T_U32 == (int)ComponentType::U32 && (int)T_F32 == (int)ComponentType::F32 && (int)T_F16 == (int)ComponentType::F16, "ComponentType");
static_assert((int)TOP_POINTS == (int)PrimitiveMode::Points && (int)TOP_TRIANGLES == (int)PrimitiveMode::Triangles && (int)TOP_TRIANGLE_FAN == (int)PrimitiveMode::TriangleFan && (int)TOP_OTHER == (int)PrimitiveMode::Other, "PrimitiveMode");

static_assert((int)S_RGBA8 == (int)TexelFormat::RGBA8 && (int)S_LUM_ALPHA8 == (int)TexelFormat::LumAlpha8 && (int)S_RGBA16F == (int)TexelFormat::RGBA16F && (int)S_RGB10A2 == (int)TexelFormat::RGB10A2
              && (int)S_DEPTH == (int)TexelFormat::Depth && (int)S_BC7 == (int)TexelFormat::BC7 && (int)S_GENERIC_COMPRESSED == (int)TexelFormat::GenericCompressed
              && (int)S_BC3_SRGBA == (int)TexelFormat::BC3_SRGBA && (int)S_R8 == (int)TexelFormat::R8 && (int)S_R11G11B10F == (int)TexelFormat::R11G11B10F, "TexelFormat");
static_assert((int)C_R == (int)PixelFormat::R && (int)C_RGB == (int)PixelFormat::RGB && (int)C_RGBA == (int)PixelFormat::RGBA && (int)C_BGRA == (int)PixelFormat::BGRA
              && (int)C_LUM_ALPHA == (int)PixelFormat::LumAlpha && (int)C_ALPHA == (int)PixelFormat::Alpha && (int)C_DEPTH == (int)PixelFormat::Depth, "PixelFormat");

static_assert((int)DIM_2D == (int)TextureTarget::Tex2D && (int)DIM_CUBE == (int)TextureTarget::Cube && (int)DIM_CUBE_ARRAY == (int)TextureTarget::CubeArray && (int)DIM_2D_ARRAY == (int)TextureTarget::Array2D
              && (int)DIM_3D == (int)TextureTarget::Tex3D && (int)DIM_MS == (int)TextureTarget::Multisample2D && (int)DIM_OTHER == (int)TextureTarget::Rect, "TextureTarget");
static_assert((int)F_NEAREST == (int)TextureFilter::Nearest && (int)F_LINEAR_MIP_NEAREST == (int)TextureFilter::LinearMipNearest && (int)F_LINEAR_MIP_LINEAR == (int)TextureFilter::LinearMipLinear
              && (int)F_NEAREST_MIP_LINEAR == (int)TextureFilter::NearestMipLinear, "TextureFilter");
static_assert((int)W_REPEAT == (int)TextureWrap::Repeat && (int)W_CLAMP_EDGE == (int)TextureWrap::ClampToEdge && (int)W_MIRRORED_REPEAT == (int)TextureWrap::MirroredRepeat && (int)W_CLAMP_BORDER == (int)TextureWrap::ClampToBorder, "TextureWrap");
static_assert((int)CMP_NEVER == (int)LLRenderState::CompareOp::Never && (int)CMP_LESS == (int)LLRenderState::CompareOp::Less && (int)CMP_LEQUAL == (int)LLRenderState::CompareOp::LessEqual && (int)CMP_GEQUAL == (int)LLRenderState::CompareOp::GreaterEqual && (int)CMP_ALWAYS == (int)LLRenderState::CompareOp::Always, "LLRenderState::CompareOp");
static_assert((int)SW_R == (int)TextureSwizzle::R && (int)SW_A == (int)TextureSwizzle::A && (int)SW_ZERO == (int)TextureSwizzle::Zero && (int)SW_ONE == (int)TextureSwizzle::One, "TextureSwizzle");
static_assert((int)LQ_WIDTH == (int)LevelQuery::Width && (int)LQ_DEPTH == (int)LevelQuery::Depth && (int)LQ_COMPRESSED == (int)LevelQuery::Compressed && (int)LQ_COMPRESSED_SIZE == (int)LevelQuery::CompressedSize, "LevelQuery");

Topology topology(U32 gl)
{
    switch (gl)
    {
    case 0x0000: return TOP_POINTS;
    case 0x0001: return TOP_LINES;
    case 0x0003: return TOP_LINE_STRIP;
    case 0x0004: return TOP_TRIANGLES;
    case 0x0005: return TOP_TRIANGLE_STRIP;
    case 0x0006: return TOP_TRIANGLE_FAN;
    default: return TOP_OTHER;
    }
}

PType componentType(U32 gl)
{
    switch (gl)
    {
    case 0x1401: return T_U8;
    case 0x1400: return T_S8;
    case 0x1403: return T_U16;
    case 0x1402: return T_S16;
    case 0x1405: return T_U32;
    case 0x1404: return T_S32;
    case 0x1406: return T_F32;
    case 0x140B: return T_F16;
    default: return T_NONE;
    }
}

BufKind bufKind(U32 gl)
{
    switch (gl)
    {
    case 0x8892: return BK_VERTEX;      // ARRAY_BUFFER
    case 0x8893: return BK_INDEX;       // ELEMENT_ARRAY_BUFFER
    case 0x8A11: return BK_UNIFORM;     // UNIFORM_BUFFER
    default: return BK_OTHER;
    }
}

U32 compCount(U8 c)
{
    switch (c)
    {
    case C_RGBA: case C_BGRA: return 4;
    case C_RGB: case C_BGR: return 3;
    case C_RG: case C_LUM_ALPHA: return 2;
    case C_R: case C_LUM: case C_ALPHA: case C_DEPTH: return 1;
    default: return 0;
    }
}

U32 typeBytes(U8 t)
{
    switch (t)
    {
    case T_U8: case T_S8: return 1;
    case T_U16: case T_S16: case T_F16: return 2;
    case T_U32: case T_S32: case T_F32: return 4;
    default: return 0;
    }
}

Store store(S32 f)
{
    switch ((U32)f)
    {
    case 0x1908: case 0x8058: case 4: return S_RGBA8;                       // RGBA, RGBA8
    case 0x1907: case 0x8051: case 3: return S_RGB8;                        // RGB, RGB8
    case 0x8C43: case 0x8C42: return S_SRGBA8;                              // SRGB8_ALPHA8, SRGB_ALPHA
    case 0x8C41: case 0x8C40: return S_SRGB8;                               // SRGB8, SRGB
    case 0x1906: case 0x803C: return S_ALPHA8;                              // ALPHA, ALPHA8
    case 0x1909: case 0x8040: case 1: return S_LUM8;                        // LUMINANCE, LUMINANCE8
    case 0x190A: case 0x8045: case 2: return S_LUM_ALPHA8;                  // LUMINANCE_ALPHA, LUMINANCE8_ALPHA8
    case 0x822F: return S_RG16F;
    case 0x8230: return S_RG32F;
    case 0x822C: return S_RG16;
    case 0x8229: return S_R8;
    case 0x822B: return S_RG8;
    case 0x822E: return S_R32F;
    case 0x822D: return S_R16F;
    case 0x881A: case 0x881B: return S_RGBA16F;                             // RGBA16F, RGB16F
    case 0x8814: case 0x8815: return S_RGBA32F;                             // RGBA32F, RGB32F
    case 0x805B: return S_RGBA16;
    case 0x8C3A: return S_R11G11B10F;
    case 0x8059: return S_RGB10A2;
    case 0x1902: case 0x81A5: case 0x81A6: case 0x81A7: case 0x8CAC: case 0x88F0: return S_DEPTH;   // DEPTH_COMPONENT, 16, 24, 32, 32F, DEPTH24_STENCIL8
    case 0x83F0: return S_BC1_RGB;
    case 0x83F1: return S_BC1_RGBA;
    case 0x83F2: return S_BC2;
    case 0x83F3: return S_BC3;
    case 0x8E8C: return S_BC7;
    case 0x8C4D: return S_BC1_SRGBA;                                        // COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT
    case 0x8C4E: return S_BC2_SRGBA;
    case 0x8C4F: return S_BC3_SRGBA;
    case 0x84EE: case 0x84ED: return S_GENERIC_COMPRESSED;                  // COMPRESSED_RGBA, COMPRESSED_RGB
    default: return S_NONE;
    }
}

namespace
{
Filter filter_of(S32 v)
{
    switch ((U32)v)
    {
    case 0x2600: return F_NEAREST;
    case 0x2601: return F_LINEAR;
    case 0x2700: return F_NEAREST_MIP_NEAREST;
    case 0x2701: return F_LINEAR_MIP_NEAREST;
    case 0x2702: return F_NEAREST_MIP_LINEAR;
    default: return F_LINEAR_MIP_LINEAR;
    }
}

Wrap wrap_of(S32 v)
{
    switch ((U32)v)
    {
    case 0x812F: case 0x2900: return W_CLAMP_EDGE;      // CLAMP_TO_EDGE, CLAMP
    case 0x8370: return W_MIRRORED_REPEAT;
    case 0x812D: return W_CLAMP_BORDER;
    default: return W_REPEAT;
    }
}

Compare compare_of(S32 v)
{
    switch ((U32)v)
    {
    case 0x0200: return CMP_NEVER;
    case 0x0201: return CMP_LESS;
    case 0x0202: return CMP_EQUAL;
    case 0x0203: return CMP_LEQUAL;
    case 0x0204: return CMP_GREATER;
    case 0x0205: return CMP_NOTEQUAL;
    case 0x0206: return CMP_GEQUAL;
    default: return CMP_ALWAYS;
    }
}
}

Swizzle swizzle(S32 v)
{
    switch ((U32)v)
    {
    case 0x1903: return SW_R;       // RED
    case 0x1904: return SW_G;
    case 0x1905: return SW_B;
    case 0x1906: return SW_A;       // ALPHA
    case 0: return SW_ZERO;
    default: return SW_ONE;
    }
}

Setting setting(U32 pname, bool is_float, S32 i, F32 f)
{
    Setting s;
    if (pname == 0x84FE)            // TEXTURE_MAX_ANISOTROPY(_EXT)
    {
        s.mParam = P_ANISOTROPY;
        s.mFloat = is_float ? f : (F32)i;
        return s;
    }
    if (is_float)
    {
        i = (S32)f;
    }
    switch (pname)
    {
    case 0x2801: s.mParam = P_MIN_FILTER; s.mValue = filter_of(i); break;
    case 0x2800: s.mParam = P_MAG_FILTER; s.mValue = filter_of(i); break;
    case 0x2802: s.mParam = P_WRAP_S; s.mValue = wrap_of(i); break;
    case 0x2803: s.mParam = P_WRAP_T; s.mValue = wrap_of(i); break;
    case 0x8072: s.mParam = P_WRAP_R; s.mValue = wrap_of(i); break;
    case 0x884C: s.mParam = P_COMPARE_MODE; s.mValue = (U32)i == 0x884E ? 1 : 0; break;     // COMPARE_REF_TO_TEXTURE
    case 0x884D: s.mParam = P_COMPARE_FUNC; s.mValue = compare_of(i); break;
    case 0x813C: s.mParam = P_BASE_LEVEL; s.mInt = i; break;
    case 0x813D: s.mParam = P_MAX_LEVEL; s.mInt = i; break;
    case 0x8E42: s.mParam = P_SWIZZLE_R; s.mValue = swizzle(i); break;
    case 0x8E43: s.mParam = P_SWIZZLE_G; s.mValue = swizzle(i); break;
    case 0x8E44: s.mParam = P_SWIZZLE_B; s.mValue = swizzle(i); break;
    case 0x8E45: s.mParam = P_SWIZZLE_A; s.mValue = swizzle(i); break;
    case 0x8191: s.mParam = P_GENERATE_MIPMAP; s.mValue = i ? 1 : 0; break;                 // GENERATE_MIPMAP
    default: break;
    }
    return s;
}

// the plugin protocol (GL numbers on the wire, until F4 media)
}

TextureFilter textureFilterFromGLTF(S32 v)
{
    return (TextureFilter)LLVkFormat::setting(0x2801, false, v, 0.f).mValue;        // TEXTURE_MIN_FILTER: the same table for min and mag
}

TextureWrap textureWrapFromGLTF(S32 v)
{
    return (TextureWrap)LLVkFormat::setting(0x2802, false, v, 0.f).mValue;
}

TexelFormat texelFormatFromGL(S32 gl)
{
    return (TexelFormat)LLVkFormat::store(gl);
}

PixelFormat pixelFormatFromGL(U32 gl)
{
    return (PixelFormat)LLVkFormat::transfer(gl, 0x1401).mComps;
}

ComponentType componentTypeFromGL(U32 gl)
{
    // packed 8-8-8-8 types (the gstreamer plugin): the same bytes as 8-bit components on a little endian machine, and the plugin uses them with BGRA only
    if (gl == 0x8035 || gl == 0x8367)       // UNSIGNED_INT_8_8_8_8 (_REV)
    {
        return ComponentType::U8;
    }
    return (ComponentType)LLVkFormat::componentType(gl);
}

namespace LLVkFormat
{

// ---- F3.6e verify ----
namespace
{
const char* const kStoreNames[] = { "None", "RGBA8", "RGB8", "SRGBA8", "SRGB8", "Alpha8", "Lum8", "LumAlpha8", "RG16F", "RG32F", "RG16", "R8", "RG8", "R32F", "R16F", "RGBA16F", "RGBA32F", "RGBA16", "R11G11B10F", "RGB10A2",
                                    "Depth", "BC1_RGB", "BC1_RGBA", "BC2", "BC3", "BC7", "GenericCompressed", "BC1_SRGBA", "BC2_SRGBA", "BC3_SRGBA" };
const char* const kCompsNames[] = { "None", "R", "RG", "RGB", "BGR", "RGBA", "BGRA", "Lum", "LumAlpha", "Alpha", "Depth" };
const char* const kTypeNames[] = { "None", "U8", "S8", "U16", "S16", "U32", "S32", "F32", "F16" };
template <size_t N> const char* name_of(const char* const (&names)[N], U8 v) { return v < N ? names[v] : "?"; }
}

bool formatVerify()
{
    static const bool on = []() { const char* e = getenv("LL_VK_FORMAT_VERIFY"); return e && e[0] == '1'; }();
    return on;
}

void noteFormat(const char* what, const char* file, int line, U8 store, U8 comps, U8 type)
{
    if (!formatVerify())
    {
        return;
    }
    static const bool tables = (verifyTables(), true);      // the migration tables are checked with the first call
    (void)tables;
    static std::mutex mutex;
    static std::unordered_set<std::string> seen;
    const char* base = file;
    for (const char* c = file; *c; ++c)
    {
        if (*c == '/' || *c == '\\')
        {
            base = c + 1;
        }
    }
    std::string key = std::string(what) + " " + base + ":" + std::to_string(line) + " store=" + name_of(kStoreNames, store) + " comps=" + name_of(kCompsNames, comps) + " type=" + name_of(kTypeNames, type);
    std::lock_guard<std::mutex> lock(mutex);
    if (seen.insert(key).second)
    {
        LL_INFOS("FormatVerify") << key << LL_ENDL;
    }
}

void noteKey(const char* what, int a, int b, int c, int d, float f)
{
    if (!formatVerify())
    {
        return;
    }
    static std::mutex mutex;
    static std::unordered_set<std::string> seen;
    std::string key = std::string(what) + " " + std::to_string(a) + " " + std::to_string(b) + " " + std::to_string(c) + " " + std::to_string(d) + " " + std::to_string(f);
    std::lock_guard<std::mutex> lock(mutex);
    if (seen.insert(key).second)
    {
        LL_INFOS("FormatVerify") << "key " << key << LL_ENDL;
    }
}

// The migration tables: each GL number the old code used and the typed value that replaced it. Logs a mismatch with the pair named.
void verifyTables()
{
    if (!formatVerify())
    {
        return;
    }
    S32 bad = 0;
    auto texel = [&](const char* n, S32 gl, TexelFormat f) { if (store(gl) != store(f)) { ++bad; LL_WARNS("FormatVerify") << "texel table: " << n << " decodes to " << name_of(kStoreNames, store(gl)) << ", typed " << name_of(kStoreNames, (U8)f) << LL_ENDL; } };
    auto pixel = [&](const char* n, U32 gl, PixelFormat f) { if (transfer(gl, 0x1401).mComps != (U8)f) { ++bad; LL_WARNS("FormatVerify") << "pixel table: " << n << " decodes to " << name_of(kCompsNames, transfer(gl, 0x1401).mComps) << ", typed " << name_of(kCompsNames, (U8)f) << LL_ENDL; } };
    auto ctype = [&](const char* n, U32 gl, ComponentType t) { if (componentType(gl) != (U8)t) { ++bad; LL_WARNS("FormatVerify") << "type table: " << n << " decodes to " << name_of(kTypeNames, componentType(gl)) << ", typed " << name_of(kTypeNames, (U8)t) << LL_ENDL; } };
#define LLVK_TEXEL(gl, f) texel(#gl, gl, TexelFormat::f)
#define LLVK_PIXEL(gl, f) pixel(#gl, gl, PixelFormat::f)
#define LLVK_CTYPE(gl, f) ctype(#gl, gl, ComponentType::f)
    LLVK_TEXEL(GL_RGBA, RGBA8); LLVK_TEXEL(GL_RGBA8, RGBA8); LLVK_TEXEL(GL_RGB, RGB8); LLVK_TEXEL(GL_RGB8, RGB8); LLVK_TEXEL(GL_SRGB8_ALPHA8, SRGBA8); LLVK_TEXEL(GL_SRGB_ALPHA, SRGBA8);
    LLVK_TEXEL(GL_SRGB8, SRGB8); LLVK_TEXEL(GL_SRGB, SRGB8); LLVK_TEXEL(GL_ALPHA, Alpha8); LLVK_TEXEL(GL_ALPHA8, Alpha8); LLVK_TEXEL(GL_LUMINANCE, Lum8); LLVK_TEXEL(GL_LUMINANCE8, Lum8);
    LLVK_TEXEL(GL_LUMINANCE_ALPHA, LumAlpha8); LLVK_TEXEL(GL_LUMINANCE8_ALPHA8, LumAlpha8); LLVK_TEXEL(GL_RG16F, RG16F); LLVK_TEXEL(GL_RG32F, RG32F); LLVK_TEXEL(GL_R8, R8); LLVK_TEXEL(GL_RG8, RG8);
    LLVK_TEXEL(GL_R32F, R32F); LLVK_TEXEL(GL_R16F, R16F); LLVK_TEXEL(GL_RGBA16F, RGBA16F); LLVK_TEXEL(GL_RGB16F, RGBA16F); LLVK_TEXEL(GL_RGBA32F, RGBA32F); LLVK_TEXEL(GL_RGB32F, RGBA32F);
    LLVK_TEXEL(GL_RGBA16, RGBA16); LLVK_TEXEL(GL_R11F_G11F_B10F, R11G11B10F); LLVK_TEXEL(GL_RGB10_A2, RGB10A2); LLVK_TEXEL(GL_DEPTH_COMPONENT, Depth); LLVK_TEXEL(GL_DEPTH_COMPONENT24, Depth);
    LLVK_TEXEL(GL_DEPTH24_STENCIL8, Depth); LLVK_TEXEL(GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, BC1_RGBA); LLVK_TEXEL(GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, BC2); LLVK_TEXEL(GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, BC3);
    LLVK_TEXEL(GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT, BC1_SRGBA); LLVK_TEXEL(GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT, BC2_SRGBA); LLVK_TEXEL(GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT, BC3_SRGBA);
    LLVK_PIXEL(GL_RGBA, RGBA); LLVK_PIXEL(GL_RGB, RGB); LLVK_PIXEL(GL_BGRA, BGRA); LLVK_PIXEL(GL_RED, R); LLVK_PIXEL(GL_RG, RG); LLVK_PIXEL(GL_ALPHA, Alpha); LLVK_PIXEL(GL_LUMINANCE, Lum);
    LLVK_PIXEL(GL_LUMINANCE_ALPHA, LumAlpha); LLVK_PIXEL(GL_DEPTH_COMPONENT, Depth);
    LLVK_CTYPE(GL_UNSIGNED_BYTE, U8); LLVK_CTYPE(GL_BYTE, S8); LLVK_CTYPE(GL_UNSIGNED_SHORT, U16); LLVK_CTYPE(GL_SHORT, S16); LLVK_CTYPE(GL_UNSIGNED_INT, U32); LLVK_CTYPE(GL_INT, S32); LLVK_CTYPE(GL_FLOAT, F32);
    // F3.6f: targets, settings, swizzles, level queries (the GL tokens the old code used)
    auto targ = [&](const char* n, U32 gl, TextureTarget t) { const Target a = target(gl), b = target(t); if (a.mDim != b.mDim || a.mFace != b.mFace) { ++bad; LL_WARNS("FormatVerify") << "target table: " << n << " decodes to dim " << (S32)a.mDim << " face " << (S32)a.mFace << ", typed dim " << (S32)b.mDim << " face " << (S32)b.mFace << LL_ENDL; } };
    auto sett = [&](const char* n, U32 pname, S32 v, Setting typed) { const Setting a = setting(pname, false, v, 0.f); if (a.mParam != typed.mParam || a.mValue != typed.mValue || a.mInt != typed.mInt) { ++bad; LL_WARNS("FormatVerify") << "setting table: " << n << " decodes to param " << (S32)a.mParam << " value " << (S32)a.mValue << ", typed " << (S32)typed.mParam << " value " << (S32)typed.mValue << LL_ENDL; } };
    auto swz = [&](const char* n, S32 gl, TextureSwizzle t) { if (swizzle(gl) != (Swizzle)t) { ++bad; LL_WARNS("FormatVerify") << "swizzle table: " << n << LL_ENDL; } };
    auto lvl = [&](const char* n, U32 gl, LevelQuery q) { if (level(gl) != (Level)q) { ++bad; LL_WARNS("FormatVerify") << "level table: " << n << LL_ENDL; } };
#define LLVK_TARGET(gl, t) targ(#gl, gl, TextureTarget::t)
    LLVK_TARGET(GL_TEXTURE_2D, Tex2D); LLVK_TARGET(GL_TEXTURE_RECTANGLE, Rect); LLVK_TARGET(GL_TEXTURE_CUBE_MAP, Cube); LLVK_TARGET(GL_TEXTURE_CUBE_MAP_ARRAY, CubeArray); LLVK_TARGET(GL_TEXTURE_2D_MULTISAMPLE, Multisample2D);
    LLVK_TARGET(GL_TEXTURE_3D, Tex3D); LLVK_TARGET(GL_TEXTURE_CUBE_MAP_POSITIVE_X, CubeFacePX); LLVK_TARGET(GL_TEXTURE_CUBE_MAP_NEGATIVE_X, CubeFaceNX); LLVK_TARGET(GL_TEXTURE_CUBE_MAP_POSITIVE_Y, CubeFacePY);
    LLVK_TARGET(GL_TEXTURE_CUBE_MAP_NEGATIVE_Y, CubeFaceNY); LLVK_TARGET(GL_TEXTURE_CUBE_MAP_POSITIVE_Z, CubeFacePZ); LLVK_TARGET(GL_TEXTURE_CUBE_MAP_NEGATIVE_Z, CubeFaceNZ);
#undef LLVK_TARGET
    sett("MIN nearest", GL_TEXTURE_MIN_FILTER, GL_NEAREST, setMinFilter(TextureFilter::Nearest)); sett("MIN linear", GL_TEXTURE_MIN_FILTER, GL_LINEAR, setMinFilter(TextureFilter::Linear));
    sett("MIN n_m_n", GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST, setMinFilter(TextureFilter::NearestMipNearest)); sett("MIN l_m_n", GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST, setMinFilter(TextureFilter::LinearMipNearest));
    sett("MIN l_m_l", GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR, setMinFilter(TextureFilter::LinearMipLinear));
    sett("MAG nearest", GL_TEXTURE_MAG_FILTER, GL_NEAREST, setMagFilter(TextureFilter::Nearest)); sett("MAG linear", GL_TEXTURE_MAG_FILTER, GL_LINEAR, setMagFilter(TextureFilter::Linear));
    sett("WRAP_S repeat", GL_TEXTURE_WRAP_S, GL_REPEAT, setWrapS(TextureWrap::Repeat)); sett("WRAP_S mirror", GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT, setWrapS(TextureWrap::MirroredRepeat));
    sett("WRAP_S clamp", GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE, setWrapS(TextureWrap::ClampToEdge)); sett("WRAP_T repeat", GL_TEXTURE_WRAP_T, GL_REPEAT, setWrapT(TextureWrap::Repeat));
    sett("WRAP_T mirror", GL_TEXTURE_WRAP_T, GL_MIRRORED_REPEAT, setWrapT(TextureWrap::MirroredRepeat)); sett("WRAP_T clamp", GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE, setWrapT(TextureWrap::ClampToEdge));
    sett("WRAP_R repeat", GL_TEXTURE_WRAP_R, GL_REPEAT, setWrapR(TextureWrap::Repeat)); sett("WRAP_R mirror", GL_TEXTURE_WRAP_R, GL_MIRRORED_REPEAT, setWrapR(TextureWrap::MirroredRepeat));
    sett("WRAP_R clamp", GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE, setWrapR(TextureWrap::ClampToEdge));
    sett("COMPARE_MODE on", GL_TEXTURE_COMPARE_MODE, GL_COMPARE_R_TO_TEXTURE, setCompareMode(true)); sett("COMPARE_MODE off", GL_TEXTURE_COMPARE_MODE, GL_NONE, setCompareMode(false));
    sett("COMPARE_FUNC lequal", GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL, setCompareFunc(LLRenderState::CompareOp::LessEqual));
    sett("BASE_LEVEL", GL_TEXTURE_BASE_LEVEL, 3, setBaseLevel(3)); sett("MAX_LEVEL", GL_TEXTURE_MAX_LEVEL, 5, setMaxLevel(5));
    swz("ZERO", GL_ZERO, TextureSwizzle::Zero); swz("ONE", GL_ONE, TextureSwizzle::One); swz("RED", GL_RED, TextureSwizzle::R); swz("GREEN", GL_GREEN, TextureSwizzle::G);
    lvl("WIDTH", GL_TEXTURE_WIDTH, LevelQuery::Width); lvl("HEIGHT", GL_TEXTURE_HEIGHT, LevelQuery::Height); lvl("COMPRESSED", GL_TEXTURE_COMPRESSED, LevelQuery::Compressed);
    lvl("COMPRESSED_IMAGE_SIZE", GL_TEXTURE_COMPRESSED_IMAGE_SIZE, LevelQuery::CompressedSize);
#undef LLVK_TEXEL
#undef LLVK_PIXEL
#undef LLVK_CTYPE
    LL_INFOS("FormatVerify") << "migration tables checked: " << (bad ? "MISMATCHES " : "all agree, ") << bad << " mismatches" << LL_ENDL;
}

Level level(U32 pname)
{
    switch (pname)
    {
    case 0x1000: return LQ_WIDTH;
    case 0x1001: return LQ_HEIGHT;
    case 0x8071: return LQ_DEPTH;
    case 0x86A1: return LQ_COMPRESSED;
    case 0x86A0: return LQ_COMPRESSED_SIZE;
    default: return LQ_NONE;
    }
}

}

#endif // LL_VULKAN
