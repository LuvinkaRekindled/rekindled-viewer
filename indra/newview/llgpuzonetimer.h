/**
 * @file llgpuzonetimer.h
 * @brief Logs how much GPU time each profiled zone takes
 *
 * The viewer's GPU profiling zones (LL_PROFILE_GPU_ZONE) are normally only visible in Tracy. While
 * the FSGpuZoneTimers setting is on, they also record GL timestamp queries, and a per-zone summary
 * (average GPU milliseconds and calls per frame) is written to the log every ten seconds.
 */

#ifndef LL_LLGPUZONETIMER_H
#define LL_LLGPUZONETIMER_H

class LLGpuZoneTimer
{
public:
    // Call once per frame, right after the buffer swap. Installs or removes the zone hooks to
    // follow the setting, collects finished queries and writes the periodic log summary.
    static void onFrameEnd();

    // True while the timers are on; lets callers skip counting work entirely when they are off.
    static bool isActive();

    // Adds to a named per-frame counter, printed next to the zones as "counter" lines.
    static void count(const char* name, double value);
};

#endif // LL_LLGPUZONETIMER_H
