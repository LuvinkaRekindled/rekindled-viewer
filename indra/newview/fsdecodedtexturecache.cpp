/**
 * @file fsdecodedtexturecache.cpp
 * @brief A disk cache of decoded textures: see the header.
 */

#include "llviewerprecompiledheaders.h"
#include "fsdecodedtexturecache.h"

#include "llimage.h"
#include "llimagebc7.h"
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
const U32 kVersion = 2;
const U32 kMaxLevels = 16;                      // down to 1x1 from 32768
const S64 kQueueCapBytes = 256ll << 20;         // decoded bytes waiting on the writer, at most
const F64 kReportSeconds = 60.0;
const U8 kFormatRaw = 0;

#pragma pack(push, 1)
struct Header
{
    char mMagic[4];
    U32 mVersion;
    U16 mWidth, mHeight;        // of the finest level
    U8 mComponents;
    U8 mLevels;                 // levels in the file, the finest first
    U8 mFinestDiscard;          // the discard level of the finest level
    U8 mFormat;                 // 0: raw texels (components bytes each); LLImageBC7::FORMAT_BC7: BC7 blocks
    U32 mCommentLength;         // the texture comment follows the header, then the levels
    U8 mFlags;                  // bits 0-1: the alpha classification + 1 (0 unknown, 1 no alpha, 2 blend, 3 mask)
    U8 mReserved[3];
};
#pragma pack(pop)

// bytes of level k (k = 0 the finest)
U64 level_bytes(const Header& h, U32 k)
{
    const U32 lw = LLImageBC7::levelWidth(h.mWidth, k), lh = LLImageBC7::levelHeight(h.mHeight, k);
    return h.mFormat == kFormatRaw ? (U64)lw * lh * h.mComponents : LLImageBC7::blockBytes(lw, lh);
}

U64 levels_offset(const Header& h, U32 k)
{
    U64 off = 0;
    for (U32 i = 0; i < k; ++i) off += level_bytes(h, i);
    return off;
}

