/**
 * @file rkgpuzonetimer.h
 * @brief Logs how much GPU time each profiled zone takes
 *
 * The viewer's GPU profiling zones (LL_PROFILE_GPU_ZONE) are normally only visible in Tracy. While
 * the FSGpuZoneTimers setting is on, they also record GL timestamp queries, and a per-zone summary
 * (average GPU milliseconds and calls per frame) is written to the log every ten seconds.
 */

#ifndef LL_LLGPUZONETIMER_H
#define LL_LLGPUZONETIMER_H

class RKGpuZoneTimer
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

// <FS> CPU zones of the main thread. With the environment variable FS_CPU_ZONES=1 every LL_CPU_ZONE scope adds its wall time to a named, inclusive zone and the log
// (tag CpuZones) gets a summary of the milliseconds per frame of each zone every 600 frames. Does nothing without the variable.
class RKCpuZoneScope
{
public:
    explicit RKCpuZoneScope(const char* name);
    ~RKCpuZoneScope();
    // once per main loop iteration
    static void frame();
private:
    const char* mName;
    long long mStart;
    unsigned long long mStartCycles;      // FS_CPU_ZONES_CPU=1: the thread's cycle counter at the start (the zone then also reports its blocked time)
};
#define LL_CPU_ZONE_CAT2(a, b) a##b
#define LL_CPU_ZONE_CAT(a, b) LL_CPU_ZONE_CAT2(a, b)
#define LL_CPU_ZONE(name) RKCpuZoneScope LL_CPU_ZONE_CAT(llcpuzone_, __LINE__)(name);

// LL_VK_HITCHWATCH=<ms>: a watcher thread that, when the main loop has not finished a frame for <ms> (a hitch), writes the stacks of all threads to the log
// ("HitchWatch" tag), again every 500 ms while the frame is still not done. Called once a frame from the main loop.
namespace HitchWatch
{
    void frame();
}

#endif // LL_LLGPUZONETIMER_H
