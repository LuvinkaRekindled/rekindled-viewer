/**
 * @file llshadermgr.cpp
 * @brief Shader manager implementation.
 *
 * $LicenseInfo:firstyear=2005&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "linden_common.h"
#include "llshadermgr.h"
#include "llvkbridge.h"
#include "llrender.h"
#include "llfile.h"
#include "lldir.h"
#include "llsdutil.h"
#include "llsdserialize.h"
#include "hbxxh.h"

// H6: the pull shaders have exactly the text they had before the texture need was measured (llFbTex is the plain table sample, no llFbNote) unless LL_VK_TEXFB is set to something but 0
// (the measurement costs about a quarter of the frame rate: stores in the shaders cost them the early depth test; see doc/vulkan/PLAN-texture-handles.md section 17)
static bool fbPlainShaders()
{
    static const bool plain = []{ const char* e = getenv("LL_VK_TEXFB"); return !e || atoi(e) == 0 || getenv("LL_VK_FB_OFF"); }();
    return plain;
}

#if LL_DARWIN
#include "OpenGL/OpenGL.h"
#endif

// Lots of STL stuff in here, using namespace std to keep things more readable
using std::vector;
using std::pair;
using std::make_pair;
using std::string;

LLShaderMgr * LLShaderMgr::sInstance = NULL;

LLShaderMgr::LLShaderMgr()
{
}


LLShaderMgr::~LLShaderMgr()
{
}

// static
LLShaderMgr * LLShaderMgr::instance()
{
    if(NULL == sInstance)
    {
        LL_ERRS("Shaders") << "LLShaderMgr should already have been instantiated by the application!" << LL_ENDL;
    }

    return sInstance;
}

bool LLShaderMgr::attachShaderFeatures(LLShaderProgram * shader)
{
    llassert_always(shader != NULL);
    LLShaderFeatures *features = & shader->mFeatures;

    if (features->attachNothing)
    {
        return true;
    }
    //////////////////////////////////////
    // Attach Vertex Shader Features First
    //////////////////////////////////////

    // NOTE order of shader object attaching is VERY IMPORTANT!!!
    if (features->calculatesAtmospherics || features->hasGamma || features->isDeferred)
    {
        if (!shader->attachVertexObject("windlight/atmosphericsVarsV.glsl"))
        {
            return false;
        }
    }

    if (features->calculatesLighting || features->calculatesAtmospherics)
    {
        if (!shader->attachVertexObject("windlight/atmosphericsHelpersV.glsl"))
        {
            return false;
        }
    }

    if (features->calculatesLighting)
    {
        if (features->isSpecular)
        {
            if (!shader->attachVertexObject("lighting/lightFuncSpecularV.glsl"))
            {
                return false;
            }

            if (!features->isAlphaLighting)
            {
                if (!shader->attachVertexObject("lighting/sumLightsSpecularV.glsl"))
                {
                    return false;
                }
            }

            if (!shader->attachVertexObject("lighting/lightSpecularV.glsl"))
            {
                return false;
            }
        }
        else
        {
            if (!shader->attachVertexObject("lighting/lightFuncV.glsl"))
            {
                return false;
            }

            if (!features->isAlphaLighting)
            {
                if (!shader->attachVertexObject("lighting/sumLightsV.glsl"))
                {
                    return false;
                }
            }

            if (!shader->attachVertexObject("lighting/lightV.glsl"))
            {
                return false;
            }
        }
    }

    // NOTE order of shader object attaching is VERY IMPORTANT!!!
    if (features->calculatesAtmospherics)
    {
        if (!shader->attachVertexObject("environment/srgbF.glsl")) // NOTE -- "F" suffix is superfluous here, there is nothing fragment specific in srgbF
        {
            return false;
        }

        if (!shader->attachVertexObject("windlight/atmosphericsFuncs.glsl")) {
            return false;
        }

        if (!shader->attachVertexObject("windlight/atmosphericsV.glsl"))
        {
            return false;
        }
    }

    if (features->hasSkinning)
    {
        if (!shader->attachVertexObject("avatar/avatarSkinV.glsl"))
        {
            return false;
        }
    }

    if (features->hasObjectSkinning)
    {
        shader->mRiggedVariant = shader;
        if (!shader->attachVertexObject("avatar/objectSkinV.glsl"))
        {
            return false;
        }
    }

    if (!shader->attachVertexObject("deferred/textureUtilV.glsl"))
    {
        return false;
    }

    ///////////////////////////////////////
    // Attach Fragment Shader Features Next
    ///////////////////////////////////////

    // NOTE order of shader object attaching is VERY IMPORTANT!!!

    if (!shader->attachFragmentObject("deferred/globalF.glsl"))
    {
        return false;
    }

    if (features->hasSrgb || features->hasAtmospherics || features->calculatesAtmospherics || features->isDeferred)
    {
        if (!shader->attachFragmentObject("environment/srgbF.glsl"))
        {
            return false;
        }
    }

    if(features->calculatesAtmospherics || features->hasGamma || features->isDeferred)
    {
        if (!shader->attachFragmentObject("windlight/atmosphericsVarsF.glsl"))
        {
            return false;
        }
    }

    if (features->calculatesLighting || features->calculatesAtmospherics)
    {
        if (!shader->attachFragmentObject("windlight/atmosphericsHelpersF.glsl"))
        {
            return false;
        }
    }

    // we want this BEFORE shadows and AO because those facilities use pos/norm access
    if (features->isDeferred || features->hasReflectionProbes)
    {
        if (!shader->attachFragmentObject("deferred/deferredUtil.glsl"))
        {
            return false;
        }
    }

    if (features->hasFullGBuffer)
    {
        if (!shader->attachFragmentObject("deferred/gbufferUtil.glsl"))
        {
            return false;
        }
    }

    if (features->hasScreenSpaceReflections || features->hasReflectionProbes)
    {
        if (!shader->attachFragmentObject("deferred/screenSpaceReflUtil.glsl"))
        {
            return false;
        }
    }

    if (features->hasShadows)
    {
        if (!shader->attachFragmentObject("deferred/shadowUtil.glsl"))
        {
            return false;
        }
    }

    if (features->hasReflectionProbes)
    {
        if (!shader->attachFragmentObject("deferred/reflectionProbeF.glsl"))
        {
            return false;
        }
    }

    if (features->hasAmbientOcclusion)
    {
        if (!shader->attachFragmentObject("deferred/aoUtil.glsl"))
        {
            return false;
        }
    }

    if (features->hasGamma || features->isDeferred)
    {
        if (!shader->attachFragmentObject("windlight/gammaF.glsl"))
        {
            return false;
        }
    }

    if (features->hasAtmospherics || features->isDeferred)
    {
        if (!shader->attachFragmentObject("windlight/atmosphericsFuncs.glsl")) {
            return false;
        }

        if (!shader->attachFragmentObject("windlight/atmosphericsF.glsl"))
        {
            return false;
        }
    }

    if (features->isPBRTerrain)
    {
        if (!shader->attachFragmentObject("deferred/pbrterrainUtilF.glsl"))
        {
            return false;
        }
    }

    if (features->hasTonemap)
    {
        if (!shader->attachFragmentObject("deferred/tonemapUtilF.glsl"))
        {
            return false;
        }
    }

    // NOTE order of shader object attaching is VERY IMPORTANT!!!
    if (features->hasAtmospherics)
    {
        if (!shader->attachFragmentObject("environment/waterFogF.glsl"))
        {
            return false;
        }
    }

    if (features->hasLighting)
    {
        if (features->mIndexedTextureChannels <= 1)
        {
            if (features->hasAlphaMask)
            {
                if (!shader->attachFragmentObject("lighting/lightAlphaMaskNonIndexedF.glsl"))
                {
                    return false;
                }
            }
            else
            {
                if (!shader->attachFragmentObject("lighting/lightNonIndexedF.glsl"))
                {
                    return false;
                }
            }
        }
        else
        {
            if (features->hasAlphaMask)
            {
                if (!shader->attachFragmentObject("lighting/lightAlphaMaskF.glsl"))
                {
                    return false;
                }
            }
            else
            {
                if (!shader->attachFragmentObject("lighting/lightF.glsl"))
                {
                    return false;
                }
            }
            shader->mFeatures.mIndexedTextureChannels = llmax(LLShaderProgram::sIndexedTextureChannels, 1);
        }
    }

    if (features->mIndexedTextureChannels <= 1)
    {
        if (!shader->attachVertexObject("objects/nonindexedTextureV.glsl"))
        {
            return false;
        }
    }
    else
    {
        if (!shader->attachVertexObject("objects/indexedTextureV.glsl"))
        {
            return false;
        }
    }

    return true;
}

//============================================================================
// Load Shader

// <FS> S6.6 LL_VERTEX_PULL: a vertex shader of the viewer becomes a pull shader (the geometry packet path: the attributes are read by address from the draw's vertex buffer, the matrices from the
// record's matrix table). The shader text is rewritten line by line: its `in` attributes become globals, the matrix uniforms the record carries (modelview_matrix, modelview_projection_matrix,
// normal_matrix, texture_matrix0) become globals, and the start of main() fills them from the draw's row. Everything else of the shader stays as it is.
// <FS> texture animation on the GPU (llvktexanim.h has the layout and the CPU twin of this function, line for line): the matrix of a face whose slot holds animation parameters.
// m is the 16 floats as stored (GLSL m[i / 4][i % 4] = float i), clk the clock (whole seconds, the rest). Injected once into every main vertex shader that reads texture matrices.
static const char* const kTexAnimCore =
    "mat4 ll_texanim_core(mat4 m, vec2 clk)\n"
    "{\n"
    "    uint bits = floatBitsToUint(m[0][0]);\n"
    "    uint mode = bits & 0xFFu;\n"
    "    float size_x = float((bits >> 8) & 0xFFu), size_y = float((bits >> 16) & 0xFFu);\n"
    "    float rate = m[0][1], start = m[0][2], len = m[0][3];\n"
    "    float elapsed = max((clk.x - m[1][0]) + (clk.y - m[1][1]), 0.0);\n"
    "    float num_frames = len != 0.0 ? len : max(1.0, size_x * size_y);\n"
    "    float full_length = num_frames;\n"
    "    if ((mode & 8u) != 0u)\n"
    "    {\n"
    "        if ((mode & 16u) != 0u) full_length = 2.0 * num_frames;\n"
    "        else if ((mode & 2u) != 0u) full_length = max(1.0, 2.0 * num_frames - 2.0);\n"
    "        else full_length = max(1.0, 2.0 * num_frames - 1.0);\n"
    "    }\n"
    "    float fc = elapsed * rate;\n"
    "    if ((mode & 2u) != 0u) fc = fc - full_length * trunc(fc / full_length);\n"
    "    else fc = min(full_length - 1.0, fc);\n"
    "    if ((mode & 16u) == 0u)\n"
    "    {\n"
    "        fc = floor(fc + 0.01);\n"
    "        fc = min(full_length - 1.0, fc);\n"
    "    }\n"
    "    if ((mode & 8u) != 0u && fc >= num_frames)\n"
    "    {\n"
    "        fc = ((mode & 16u) != 0u) ? num_frames - (fc - num_frames) : (num_frames - 1.99) - (fc - num_frames);\n"
    "    }\n"
    "    if ((mode & 4u) != 0u)\n"
    "    {\n"
    "        fc = ((mode & 16u) != 0u) ? num_frames - fc : (num_frames - 0.99) - fc;\n"
    "    }\n"
    "    fc += start;\n"
    "    if ((mode & 16u) == 0u) fc = floor(fc + 0.5);\n"
    "    float off_s = m[1][2], off_t = m[1][3], scale_s = m[2][0], scale_t = m[2][1], rot = m[2][2];\n"
    "    if ((mode & 32u) != 0u)\n"
    "    {\n"
    "        rot = fc;\n"
    "    }\n"
    "    else if ((mode & 64u) != 0u)\n"
    "    {\n"
    "        scale_s = fc;\n"
    "        scale_t = fc;\n"
    "    }\n"
    "    else if (size_x != 0.0 && size_y != 0.0)\n"
    "    {\n"
    "        scale_s = 1.0 / size_x;\n"
    "        scale_t = 1.0 / size_y;\n"
    "        float x_frame = fc - size_x * trunc(fc / size_x);\n"
    "        float y_frame = float(int(fc / size_x));\n"
    "        off_s = (-0.5 + 0.5 * scale_s) + x_frame * scale_s;\n"
    "        off_t = (0.5 - 0.5 * scale_t) - y_frame * scale_t;\n"
    "    }\n"
    "    else\n"
    "    {\n"
    "        off_s = fc;\n"
    "        off_t = 0.0;\n"
    "    }\n"
    "    float c = cos(rot), s = sin(rot) * m[2][3];\n"
    "    float m00 = c, m01 = -s, m10 = s, m11 = c;\n"
    "    return mat4(vec4(m00 * scale_s, m01 * scale_t, 0.0, 0.0),\n"
    "                vec4(m10 * scale_s, m11 * scale_t, 0.0, 0.0),\n"
    "                vec4(0.0, 0.0, 1.0, 0.0),\n"
    "                vec4(-0.5 * (m00 + m10) * scale_s + off_s + 0.5, -0.5 * (m01 + m11) * scale_t + off_t + 0.5, 0.0, 1.0));\n"
    "}\n";

// the main vertex shaders that read texture matrices from a record's table without the generic rewrite
static bool texanim_hand_written_pull(const std::string& filename)
{
    static const char* const names[] = { "alphaPullV.glsl", "materialPullV.glsl", "pbrAlphaPullV.glsl", "pbrGlowPullV.glsl", "shadowAlphaPullV.glsl" };
    for (const char* n : names)
    {
        const size_t len = strlen(n);
        if (filename.size() >= len && filename.compare(filename.size() - len, len, n) == 0)
        {
            return true;
        }
    }
    return false;
}

static bool pull_parse_decl(const std::string& line, const char* keyword, std::string& type, std::string& name)
{
    const size_t npos = std::string::npos;
    size_t p = line.find_first_not_of(" \t");
    const size_t k = strlen(keyword);
    if (p == npos || line.compare(p, k, keyword) != 0 || p + k >= line.size() || (line[p + k] != ' ' && line[p + k] != '\t'))
    {
        return false;
    }
    p += k;
    const size_t t0 = line.find_first_not_of(" \t", p);
    if (t0 == npos) return false;
    const size_t t1 = line.find_first_of(" \t", t0);
    if (t1 == npos) return false;
    const size_t n0 = line.find_first_not_of(" \t", t1);
    if (n0 == npos) return false;
    const size_t n1 = line.find_first_of(" \t;[", n0);
    if (n1 == npos || line[n1] == '[') return false;
    if (line.find(';', n1) == npos) return false;
    type = line.substr(t0, t1 - t0);
    name = line.substr(n0, n1 - n0);
    return true;
}

static std::string pull_prologue(const std::vector<std::pair<std::string, std::string> >& attrs, const std::vector<std::string>& mats)
{
    std::string s;
    s += "    {\n";
    s += "        GeoPacket ll_pk = GeoPackets(ll_packet_table).p[gl_InstanceIndex];\n";
    s += "        uint ll_i = ll_pk.first + uint(gl_VertexIndex);\n";
    s += "        uint ll_index = ll_i;\n";
    s += "        if ((ll_pk.flags & 4u) == 0u)\n        {\n";
    s += "            uint ll_word = GeoWords(ll_pk.indices).w[ll_i >> 1];\n";
    s += "            ll_index = ((ll_i & 1u) != 0u) ? (ll_word >> 16) : (ll_word & 0xFFFFu);\n        }\n";
    s += "        GeoMatrix ll_m = GeoMatrices(ll_packet_models).m[ll_pk.model];\n";
    for (const auto& a : attrs)
    {
        const std::string& type = a.first;
        const std::string& name = a.second;
        s += "#ifdef LL_PULL_HAS_" + name + "\n";
        if (name == "position")
        {
            s += "        position = GeoVec4(ll_pk.positions).v[ll_index]" + std::string(type == "vec3" ? ".xyz" : "") + ";\n";
        }
        else if (name == "normal")
        {
            s += "        normal = GeoVec4(ll_pk.normals).v[ll_index].xyz;\n";
        }
        else if (name == "texcoord0")
        {
            s += "        texcoord0 = GeoVec2(ll_pk.texcoords).t[ll_index];\n";
        }
        else if (name == "texcoord1")
        {
            s += "        texcoord1 = GeoVec2(ll_pk.texcoords1).t[ll_index];\n";
        }
        else if (name == "texcoord2")
        {
            s += "        texcoord2 = GeoVec2(ll_pk.texcoords2).t[ll_index];\n";
        }
        else if (name == "diffuse_color")
        {
            s += "        diffuse_color = unpackUnorm4x8(GeoWords(ll_pk.colors).w[ll_index]);\n";
        }
        else if (name == "tangent")
        {
            // (a mesh without the array has its offset 0, the address of the positions: the attribute is then 0, as the unset attribute of the old draw is)
            s += "        tangent = all(equal(ll_pk.tangents, ll_pk.positions)) ? vec4(0.0) : GeoVec4(ll_pk.tangents).v[ll_index];\n";
        }
        else if (name == "clothing")
        {
            s += "        clothing = all(equal(ll_pk.tangents, ll_pk.positions)) ? vec4(0.0) : GeoVec4(ll_pk.tangents).v[ll_index];\n";          // (the avatar's clothing array travels in the tangent slot: no mesh has both)
        }
        else if (name == "texture_index")
        {
            // the batched texture index of a face rides in the w of the position (a U32: TYPE_TEXTURE_INDEX is "position.w" in the vertex buffer)
            s += type == "uint" ? "        texture_index = floatBitsToUint(GeoVec4(ll_pk.positions).v[ll_index].w);\n" : "        texture_index = int(floatBitsToUint(GeoVec4(ll_pk.positions).v[ll_index].w));\n";
        }
        else if (name == "weight")
        {
            // a float a vertex (the avatar body); GL fills a vec4 attribute from one float as (x, 0, 0, 1)
            // (a draw without the array, a program that skins by the library's `weight` and is drawn rigid, has none: nothing is read)
            s += type == "vec4" ? "        if (any(notEqual(ll_pk.weights, uvec2(0u)))) weight = vec4(GeoFloats(ll_pk.weights).f[ll_index], 0.0, 0.0, 1.0);\n"
                                : "        if (any(notEqual(ll_pk.weights, uvec2(0u)))) weight = GeoFloats(ll_pk.weights).f[ll_index];\n";
        }
        else
        {
            LL_WARNS("ShaderLoading") << "LL_VERTEX_PULL: the attribute '" << name << "' cannot be read from a draw's arrays: it stays 0" << LL_ENDL;
        }
        s += "#endif\n";
    }
    for (const std::string& m : mats)
    {
        s += "#ifdef LL_PULL_HAS_" + m + "\n";
        if (m == "modelview_matrix") s += "        modelview_matrix = ll_m.mv;\n";
        else if (m == "modelview_projection_matrix") s += "        modelview_projection_matrix = ll_m.mvp;\n";
        else if (m == "normal_matrix") s += "        normal_matrix = mat3(ll_m.n0.xyz, ll_m.n1.xyz, ll_m.n2.xyz);\n";
        else if (m == "texture_matrix0")
        {
            s += "        texture_matrix0 = (ll_pk.pad.x != 0u) ? GeoTexMatrices(ll_packet_texmats).t[ll_pk.pad.x] : mat4(1.0);\n";
            // <FS> a texture animation's matrix slot holds its parameters (0 in the last float, llvktexanim.h): the matrix is made here, against the clock in slot 0
            s += "        if (texture_matrix0[3][3] == 0.0) texture_matrix0 = ll_texanim_core(texture_matrix0, GeoTexMatrices(ll_packet_texmats).t[0][0].yz);\n";
        }
        s += "#endif\n";
    }
    s += "    }\n";
    return s;
}

U32 LLShaderMgr::loadShaderFile(const std::string& filename, S32 & shader_level, ShaderStage type, std::map<std::string, std::string>* defines, S32 texture_index_channels)
{

// endsure work-around for missing GLSL funcs gets propogated to feature shader files (e.g. srgbF.glsl)
#if LL_DARWIN
    if (!gDeviceCaps.mIsApple && defines)
    {
        (*defines)["OLD_SELECT"] = "1";
    }
#endif

    if (filename.empty())
    {
        LL_WARNS("ShaderLoading") << "tried loading empty filename" << LL_ENDL;
        return 0;
    }


    //read in from file
    LLFILE* file = NULL;

    S32 try_gpu_class = shader_level;
    S32 gpu_class;

    std::string open_file_name;

#if 0  // WIP -- try to come up with a way to fallback to an error shader without needing debug stubs all over the place in the shader tree
    if (shader_level == -1)
    {
        // use "error" fallback
        if (type == ShaderStage::Vertex)
        {
            open_file_name = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "shaders/errorV.glsl");
        }
        else
        {
            llassert(type == ShaderStage::Fragment);  // type must be vertex or fragment shader
            open_file_name = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "shaders/errorF.glsl");
        }

        file = LLFile::fopen(open_file_name, "r");
    }
    else
#endif
    {
        //find the most relevant file
        for (gpu_class = try_gpu_class; gpu_class > 0; gpu_class--)
        {   //search from the current gpu class down to class 1 to find the most relevant shader
            std::stringstream fname;
            fname << getShaderDirPrefix();
            fname << gpu_class << gDirUtilp->getDirDelimiter() << filename;

            open_file_name = fname.str();

            /*
            Would be awesome, if we didn't have shaders that re-use files
            with different environments to say, add skinning, etc
            can't depend on cached version to have evaluate ifdefs identically...
            if we can define a deterministic hash for the shader based on
            all the inputs, maybe we can save some time here.
            if (mShaderObjects.count(filename) > 0)
            {
                return mShaderObjects[filename];
            }

            */

            LL_DEBUGS("ShaderLoading") << "Looking in " << open_file_name << LL_ENDL;
            file = LLFile::fopen(open_file_name, "r");      /* Flawfinder: ignore */
            if (file)
            {
                LL_DEBUGS("ShaderLoading") << "Loading file: " << open_file_name << " (Want class " << gpu_class << ")" << LL_ENDL;
                break; // done
            }
        }
    }

    if (file == NULL)
    {
        if (gDirUtilp->fileExists(open_file_name))
        {
            LL_WARNS("ShaderLoading") << "GLSL Shader file failed to open: " << open_file_name << LL_ENDL;
        }
        else
        {
            LL_WARNS("ShaderLoading") << "GLSL Shader file not found: " << open_file_name << LL_ENDL;
        }
        return 0;
    }

    //we can't have any lines longer than 1024 characters
    //or any shaders longer than 4096 lines... deal - DaveP
    char buff[1024];
    char *extra_code_text[1024];
    char *shader_code_text[4096 + LL_ARRAY_SIZE(extra_code_text)] = { NULL };
    U32 extra_code_count = 0, shader_code_count = 0;
    BOOST_STATIC_ASSERT(LL_ARRAY_SIZE(extra_code_text) < LL_ARRAY_SIZE(shader_code_text));


    S32 major_version = gDeviceCaps.mShaderLangMajor;
    S32 minor_version = gDeviceCaps.mShaderLangMinor;

    if (major_version == 1 && minor_version < 30)
    {
        llassert(false); // GL 3.1 or later required
    }
    else
    {
        if (major_version >= 4)
        {
            //set version to 400 or 420
            if (minor_version >= 20)
            {
                shader_code_text[shader_code_count++] = strdup("#version 420\n");
            }
            else
            {
                shader_code_text[shader_code_count++] = strdup("#version 400\n");
            }
        }
        else if (major_version == 3)
        {
            if (minor_version <= 29)
            {
                // OpenGL 3.2 had GLSL version 1.50.  anything after that the version numbers match.
                // https://www.khronos.org/opengl/wiki/Core_Language_(GLSL)#OpenGL_and_GLSL_versions
                shader_code_text[shader_code_count++] = strdup("#version 150\n");
            }
            else
            {
                shader_code_text[shader_code_count++] = strdup("#version 330\n");
            }
        }
        else
        {
            // OpenGL 3.2 had GLSL version 1.50.  anything after that the version numbers match.
            if (type == ShaderStage::Geometry || minor_version >= 50)
            {
                //set version to 1.50
                shader_code_text[shader_code_count++] = strdup("#version 150\n");
                //some implementations of GLSL 1.30 require integer precision be explicitly declared
                extra_code_text[extra_code_count++] = strdup("precision mediump int;\n");
                extra_code_text[extra_code_count++] = strdup("precision highp float;\n");
            }
            else
            {
                //set version to 1.40
                shader_code_text[shader_code_count++] = strdup("#version 140\n");
                //some implementations of GLSL 1.30 require integer precision be explicitly declared
                extra_code_text[extra_code_count++] = strdup("precision mediump int;\n");
                extra_code_text[extra_code_count++] = strdup("precision highp float;\n");
            }
        }
    }

    if (type == ShaderStage::Fragment)
    {
        extra_code_text[extra_code_count++] = strdup("#define FRAGMENT_SHADER 1\n");
    }
    else
    {
        extra_code_text[extra_code_count++] = strdup("#define VERTEX_SHADER 1\n");
    }

    // Use alpha float to store bit flags
    // See: C++: addDeferredAttachment(), shader: frag_data[2]
    extra_code_text[extra_code_count++] = strdup("#define GBUFFER_FLAG_SKIP_ATMOS   0.0 \n"); // atmo kill
    extra_code_text[extra_code_count++] = strdup("#define GBUFFER_FLAG_HAS_ATMOS    0.34\n"); // bit 0
    extra_code_text[extra_code_count++] = strdup("#define GBUFFER_FLAG_HAS_PBR      0.67\n"); // bit 1
    extra_code_text[extra_code_count++] = strdup("#define GBUFFER_FLAG_HAS_HDRI      1.0\n");  // bit 2
    extra_code_text[extra_code_count++] = strdup("#define GET_GBUFFER_FLAG(data, flag)    (abs(data-flag)< 0.1)\n");

    if (defines)
    {
        for (auto iter = defines->begin(); iter != defines->end(); ++iter)
        {
            std::string define = "#define " + iter->first + " " + iter->second + "\n";
            extra_code_text[extra_code_count++] = (char *) strdup(define.c_str());
        }
    }

    if( gDeviceCaps.mIsAMD )
    {
        extra_code_text[extra_code_count++] = strdup( "#define IS_AMD_CARD 1\n" );
    }

    const bool pull_material = defines && defines->find("LL_BINDLESS_MATERIAL") != defines->end() && type == ShaderStage::Fragment;
    const bool pull_pbr = defines && defines->find("LL_BINDLESS_PBR") != defines->end() && type == ShaderStage::Fragment;
    const bool pull_projector = defines && defines->find("LL_BINDLESS_PROJECTOR") != defines->end() && type == ShaderStage::Fragment;
    const bool pull_vertex = defines && defines->find("LL_VERTEX_PULL") != defines->end() && type == ShaderStage::Vertex;
    std::vector<std::pair<std::string, std::string> > pull_attrs;
    std::vector<std::string> pull_mats;
    bool pull_main_seen = false, pull_main_done = false, pull_skin_seen = false, pull_index_seen = false;
    bool texanim_core_here = false;
    if (pull_vertex)
    {
        // S6.6: the declarations of the geometry packet path (the layout of the worker's rows, as diffusePullV.glsl)
        extra_code_text[extra_code_count++] = strdup(
            "#extension GL_EXT_buffer_reference : require\n"
            "#extension GL_EXT_buffer_reference_uvec2 : require\n"
            "layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer GeoVec4 { vec4 v[]; };\n"
            "layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer GeoVec2 { vec2 t[]; };\n"
            "layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer GeoWords { uint w[]; };\n"
            "layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer GeoFloats { float f[]; };\n"
            "struct GeoPacket { uvec2 positions; uvec2 normals; uvec2 texcoords; uvec2 colors; uvec2 indices; uvec2 tangents; uvec2 texcoords1; uvec2 texcoords2; uint first; uint model; uint flags; uint mapSlots;"
            " uvec2 slots; uvec2 pad; vec4 specular; float envIntensity; float cutoff; float brightness; float pad2; uvec2 weights; uvec2 palette; uint joints; uint skinPad0; uint skinPad1; uint skinPad2; };\n"
            "struct GeoMatrix { mat4 mv; mat4 mvp; vec4 n0; vec4 n1; vec4 n2; };\n"
            "layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer GeoPackets { GeoPacket p[]; };\n"
            "layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer GeoMatrices { GeoMatrix m[]; };\n"
            "layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer GeoTexMatrices { mat4 t[]; };\n"
            "uniform uvec2 ll_packet_table;\nuniform uvec2 ll_packet_models;\nuniform uvec2 ll_packet_texmats;\n");
    }
    // <FS> the texture animation function, once per program: the generic pull rewrite puts it before main() (only the main file has one: a library file of the same program defining it
    // too would not link); the hand written pull shaders get it after their own #extension lines (below, in the line loop)
    const bool texanim_after_extension = type == ShaderStage::Vertex && !pull_vertex && texanim_hand_written_pull(filename);
    if (defines && defines->find("LL_BINDLESS_DIFFUSE") != defines->end() && type == ShaderStage::Fragment)
    {
        // <FS> render packets (Vulkan mirror only): the draw's texture comes from the bindless table (set 2), by the slot the vertex shader passes on, instead of tex0..texN
        extra_code_text[extra_code_count++] = strdup("#define HAS_DIFFUSE_LOOKUP\n");
        extra_code_text[extra_code_count++] = strdup("#extension GL_EXT_nonuniform_qualifier : require\n");
        extra_code_text[extra_code_count++] = strdup("layout(set = 2, binding = 0) uniform sampler2D ll_bindless_textures[];\n");
        extra_code_text[extra_code_count++] = strdup("flat in uint vary_slot;\n");
        extra_code_text[extra_code_count++] = strdup("vec4 diffuseLookup(vec2 texcoord)\n");
        extra_code_text[extra_code_count++] = strdup("{\n");
        if (!fbPlainShaders()) extra_code_text[extra_code_count++] = strdup("    llFbNote(vary_slot, texcoord);\n");
        extra_code_text[extra_code_count++] = strdup("    return texture(ll_bindless_textures[nonuniformEXT(LL_HM(vary_slot))], texcoord);\n");
        extra_code_text[extra_code_count++] = strdup("}\n");
        if (defines->find("ALPHA_UBER_MAT") != defines->end())
        {
            // <FS> item D2: the merged alpha program also reads the normal and specular maps of a material draw from the table (the slots come from the vertex shader, declared in alphaF.glsl)
            extra_code_text[extra_code_count++] = strdup(fbPlainShaders() ? "#define llFbTex(slot, uv) texture(ll_bindless_textures[nonuniformEXT(LL_HM(slot))], uv)\n" : "vec4 llFbTex(uint slot, vec2 uv)\n{\n    llFbNote(slot, uv);\n    return texture(ll_bindless_textures[nonuniformEXT(LL_HM(slot))], uv);\n}\n");
        }
    }
    else if (pull_material)
    {
        // <FS> render packets: the materials fragment shader takes its textures from the bindless table (the slots come from the vertex shader) and its per-draw uniforms
        // (rewritten line by line below) as flat inputs
        extra_code_text[extra_code_count++] = strdup("#extension GL_EXT_nonuniform_qualifier : require\n");
        extra_code_text[extra_code_count++] = strdup("layout(set = 2, binding = 0) uniform sampler2D ll_bindless_textures[];\n");
        extra_code_text[extra_code_count++] = strdup("flat in uint vary_slot;\n");
        extra_code_text[extra_code_count++] = strdup(fbPlainShaders() ? "#define llFbTex(slot, uv) texture(ll_bindless_textures[nonuniformEXT(LL_HM(slot))], uv)\n" : "vec4 llFbTex(uint slot, vec2 uv)\n{\n    llFbNote(slot, uv);\n    return texture(ll_bindless_textures[nonuniformEXT(LL_HM(slot))], uv);\n}\n");
        extra_code_text[extra_code_count++] = strdup("#ifdef HAS_NORMAL_MAP\n");
        extra_code_text[extra_code_count++] = strdup("flat in uint vary_slot_normal;\n");
        extra_code_text[extra_code_count++] = strdup("#endif\n");
        extra_code_text[extra_code_count++] = strdup("#ifdef HAS_SPECULAR_MAP\n");
        extra_code_text[extra_code_count++] = strdup("flat in uint vary_slot_specular;\n");
        extra_code_text[extra_code_count++] = strdup("#endif\n");
    }
    else if (pull_projector)
    {
        // <FS> S6 LV2: the projectors of a lighting pass: the projected image comes from the bindless table by the slot of the light's row (flat in uint vary_proj_slot)
        extra_code_text[extra_code_count++] = strdup("#extension GL_EXT_nonuniform_qualifier : require\n");
        extra_code_text[extra_code_count++] = strdup("layout(set = 2, binding = 0) uniform sampler2D ll_bindless_textures[];\n");
        extra_code_text[extra_code_count++] = strdup("flat in uint vary_proj_slot;\n");
    }
    else if (pull_pbr)
    {
        // <FS> S5c: the PBR alpha fragment shader takes its four textures from the bindless table (slots from the vertex shader: base colour, normal, occlusion roughness metallic, emissive) and its
        // per-draw uniforms (rewritten line by line below) as flat inputs. A normal map without an image yet (slot 0, the stand-in) is the flat normal the old path binds then.
        extra_code_text[extra_code_count++] = strdup("#extension GL_EXT_nonuniform_qualifier : require\n");
        extra_code_text[extra_code_count++] = strdup("layout(set = 2, binding = 0) uniform sampler2D ll_bindless_textures[];\n");
        extra_code_text[extra_code_count++] = strdup("flat in uint vary_slot;\n");
        extra_code_text[extra_code_count++] = strdup("flat in uint vary_slot_normal;\n");
        extra_code_text[extra_code_count++] = strdup("flat in uint vary_slot_specular;\n");
        extra_code_text[extra_code_count++] = strdup("flat in uint vary_slot_emissive;\n");
        extra_code_text[extra_code_count++] = strdup(fbPlainShaders() ? "#define llFbTex(slot, uv) texture(ll_bindless_textures[nonuniformEXT(LL_HM(slot))], uv)\n" : "vec4 llFbTex(uint slot, vec2 uv)\n{\n    llFbNote(slot, uv);\n    return texture(ll_bindless_textures[nonuniformEXT(LL_HM(slot))], uv);\n}\n");
        extra_code_text[extra_code_count++] = strdup("vec3 pbrNormalTap(vec2 uv)\n{\n    vec3 tap = llFbTex(vary_slot_normal, uv).xyz * 2.0 - 1.0;\n    return vary_slot_normal == 0u ? vec3(0.0, 0.0, 1.0) : tap;\n}\n");
    }
    else if (texture_index_channels > 0 && type == ShaderStage::Fragment)
    {
        //use specified number of texture channels for indexed texture rendering

        /* prepend shader code that looks like this:

        uniform sampler2D tex0;
        uniform sampler2D tex1;
        uniform sampler2D tex2;
        .
        .
        .
        uniform sampler2D texN;

        flat in int vary_texture_index;

        vec4 ret = vec4(1,0,1,1);

        vec4 diffuseLookup(vec2 texcoord)
        {
            switch (vary_texture_index)
            {
                case 0: ret = texture(tex0, texcoord); break;
                case 1: ret = texture(tex1, texcoord); break;
                case 2: ret = texture(tex2, texcoord); break;
                .
                .
                .
                case N: return texture(texN, texcoord); break;
            }

            return ret;
        }
        */

        extra_code_text[extra_code_count++] = strdup("#define HAS_DIFFUSE_LOOKUP\n");

        //uniform declartion
        for (S32 i = 0; i < texture_index_channels; ++i)
        {
            std::string decl = llformat("uniform sampler2D tex%d;\n", i);
            extra_code_text[extra_code_count++] = strdup(decl.c_str());
        }

        if (texture_index_channels > 1)
        {
            extra_code_text[extra_code_count++] = strdup("flat in int vary_texture_index;\n");
        }

        extra_code_text[extra_code_count++] = strdup("vec4 diffuseLookup(vec2 texcoord)\n");
        extra_code_text[extra_code_count++] = strdup("{\n");


        if (texture_index_channels == 1)
        { //don't use flow control, that's silly
            extra_code_text[extra_code_count++] = strdup("return texture(tex0, texcoord);\n");
            extra_code_text[extra_code_count++] = strdup("}\n");
        }
        else if (major_version > 1 || minor_version >= 30)
        {  //switches are supported in GLSL 1.30 and later
            if (gDeviceCaps.mIsNVIDIA)
            { //switches are unreliable on some NVIDIA drivers
                for (S32 i = 0; i < texture_index_channels; ++i)
                {
                    std::string if_string = llformat("\t%sif (vary_texture_index == %d) { return texture(tex%d, texcoord); }\n", i > 0 ? "else " : "", i, i);
                    extra_code_text[extra_code_count++] = strdup(if_string.c_str());
                }
                extra_code_text[extra_code_count++] = strdup("\treturn vec4(1,0,1,1);\n");
                extra_code_text[extra_code_count++] = strdup("}\n");
            }
            else
            {
                extra_code_text[extra_code_count++] = strdup("\tvec4 ret = vec4(1,0,1,1);\n");
                extra_code_text[extra_code_count++] = strdup("\tswitch (vary_texture_index)\n");
                extra_code_text[extra_code_count++] = strdup("\t{\n");

                //switch body
                for (S32 i = 0; i < texture_index_channels; ++i)
                {
                    std::string case_str = llformat("\t\tcase %d: return texture(tex%d, texcoord);\n", i, i);
                    extra_code_text[extra_code_count++] = strdup(case_str.c_str());
                }

                extra_code_text[extra_code_count++] = strdup("\t}\n");
                extra_code_text[extra_code_count++] = strdup("\treturn ret;\n");
                extra_code_text[extra_code_count++] = strdup("}\n");
            }
        }
        else
        { //should never get here.  Indexed texture rendering requires GLSL 1.30 or later
            // (for passing integers between vertex and fragment shaders)
            LL_ERRS() << "Indexed texture rendering requires GLSL 1.30 or later." << LL_ENDL;
        }
    }

    // Master definition can be found in deferredUtil.glsl
    extra_code_text[extra_code_count++] = strdup("struct GBufferInfo { vec4 albedo; vec4 specular; vec3 normal; vec4 emissive; float gbufferFlag; float envIntensity; };\n");

    //copy file into memory
    enum {
          flag_write_to_out_of_extra_block_area = 0x01
        , flag_extra_block_marker_was_found = 0x02
    };

    unsigned char flags = flag_write_to_out_of_extra_block_area;

    U32 out_of_extra_block_counter = 0, start_shader_code = shader_code_count, file_lines_count = 0;