S64 now_seconds()
{
    return (S64)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

S64 file_time_seconds(const std::filesystem::file_time_type& t)
{
    const auto delta = std::filesystem::file_time_type::clock::now() - t;
    return now_seconds() - (S64)std::chrono::duration_cast<std::chrono::seconds>(delta).count();
}

// std::filesystem paths from and to the viewer's UTF-8 strings (C++20 made u8string() a char8_t string)
std::filesystem::path fs_path(const std::string& utf8) { return std::filesystem::path(ll_convert_string_to_wide(utf8)); }
std::string fs_string(const std::filesystem::path& p) { return ll_convert_wide_to_string(p.wstring()); }

const char* extension_of(U8 format) { return format == kFormatRaw ? ".dec" : ".dc7"; }
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

std::string FSDecodedTextureCache::fileName(const LLUUID& id, S32 discard, U8 format) const
{
    return subDir(id) + gDirUtilp->getDirDelimiter() + id.asString() + "_" + std::to_string(discard) + extension_of(format);
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
    LL_INFOS("DecodedCache") << "decoded texture cache at " << mDir << ", cap " << (mCapBytes.load() >> 20) << " MB, " << (enabled() ? "on" : "off") << ", BC7 at decode "
                             << (LLImageBC7::enabled() ? "on" : "off") << LL_ENDL;
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
    static LLCachedControl<bool> compress(gSavedSettings, "RenderCompressTextures", false);
    mEnabled.store(mInitialised && on(), std::memory_order_relaxed);
    mCapBytes.store((U64)llmax(256u, (U32)cap_mb) << 20, std::memory_order_relaxed);
    LLImageBC7::setEnabled(compress());         // BC7 at decode, live

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
                                         << (enabled() ? "on" : "off") << ", BC7 " << (LLImageBC7::enabled() ? "on" : "off") << ")" << LL_ENDL;
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
    const U32 hits = mHits.exchange(0), hits_bc7 = mHitsBC7.exchange(0), absent = mMissAbsent.exchange(0), coarser = mMissCoarser.exchange(0), format = mMissFormat.exchange(0), bad = mMissBad.exchange(0);
    const U32 writes = mWrites.exchange(0), writes_bc7 = mWritesBC7.exchange(0), drops = mDrops.exchange(0), skips = mSkips.exchange(0), evictions = mEvictions.exchange(0);
    const U64 hit_bytes = mHitBytes.exchange(0), write_bytes = mWriteBytes.exchange(0);
    const LLImageBC7::Stats bc7 = LLImageBC7::takeStats();
    if (hits + absent + coarser + format + bad + writes + drops + evictions + bc7.mCompressed + bc7.mKeptRaw == 0)
    {
        return;
    }
    U64 total = 0; size_t entries = 0;
    {
        std::lock_guard<std::mutex> lock(mIndexMutex);
        total = mTotalBytes;
        entries = mIndex.size();
    }
    LL_INFOS("DecodedCache") << "decoded texture cache, last " << (S32)kReportSeconds << " s: hits " << hits << " (" << hits_bc7 << " BC7; " << (hit_bytes >> 20) << " MB read in place of a decode), misses "
                             << (absent + coarser + format + bad) << " (absent " << absent << ", stored coarser than asked " << coarser << ", stored in the other format " << format << ", unreadable " << bad
                             << "), writes " << writes << " (" << writes_bc7 << " BC7; " << (write_bytes >> 20) << " MB), skipped as already stored " << skips << ", dropped (queue full) " << drops
                             << ", evicted " << evictions << "; BC7 at decode: " << bc7.mCompressed << " compressed, " << bc7.mKeptRaw << " kept raw by the check, " << (bc7.mEncodeMicroseconds / 1000)
                             << " ms of encoding; on disk " << entries << " textures, " << (total >> 20) << " of " << (mCapBytes.load() >> 20) << " MB" << LL_ENDL;
}

void FSDecodedTextureCache::scanIndex()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    const F64 start = LLTimer::getElapsedSeconds();
    std::unordered_map<LLUUID, Entry> index;
    U64 total = 0;
    size_t entries = 0;
    std::vector<std::string> stale;             // a texture with two files: the coarser one goes (the formats tie: the raw one goes)
    std::error_code ec;
    for (char c : std::string("0123456789abcdef"))
    {
        const std::string dir = mDir + gDirUtilp->getDirDelimiter() + c;
        for (const auto& entry : std::filesystem::directory_iterator(fs_path(dir), ec))
        {
            if (!entry.is_regular_file(ec)) continue;
            const std::string name = fs_string(entry.path().filename());
            // <uuid 36>_<discard>.dec | .dc7
            if (name.size() < 36 + 6 || name[36] != '_') continue;
            const std::string ext = name.substr(name.size() - 4);
            U8 format;
            if (ext == ".dec") format = kFormatRaw;
            else if (ext == ".dc7") format = LLImageBC7::FORMAT_BC7;
            else continue;
            LLUUID id;
            if (!id.set(name.substr(0, 36), false)) continue;
            const S32 discard = atoi(name.c_str() + 37);
            Entry e;
            e.mDiscard = discard;
            e.mFormat = format;
            e.mSize = (U64)entry.file_size(ec);
            e.mTime = file_time_seconds(entry.last_write_time(ec));
            auto it = index.find(id);
            if (it != index.end())
            {
                const bool keep_old = it->second.mDiscard < discard || (it->second.mDiscard == discard && it->second.mFormat != kFormatRaw);
                if (keep_old)
                {
                    stale.push_back(fs_string(entry.path()));
                    continue;
                }
                stale.push_back(fileName(id, it->second.mDiscard, it->second.mFormat));
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

LLPointer<LLImageRaw> FSDecodedTextureCache::read(const LLUUID& id, S32 discard, bool allow_compressed)
{
    if (!enabled() || discard < 0)
    {
        return nullptr;
    }
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    S32 stored = -1;
    U8 format = kFormatRaw;
    {
        std::lock_guard<std::mutex> lock(mIndexMutex);
        auto it = mIndex.find(id);
        if (it == mIndex.end())
        {
            ++mMissAbsent;
            return nullptr;
        }
        stored = it->second.mDiscard;
        format = it->second.mFormat;
    }
    if (stored > discard)
    {
        ++mMissCoarser;
        return nullptr;
    }
    if (format != kFormatRaw && (!allow_compressed || !LLImageBC7::enabled()))
    {
        ++mMissFormat;          // a chain is stored but this request wants texels (or compression is off): decoded instead
        return nullptr;
    }
    const std::string path = fileName(id, stored, format);
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
    bool ok = fread(&h, sizeof(h), 1, f) == 1 && memcmp(h.mMagic, kMagic, 4) == 0 && h.mVersion == kVersion && h.mFormat == format && h.mComponents >= 1 && h.mComponents <= 4
              && h.mLevels >= 1 && h.mLevels <= kMaxLevels && h.mFinestDiscard == (U8)stored && h.mCommentLength < 65536;
    const S32 k = discard - stored;
    ok = ok && k < (S32)h.mLevels;
    if (ok && h.mCommentLength)
    {
        comment.resize(h.mCommentLength);
        ok = fread(&comment[0], 1, h.mCommentLength, f) == h.mCommentLength;
    }
    if (ok && format == kFormatRaw)
    {
        const U64 offset = sizeof(Header) + h.mCommentLength + levels_offset(h, (U32)k);
        const U64 bytes = level_bytes(h, (U32)k);
        U8* data = (U8*)ll_aligned_malloc_16((size_t)bytes);
        ok = data && _fseeki64(f, (S64)offset, SEEK_SET) == 0 && fread(data, 1, (size_t)bytes, f) == bytes;
        if (ok)
        {
            result = new LLImageRaw(data, (U16)LLImageBC7::levelWidth(h.mWidth, k), (U16)LLImageBC7::levelHeight(h.mHeight, k), (S8)h.mComponents, true /*take ownership*/);
            result->mComment = comment;
            mHitBytes += bytes;
        }
        else if (data)
        {
            ll_aligned_free_16(data);
        }
    }
    else if (ok)
    {
        // the BC7 chain from level k down: read in one go (contiguous in the file, the finest first), split, assembled in the GPU's order
        const U64 offset = sizeof(Header) + h.mCommentLength + levels_offset(h, (U32)k);
        const U64 bytes = levels_offset(h, h.mLevels) - levels_offset(h, (U32)k);
        std::vector<U8> buf((size_t)bytes);
        ok = _fseeki64(f, (S64)offset, SEEK_SET) == 0 && fread(buf.data(), 1, (size_t)bytes, f) == bytes;
        if (ok)
        {
            std::vector<std::vector<U8>> levels(h.mLevels - k);
            const U8* p = buf.data();
            for (U32 i = (U32)k; i < h.mLevels; ++i)
            {
                const U64 n = level_bytes(h, i);
                levels[i - k].assign(p, p + n);
                p += n;
            }
            const S8 alpha = (h.mFlags & 3) == 0 ? -1 : (S8)((h.mFlags & 3) - 2);        // 1 no alpha -> -1, 2 blend -> 0, 3 mask -> 1
            result = LLImageBC7::assemble(levels, LLImageBC7::levelWidth(h.mWidth, k), LLImageBC7::levelHeight(h.mHeight, k), h.mComponents, alpha);
            ok = result.notNull();
            if (ok)
            {
                result->mComment = comment;
                mHitBytes += bytes;
                ++mHitsBC7;
            }
        }
    }
    fclose(f);
    if (!ok)
    {
        ++mMissBad;
        // a file that does not read (an older version, a corrupt one) is removed, so the next decode writes a fresh one
        remove(id);
        return nullptr;
    }
    ++mHits;
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
    const U8 format = raw->isCompressed() ? raw->getCompressedFormat() : kFormatRaw;
    {
        std::lock_guard<std::mutex> lock(mIndexMutex);
        auto it = mIndex.find(id);
        if (it != mIndex.end() && it->second.mDiscard < discard)
        {
            ++mSkips;
            return;         // a finer level is stored, whatever its format: a coarser one never replaces it
        }
        if (it != mIndex.end() && it->second.mDiscard == discard)
        {
            // the same level is stored. The format: a stored chain stays when compression is on (a raw decode for a texture that needs texels is not a
            // reason to drop it); a stored raw stays when this is raw; a chain replaces a raw of the same or a coarser level, and a raw replaces a chain once compression is off
            const bool stored_bc7 = it->second.mFormat != kFormatRaw;
            const bool keep = (stored_bc7 && (format != kFormatRaw || LLImageBC7::enabled())) || (!stored_bc7 && format == kFormatRaw);     // (a strictly finer raw never reaches here: it always writes)
            if (keep)
            {
                ++mSkips;
                return;
            }
        }
    }
    const S64 bytes = (S64)raw->getDataSize();
    if (mQueuedBytes.load(std::memory_order_relaxed) + bytes > kQueueCapBytes)
    {
        ++mDrops;
        return;
    }
    // a copy: the decoded image goes on to the viewer, which may scale it (icons) while the writer still reads it
    LLPointer<LLImageRaw> copy;
    if (raw->isCompressed())
    {
        U8* data = (U8*)ll_aligned_malloc_16(raw->getDataSize());
        if (!data) { ++mDrops; return; }
        memcpy(data, raw->getData(), raw->getDataSize());
        copy = new LLImageRaw();
        copy->setCompressedChain(data, raw->getDataSize(), raw->getWidth(), raw->getHeight(), raw->getComponents(), raw->getCompressedFormat(), raw->getLevelCount(), raw->getAlphaIsMask());
    }
    else
    {
        copy = new LLImageRaw(raw->getWidth(), raw->getHeight(), raw->getComponents());
        if (!copy->getData()) { ++mDrops; return; }
        memcpy(copy->getData(), raw->getData(), raw->getDataSize());
    }
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
    const U8 format = raw->isCompressed() ? raw->getCompressedFormat() : kFormatRaw;
    // the levels, the finest first
    std::vector<std::vector<U8>> levels;
    if (raw->isCompressed())
    {
        if (!LLImageBC7::splitLevels(raw, levels)) { ++mDrops; return; }
    }
    else
    {
        LLImageBC7::buildChain(raw->getData(), w, h, c, levels);
    }
    if (levels.empty() || levels.size() > kMaxLevels) { ++mDrops; return; }

    Header hd = {};
    memcpy(hd.mMagic, kMagic, 4);
    hd.mVersion = kVersion;
    hd.mWidth = (U16)w;
    hd.mHeight = (U16)h;
    hd.mComponents = (U8)c;
    hd.mLevels = (U8)levels.size();
    hd.mFinestDiscard = (U8)discard;
    hd.mFormat = format;
    const S8 alpha = raw->getAlphaIsMask();
    hd.mFlags = (U8)(alpha < 0 ? (raw->isCompressed() ? 1 : 0) : (alpha == 0 ? 2 : 3));
    const std::string& comment = raw->mComment;
    hd.mCommentLength = (U32)llmin((size_t)65535, comment.size());

    const std::string path = fileName(id, discard, format);
    const std::string tmp = path + ".tmp";
    LLFILE* f = LLFile::fopen(tmp, "wb");
    if (!f)
    {
        ++mDrops;
        return;
    }
    bool ok = fwrite(&hd, sizeof(hd), 1, f) == 1 && (hd.mCommentLength == 0 || fwrite(comment.data(), 1, hd.mCommentLength, f) == hd.mCommentLength);
    U64 size = sizeof(hd) + hd.mCommentLength;
    for (size_t k = 0; ok && k < levels.size(); ++k)
    {
        ok = fwrite(levels[k].data(), 1, levels[k].size(), f) == levels[k].size();
        size += levels[k].size();
    }
    fclose(f);
    if (!ok)
    {
        LLFile::remove(tmp, ENOENT);
        ++mDrops;
        return;
    }
    // the file first (a reader that finds the index entry must find the file), then the index, then the file it replaces
    LLFile::remove(path, ENOENT);
    if (LLFile::rename(tmp, path) != 0)
    {
        LLFile::remove(tmp, ENOENT);
        ++mDrops;
        return;
    }
    S32 old_discard = -1;
    U8 old_format = kFormatRaw;
    {
        std::lock_guard<std::mutex> lock(mIndexMutex);
        auto it = mIndex.find(id);
        if (it != mIndex.end())
        {
            old_discard = it->second.mDiscard;
            old_format = it->second.mFormat;
            mTotalBytes -= it->second.mSize;
        }
        Entry e;
        e.mDiscard = discard;
        e.mFormat = format;
        e.mSize = size;
        e.mTime = now_seconds();
        mIndex[id] = e;
        mTotalBytes += size;
    }
    if (old_discard >= 0 && (old_discard != discard || old_format != format))
    {
        LLFile::remove(fileName(id, old_discard, old_format), ENOENT);      // a reader that looked the old one up a moment ago finds no file: a plain miss
    }
    ++mWrites;
    if (format != kFormatRaw) ++mWritesBC7;
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
    U8 format = kFormatRaw;
    {
        std::lock_guard<std::mutex> lock(mIndexMutex);
        auto it = mIndex.find(id);
        if (it == mIndex.end()) return;
        it->second.mTime = now_seconds();
        discard = it->second.mDiscard;
        format = it->second.mFormat;
    }
    std::error_code ec;
    std::filesystem::last_write_time(fs_path(fileName(id, discard, format)), std::filesystem::file_time_type::clock::now(), ec);
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
        LLFile::remove(fileName(oldest->first, oldest->second.mDiscard, oldest->second.mFormat), ENOENT);
        mTotalBytes -= oldest->second.mSize;
        mIndex.erase(oldest);
        ++mEvictions;
    }
}

void FSDecodedTextureCache::remove(const LLUUID& id)
{
    S32 discard = -1;
    U8 format = kFormatRaw;
    {
        std::lock_guard<std::mutex> lock(mIndexMutex);
        auto it = mIndex.find(id);
        if (it == mIndex.end()) return;
        discard = it->second.mDiscard;
        format = it->second.mFormat;
        mTotalBytes -= it->second.mSize;
        mIndex.erase(it);
    }
    const std::string path = fileName(id, discard, format);
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
