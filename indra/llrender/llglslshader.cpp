/**
 * @file llglslshader.cpp
 * @brief GLSL helper functions and state.
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

#include "llglslshader.h"
#include <cctype>
#include <set>

#include "llshadermgr.h"
#include "llfile.h"
#include "llrender.h"
#include "llvertexbuffer.h"
#include "llrendertarget.h"
#if defined(LL_VULKAN)
#include "llvkshader.h"
#include "llvkrenderer.h"
#include "llvkasync.h"
#include "llvkbuf.h"
#include "llvkquery.h"
#include "llvkmirror.h"
#endif

#include "hbxxh.h"
#include "llsdserialize.h"

#if LL_DARWIN
#include "OpenGL/OpenGL.h"
#endif

 // Print-print list of shader included source files that are linked together via glAttachShader()
 // i.e. On macOS / OSX the AMD GLSL linker will display an error if a varying is left in an undefined state.
#define DEBUG_SHADER_INCLUDES 0

// Lots of STL stuff in here, using namespace std to keep things more readable
using std::vector;
using std::pair;
using std::make_pair;
using std::string;

GLuint LLGLSLShader::sCurBoundShader = 0;
LLGLSLShader* LLGLSLShader::sCurBoundShaderPtr = NULL;
S32 LLGLSLShader::sIndexedTextureChannels = 0;
U32 LLGLSLShader::sMaxGLTFMaterials = 0;
U32 LLGLSLShader::sMaxGLTFNodes = 0;
bool LLGLSLShader::sProfileEnabled = false;
bool LLGLSLShader::sCanProfile = true;
std::set<LLGLSLShader*> LLGLSLShader::sInstances;
LLGLSLShader::defines_map_t LLGLSLShader::sGlobalDefines;
U64 LLGLSLShader::sTotalTimeElapsed = 0;
U32 LLGLSLShader::sTotalTrianglesDrawn = 0;
U64 LLGLSLShader::sTotalSamplesDrawn = 0;
U32 LLGLSLShader::sTotalBinds = 0;
boost::json::value LLGLSLShader::sDefaultStats;

//UI shader -- declared here so llui_libtest will link properly
LLGLSLShader    gUIProgram;
LLGLSLShader    gSolidColorProgram;

// NOTE: Keep gShaderConsts* and LLGLSLShader::ShaderConsts_e in sync!
const std::string gShaderConstsKey[LLGLSLShader::NUM_SHADER_CONSTS] =
{
      "LL_SHADER_CONST_CLOUD_MOON_DEPTH"
    , "LL_SHADER_CONST_STAR_DEPTH"
};

// NOTE: Keep gShaderConsts* and LLGLSLShader::ShaderConsts_e in sync!
const std::string gShaderConstsVal[LLGLSLShader::NUM_SHADER_CONSTS] =
{
      "0.99998" // SHADER_CONST_CLOUD_MOON_DEPTH // SL-14113
    , "0.99999" // SHADER_CONST_STAR_DEPTH       // SL-14113
};


bool shouldChange(const LLVector4& v1, const LLVector4& v2)
{
    return v1 != v2;
}

//===============================
// LLGLSL Shader implementation
//===============================

//static
void LLGLSLShader::initProfile()
{
    sProfileEnabled = true;
    sTotalTimeElapsed = 0;
    sTotalTrianglesDrawn = 0;
    sTotalSamplesDrawn = 0;
    sTotalBinds = 0;

    for (auto ptr : sInstances)
    {
        ptr->clearStats();
    }
}


struct LLGLSLShaderCompareTimeElapsed
{
    bool operator()(const LLGLSLShader* const& lhs, const LLGLSLShader* const& rhs)
    {
        return lhs->mTimeElapsed < rhs->mTimeElapsed;
    }
};

//static
void LLGLSLShader::finishProfile(boost::json::value& statsv)
{
    sProfileEnabled = false;

    if (! statsv.is_null())
    {
        std::vector<LLGLSLShader*> sorted(sInstances.begin(), sInstances.end());
        std::sort(sorted.begin(), sorted.end(), LLGLSLShaderCompareTimeElapsed());

        auto& stats = statsv.as_object();
        auto shadersit = stats.emplace("shaders", boost::json::array_kind).first;
        auto& shaders = shadersit->value().as_array();
        bool unbound = false;
        for (auto ptr : sorted)
        {
            if (ptr->mBinds == 0)
            {
                unbound = true;
            }
            else
            {
                auto& shaderit = shaders.emplace_back(boost::json::object_kind);
                ptr->dumpStats(shaderit.as_object());
            }
        }

        constexpr float mega = 1'000'000.f;
        float totalTimeMs = sTotalTimeElapsed / mega;
        LL_INFOS() << "-----------------------------------" << LL_ENDL;
        LL_INFOS() << "Total rendering time: " << llformat("%.4f ms", totalTimeMs) << LL_ENDL;
        LL_INFOS() << "Total samples drawn: " << llformat("%.4f million", sTotalSamplesDrawn / mega) << LL_ENDL;
        LL_INFOS() << "Total triangles drawn: " << llformat("%.3f million", sTotalTrianglesDrawn / mega) << LL_ENDL;
        LL_INFOS() << "-----------------------------------" << LL_ENDL;
        auto totalsit = stats.emplace("totals", boost::json::object_kind).first;
        auto& totals = totalsit->value().as_object();
        totals.emplace("time", totalTimeMs / 1000.0);
        totals.emplace("binds", sTotalBinds);
        totals.emplace("samples", sTotalSamplesDrawn);
        totals.emplace("triangles", sTotalTrianglesDrawn);

        auto unusedit = stats.emplace("unused", boost::json::array_kind).first;
        auto& unused = unusedit->value().as_array();
        if (unbound)
        {
            LL_INFOS() << "The following shaders were unused: " << LL_ENDL;
            for (auto ptr : sorted)
            {
                if (ptr->mBinds == 0)
                {
                    LL_INFOS() << ptr->mName << LL_ENDL;
                    unused.emplace_back(ptr->mName);
                }
            }
        }
    }
}

void LLGLSLShader::clearStats()
{
    mTrianglesDrawn = 0;
    mTimeElapsed = 0;
    mSamplesDrawn = 0;
    mBinds = 0;
}

void LLGLSLShader::dumpStats(boost::json::object& stats)
{
    stats.emplace("name", mName);
    auto filesit = stats.emplace("files", boost::json::array_kind).first;
    auto& files = filesit->value().as_array();
    LL_INFOS() << "=============================================" << LL_ENDL;
    LL_INFOS() << mName << LL_ENDL;
    for (U32 i = 0; i < mShaderFiles.size(); ++i)
    {
        LL_INFOS() << mShaderFiles[i].first << LL_ENDL;
        files.emplace_back(mShaderFiles[i].first);
    }
    LL_INFOS() << "=============================================" << LL_ENDL;

    constexpr float  mega = 1'000'000.f;
    constexpr double giga = 1'000'000'000.0;
    F32 ms = mTimeElapsed / mega;
    F32 seconds = ms / 1000.f;

    F32 pct_tris = (F32)mTrianglesDrawn / (F32)sTotalTrianglesDrawn * 100.f;
    F32 tris_sec = (F32)(mTrianglesDrawn / mega);
    tris_sec /= seconds;

    F32 pct_samples = (F32)((F64)mSamplesDrawn / (F64)sTotalSamplesDrawn) * 100.f;
    F32 samples_sec = (F32)(mSamplesDrawn / giga);
    samples_sec /= seconds;

    F32 pct_binds = (F32)mBinds / (F32)sTotalBinds * 100.f;

    LL_INFOS() << "Triangles Drawn: " << mTrianglesDrawn << " " << llformat("(%.2f pct of total, %.3f million/sec)", pct_tris, tris_sec) << LL_ENDL;
    LL_INFOS() << "Binds: " << mBinds << " " << llformat("(%.2f pct of total)", pct_binds) << LL_ENDL;
    LL_INFOS() << "SamplesDrawn: " << mSamplesDrawn << " " << llformat("(%.2f pct of total, %.3f billion/sec)", pct_samples, samples_sec) << LL_ENDL;
    LL_INFOS() << "Time Elapsed: " << mTimeElapsed << " " << llformat("(%.2f pct of total, %.5f ms)\n", (F32)((F64)mTimeElapsed / (F64)sTotalTimeElapsed) * 100.f, ms) << LL_ENDL;
    stats.emplace("time", seconds);
    stats.emplace("binds", mBinds);
    stats.emplace("samples", mSamplesDrawn);
    stats.emplace("triangles", mTrianglesDrawn);
}

//static
void LLGLSLShader::startProfile()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    if (sProfileEnabled && sCurBoundShaderPtr)
    {
        sCurBoundShaderPtr->placeProfileQuery();
    }

}

//static
void LLGLSLShader::stopProfile()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    if (sProfileEnabled && sCurBoundShaderPtr)
    {
        sCurBoundShaderPtr->unbind();
    }
}

void LLGLSLShader::placeProfileQuery(bool for_runtime)
{
    if (sProfileEnabled || for_runtime)
    {
        if (mTimerQuery == 0)
        {
            LLVkQuery::create(1, &mSamplesQuery);
            LLVkQuery::create(1, &mTimerQuery);
            LLVkQuery::create(1, &mPrimitivesQuery);
        }

        LLVkQuery::begin(LLVkQuery::TIME_ELAPSED, mTimerQuery);

        if (!for_runtime)
        {
            LLVkQuery::begin(LLVkQuery::SAMPLES_PASSED, mSamplesQuery);
            LLVkQuery::begin(LLVkQuery::PRIMITIVES_GENERATED, mPrimitivesQuery);
        }
    }
}

bool LLGLSLShader::readProfileQuery(bool for_runtime, bool force_read)
{
    if ((sProfileEnabled || for_runtime) && sCanProfile)
    {
        if (!mProfilePending)
        {
            LLVkQuery::end(LLVkQuery::TIME_ELAPSED);
            if (!for_runtime)
            {
                LLVkQuery::end(LLVkQuery::SAMPLES_PASSED);
                LLVkQuery::end(LLVkQuery::PRIMITIVES_GENERATED);
            }
            mProfilePending = for_runtime;
        }

        if (mProfilePending && for_runtime && !force_read)
        {
            if (!LLVkQuery::ready(mTimerQuery))
            {
                return false;
            }
        }

        GLuint64 time_elapsed = LLVkQuery::result(mTimerQuery);
        mTimeElapsed += time_elapsed;
        mProfilePending = false;

        if (!for_runtime)
        {
            GLuint64 samples_passed = LLVkQuery::result(mSamplesQuery);

            GLuint64 primitives_generated = LLVkQuery::result(mPrimitivesQuery);
            sTotalTimeElapsed += time_elapsed;

            sTotalSamplesDrawn += samples_passed;
            mSamplesDrawn += samples_passed;

            U32 tri_count = (U32)primitives_generated / 3;

            mTrianglesDrawn += tri_count;
            sTotalTrianglesDrawn += tri_count;

            sTotalBinds++;
            mBinds++;
        }
    }

    return true;
}



LLGLSLShader::LLGLSLShader()
    : mProgramObject(0),
    mAttributeMask(0),
    mTotalUniformSize(0),
    mActiveTextureChannels(0),
    mShaderLevel(0),
    mShaderGroup(SG_DEFAULT),
    mFeatures(),
    mUniformsDirty(false),
    mTimerQuery(0),
    mSamplesQuery(0),
    mPrimitivesQuery(0)
{

}

LLGLSLShader::~LLGLSLShader()
{
}

void LLGLSLShader::unload()
{
    mShaderFiles.clear();
    mDefines.clear();
    mFeatures = LLShaderFeatures();

    unloadInternal();
}

void LLGLSLShader::unloadInternal()
{
    sInstances.erase(this);
    mirrorDestroy();

    stop_glerror();
    mAttribute.clear();
    mTexture.clear();
    mUniform.clear();

    mAttachedObjects.clear();
    mProgramObject = 0;

    if (mTimerQuery)
    {
        LLVkQuery::destroy(1, &mTimerQuery);
        mTimerQuery = 0;
    }

    if (mSamplesQuery)
    {
        LLVkQuery::destroy(1, &mSamplesQuery);
        mSamplesQuery = 0;
    }

    stop_glerror();
}

#if defined(LL_VULKAN)
void LLGLSLShader::mirrorDestroy()
{
    if (!mVkProgram && !mVkProgramPending && !LLVkMirror::isReady()) return;   // cheap exit when the mirror is off
    if (LLVkAsync::gEnabled)
    {
        // the worker destroys them after everything queued for them so far
        if (mVkProgram)
        {
            LLVkAsync::programDestroy(mVkProgram);
            mVkProgram = nullptr;
        }
        if (mVkProgramPending)
        {
            LLVkAsync::programDestroy(mVkProgramPending);
            mVkProgramPending = nullptr;
        }
        return;
    }
    std::lock_guard<LLVkMirror::Mutex> mirror_lock(LLVkMirror::lock());
    for (LLVk::Program** p : { &mVkProgram, &mVkProgramPending })
    {
        if (*p)
        {
            (*p)->destroy();
            delete *p;
            *p = nullptr;
        }
    }
}

// the program's own uniform index behind a location (a sampler is returned as ~index), -1 when unknown
S32 LLGLSLShader::vkTwinOf(GLint location) const
{
    if (location < 0)
    {
        return -1;
    }
    return location >= SAMPLER_LOCATION_BASE ? ~(S32)(location - SAMPLER_LOCATION_BASE) : (S32)location;
}

void LLGLSLShader::mirrorScalar(GLint location, int kind, std::initializer_list<double> values)
{
    LLVkAsync::frontCount(LLVkAsync::FP_UNIFORM_SENT);
    if (!mVkProgram) return;
    const bool async = LLVkAsync::gEnabled;
    LLVkMirror::PhaseTimer phase_timer(LLVkMirror::PH_UNIFORM);
    std::unique_lock<LLVkMirror::Mutex> mirror_lock(LLVkMirror::lock(), std::defer_lock);
    if (!async)
    {
        mirror_lock.lock();
    }
    const S32 twin = vkTwinOf(location);
    if (twin == -1)
    {
        return;
    }
    if (twin < 0)
    {
        // a sampler: the integer is the texture unit it now reads
        if (async)
        {
            LLVkAsync::samplerUnit(mVkProgram, (unsigned)(~twin), (unsigned)(S32)*values.begin());
            return;
        }
        mVkProgram->setSamplerUnit((size_t)(~twin), (U32)(S32)*values.begin());
        return;
    }
    F32 f[4] = {};
    S32 i[4] = {};
    U32 n = 0;
    for (double v : values)
    {
        if (n < 4)
        {
            f[n] = (F32)v;
            i[n] = (S32)v;
            ++n;
        }
    }
    if (async)
    {
        LLVkAsync::uniformScalar(mVkProgram, (unsigned)twin, kind, n, f, i);
        return;
    }
    if (kind == 0)
    {
        mVkProgram->setFloatsAt((size_t)twin, f, n, 1);
    }
    else
    {
        mVkProgram->setFloatsAt((size_t)twin, reinterpret_cast<const F32*>(i), n, 1);
    }
}

void LLGLSLShader::mirrorVector(GLint location, int kind, U32 components, U32 count, const void* data)
{
    LLVkAsync::frontCount(LLVkAsync::FP_UNIFORM_SENT);
    if (!mVkProgram || !data) return;
    const bool async = LLVkAsync::gEnabled;
    LLVkMirror::PhaseTimer phase_timer(LLVkMirror::PH_UNIFORM);
    std::unique_lock<LLVkMirror::Mutex> mirror_lock(LLVkMirror::lock(), std::defer_lock);
    if (!async)
    {
        mirror_lock.lock();
    }
    const S32 twin = vkTwinOf(location);
    if (twin < 0)
    {
        return;     // unknown, or a sampler array (units are fixed at link time)
    }
    (void)kind;     // floats and ints are copied bit for bit
    if (async)
    {
        LLVkAsync::uniformVector(mVkProgram, (unsigned)twin, components, count, data);
        return;
    }
    mVkProgram->setFloatsAt((size_t)twin, reinterpret_cast<const F32*>(data), components, count);
}

void LLGLSLShader::mirrorMatrix(GLint location, U32 columns, U32 rows, GLboolean transpose, U32 count, const GLfloat* data)
{
    LLVkAsync::frontCount(LLVkAsync::FP_UNIFORM_SENT);
    if (!mVkProgram || !data) return;
    const bool async = LLVkAsync::gEnabled;
    LLVkMirror::PhaseTimer phase_timer(LLVkMirror::PH_UNIFORM);
    std::unique_lock<LLVkMirror::Mutex> mirror_lock(LLVkMirror::lock(), std::defer_lock);
    if (!async)
    {
        mirror_lock.lock();
    }
    const S32 twin = vkTwinOf(location);
    if (twin < 0)
    {
        return;
    }
    if (transpose)
    {
        // the viewer hands the matrices over row-major; the program wants them column-major
        std::vector<F32> tmp((size_t)count * columns * rows);
        for (U32 m = 0; m < count; ++m)
        {
            for (U32 c = 0; c < columns; ++c)
            {
                for (U32 r = 0; r < rows; ++r)
                {
                    tmp[(size_t)m * columns * rows + c * rows + r] = data[(size_t)m * columns * rows + r * columns + c];
                }
            }
        }
        if (async)
        {
            LLVkAsync::uniformMatrix(mVkProgram, (unsigned)twin, columns, rows, count, tmp.data());
            return;
        }
        mVkProgram->setMatricesAt((size_t)twin, tmp.data(), columns, rows, count);
        return;
    }
    if (async)
    {
        LLVkAsync::uniformMatrix(mVkProgram, (unsigned)twin, columns, rows, count, data);
        return;
    }
    mVkProgram->setMatricesAt((size_t)twin, data, columns, rows, count);
}

// the program has been built (link()) and the viewer-side mapping is done: it becomes the one draws use, and its samplers get their texture units
void LLGLSLShader::mirrorCreate()
{
    if (!mVkProgramPending)
    {
        return;
    }
    mVkProgram = mVkProgramPending;
    mVkProgramPending = nullptr;
    LLShaderMgr* mgr = LLShaderMgr::instance();
    const LLVk::ProgramInfo& pi = mVkProgram->getInfo();

    // texture units: the viewer's reserved samplers got their channel in mTexture, "texN" are fixed
    for (U32 r = 0; r < mTexture.size(); ++r)
    {
        if (mTexture[r] >= 0)
        {
            const S32 sampler = mVkProgram->findSampler(mgr->mReservedUniforms[r]);
            if (sampler >= 0)
            {
                mVkProgram->setSamplerUnit((size_t)sampler, (U32)mTexture[r]);
            }
        }
    }
    for (size_t s = 0; s < pi.mSamplers.size(); ++s)
    {
        const std::string& name = pi.mSamplers[s].mName;
        if (name.size() > 3 && name.compare(0, 3, "tex") == 0 && std::isdigit((unsigned char)name[3]))
        {
            mVkProgram->setSamplerUnit(s, (U32)atoi(name.c_str() + 3));
        }
    }
}
#else
void LLGLSLShader::mirrorDestroy() {}
void LLGLSLShader::mirrorCreate() {}
void LLGLSLShader::mirrorScalar(GLint, int, std::initializer_list<double>) {}
void LLGLSLShader::mirrorVector(GLint, int, U32, U32, const void*) {}
void LLGLSLShader::mirrorMatrix(GLint, U32, U32, GLboolean, U32, const GLfloat*) {}
#endif

bool LLGLSLShader::createShader()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    unloadInternal();

    sInstances.insert(this);

    //reloading, reset matrix hash values
    for (U32 i = 0; i < LLRender::NUM_MATRIX_MODES; ++i)
    {
        mMatHash[i] = 0xFFFFFFFF;
    }
    mLightHash = 0xFFFFFFFF;

    llassert_always(!mShaderFiles.empty());

#if LL_DARWIN
    if(!gGLManager.mIsApple)
    {
        // work-around missing mix(vec3,vec3,bvec3)
        mDefines["OLD_SELECT"] = "1";
    }
#endif

    mShaderHash = hash();

    // a program is a handle of our own: the shader objects attached to it are compiled together when it is linked
    static GLuint sNextProgramObject = 0;
    mProgramObject = ++sNextProgramObject;

    bool success = true;

    mUsingBinaryProgram = false;

    if (!mUsingBinaryProgram)
    {
#if DEBUG_SHADER_INCLUDES
        fprintf(stderr, "--- %s ---\n", mName.c_str());
#endif // DEBUG_SHADER_INCLUDES

        //compile new source
        vector< pair<string, GLenum> >::iterator fileIter = mShaderFiles.begin();
        for (; fileIter != mShaderFiles.end(); fileIter++)
        {
            GLuint shaderhandle = LLShaderMgr::instance()->loadShaderFile((*fileIter).first, mShaderLevel, (*fileIter).second, &mDefines, mFeatures.mIndexedTextureChannels);
            LL_DEBUGS("ShaderLoading") << "SHADER FILE: " << (*fileIter).first << " mShaderLevel=" << mShaderLevel << LL_ENDL;
            if (shaderhandle)
            {
                attachObject(shaderhandle);
            }
            else
            {
                success = false;
            }
        }
    }

    // Attach existing objects
    if (!LLShaderMgr::instance()->attachShaderFeatures(this))
    {
        unloadInternal();
        return false;
    }
    // Map attributes and uniforms
    if (success)
    {
        success = mapAttributes();
    }
    if (success)
    {
        success = mapUniforms();
    }
    if (!success)
    {
        LL_SHADER_LOADING_WARNS() << "Failed to link shader: " << mName << LL_ENDL;

        // Try again using a lower shader level;
        if (mShaderLevel > 0)
        {
            LL_SHADER_LOADING_WARNS() << "Failed to link using shader level " << mShaderLevel << " trying again using shader level " << (mShaderLevel - 1) << LL_ENDL;
            mShaderLevel--;
            return createShader();
        }
        else
        {
            // Give up and unload shader.
            unloadInternal();
        }
    }
    else if (mFeatures.mIndexedTextureChannels > 0)
    { //override texture channels for indexed texture rendering
        llassert(mFeatures.mIndexedTextureChannels == LLGLSLShader::sIndexedTextureChannels); // these numbers must always match
        bind();
        S32 channel_count = mFeatures.mIndexedTextureChannels;

        for (S32 i = 0; i < channel_count; i++)
        {
            LLStaticHashedString uniName(llformat("tex%d", i));
            uniform1i(uniName, i);
        }

        //adjust any texture channels that might have been overwritten
        for (U32 i = 0; i < mTexture.size(); i++)
        {
            if (mTexture[i] > -1)
            {
                S32 new_tex = mTexture[i] + channel_count;
                uniform1i(i, new_tex);
                mTexture[i] = new_tex;
            }
        }

        // get the true number of active texture channels
        mActiveTextureChannels = channel_count;
        for (auto& tex : mTexture)
        {
            mActiveTextureChannels = llmax(mActiveTextureChannels, tex + 1);
        }

        // when indexed texture channels are used, enforce an upper limit of 16
        // this should act as a canary in the coal mine for adding textures
        // and breaking machines that are limited to 16 texture channels
        llassert(mActiveTextureChannels <= 16);
        unbind();
    }

#if defined(LL_VULKAN)
    if (success)
    {
        mirrorCreate();
    }
#endif

#if defined(LL_VULKAN)
    if (success)
    {
        // LL_VK_PROGRAM_MAP=<tag>: appends what the viewer sees of this program (which reserved uniform / attribute exists, the program's own uniform index behind it and the
        // texture channel) to run-logs/program-map-<tag>.txt, to compare two builds of the program front end
        static const char* map_tag = getenv("LL_VK_PROGRAM_MAP");
        if (map_tag && mVkProgram)
        {
            static FILE* map_file = nullptr;
            if (!map_file)
            {
                map_file = fopen((std::string("D:/claude/run-logs/program-map-") + map_tag + ".txt").c_str(), "w");
            }
            if (map_file)
            {
                fprintf(map_file, "%s | channels %d | attrmask %x | total %d\n", mName.c_str(), (int)mActiveTextureChannels, (unsigned)mAttributeMask, (int)mTotalUniformSize);
                for (size_t i = 0; i < mUniform.size(); ++i)
                {
                    if (mUniform[i] != -1)
                    {
                        fprintf(map_file, "  u %zu %s twin %d tex %d\n", i, LLShaderMgr::instance()->mReservedUniforms[i].c_str(), vkTwinOf(mUniform[i]), (int)mTexture[i]);
                    }
                }
                for (size_t i = 0; i < mAttribute.size(); ++i)
                {
#if LL_RELEASE_WITH_DEBUG_INFO
                    const GLint loc = mAttribute[i].loc;
#else
                    const GLint loc = mAttribute[i];
#endif
                    if (loc != -1)
                    {
                        fprintf(map_file, "  a %zu %s loc %d\n", i, LLShaderMgr::instance()->mReservedAttribs[i].c_str(), (int)loc);
                    }
                }
                fflush(map_file);
            }
        }
    }
#endif

    LL_DEBUGS("GLSLTextureChannels") << mName << " has " << mActiveTextureChannels << " active texture channels" << LL_ENDL;

    for (U32 i = 0; i < mTexture.size(); i++)
    {
        if (mTexture[i] > -1)
        {
            LL_DEBUGS("GLSLTextureChannels") << "Texture " << LLShaderMgr::instance()->mReservedUniforms[i] << " assigned to channel " << mTexture[i] << LL_ENDL;
        }
    }

#if LL_PROFILER_ENABLE_RENDER_DOC
    setLabel(mName.c_str());
#endif

    return success;
}

#if DEBUG_SHADER_INCLUDES
void dumpAttachObject(const char* func_name, GLuint program_object, const std::string& object_path)
{
    GLchar* info_log;
    GLint      info_len_expect = 0;
    GLint      info_len_actual = 0;

    glGetShaderiv(program_object, GL_INFO_LOG_LENGTH, , &info_len_expect);
    fprintf(stderr, " * %-20s(), log size: %d, %s\n", func_name, info_len_expect, object_path.c_str());

    if (info_len_expect > 0)
    {
        fprintf(stderr, " ========== %s() ========== \n", func_name);
        info_log = new GLchar[info_len_expect];
        glGetProgramInfoLog(program_object, info_len_expect, &info_len_actual, info_log);
        fprintf(stderr, "%s\n", info_log);
        delete[] info_log;
    }
}
#endif // DEBUG_SHADER_INCLUDES

bool LLGLSLShader::attachVertexObject(std::string object_path)
{
    if (LLShaderMgr::instance()->mVertexShaderObjects.count(object_path) > 0)
    {
        mAttachedObjects.push_back(LLShaderMgr::instance()->mVertexShaderObjects[object_path]);
#if DEBUG_SHADER_INCLUDES
        dumpAttachObject("attachVertexObject", mProgramObject, object_path);
#endif // DEBUG_SHADER_INCLUDES
        stop_glerror();
        return true;
    }
    else
    {
        LL_SHADER_LOADING_WARNS() << "Attempting to attach shader object: '" << object_path << "' that hasn't been compiled." << LL_ENDL;
        return false;
    }
}

bool LLGLSLShader::attachFragmentObject(std::string object_path)
{
    if(mUsingBinaryProgram)
        return true;

    if (LLShaderMgr::instance()->mFragmentShaderObjects.count(object_path) > 0)
    {
        mAttachedObjects.push_back(LLShaderMgr::instance()->mFragmentShaderObjects[object_path]);
#if DEBUG_SHADER_INCLUDES
        dumpAttachObject("attachFragmentObject", mProgramObject, object_path);
#endif // DEBUG_SHADER_INCLUDES
        stop_glerror();
        return true;
    }
    else
    {
        LL_SHADER_LOADING_WARNS() << "Attempting to attach shader object: '" << object_path << "' that hasn't been compiled." << LL_ENDL;
        return false;
    }
}

void LLGLSLShader::attachObject(GLuint object)
{
    if(mUsingBinaryProgram)
        return;

    if (object != 0)
    {
        mAttachedObjects.push_back(object);
#if DEBUG_SHADER_INCLUDES
        std::string object_path("???");
        dumpAttachObject("attachObject", mProgramObject, object_path);
#endif // DEBUG_SHADER_INCLUDES
        stop_glerror();
    }
    else
    {
        LL_SHADER_LOADING_WARNS() << "Attempting to attach non existing shader object. " << LL_ENDL;
    }
}

void LLGLSLShader::attachObjects(GLuint* objects, S32 count)
{
    if(mUsingBinaryProgram)
        return;

    for (S32 i = 0; i < count; i++)
    {
        attachObject(objects[i]);
    }
}

bool LLGLSLShader::mapAttributes()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    bool res = true;
    if (!mUsingBinaryProgram)
    {
        //link the program
        res = link();
    }

    mAttribute.clear();
#if LL_RELEASE_WITH_DEBUG_INFO
    mAttribute.resize(LLShaderMgr::instance()->mReservedAttribs.size(), { -1, NULL });
#else
    mAttribute.resize(LLShaderMgr::instance()->mReservedAttribs.size(), -1);
#endif

    if (res)
    { //read back channel locations

        mAttributeMask = 0;

        //read back reserved channels first (the program's attribute locations are the viewer's reserved indices)
        const LLVk::ProgramInfo& pinfo = mVkProgramPending->getInfo();
        for (U32 i = 0; i < LLShaderMgr::instance()->mReservedAttribs.size(); i++)
        {
            const char* name = LLShaderMgr::instance()->mReservedAttribs[i].c_str();
            S32 index = -1;
            for (size_t a = 0; a < pinfo.mAttributes.size() && a < pinfo.mAttributeLocations.size(); ++a)
            {
                if (pinfo.mAttributes[a] == name)
                {
                    index = (S32)pinfo.mAttributeLocations[a];
                    break;
                }
            }
            if (index != -1)
            {
#if LL_RELEASE_WITH_DEBUG_INFO
                mAttribute[i] = { index, name };
#else
                mAttribute[i] = index;
#endif
                mAttributeMask |= 1 << i;
                LL_DEBUGS("ShaderUniform") << "Attribute " << name << " assigned to channel " << index << LL_ENDL;
            }
        }

        return true;
    }

    return false;
}

// the GL enum of a GLSL type name (what the mapping below tests for samplers)
static GLenum glsl_type_enum(const std::string& t)
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

void LLGLSLShader::mapUniform(GLint index)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    if (index == -1)
    {
        return;
    }

    if (index < 0 || index >= (GLint)mActiveUniforms.size())
    {
        return;
    }
    const ActiveUniform& active = mActiveUniforms[index];
    const GLenum type = glsl_type_enum(active.mType);
    GLint size = active.mSize;
    char name[1024];        /* Flawfinder: ignore */
    strncpy(name, active.mName.c_str(), sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;

    if (size > 0)
    {
        switch (type)
        {
        case GL_FLOAT_VEC2: size *= 2; break;
        case GL_FLOAT_VEC3: size *= 3; break;
        case GL_FLOAT_VEC4: size *= 4; break;
        case GL_INT_VEC2: size *= 2; break;
        case GL_INT_VEC3: size *= 3; break;
        case GL_INT_VEC4: size *= 4; break;
        case GL_UNSIGNED_INT_VEC2: size *= 2; break;
        case GL_UNSIGNED_INT_VEC3: size *= 3; break;
        case GL_UNSIGNED_INT_VEC4: size *= 4; break;
        case GL_BOOL_VEC2: size *= 2; break;
        case GL_BOOL_VEC3: size *= 3; break;
        case GL_BOOL_VEC4: size *= 4; break;
        case GL_FLOAT_MAT2: size *= 4; break;
        case GL_FLOAT_MAT3: size *= 9; break;
        case GL_FLOAT_MAT4: size *= 16; break;
        case GL_FLOAT_MAT2x3: size *= 6; break;
        case GL_FLOAT_MAT2x4: size *= 8; break;
        case GL_FLOAT_MAT3x2: size *= 6; break;
        case GL_FLOAT_MAT3x4: size *= 12; break;
        case GL_FLOAT_MAT4x2: size *= 8; break;
        case GL_FLOAT_MAT4x3: size *= 12; break;
        }
        mTotalUniformSize += size;
    }

    S32 location = active.mLocation;
    if (location != -1)
    {
        //chop off "[0]" so we can always access the first element
        //of an array by the array name
        char* is_array = strstr(name, "[0]");
        if (is_array)
        {
            is_array[0] = 0;
        }

        LLStaticHashedString hashedName(name);
        mUniformMap[hashedName] = location;

        LL_DEBUGS("ShaderUniform") << "Uniform " << name << " is at location " << location << LL_ENDL;

        //find the index of this uniform
        for (S32 i = 0; i < (S32)LLShaderMgr::instance()->mReservedUniforms.size(); i++)
        {
            if ((mUniform[i] == -1)
                && (LLShaderMgr::instance()->mReservedUniforms[i] == name))
            {
                //found it
                mUniform[i] = location;
                mTexture[i] = mapUniformTextureChannel(location, type, size);
                if (mTexture[i] != -1)
                {
                    LL_DEBUGS("GLSLTextureChannels") << name << " assigned to texture channel " << mTexture[i] << LL_ENDL;
                }
                return;
            }
        }
    }
}