#define TOUCH_SHADERS 0

#if TOUCH_SHADERS
    const char* marker = "// touched";
    bool touched = false;
#endif

    while(NULL != fgets((char *)buff, 1024, file)
          && shader_code_count < (LL_ARRAY_SIZE(shader_code_text) - LL_ARRAY_SIZE(extra_code_text)))
    {
        file_lines_count++;

        bool extra_block_area_found = NULL != strstr((const char*)buff, "[EXTRA_CODE_HERE]");

#if TOUCH_SHADERS
        if (NULL != strstr((const char*)buff, marker))
        {
            touched = true;
        }
#endif

        if(extra_block_area_found && !(flag_extra_block_marker_was_found & flags))
        {
            if(!(flag_write_to_out_of_extra_block_area & flags))
            {
                //shift
                for(U32 to = start_shader_code, from = extra_code_count + start_shader_code;
                    from < shader_code_count; ++to, ++from)
                {
                    shader_code_text[to] = shader_code_text[from];
                }

                shader_code_count -= extra_code_count;
            }

            //copy extra code
            for(U32 n = 0; n < extra_code_count
                && shader_code_count < (LL_ARRAY_SIZE(shader_code_text) - LL_ARRAY_SIZE(extra_code_text)); ++n)
            {
                shader_code_text[shader_code_count++] = extra_code_text[n];
            }

            extra_code_count = 0;

            flags &= ~flag_write_to_out_of_extra_block_area;
            flags |= flag_extra_block_marker_was_found;
        }
        else
        {
            if (pull_material || pull_pbr)
            {
                std::string fixed((const char*)buff);
                auto replace = [&fixed](const char* from, const char* to)
                {
                    const size_t from_len = strlen(from), to_len = strlen(to);
                    for (size_t at = fixed.find(from); at != std::string::npos; at = fixed.find(from, at + to_len))
                    {
                        fixed.replace(at, from_len, to);
                    }
                };
                if (pull_pbr)
                {
                    replace("uniform sampler2D diffuseMap;", "");
                    replace("uniform sampler2D bumpMap;", "");
                    replace("uniform sampler2D emissiveMap;", "");
                    replace("uniform sampler2D specularMap;", "");
                    replace("texture(bumpMap, normal_texcoord.xy).xyz*2.0-1.0", "pbrNormalTap(normal_texcoord.xy)");
                    replace("texture(diffuseMap,", "llFbTex(vary_slot,");
                    replace("texture(specularMap,", "llFbTex(vary_slot_specular,");
                    replace("texture(emissiveMap,", "llFbTex(vary_slot_emissive,");
                    replace("uniform float metallicFactor;", "flat in float metallicFactor;");
                    replace("uniform float roughnessFactor;", "flat in float roughnessFactor;");
                    replace("uniform vec3 emissiveColor;", "flat in vec3 emissiveColor;");
                    replace("uniform float minimum_alpha;", "flat in float minimum_alpha;");
                    shader_code_text[shader_code_count] = (char *)strdup(fixed.c_str());
                }
                else
                {
                replace("uniform sampler2D diffuseMap;", "");
                replace("uniform sampler2D bumpMap;", "");
                replace("uniform sampler2D specularMap;", "");
                replace("texture(diffuseMap,", "llFbTex(vary_slot,");
                replace("texture(bumpMap,", "llFbTex(vary_slot_normal,");
                replace("texture(specularMap,", "llFbTex(vary_slot_specular,");
                replace("uniform sampler2D texture0;", "");       // (J1 K4: the emboss pass samples one image, the bump image, twice)
                replace("uniform sampler2D texture1;", "");
                replace("texture(texture0,", "llFbTex(vary_slot,");
                replace("texture(texture1,", "llFbTex(vary_slot,");
                replace("uniform float emissive_brightness;", "flat in float emissive_brightness;");
                replace("uniform float env_intensity;", "flat in float env_intensity;");
                replace("uniform vec4 specular_color;", "flat in vec4 specular_color;");
                replace("uniform float minimum_alpha;", "flat in float minimum_alpha;");
                shader_code_text[shader_code_count] = (char *)strdup(fixed.c_str());
                }
            }
            else if (pull_vertex)
            {
                std::string line((const char*)buff);
                std::string decl_type, decl_name;
                if (pull_parse_decl(line, "in", decl_type, decl_name))
                {
                    pull_attrs.push_back({ decl_type, decl_name });         // an attribute becomes a global the start of main() fills
                    line = "#ifndef LL_PULL_HAS_" + decl_name + "\n" + decl_type + " " + decl_name + ";\n#define LL_PULL_HAS_" + decl_name + " 1\n#endif\n";       // (a shader may declare it in more than one branch)
                }
                else if (pull_parse_decl(line, "uniform", decl_type, decl_name) && (decl_type == "mat4" || decl_type == "mat3")
                         && (decl_name == "modelview_matrix" || decl_name == "modelview_projection_matrix" || decl_name == "normal_matrix" || decl_name == "texture_matrix0"))
                {
                    pull_mats.push_back(decl_name);
                    line = "#ifndef LL_PULL_HAS_" + decl_name + "\n" + decl_type + " " + decl_name + ";\n#define LL_PULL_HAS_" + decl_name + " 1\n#endif\n";       // (a shader may declare it in more than one branch)
                }
                else if (!pull_main_done)
                {
                    if (!pull_main_seen && line.find("getSkinnedTransform") != std::string::npos)
                    {
                        pull_skin_seen = true;          // (the avatar skin function of the library reads `weight`)
                    }
                    if (!pull_main_seen && line.find("passTextureIndex") != std::string::npos)
                    {
                        pull_index_seen = true;         // (the indexed texture library reads `texture_index`)
                    }
                    if (!pull_main_seen && line.find("void main") != std::string::npos)
                    {
                        pull_main_seen = true;
                        texanim_core_here = true;
                        bool has_weight = false;
                        for (const auto& a : pull_attrs)
                        {
                            has_weight = has_weight || a.second == "weight";
                        }
                        bool has_index = false;
                        for (const auto& a : pull_attrs)
                        {
                            has_index = has_index || a.second == "texture_index";
                        }
                        if (pull_index_seen && !has_index)
                        {
                            pull_attrs.push_back({ "int", "texture_index" });
                            line = "#ifndef LL_PULL_HAS_texture_index\nint texture_index;\n#define LL_PULL_HAS_texture_index 1\n#endif\n" + line;
                        }
                        if (pull_skin_seen && !has_weight)
                        {
                            // the main shader does not declare `weight`, only the library does (the alpha and shadow avatar shaders): it is declared here, so that the start of main() fills it
                            pull_attrs.push_back({ "vec4", "weight" });
                            line = "#ifndef LL_PULL_HAS_weight\nvec4 weight;\n#define LL_PULL_HAS_weight 1\n#endif\n" + line;
                        }
                    }
                    if (pull_main_seen)
                    {
                        const size_t brace = line.find('{');
                        if (brace != std::string::npos)
                        {
                            line.insert(brace + 1, "\n" + pull_prologue(pull_attrs, pull_mats));
                            pull_main_done = true;
                        }
                    }
                    if (texanim_core_here)
                    {
                        line = kTexAnimCore + line;         // (before main(): its prologue uses it for the texture matrix of a texture animation)
                        texanim_core_here = false;
                    }
                }
                shader_code_text[shader_code_count] = (char *)strdup(line.c_str());
            }
            else if (texanim_after_extension && strstr((const char*)buff, "GL_EXT_buffer_reference_uvec2"))
            {
                shader_code_text[shader_code_count] = (char *)strdup((std::string((const char*)buff) + kTexAnimCore).c_str());
            }
            else
            {
                shader_code_text[shader_code_count] = (char *)strdup((char *)buff);
            }

            if(flag_write_to_out_of_extra_block_area & flags)
            {
                shader_code_text[extra_code_count + start_shader_code + out_of_extra_block_counter]
                    = shader_code_text[shader_code_count];
                out_of_extra_block_counter++;

                if(out_of_extra_block_counter == extra_code_count)
                {
                    shader_code_count += extra_code_count;
                    flags &= ~flag_write_to_out_of_extra_block_area;
                }
            }

            ++shader_code_count;
        }
    } //while

    if(!(flag_extra_block_marker_was_found & flags))
    {
        for(U32 n = start_shader_code; n < extra_code_count + start_shader_code; ++n)
        {
            shader_code_text[n] = extra_code_text[n - start_shader_code];
        }

        if (file_lines_count < extra_code_count)
        {
            shader_code_count += extra_code_count;
        }

        extra_code_count = 0;
    }

