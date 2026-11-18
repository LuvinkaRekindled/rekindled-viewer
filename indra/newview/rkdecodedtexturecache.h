/**
 * @file rkdecodedtexturecache.h
 * @brief A disk cache of decoded textures (doc/vulkan/PLAN-aurora-ideas.md section 2, stages a and b).
 *
 * The texture cache keeps the JPEG 2000 bytes, so every session decodes every texture again. This cache keeps what the decode produced: one file per texture
 * id with the finest level decoded so far and its mip chain below it, so that a request for any discard level from the finest one down is one read, no decode
 * and no scaling. Two file formats: raw texels (<uuid>_<discard>.dec, a 2x2 box filter per level) and a BC7 chain (<uuid>_<discard>.dc7, what BC7 at decode
 * produced, rkimagebc7.h), the format in the name so the index needs no file opened. Texture assets are immutable by id, so a stored level is valid for ever;
 * the only invalidations are the user's "clear cache", a format version change, and the texture cache removing a texture it found corrupt.
 *
 * Threads: reads run on the image decode pool, in place of the decode (RKDecodedImageCache, llimageworker.h); writes are queued (a copy of the decoded
 * image, 256 MB of queue at most, a full queue drops the write and the texture is decoded again next time) to one writer thread, which also builds the
 * index at start-up (one directory listing) and keeps the cache under its size cap by removing the files read least recently (a hit touches the file's time).
 * Nothing here runs on the main thread except the once-a-minute report and the settings read.
 *
 * Settings: FSDecodedTextureCache (on/off, live: off means no read and no write, today's path), FSDecodedTextureCacheSizeMB (the cap). RenderCompressTextures
 * decides whether BC7 chains are produced and may be served (off: a BC7 file is a miss, and a raw decode replaces it).
 */

#ifndef RK_DECODEDTEXTURECACHE_H
#define RK_DECODEDTEXTURECACHE_H

#include "llimageworker.h"
#include "lldir.h"
#include "threadpool_fwd.h"     // LL::ThreadPool is an alias there

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <string>

class FSDecodedTextureCache : public RKDecodedImageCache
{
public:
    static FSDecodedTextureCache& instance();

    // Main thread, after the texture cache's initCache: the directory under 'location' (<cache>/decodedcache), the writer thread, the index scan (on the writer).
    // 'purge' (the texture cache was purged or its version changed) removes everything first.
    void init(ELLPath location, bool purge);
    // Main thread, after the image decode pool has shut down (no reader is running then): closes the writer, drops the index.
    void shutdown();

    // RKDecodedImageCache (decode pool threads)
    LLPointer<LLImageRaw> read(const LLUUID& id, S32 discard, bool allow_compressed, bool* checked_raw) override;
    void write(const LLUUID& id, S32 discard, const LLImageRaw* raw, bool checked_raw) override;

    // Main thread, every frame (cheap): reads the settings, logs a report once a minute while anything happened, stamps the texture arrival once a session
    void update();

    // the texture cache removed this texture (corrupt): ours goes too. Any thread.
    void remove(const LLUUID& id);
    // the user cleared the cache (or its version changed): everything goes. Main thread, before or after init.
    void purgeAll();

    bool enabled() const { return mEnabled.load(std::memory_order_relaxed); }

    // <FS> 2026-11-14 (a server bake that shows another texture's image after BC7 at decode): whether a hit of this cache has served this texture id lately (the last 4096 ids; the BakeTex log asks)
    bool recentlyServed(const LLUUID& id) const;

private:
    FSDecodedTextureCache();
    ~FSDecodedTextureCache();

    struct Entry
    {
        S32 mDiscard = 0;       // the finest level in the file
        U8 mFormat = 0;         // 0 raw texels, RKImageBC7::FORMAT_BC7 a BC7 chain
        U64 mSize = 0;          // bytes on disk
        S64 mTime = 0;          // last read or write, seconds (the eviction order)
    };

    std::string fileName(const LLUUID& id, S32 discard, U8 format) const;
    std::string subDir(const LLUUID& id) const;
    void scanIndex();                                   // writer thread, at init
    void writeFile(const LLUUID& id, S32 discard, LLPointer<LLImageRaw> raw, bool checked_raw);     // writer thread
    void convertFile(const LLUUID& id);                 // writer thread: a stored raw file is judged at its finest level and becomes a BC7 chain, or is marked as kept raw
    void markFile(const LLUUID& id);                    // writer thread: sets the checked-raw mark in a stored raw file's header, in place
    void touch(const LLUUID& id);                       // writer thread: the file's time and the index
    void evict();                                       // writer thread, index lock held by the caller
    bool post(std::function<void()> work);              // to the writer; false when closed

    std::string mDir;
    std::unique_ptr<LL::ThreadPool> mWriter;
    std::mutex mIndexMutex;
    std::unordered_map<LLUUID, Entry> mIndex;           // what is on disk (the writer keeps it; readers look up)
    std::unordered_set<LLUUID> mConverting;             // ids with a conversion or mark queued (index lock; one job per file)
    mutable std::mutex mServedMutex;                    // <FS> the ids a hit served lately (recentlyServed)
    std::unordered_set<LLUUID> mServed;
    U64 mTotalBytes = 0;                                // sum of mIndex sizes
    bool mScanned = false;
    bool mInitialised = false;

    std::atomic<bool> mEnabled { false };
    std::atomic<U64> mCapBytes { 4ull << 30 };
    std::atomic<S64> mQueuedBytes { 0 };                // decoded bytes waiting on the writer

    // statistics since the last report
    std::atomic<U32> mHits { 0 }, mHitsBC7 { 0 }, mMissAbsent { 0 }, mMissCoarser { 0 }, mMissFormat { 0 }, mMissBad { 0 }, mWrites { 0 }, mWritesBC7 { 0 }, mDrops { 0 }, mSkips { 0 }, mEvictions { 0 }, mConverted { 0 }, mMarked { 0 };
    std::atomic<U64> mHitBytes { 0 }, mWriteBytes { 0 };
    F64 mLastReport = 0.0;
};

#endif // RK_DECODEDTEXTURECACHE_H