void LLGLSLShader::clearPermutations()
{
    mDefines.clear();
}

void LLGLSLShader::addPermutation(std::string name, std::string value)
{
    mDefines[name] = value;
}

void LLGLSLShader::addConstant(const LLGLSLShader::eShaderConsts shader_const)
{
    addPermutation(gShaderConstsKey[shader_const], gShaderConstsVal[shader_const]);
}

void LLGLSLShader::removePermutation(std::string name)
{
    mDefines.erase(name);
}

GLint LLGLSLShader::mapUniformTextureChannel(GLint location, GLenum type, GLint size)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    if ((type >= GL_SAMPLER_1D && type <= GL_SAMPLER_2D_RECT_SHADOW) ||
        type == GL_SAMPLER_2D_MULTISAMPLE ||
        type == GL_SAMPLER_CUBE_MAP_ARRAY)
    {   //this here is a texture
        GLint ret = mActiveTextureChannels;
        if (size == 1)
        {
            mirrorScalar(location, 1, {(double)(mActiveTextureChannels)});
            mActiveTextureChannels++;
        }
        else
        {
            //is array of textures, make sequential after this texture
            GLint channel[16]; // <=== only support up to 16 texture channels
            llassert(size <= 16);
            size = llmin(size, 16);
            for (int i = 0; i < size; ++i)
            {
                channel[i] = mActiveTextureChannels++;
            }
            mirrorVector(location, 1, 1, size, channel);
        }

        return ret;
    }
    return -1;
}

