/**
 * @file llvkshader.h
 * @brief Turns the viewer's OpenGL-flavoured GLSL programs into Vulkan SPIR-V.
 *
 * The viewer's shaders are written for OpenGL: loose "uniform" variables, samplers whose unit is set at
 * run time, varyings and outputs without explicit locations, and GL clip-space depth. Vulkan GLSL has none of
 * that, so each program (the set of stage objects the viewer links together) is preprocessed with glslang,
 * rewritten, linked with glslang and emitted as SPIR-V:
 *
 *   set 0, binding 0        one std140 block holding every loose uniform of the program (laid out here)
 *   set 0, binding 1 + n    the viewer's named uniform blocks (n = index in the viewer's block list)
 *   set 1, binding k        samplers, in order of first appearance (push descriptors)
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

struct StageSource
{
    enum Stage { Vertex = 0, Fragment = 1, Geometry = 2 };
    Stage mStage = Vertex;
    std::string mName;      // file name, for messages
    std::string mSource;    // the complete GLSL text the viewer would hand to glShaderSource
};

struct ShaderEnv
{
    std::vector<std::string> mAttributeNames;       // vertex attribute locations, by index
    std::vector<std::string> mUniformBlockNames;    // named uniform blocks, binding = 1 + index
};

struct LooseUniform
{
    std::string mName;
    std::string mType;          // GLSL type name
    U32 mArraySize = 0;         // 0: not an array
    U32 mOffset = 0;            // byte offset in the std140 block
    U32 mSize = 0;              // bytes occupied (arrays include their padding)
    std::string mInit;          // initializer text from the shader, if any
};

struct SamplerInfo
{
    std::string mName;
    std::string mType;
    U32 mBinding = 0;
};

struct ProgramInfo
{
    std::vector<LooseUniform> mUniforms;
    U32 mBlockSize = 0;
    std::vector<SamplerInfo> mSamplers;
    std::vector<std::string> mAttributes;       // vertex inputs the program declares
    std::vector<U32> mAttributeLocations;       // their locations (index in the viewer's reserved attribute list)
    std::vector<std::string> mAttributeTypes;   // their GLSL types ("vec4", "int", ...)
    std::vector<std::string> mOutputs;          // fragment outputs
    std::vector<U32> mSpirv[3];                 // indexed by StageSource::Stage
    std::string mError;
};

// Compile one program. On failure returns false and mError says why. If dump_dir is not empty the
// rewritten source of a failing stage is written there for inspection.
bool compileProgram(const std::string& program_name, const std::vector<StageSource>& stages, const ShaderEnv& env,
                    ProgramInfo& out, const std::string& dump_dir);

// debug: the sampler2D names (in the order the generated debug code numbers them) of a program compiled with the fragment debug wrapper
const std::vector<std::string>* findDebugSamplers(const std::string& program_name);
bool debugShadersEnabled();

// Programs compiled by the check path are remembered by name so Vulkan code can use the viewer's real shaders.
void registerCompiledProgram(const std::string& name, const ProgramInfo& info);
const ProgramInfo* findCompiledProgram(const std::string& name);

} // namespace LLVk

#endif // LL_VULKAN

#endif // LL_LLVKSHADER_H
