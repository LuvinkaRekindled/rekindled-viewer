/**
 * @file llshaderprogram.cpp
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

#include "llshaderprogram.h"
#include <cctype>
#include <set>

#include "llshadermgr.h"
#include "llfile.h"
#include "llrender.h"
#include "llvertexbuffer.h"
#include "llrendertarget.h"
#if defined(LL_VULKAN)
#include "llvkshader.h"
#include "llvknative.h"
#include "llvkrenderer.h"
#include "llvkasync.h"
#include <emmintrin.h>
#include "llvkbuf.h"
#include "llvkquery.h"
#include "llvkbridge.h"
#endif

#include "hbxxh.h"
#include "llsdserialize.h"

#if LL_DARWIN
#include "OpenGL/OpenGL.h"
#endif

 // Print-print list of shader included source files that are linked together via glAttachShader()
 // i.e. On macOS / OSX the AMD GLSL linker will display an error if a varying is left in an undefined state.

// Lots of STL stuff in here, using namespace std to keep things more readable
using std::vector;
using std::pair;
using std::make_pair;
using std::string;

U32 LLShaderProgram::sEnvUniformEpoch = 1;      // (programs start at 0: the first bind of any program takes the environment's uniforms)
U32 LLShaderProgram::sCurBoundShader = 0;
LLShaderProgram* LLShaderProgram::sCurBoundShaderPtr = NULL;
S32 LLShaderProgram::sIndexedTextureChannels = 0;
U32 LLShaderProgram::sMaxGLTFMaterials = 0;
U32 LLShaderProgram::sMaxGLTFNodes = 0;
bool LLShaderProgram::sProfileEnabled = false;
bool LLShaderProgram::sCanProfile = true;
std::set<LLShaderProgram*> LLShaderProgram::sInstances;
LLShaderProgram::defines_map_t LLShaderProgram::sGlobalDefines;
U64 LLShaderProgram::sTotalTimeElapsed = 0;
U32 LLShaderProgram::sTotalTrianglesDrawn = 0;
U64 LLShaderProgram::sTotalSamplesDrawn = 0;
U32 LLShaderProgram::sTotalBinds = 0;
boost::json::value LLShaderProgram::sDefaultStats;

//UI shader -- declared here so llui_libtest will link properly
LLShaderProgram    gUIProgram;
LLShaderProgram    gSolidColorProgram;

// NOTE: Keep gShaderConsts* and LLShaderProgram::ShaderConsts_e in sync!
const std::string gShaderConstsKey[LLShaderProgram::NUM_SHADER_CONSTS] =
{
      "LL_SHADER_CONST_CLOUD_MOON_DEPTH"
    , "LL_SHADER_CONST_STAR_DEPTH"
};

// NOTE: Keep gShaderConsts* and LLShaderProgram::ShaderConsts_e in sync!
const std::string gShaderConstsVal[LLShaderProgram::NUM_SHADER_CONSTS] =
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
void LLShaderProgram::initProfile()
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


struct LLShaderProgramCompareTimeElapsed
{
    bool operator()(const LLShaderProgram* const& lhs, const LLShaderProgram* const& rhs)
    {
        return lhs->mTimeElapsed < rhs->mTimeElapsed;
    }
};

//static
void LLShaderProgram::finishProfile(boost::json::value& statsv)
{
    sProfileEnabled = false;

    if (! statsv.is_null())
    {
        std::vector<LLShaderProgram*> sorted(sInstances.begin(), sInstances.end());
        std::sort(sorted.begin(), sorted.end(), LLShaderProgramCompareTimeElapsed());

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

void LLShaderProgram::clearStats()
{
    mTrianglesDrawn = 0;
    mTimeElapsed = 0;
    mSamplesDrawn = 0;
    mBinds = 0;
}

void LLShaderProgram::dumpStats(boost::json::object& stats)
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
void LLShaderProgram::startProfile()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    if (sProfileEnabled && sCurBoundShaderPtr)
    {
        sCurBoundShaderPtr->placeProfileQuery();
    }

}

//static
void LLShaderProgram::stopProfile()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    if (sProfileEnabled && sCurBoundShaderPtr)
    {
        sCurBoundShaderPtr->unbind();
    }
}

void LLShaderProgram::placeProfileQuery(bool for_runtime)
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

bool LLShaderProgram::readProfileQuery(bool for_runtime, bool force_read)
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

        U64 time_elapsed = LLVkQuery::result(mTimerQuery);
        mTimeElapsed += time_elapsed;
        mProfilePending = false;

        if (!for_runtime)
        {
            U64 samples_passed = LLVkQuery::result(mSamplesQuery);

            U64 primitives_generated = LLVkQuery::result(mPrimitivesQuery);
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



LLShaderProgram::LLShaderProgram()
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

LLShaderProgram::~LLShaderProgram()
{
}

void LLShaderProgram::unload()
{
    mShaderFiles.clear();
    mDefines.clear();
    mFeatures = LLShaderFeatures();

    unloadInternal();
}

void LLShaderProgram::unloadInternal()
{
    sInstances.erase(this);
    mirrorDestroy();

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

}

#if defined(LL_VULKAN)
void LLShaderProgram::mirrorDestroy()
{
    if (!mVkProgram && !mVkProgramPending && !LLVkBridge::isReady()) return;   // cheap exit when the mirror is off
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
    std::lock_guard<LLVkBridge::Mutex> mirror_lock(LLVkBridge::lock());
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
S32 LLShaderProgram::vkTwinOf(S32 location) const
{
    if (location < 0)
    {
        return -1;
    }
    return location >= SAMPLER_LOCATION_BASE ? ~(S32)(location - SAMPLER_LOCATION_BASE) : (S32)location;
}

void LLShaderProgram::mirrorScalar(S32 location, int kind, std::initializer_list<double> values)
{
    LLVkAsync::frontCount(LLVkAsync::FP_UNIFORM_SENT);
    if (LLUniCensus::sOn) { censusProgram(); LLUniCensus::note_call(); ++mCensus.mCalls; }
    if (!mVkProgram) return;
    if (LLVkAsync::mainOnlyProducer())
    {
        // range writes: the copy of the class block does the comparing (samplers and unknown locations take the long way below)
        const S32 twin = vkTwinOf(location);
        if (twin >= 0)
        {
            F32 f[4] = {};
            S32 iv[4] = {};
            U32 n = 0;
            for (double v : values)
            {
                if (n < 4)
                {
                    f[n] = (F32)v;
                    iv[n] = (S32)v;
                    ++n;
                }
            }
            vkWriteFloats(twin, kind == 0 ? f : reinterpret_cast<const F32*>(iv), n, 1);
            return;
        }
    }
    const bool async = LLVkAsync::gEnabled;
    LLVkBridge::PhaseTimer phase_timer(LLVkBridge::PH_UNIFORM);
    std::unique_lock<LLVkBridge::Mutex> mirror_lock(LLVkBridge::lock(), std::defer_lock);
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
            // (the value cache does not filter these any more: only a changed unit is a record)
            const size_t si = (size_t)(~twin);
            const S32 unit = (S32)*values.begin();
            if (mVkSamplerUnits.size() <= si)
            {
                mVkSamplerUnits.resize(si + 1, -1);
            }
            if (mVkSamplerUnits[si] == unit)
            {
                return;
            }
            mVkSamplerUnits[si] = unit;
        }
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
    if ((!async || LLVkAsync::mainOnlyProducer()))
    {
        vkWriteFloats(twin, kind == 0 ? f : reinterpret_cast<const F32*>(i), n, 1);
        return;
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

void LLShaderProgram::mirrorVector(S32 location, int kind, U32 components, U32 count, const void* data)
{
    LLVkAsync::frontCount(LLVkAsync::FP_UNIFORM_SENT);
    if (LLUniCensus::sOn) { censusProgram(); LLUniCensus::note_call(); ++mCensus.mCalls; }
    if (!mVkProgram || !data) return;
    if (LLVkAsync::mainOnlyProducer())
    {
        const S32 twin = vkTwinOf(location);
        if (twin >= 0)
        {
            vkWriteFloats(twin, reinterpret_cast<const F32*>(data), components, count);
            return;
        }
    }
    const bool async = LLVkAsync::gEnabled;
    LLVkBridge::PhaseTimer phase_timer(LLVkBridge::PH_UNIFORM);
    std::unique_lock<LLVkBridge::Mutex> mirror_lock(LLVkBridge::lock(), std::defer_lock);
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
    if ((!async || LLVkAsync::mainOnlyProducer()))
    {
        vkWriteFloats(twin, reinterpret_cast<const F32*>(data), components, count);
        return;
    }
    if (async)
    {
        LLVkAsync::uniformVector(mVkProgram, (unsigned)twin, components, count, data);
        return;
    }
    mVkProgram->setFloatsAt((size_t)twin, reinterpret_cast<const F32*>(data), components, count);
}

void LLShaderProgram::mirrorMatrix(S32 location, U32 columns, U32 rows, bool transpose, U32 count, const F32* data)
{
    LLVkAsync::frontCount(LLVkAsync::FP_UNIFORM_SENT);
    if (LLUniCensus::sOn) { censusProgram(); LLUniCensus::note_call(); ++mCensus.mCalls; }
    if (!mVkProgram || !data) return;
    if (LLVkAsync::mainOnlyProducer())
    {
        const S32 twin = vkTwinOf(location);
        if (twin >= 0)
        {
            vkWriteMatrix(twin, data, columns, rows, transpose != false, count);
            return;
        }
    }
    const bool async = LLVkAsync::gEnabled;
    LLVkBridge::PhaseTimer phase_timer(LLVkBridge::PH_UNIFORM);
    std::unique_lock<LLVkBridge::Mutex> mirror_lock(LLVkBridge::lock(), std::defer_lock);
    if (!async)
    {
        mirror_lock.lock();
    }
    const S32 twin = vkTwinOf(location);
    if (twin < 0)
    {
        return;
    }
    if ((!async || LLVkAsync::mainOnlyProducer()))
    {
        vkWriteMatrix(twin, data, columns, rows, transpose != false, count);
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

// copies 'floats' values (1..4) over the destination when they differ; true when it changed (inline: a variable size memcmp/memcpy call per uniform was most of the cost)
static inline bool vkDiffCopy(U8* dst, const F32* src, U32 floats)
{
    switch (floats)
    {
    case 4:
    {
        const __m128i a = _mm_loadu_si128((const __m128i*)dst);
        const __m128i b = _mm_loadu_si128((const __m128i*)src);
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(a, b)) == 0xFFFF)
        {
            return false;
        }
        _mm_storeu_si128((__m128i*)dst, b);
        return true;
    }
    case 3:
    {
        U64 d0, s0;
        U32 d1, s1;
        memcpy(&d0, dst, 8); memcpy(&s0, src, 8);
        memcpy(&d1, dst + 8, 4); memcpy(&s1, (const U8*)src + 8, 4);
        if (d0 == s0 && d1 == s1)
        {
            return false;
        }
        memcpy(dst, &s0, 8); memcpy(dst + 8, &s1, 4);
        return true;
    }
    case 2:
    {
        U64 d0, s0;
        memcpy(&d0, dst, 8); memcpy(&s0, src, 8);
        if (d0 == s0)
        {
            return false;
        }
        memcpy(dst, &s0, 8);
        return true;
    }
    default:
    {
        U32 d0, s0;
        memcpy(&d0, dst, 4); memcpy(&s0, src, 4);
        if (d0 == s0)
        {
            return false;
        }
        memcpy(dst, &s0, 4);
        return true;
    }
    }
}

bool LLShaderProgram::uniformValueChanged(S32 location, const LLVector4& value, bool force)
{
    // (the class block copy compares the values: nothing is filtered here any more)
    return true;
}

// a range of this shader's copy of a class block changed: it goes to the worker (or, before the queue runs, straight into the program)
void LLShaderProgram::vkSendRange(U32 cls, U32 offset, U32 size)
{
    if (LLUniCensus::sOn) { LLUniCensus::note_changed(size); ++mCensus.mChanged; }
    const U8* bytes = &mVkShadow[cls][offset];
    if (LLVkAsync::gEnabled)
    {
        LLVkAsync::uniformRange(mVkProgram, cls, offset, size, bytes);
    }
    else
    {
        mVkProgram->writeRange(cls, offset, size, bytes);
    }
}

void LLShaderProgram::vkAnnounce()
{
    mVkEpoch = LLVkAsync::passEpoch();
    if (!mVkProgram || !LLVkAsync::gEnabled || !LLVkAsync::mainOnlyProducer())
    {
        return;
    }
    for (U32 cls = 0; cls < LLVk::UC_COUNT; ++cls)
    {
        if (!mVkShadow[cls].empty())
        {
            LLVkAsync::uniformRangeAnnounce(mVkProgram, cls, 0, (U32)mVkShadow[cls].size(), mVkShadow[cls].data());
        }
    }
}

void LLShaderProgram::announceBound()
{
    if (sCurBoundShaderPtr && sCurBoundShaderPtr->mVkEpoch != LLVkAsync::passEpoch())
    {
        sCurBoundShaderPtr->vkAnnounce();
    }
}

bool LLShaderProgram::vkWriteFloats(S32 twin, const F32* data, U32 components, U32 count)
{
    if ((size_t)twin >= mVkUni.size())
    {
        return false;
    }
    const VkUni& u = mVkUni[twin];
    std::vector<U8>& sh = mVkShadow[u.mClass];
    if (LLUniCensus::sOn) censusUniform(twin, false);
    if (u.mArraySize == 0 && count <= 1)
    {
        // the common case: one scalar or vector
        if (u.mOffset + components * 4 > sh.size() || components < 1 || components > 4 || !vkDiffCopy(&sh[u.mOffset], data, components))
        {
            return false;
        }
        if (LLUniCensus::sOn) censusUniform(twin, true);
        vkSendRange(u.mClass, u.mOffset, components * 4);
        return true;
    }
    const U32 max_count = std::max<U32>(1, u.mArraySize);
    count = std::min(count, max_count);
    const U32 stride = u.mArraySize > 0 ? 16 : 4 * components;
    U32 lo = ~0u, hi = 0;
    for (U32 i = 0; i < count; ++i)
    {
        const U32 off = u.mOffset + i * stride;
        if (off + components * 4 > sh.size())
        {
            break;
        }
        if (memcmp(&sh[off], data + i * components, components * 4) != 0)
        {
            memcpy(&sh[off], data + i * components, components * 4);
            lo = std::min(lo, off);
            hi = std::max(hi, off + components * 4);
        }
    }
    if (lo == ~0u)
    {
        return false;
    }
    if (LLUniCensus::sOn) censusUniform(twin, true);
    vkSendRange(u.mClass, lo, hi - lo);
    return true;
}

bool LLShaderProgram::vkWriteMatrix(S32 twin, const F32* data, U32 columns, U32 rows, bool transpose, U32 count)
{
    if ((size_t)twin >= mVkUni.size())
    {
        return false;
    }
    const VkUni& u = mVkUni[twin];
    std::vector<U8>& sh = mVkShadow[u.mClass];
    if (LLUniCensus::sOn) censusUniform(twin, false);
    const U32 max_count = std::max<U32>(1, u.mArraySize);
    count = std::min(count, max_count);
    const U32 stride = columns * 16;        // std140: every column occupies 16 bytes
    U32 lo = ~0u, hi = 0;
    for (U32 m = 0; m < count; ++m)
    {
        for (U32 c = 0; c < columns; ++c)
        {
            const U32 off = u.mOffset + m * stride + c * 16;
            if (off + rows * 4 > sh.size())
            {
                continue;
            }
            const F32* col = data + ((size_t)m * columns + c) * rows;       // column-major data: the column is in place
            F32 gathered[4];
            if (transpose)
            {
                for (U32 r = 0; r < rows; ++r)
                {
                    gathered[r] = data[(size_t)m * columns * rows + r * columns + c];       // (the viewer hands transposed matrices over row-major)
                }
                col = gathered;
            }
            if (vkDiffCopy(&sh[off], col, rows))
            {
                lo = std::min(lo, off);
                hi = std::max(hi, off + rows * 4);
            }
        }
    }
    if (lo == ~0u)
    {
        return false;
    }
    if (LLUniCensus::sOn) censusUniform(twin, true);
    vkSendRange(u.mClass, lo, hi - lo);
    return true;
}

// the program has been built (link()) and the viewer-side mapping is done: it becomes the one draws use, and its samplers get their texture units
void LLShaderProgram::mirrorCreate()
{
    if (!mVkProgramPending)
    {
        return;
    }
    mVkProgram = mVkProgramPending;
    mVkProgramPending = nullptr;
    LLShaderMgr* mgr = LLShaderMgr::instance();
    const LLVk::ProgramInfo& pi = mVkProgram->getInfo();

    // F2.5: this shader's copy of the class blocks starts as the program built them (initial values from the shader text)
    static_assert(LLVk::UC_COUNT == 5, "LLShaderProgram::mVkShadow");
    mVkUni.clear();
    mVkUni.reserve(pi.mUniforms.size());
    for (const LLVk::LooseUniform& lu : pi.mUniforms)
    {
        VkUni vu;
        vu.mClass = lu.mClass;
        vu.mOffset = lu.mOffset;
        vu.mArraySize = lu.mArraySize;
        mVkUni.push_back(vu);
    }
    for (U32 k = 0; k < LLVk::UC_COUNT; ++k)
    {
        mVkShadow[k] = mVkProgram->shadowOf(k);
    }
    mVkSamplerUnits.clear();

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
void LLShaderProgram::mirrorDestroy() {}
void LLShaderProgram::mirrorCreate() {}
void LLShaderProgram::mirrorScalar(S32, int, std::initializer_list<double>) {}
void LLShaderProgram::mirrorVector(S32, int, U32, U32, const void*) {}
void LLShaderProgram::mirrorMatrix(S32, U32, U32, bool, U32, const F32*) {}
#endif

bool LLShaderProgram::createShader()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    unloadInternal();

    sInstances.insert(this);
    mCensusBinds = 0;

#if defined(LL_VULKAN)
    if (mNativeFragment.empty() || mNativeFromManifest)
    {
        // <FS> stage 4 step 1: a program of a converted family is built from its manifest line (no stock files behind it)
        std::string native_vertex, native_fragment, native_geometry;
        std::vector<std::pair<std::string, std::string>> native_defines;
        if (LLVk::nativeOnlyLine(mName, native_vertex, native_fragment, native_defines, native_geometry))
        {
            mNativeVertex = native_vertex;
            mNativeFragment = native_fragment;
            mNativeGeometry = native_geometry;
            mNativeDefines = native_defines;
            mNativeFromManifest = true;
        }
    }
#endif

    //reloading, reset matrix hash values
    for (U32 i = 0; i < LLRender::NUM_MATRIX_MODES; ++i)
    {
        mMatHash[i] = 0xFFFFFFFF;
    }
    mLightHash = 0xFFFFFFFF;

    if (mShaderFiles.empty() && mNativeFragment.empty())
    {
        LL_WARNS("ShaderLoading") << "program " << mName << " has no stock files and no native source (a converted family needs its line in native/manifest.txt)" << LL_ENDL;
        return false;
    }

#if LL_DARWIN
    if(!gDeviceCaps.mIsApple)
    {
        // work-around missing mix(vec3,vec3,bvec3)
        mDefines["OLD_SELECT"] = "1";
    }
#endif

    mShaderHash = hash();

    // a program is a handle of our own: the shader objects attached to it are compiled together when it is linked
    static U32 sNextProgramObject = 0;
    mProgramObject = ++sNextProgramObject;

    bool success = true;

    mUsingBinaryProgram = false;

    if (!mUsingBinaryProgram && mNativeFragment.empty())
    {

        //compile new source
        vector< pair<string, ShaderStage> >::iterator fileIter = mShaderFiles.begin();
        for (; fileIter != mShaderFiles.end(); fileIter++)
        {
            U32 shaderhandle = LLShaderMgr::instance()->loadShaderFile((*fileIter).first, mShaderLevel, (*fileIter).second, &mDefines, mFeatures.mIndexedTextureChannels);
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
    if (mNativeFragment.empty() && !LLShaderMgr::instance()->attachShaderFeatures(this))
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
        llassert(mFeatures.mIndexedTextureChannels == LLShaderProgram::sIndexedTextureChannels); // these numbers must always match
        mEnvUniformEpoch = sEnvUniformEpoch;       // <FS> (as in mapUniforms: a program being made takes no environment uniforms yet)
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

    mCensusBinds = 0;       // <FS> the binds a program makes while it is made (uniform mapping, texture channels) are not use

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
                map_file = fopen((std::string("<work>/run-logs/program-map-") + map_tag + ".txt").c_str(), "w");
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
                    const S32 loc = mAttribute[i].loc;
#else
                    const S32 loc = mAttribute[i];
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


bool LLShaderProgram::attachVertexObject(std::string object_path)
{
    // <FS> S6.6: a pull program (LL_VERTEX_PULL) takes the library vertex shaders that declare attributes (the avatar skin function reads `weight`) in their rewritten form: loaded once under their own key
    if (mDefines.find("LL_VERTEX_PULL") != mDefines.end() && (object_path == "avatar/avatarSkinV.glsl" || object_path == "objects/indexedTextureV.glsl"))
    {
        const std::string key = object_path + "#pull";
        LLShaderMgr* mgr = LLShaderMgr::instance();
        if (mgr->mVertexShaderObjects.count(key) == 0)
        {
            std::map<std::string, std::string> defines;
            defines["LL_VERTEX_PULL"] = "1";
            S32 level = 1;
            const auto original = mgr->mVertexShaderObjects.find(object_path);
            const bool had_original = original != mgr->mVertexShaderObjects.end();
            const U32 original_handle = had_original ? original->second : 0;
            const U32 object = mgr->loadShaderFile(object_path, level, ShaderStage::Vertex, &defines, 0);      // (it registers the object under the file name: put the plain one back)
            if (had_original)
            {
                mgr->mVertexShaderObjects[object_path] = original_handle;
            }
            else
            {
                mgr->mVertexShaderObjects.erase(object_path);
            }
            if (object)
            {
                mgr->mVertexShaderObjects[key] = object;
            }
        }
        if (mgr->mVertexShaderObjects.count(key) > 0)
        {
            mAttachedObjects.push_back(mgr->mVertexShaderObjects[key]);
            return true;
        }
    }
    if (LLShaderMgr::instance()->mVertexShaderObjects.count(object_path) > 0)
    {
        mAttachedObjects.push_back(LLShaderMgr::instance()->mVertexShaderObjects[object_path]);
        return true;
    }
    else
    {
        LL_SHADER_LOADING_WARNS() << "Attempting to attach shader object: '" << object_path << "' that hasn't been compiled." << LL_ENDL;
        return false;
    }
}

bool LLShaderProgram::attachFragmentObject(std::string object_path)
{
    if(mUsingBinaryProgram)
        return true;

    if (LLShaderMgr::instance()->mFragmentShaderObjects.count(object_path) > 0)
    {
        mAttachedObjects.push_back(LLShaderMgr::instance()->mFragmentShaderObjects[object_path]);
        return true;
    }
    else
    {
        LL_SHADER_LOADING_WARNS() << "Attempting to attach shader object: '" << object_path << "' that hasn't been compiled." << LL_ENDL;
        return false;
    }
}

void LLShaderProgram::attachObject(U32 object)
{
    if(mUsingBinaryProgram)
        return;

    if (object != 0)
    {
        mAttachedObjects.push_back(object);
    }
    else
    {
        LL_SHADER_LOADING_WARNS() << "Attempting to attach non existing shader object. " << LL_ENDL;
    }
}

void LLShaderProgram::attachObjects(U32* objects, S32 count)
{
    if(mUsingBinaryProgram)
        return;

    for (S32 i = 0; i < count; i++)
    {
        attachObject(objects[i]);
    }
}

bool LLShaderProgram::mapAttributes()
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

// what the uniform mapping needs of a GLSL type name: how many floats / ints one element takes (a multiple of the array size), and whether it is a texture the shader samples
struct GlslType
{
    S32 mScale = 1;
    bool mSampler = false;
};

static GlslType glsl_type(const std::string& t)
{
    static const std::unordered_map<std::string, GlslType> map = {
        { "vec2", { 2, false } }, { "vec3", { 3, false } }, { "vec4", { 4, false } },
        { "ivec2", { 2, false } }, { "ivec3", { 3, false } }, { "ivec4", { 4, false } },
        { "bvec2", { 2, false } }, { "bvec3", { 3, false } }, { "bvec4", { 4, false } },
        { "uvec2", { 2, false } }, { "uvec3", { 3, false } }, { "uvec4", { 4, false } },
        { "mat2", { 4, false } }, { "mat3", { 9, false } }, { "mat4", { 16, false } },
        { "mat2x3", { 6, false } }, { "mat2x4", { 8, false } }, { "mat3x2", { 6, false } }, { "mat3x4", { 12, false } }, { "mat4x2", { 8, false } }, { "mat4x3", { 12, false } },
        // the samplers that take a texture channel (the 1D .. rectangle shadow range, the multisample and cube array samplers of the GL numbering; array / cube shadow samplers did not)
        { "sampler1D", { 1, true } }, { "sampler2D", { 1, true } }, { "sampler3D", { 1, true } }, { "samplerCube", { 1, true } },
        { "sampler1DShadow", { 1, true } }, { "sampler2DShadow", { 1, true } }, { "sampler2DRect", { 1, true } }, { "sampler2DRectShadow", { 1, true } },
        { "samplerCubeArray", { 1, true } }, { "sampler2DMS", { 1, true } },
    };
    auto it = map.find(t);
    return it == map.end() ? GlslType() : it->second;
}

void LLShaderProgram::mapUniform(S32 index)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    if (index == -1)
    {
        return;
    }

    if (index < 0 || index >= (S32)mActiveUniforms.size())
    {
        return;
    }
    const ActiveUniform& active = mActiveUniforms[index];
    const GlslType type = glsl_type(active.mType);
    S32 size = active.mSize;
    char name[1024];        /* Flawfinder: ignore */
    strncpy(name, active.mName.c_str(), sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;

    if (size > 0)
    {
        size *= type.mScale;
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
                mTexture[i] = mapUniformTextureChannel(location, type.mSampler, size);
                if (mTexture[i] != -1)
                {
                    LL_DEBUGS("GLSLTextureChannels") << name << " assigned to texture channel " << mTexture[i] << LL_ENDL;
                }
                return;
            }
        }
    }
}

