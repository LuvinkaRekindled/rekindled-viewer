/**
 * @file fsdecodedtexturecache.cpp
 * @brief A disk cache of decoded textures: see the header.
 */

#include "llviewerprecompiledheaders.h"
#include "fsdecodedtexturecache.h"

#include "llimage.h"
#include "llfile.h"
#include "llstring.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "llappviewer.h"
#include "llstartup.h"
#include "lltexturefetch.h"
#include "threadpool.h"

#include <filesystem>
#include <chrono>
#include <vector>

namespace
{
const char* kDirName = "decodedcache";
const char kMagic[4] = { 'F', 'S', 'D', 'C' };
const U32 kVersion = 1;
const U32 kMaxLevels = 8;                       // the finest level and seven below it (the viewer never asks more than MAX_DISCARD_LEVEL 5 beyond)
const S64 kQueueCapBytes = 256ll << 20;         // decoded bytes waiting on the writer, at most
const F64 kReportSeconds = 60.0;

#pragma pack(push, 1)
struct Header
{
    char mMagic[4];
    U32 mVersion;
    U16 mWidth, mHeight;        // of the finest level
    U8 mComponents;
    U8 mLevels;                 // levels in the file, the finest first
    U8 mFinestDiscard;          // the discard level of the finest level
    U8 mFormat;                 // 0: raw texels (components bytes each)
    U32 mCommentLength;         // the texture comment follows the header, then the levels
};
#pragma pack(pop)

// bytes of level k (k = 0 the finest), raw texels
U64 level_bytes(U32 w, U32 h, U32 c, U32 k)
{
    const U32 lw = llmax(1u, w >> k), lh = llmax(1u, h >> k);
    return (U64)lw * lh * c;
}

U64 levels_offset(U32 w, U32 h, U32 c, U32 k)
{
    U64 off = 0;
    for (U32 i = 0; i < k; ++i) off += level_bytes(w, h, c, i);
    return off;
}

U32 levels_for(U32 w, U32 h)
{
    U32 n = 1;
    while (n < kMaxLevels && ((w >> n) >= 1 || (h >> n) >= 1) && (w >> (n - 1)) > 1 && (h >> (n - 1)) > 1)
    {
        ++n;
    }
    return n;
}

// the next level of a chain: a 2x2 box filter (what the GPU's mip generation does); odd sizes drop the last row / column, as a power of two never has
void box_down(const U8* src, U32 sw, U32 sh, U32 c, U8* dst)
{
    const U32 dw = llmax(1u, sw >> 1), dh = llmax(1u, sh >> 1);
    const U32 sx = sw > 1 ? 1 : 0, sy = sh > 1 ? 1 : 0;        // a 1-wide source has no second column
    for (U32 y = 0; y < dh; ++y)
    {
        const U8* r0 = src + (U64)(2 * y) * sw * c;
        const U8* r1 = r0 + (U64)sy * sw * c;
        U8* d = dst + (U64)y * dw * c;
        for (U32 x = 0; x < dw; ++x)
        {
            const U8* p00 = r0 + (U64)(2 * x) * c;
            const U8* p01 = p00 + (U64)sx * c;
            const U8* p10 = r1 + (U64)(2 * x) * c;
            const U8* p11 = p10 + (U64)sx * c;
            for (U32 i = 0; i < c; ++i)
            {
                d[(U64)x * c + i] = (U8)(((U32)p00[i] + p01[i] + p10[i] + p11[i] + 2) >> 2);
            }
        }
    }
}

// std::filesystem paths from and to the viewer's UTF-8 strings (C++20 made u8string() a char8_t string)
std::filesystem::path fs_path(const std::string& utf8) { return std::filesystem::path(ll_convert_string_to_wide(utf8)); }
std::string fs_string(const std::filesystem::path& p) { return ll_convert_wide_to_string(p.wstring()); }

S64 now_seconds()
{
    return (S64)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

S64 file_time_seconds(const std::filesystem::file_time_type& t)
{
    // file_time_type's clock is not the system clock before C++20 guarantees; the difference to now is what matters for the eviction order
    const auto delta = std::filesystem::file_time_type::clock::now() - t;
    return now_seconds() - (S64)std::chrono::duration_cast<std::chrono::seconds>(delta).count();
}
}

FSDecodedTextureCache& FSDecodedTextureCache::instance()
{
    static FSDecodedTextureCache s;
    return s;
}

FSDecodedTextureCache::FSDecodedTextureCache() {}
FSDecodedTextureCache::~FSDecodedTextureCache() {}

std::string FSDecodedTextureCache::subDir(const LLUUID& id) const
{
    const std::string s = id.asString();
    return mDir + gDirUtilp->getDirDelimiter() + s.substr(0, 1);
}

std::string FSDecodedTextureCache::fileName(const LLUUID& id, S32 discard) const
{
    return subDir(id) + gDirUtilp->getDirDelimiter() + id.asString() + "_" + std::to_string(discard) + ".dec";
}

void FSDecodedTextureCache::init(ELLPath location, bool purge)
{
    mDir = gDirUtilp->getExpandedFilename(location, kDirName);
    if (purge)
    {
        purgeAll();
    }
    LLFile::mkdir(mDir);
    for (char c : std::string("0123456789abcdef"))
    {
        LLFile::mkdir(mDir + gDirUtilp->getDirDelimiter() + c);
    }
    mWriter = std::make_unique<LL::ThreadPool>("DecodedTextureCache", 1, 1024 * 64);
    mWriter->start();
    mInitialised = true;
    update();           // the settings
    post([this]() { scanIndex(); });
    LLImageDecodeThread::setDecodedCache(this);
    LL_INFOS("DecodedCache") << "decoded texture cache at " << mDir << ", cap " << (mCapBytes.load() >> 20) << " MB, " << (enabled() ? "on" : "off") << LL_ENDL;
}

void FSDecodedTextureCache::shutdown()
{
    LLImageDecodeThread::setDecodedCache(nullptr);
    if (mWriter)
    {
        mWriter->close();       // runs what is queued, then joins
        mWriter.reset();
    }
    std::lock_guard<std::mutex> lock(mIndexMutex);
    mIndex.clear();
    mTotalBytes = 0;
    mInitialised = false;
}

bool FSDecodedTextureCache::post(std::function<void()> work)
{
    if (!mWriter)
    {
        return false;
    }
    return mWriter->getQueue().post(std::move(work));
}

void FSDecodedTextureCache::update()
{
    static LLCachedControl<bool> on(gSavedSettings, "FSDecodedTextureCache", true);
    static LLCachedControl<U32> cap_mb(gSavedSettings, "FSDecodedTextureCacheSizeMB", 4096);
    mEnabled.store(mInitialised && on(), std::memory_order_relaxed);
    mCapBytes.store((U64)llmax(256u, (U32)cap_mb) << 20, std::memory_order_relaxed);

    const F64 now = LLTimer::getElapsedSeconds();

    // the arrival stamp (plan 2.1 gate G1): once per session, the time from the world appearing (STATE_STARTED) to the texture fetch and decode queues staying empty for 3 s
    static F64 world_time = -1.0, quiet_since = -1.0;
    static bool settled = false;
    if (!settled && LLStartUp::getStartupState() >= STATE_STARTED)
    {
        if (world_time < 0.0)
        {
            world_time = now;
        }
        const S32 requests = LLAppViewer::getTextureFetch() ? LLAppViewer::getTextureFetch()->getNumRequests() : 0;
        const size_t pending = LLAppViewer::getImageDecodeThread() ? LLAppViewer::getImageDecodeThread()->getPending() : 0;
        if (requests == 0 && pending == 0)
        {
            if (quiet_since < 0.0)
            {
                quiet_since = now;
            }
            else if (now - quiet_since >= 3.0)
            {
                settled = true;
                LL_INFOS("DecodedCache") << "textures settled " << (S32)((quiet_since - world_time) * 1000.0) << " ms after the world appeared (fetch and decode queues empty for 3 s; decoded cache "
                                         << (enabled() ? "on" : "off") << ")" << LL_ENDL;
            }
        }
        else
        {
            quiet_since = -1.0;
        }
    }

    if (now - mLastReport < kReportSeconds)
    {
        return;
    }
    mLastReport = now;
    const U32 hits = mHits.exchange(0), absent = mMissAbsent.exchange(0), coarser = mMissCoarser.exchange(0), bad = mMissBad.exchange(0);
    const U32 writes = mWrites.exchange(0), drops = mDrops.exchange(0), skips = mSkips.exchange(0), evictions = mEvictions.exchange(0);
    const U64 hit_bytes = mHitBytes.exchange(0), write_bytes = mWriteBytes.exchange(0);
    if (hits + absent + coarser + bad + writes + drops + evictions == 0)
    {
        return;
    }
    U64 total = 0; size_t entries = 0;
    {
        std::lock_guard<std::mutex> lock(mIndexMutex);
        total = mTotalBytes;
        entries = mIndex.size();
    }
    LL_INFOS("DecodedCache") << "decoded texture cache, last " << (S32)kReportSeconds << " s: hits " << hits << " (" << (hit_bytes >> 20) << " MB read in place of a decode), misses " << (absent + coarser + bad)
                             << " (absent " << absent << ", stored coarser than asked " << coarser << ", unreadable " << bad << "), writes " << writes << " (" << (write_bytes >> 20) << " MB), skipped as already stored " << skips
                             << ", dropped (queue full) " << drops << ", evicted " << evictions << "; on disk " << entries << " textures, " << (total >> 20) << " of " << (mCapBytes.load() >> 20) << " MB" << LL_ENDL;
}

void FSDecodedTextureCache::scanIndex()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    const F64 start = LLTimer::getElapsedSeconds();
    std::unordered_map<LLUUID, Entry> index;
    U64 total = 0;
    size_t entries = 0;
    std::vector<std::string> stale;             // a texture with two files: the coarser one goes
    std::error_code ec;
    for (char c : std::string("0123456789abcdef"))
    {
        const std::string dir = mDir + gDirUtilp->getDirDelimiter() + c;
        for (const auto& entry : std::filesystem::directory_iterator(fs_path(dir), ec))
        {
            if (!entry.is_regular_file(ec)) continue;
            const std::string name = fs_string(entry.path().filename());
            // <uuid 36>_<discard>.dec
            if (name.size() < 36 + 6 || name[36] != '_' || name.compare(name.size() - 4, 4, ".dec") != 0) continue;
            LLUUID id;
            if (!id.set(name.substr(0, 36), false)) continue;
            const S32 discard = atoi(name.c_str() + 37);
            Entry e;
            e.mDiscard = discard;
            e.mSize = (U64)entry.file_size(ec);
            e.mTime = file_time_seconds(entry.last_write_time(ec));
            auto it = index.find(id);
            if (it != index.end())
            {
                if (it->second.mDiscard <= discard)
                {
                    stale.push_back(fs_string(entry.path()));
                    continue;
                }
                stale.push_back(fileName(id, it->second.mDiscard));
                total -= it->second.mSize;
            }
            total += e.mSize;
            index[id] = e;
        }
    }
    for (const std::string& f : stale)
    {
        LLFile::remove(f, ENOENT);
    }
    {
        std::lock_guard<std::mutex> lock(mIndexMutex);
        // writes that landed while the scan ran are already in mIndex: keep them
        for (auto& kv : mIndex)
        {
            auto it = index.find(kv.first);
            if (it != index.end()) total -= it->second.mSize;
            index[kv.first] = kv.second;
            total += kv.second.mSize;
        }
        mIndex.swap(index);
        mTotalBytes = total;
        mScanned = true;
        evict();
        entries = mIndex.size();
    }
    LL_INFOS("DecodedCache") << "decoded texture cache index: " << entries << " textures, " << (total >> 20) << " MB, " << stale.size() << " stale files removed, scanned in "
                             << (S32)((LLTimer::getElapsedSeconds() - start) * 1000.0) << " ms" << LL_ENDL;
}