#if TOUCH_SHADERS
    if (!touched)
    {
        fprintf(file, "\n%s\n", marker);
    }
#endif

    fclose(file);

    // <FS> F1.5: a shader object is its source text (stage, file name, text) under a handle of our own; the program build compiles the objects attached to a program together
    U32 ret = ++mNextShaderObject;
    {
        RecordedShader rec;
        rec.mType = type;
        rec.mFile = filename;
        for (U32 i = 0; i < shader_code_count; ++i)
        {
            if (shader_code_text[i])
            {
                rec.mSource += shader_code_text[i];
            }
        }
        mRecordedShaders[ret] = std::move(rec);
    }

    //free memory
    for (U32 i = 0; i < shader_code_count; i++)
    {
        free(shader_code_text[i]);
    }

    //successfully loaded, save results
    if (ret)
    {
        // Add shader file to map
        if (type == ShaderStage::Vertex) {
            mVertexShaderObjects[filename] = ret;
        }
        else if (type == ShaderStage::Fragment) {
            mFragmentShaderObjects[filename] = ret;
        }
        shader_level = try_gpu_class;
    }
    else
    {
        if (shader_level > 1)
        {
            shader_level--;
            return loadShaderFile(filename, shader_level, type, defines, texture_index_channels);
        }
        LL_WARNS("ShaderLoading") << "Failed to load " << filename << LL_ENDL;
    }

    LL_DEBUGS("ShaderLoading") << "loadShaderFile() completed, ret: " << U32(ret) << LL_ENDL;
    return ret;
}