void LLShaderProgram::clearPermutations()
{
    mDefines.clear();
    mVertexPull = false;
}

void LLShaderProgram::addPermutation(std::string name, std::string value)
{
    mDefines[name] = value;
    if (name == "LL_VERTEX_PULL")
    {
        mVertexPull = true;
    }
}

void LLShaderProgram::addConstant(const LLShaderProgram::eShaderConsts shader_const)
{
    addPermutation(gShaderConstsKey[shader_const], gShaderConstsVal[shader_const]);
}

void LLShaderProgram::removePermutation(std::string name)
{
    mDefines.erase(name);
}

S32 LLShaderProgram::mapUniformTextureChannel(S32 location, bool is_sampler, S32 size)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    if (is_sampler)
    {   //this here is a texture
        S32 ret = mActiveTextureChannels;
        if (size == 1)
        {
            mirrorScalar(location, 1, {(double)(mActiveTextureChannels)});
            mActiveTextureChannels++;
        }
        else
        {
            //is array of textures, make sequential after this texture
            S32 channel[16]; // <=== only support up to 16 texture channels
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

bool LLShaderProgram::mapUniforms()
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

    // <FS> a program being made binds here before its uniform tables exist: it must not take the environment's uniforms now (it takes them at its first bind after the next environment update)
    mEnvUniformEpoch = sEnvUniformEpoch;
    bind();

    // what the program reports as active: its loose uniforms (arrays by their first element), then its samplers
    mActiveUniforms.clear();
    {
        const LLVk::ProgramInfo& pinfo = mVkProgramPending->getInfo();
        for (size_t u = 0; u < pinfo.mUniforms.size(); ++u)
        {
            const LLVk::LooseUniform& lu = pinfo.mUniforms[u];
            mActiveUniforms.push_back({ lu.mArraySize ? lu.mName + "[0]" : lu.mName, lu.mType, lu.mArraySize ? (S32)lu.mArraySize : 1, (S32)u });
        }
        for (size_t sm = 0; sm < pinfo.mSamplers.size(); ++sm)
        {
            mActiveUniforms.push_back({ pinfo.mSamplers[sm].mName, pinfo.mSamplers[sm].mType, 1, SAMPLER_LOCATION_BASE + (S32)sm });
        }
    }
    const S32 activeCount = (S32)mActiveUniforms.size();
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


bool LLShaderProgram::link(bool suppress_errors)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    // the attached shader objects are compiled together into one program (the build runs on the render thread, which owns Vulkan)
    LLShaderMgr* mgr = LLShaderMgr::instance();
    std::vector<LLVk::StageSource> stages;
    for (U32 object : mAttachedObjects)
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
        case ShaderStage::Vertex: s.mStage = LLVk::StageSource::Vertex; break;
        case ShaderStage::Fragment: s.mStage = LLVk::StageSource::Fragment; break;
        case ShaderStage::Geometry: s.mStage = LLVk::StageSource::Geometry; break;
        default: continue;
        }
        stages.push_back(std::move(s));
    }
    LLVk::ShaderEnv env;
    env.mAttributeNames = mgr->mReservedAttribs;
    env.mUniformBlockNames = { "ReflectionProbes", "GLTFJoints", "GLTFNodes", "GLTFMaterials" };
    const char* dump = getenv("LL_VK_SHADER_DUMP_DIR");

    LLVk::Program* program = nullptr;
    std::string error;
    auto build = [&]()
    {
        std::lock_guard<LLVkBridge::Mutex> mirror_lock(LLVkBridge::lock());
        if (!LLVkBridge::ensureReady())
        {
            error = "the Vulkan side is not ready";
            return;
        }
        LLVk::ProgramInfo info;
        if (!mNativeFragment.empty())
        {
            // <FS> every native program is built with the viewer's global defines first and its own after them, as the swapped in programs are (llvknative.cpp build). The programs named in C++ (Native Sun
            // Lightmap Half, Native Light Upsample) did not get them until step 4 of the stock carrier removal made `sun_shadow()` depend on SUN_SHADOW: without it the half resolution sun lightmap had no shadow
            std::vector<std::pair<std::string, std::string>> native_defines = LLVk::nativeGlobalDefinesCopy();
            native_defines.insert(native_defines.end(), mNativeDefines.begin(), mNativeDefines.end());
            if (!LLVk::compileNativeProgram(mName, LLVk::nativeRoot(), mNativeVertex, mNativeFragment, native_defines, info, nullptr, &error, mNativeGeometry))
            {
                return;
            }
        }
        else if (!LLVk::compileProgram(mName, stages, env, info, dump ? dump : ""))
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
        if (!mNativeFragment.empty())
        {
            made->setNativeIdx(-1);         // <FS> a native only program is not replaced at the draw (nativeOverride)
        }
        program = made.release();
    };
    if (stages.empty() && mNativeFragment.empty())
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

