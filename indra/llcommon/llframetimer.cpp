/**
 * @file llframetimer.cpp
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
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

#include "u64.h"

#include "llframetimer.h"
#include "rkdiag.h"
#include <algorithm>
#include <cmath>

// Static members
//LLTimer   LLFrameTimer::sInternalTimer;
U64 LLFrameTimer::sStartTotalTime = totalTime();
F64 LLFrameTimer::sFrameTime = 0.0;
U64 LLFrameTimer::sTotalTime = 0;
U64 LLFrameTimer::sSimTotalTime = 0;
F64 LLFrameTimer::sTotalSeconds = 0.0;
S32 LLFrameTimer::sFrameCount = 0;
U64 LLFrameTimer::sFrameDeltaTime = 0;
const F64 USEC_TO_SEC_F64 = 0.000001;

namespace
{
// <FS> Rekindled 2026-11-24: the simulation clock. Under vsync the viewer's frames came in short and long pairs (3.5 and 13 ms at 120 Hz: the display gives the swapchain images back in bursts), while
// the picture is shown at an even rate. The animation, the avatar physics and the object update stepped by the real, uneven delta, so everything was sampled at uneven times (a pose step divided
// by a 2.7 ms step made the breast physics swing over its whole range). The frame time and the frame delta now come from a clock that advances by the average of the last few real deltas, kept within
// 16 ms behind the real time and never ahead of it, so a real hitch still passes through. RK_VK_SIMCLOCK=0: the real clock as before; =N: average N frames (2..16, default 4: with 3 swapchain images the step spread went from 5.3 to 1.8 ms).
struct SimClock
{
    static const int kMax = 16;
    static const U64 kMaxLagUs = 16000;
    int mWindow = 4;
    U64 mRing[kMax] = {};
    int mCount = 0, mNext = 0;
    // RK_VK_DIAG: raw against smoothed deltas, every 600 frames
    U32 mFrames = 0;
    F64 mRawSum = 0, mRawSq = 0, mSimSum = 0, mSimSq = 0, mLagSum = 0;
    U64 mMaxLag = 0;

    SimClock()
    {
        const char* v = getenv("RK_VK_SIMCLOCK");
        if (v && v[0])
        {
            mWindow = atoi(v);
            if (mWindow < 0) mWindow = 0;
            if (mWindow > kMax) mWindow = kMax;
        }
    }

    // prev_sim: the simulation time of the previous frame; total_time: the real time now; raw_delta: the real delta since the previous frame. Returns the simulation time of this frame.
    U64 step(U64 prev_sim, U64 total_time, U64 raw_delta)
    {
        U64 sim = total_time;
        if (mWindow > 1)
        {
            mRing[mNext] = raw_delta;
            mNext = (mNext + 1) % mWindow;
            if (mCount < mWindow) ++mCount;
            U64 sum = 0;
            for (int i = 0; i < mCount; ++i) sum += mRing[i];
            sim = prev_sim + sum / (U64)mCount;
            if (sim > total_time)
            {
                sim = total_time;                       // never ahead of the real time
            }
            else
            {
                sim += (total_time - sim) / 16;         // a gentle pull toward the real time (an average of the deltas alone never gives back what a hitch cost)
                if (total_time - sim > kMaxLagUs)
                {
                    sim = total_time - kMaxLagUs;       // and never far behind it: a real hitch passes through
                }
            }
            if (sim < prev_sim)
            {
                sim = prev_sim;                         // monotonic (the system clock can go backwards)
            }
        }
        else if (sim < prev_sim)
        {
            sim = prev_sim;
        }
        if (rk_diag_on())
        {
            const F64 raw_ms = U64_to_F64(raw_delta) * 0.001, sim_ms = U64_to_F64(sim - prev_sim) * 0.001;
            mRawSum += raw_ms; mRawSq += raw_ms * raw_ms; mSimSum += sim_ms; mSimSq += sim_ms * sim_ms;
            const U64 lag = total_time > sim ? total_time - sim : 0;
            if (lag > mMaxLag) mMaxLag = lag;
            mLagSum += U64_to_F64(lag) * 0.001;
            if (++mFrames >= 600)
            {
                const F64 n = (F64)mFrames;
                const F64 raw_sd = sqrt(std::max(0.0, mRawSq / n - (mRawSum / n) * (mRawSum / n)));
                const F64 sim_sd = sqrt(std::max(0.0, mSimSq / n - (mSimSum / n) * (mSimSum / n)));
                LL_INFOS("SimClock") << "simulation clock (window " << mWindow << "), last " << mFrames << " frames: real delta mean " << mRawSum / n << " ms, spread " << raw_sd << " ms | simulation delta mean "
                                     << mSimSum / n << " ms, spread " << sim_sd << " ms | the simulation clock was behind the real one by " << mLagSum / n << " ms on average and at most " << U64_to_F64(mMaxLag) * 0.001 << " ms" << LL_ENDL;
                mFrames = 0; mRawSum = mRawSq = mSimSum = mSimSq = mLagSum = 0; mMaxLag = 0;
            }
        }
        return sim;
    }
};
}

// static
void LLFrameTimer::updateFrameTime()
{
    static SimClock sim_clock;
    U64 total_time = totalTime();
    const bool first = (sTotalTime == 0);
    const U64 raw_delta = first ? total_time : (total_time > sTotalTime ? total_time - sTotalTime : 0);
    if (first)
    {
        sSimTotalTime = total_time;                     // the first call: as before (the delta is the whole time since epoch)
        sFrameDeltaTime = raw_delta;
    }
    else
    {
        const U64 prev_sim = sSimTotalTime ? sSimTotalTime : sTotalTime;
        sSimTotalTime = sim_clock.step(prev_sim, total_time, raw_delta);
        sFrameDeltaTime = sSimTotalTime - prev_sim;
    }
    sTotalTime = total_time;
    sTotalSeconds = U64_to_F64(sTotalTime) * USEC_TO_SEC_F64;
    sFrameTime = U64_to_F64(sSimTotalTime - sStartTotalTime) * USEC_TO_SEC_F64;
}

void LLFrameTimer::start()
{
    reset();
    mStarted = true;
}

void LLFrameTimer::stop()
{
    mStarted = false;
}

void LLFrameTimer::reset()
{
    mStartTime = sFrameTime;
    mExpiry = sFrameTime;
}

void LLFrameTimer::resetWithExpiry(F32 expiration)
{
    reset();
    setTimerExpirySec(expiration);
}

// Don't combine pause/unpause with start/stop
// Useage:
//  LLFrameTime foo; // starts automatically
//  foo.unpause(); // noop but safe
//  foo.pause(); // pauses timer
//  foo.unpause() // unpauses
//  F32 elapsed = foo.getElapsedTimeF32() // does not include time between pause() and unpause()
//  Note: elapsed would also be valid with no unpause() call (= time run until pause() called)
void LLFrameTimer::pause()
{
    if (mStarted)
        mStartTime = sFrameTime - mStartTime; // save dtime
    mStarted = false;
}

void LLFrameTimer::unpause()
{
    if (!mStarted)
        mStartTime = sFrameTime - mStartTime; // restore dtime
    mStarted = true;
}

void LLFrameTimer::setTimerExpirySec(F32 expiration)
{
    mExpiry = expiration + mStartTime;
}

void LLFrameTimer::setExpiryAt(F64 seconds_since_epoch)
{
    mStartTime = sFrameTime;
    mExpiry = seconds_since_epoch - (USEC_TO_SEC_F64 * sStartTotalTime);
}

F64 LLFrameTimer::expiresAt() const
{
    F64 expires_at = U64_to_F64(sStartTotalTime) * USEC_TO_SEC_F64;
    expires_at += mExpiry;
    return expires_at;
}

bool LLFrameTimer::checkExpirationAndReset(F32 expiration)
{
    //LL_INFOS() << "LLFrameTimer::checkExpirationAndReset()" << LL_ENDL;
    //LL_INFOS() << "  mStartTime:" << mStartTime << LL_ENDL;
    //LL_INFOS() << "  sFrameTime:" << sFrameTime << LL_ENDL;
    //LL_INFOS() << "  mExpiry:   " <<  mExpiry << LL_ENDL;

    if(hasExpired())
    {
        reset();
        setTimerExpirySec(expiration);
        return true;
    }
    return false;
}

// static
F32 LLFrameTimer::getFrameDeltaTimeF32()
{
    return (F32)(U64_to_F64(sFrameDeltaTime) * USEC_TO_SEC_F64);
}


//  static
// Return seconds since the current frame started
F32  LLFrameTimer::getCurrentFrameTime()
{
    U64 frame_time = totalTime() - sTotalTime;
    return (F32)(U64_to_F64(frame_time) * USEC_TO_SEC_F64);
}

// Glue code to avoid full class .h file #includes
F32  getCurrentFrameTime()
{
    return (F32)(LLFrameTimer::getCurrentFrameTime());
}