void LLShaderMgr::initShaderCache(bool enabled, const LLUUID& old_cache_version, const LLUUID& current_cache_version, bool second_instance)
{
    LL_PROFILE_ZONE_SCOPED;
    LL_INFOS("ShaderMgr") << "Initializing shader cache" << LL_ENDL;

    mRecordShaderSources = true;       // (LL_VK_SHADER_CHECK used to be read here: the result was or-ed with true)
    if (mRecordShaderSources)
    {
        enabled = false;    // cached binaries would skip loadShaderFile, so there would be no source to check
        LL_INFOS("ShaderMgr") << "LL_VK_SHADER_CHECK set: shader cache disabled, Vulkan compile check enabled" << LL_ENDL;
    }
    mShaderCacheEnabled = false;        // there are no GL program binaries to cache any more

    if(!mShaderCacheEnabled || mShaderCacheVersion.notNull())
        return;

    mShaderCacheVersion = current_cache_version;

    mShaderCacheDir = gDirUtilp->getExpandedFilename(LL_PATH_CACHE, "shader_cache");
    LLFile::mkdir(mShaderCacheDir);

    {
        std::string meta_out_path = gDirUtilp->add(mShaderCacheDir, "shaderdata.llsd");
        if (gDirUtilp->fileExists(meta_out_path))
        {
            LL_PROFILE_ZONE_NAMED("shader_cache");
            LL_INFOS("ShaderMgr") << "Loading shader cache metadata" << LL_ENDL;

            llifstream instream(meta_out_path, std::ifstream::in | std::ifstream::binary);
            LLSD in_data;
            try
            {
                LLSDSerialize::fromBinary(in_data, instream, LLSDSerialize::SIZE_UNLIMITED);
            }
            catch( std::bad_alloc& )
            {
                // Try to get a bit more memory back before we try to clear the cache.
                in_data.clear();
                // Just in case it was somehow the cause, clear cache.
                clearShaderCache();
                // If user run out of memory this early in init,
                // we don't want to keep going just to crash again.
                // Notify user and close.
                LLError::LLUserWarningMsg::showOutOfMemory();
                LL_ERRS("ShaderMgr") << "Failed to parse shader cache metadata, potentially due to size. Purged cache." << LL_ENDL;
                return;
            }
            instream.close();

            if (old_cache_version == current_cache_version
                && in_data["version"].asUUID() == current_cache_version)
            {
                for (const auto& data_pair : llsd::inMap(in_data["shaders"]))
                {
                    ProgramBinaryData binary_info = ProgramBinaryData();
                    binary_info.mBinaryFormat = data_pair.second["binary_format"].asInteger();
                    binary_info.mBinaryLength = data_pair.second["binary_size"].asInteger();
                    binary_info.mLastUsedTime = (F32)data_pair.second["last_used"].asReal();
                    mShaderBinaryCache.insert_or_assign(LLUUID(data_pair.first), binary_info);
                }
            }
            else if (!second_instance)
            {
                LL_INFOS("ShaderMgr") << "Shader cache version mismatch detected. Purging." << LL_ENDL;
                clearShaderCache();
            }
            else
            {
                LL_INFOS("ShaderMgr") << "Shader cache version mismatch detected." << LL_ENDL;
            }
        }
    }
}