void LLShaderProgram::censusProgram()
{
    if (!mCensus.mReg)
    {
        mCensus.mReg = true;
        LLUniCensus::sProgs.push_back({ &mName, &mCensus, this });
    }
}

void LLShaderProgram::censusUniform(S32 twin, bool changed)
{
    if ((size_t)twin >= mCensus.mUniCalls.size())
    {
        mCensus.mUniCalls.resize(std::max<size_t>(mVkUni.size(), (size_t)twin + 1), 0);
        mCensus.mUniChanged.resize(mCensus.mUniCalls.size(), 0);
    }
    ++(changed ? mCensus.mUniChanged : mCensus.mUniCalls)[twin];
}

const std::string* LLShaderProgram::censusUniformName(size_t index) const
{
    if (!mVkProgram || index >= mVkProgram->getInfo().mUniforms.size())
    {
        return nullptr;
    }
    return &mVkProgram->getInfo().mUniforms[index].mName;
}

int LLShaderProgram::censusUniformClass(size_t index) const
{
    if (!mVkProgram || index >= mVkProgram->getInfo().mUniforms.size())
    {
        return -1;
    }
    return (int)mVkProgram->getInfo().mUniforms[index].mClass;
}

bool LLShaderProgram::vkMainOnlyProducer()
{
    return LLVkAsync::mainOnlyProducer();
}

