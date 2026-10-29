/**
 * @file llimageworker.h
 * @brief Object for managing images and their textures.
 *
 * $LicenseInfo:firstyear=2000&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef LL_LLIMAGEWORKER_H
#define LL_LLIMAGEWORKER_H

#include "llimage.h"
#include "llpointer.h"
#include "lluuid.h"
#include "threadpool_fwd.h"

// <FS> A cache of decoded images the decode pool asks before decoding (fsdecodedtexturecache.h implements it; doc/vulkan/PLAN-aurora-ideas.md section 2).
// read: the image of 'id' at exactly 'discard', or null. write: the decode just produced this image at this discard level (the cache copies what it keeps). Both run on the decode
// pool's threads and must not block for long.
class LLDecodedImageCache
{
public:
    virtual ~LLDecodedImageCache() {}
    // allow_compressed: a compressed (BC7 chain) image may be returned; false: only texels (the texture needs raw pixels).
    // checked_raw (out, may be null): the stored texels were already judged by the BC7 check and kept raw, so the check need not run again on this hit
    virtual LLPointer<LLImageRaw> read(const LLUUID& id, S32 discard, bool allow_compressed, bool* checked_raw) = 0;
    // checked_raw: these texels were judged by the BC7 check and kept raw (stored with the file, so a later hit skips the check)
    virtual void write(const LLUUID& id, S32 discard, const LLImageRaw* raw, bool checked_raw) = 0;
};

class LLImageDecodeThread
{
public:
    class Responder : public LLThreadSafeRefCount
    {
    protected:
        virtual ~Responder();
    public:
        virtual void completed(bool success, const std::string& error_message, LLImageRaw* raw, LLImageRaw* aux, U32 request_id) = 0;
    };

public:
    LLImageDecodeThread(bool threaded = true);
    virtual ~LLImageDecodeThread();

    // meant to resemble LLQueuedThread::handle_t
    typedef U32 handle_t;
    // <FS> cache_id: the texture asset id, when the decoded image may come from / go to the decoded cache (null: never, as for local files and bakes)
    handle_t decodeImage(const LLPointer<LLImageFormatted>& image,
                         S32 discard, bool needs_aux,
                         const LLPointer<Responder>& responder,
                         const LLUUID& cache_id = LLUUID::null,
                         bool allow_compressed = false);     // <FS> BC7 at decode may replace the texels (RenderCompressTextures, llimagebc7.h) and the cache may answer with a chain
    // <FS> the decoded cache the pool asks (null: none); set before any decode runs and cleared after the pool has shut down
    static void setDecodedCache(LLDecodedImageCache* cache);
    static LLDecodedImageCache* getDecodedCache();
    size_t getPending();
    size_t update(F32 max_time_ms);
    S32 getTotalDecodeCount() { return mDecodeCount; }
    void shutdown();

private:
    // As of SL-17483, LLImageDecodeThread is no longer itself an
    // LLQueuedThread - instead this is the API by which we submit work to the
    // "ImageDecode" ThreadPool.
    std::unique_ptr<LL::ThreadPool> mThreadPool;
    LLAtomicU32 mDecodeCount;
};

#endif
