/**
 * @file llframetimer.h
 * @brief A lightweight timer that measures seconds and is only
 * updated once per frame.
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

#ifndef LL_LLFRAMETIMER_H
#define LL_LLFRAMETIMER_H

/**
 * *NOTE: Because of limitations on linux which we do not really have
 * time to explore, the total time is derived from the frame time
 * and is recsynchronized on every frame.
 */

#include "lltimer.h"

class LL_COMMON_API LLFrameTimer
{
public:
    LLFrameTimer() : mStartTime( sFrameTime ), mExpiry(0), mStarted(true) {}

    // Return the number of seconds since the start of this
    // application instance.
    static F64SecondsImplicit getElapsedSeconds()
    {
        // Loses msec precision after ~4.5 hours...
        return sFrameTime;
    }

    // <FS> Rekindled 2026-11-24: the frame start in the simulation clock, microseconds since epoch (the same scale as getTotalTime). sFrameTime and the frame delta are this clock: it
    // advances by the average of the last few real frame deltas (never ahead of the real time, never more than 16 ms behind it), so frames that alternate between a short and a long step under
    // vsync still step the animation, the physics and the object update evenly. RK_VK_SIMCLOCK=0 makes it the real clock again, RK_VK_SIMCLOCK=N averages N frames (default 4); it is also pulled gently toward the real time.
    static U64 getSimTotalTime()
    {
        return sSimTotalTime ? sSimTotalTime : sTotalTime;
    }

    // Return a low precision usec since epoch
    static U64 getTotalTime()
    {
        return sTotalTime ? U64MicrosecondsImplicit(sTotalTime) : totalTime();
    }

    // Return a low precision seconds since epoch
    static F64 getTotalSeconds()
    {
        return sTotalSeconds;
    }

    // Call this method once per frame to update the current frame time.   This is actually called
    // at some other times as well
    static void updateFrameTime();

    // Call this method once, and only once, per frame to update the current frame count.
    static void updateFrameCount()                  { sFrameCount++; }

    static U32  getFrameCount()                     { return sFrameCount; }

    static F32  getFrameDeltaTimeF32();

    // Return seconds since the current frame started
    static F32  getCurrentFrameTime();

    // MANIPULATORS
    void start();
    void stop();
    void reset();
    void resetWithExpiry(F32 expiration);
    void pause();
    void unpause();
    void setTimerExpirySec(F32 expiration);
    void setExpiryAt(F64 seconds_since_epoch);
    bool checkExpirationAndReset(F32 expiration);
    F32 getElapsedTimeAndResetF32()                 { F32 t = F32(sFrameTime - mStartTime); reset(); return t; }

    void setAge(const F64 age)                      { mStartTime = sFrameTime - age; }

    // ACCESSORS
    bool hasExpired() const                         { return (sFrameTime >= mExpiry); }
    F32  getTimeToExpireF32() const                 { return (F32)(mExpiry - sFrameTime); }
    F32  getElapsedTimeF32() const                  { return mStarted ? (F32)(sFrameTime - mStartTime) : (F32)mStartTime; }
    bool getStarted() const                         { return mStarted; }

    // return the seconds since epoch when this timer will expire.
    F64 expiresAt() const;

protected:
    // A single, high resolution timer that drives all LLFrameTimers
    // *NOTE: no longer used.
    //static LLTimer sInternalTimer;

    //
    // Aplication constants
    //

    // Start time of opp in usec since epoch
    static U64 sStartTotalTime;

    //
    // Data updated per frame
    //

    // Seconds since application start
    static F64 sFrameTime;

    // Time that has elapsed since last call to updateFrameTime()
    static U64 sFrameDeltaTime;

    // Total microseconds since epoch.
    static U64 sTotalTime;

    // <FS> the smoothed simulation clock, microseconds since epoch (0 before the first frame)
    static U64 sSimTotalTime;

    // Seconds since epoch.
    static F64 sTotalSeconds;

    // Total number of frames elapsed in application
    static S32 sFrameCount;

    //
    // Member data
    //

    // Number of seconds after application start when this timer was
    // started. Set equal to sFrameTime when reset.
    F64 mStartTime;

    // Timer expires this many seconds after application start time.
    F64 mExpiry;

    // Useful bit of state usually associated with timers, but does
    // not affect actual functionality
    bool mStarted;
};

// Glue code for Havok (or anything else that doesn't want the full .h files)
extern F32  getCurrentFrameTime();

#endif  // LL_LLFRAMETIMER_H