std::string LLShaderProgram::vkUniformAt(U32 cls, U32 offset) const
{
    S64 best = -1;
    for (size_t i = 0; i < mVkUni.size(); ++i)
    {
        const VkUni& u = mVkUni[i];
        if (u.mClass == cls && u.mOffset <= offset && (best < 0 || u.mOffset >= mVkUni[(size_t)best].mOffset))
        {
            best = (S64)i;
        }
    }
    if (best < 0)
    {
        return "?";
    }
    const std::string* n = censusUniformName((size_t)best);
    return n ? *n : std::string("?");
}

// <FS> shader cleanup (stage 4): what the viewer loaded and what it bound in this run (LL_VK_PROGRAM_CENSUS=1). Called before the programs are unloaded (a shader reload or the end of the run).
// The program's own name, the files it lists (the stock GLSL paths below shaders/class<N>/, libraries included), the native files that stand in for it, its shader level and how often it was bound.
void LLShaderProgram::dumpProgramCensus()
{
    static const bool on = getenv("LL_VK_PROGRAM_CENSUS") != nullptr;
    static const bool iface = getenv("LL_VK_INTERFACE_DIFF") != nullptr;
#if defined(LL_VULKAN)
    if (iface)
    {
        // <FS> stage 4 step 0: what each loaded program declares, and (with LL_VK_UNICENSUS=1) which of its uniforms the callers wrote; doc/vulkan/interface-diff.py compares it with the native side
        U32 shown = 0;
        for (const LLShaderProgram* p : sInstances)
        {
            if (!p->mVkProgram)
            {
                continue;
            }
            std::string written;
            for (size_t i = 0; i < p->mCensus.mUniCalls.size(); ++i)
            {
                if (p->mCensus.mUniCalls[i] + (i < p->mCensus.mUniChanged.size() ? p->mCensus.mUniChanged[i] : 0u) > 0)
                {
                    const std::string* n = p->censusUniformName(i);
                    written += (written.empty() ? "" : ";") + (n ? *n : std::string("?"));
                }
            }
            LL_INFOS("InterfaceDiff") << "stock | " << (p->mName.empty() ? std::string("(unnamed)") : p->mName) << " | " << LLVk::interfaceText(p->mVkProgram->getInfo())
                << " | set " << written << LL_ENDL;
            ++shown;
        }
        LL_INFOS("InterfaceDiff") << "stock programs " << shown << LL_ENDL;
        LLVk::dumpNativeInterfaces();
    }
#endif
    if (!on)
    {
        return;
    }
    U32 count = 0;
    for (const LLShaderProgram* p : sInstances)
    {
        std::string files;
        for (const auto& f : p->mShaderFiles)
        {
            files += files.empty() ? "" : ";";
            files += f.first;
        }
        LL_INFOS("ProgramCensus") << "program | " << (p->mName.empty() ? std::string("(unnamed)") : p->mName) << " | loaded " << (p->mProgramObject != 0 ? 1 : 0) << " | binds " << p->mCensusBinds
            << " | level " << p->mShaderLevel << " | files " << files << " | native " << p->mNativeVertex << ";" << p->mNativeFragment << (p->mNativeGeometry.empty() ? "" : ";" + p->mNativeGeometry) << LL_ENDL;
        ++count;
    }
    LL_INFOS("ProgramCensus") << "programs loaded " << count << LL_ENDL;
#if defined(LL_VULKAN)
    LLVk::dumpNativeCensus();
#endif
}

