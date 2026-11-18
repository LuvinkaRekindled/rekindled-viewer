/**
 * @file rkrenderformat.h
 * @brief F3.6: the typed vocabulary that replaces the GL constants in the viewer's code (no GL, no Vulkan in this header).
 *
 * The viewer states what it means with a typed value (a shader stage, a clear mask, a buffer kind, a pixel format ...); the numbers of OpenGL appear only
 * where such a value is decoded (rkvkformat.cpp and the rkvk* internals that F5 deletes). The values of the enumerators are NOT GL numbers.
 * Design and steps: doc/vulkan/F3-6-gltypes-design.md.
 */

#ifndef LL_LLRENDERFORMAT_H
#define LL_LLRENDERFORMAT_H

#include "stdtypes.h"

// F3.6b: the stage a shader source is compiled for (was the GL shader type numbers: vertex, fragment, geometry)
enum class ShaderStage : U8 { Vertex = 1, Fragment = 2, Geometry = 3 };

// F3.6c: what a clear clears (was GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT; the values are not GL numbers)
enum class ClearBits : U32 { None = 0, Color = 1, Depth = 2, Stencil = 4, All = 7 };
constexpr ClearBits operator|(ClearBits a, ClearBits b) { return (ClearBits)((U32)a | (U32)b); }
constexpr ClearBits operator&(ClearBits a, ClearBits b) { return (ClearBits)((U32)a & (U32)b); }
constexpr bool hasBits(ClearBits mask, ClearBits bits) { return ((U32)mask & (U32)bits) != 0; }

// F3.6d: buffers, vertex data and draws. The values equal LLVkFormat::BufKind / PType / Topology (the producer's records carry them as they are).
enum class BufferKind : U8 { Vertex = 0, Index = 1, Uniform = 2 };
enum class ComponentType : U8 { None = 0, U8, S8, U16, S16, U32, S32, F32, F16 };        // vertex attribute and index component types
enum class PrimitiveMode : U8 { Points = 0, Lines, LineStrip, Triangles, TriangleStrip, TriangleFan, Other };

// F3.6e: texture data. The values equal LLVkFormat::Store / Comps / PType (static_asserts in rkvkformat.cpp), so the producer's records carry them as they are.
// TexelFormat: how a texture STORES its texels (was the GL internal format). One entry per distinct treatment: the quirks of the old decode are visible here,
// a caller says what it gets (GL_RGB16F was stored as RGBA16F: write RGBA16F; GL_RGB / GL_RGB8 were stored as RGB8; every depth format is Depth).
enum class TexelFormat : U8
{
    None = 0,
    RGBA8, RGB8, SRGBA8, SRGB8, Alpha8, Lum8, LumAlpha8,
    RG16F, RG32F, RG16, R8, RG8, R32F, R16F, RGBA16F, RGBA32F, RGBA16, R11G11B10F, RGB10A2,
    Depth,
    BC1_RGB, BC1_RGBA, BC2, BC3, BC7,
    GenericCompressed,
    BC1_SRGBA, BC2_SRGBA, BC3_SRGBA
};
// PixelFormat: the component layout of client pixel data (was the GL format: GL_RGBA, GL_RED ...). ComponentType (F3.6d) is its component type.
enum class PixelFormat : U8 { None = 0, R, RG, RGB, BGR, RGBA, BGRA, Lum, LumAlpha, Alpha, Depth };

// F3.6f: texture targets, sampler settings, level queries. Values equal LLVkFormat::Dim (+ 8 + face for a cube map face) / Filter / Wrap / Swizzle / Level (static_asserts in rkvkformat.cpp).
// The compare function of a depth texture is CompareOp (rkrenderstate.h).
enum class TextureTarget : U8
{
    Tex2D = 0, Cube = 1, CubeArray = 2, Array2D = 3, Tex3D = 4, Multisample2D = 5, Rect = 7,
    CubeFacePX = 8, CubeFaceNX, CubeFacePY, CubeFaceNY, CubeFacePZ, CubeFaceNZ
};
enum class TextureFilter : U8 { Nearest = 0, Linear, NearestMipNearest, LinearMipNearest, NearestMipLinear, LinearMipLinear };
enum class TextureWrap : U8 { Repeat = 0, ClampToEdge, MirroredRepeat, ClampToBorder };
enum class TextureSwizzle : U8 { R = 0, G, B, A, Zero, One };
enum class LevelQuery : U8 { None = 0, Width, Height, Depth, Compressed, CompressedSize };

// glTF files carry GL numbers for their sampler settings (they are file format values): decoded in rkvkformat.cpp
TextureFilter textureFilterFromGLTF(S32 value);
TextureWrap textureWrapFromGLTF(S32 value);

// The media plugin protocol still carries GL numbers on the wire (llpluginclassmedia, media_plugins: separate processes): these decode them (rkvkformat.cpp). F4 (media) replaces the protocol values.
TexelFormat texelFormatFromGL(S32 gl_internal_format);
PixelFormat pixelFormatFromGL(U32 gl_format);
ComponentType componentTypeFromGL(U32 gl_type);

#endif