LLPointer<LLImageRaw> FSDecodedTextureCache::read(const LLUUID& id, S32 discard)
{
    if (!enabled() || discard < 0)
    {
        return nullptr;
    }
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    S32 stored = -1;
    {
        std::lock_guard<std::mutex> lock(mIndexMutex);
        auto it = mIndex.find(id);
        if (it == mIndex.end())
        {
            ++mMissAbsent;
            return nullptr;
        }
        stored = it->second.mDiscard;
    }
    if (stored > discard)
    {
        ++mMissCoarser;
        return nullptr;
    }
    const std::string path = fileName(id, stored);
    LLFILE* f = LLFile::fopen(path, "rb");
    if (!f)
    {
        // not there (a write of a finer level may just have replaced it, or an eviction): a miss, nothing to remove
        ++mMissBad;
        return nullptr;
    }
    LLPointer<LLImageRaw> result;
    Header h = {};
    std::string comment;
    bool ok = fread(&h, sizeof(h), 1, f) == 1 && memcmp(h.mMagic, kMagic, 4) == 0 && h.mVersion == kVersion && h.mFormat == 0 && h.mComponents >= 1 && h.mComponents <= 4
              && h.mLevels >= 1 && h.mLevels <= kMaxLevels && h.mFinestDiscard == (U8)stored && h.mCommentLength < 65536;
    const S32 k = discard - stored;
    ok = ok && k < (S32)h.mLevels;
    if (ok && h.mCommentLength)
    {
        comment.resize(h.mCommentLength);
        ok = fread(&comment[0], 1, h.mCommentLength, f) == h.mCommentLength;
    }
    if (ok)
    {
        const U64 offset = sizeof(Header) + h.mCommentLength + levels_offset(h.mWidth, h.mHeight, h.mComponents, (U32)k);
        const U64 bytes = level_bytes(h.mWidth, h.mHeight, h.mComponents, (U32)k);
        U8* data = (U8*)ll_aligned_malloc_16((size_t)bytes);
        ok = data && _fseeki64(f, (S64)offset, SEEK_SET) == 0 && fread(data, 1, (size_t)bytes, f) == bytes;
        if (ok)
        {
            result = new LLImageRaw(data, (U16)llmax(1u, (U32)h.mWidth >> k), (U16)llmax(1u, (U32)h.mHeight >> k), (S8)h.mComponents, true /*take ownership*/);
            result->mComment = comment;
            ++mHits;
            mHitBytes += bytes;
        }
        else if (data)
        {
            ll_aligned_free_16(data);
        }
    }
    fclose(f);
    if (!ok)
    {
        ++mMissBad;
        // a file that does not read is removed, so the next decode writes a fresh one
        remove(id);
        return nullptr;
    }
    post([this, id]() { touch(id); });
    return result;
}