void LLShaderMgr::clearShaderCache()
{
    std::string shader_cache = gDirUtilp->getExpandedFilename(LL_PATH_CACHE, "shader_cache");
    LL_INFOS("ShaderMgr") << "Removing shader cache at " << shader_cache << LL_ENDL;
    const std::string mask = "*";
    gDirUtilp->deleteFilesInDir(shader_cache, mask);
    LLFile::rmdir(shader_cache);
    mShaderBinaryCache.clear();
}

void LLShaderMgr::persistShaderCacheMetadata()
{
    if(!mShaderCacheEnabled) return;
    if (mShaderCacheVersion.isNull())
    {
        LL_WARNS("ShaderMgr") << "Attempted to save shader cache with no version set" << LL_ENDL;
        return;
    }

    LL_INFOS("ShaderMgr") << "Persisting shader cache metadata to disk" << LL_ENDL;

    LLSD out;
    // Settings and shader cache get saved at different time, thus making
    // RenderShaderCacheVersion unreliable when running multiple viewer
    // instances, or for cases where viewer crashes before saving settings.
    // Dupplicate version to the cache itself.
    out["version"] = mShaderCacheVersion;
    out["shaders"] = LLSD::emptyMap();
    LLSD &shaders = out["shaders"];

    static const F32 LRU_TIME = (60.f * 60.f) * 24.f * 7.f; // 14 days
    const F32 current_time = (F32)LLTimer::getTotalSeconds();
    for (auto it = mShaderBinaryCache.begin(); it != mShaderBinaryCache.end();)
    {
        const ProgramBinaryData& shader_metadata = it->second;
        if ((shader_metadata.mLastUsedTime + LRU_TIME) < current_time)
        {
            std::string shader_path = gDirUtilp->add(mShaderCacheDir, it->first.asString() + ".shaderbin");
            LLFile::remove(shader_path);
            it = mShaderBinaryCache.erase(it);
        }
        else
        {
            LLSD data = LLSD::emptyMap();
            data["binary_format"] = LLSD::Integer(shader_metadata.mBinaryFormat);
            data["binary_size"] = LLSD::Integer(shader_metadata.mBinaryLength);
            data["last_used"] = LLSD::Real(shader_metadata.mLastUsedTime);
            shaders[it->first.asString()] = data;
            ++it;
        }
    }

    std::string meta_out_path = gDirUtilp->add(mShaderCacheDir, "shaderdata.llsd");
    llofstream outstream(meta_out_path, std::ios_base::out | std::ios_base::binary);
    if (!outstream.is_open())
    {
        LL_WARNS("ShaderMgr") << "Failed to open file. Unable to save shader cache to: " << mShaderCacheDir << LL_ENDL;
        return;
    }
    LLSDSerialize::toBinary(out, outstream);
    outstream.close();
}

