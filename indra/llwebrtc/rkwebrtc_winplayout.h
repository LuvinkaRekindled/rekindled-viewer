/**
 * @file rkwebrtc_winplayout.h
 * @brief Windows voice playout through a normal media stream
 *
 * <FS> Rekindled. libwebrtc's own Windows playout opens its render stream as a communications stream.
 * While one is open on a Bluetooth headset Windows narrows everything played on that device to about
 * 6 kHz and halves the level, the world sound included. This plays the decoded voice through an
 * ordinary shared-mode stream instead; the voice engine's data is pulled by a callback.
 *
 * Kept free of libwebrtc headers so the Windows audio headers stay out of them.
 */
#pragma once

#if defined(_WIN32)

#include <atomic>
#include <functional>
#include <string>
#include <thread>

class RKWinMediaPlayout
{
public:
    static const int kRate        = 48000;
    static const int kChannels    = 2;
    static const int kChunkFrames = 480;   // 10 ms, the unit the voice engine produces

    // Fills |frames| frames of 16 bit interleaved stereo at 48 kHz.
    using PullFn = std::function<void(void* buffer, size_t frames)>;

    RKWinMediaPlayout();
    ~RKWinMediaPlayout();

    // |endpoint_id| is a Windows endpoint id in UTF-8; empty (or one that no longer exists) = the
    // default playback device for the console role. Returns when the stream runs or has failed.
    bool start(const std::string& endpoint_id, PullFn pull);
    void stop();
    bool running() const { return mRunning.load(std::memory_order_acquire); }

private:
    void run(std::string endpoint_id, PullFn pull, std::atomic<int>* started);

    std::thread       mThread;
    std::atomic<bool> mStop{ false };
    std::atomic<bool> mRunning{ false };
};

#endif // _WIN32
