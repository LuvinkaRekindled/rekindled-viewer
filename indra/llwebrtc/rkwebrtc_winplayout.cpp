/**
 * @file rkwebrtc_winplayout.cpp
 * @brief Windows voice playout through a normal media stream (see the header)
 */

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

#include "rkwebrtc_winplayout.h"

namespace
{
template <class T>
struct ComPtr
{
    T* p = nullptr;
    ~ComPtr() { reset(); }
    void reset()
    {
        if (p)
        {
            p->Release();
            p = nullptr;
        }
    }
    T**  operator&() { reset(); return &p; }
    T*   operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

std::wstring widen(const std::string& s)
{
    if (s.empty())
    {
        return std::wstring();
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int) s.size(), nullptr, 0);
    std::wstring w((size_t) n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int) s.size(), &w[0], n);
    return w;
}
} // namespace

RKWinMediaPlayout::RKWinMediaPlayout() {}

RKWinMediaPlayout::~RKWinMediaPlayout()
{
    stop();
}

bool RKWinMediaPlayout::start(const std::string& endpoint_id, PullFn pull)
{
    stop();
    mStop.store(false);
    std::atomic<int> started{ 0 }; // 0 starting, 1 running, -1 failed
    mThread = std::thread([this, endpoint_id, pull, &started]() { run(endpoint_id, pull, &started); });
    while (started.load(std::memory_order_acquire) == 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (started.load() < 0)
    {
        mThread.join();
        return false;
    }
    return true;
}

void RKWinMediaPlayout::stop()
{
    if (mThread.joinable())
    {
        mStop.store(true);
        mThread.join();
    }
    mRunning.store(false, std::memory_order_release);
}

void RKWinMediaPlayout::run(std::string endpoint_id, PullFn pull, std::atomic<int>* started)
{
    const bool com_ok = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    HANDLE     event  = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    DWORD      mmcss  = 0;
    HANDLE     task   = AvSetMmThreadCharacteristicsW(L"Audio", &mmcss);

    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice>           device;
    ComPtr<IAudioClient2>       client;
    ComPtr<IAudioRenderClient>  render;
    bool                        ok = false;

    do
    {
        if (!event)
        {
            break;
        }
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**) &enumerator)))
        {
            break;
        }
        if (!endpoint_id.empty())
        {
            std::wstring wid = widen(endpoint_id);
            if (FAILED(enumerator->GetDevice(wid.c_str(), &device)))
            {
                device.reset();
            }
        }
        if (!device && FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)))
        {
            break;
        }
        if (FAILED(device->Activate(__uuidof(IAudioClient2), CLSCTX_ALL, nullptr, (void**) &client)))
        {
            break;
        }

        // A media stream, not a communications one.
        AudioClientProperties props = {};
        props.cbSize                = sizeof(props);
        props.bIsOffload            = FALSE;
        props.eCategory             = AudioCategory_Media;
        client->SetClientProperties(&props);

        WAVEFORMATEX fmt    = {};
        fmt.wFormatTag      = WAVE_FORMAT_PCM;
        fmt.nChannels       = kChannels;
        fmt.nSamplesPerSec  = kRate;
        fmt.wBitsPerSample  = 16;
        fmt.nBlockAlign     = (WORD) (fmt.nChannels * fmt.wBitsPerSample / 8);
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

        REFERENCE_TIME default_period = 0, min_period = 0;
        client->GetDevicePeriod(&default_period, &min_period);
        if (default_period < 100000) // 10 ms
        {
            default_period = 100000;
        }
        // The engine converts to the device format.
        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, default_period * 2, 0, &fmt, nullptr)))
        {
            break;
        }
        if (FAILED(client->SetEventHandle(event)))
        {
            break;
        }
        UINT32 buffer_frames = 0;
        if (FAILED(client->GetBufferSize(&buffer_frames)))
        {
            break;
        }
        if (FAILED(client->GetService(__uuidof(IAudioRenderClient), (void**) &render)))
        {
            break;
        }
        // Start with silence so the first period is not garbage.
        BYTE* data = nullptr;
        if (SUCCEEDED(render->GetBuffer(buffer_frames, &data)))
        {
            render->ReleaseBuffer(buffer_frames, AUDCLNT_BUFFERFLAGS_SILENT);
        }
        if (FAILED(client->Start()))
        {
            break;
        }
        ok = true;
    } while (false);

    started->store(ok ? 1 : -1, std::memory_order_release);

    if (ok)
    {
        mRunning.store(true, std::memory_order_release);
        const size_t   frame_bytes = kChannels * sizeof(int16_t);
        std::vector<BYTE> staged((size_t) kChunkFrames * frame_bytes);
        size_t         staged_frames = 0; // frames left in |staged|, taken from its end
        UINT32         buffer_frames = 0;
        client->GetBufferSize(&buffer_frames);

        while (!mStop.load(std::memory_order_acquire))
        {
            if (WaitForSingleObject(event, 200) == WAIT_FAILED)
            {
                break;
            }
            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding)))
            {
                break;
            }
            UINT32 available = buffer_frames - padding;
            if (available == 0)
            {
                continue;
            }
            BYTE* out = nullptr;
            if (FAILED(render->GetBuffer(available, &out)))
            {
                break;
            }
            UINT32 written = 0;
            while (written < available)
            {
                if (staged_frames == 0)
                {
                    pull(staged.data(), kChunkFrames);
                    staged_frames = kChunkFrames;
                }
                const size_t take = std::min<size_t>(staged_frames, available - written);
                memcpy(out + (size_t) written * frame_bytes, staged.data() + (kChunkFrames - staged_frames) * frame_bytes, take * frame_bytes);
                staged_frames -= take;
                written += (UINT32) take;
            }
            render->ReleaseBuffer(available, 0);
        }
        client->Stop();
        mRunning.store(false, std::memory_order_release);
    }

    render.reset();
    client.reset();
    device.reset();
    enumerator.reset();
    if (task)
    {
        AvRevertMmThreadCharacteristics(task);
    }
    if (event)
    {
        CloseHandle(event);
    }
    if (com_ok)
    {
        CoUninitialize();
    }
}

#endif // _WIN32
