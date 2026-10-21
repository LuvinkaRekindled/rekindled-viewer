/**
 * @file lluniformcensus.h
 * @brief U0 (doc/vulkan/U-pass-uniforms-design.md): counters for where the uniform / shader bind / texture bind work of the main thread goes. Off unless LL_VK_UNICENSUS=1
 *        (every hook is one branch on a constant). Header only. Calls are attributed to the innermost CPU zone (LL_CPU_ZONE, FS_CPU_ZONES=1 is needed for that part) and to the program.
 *        The log lines (tag UniCensus) are written by LLCpuZoneScope::frame() every 600 frames.
 */
#ifndef LL_LLUNIFORMCENSUS_H
#define LL_LLUNIFORMCENSUS_H

#include "stdtypes.h"
#include <chrono>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

class LLShaderProgram;

namespace LLUniCensus
{
    struct Counters
    {
        U64 mCalls = 0;         // uniform writes that reached the program's class block compare (the mirror* entry points)
        U64 mChanged = 0;       // ... of which changed a value (a range goes to the worker)
        U64 mBytes = 0;         // bytes of those ranges
        U64 mTex = 0;           // LLTexUnit bind / bindManual / bindFast calls
        U64 mBinds = 0;         // LLShaderProgram::bind calls
        U64 mScopes = 0;        // times the zone was entered
    };

    const int SECTIONS = 16;
    struct ProgStats
    {
        U64 mCalls = 0, mChanged = 0, mBinds = 0;                       // everything that touched the program
        U64 mDefBinds = 0, mDefNs = 0, mDefCalls = 0, mDefChanged = 0, mDefTex = 0;       // LLPipeline::bindDeferredShader calls on it: count, wall time, uniform writes / changes and texture binds inside
        U64 mUpdBinds = 0, mUpdNs = 0, mUpdCalls = 0;                   // the times LLShaderProgram::bind had to run LLShaderMgr::updateShaderUniforms (the program's environment uniforms were dirty): count, wall time, uniform writes
        std::vector<U32> mUniCalls, mUniChanged;                        // by the program's own uniform index: writes and changes
        U64 mSec[SECTIONS] = {};                                        // wall time of the sections of bindDeferredShader (see pipeline.cpp)
        bool mReg = false;
    };
    struct ProgReg { const std::string* mName; ProgStats* mStats; const LLShaderProgram* mProg; };

    inline const bool sOn = getenv("LL_VK_UNICENSUS") != nullptr;
    inline Counters sTotal;
    inline std::unordered_map<const char*, Counters> sByZone;
    inline Counters* sCur = nullptr;
    inline std::vector<Counters*> sStack;
    inline std::vector<ProgReg> sProgs;

    inline void push(const char* name)
    {
        if (!sOn)
        {
            return;
        }
        Counters& c = sByZone[name];
        ++c.mScopes;
        sStack.push_back(sCur);
        sCur = &c;
    }
    inline void pop()
    {
        if (!sOn || sStack.empty())
        {
            return;
        }
        sCur = sStack.back();
        sStack.pop_back();
    }
    inline void note_call() { ++sTotal.mCalls; if (sCur) ++sCur->mCalls; }
    inline void note_changed(U32 bytes) { ++sTotal.mChanged; sTotal.mBytes += bytes; if (sCur) { ++sCur->mChanged; sCur->mBytes += bytes; } }
    inline void note_tex() { ++sTotal.mTex; if (sCur) ++sCur->mTex; }
    inline void note_bind() { ++sTotal.mBinds; if (sCur) ++sCur->mBinds; }
    inline long long now_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

    // the sections of one bindDeferredShader call: lap(n) adds the time since the last lap to section n of the program
    struct Lap
    {
        ProgStats* mP = nullptr;
        long long mT0 = 0, mT = 0;
        U64 mCalls0 = 0, mChanged0 = 0, mTex0 = 0;
        explicit Lap(ProgStats& p)
        {
            if (sOn)
            {
                mP = &p;
                mT0 = mT = now_ns();
                mCalls0 = sTotal.mCalls;
                mChanged0 = sTotal.mChanged;
                mTex0 = sTotal.mTex;
            }
        }
        void operator()(int section)
        {
            if (mP)
            {
                const long long n = now_ns();
                mP->mSec[section] += (U64)(n - mT);
                mT = n;
            }
        }
        ~Lap()
        {
            if (mP)
            {
                const long long n = now_ns();
                mP->mSec[SECTIONS - 1] += (U64)(n - mT);      // the rest after the last lap
                ++mP->mDefBinds;
                mP->mDefNs += (U64)(n - mT0);
                mP->mDefCalls += sTotal.mCalls - mCalls0;
                mP->mDefChanged += sTotal.mChanged - mChanged0;
                mP->mDefTex += sTotal.mTex - mTex0;
            }
        }
    };
}

#endif // LL_LLUNIFORMCENSUS_H
