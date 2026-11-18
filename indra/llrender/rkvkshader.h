/**
 * @file rkvkshader.h
 * @brief Turns the viewer's OpenGL-flavoured GLSL programs into Vulkan SPIR-V.
 *
 * The viewer's shaders are written for OpenGL: loose "uniform" variables, samplers whose unit is set at
 * run time, varyings and outputs without explicit locations, and GL clip-space depth. Vulkan GLSL has none of
 * that, so each program (the set of stage objects the viewer links together) is preprocessed with glslang,
 * rewritten, linked with glslang and emitted as SPIR-V:
 *
 *   set 0, binding 0        the std140 block of the loose uniforms of the 'pass' class; the other classes (draw, material, light, skin) have blocks of their own at
 *                           bindings 5-8 (UC_BINDING), each copied to the uniform ring only when one of its values changed (F2.3)
 *   set 0, binding 1 + n    the viewer's named uniform blocks (n = index in the viewer's block list)
 *   set 1                   empty (the number is kept: sets 2 and 3 follow)
 *   set 2, binding 0-3      the tables of sampled images (2D, cube, cube array, 2D shadow); binding 4: the handle map. A sampler is a macro for a table element read with the id
 *                           in a loose uint uniform (ll_ti_<name>) through the map
 *   vertex inputs           location = index of the attribute in the viewer's reserved attribute list
 *   varyings                location by name, assigned in order of first appearance in the vertex stage
 *   fragment outputs        locations in order of declaration
 *   vertex main             wrapped so gl_Position.z is remapped from GL [-w,w] to Vulkan [0,w]
 *
 * Compiled only with -DUSE_VULKAN=ON.
 */

#ifndef LL_LLVKSHADER_H
#define LL_LLVKSHADER_H

#if defined(LL_VULKAN)

#include "linden_common.h"

#include <string>
#include <vector>

namespace LLVk
{

// How often a loose uniform changes (doc/vulkan/uniform-classes.tsv): each class is a block of its own, copied independently
enum UniformClass : U8 { UC_PASS = 0, UC_DRAW, UC_MATERIAL, UC_LIGHT, UC_SKIN, UC_COUNT };
constexpr U32 UC_BINDING[UC_COUNT] = { 0, 5, 6, 7, 8 };         // set 0 binding of each class's block (dynamic uniform buffers, in this order)

struct LooseUniform
{
    std::string mName;
    std::string mType;          // GLSL type name
    U32 mArraySize = 0;         // 0: not an array
    U32 mOffset = 0;            // byte offset in the std140 block of its class
    U8 mClass = UC_PASS;        // UniformClass
    U32 mSize = 0;              // bytes occupied (arrays include their padding)
    std::string mInit;          // initializer text from the shader, if any
};

struct SamplerInfo
{
    std::string mName;
    std::string mType;
    U32 mBinding = 0;
    S32 mIndexUniform = -1;     // bindless programs: index in mUniforms of the uint that holds this sampler's table slot (ll_ti_<name>)
};

struct ProgramInfo
{
    std::vector<LooseUniform> mUniforms;
    U32 mBlockSizes[UC_COUNT] = {};             // bytes of each class's block (0: the program has no uniform of that class)
    std::vector<SamplerInfo> mSamplers;
    bool mBindless = false;                     // the program has samplers (all translated to texture handles): the draw writes the id of each into the loose uniform block
    bool mUsesRt = false;                       // traces rays against the frame's scene (set 3, binding 0: ll_rt_tlas): the draw binds the ray tracing descriptor set
    std::vector<std::string> mAttributes;       // vertex inputs the program declares
    std::vector<U32> mAttributeLocations;       // their locations (index in the viewer's reserved attribute list)
    std::vector<std::string> mAttributeTypes;   // their GLSL types ("vec4", "int", ...)
    std::vector<std::string> mOutputs;          // fragment outputs
    std::vector<U32> mSpirv[3];                 // 0 vertex, 1 fragment, 2 geometry
    std::vector<std::pair<U32, U32>> mSpec;     // native programs: specialization constants (constant_id, value), applied to every stage when a pipeline is made
    std::string mError;
};

// LL_VK_DEBUG_SHADERS (environment): the draw state overrides of <work>/vk-debug.txt (rkvkworker.cpp)
inline bool debugShadersEnabled()
{
    static const bool enabled = getenv("LL_VK_DEBUG_SHADERS") != nullptr;
    return enabled;
}

} // namespace LLVk

#endif // LL_VULKAN

#endif // LL_LLVKSHADER_H