bool LLGLSLShader::mapUniforms()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    bool res = true;

    mTotalUniformSize = 0;
    mActiveTextureChannels = 0;
    mUniform.clear();
    mUniformMap.clear();
    mTexture.clear();
    mValue.clear();
    //initialize arrays
    mUniform.resize(LLShaderMgr::instance()->mReservedUniforms.size(), -1);
    mTexture.resize(LLShaderMgr::instance()->mReservedUniforms.size(), -1);

    bind();

    // what the program reports as active: its loose uniforms (arrays by their first element), then its samplers
    mActiveUniforms.clear();
    {
        const LLVk::ProgramInfo& pinfo = mVkProgramPending->getInfo();
        for (size_t u = 0; u < pinfo.mUniforms.size(); ++u)
        {
            const LLVk::LooseUniform& lu = pinfo.mUniforms[u];
            mActiveUniforms.push_back({ lu.mArraySize ? lu.mName + "[0]" : lu.mName, lu.mType, lu.mArraySize ? (GLint)lu.mArraySize : 1, (GLint)u });
        }
        for (size_t sm = 0; sm < pinfo.mSamplers.size(); ++sm)
        {
            mActiveUniforms.push_back({ pinfo.mSamplers[sm].mName, pinfo.mSamplers[sm].mType, 1, SAMPLER_LOCATION_BASE + (GLint)sm });
        }
    }
    const GLint activeCount = (GLint)mActiveUniforms.size();
    auto location_of = [this](const char* name) -> S32
    {
        for (const ActiveUniform& a : mActiveUniforms)
        {
            if (a.mName == name)
            {
                return a.mLocation;
            }
        }
        return -1;
    };

    //........................................................................................................................................
    //........................................................................................

    /*
    EXPLANATION:
    This is part of code is temporary because as the final result the mapUniform() should be rewrited.
    But it's a huge a volume of work which is need to be a more carefully performed for avoid possible
    regression's (i.e. it should be formalized a separate ticket in JIRA).

    RESON:
    The reason of this code is that SL engine is very sensitive to fact that "diffuseMap" should be appear
    first as uniform parameter which is should get 0-"texture channel" index (see mapUniformTextureChannel() and mActiveTextureChannels)
    it influence to which is texture matrix will be updated during rendering.

    But, order of indexe's of uniform variables is not defined and GLSL compiler can change it as want
    , even if the "diffuseMap" will be appear and use first in shader code.

    As example where this situation appear see: "Deferred Material Shader 28/29/30/31"
    And tickets: MAINT-4165, MAINT-4839, MAINT-3568, MAINT-6437

    --- davep TODO -- pretty sure the entire block here is superstitious and that the uniform index has nothing to do with the texture channel
                texture channel should follow the uniform VALUE
    */


    S32 diffuseMap = location_of("diffuseMap");
    S32 specularMap = location_of("specularMap");
    S32 bumpMap = location_of("bumpMap");
    S32 altDiffuseMap = location_of("altDiffuseMap");
    S32 environmentMap = location_of("environmentMap");
    S32 reflectionMap = location_of("reflectionMap");

    std::set<S32> skip_index;

    if (-1 != diffuseMap && (-1 != specularMap || -1 != bumpMap || -1 != environmentMap || -1 != altDiffuseMap))
    {
        diffuseMap = altDiffuseMap = specularMap = bumpMap = environmentMap = -1;

        for (S32 i = 0; i < activeCount; i++)
        {
            const std::string& name = mActiveUniforms[i].mName;

            if (-1 == diffuseMap && name == "diffuseMap")
            {
                diffuseMap = i;
                continue;
            }

            if (-1 == specularMap && name == "specularMap")
            {
                specularMap = i;
                continue;
            }

            if (-1 == bumpMap && name == "bumpMap")
            {
                bumpMap = i;
                continue;
            }

            if (-1 == environmentMap && name == "environmentMap")
            {
                environmentMap = i;
                continue;
            }

            if (-1 == reflectionMap && name == "reflectionMap")
            {
                reflectionMap = i;
                continue;
            }

            if (-1 == altDiffuseMap && name == "altDiffuseMap")
            {
                altDiffuseMap = i;
                continue;
            }
        }

        bool specularDiff = specularMap < diffuseMap && -1 != specularMap;
        bool bumpLessDiff = bumpMap < diffuseMap && -1 != bumpMap;
        bool envLessDiff = environmentMap < diffuseMap && -1 != environmentMap;
        bool refLessDiff = reflectionMap < diffuseMap && -1 != reflectionMap;

        if (specularDiff || bumpLessDiff || envLessDiff || refLessDiff)
        {
            mapUniform(diffuseMap);
            skip_index.insert(diffuseMap);

            if (-1 != specularMap) {
                mapUniform(specularMap);
                skip_index.insert(specularMap);
            }

            if (-1 != bumpMap) {
                mapUniform(bumpMap);
                skip_index.insert(bumpMap);
            }

            if (-1 != environmentMap) {
                mapUniform(environmentMap);
                skip_index.insert(environmentMap);
            }

            if (-1 != reflectionMap) {
                mapUniform(reflectionMap);
                skip_index.insert(reflectionMap);
            }
        }
    }

    //........................................................................................

    for (S32 i = 0; i < activeCount; i++)
    {
        //........................................................................................
        if (skip_index.end() != skip_index.find(i)) continue;
        //........................................................................................

        mapUniform(i);
    }
    //........................................................................................................................................

    // (the named uniform blocks are bound by index when the program is compiled)

    unbind();

    LL_DEBUGS("ShaderUniform") << "Total Uniform Size: " << mTotalUniformSize << LL_ENDL;
    return res;
}