void LLShaderProgram::bind()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_SHADER_BIND);
    ++mCensusBinds;
    if (LLUniCensus::sOn) { censusProgram(); LLUniCensus::note_bind(); ++mCensus.mBinds; }

    llassert_always(mProgramObject != 0);

    gRender.flush();

    if (sCurBoundShader != mProgramObject)  // Don't re-bind current shader
    {
        if (sCurBoundShaderPtr)
        {
            sCurBoundShaderPtr->readProfileQuery();
        }
        // <FS> the program of the next draws; no buffers are bound after a program bind
        LLVertexBuffer::forgetBoundBuffers();
        sCurBoundShader = mProgramObject;
        sCurBoundShaderPtr = this;
        LLVkAsync::shaderBind(mVkProgram);
        placeProfileQuery();
    }

    if (mUniformsDirty || mEnvUniformEpoch != sEnvUniformEpoch)
    {
        const long long t_upd = LLUniCensus::sOn ? LLUniCensus::now_ns() : 0;
        const U64 calls_upd = LLUniCensus::sTotal.mCalls;
        LLShaderMgr::instance()->updateShaderUniforms(this);
        mUniformsDirty = false;
        mEnvUniformEpoch = sEnvUniformEpoch;
        if (LLUniCensus::sOn)
        {
            ++mCensus.mUpdBinds;
            mCensus.mUpdNs += (U64)(LLUniCensus::now_ns() - t_upd);
            mCensus.mUpdCalls += LLUniCensus::sTotal.mCalls - calls_upd;
        }
    }

    if (mVkEpoch != LLVkAsync::passEpoch())
    {
        vkAnnounce();       // S6.2: the first use of the program in this pass
    }

    llassert_always(sCurBoundShaderPtr != nullptr);
    llassert_always(sCurBoundShader == mProgramObject);
}

