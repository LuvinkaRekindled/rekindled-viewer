/**
 * @file llaudiostall.h
 * @brief Timer for calls of the audio code that run on the main thread: logs one line the moment a call takes longer than its limit
 *
 * The audio engine is called from the main loop (LLAudioEngine::idle and the audio_update_* calls before it) and it calls FMOD synchronously:
 * a sound load, a device change or an FMOD update that takes seconds is a frame that takes seconds. The cpu zones only show an average over a
 * window, so these timers name the call and the sound the moment it happens ("AudioStall" tag). doc/vulkan/PLAN-audio-main-thread.md.
 */

#ifndef LL_LLAUDIOSTALL_H
#define LL_LLAUDIOSTALL_H

#include "linden_common.h"
#include "lluuid.h"
#include <chrono>

class LLAudioStallTimer
{
public:
    LLAudioStallTimer(const char* what, U32 limit_ms = 20) : mWhat(what), mLimitMs(limit_ms), mStart(std::chrono::steady_clock::now()), mDone(false) {}
    ~LLAudioStallTimer() { stop(); }

    void setId(const LLUUID& id) { mId = id; }
    void setDetail(const std::string& detail) { mDetail = detail; }

    // ends the timing before the end of the scope
    void stop()
    {
        if (mDone)
        {
            return;
        }
        mDone = true;
        const F64 ms = std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now() - mStart).count();
        if (ms >= (F64)mLimitMs)
        {
            LL_WARNS("AudioStall") << mWhat << " took " << ms << " ms (limit " << mLimitMs << ")"
                                   << (mId.notNull() ? " sound " + mId.asString() : std::string())
                                   << (mDetail.empty() ? std::string() : " " + mDetail) << LL_ENDL;
        }
    }

private:
    const char* mWhat;
    U32 mLimitMs;
    std::chrono::steady_clock::time_point mStart;
    bool mDone;
    LLUUID mId;
    std::string mDetail;
};

#endif // LL_LLAUDIOSTALL_H