bool LLGLSLShader::link(bool suppress_errors)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    // the attached shader objects are compiled together into one program (the build runs on the render thread, which owns Vulkan)
    LLShaderMgr* mgr = LLShaderMgr::instance();
    std::vector<LLVk::StageSource> stages;
    for (GLuint object : mAttachedObjects)
    {
        auto it = mgr->mRecordedShaders.find(object);
        if (it == mgr->mRecordedShaders.end())
        {
            continue;
        }
        LLVk::StageSource s;
        s.mName = it->second.mFile;
        s.mSource = it->second.mSource;
        switch (it->second.mType)
        {
        case GL_VERTEX_SHADER: s.mStage = LLVk::StageSource::Vertex; break;
        case GL_FRAGMENT_SHADER: s.mStage = LLVk::StageSource::Fragment; break;
        case GL_GEOMETRY_SHADER: s.mStage = LLVk::StageSource::Geometry; break;
        default: continue;
        }
        stages.push_back(std::move(s));
    }
    LLVk::ShaderEnv env;
    env.mAttributeNames = mgr->mReservedAttribs;
    env.mUniformBlockNames = { "ReflectionProbes", "GLTFJoints", "GLTFNodes", "GLTFMaterials" };
    env.mBindless = LLVk::bindlessShaders();
    const char* dump = getenv("LL_VK_SHADER_DUMP_DIR");

    LLVk::Program* program = nullptr;
    std::string error;
    auto build = [&]()
    {
        std::lock_guard<LLVkMirror::Mutex> mirror_lock(LLVkMirror::lock());
        if (!LLVkMirror::ensureReady())
        {
            error = "the Vulkan side is not ready";
            return;
        }
        LLVk::ProgramInfo info;
        if (!LLVk::compileProgram(mName, stages, env, info, dump ? dump : ""))
        {
            error = info.mError;
            return;
        }
        std::unique_ptr<LLVk::Program> made(new LLVk::Program);
        if (!made->create(mName, info))
        {
            error = "the program could not be created";
            return;
        }
        program = made.release();
    };
    if (stages.empty())
    {
        error = "no shader objects attached";
    }
    else if (LLVkAsync::gEnabled)
    {
        LLVkAsync::syncCall(build);
    }
    else
    {
        build();
    }

    if (!program)
    {
        if (!suppress_errors)
        {
            LL_SHADER_LOADING_WARNS() << "GLSL Linker Error: program " << mName << " does not compile for Vulkan: " << error.substr(0, 600) << LL_ENDL;
        }
        return false;
    }
    mVkProgramPending = program;
    return true;
}

