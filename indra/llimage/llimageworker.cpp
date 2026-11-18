/**
 * @file llimageworker.cpp
 * @brief Base class for images.
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
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

#include "linden_common.h"

#include "llimageworker.h"
#include "rkimagebc7.h"
#include "llimagedxt.h"
#include "threadpool.h"

/*--------------------------------------------------------------------------*/
class ImageRequest
{
public:
    ImageRequest(const LLPointer<LLImageFormatted>& image,
                 S32 discard,
                 bool needs_aux,
                 const LLPointer<LLImageDecodeThread::Responder>& responder,
                 U32 request_id,
                 const LLUUID& cache_id,
                 bool allow_compressed);
    virtual ~ImageRequest();

    /*virtual*/ bool processRequest();
    /*virtual*/ void finishRequest(bool completed);

private:
    // LLPointers stored in ImageRequest MUST be LLPointer instances rather
    // than references: we need to increment the refcount when storing these.
    // input
    LLPointer<LLImageFormatted> mFormattedImage;
    S32 mDiscardLevel;
    U32 mRequestId;
    bool mNeedsAux;
    LLUUID mCacheID;        // <FS> null: the decoded cache is not asked
    bool mAllowCompressed;  // <FS> the result may be a BC7 chain
    // output
    LLPointer<LLImageRaw> mDecodedImageRaw;
    LLPointer<LLImageRaw> mDecodedImageAux;
    bool mDecodedRaw;
    bool mDecodedAux;
    LLPointer<LLImageDecodeThread::Responder> mResponder;
    std::string mErrorString;};


//----------------------------------------------------------------------------

// MAIN THREAD
LLImageDecodeThread::LLImageDecodeThread(bool /*threaded*/)
    : mDecodeCount(0)
{
    mThreadPool = std::make_unique<LL::ThreadPool>("ImageDecode", 8);
    mThreadPool->start();
}

//virtual
LLImageDecodeThread::~LLImageDecodeThread()
{}

// MAIN THREAD
// virtual
size_t LLImageDecodeThread::update(F32 max_time_ms)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    return getPending();
}

size_t LLImageDecodeThread::getPending()
{
    return mThreadPool->getQueue().size();
}

// <FS> the decoded cache (rkdecodedtexturecache.cpp)
static RKDecodedImageCache* sDecodedCache = nullptr;
void LLImageDecodeThread::setDecodedCache(RKDecodedImageCache* cache) { sDecodedCache = cache; }
RKDecodedImageCache* LLImageDecodeThread::getDecodedCache() { return sDecodedCache; }

LLImageDecodeThread::handle_t LLImageDecodeThread::decodeImage(
    const LLPointer<LLImageFormatted>& image,
    S32 discard,
    bool needs_aux,
    const LLPointer<LLImageDecodeThread::Responder>& responder,
    const LLUUID& cache_id,
    bool allow_compressed)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    U32 decode_id = ++mDecodeCount;
    if (decode_id == 0)
        decode_id = ++mDecodeCount;

    // Instantiate the ImageRequest right in the lambda, why not?
    bool posted = mThreadPool->getQueue().post(
        [req = ImageRequest(image, discard, needs_aux, responder, decode_id, cache_id, allow_compressed)]
        () mutable
        {
            auto done = req.processRequest();
            req.finishRequest(done);
        });
    if (! posted)
    {
        LL_DEBUGS() << "Tried to start decoding on shutdown" << LL_ENDL;
        return 0;
    }

    return decode_id;
}

void LLImageDecodeThread::shutdown()
{
    mThreadPool->close();
}

LLImageDecodeThread::Responder::~Responder()
{
}

//----------------------------------------------------------------------------