void LLShaderProgram::bind(U8 variant)
{
    llassert_always(mGLTFVariants.size() == LLShaderProgram::NUM_GLTF_VARIANTS);
    llassert_always(variant < LLShaderProgram::NUM_GLTF_VARIANTS);
    mGLTFVariants[variant].bind();
}

void LLShaderProgram::bind(bool rigged)
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

void LLShaderProgram::unbind(void)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    gRender.flush();
    LLVertexBuffer::unbind();

    if (sCurBoundShaderPtr)
    {
        sCurBoundShaderPtr->readProfileQuery();
    }

    sCurBoundShader = 0;
    sCurBoundShaderPtr = NULL;
#if defined(LL_VULKAN)
    if (LLVkBridge::isReady())
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

S32 LLShaderProgram::bindTexture(const std::string& uniform, LLTexture* texture, LLTexUnit::eTextureType mode)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    S32 channel = 0;
    channel = getUniformLocation(uniform);

    return bindTexture(channel, texture, mode);
}

S32 LLShaderProgram::bindTexture(S32 uniform, LLTexture* texture, LLTexUnit::eTextureType mode)
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
        gRender.getTexUnit(uniform)->bindFast(texture);
    }

    return uniform;
}

S32 LLShaderProgram::bindTexture(S32 uniform, LLRenderTarget* texture, bool depth, LLTexUnit::eTextureFilterOptions mode, U32 index)
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
            gRender.getTexUnit(uniform)->bind(texture, true);
        }
        else {
            bool has_mips = mode == LLTexUnit::TFO_TRILINEAR || mode == LLTexUnit::TFO_ANISOTROPIC;
            gRender.getTexUnit(uniform)->bindManual(texture->getUsage(), texture->getTexture(index), has_mips);
        }

        gRender.getTexUnit(uniform)->setTextureFilteringOption(mode);
    }

    return uniform;
}

S32 LLShaderProgram::bindTexture(const std::string& uniform, LLRenderTarget* texture, bool depth, LLTexUnit::eTextureFilterOptions mode)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    S32 channel = 0;
    channel = getUniformLocation(uniform);

    return bindTexture(channel, texture, depth, mode);
}

S32 LLShaderProgram::unbindTexture(const std::string& uniform, LLTexUnit::eTextureType mode)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    S32 channel = 0;
    channel = getUniformLocation(uniform);

    return unbindTexture(channel);
}

S32 LLShaderProgram::unbindTexture(S32 uniform, LLTexUnit::eTextureType mode)
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
        gRender.getTexUnit(uniform)->unbindFast(mode);
    }

    return uniform;
}

S32 LLShaderProgram::getTextureChannel(S32 uniform) const
{
    return mTexture[uniform];
}

S32 LLShaderProgram::enableTexture(S32 uniform, LLTexUnit::eTextureType mode)
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
    }
    return index;
}

S32 LLShaderProgram::disableTexture(S32 uniform, LLTexUnit::eTextureType mode)
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

    LLTexUnit* tex_unit = gRender.getTexUnit(index);
    if (!tex_unit)
    {
        // Invalid texture unit
        LL_WARNS_ONCE("Shader") << "Invalid texture unit at index: " << index << LL_ENDL;
        return index;
    }

    LLTexUnit::eTextureType curr_type = tex_unit->getCurrType();
    if (curr_type != LLTexUnit::TT_NONE)
    {
        if (gDebugRender && curr_type != mode)
        {
            if (gDebugSession)
            {
                gFailLog << "Texture channel " << index << " texture type corrupted. Expected: " << mode << ", Found: " << curr_type << std::endl;
                ll_fail("LLShaderProgram::disableTexture failed");
            }
            else
            {
                LL_ERRS() << "Texture channel " << index << " texture type corrupted. Expected: " << mode << ", Found: " << curr_type << LL_ENDL;
            }
        }
        tex_unit->clear();
    }

    return index;
}

