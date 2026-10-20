/**
 * @file llrenderformat.h
 * @brief F3.6: the typed vocabulary that replaces the GL constants in the viewer's code (no GL, no Vulkan in this header).
 *
 * The viewer states what it means with a typed value (a shader stage, a clear mask, a buffer kind, a pixel format ...); the numbers of OpenGL appear only
 * where such a value is decoded (llvkformat.cpp and the llvk* internals that F5 deletes). The values of the enumerators are NOT GL numbers.
 * Design and steps: doc/vulkan/F3-6-gltypes-design.md.
 */

#ifndef LL_LLRENDERFORMAT_H
#define LL_LLRENDERFORMAT_H

#include "stdtypes.h"

// F3.6b: the stage a shader source is compiled for (was the GL shader type numbers: vertex, fragment, geometry)
enum class ShaderStage : U8 { Vertex = 1, Fragment = 2, Geometry = 3 };

#endif