ImageRequest::ImageRequest(const LLPointer<LLImageFormatted>& image,
                           S32 discard,
                           bool needs_aux,
                           const LLPointer<LLImageDecodeThread::Responder>& responder,
                           U32 request_id,
                           const LLUUID& cache_id,
                           bool allow_compressed)
    : mFormattedImage(image),
      mDiscardLevel(discard),
      mNeedsAux(needs_aux),
      mDecodedRaw(false),
      mDecodedAux(false),
      mResponder(responder),
      mRequestId(request_id),
      mCacheID(cache_id),
      mAllowCompressed(allow_compressed)
{
}

ImageRequest::~ImageRequest()
{
    mDecodedImageRaw = NULL;
    mDecodedImageAux = NULL;
    mFormattedImage = NULL;
}

//----------------------------------------------------------------------------


// <FS> BC7 at decode (RenderCompressTextures, rkimagebc7.h): the texels of 'raw' become a BC7 mip chain when the policy allows it and the per-block check passes; a small image, one with
// 1 or 2 components, or one the check rejects stays as it is. Runs on the decode pool.
// returns true when the check ran and kept the texels (the caller may record that so the check is not run again on a cache hit)
static bool compress_if_wanted(LLPointer<LLImageRaw>& raw)
{
    if (raw.isNull() || raw->isCompressed() || !raw->getData() || !RKImageBC7::enabled()
        || !RKImageBC7::eligible(raw->getWidth(), raw->getHeight(), raw->getComponents()))
    {
        return false;
    }
    const U32 w = raw->getWidth(), h = raw->getHeight(), c = raw->getComponents();
    std::vector<std::vector<U8>> chain;
    RKImageBC7::buildChain(raw->getData(), w, h, c, chain);
    const S8 alpha_is_mask = RKImageBC7::classifyAlpha(raw->getData(), w, h, c);
    F32 harmed = 0.f;
    LLPointer<LLImageRaw> compressed = RKImageBC7::compress(chain, w, h, c, RKImageBC7::MAX_HARMED_PERCENT, harmed, alpha_is_mask);
    if (compressed.notNull())
    {
        compressed->mComment = raw->mComment;
        raw = compressed;
        return false;
    }
    return true;
}