void LLGLSLShader::bind()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_SHADER_BIND);

    llassert_always(mProgramObject != 0);

    gGL.flush();

    if (sCurBoundShader != mProgramObject)  // Don't re-bind current shader
    {
        if (sCurBoundShaderPtr)
        {
            sCurBoundShaderPtr->readProfileQuery();
        }
        // <FS> one record for the buffer unbinds, the program bind and the attribute array switches
        LLVertexBuffer::forgetBoundBuffers();
        sCurBoundShader = mProgramObject;
        sCurBoundShaderPtr = this;
        U32 enable = 0, disable = 0;
        LLVertexBuffer::arrayMaskChange(mAttributeMask, enable, disable);
        LLVkAsync::shaderBind(mVkProgram, enable, disable);
        placeProfileQuery();
    }

    if (mUniformsDirty)
    {
        LLShaderMgr::instance()->updateShaderUniforms(this);
        mUniformsDirty = false;
    }

    llassert_always(sCurBoundShaderPtr != nullptr);
    llassert_always(sCurBoundShader == mProgramObject);
}

void LLGLSLShader::bind(U8 variant)
{
    llassert_always(mGLTFVariants.size() == LLGLSLShader::NUM_GLTF_VARIANTS);
    llassert_always(variant < LLGLSLShader::NUM_GLTF_VARIANTS);
    mGLTFVariants[variant].bind();
}

void LLGLSLShader::bind(bool rigged)
{
    if (rigged)
    {
        llassert_always(mRiggedVariant);
        mRiggedVariant->bind();
    }
    else
    {
        bind();
    }
}

void LLGLSLShader::unbind(void)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    gGL.flush();
    LLVertexBuffer::unbind();

    if (sCurBoundShaderPtr)
    {
        sCurBoundShaderPtr->readProfileQuery();
    }

    sCurBoundShader = 0;
    sCurBoundShaderPtr = NULL;