void FSDecodedTextureCache::write(const LLUUID& id, S32 discard, const LLImageRaw* raw)
{
    if (!enabled() || !raw || discard < 0 || discard > 255 || !raw->getData() || raw->getWidth() == 0 || raw->getHeight() == 0)
    {
        return;
    }
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    {
        std::lock_guard<std::mutex> lock(mIndexMutex);
        auto it = mIndex.find(id);
        if (it != mIndex.end() && it->second.mDiscard <= discard)
        {
            ++mSkips;
            return;         // what is stored is this level or a finer one
        }
    }
    const S64 bytes = (S64)raw->getDataSize();
    if (mQueuedBytes.load(std::memory_order_relaxed) + bytes > kQueueCapBytes)
    {
        ++mDrops;
        return;
    }
    // a copy: the decoded image goes on to the viewer, which may scale it (icons) while the writer still reads it
    LLPointer<LLImageRaw> copy = new LLImageRaw(raw->getWidth(), raw->getHeight(), raw->getComponents());
    if (!copy->getData())
    {
        ++mDrops;
        return;
    }
    memcpy(copy->getData(), raw->getData(), raw->getDataSize());
    copy->mComment = raw->mComment;
    mQueuedBytes += bytes;
    if (!post([this, id, discard, copy]() { writeFile(id, discard, copy); mQueuedBytes -= copy->getDataSize(); }))
    {
        mQueuedBytes -= bytes;
        ++mDrops;
    }
}