// Returns true when done, whether or not decode was successful.
bool ImageRequest::processRequest()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    if (mFormattedImage.isNull())
        return true;

    const F32 decode_time_slice = 0.f; //disable time slicing
    bool done = true;

    LLImageDataLock lockFormatted(mFormattedImage);
    LLImageDataLock lockDecodedRaw(mDecodedImageRaw);
    LLImageDataLock lockDecodedAux(mDecodedImageAux);

    if (!mDecodedRaw)
    {
        // Decode primary channels
        if (mDecodedImageRaw.isNull())
        {
            // parse formatted header
            if (!mFormattedImage->updateData())
            {
                return true; // done (failed)
            }
            if ((mFormattedImage->getWidth() * mFormattedImage->getHeight() * mFormattedImage->getComponents()) == 0)
            {
                return true; // done (failed)
            }
            if (mDiscardLevel >= 0)
            {
                mFormattedImage->setDiscardLevel(mDiscardLevel);
            }
            // <FS> the decoded cache first: a hit is the image this decode would have produced, at this discard level (no aux channel through the cache)
            RKDecodedImageCache* cache = sDecodedCache;
            if (cache && mCacheID.notNull() && !mNeedsAux)
            {
                const S32 discard = llmax(0, (S32)mFormattedImage->getDiscardLevel());
                bool checked_raw = false;
                LLPointer<LLImageRaw> cached = cache->read(mCacheID, discard, mAllowCompressed, &checked_raw);
                // the decoder produces the level's size (the formatted image reports the full size; comparing with that threw every hit above discard 0 away
                // and decoded it again, found 2026-10-29 at Citadel: the encode count matched the BC7 hit count)
                const U32 level_w = ((U32)mFormattedImage->getWidth() + (1u << discard) - 1) >> discard;
                const U32 level_h = ((U32)mFormattedImage->getHeight() + (1u << discard) - 1) >> discard;
                if (cached.notNull() && cached->getData() && cached->getComponents() == mFormattedImage->getComponents()
                    && (U32)cached->getWidth() == level_w && (U32)cached->getHeight() == level_h)
                {
                    mDecodedImageRaw = cached;
                    mDecodedRaw = true;
                    // a raw chain served while compression is on is encoded like a decode would be (the cache then keeps the BC7 chain; a raw hit used to bypass the encoder, 2026-10-29),
                    // unless the file says the check already kept these texels raw
                    if (mAllowCompressed && !cached->isCompressed() && !checked_raw)
                    {
                        const bool kept_raw = compress_if_wanted(mDecodedImageRaw);
                        if (mDecodedImageRaw->isCompressed() || kept_raw)
                        {
                            cache->write(mCacheID, discard, mDecodedImageRaw, kept_raw);      // the chain, or the texels again with the checked mark
                        }
                    }
                    if (!mDecodedImageRaw->isCompressed()) mDecodedImageRaw->analyzeForUpload();        // <FS> the alpha class and pick mask, here rather than at upload
                    return true;
                }
            }
            mDecodedImageRaw = new LLImageRaw(mFormattedImage->getWidth(),
                                              mFormattedImage->getHeight(),
                                              mFormattedImage->getComponents());
        }

        // <FS:ND> Probably out of memory crash
        // done = mFormattedImage->decode(mDecodedImageRaw, decode_time_slice);
        if( mDecodedImageRaw->getData() )
            done = mFormattedImage->decode(mDecodedImageRaw, decode_time_slice);
        else
        {
            LL_WARNS() << "No memory for LLImageRaw of size " << (U32)mFormattedImage->getWidth() << "x" << (U32)mFormattedImage->getHeight() << "x"
                       << (U32)mFormattedImage->getComponents() << LL_ENDL;
            done = false;
        }
        // </FS:ND>

        // some decoders are removing data when task is complete and there were errors
        mDecodedRaw = done && mDecodedImageRaw->getData();
        // <FS> BC7 at decode (RenderCompressTextures): the texels become a BC7 mip chain when the check passes
        bool checked_raw = false;
        if (mDecodedRaw && mAllowCompressed && mCacheID.notNull() && !mNeedsAux)
        {
            checked_raw = compress_if_wanted(mDecodedImageRaw);
        }
        if (mDecodedRaw && !mDecodedImageRaw->isCompressed())
        {
            mDecodedImageRaw->analyzeForUpload();       // <FS> the alpha class and pick mask, here rather than at upload (PLAN-aurora-ideas.md 6.2)
        }
        // <FS> what the decode produced goes to the decoded cache (it copies)
        if (mDecodedRaw && mCacheID.notNull() && !mNeedsAux)
        {
            if (RKDecodedImageCache* cache = sDecodedCache)
            {
                cache->write(mCacheID, llmax(0, (S32)mFormattedImage->getDiscardLevel()), mDecodedImageRaw, checked_raw);
            }
        }

        // Pick up errors from decoding
        mErrorString = LLImage::getLastThreadError();
    }
    if (done && mNeedsAux && !mDecodedAux && mFormattedImage.notNull())
    {
        // Decode aux channel
        if (!mDecodedImageAux)
        {
            mDecodedImageAux = new LLImageRaw(mFormattedImage->getWidth(),
                                              mFormattedImage->getHeight(),
                                              1);
        }
        done = mFormattedImage->decodeChannels(mDecodedImageAux, decode_time_slice, 4, 4);
        mDecodedAux = done && mDecodedImageAux->getData();

        // Pick up errors from decoding
        mErrorString = LLImage::getLastThreadError();
    }

    return done;
}

void ImageRequest::finishRequest(bool completed)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (mResponder.notNull())
    {
        bool success = completed && mDecodedRaw && (!mNeedsAux || mDecodedAux);
        mResponder->completed(success, mErrorString, mDecodedImageRaw, mDecodedImageAux, mRequestId);
    }
    // Will automatically be deleted
}