void LLShaderProgram::uniform1i(U32 index, S32 x)
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
            if (uniformValueChanged(mUniform[index], LLVector4((F32)x, 0.f, 0.f, 0.f)))
            {
                mirrorScalar(mUniform[index], 1, {(double)(x)});
            }
        }
    }
}

void LLShaderProgram::uniform1fAt(S32 location, F32 v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    if (sCurBoundShaderPtr) sCurBoundShaderPtr->mirrorScalar(location, 0, {(double)(v)});
}

void LLShaderProgram::uniform4fvAt(S32 location, S32 count, const F32* v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    if (sCurBoundShaderPtr) sCurBoundShaderPtr->mirrorVector(location, 0, 4, count, v);
}

void LLShaderProgram::uniform1f(U32 index, F32 x)
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
            if (uniformValueChanged(mUniform[index], LLVector4(x, 0.f, 0.f, 0.f)))
            {
                mirrorScalar(mUniform[index], 0, {(double)(x)});
            }
        }
    }
}

void LLShaderProgram::fastUniform1f(U32 index, F32 x)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    llassert(sCurBoundShaderPtr == this);
    llassert(mProgramObject);
    llassert(mUniform.size() <= index);
    llassert(mUniform[index] >= 0);
    mirrorScalar(mUniform[index], 0, {(double)(x)});
}

void LLShaderProgram::uniform2f(U32 index, F32 x, F32 y)
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
            LLVector4 vec(x, y, 0.f, 0.f);
            if (uniformValueChanged(mUniform[index], vec))
            {
                mirrorScalar(mUniform[index], 0, {(double)(x), (double)(y)});
            }
        }
    }
}

void LLShaderProgram::uniform3f(U32 index, F32 x, F32 y, F32 z)
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
            LLVector4 vec(x, y, z, 0.f);
            if (uniformValueChanged(mUniform[index], vec))
            {
                mirrorScalar(mUniform[index], 0, {(double)(x), (double)(y), (double)(z)});
            }
        }
    }
}

void LLShaderProgram::uniform4f(U32 index, F32 x, F32 y, F32 z, F32 w)
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
            LLVector4 vec(x, y, z, w);
            if (uniformValueChanged(mUniform[index], vec))
            {
                mirrorScalar(mUniform[index], 0, {(double)(x), (double)(y), (double)(z), (double)(w)});
            }
        }
    }
}

void LLShaderProgram::uniform1iv(U32 index, U32 count, const S32* v)
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
            LLVector4 vec((F32)v[0], 0.f, 0.f, 0.f);
            if (uniformValueChanged(mUniform[index], vec, true))
            {
                mirrorVector(mUniform[index], 1, 1, count, v);
            }
        }
    }
}

void LLShaderProgram::uniform4iv(U32 index, U32 count, const S32* v)
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
            LLVector4 vec((F32)v[0], (F32)v[1], (F32)v[2], (F32)v[3]);
            if (uniformValueChanged(mUniform[index], vec, true))
            {
            }
        }
    }
}


void LLShaderProgram::uniform1fv(U32 index, U32 count, const F32* v)
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
            LLVector4 vec(v[0], 0.f, 0.f, 0.f);
            if (uniformValueChanged(mUniform[index], vec, true))
            {
                mirrorVector(mUniform[index], 0, 1, count, v);
            }
        }
    }
}

void LLShaderProgram::uniform2fv(U32 index, U32 count, const F32* v)
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
            LLVector4 vec(v[0], v[1], 0.f, 0.f);
            if (uniformValueChanged(mUniform[index], vec, true))
            {
                mirrorVector(mUniform[index], 0, 2, count, v);
            }
        }
    }
}

void LLShaderProgram::uniform3fv(U32 index, U32 count, const F32* v)
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
            LLVector4 vec(v[0], v[1], v[2], 0.f);
            if (uniformValueChanged(mUniform[index], vec, true))
            {
                mirrorVector(mUniform[index], 0, 3, count, v);
            }
        }
    }
}

void LLShaderProgram::uniform4fv(U32 index, U32 count, const F32* v)
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
            LLVector4 vec(v[0], v[1], v[2], v[3]);
            if (uniformValueChanged(mUniform[index], vec, true))
            {
                LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
                mirrorVector(mUniform[index], 0, 4, count, v);
            }
        }
    }
}

void LLShaderProgram::uniform4uiv(U32 index, U32 count, const U32* v)
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
            LLVector4 vec((F32)v[0], (F32)v[1], (F32)v[2], (F32)v[3]);
            if (uniformValueChanged(mUniform[index], vec, true))
            {
                LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
                mirrorVector(mUniform[index], 1, 4, count, v);
            }
        }
    }
}

void LLShaderProgram::uniformMatrix2fv(U32 index, U32 count, bool transpose, const F32* v)
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

void LLShaderProgram::uniformMatrix3fv(U32 index, U32 count, bool transpose, const F32* v)
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

void LLShaderProgram::uniformMatrix3x4fv(U32 index, U32 count, bool transpose, const F32* v)
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

void LLShaderProgram::uniformMatrix4fv(U32 index, U32 count, bool transpose, const F32* v)
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

S32 LLShaderProgram::getUniformLocation(const LLStaticHashedString& uniform)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    S32 ret = -1;
    if (mProgramObject)
    {
        LLStaticStringTable<S32>::iterator iter = mUniformMap.find(uniform);
        if (iter != mUniformMap.end())
        {
            ret = iter->second;
        }
    }

    return ret;
}

S32 LLShaderProgram::getUniformLocation(U32 index)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;

    S32 ret = -1;
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

S32 LLShaderProgram::getAttribLocation(U32 attrib)
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

void LLShaderProgram::uniform1i(const LLStaticHashedString& uniform, S32 v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    S32 location = getUniformLocation(uniform);

    if (location >= 0)
    {
        LLVector4 vec((F32)v, 0.f, 0.f, 0.f);
        if (uniformValueChanged(location, vec))
        {
            mirrorScalar(location, 1, {(double)(v)});
        }
    }
}

void LLShaderProgram::uniform1iv(const LLStaticHashedString& uniform, U32 count, const S32* v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    S32 location = getUniformLocation(uniform);

    if (location >= 0)
    {
        LLVector4 vec((F32)v[0], 0.f, 0.f, 0.f);
        if (uniformValueChanged(location, vec, true))
        {
            LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
            mirrorVector(location, 1, 1, count, v);
        }
    }
}

void LLShaderProgram::uniform4iv(const LLStaticHashedString& uniform, U32 count, const S32* v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    S32 location = getUniformLocation(uniform);

    if (location >= 0)
    {
        LLVector4 vec((F32)v[0], (F32)v[1], (F32)v[2], (F32)v[3]);
        if (uniformValueChanged(location, vec, true))
        {
            LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
            mirrorVector(location, 1, 4, count, v);
        }
    }
}