void FSDecodedTextureCache::writeFile(const LLUUID& id, S32 discard, LLPointer<LLImageRaw> raw)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    const U32 w = raw->getWidth(), h = raw->getHeight(), c = raw->getComponents();
    const U32 levels = levels_for(w, h);
    Header hd = {};
    memcpy(hd.mMagic, kMagic, 4);
    hd.mVersion = kVersion;
    hd.mWidth = (U16)w;
    hd.mHeight = (U16)h;
    hd.mComponents = (U8)c;
    hd.mLevels = (U8)levels;
    hd.mFinestDiscard = (U8)discard;
    hd.mFormat = 0;
    const std::string& comment = raw->mComment;
    hd.mCommentLength = (U32)llmin((size_t)65535, comment.size());

    const std::string path = fileName(id, discard);
    const std::string tmp = path + ".tmp";
    LLFILE* f = LLFile::fopen(tmp, "wb");
    if (!f)
    {
        ++mDrops;
        return;
    }
    bool ok = fwrite(&hd, sizeof(hd), 1, f) == 1 && (hd.mCommentLength == 0 || fwrite(comment.data(), 1, hd.mCommentLength, f) == hd.mCommentLength);
    ok = ok && fwrite(raw->getData(), 1, raw->getDataSize(), f) == (size_t)raw->getDataSize();
    // the chain below the finest level
    std::vector<U8> cur(raw->getData(), raw->getData() + raw->getDataSize()), next;
    U32 lw = w, lh = h;
    for (U32 k = 1; ok && k < levels; ++k)
    {
        const U32 nw = llmax(1u, lw >> 1), nh = llmax(1u, lh >> 1);
        next.resize((size_t)nw * nh * c);
        box_down(cur.data(), lw, lh, c, next.data());
        ok = fwrite(next.data(), 1, next.size(), f) == next.size();
        cur.swap(next);
        lw = nw;
        lh = nh;
    }
    const U64 size = sizeof(hd) + hd.mCommentLength + levels_offset(w, h, c, levels);
    fclose(f);
    if (!ok)
    {
        LLFile::remove(tmp, ENOENT);
        ++mDrops;
        return;
    }
    // the file first (a reader that finds the index entry must find the file), then the index, then the coarser file it replaces
    LLFile::remove(path, ENOENT);
    if (LLFile::rename(tmp, path) != 0)
    {
        LLFile::remove(tmp, ENOENT);
        ++mDrops;
        return;
    }
    S32 old_discard = -1;
    {
        std::lock_guard<std::mutex> lock(mIndexMutex);
        auto it = mIndex.find(id);
        if (it != mIndex.end())
        {
            old_discard = it->second.mDiscard;
            mTotalBytes -= it->second.mSize;
        }
        Entry e;
        e.mDiscard = discard;
        e.mSize = size;
        e.mTime = now_seconds();
        mIndex[id] = e;
        mTotalBytes += size;
    }
    if (old_discard >= 0 && old_discard != discard)
    {
        LLFile::remove(fileName(id, old_discard), ENOENT);      // a reader that looked the old level up a moment ago finds no file: a plain miss
    }
    ++mWrites;
    mWriteBytes += size;
    std::lock_guard<std::mutex> lock(mIndexMutex);
    if (mScanned)
    {
        evict();
    }
}