void LLShaderMgr::initAttribsAndUniforms()
{
    //MUST match order of enum in LLVertexBuffer.h
    mReservedAttribs.push_back("position");
    mReservedAttribs.push_back("normal");
    mReservedAttribs.push_back("texcoord0");
    mReservedAttribs.push_back("texcoord1");
    mReservedAttribs.push_back("texcoord2");
    mReservedAttribs.push_back("texcoord3");
    mReservedAttribs.push_back("diffuse_color");
    mReservedAttribs.push_back("emissive");
    mReservedAttribs.push_back("tangent");
    mReservedAttribs.push_back("weight");
    mReservedAttribs.push_back("weight4");
    mReservedAttribs.push_back("clothing");
    mReservedAttribs.push_back("joint");
    mReservedAttribs.push_back("texture_index");

    //matrix state
    mReservedUniforms.push_back("modelview_matrix");
    mReservedUniforms.push_back("projection_matrix");
    mReservedUniforms.push_back("inv_proj");
    mReservedUniforms.push_back("modelview_projection_matrix");
    mReservedUniforms.push_back("inv_modelview");
    mReservedUniforms.push_back("identity_matrix");
    mReservedUniforms.push_back("normal_matrix");
    mReservedUniforms.push_back("texture_matrix0");
    mReservedUniforms.push_back("texture_matrix1");
    mReservedUniforms.push_back("texture_matrix2");
    mReservedUniforms.push_back("texture_matrix3");
    mReservedUniforms.push_back("object_plane_s");
    mReservedUniforms.push_back("object_plane_t");

    mReservedUniforms.push_back("texture_base_color_transform"); // (GLTF)
    mReservedUniforms.push_back("texture_normal_transform"); // (GLTF)
    mReservedUniforms.push_back("texture_metallic_roughness_transform"); // (GLTF)
    mReservedUniforms.push_back("texture_occlusion_transform"); // (GLTF)
    mReservedUniforms.push_back("texture_emissive_transform"); // (GLTF)
    mReservedUniforms.push_back("base_color_texcoord"); // (GLTF)
    mReservedUniforms.push_back("emissive_texcoord"); // (GLTF)
    mReservedUniforms.push_back("normal_texcoord"); // (GLTF)
    mReservedUniforms.push_back("metallic_roughness_texcoord"); // (GLTF)
    mReservedUniforms.push_back("occlusion_texcoord"); // (GLTF)
    mReservedUniforms.push_back("gltf_node_id"); // (GLTF)
    mReservedUniforms.push_back("gltf_material_id"); // (GLTF)

    mReservedUniforms.push_back("terrain_texture_transforms"); // (GLTF)

    llassert(mReservedUniforms.size() == LLShaderMgr::TERRAIN_TEXTURE_TRANSFORMS +1);

    mReservedUniforms.push_back("viewport");

    mReservedUniforms.push_back("light_position");
    mReservedUniforms.push_back("light_direction");
    mReservedUniforms.push_back("light_attenuation");
    mReservedUniforms.push_back("light_deferred_attenuation");
    mReservedUniforms.push_back("light_diffuse");
    mReservedUniforms.push_back("light_ambient");
    mReservedUniforms.push_back("light_count");
    mReservedUniforms.push_back("light");
    mReservedUniforms.push_back("light_col");
    mReservedUniforms.push_back("far_z");

    llassert(mReservedUniforms.size() == LLShaderMgr::MULTI_LIGHT_FAR_Z+1);

    //NOTE: MUST match order in eGLSLReservedUniforms
    mReservedUniforms.push_back("proj_mat");
    mReservedUniforms.push_back("proj_near");
    mReservedUniforms.push_back("proj_p");
    mReservedUniforms.push_back("proj_n");
    mReservedUniforms.push_back("proj_origin");
    mReservedUniforms.push_back("proj_range");
    mReservedUniforms.push_back("proj_ambiance");
    mReservedUniforms.push_back("proj_shadow_idx");
    mReservedUniforms.push_back("shadow_fade");
    mReservedUniforms.push_back("proj_focus");
    mReservedUniforms.push_back("proj_lod");
    mReservedUniforms.push_back("proj_ambient_lod");

    llassert(mReservedUniforms.size() == LLShaderMgr::PROJECTOR_AMBIENT_LOD+1);

    mReservedUniforms.push_back("color");
    mReservedUniforms.push_back("emissiveColor");
    mReservedUniforms.push_back("metallicFactor");
    mReservedUniforms.push_back("roughnessFactor");
    mReservedUniforms.push_back("mirror_flag");
    mReservedUniforms.push_back("clipPlane");
    mReservedUniforms.push_back("clipSign");

    mReservedUniforms.push_back("diffuseMap");
    mReservedUniforms.push_back("altDiffuseMap");
    mReservedUniforms.push_back("specularMap");
    mReservedUniforms.push_back("metallicRoughnessMap");
    mReservedUniforms.push_back("normalMap");
    mReservedUniforms.push_back("occlusionMap");
    mReservedUniforms.push_back("emissiveMap");
    mReservedUniforms.push_back("bumpMap");
    mReservedUniforms.push_back("bumpMap2");
    mReservedUniforms.push_back("environmentMap");
    mReservedUniforms.push_back("sceneMap");
    mReservedUniforms.push_back("sceneDepth");
    mReservedUniforms.push_back("reflectionProbes");
    mReservedUniforms.push_back("irradianceProbes");
    mReservedUniforms.push_back("heroProbes");
    mReservedUniforms.push_back("mirrorView");
    mReservedUniforms.push_back("rtReflMap");
    mReservedUniforms.push_back("cloud_noise_texture");
    mReservedUniforms.push_back("cloud_noise_texture_next");
    mReservedUniforms.push_back("lightnorm");
    mReservedUniforms.push_back("sunlight_color");
    mReservedUniforms.push_back("ambient_color");
    mReservedUniforms.push_back("sky_hdr_scale");
    mReservedUniforms.push_back("sky_sunlight_scale");
    mReservedUniforms.push_back("sky_ambient_scale");
    mReservedUniforms.push_back("classic_mode");
    mReservedUniforms.push_back("blue_horizon");
    mReservedUniforms.push_back("blue_density");
    mReservedUniforms.push_back("haze_horizon");
    mReservedUniforms.push_back("haze_density");
    mReservedUniforms.push_back("cloud_shadow");
    mReservedUniforms.push_back("density_multiplier");
    mReservedUniforms.push_back("distance_multiplier");
    mReservedUniforms.push_back("max_y");
    mReservedUniforms.push_back("glow");
    mReservedUniforms.push_back("cloud_color");
    mReservedUniforms.push_back("cloud_pos_density1");
    mReservedUniforms.push_back("cloud_pos_density2");
    mReservedUniforms.push_back("cloud_scale");
    mReservedUniforms.push_back("gamma");
    mReservedUniforms.push_back("scene_light_strength");

    llassert(mReservedUniforms.size() == LLShaderMgr::SCENE_LIGHT_STRENGTH+1);

    mReservedUniforms.push_back("center");
    mReservedUniforms.push_back("size");
    mReservedUniforms.push_back("falloff");

    mReservedUniforms.push_back("box_center");
    mReservedUniforms.push_back("box_size");


    mReservedUniforms.push_back("minLuminance");
    mReservedUniforms.push_back("maxExtractAlpha");
    mReservedUniforms.push_back("lumWeights");
    mReservedUniforms.push_back("warmthWeights");
    mReservedUniforms.push_back("warmthAmount");
    mReservedUniforms.push_back("glowStrength");
    mReservedUniforms.push_back("glowDelta");
    mReservedUniforms.push_back("glowNoiseMap");

    llassert(mReservedUniforms.size() == LLShaderMgr::GLOW_NOISE_MAP+1);


    mReservedUniforms.push_back("minimum_alpha");
    mReservedUniforms.push_back("emissive_brightness");

    // Deferred
    mReservedUniforms.push_back("shadow_matrix");
    mReservedUniforms.push_back("env_mat");
    mReservedUniforms.push_back("shadow_clip");
    mReservedUniforms.push_back("sun_wash");
    mReservedUniforms.push_back("shadow_noise");
    mReservedUniforms.push_back("blur_size");
    mReservedUniforms.push_back("ssao_radius");
    mReservedUniforms.push_back("ssao_max_radius");
    mReservedUniforms.push_back("ssao_factor");
    mReservedUniforms.push_back("ssao_factor_inv");
    mReservedUniforms.push_back("ssao_effect_mat");
    mReservedUniforms.push_back("screen_res");
    mReservedUniforms.push_back("near_clip");
    mReservedUniforms.push_back("shadow_offset");
    mReservedUniforms.push_back("shadow_bias");
    mReservedUniforms.push_back("spot_shadow_bias");
    mReservedUniforms.push_back("spot_shadow_offset");
    mReservedUniforms.push_back("sun_dir");
    mReservedUniforms.push_back("moon_dir");
    mReservedUniforms.push_back("shadow_res");
    mReservedUniforms.push_back("proj_shadow_res");
    mReservedUniforms.push_back("depth_cutoff");
    mReservedUniforms.push_back("norm_cutoff");
    mReservedUniforms.push_back("shadow_target_width");

    llassert(mReservedUniforms.size() == LLShaderMgr::DEFERRED_SHADOW_TARGET_WIDTH + 1);

    mReservedUniforms.push_back("iterationCount");
    mReservedUniforms.push_back("rayStep");
    mReservedUniforms.push_back("distanceBias");
    mReservedUniforms.push_back("depthRejectBias");
    mReservedUniforms.push_back("glossySampleCount");
    mReservedUniforms.push_back("noiseSine");
    mReservedUniforms.push_back("adaptiveStepMultiplier");
    mReservedUniforms.push_back("ssrTraceMode");

    mReservedUniforms.push_back("modelview_delta");
    mReservedUniforms.push_back("inv_modelview_delta");
    mReservedUniforms.push_back("cube_snapshot");

    mReservedUniforms.push_back("tc_scale");
    mReservedUniforms.push_back("rcp_screen_res");
    mReservedUniforms.push_back("rcp_frame_opt");
    mReservedUniforms.push_back("rcp_frame_opt2");

    mReservedUniforms.push_back("focal_distance");
    mReservedUniforms.push_back("blur_constant");
    mReservedUniforms.push_back("tan_pixel_angle");
    mReservedUniforms.push_back("magnification");
    mReservedUniforms.push_back("max_cof");
    mReservedUniforms.push_back("res_scale");
    mReservedUniforms.push_back("dof_width");
    mReservedUniforms.push_back("dof_height");

    mReservedUniforms.push_back("depthMap");
    mReservedUniforms.push_back("shadowMap0");
    mReservedUniforms.push_back("shadowMap1");
    mReservedUniforms.push_back("shadowMap2");
    mReservedUniforms.push_back("shadowMap3");
    mReservedUniforms.push_back("shadowMap4");
    mReservedUniforms.push_back("shadowMap5");

    llassert(mReservedUniforms.size() == LLShaderMgr::DEFERRED_SHADOW5+1);

    mReservedUniforms.push_back("positionMap");
    mReservedUniforms.push_back("diffuseRect");
    mReservedUniforms.push_back("specularRect");
    mReservedUniforms.push_back("emissiveRect");
    mReservedUniforms.push_back("exposureMap");
    mReservedUniforms.push_back("brdfLut");
    mReservedUniforms.push_back("noiseMap");
    mReservedUniforms.push_back("lightFunc");
    mReservedUniforms.push_back("lightMap");
    mReservedUniforms.push_back("bloomMap");
    mReservedUniforms.push_back("projectionMap");
    mReservedUniforms.push_back("norm_mat");

    mReservedUniforms.push_back("specular_color");
    mReservedUniforms.push_back("env_intensity");

    mReservedUniforms.push_back("matrixPalette");
    mReservedUniforms.push_back("translationPalette");

// <FS:CR> Import Vignette from Exodus
    mReservedUniforms.push_back("vignette");
// </FS:CR> Import Vignette from Exodus

    mReservedUniforms.push_back("screenTex");
    mReservedUniforms.push_back("screenDepth");
    mReservedUniforms.push_back("refTex");
    mReservedUniforms.push_back("exclusionTex");
    mReservedUniforms.push_back("eyeVec");
    mReservedUniforms.push_back("time");
    mReservedUniforms.push_back("waveDir1");
    mReservedUniforms.push_back("waveDir2");
    mReservedUniforms.push_back("lightDir");
    mReservedUniforms.push_back("specular");
    mReservedUniforms.push_back("lightExp");
    mReservedUniforms.push_back("waterFogColor");
    mReservedUniforms.push_back("waterFogColorLinear");
    mReservedUniforms.push_back("waterFogDensity");
    mReservedUniforms.push_back("waterFogKS");
    mReservedUniforms.push_back("refScale");
    mReservedUniforms.push_back("waterHeight");
    mReservedUniforms.push_back("waterPlane");
    mReservedUniforms.push_back("normScale");
    mReservedUniforms.push_back("fresnelScale");
    mReservedUniforms.push_back("fresnelOffset");
    mReservedUniforms.push_back("blurMultiplier");
    mReservedUniforms.push_back("sunAngle");
    mReservedUniforms.push_back("scaledAngle");
    mReservedUniforms.push_back("sunAngle2");

    mReservedUniforms.push_back("camPosLocal");
// [RLVa:KB] - @setsphere
    mReservedUniforms.push_back("rlvEffectMode");
    mReservedUniforms.push_back("rlvEffectParam1");
    mReservedUniforms.push_back("rlvEffectParam2");
    mReservedUniforms.push_back("rlvEffectParam3");
    mReservedUniforms.push_back("rlvEffectParam4");
    mReservedUniforms.push_back("rlvEffectParam5");
// [/RLV:KB]

    mReservedUniforms.push_back("gWindDir");
    mReservedUniforms.push_back("gSinWaveParams");
    mReservedUniforms.push_back("gGravity");

    mReservedUniforms.push_back("detail_0");
    mReservedUniforms.push_back("detail_1");
    mReservedUniforms.push_back("detail_2");
    mReservedUniforms.push_back("detail_3");

    mReservedUniforms.push_back("alpha_ramp");
    mReservedUniforms.push_back("paint_map");

    mReservedUniforms.push_back("detail_0_base_color");
    mReservedUniforms.push_back("detail_1_base_color");
    mReservedUniforms.push_back("detail_2_base_color");
    mReservedUniforms.push_back("detail_3_base_color");
    mReservedUniforms.push_back("detail_0_normal");
    mReservedUniforms.push_back("detail_1_normal");
    mReservedUniforms.push_back("detail_2_normal");
    mReservedUniforms.push_back("detail_3_normal");
    mReservedUniforms.push_back("detail_0_metallic_roughness");
    mReservedUniforms.push_back("detail_1_metallic_roughness");
    mReservedUniforms.push_back("detail_2_metallic_roughness");
    mReservedUniforms.push_back("detail_3_metallic_roughness");
    mReservedUniforms.push_back("detail_0_emissive");
    mReservedUniforms.push_back("detail_1_emissive");
    mReservedUniforms.push_back("detail_2_emissive");
    mReservedUniforms.push_back("detail_3_emissive");

    mReservedUniforms.push_back("baseColorFactors");
    mReservedUniforms.push_back("metallicFactors");
    mReservedUniforms.push_back("roughnessFactors");
    mReservedUniforms.push_back("emissiveColors");
    mReservedUniforms.push_back("minimum_alphas");

    mReservedUniforms.push_back("region_scale");

    mReservedUniforms.push_back("origin");
    mReservedUniforms.push_back("display_gamma");

    mReservedUniforms.push_back("inscatter");
    mReservedUniforms.push_back("sun_size");
    mReservedUniforms.push_back("fog_color");

    mReservedUniforms.push_back("transmittance_texture");
    mReservedUniforms.push_back("scattering_texture");
    mReservedUniforms.push_back("single_mie_scattering_texture");
    mReservedUniforms.push_back("irradiance_texture");
    mReservedUniforms.push_back("blend_factor");
    mReservedUniforms.push_back("moisture_level");
    mReservedUniforms.push_back("droplet_radius");
    mReservedUniforms.push_back("ice_level");
    mReservedUniforms.push_back("rainbow_map");
    mReservedUniforms.push_back("halo_map");
    mReservedUniforms.push_back("moon_brightness");
    mReservedUniforms.push_back("cloud_variance");
    mReservedUniforms.push_back("reflection_probe_ambiance");
    mReservedUniforms.push_back("max_probe_lod");
    mReservedUniforms.push_back("probe_strength");

    mReservedUniforms.push_back("sh_input_r");
    mReservedUniforms.push_back("sh_input_g");
    mReservedUniforms.push_back("sh_input_b");

    mReservedUniforms.push_back("sun_moon_glow_factor");
    mReservedUniforms.push_back("water_edge");
    mReservedUniforms.push_back("sun_up_factor");
    mReservedUniforms.push_back("moonlight_color");

    mReservedUniforms.push_back("debug_normal_draw_length");

    mReservedUniforms.push_back("edgesTex");
    mReservedUniforms.push_back("areaTex");
    mReservedUniforms.push_back("searchTex");
    mReservedUniforms.push_back("blendTex");
    // <FS:Beq> reserved uniforms for snapshot frame
    mReservedUniforms.push_back("border_color");
    mReservedUniforms.push_back("border_thickness");
    mReservedUniforms.push_back("frame_rect");
    // </FS:Beq>

    llassert(mReservedUniforms.size() == END_RESERVED_UNIFORMS);

    std::set<std::string> dupe_check;

    for (U32 i = 0; i < mReservedUniforms.size(); ++i)
    {
        if (dupe_check.find(mReservedUniforms[i]) != dupe_check.end())
        {
            LL_ERRS() << "Duplicate reserved uniform name found: " << mReservedUniforms[i] << LL_ENDL;
        }
        dupe_check.insert(mReservedUniforms[i]);
    }
}
