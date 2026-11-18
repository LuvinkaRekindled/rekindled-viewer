/**
 * @file rkvksubstream.h
 * @brief Recording draw work on another thread (F3), for code outside the render library (no RK_VULKAN needed to include this).
 *
 * A thread that calls begin() writes its render state, texture binds, uniforms and draws into a private buffer of records (its own render state context, its own pending
 * slots and uniform writes); the main thread calls splice() at the point of the frame where that work would have run, and the buffer joins the queue, in order. See
 * RKVkAsync::SubStream. The sub-stream starts from the render state the main producer has when it is constructed, so build it on the main thread where the work belongs.
 */

#ifndef RK_RKVKSUBSTREAM_H
#define RK_RKVKSUBSTREAM_H

#include <vector>
#include <string>
#include <cstddef>

class RKVkSubStream
{
public:
    // true when records can be written at all (the Vulkan worker runs)
    static bool usable();

    RKVkSubStream();                // main thread
    ~RKVkSubStream();
    void begin();                   // the thread that records
    void end();                     // the same thread
    void splice();                  // main thread: the buffer joins the queue here
    unsigned records() const;

    // UI record cache (doc/vulkan/PLAN-ui-record-cache.md): a view's draw recorded as a segment and played again later
    void reseed();                                          // main thread: the next begin() starts from the render state the main producer has now
    bool takeBytes(std::vector<unsigned char>& out) const;  // the records since begin(), before splice()
    bool leftState() const;                                 // end() found the render state changed: the segment is not a pure draw
    static void segmentBoundary();                          // before and after recording or replaying a segment
    static void replay(const unsigned char* data, size_t size);
    static void replaySideEffects(const unsigned char* data, size_t size);      // of a recording that is not played: the records that are not drawing (uploads, buffer writes), which the work did to the worker as well
    static std::string describeDifference(const unsigned char* a, size_t a_size, const unsigned char* b, size_t b_size);
    struct Scan { bool mCacheable = true; unsigned short mBadOp = 0; unsigned mRecords = 0, mDraws = 0; std::vector<unsigned> mTextures, mBuffers; };
    static bool scan(const unsigned char* data, size_t size, Scan& out);
    static void nameTextures(const unsigned* ids, size_t count);    // the textures a cached segment draws with (a freed one makes every cached segment stale)
    static void nameBuffers(const unsigned* names, size_t count);   // the buffers it draws from (recycled, destroyed or written: the same)
    static unsigned long long cacheEpoch();
    static void cacheInvalidate();
    static void stateSnapshot(std::vector<unsigned char>& out);     // the producer's render state block now
    static bool stateEqual(const std::vector<unsigned char>& snapshot);

private:
    RKVkSubStream(const RKVkSubStream&) = delete;
    RKVkSubStream& operator=(const RKVkSubStream&) = delete;
    void* mImpl;
};

#endif // RK_RKVKSUBSTREAM_H