#if defined(LL_VULKAN)
    if (LLVkMirror::isReady())
    {
        if (LLVkAsync::gEnabled)
        {
            LLVkAsync::programBind(nullptr);        // in order with the draws already queued
        }
        else
        {
            LLVk::Renderer::instance().setProgram(nullptr);
        }
    }
#endif
}

S32 LLGLSLShader::bindTexture(const std::string& uniform, LLTexture* texture, LLTexUnit::eTextureType mode)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    S32 channel = 0;
    channel = getUniformLocation(uniform);

    return bindTexture(channel, texture, mode);
}

S32 LLGLSLShader::bindTexture(S32 uniform, LLTexture* texture, LLTexUnit::eTextureType mode)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    if (uniform < 0 || uniform >= (S32)mTexture.size())
    {
        LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << uniform << LL_ENDL;
        llassert(false);
        return -1;
    }

    uniform = mTexture[uniform];

    if (uniform > -1)
    {
        gGL.getTexUnit(uniform)->bindFast(texture);
    }

    return uniform;
}

S32 LLGLSLShader::bindTexture(S32 uniform, LLRenderTarget* texture, bool depth, LLTexUnit::eTextureFilterOptions mode, U32 index)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    if (uniform < 0 || uniform >= (S32)mTexture.size())
    {
        LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << uniform << LL_ENDL;
        llassert(false);
        return -1;
    }

    uniform = getTextureChannel(uniform);

    if (uniform > -1)
    {
        if (depth) {
            gGL.getTexUnit(uniform)->bind(texture, true);
        }
        else {
            bool has_mips = mode == LLTexUnit::TFO_TRILINEAR || mode == LLTexUnit::TFO_ANISOTROPIC;
            gGL.getTexUnit(uniform)->bindManual(texture->getUsage(), texture->getTexture(index), has_mips);
        }

        gGL.getTexUnit(uniform)->setTextureFilteringOption(mode);
    }

    return uniform;
}

S32 LLGLSLShader::bindTexture(const std::string& uniform, LLRenderTarget* texture, bool depth, LLTexUnit::eTextureFilterOptions mode)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    S32 channel = 0;
    channel = getUniformLocation(uniform);

    return bindTexture(channel, texture, depth, mode);
}

S32 LLGLSLShader::unbindTexture(const std::string& uniform, LLTexUnit::eTextureType mode)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    S32 channel = 0;
    channel = getUniformLocation(uniform);

    return unbindTexture(channel);
}

S32 LLGLSLShader::unbindTexture(S32 uniform, LLTexUnit::eTextureType mode)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    if (uniform < 0 || uniform >= (S32)mTexture.size())
    {
        LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << uniform << LL_ENDL;
        llassert(false);
        return -1;
    }

    uniform = mTexture[uniform];

    if (uniform > -1)
    {
        gGL.getTexUnit(uniform)->unbindFast(mode);
    }

    return uniform;
}

S32 LLGLSLShader::getTextureChannel(S32 uniform) const
{
    return mTexture[uniform];
}

S32 LLGLSLShader::enableTexture(S32 uniform, LLTexUnit::eTextureType mode)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    if (uniform < 0 || uniform >= (S32)mTexture.size())
    {
        LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << uniform << LL_ENDL;
        llassert(false);
        return -1;
    }


    S32 index = mTexture[uniform];
    if (index != -1)
    {
        gGL.getTexUnit(index)->activate();
        gGL.getTexUnit(index)->enable(mode);
    }
    return index;
}

S32 LLGLSLShader::disableTexture(S32 uniform, LLTexUnit::eTextureType mode)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    if (uniform < 0 || uniform >= (S32)mTexture.size())
    {
        LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << uniform << LL_ENDL;
        llassert(false);
        return -1;
    }

    S32 index = mTexture[uniform];
    if (index < 0)
    {
        // Invalid texture index - nothing to disable
        return index;
    }

    LLTexUnit* tex_unit = gGL.getTexUnit(index);
    if (!tex_unit)
    {
        // Invalid texture unit
        LL_WARNS_ONCE("Shader") << "Invalid texture unit at index: " << index << LL_ENDL;
        return index;
    }

    LLTexUnit::eTextureType curr_type = tex_unit->getCurrType();
    if (curr_type != LLTexUnit::TT_NONE)
    {
        if (gDebugGL && curr_type != mode)
        {
            if (gDebugSession)
            {
                gFailLog << "Texture channel " << index << " texture type corrupted. Expected: " << mode << ", Found: " << curr_type << std::endl;
                ll_fail("LLGLSLShader::disableTexture failed");
            }
            else
            {
                LL_ERRS() << "Texture channel " << index << " texture type corrupted. Expected: " << mode << ", Found: " << curr_type << LL_ENDL;
            }
        }
        tex_unit->disable();
    }

    return index;
}

void LLGLSLShader::uniform1i(U32 index, GLint x)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    llassert(sCurBoundShaderPtr == this);
    if (mProgramObject)
    {
        if (mUniform.size() <= index)
        {
            LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << index << LL_ENDL;
            llassert(false);
            return;
        }

        if (mUniform[index] >= 0)
        {
            const auto& iter = mValue.find(mUniform[index]);
            if (iter == mValue.end() || iter->second.mV[0] != x)
            {
                mirrorScalar(mUniform[index], 1, {(double)(x)});
                mValue[mUniform[index]] = LLVector4((F32)x, 0.f, 0.f, 0.f);
            }
        }
    }
}

void LLGLSLShader::uniform1fAt(S32 location, F32 v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    if (sCurBoundShaderPtr) sCurBoundShaderPtr->mirrorScalar(location, 0, {(double)(v)});
}

void LLGLSLShader::uniform4fvAt(S32 location, S32 count, const F32* v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    if (sCurBoundShaderPtr) sCurBoundShaderPtr->mirrorVector(location, 0, 4, count, v);
}

void LLGLSLShader::uniform1f(U32 index, GLfloat x)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    llassert(sCurBoundShaderPtr == this);

    if (mProgramObject)
    {
        if (mUniform.size() <= index)
        {
            LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << index << LL_ENDL;
            llassert(false);
            return;
        }

        if (mUniform[index] >= 0)
        {
            const auto& iter = mValue.find(mUniform[index]);
            if (iter == mValue.end() || iter->second.mV[0] != x)
            {
                mirrorScalar(mUniform[index], 0, {(double)(x)});
                mValue[mUniform[index]] = LLVector4(x, 0.f, 0.f, 0.f);
            }
        }
    }
}

void LLGLSLShader::fastUniform1f(U32 index, GLfloat x)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    llassert(sCurBoundShaderPtr == this);
    llassert(mProgramObject);
    llassert(mUniform.size() <= index);
    llassert(mUniform[index] >= 0);
    mirrorScalar(mUniform[index], 0, {(double)(x)});
}

void LLGLSLShader::uniform2f(U32 index, GLfloat x, GLfloat y)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    llassert(sCurBoundShaderPtr == this);

    if (mProgramObject)
    {
        if (mUniform.size() <= index)
        {
            LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << index << LL_ENDL;
            llassert(false);
            return;
        }

        if (mUniform[index] >= 0)
        {
            const auto& iter = mValue.find(mUniform[index]);
            LLVector4 vec(x, y, 0.f, 0.f);
            if (iter == mValue.end() || shouldChange(iter->second, vec))
            {
                mirrorScalar(mUniform[index], 0, {(double)(x), (double)(y)});
                mValue[mUniform[index]] = vec;
            }
        }
    }
}

void LLGLSLShader::uniform3f(U32 index, GLfloat x, GLfloat y, GLfloat z)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    llassert(sCurBoundShaderPtr == this);

    if (mProgramObject)
    {
        if (mUniform.size() <= index)
        {
            LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << index << LL_ENDL;
            llassert(false);
            return;
        }

        if (mUniform[index] >= 0)
        {
            const auto& iter = mValue.find(mUniform[index]);
            LLVector4 vec(x, y, z, 0.f);
            if (iter == mValue.end() || shouldChange(iter->second, vec))
            {
                mirrorScalar(mUniform[index], 0, {(double)(x), (double)(y), (double)(z)});
                mValue[mUniform[index]] = vec;
            }
        }
    }
}

void LLGLSLShader::uniform4f(U32 index, GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    llassert(sCurBoundShaderPtr == this);

    if (mProgramObject)
    {
        if (mUniform.size() <= index)
        {
            LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << index << LL_ENDL;
            llassert(false);
            return;
        }

        if (mUniform[index] >= 0)
        {
            const auto& iter = mValue.find(mUniform[index]);
            LLVector4 vec(x, y, z, w);
            if (iter == mValue.end() || shouldChange(iter->second, vec))
            {
                mirrorScalar(mUniform[index], 0, {(double)(x), (double)(y), (double)(z), (double)(w)});
                mValue[mUniform[index]] = vec;
            }
        }
    }
}

