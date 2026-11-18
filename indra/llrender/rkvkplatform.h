/**
 * @file rkvkplatform.h
 * @brief What the Vulkan renderer takes from the platform: on Windows <windows.h> and <intrin.h> as before, on Linux the few calls it uses with the same names
 *        (the performance counter, Sleep, thread ids, GetEnvironmentVariableA, LoadLibrary / GetProcAddress, the MSVC bit intrinsics),
 *        so the renderer sources keep one spelling. Linux build: doc/vulkan/PLAN-linux-build.md
 */

#ifndef LL_LLVKPLATFORM_H
#define LL_LLVKPLATFORM_H

#if LL_WINDOWS

#include <windows.h>
#include <intrin.h>

#else // Linux

#include <x86intrin.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <strings.h>
#include <execinfo.h>
#include <errno.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <linux/futex.h>
#include <type_traits>

typedef uint32_t DWORD;
typedef uint16_t USHORT;
typedef int BOOL;
typedef int32_t LONG;
typedef long long LONGLONG;
typedef uintptr_t DWORD_PTR;
#define WINAPI
typedef void* LPVOID;
#ifndef MAX_PATH
#define MAX_PATH 4096
#endif
typedef void* HANDLE;
typedef void* HMODULE;
typedef void* HWND;                 // the platform window the renderer presents into (an SDL_Window* on Linux)

typedef union _LARGE_INTEGER
{
    struct { DWORD LowPart; int32_t HighPart; };
    int64_t QuadPart;
} LARGE_INTEGER;

// the performance counter counts nanoseconds
inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* f)
{
    f->QuadPart = 1000000000LL;
    return 1;
}

inline BOOL QueryPerformanceCounter(LARGE_INTEGER* c)
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    c->QuadPart = (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
    return 1;
}

inline void Sleep(DWORD ms)
{
    if (ms == 0)
    {
        sched_yield();
        return;
    }
    timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, nullptr);
}

inline DWORD GetCurrentThreadId()
{
    return (DWORD)syscall(SYS_gettid);
}

inline DWORD GetEnvironmentVariableA(const char* name, char* buffer, DWORD size)
{
    const char* v = getenv(name);
    if (!v)
    {
        return 0;
    }
    const size_t n = strlen(v);
    if (n + 1 > size)
    {
        return (DWORD)(n + 1);
    }
    memcpy(buffer, v, n + 1);
    return (DWORD)n;
}

inline HMODULE LoadLibraryA(const char* name) { return dlopen(name, RTLD_NOW | RTLD_LOCAL); }
inline void* GetProcAddress(HMODULE m, const char* name) { return dlsym(m, name); }
inline BOOL FreeLibrary(HMODULE m) { return dlclose(m) == 0; }

#define _ReturnAddress() __builtin_return_address(0)
inline int _stricmp(const char* a, const char* b) { return strcasecmp(a, b); }
inline void YieldProcessor() { _mm_pause(); }
inline BOOL SwitchToThread() { return sched_yield() == 0; }
inline DWORD GetLastError() { return (DWORD)errno; }