void LLShaderProgram::uniform2i(const LLStaticHashedString& uniform, S32 i, S32 j)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    S32 location = getUniformLocation(uniform);

    if (location >= 0)
    {
        LLVector4 vec((F32)i, (F32)j, 0.f, 0.f);
        if (uniformValueChanged(location, vec))
        {
            mirrorScalar(location, 1, {(double)(i), (double)(j)});
        }
    }
}


void LLShaderProgram::uniform1f(const LLStaticHashedString& uniform, F32 v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    S32 location = getUniformLocation(uniform);

    if (location >= 0)
    {
        LLVector4 vec(v, 0.f, 0.f, 0.f);
        if (uniformValueChanged(location, vec))
        {
            mirrorScalar(location, 0, {(double)(v)});
        }
    }
}

void LLShaderProgram::uniform2f(const LLStaticHashedString& uniform, F32 x, F32 y)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    S32 location = getUniformLocation(uniform);

    if (location >= 0)
    {
        LLVector4 vec(x, y, 0.f, 0.f);
        if (uniformValueChanged(location, vec))
        {
            mirrorScalar(location, 0, {(double)(x), (double)(y)});
        }
    }

}

void LLShaderProgram::uniform3f(const LLStaticHashedString& uniform, F32 x, F32 y, F32 z)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    S32 location = getUniformLocation(uniform);

    if (location >= 0)
    {
        LLVector4 vec(x, y, z, 0.f);
        if (uniformValueChanged(location, vec))
        {
            mirrorScalar(location, 0, {(double)(x), (double)(y), (double)(z)});
        }
    }
}

void LLShaderProgram::uniform4f(const LLStaticHashedString& uniform, F32 x, F32 y, F32 z, F32 w)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    S32 location = getUniformLocation(uniform);

    if (location >= 0)
    {
        LLVector4 vec(x, y, z, w);
        if (uniformValueChanged(location, vec))
        {
            mirrorScalar(location, 0, {(double)(x), (double)(y), (double)(z), (double)(w)});
        }
    }
}

void LLShaderProgram::uniform1fv(const LLStaticHashedString& uniform, U32 count, const F32* v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    S32 location = getUniformLocation(uniform);

    if (location >= 0)
    {
        LLVector4 vec(v[0], 0.f, 0.f, 0.f);
        if (uniformValueChanged(location, vec, true))
        {
            mirrorVector(location, 0, 1, count, v);
        }
    }
}

void LLShaderProgram::uniform2fv(const LLStaticHashedString& uniform, U32 count, const F32* v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    S32 location = getUniformLocation(uniform);

    if (location >= 0)
    {
        LLVector4 vec(v[0], v[1], 0.f, 0.f);
        if (uniformValueChanged(location, vec, true))
        {
            mirrorVector(location, 0, 2, count, v);
        }
    }
}

void LLShaderProgram::uniform3fv(const LLStaticHashedString& uniform, U32 count, const F32* v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    S32 location = getUniformLocation(uniform);

    if (location >= 0)
    {
        LLVector4 vec(v[0], v[1], v[2], 0.f);
        if (uniformValueChanged(location, vec, true))
        {
            mirrorVector(location, 0, 3, count, v);
        }
    }
}

void LLShaderProgram::uniform4fv(const LLStaticHashedString& uniform, U32 count, const F32* v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    S32 location = getUniformLocation(uniform);

    if (location >= 0)
    {
        LLVector4 vec(v);
        if (uniformValueChanged(location, vec, true))
        {
            LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
            mirrorVector(location, 0, 4, count, v);
        }
    }
}

void LLShaderProgram::uniform4uiv(const LLStaticHashedString& uniform, U32 count, const U32* v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    S32 location = getUniformLocation(uniform);

    if (location >= 0)
    {
        LLVector4 vec((F32)v[0], (F32)v[1], (F32)v[2], (F32)v[3]);
        if (uniformValueChanged(location, vec, true))
        {
            LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
            mirrorVector(location, 1, 4, count, v);
        }
    }
}

void LLShaderProgram::uniformMatrix3fv(const LLStaticHashedString& uniform, U32 count, bool transpose, const F32* v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM_MATRIX);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    S32 location = getUniformLocation(uniform);

    if (location >= 0)
    {
        mirrorMatrix(location, 3, 3, transpose, count, v);
    }
}

void LLShaderProgram::uniformMatrix4fv(const LLStaticHashedString& uniform, U32 count, bool transpose, const F32* v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM_MATRIX);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    S32 location = getUniformLocation(uniform);

    if (location >= 0)
    {
        mirrorMatrix(location, 4, 4, transpose, count, v);
    }
}

void LLShaderProgram::uniformMatrix3x4fv(const LLStaticHashedString& uniform, U32 count, bool transpose, const F32* v)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_UNIFORM_MATRIX);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    S32 location = getUniformLocation(uniform);

    if (location >= 0)
    {
        mirrorMatrix(location, 3, 4, transpose, count, v);
    }
}


void LLShaderProgram::vertexAttrib4f(U32 index, F32 x, F32 y, F32 z, F32 w)
{
    if (mAttribute[index] > 0)
    {
        LLVkBuf::setConstantAttribute(mAttribute[index], x, y, z, w);
    }
}

void LLShaderProgram::vertexAttrib4fv(U32 index, F32* v)
{
    if (mAttribute[index] > 0)
    {
        LLVkBuf::setConstantAttribute(mAttribute[index], v[0], v[1], v[2], v[3]);
    }
}

void LLShaderProgram::setMinimumAlpha(F32 minimum)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SHADER;
    gRender.flush();
    uniform1f(LLShaderMgr::MINIMUM_ALPHA, minimum);
}

void LLShaderUniforms::apply(LLShaderProgram* shader)
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

LLUUID LLShaderProgram::hash()
{
    HBXXH128 hash_obj;
    hash_obj.update(mName);
    hash_obj.update(&mShaderGroup, sizeof(mShaderGroup));
    hash_obj.update(&mShaderLevel, sizeof(mShaderLevel));
    for (const auto& shdr_pair : mShaderFiles)
    {
        hash_obj.update(shdr_pair.first);
        hash_obj.update(&shdr_pair.second, sizeof(ShaderStage));
    }
    for (const auto& define_pair : mDefines)
    {
        hash_obj.update(define_pair.first);
        hash_obj.update(define_pair.second);

    }
    for (const auto& define_pair : LLShaderProgram::sGlobalDefines)
    {
        hash_obj.update(define_pair.first);
        hash_obj.update(define_pair.second);

    }
    hash_obj.update(&mFeatures, sizeof(LLShaderFeatures));
    hash_obj.update(gDeviceCaps.mVendorString);
    hash_obj.update(gDeviceCaps.mDeviceString);
    hash_obj.update(gDeviceCaps.mApiVersionString);
    return hash_obj.digest();
}

#if LL_PROFILER_ENABLE_RENDER_DOC
void LLShaderProgram::setLabel(const char* label) {
    (void)label;        // nothing to label: a program is not a GL object any more
}
#endif