void LLGLSLShader::uniform1iv(U32 index, U32 count, const GLint* v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    llassert(sCurBoundShaderPtr == this);

    if (mProgramObject)
    {
        if (mUniform.size() <= index)
        {
            LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << index << LL_ENDL;
            llassert(false);
            return;
        }

        if (mUniform[index] >= 0)
        {
            const auto& iter = mValue.find(mUniform[index]);
            LLVector4 vec((F32)v[0], 0.f, 0.f, 0.f);
            if (iter == mValue.end() || shouldChange(iter->second, vec) || count != 1)
            {
                mirrorVector(mUniform[index], 1, 1, count, v);
                mValue[mUniform[index]] = vec;
            }
        }
    }
}

void LLGLSLShader::uniform4iv(U32 index, U32 count, const GLint* v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    llassert(sCurBoundShaderPtr == this);

    if (mProgramObject)
    {
        if (mUniform.size() <= index)
        {
            LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << index << LL_ENDL;
            llassert(false);
            return;
        }

        if (mUniform[index] >= 0)
        {
            const auto& iter = mValue.find(mUniform[index]);
            LLVector4 vec((F32)v[0], (F32)v[1], (F32)v[2], (F32)v[3]);
            if (iter == mValue.end() || shouldChange(iter->second, vec) || count != 1)
            {
                mValue[mUniform[index]] = vec;
            }
        }
    }
}


void LLGLSLShader::uniform1fv(U32 index, U32 count, const GLfloat* v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    llassert(sCurBoundShaderPtr == this);

    if (mProgramObject)
    {
        if (mUniform.size() <= index)
        {
            LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << index << LL_ENDL;
            llassert(false);
            return;
        }

        if (mUniform[index] >= 0)
        {
            const auto& iter = mValue.find(mUniform[index]);
            LLVector4 vec(v[0], 0.f, 0.f, 0.f);
            if (iter == mValue.end() || shouldChange(iter->second, vec) || count != 1)
            {
                mirrorVector(mUniform[index], 0, 1, count, v);
                mValue[mUniform[index]] = vec;
            }
        }
    }
}

void LLGLSLShader::uniform2fv(U32 index, U32 count, const GLfloat* v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    llassert(sCurBoundShaderPtr == this);

    if (mProgramObject)
    {
        if (mUniform.size() <= index)
        {
            LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << index << LL_ENDL;
            llassert(false);
            return;
        }

        if (mUniform[index] >= 0)
        {
            const auto& iter = mValue.find(mUniform[index]);
            LLVector4 vec(v[0], v[1], 0.f, 0.f);
            if (iter == mValue.end() || shouldChange(iter->second, vec) || count != 1)
            {
                mirrorVector(mUniform[index], 0, 2, count, v);
                mValue[mUniform[index]] = vec;
            }
        }
    }
}

void LLGLSLShader::uniform3fv(U32 index, U32 count, const GLfloat* v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    llassert(sCurBoundShaderPtr == this);

    if (mProgramObject)
    {
        if (mUniform.size() <= index)
        {
            LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << index << LL_ENDL;
            llassert(false);
            return;
        }

        if (mUniform[index] >= 0)
        {
            const auto& iter = mValue.find(mUniform[index]);
            LLVector4 vec(v[0], v[1], v[2], 0.f);
            if (iter == mValue.end() || shouldChange(iter->second, vec) || count != 1)
            {
                mirrorVector(mUniform[index], 0, 3, count, v);
                mValue[mUniform[index]] = vec;
            }
        }
    }
}

void LLGLSLShader::uniform4fv(U32 index, U32 count, const GLfloat* v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    llassert(sCurBoundShaderPtr == this);

    if (mProgramObject)
    {
        if (mUniform.size() <= index)
        {
            LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << index << LL_ENDL;
            llassert(false);
            return;
        }

        if (mUniform[index] >= 0)
        {
            const auto& iter = mValue.find(mUniform[index]);
            LLVector4 vec(v[0], v[1], v[2], v[3]);
            if (iter == mValue.end() || shouldChange(iter->second, vec) || count != 1)
            {
                LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
                mirrorVector(mUniform[index], 0, 4, count, v);
                mValue[mUniform[index]] = vec;
            }
        }
    }
}

void LLGLSLShader::uniform4uiv(U32 index, U32 count, const GLuint* v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    llassert(sCurBoundShaderPtr == this);

    if (mProgramObject)
    {
        if (mUniform.size() <= index)
        {
            LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << index << LL_ENDL;
            llassert(false);
            return;
        }

        if (mUniform[index] >= 0)
        {
            const auto& iter = mValue.find(mUniform[index]);
            LLVector4 vec((F32)v[0], (F32)v[1], (F32)v[2], (F32)v[3]);
            if (iter == mValue.end() || shouldChange(iter->second, vec) || count != 1)
            {
                LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
                mirrorVector(mUniform[index], 1, 4, count, v);
                mValue[mUniform[index]] = vec;
            }
        }
    }
}

void LLGLSLShader::uniformMatrix2fv(U32 index, U32 count, GLboolean transpose, const GLfloat* v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM_MATRIX);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    llassert(sCurBoundShaderPtr == this);

    if (mProgramObject)
    {
        if (mUniform.size() <= index)
        {
            LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << index << LL_ENDL;
            llassert(false);
            return;
        }

        if (mUniform[index] >= 0)
        {
            mirrorMatrix(mUniform[index], 2, 2, transpose, count, v);
        }
    }
}

void LLGLSLShader::uniformMatrix3fv(U32 index, U32 count, GLboolean transpose, const GLfloat* v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM_MATRIX);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    llassert(sCurBoundShaderPtr == this);

    if (mProgramObject)
    {
        if (mUniform.size() <= index)
        {
            LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << index << LL_ENDL;
            llassert(false);
            return;
        }

        if (mUniform[index] >= 0)
        {
            mirrorMatrix(mUniform[index], 3, 3, transpose, count, v);
        }
    }
}

void LLGLSLShader::uniformMatrix3x4fv(U32 index, U32 count, GLboolean transpose, const GLfloat* v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM_MATRIX);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    llassert(sCurBoundShaderPtr == this);

    if (mProgramObject)
    {
        if (mUniform.size() <= index)
        {
            LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << index << LL_ENDL;
            llassert(false);
            return;
        }

        if (mUniform[index] >= 0)
        {
            mirrorMatrix(mUniform[index], 3, 4, transpose, count, v);
        }
    }
}

void LLGLSLShader::uniformMatrix4fv(U32 index, U32 count, GLboolean transpose, const GLfloat* v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM_MATRIX);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    llassert(sCurBoundShaderPtr == this);

    if (mProgramObject)
    {
        if (mUniform.size() <= index)
        {
            LL_WARNS_ONCE("Shader") << "Uniform index out of bounds. Size: " << (S32)mUniform.size() << " index: " << index << LL_ENDL;
            llassert(false);
            return;
        }

        if (mUniform[index] >= 0)
        {
            mirrorMatrix(mUniform[index], 4, 4, transpose, count, v);
        }
    }
}

GLint LLGLSLShader::getUniformLocation(const LLStaticHashedString& uniform)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    GLint ret = -1;
    if (mProgramObject)
    {
        LLStaticStringTable<GLint>::iterator iter = mUniformMap.find(uniform);
        if (iter != mUniformMap.end())
        {
            ret = iter->second;
        }
    }

    return ret;
}

GLint LLGLSLShader::getUniformLocation(U32 index)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    GLint ret = -1;
    if (mProgramObject)
    {
        if (index >= mUniform.size())
        {
            LL_WARNS_ONCE("Shader") << "Uniform index " << index << " out of bounds " << (S32)mUniform.size() << LL_ENDL;
            return ret;
        }
        return mUniform[index];
    }

    return ret;
}

GLint LLGLSLShader::getAttribLocation(U32 attrib)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    if (attrib < mAttribute.size())
    {
        return mAttribute[attrib];
    }
    else
    {
        return -1;
    }
}

void LLGLSLShader::uniform1i(const LLStaticHashedString& uniform, GLint v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    GLint location = getUniformLocation(uniform);

    if (location >= 0)
    {
        const auto& iter = mValue.find(location);
        LLVector4 vec((F32)v, 0.f, 0.f, 0.f);
        if (iter == mValue.end() || shouldChange(iter->second, vec))
        {
            mirrorScalar(location, 1, {(double)(v)});
            mValue[location] = vec;
        }
    }
}

void LLGLSLShader::uniform1iv(const LLStaticHashedString& uniform, U32 count, const GLint* v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    GLint location = getUniformLocation(uniform);

    if (location >= 0)
    {
        LLVector4 vec((F32)v[0], 0.f, 0.f, 0.f);
        const auto& iter = mValue.find(location);
        if (iter == mValue.end() || shouldChange(iter->second, vec) || count != 1)
        {
            LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
            mirrorVector(location, 1, 1, count, v);
            mValue[location] = vec;
        }
    }
}

