/**
 * @file llvknative.h
 * @brief The native shader path (doc/vulkan/PLAN-shaders-rewrite.md, slice 0): shaders written for Vulkan from scratch, next to the viewer's translated GL shaders.
 *
 * A native program is plain Vulkan GLSL 460 with explicit layouts. The loader prepends one prelude (native/ll_native.glsl: the bindless texture tables and the handle map), resolves
 * `#include "lib/x.glsl"` from the native directory, compiles with glslang and builds the same ProgramInfo the renderer already draws with. The contract:
 *
 *   set 0, binding 0   `layout(std140, set = 0, binding = 0) uniform LLLooseUniforms { ... };`   per pass values (the renderer's uniform class UC_PASS)
 *   set 0, binding 5   `layout(std140, set = 0, binding = 5) uniform LLLooseDraw { ... };`       per draw values (UC_DRAW); a member `uint ll_ti_<name>` is the texture slot of a sampler `<name>`
 *                                                                                                 (`ll_ti_cube_<name>`: a cube map). Bindings 6-8: LLLooseMaterial / LLLooseLight / LLLooseSkin.
 *   set 2              the bindless tables (ll_tbl2d, ll_tblcube, ll_tblcubearray, ll_tbl2dshadow) and the handle map: `texture(ll_tbl2d[LL_HM(ll_ti_name)], uv)`
 *   vertex inputs      explicit `layout(location = n)`, n = index in the viewer's reserved attribute list (position 0)
 *   outputs            explicit locations
 *   depth              Vulkan clip space (z in [0, w]); no GL remap
 *
 * Which old program a native one stands in for, and the switch that turns it on, are in native/manifest.txt:
 *     override | <old program name> | <vertex file> | <fragment file> | <family> [| define NAME=VALUE | spec ID=VALUE ...]
 * The family is a live key of rt-debug.txt (native_<family>=1; 0 or missing: the old program). The renderer swaps the program at the draw (nativeOverride), the old program's uniform values and sampler units
 * are copied by name into the native one, so the callers do not change. LL_VK_NATIVE_DIR points the loader at the source directory (edit a shader, save: it is recompiled within a second).
 */

#ifndef LL_LLVKNATIVE_H
#define LL_LLVKNATIVE_H

#if defined(LL_VULKAN)

#include "llvkshader.h"

namespace LLVk
{
class Program;

// Compile one native program from the files under 'root' (vertex or fragment may be empty for a one stage program). 'defines' become `#define NAME VALUE` before the prelude. Fills 'out'
// (SPIR-V, the loose uniform blocks as reflected, the texture slots, the vertex inputs and the outputs). 'files' lists every file read (the shader files and their includes) for hot reload.
bool compileNativeProgram(const std::string& name, const std::string& root, const std::string& vertex_file, const std::string& fragment_file,
                          const std::vector<std::pair<std::string, std::string>>& defines, ProgramInfo& out, std::vector<std::string>* files, std::string* error);

// The directory the native shaders are read from (LL_VK_NATIVE_DIR, else app_settings/shaders/native next to the executable).
const std::string& nativeRoot();

// A live key of rt-debug.txt (read at most twice a second; any thread): the value of `key=N`, or 'def' when the line is missing.
int nativeKey(const std::string& key, int def);

// The program to draw with in place of 'p' (p itself when no native program stands in for it or its family is off). Called on the worker thread for every draw.
Program* nativeOverride(Program* p);

} // namespace LLVk

#endif // LL_VULKAN

#endif // LL_LLVKNATIVE_H
