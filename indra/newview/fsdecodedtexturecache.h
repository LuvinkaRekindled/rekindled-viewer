/**
 * @file fsdecodedtexturecache.h
 * @brief A disk cache of decoded textures (doc/vulkan/PLAN-aurora-ideas.md section 2, stage a).
 *
 * The texture cache keeps the JPEG 2000 bytes, so every session decodes every texture again. This cache keeps what the decode produced: one file per texture
 * id with the finest level decoded so far and its mip chain below it (a 2x2 box filter per level), so that a request for any discard level from the finest
 * one down is one read of one level, no decode and no scaling. Texture assets are immutable by id, so a stored level is valid for ever; the only
 * invalidations are the user's "clear cache", a format version change, and the texture cache removing a texture it found corrupt.
 *
 * Threads: reads run on the image decode pool, in place of the decode (LLDecodedImageCache, llimageworker.h); writes are queued (a copy of the decoded
 * image, 256 MB of queue at most, a full queue drops the write and the texture is decoded again next time) to one writer thread, which also builds the
 * index at start-up (one directory listing: the discard level is in the file name) and keeps the cache under its size cap by removing the files read
 * least recently (a hit touches the file's time). Nothing here runs on the main thread except the once-a-minute report and the settings read.
 *
 * Settings: FSDecodedTextureCache (on/off, live: off means no read and no write, today's path), FSDecodedTextureCacheSizeMB (the cap).
 * Stage b (BC7 at decode, taking over RenderCompressTextures) will add a format to the file header; the chain layout is made for it.
 */

#ifndef FS_DECODEDTEXTURECACHE_H
#define FS_DECODEDTEXTURECACHE_H

#include "llimageworker.h"
#include "lldir.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <string>

#include "threadpool_fwd.h"     // LL::ThreadPool is an alias there

class FSDecodedTextureCache : public LLDecodedImageCache
{
public:
    static FSDecodedTextureCache& instance();

    // Main thread, after the texture cache's initCache: the directory under 'location' (<cache>/decodedcache), the writer thread, the index scan (on the writer).
    // 'purge' (the texture cache was purged or its version changed) removes everything first.
    void init(ELLPath location, bool purge);
    // Main thread, after the image decode pool has shut down (no reader is running then): closes the writer, drops the index.
    void shutdown();

    // LLDecodedImageCache (decode pool threads)
    LLPointer<LLImageRaw> read(const LLUUID& id, S32 discard) override;
    void write(const LLUUID& id, S32 discard, const LLImageRaw* raw) override;

    // Main thread, every frame (cheap): reads the settings, logs a report once a minute while anything happened
    void update();

    // the texture cache removed this texture (corrupt): ours goes too. Any thread.
    void remove(const LLUUID& id);
    // the user cleared the cache (or its version changed): everything goes. Main thread, before or after init.
    void purgeAll();

    bool enabled() const { return mEnabled.load(std::memory_order_relaxed); }

private:
    FSDecodedTextureCache();
    ~FSDecodedTextureCache();

    struct Entry
    {
        S32 mDiscard = 0;       // the finest level in the file
        U64 mSize = 0;          // bytes on disk
        S64 mTime = 0;          // last read or write, seconds (the eviction order)
    };

    std::string fileName(const LLUUID& id, S32 discard) const;
    std::string subDir(const LLUUID& id) const;
    void scanIndex();                                   // writer thread, at init
    void writeFile(const LLUUID& id, S32 discard, LLPointer<LLImageRaw> raw);     // writer thread
    void touch(const LLUUID& id);                       // writer thread: the file's time and the index
    void evict();                                       // writer thread, index lock held by the caller
    bool post(std::function<void()> work);              // to the writer; false when closed

    std::string mDir;
    std::unique_ptr<LL::ThreadPool> mWriter;
    std::mutex mIndexMutex;
    std::unordered_map<LLUUID, Entry> mIndex;           // what is on disk (the writer keeps it; readers look up)
    U64 mTotalBytes = 0;                                // sum of mIndex sizes
    bool mScanned = false;
    bool mInitialised = false;

    std::atomic<bool> mEnabled { false };
    std::atomic<U64> mCapBytes { 4ull << 30 };
    std::atomic<S64> mQueuedBytes { 0 };                // decoded bytes waiting on the writer

    // statistics since the last report
    std::atomic<U32> mHits { 0 }, mMissAbsent { 0 }, mMissCoarser { 0 }, mMissBad { 0 }, mWrites { 0 }, mDrops { 0 }, mSkips { 0 }, mEvictions { 0 };
    std::atomic<U64> mHitBytes { 0 }, mWriteBytes { 0 };
    F64 mLastReport = 0.0;
};

#endif // FS_DECODEDTEXTURECACHE_H