void FSDecodedTextureCache::touch(const LLUUID& id)
{
    S32 discard = -1;
    {
        std::lock_guard<std::mutex> lock(mIndexMutex);
        auto it = mIndex.find(id);
        if (it == mIndex.end()) return;
        it->second.mTime = now_seconds();
        discard = it->second.mDiscard;
    }
    std::error_code ec;
    std::filesystem::last_write_time(fs_path(fileName(id, discard)), std::filesystem::file_time_type::clock::now(), ec);
}

// index lock held by the caller
void FSDecodedTextureCache::evict()
{
    const U64 cap = mCapBytes.load(std::memory_order_relaxed);
    while (mTotalBytes > cap && !mIndex.empty())
    {
        auto oldest = mIndex.begin();
        for (auto it = mIndex.begin(); it != mIndex.end(); ++it)
        {
            if (it->second.mTime < oldest->second.mTime) oldest = it;
        }
        LLFile::remove(fileName(oldest->first, oldest->second.mDiscard), ENOENT);
        mTotalBytes -= oldest->second.mSize;
        mIndex.erase(oldest);
        ++mEvictions;
    }
}

void FSDecodedTextureCache::remove(const LLUUID& id)
{
    S32 discard = -1;
    {
        std::lock_guard<std::mutex> lock(mIndexMutex);
        auto it = mIndex.find(id);
        if (it == mIndex.end()) return;
        discard = it->second.mDiscard;
        mTotalBytes -= it->second.mSize;
        mIndex.erase(it);
    }
    const std::string path = fileName(id, discard);
    if (!post([path]() { LLFile::remove(path, ENOENT); }))
    {
        LLFile::remove(path, ENOENT);
    }
}

void FSDecodedTextureCache::purgeAll()
{
    if (mDir.empty())
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mIndexMutex);
        mIndex.clear();
        mTotalBytes = 0;
    }
    std::error_code ec;
    const auto removed = std::filesystem::remove_all(fs_path(mDir), ec);
    LL_INFOS("DecodedCache") << "decoded texture cache purged: " << removed << " files and directories" << (ec ? " (with errors)" : "") << LL_ENDL;
    LLFile::mkdir(mDir);
    for (char c : std::string("0123456789abcdef"))
    {
        LLFile::mkdir(mDir + gDirUtilp->getDirDelimiter() + c);
    }
}