void LLGLSLShader::uniform4iv(const LLStaticHashedString& uniform, U32 count, const GLint* v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    GLint location = getUniformLocation(uniform);

    if (location >= 0)
    {
        LLVector4 vec((F32)v[0], (F32)v[1], (F32)v[2], (F32)v[3]);
        const auto& iter = mValue.find(location);
        if (iter == mValue.end() || shouldChange(iter->second, vec) || count != 1)
        {
            LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
            mirrorVector(location, 1, 4, count, v);
            mValue[location] = vec;
        }
    }
}

void LLGLSLShader::uniform2i(const LLStaticHashedString& uniform, GLint i, GLint j)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    GLint location = getUniformLocation(uniform);

    if (location >= 0)
    {
        const auto& iter = mValue.find(location);
        LLVector4 vec((F32)i, (F32)j, 0.f, 0.f);
        if (iter == mValue.end() || shouldChange(iter->second, vec))
        {
            mirrorScalar(location, 1, {(double)(i), (double)(j)});
            mValue[location] = vec;
        }
    }
}


void LLGLSLShader::uniform1f(const LLStaticHashedString& uniform, GLfloat v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    GLint location = getUniformLocation(uniform);

    if (location >= 0)
    {
        const auto& iter = mValue.find(location);
        LLVector4 vec(v, 0.f, 0.f, 0.f);
        if (iter == mValue.end() || shouldChange(iter->second, vec))
        {
            mirrorScalar(location, 0, {(double)(v)});
            mValue[location] = vec;
        }
    }
}

void LLGLSLShader::uniform2f(const LLStaticHashedString& uniform, GLfloat x, GLfloat y)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    GLint location = getUniformLocation(uniform);

    if (location >= 0)
    {
        const auto& iter = mValue.find(location);
        LLVector4 vec(x, y, 0.f, 0.f);
        if (iter == mValue.end() || shouldChange(iter->second, vec))
        {
            mirrorScalar(location, 0, {(double)(x), (double)(y)});
            mValue[location] = vec;
        }
    }

}

void LLGLSLShader::uniform3f(const LLStaticHashedString& uniform, GLfloat x, GLfloat y, GLfloat z)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    GLint location = getUniformLocation(uniform);

    if (location >= 0)
    {
        const auto& iter = mValue.find(location);
        LLVector4 vec(x, y, z, 0.f);
        if (iter == mValue.end() || shouldChange(iter->second, vec))
        {
            mirrorScalar(location, 0, {(double)(x), (double)(y), (double)(z)});
            mValue[location] = vec;
        }
    }
}

void LLGLSLShader::uniform4f(const LLStaticHashedString& uniform, GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    GLint location = getUniformLocation(uniform);

    if (location >= 0)
    {
        const auto& iter = mValue.find(location);
        LLVector4 vec(x, y, z, w);
        if (iter == mValue.end() || shouldChange(iter->second, vec))
        {
            mirrorScalar(location, 0, {(double)(x), (double)(y), (double)(z), (double)(w)});
            mValue[location] = vec;
        }
    }
}

void LLGLSLShader::uniform1fv(const LLStaticHashedString& uniform, U32 count, const GLfloat* v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    GLint location = getUniformLocation(uniform);

    if (location >= 0)
    {
        const auto& iter = mValue.find(location);
        LLVector4 vec(v[0], 0.f, 0.f, 0.f);
        if (iter == mValue.end() || shouldChange(iter->second, vec) || count != 1)
        {
            mirrorVector(location, 0, 1, count, v);
            mValue[location] = vec;
        }
    }
}

void LLGLSLShader::uniform2fv(const LLStaticHashedString& uniform, U32 count, const GLfloat* v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    GLint location = getUniformLocation(uniform);

    if (location >= 0)
    {
        const auto& iter = mValue.find(location);
        LLVector4 vec(v[0], v[1], 0.f, 0.f);
        if (iter == mValue.end() || shouldChange(iter->second, vec) || count != 1)
        {
            mirrorVector(location, 0, 2, count, v);
            mValue[location] = vec;
        }
    }
}

void LLGLSLShader::uniform3fv(const LLStaticHashedString& uniform, U32 count, const GLfloat* v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    GLint location = getUniformLocation(uniform);

    if (location >= 0)
    {
        const auto& iter = mValue.find(location);
        LLVector4 vec(v[0], v[1], v[2], 0.f);
        if (iter == mValue.end() || shouldChange(iter->second, vec) || count != 1)
        {
            mirrorVector(location, 0, 3, count, v);
            mValue[location] = vec;
        }
    }
}

void LLGLSLShader::uniform4fv(const LLStaticHashedString& uniform, U32 count, const GLfloat* v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    GLint location = getUniformLocation(uniform);

    if (location >= 0)
    {
        LLVector4 vec(v);
        const auto& iter = mValue.find(location);
        if (iter == mValue.end() || shouldChange(iter->second, vec) || count != 1)
        {
            LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
            mirrorVector(location, 0, 4, count, v);
            mValue[location] = vec;
        }
    }
}

void LLGLSLShader::uniform4uiv(const LLStaticHashedString& uniform, U32 count, const GLuint* v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    GLint location = getUniformLocation(uniform);

    if (location >= 0)
    {
        LLVector4 vec((F32)v[0], (F32)v[1], (F32)v[2], (F32)v[3]);
        const auto& iter = mValue.find(location);
        if (iter == mValue.end() || shouldChange(iter->second, vec) || count != 1)
        {
            LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
            mirrorVector(location, 1, 4, count, v);
            mValue[location] = vec;
        }
    }
}

void LLGLSLShader::uniformMatrix3fv(const LLStaticHashedString& uniform, U32 count, GLboolean transpose, const GLfloat* v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM_MATRIX);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    GLint location = getUniformLocation(uniform);

    if (location >= 0)
    {
        stop_glerror();
        mirrorMatrix(location, 3, 3, transpose, count, v);
        stop_glerror();
    }
}

void LLGLSLShader::uniformMatrix4fv(const LLStaticHashedString& uniform, U32 count, GLboolean transpose, const GLfloat* v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM_MATRIX);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    GLint location = getUniformLocation(uniform);

    if (location >= 0)
    {
        stop_glerror();
        mirrorMatrix(location, 4, 4, transpose, count, v);
        stop_glerror();
    }
}

void LLGLSLShader::uniformMatrix3x4fv(const LLStaticHashedString& uniform, U32 count, GLboolean transpose, const GLfloat* v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM_MATRIX);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    GLint location = getUniformLocation(uniform);

    if (location >= 0)
    {
        mirrorMatrix(location, 3, 4, transpose, count, v);
    }
}


void LLGLSLShader::vertexAttrib4f(U32 index, GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
    if (mAttribute[index] > 0)
    {
        LLVkBuf::setConstantAttribute(mAttribute[index], x, y, z, w);
    }
}

void LLGLSLShader::vertexAttrib4fv(U32 index, GLfloat* v)
{
    if (mAttribute[index] > 0)
    {
        LLVkBuf::setConstantAttribute(mAttribute[index], v[0], v[1], v[2], v[3]);
    }
}

void LLGLSLShader::setMinimumAlpha(F32 minimum)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    gGL.flush();
    uniform1f(LLShaderMgr::MINIMUM_ALPHA, minimum);
}

void LLShaderUniforms::apply(LLGLSLShader* shader)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    for (auto& uniform : mIntegers)
    {
        shader->uniform1i(uniform.mUniform, uniform.mValue);
    }

    for (auto& uniform : mFloats)
    {
        shader->uniform1f(uniform.mUniform, uniform.mValue);
    }

    for (auto& uniform : mVectors)
    {
        shader->uniform4fv(uniform.mUniform, 1, uniform.mValue.mV);
    }

    for (auto& uniform : mVector3s)
    {
        shader->uniform3fv(uniform.mUniform, 1, uniform.mValue.mV);
    }
}

LLUUID LLGLSLShader::hash()
{
    HBXXH128 hash_obj;
    hash_obj.update(mName);
    hash_obj.update(&mShaderGroup, sizeof(mShaderGroup));
    hash_obj.update(&mShaderLevel, sizeof(mShaderLevel));
    for (const auto& shdr_pair : mShaderFiles)
    {
        hash_obj.update(shdr_pair.first);
        hash_obj.update(&shdr_pair.second, sizeof(GLenum));
    }
    for (const auto& define_pair : mDefines)
    {
        hash_obj.update(define_pair.first);
        hash_obj.update(define_pair.second);

    }
    for (const auto& define_pair : LLGLSLShader::sGlobalDefines)
    {
        hash_obj.update(define_pair.first);
        hash_obj.update(define_pair.second);

    }
    hash_obj.update(&mFeatures, sizeof(LLShaderFeatures));
    hash_obj.update(gGLManager.mGLVendor);
    hash_obj.update(gGLManager.mGLRenderer);
    hash_obj.update(gGLManager.mGLVersionString);
    return hash_obj.digest();
}

#if LL_PROFILER_ENABLE_RENDER_DOC
void LLGLSLShader::setLabel(const char* label) {
    LL_LABEL_OBJECT_GL(GL_PROGRAM, mProgramObject, strlen(label), label);
}
#endif