template <class T> inline T InterlockedExchange(volatile T* p, std::type_identity_t<T> v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
inline LONG InterlockedIncrement(volatile LONG* p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }

// WaitOnAddress / WakeByAddress: a futex on the low 32 bits of the word (the words are little-endian ints or longs)
inline BOOL WaitOnAddress(volatile void* address, void* undesired, size_t /*size*/, DWORD ms)
{
    timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    syscall(SYS_futex, (uint32_t*)address, FUTEX_WAIT_PRIVATE, *(uint32_t*)undesired, ms == 0xFFFFFFFFu ? nullptr : &ts, nullptr, 0);
    return 1;
}
inline void WakeByAddressSingle(void* address) { syscall(SYS_futex, (uint32_t*)address, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0); }
inline void WakeByAddressAll(void* address) { syscall(SYS_futex, (uint32_t*)address, FUTEX_WAKE_PRIVATE, 0x7FFFFFFF, nullptr, nullptr, 0); }

// threads: CreateThread / WaitForSingleObject / CloseHandle over pthreads (the worker thread is the only user)
struct RKVkThreadHandle
{
    pthread_t mThread;
    DWORD (*mProc)(LPVOID);
    LPVOID mParam;
};
inline void* ll_vk_thread_entry(void* p)
{
    RKVkThreadHandle* h = (RKVkThreadHandle*)p;
    h->mProc(h->mParam);
    return nullptr;
}
inline HANDLE CreateThread(void*, size_t, DWORD (*proc)(LPVOID), LPVOID param, DWORD, DWORD*)
{
    RKVkThreadHandle* h = new RKVkThreadHandle{ {}, proc, param };
    if (pthread_create(&h->mThread, nullptr, ll_vk_thread_entry, h) != 0)
    {
        delete h;
        return nullptr;
    }
    return h;
}
inline DWORD WaitForSingleObject(HANDLE handle, DWORD ms)
{
    RKVkThreadHandle* h = (RKVkThreadHandle*)handle;
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }
    return pthread_timedjoin_np(h->mThread, nullptr, &ts) == 0 ? 0 : 0x102;
}
inline BOOL CloseHandle(HANDLE handle) { delete (RKVkThreadHandle*)handle; return 1; }
inline HANDLE GetCurrentThread() { return nullptr; }
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
inline BOOL SetThreadPriority(HANDLE, int priority)
{
    // a lower priority is a higher nice value for this thread (raising it needs rights, so only lowering is done)
    return priority < 0 ? setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 5) == 0 : 1;
}
inline DWORD_PTR SetThreadAffinityMask(HANDLE, DWORD_PTR mask)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int i = 0; i < 64; ++i) { if (mask & ((DWORD_PTR)1 << i)) CPU_SET(i, &set); }
    return sched_setaffinity(0, sizeof(set), &set) == 0 ? (DWORD_PTR)1 : (DWORD_PTR)0;
}
inline DWORD GetCurrentProcessorNumber() { const int c = sched_getcpu(); return c < 0 ? 0 : (DWORD)c; }

// memory, files, stacks
#define MEM_RESERVE 0x2000
#define MEM_COMMIT 0x1000
#define PAGE_READWRITE 0x04
inline void* VirtualAlloc(void*, size_t size, DWORD, DWORD)
{
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}
inline USHORT CaptureStackBackTrace(DWORD skip, DWORD count, void** frames, DWORD*)
{
    void* all[128];
    const int n = backtrace(all, (int)(count + skip > 128 ? 128 : count + skip));
    int out = 0;
    for (int i = (int)skip; i < n && out < (int)count; ++i) { frames[out++] = all[i]; }
    return (USHORT)out;
}
inline DWORD GetModuleFileNameA(HMODULE, char* path, DWORD size)
{
    const ssize_t n = readlink("/proc/self/exe", path, size - 1);
    if (n < 0) { path[0] = 0; return 0; }
    path[n] = 0;
    return (DWORD)n;
}
inline BOOL CreateDirectoryA(const char* path, void*) { return mkdir(path, 0755) == 0; }

// the thread id the Windows code reads from the thread environment block (offset 0x48 of the TEB)
inline unsigned long __readgsdword(unsigned long /*offset*/)
{
    static thread_local unsigned long tid = (unsigned long)syscall(SYS_gettid);
    return tid;
}

// MSVC bit intrinsics
inline unsigned int __popcnt(unsigned int v) { return (unsigned int)__builtin_popcount(v); }
inline unsigned char _BitScanForward(unsigned long* index, unsigned long mask)
{
    if (!mask)
    {
        return 0;
    }
    *index = (unsigned long)__builtin_ctzl(mask);
    return 1;
}

#endif // LL_WINDOWS

#endif // LL_LLVKPLATFORM_H
