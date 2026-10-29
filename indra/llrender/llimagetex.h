/**
 * @file llimagetex.h
 * @brief Object for managing images and their textures
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


#ifndef LL_LLIMAGEGL_H
#define LL_LLIMAGEGL_H

#include "llimage.h"

#include "llgltypes.h"
#include "llpointer.h"
#include "llrefcount.h"
#include "v2math.h"
#include "llunits.h"
#include "llthreadsafequeue.h"
#include "llrender.h"
#include "threadpool.h"
#include "workqueue.h"
#include <unordered_set>
#include <vector>
#include <mutex>
#include <algorithm>

#define LL_IMAGEGL_THREAD_CHECK 0 //set to 1 to enable thread debugging for ImageGL

class LLWindow;

#define BYTES_TO_MEGA_BYTES(x) ((x) >> 20)
#define MEGA_BYTES_TO_BYTES(x) ((x) << 20)

namespace LLImageTexMemory
{
    void alloc_tex_image(U32 width, U32 height, TexelFormat intformat, U32 count);
    void free_tex_image(U32 texName);
    void free_tex_images(U32 count, const U32* texNames);
    void free_cur_tex_image();
}

//============================================================================
class LLImageTex : public LLRefCount
{
    friend class LLTexUnit;
public:

    // <FS> native-vk (C4): while this object lives, texture uploads made on this thread from pixels inside 'raw' are not copied into the render
    // queue: the render thread keeps a reference on 'raw' and reads the pixels where they are, after the call has returned. The image must not be
    // written through getData() until it is released (calls that reallocate or write it wait for the render thread by themselves, see LLImageBase).
    class PixelHandover
    {
    public:
        explicit PixelHandover(const LLImageRaw* raw);
        ~PixelHandover();
    private:
        bool mActive;
    };

    // call once per frame
    static void updateClass();

    // Get an estimate of how many bytes have been allocated in vram for textures.
    // Does not include mipmaps.
    // NOTE: multiplying this number by two gives a good estimate for total
    // video memory usage based on testing in lagland against an NVIDIA GPU.
    static U64 getTextureBytesAllocated();

    // These 2 functions replace glGenTextures() and glDeleteTextures()
    static void generateTextures(S32 numTextures, U32 *textures);
    static void deleteTextures(S32 numTextures, const U32 *textures);

    // Size calculation
    // (F3.6e) texel formats: bits a texel takes in memory; pixel formats: client data of 8 bit components
    static S32 dataFormatBits(TexelFormat dataformat);
    static S32 dataFormatBits(PixelFormat dataformat);
    static S64 dataFormatBytes(TexelFormat dataformat, S32 width, S32 height);
    static S64 dataFormatBytes(PixelFormat dataformat, S32 width, S32 height);
    static S32 dataFormatComponents(PixelFormat dataformat);

    bool updateBindStats() const ;
    F32 getTimePassedSinceLastBound();
    void forceUpdateBindStats(void) const;

    // needs to be called every frame
    static void updateStats(F32 current_time);

    // cleanup GL state
    static void destroyResources();
    static void dirtyTexOptions();

    static bool checkSize(S32 width, S32 height);

    //for server side use only.
    // Not currently necessary for LLImageTex, but required in some derived classes,
    // so include for compatability
    static bool create(LLPointer<LLImageTex>& dest, bool usemipmaps = true);
    static bool create(LLPointer<LLImageTex>& dest, U32 width, U32 height, U8 components, bool usemipmaps = true);
    static bool create(LLPointer<LLImageTex>& dest, const LLImageRaw* imageraw, bool usemipmaps = true);

public:
    LLImageTex(bool usemipmaps = true, bool allow_compression = true);
    LLImageTex(U32 width, U32 height, U8 components, bool usemipmaps = true, bool allow_compression = true);
    LLImageTex(const LLImageRaw* imageraw, bool usemipmaps = true, bool allow_compression = true);

    // For wrapping textures created via GL elsewhere with our API only. Use with caution.
    LLImageTex(U32 mTexName, U32 components, TextureTarget target, TexelFormat formatInternal, PixelFormat formatPrimary, ComponentType formatType, LLTexUnit::eTextureAddressMode addressMode, const char* file = __builtin_FILE(), int line = __builtin_LINE());

protected:
    virtual ~LLImageTex();

    void analyzeAlpha(const void* data_in, U32 w, U32 h);
    void calcAlphaChannelOffsetAndStride();

public:
    virtual void dump();    // debugging info to LL_INFOS()

    bool setSize(S32 width, S32 height, S32 ncomponents, S32 discard_level = -1);
    void setComponents(S32 ncomponents) { mComponents = (S8)ncomponents ;}
    void setAllowCompression(bool allow) { mAllowCompression = allow; }

    // the texture is named: nothing is bound or read from the unit state
    static void setManualImage(U32 texname, TextureTarget target, S32 miplevel, TexelFormat intformat, S32 width, S32 height, PixelFormat pixformat, ComponentType pixtype, const void *pixels, bool allow_compression = true, const char* file = __builtin_FILE(), int line = __builtin_LINE());       // file/line: LL_VK_FORMAT_VERIFY

    bool createImageTexture() ;
    bool createImageTexture(S32 discard_level, const LLImageRaw* imageraw, S32 usename = 0, bool to_create = true,
        S32 category = sMaxCategories-1, bool defer_copy = false, U32* tex_name = nullptr);
    bool createImageTexture(S32 discard_level, const U8* data, bool data_hasmips = false, S32 usename = 0, bool defer_copy = false, U32* tex_name = nullptr);
    void setImage(const LLImageRaw* imageraw);
    bool setImage(const U8* data_in, bool data_hasmips = false, S32 usename = 0);
    // *TODO: This function may not work if the textures is compressed (i.e.
    // RenderCompressTextures is 0). Partial image updates do not work on
    // compressed textures.
    bool setSubImage(const LLImageRaw* imageraw, S32 x_pos, S32 y_pos, S32 width, S32 height, bool force_fast_update = false, U32 use_name = 0);
    bool setSubImage(const U8* datap, S32 data_width, S32 data_height, S32 x_pos, S32 y_pos, S32 width, S32 height, bool force_fast_update = false, U32 use_name = 0);
    bool setSubImageFromFrameBuffer(S32 fb_x, S32 fb_y, S32 x_pos, S32 y_pos, S32 width, S32 height);

    // wait for gl commands to finish on current thread and push
    // a lambda to main thread to swap mNewTexName and mTexName
    void syncToMainThread(U32 new_tex_name);

    // Read back a raw image for this discard level, if it exists
    bool readBackRaw(S32 discard_level, LLImageRaw* imageraw, bool compressed_ok) const;
    void destroyImageTexture();
    void forceToInvalidateGLTexture();

    void setExplicitFormat(TexelFormat internal_format, PixelFormat primary_format, ComponentType type_format = ComponentType::None, const char* file = __builtin_FILE(), int line = __builtin_LINE());
    void setComponents(S8 ncomponents) { mComponents = ncomponents; }

    S32  getDiscardLevel() const        { return mCurrentDiscardLevel; }
    S32  getMaxDiscardLevel() const     { return mMaxDiscardLevel; }

    // override the current discard level
    // should only be used for local textures where you know exactly what you're doing
    void setDiscardLevel(S32 level) { mCurrentDiscardLevel = level; }

    S32  getCurrentWidth() const { return mWidth ;}
    S32  getCurrentHeight() const { return mHeight ;}
    S32  getWidth(S32 discard_level = -1) const;
    S32  getHeight(S32 discard_level = -1) const;
    U8   getComponents() const { return mComponents; }
    S64  getBytes(S32 discard_level = -1) const;
    S64  getMipBytes(S32 discard_level = -1) const;
    bool getBoundRecently() const;
    bool isJustBound() const;
    bool getHasExplicitFormat() const { return mHasExplicitFormat; }
    PixelFormat getPrimaryFormat() const { return mFormatPrimary; }
    ComponentType getFormatType() const { return mFormatType; }

    bool getHasImageTexture() const { return mTexName != 0; }
    U32 getTexName() const { return mTexName; }
    // <FS> S texture identity: an id that stays with this texture object for its life (the GL name changes when the texture is made again or another discard level is loaded), and the
    // names that changed since the events were last taken: the ids of the objects whose name changed. A consumer keeps what it knows about a texture by id and hears about changes instead of
    // asking every texture every frame. Taken on the main thread (the changes are made there).
    U32 getTexId() const { return mTexId; }
    static void takeNameEvents(std::vector<U32>& ids);
    static void wantNameEvents();       // until a consumer asks, the events are not kept

    bool getIsAlphaMask() const;
    void setIsMask(bool is_mask);       // <FS> BC7 at decode: the classification handed in with the compressed image
    void adoptUploadAnalysis(const LLImageRaw* raw, U32 w, U32 h);     // <FS> the alpha class and pick mask the decode pool computed (LLImageRaw::analyzeForUpload)
    void clearExplicitFormat();         // <FS> BC7 at decode: back to the format the components choose (a texture that had a BC7 chain and now gets texels)
    bool isCompressedFormat() const;    // <FS> the storage format is a block format (BC1-3, BC7)

    bool getIsResident(bool test_now = false); // not const

    void setTarget(const TextureTarget target, const LLTexUnit::eTextureType bind_target);

    LLTexUnit::eTextureType getTarget(void) const { return mBindTarget; }
    bool isImageTextureCreated(void) const { return mImageTextureCreated ; }
    void setImageTextureCreated (bool initialized) { mImageTextureCreated = initialized; }

    bool getUseMipMaps() const { return mUseMipMaps; }
    void setUseMipMaps(bool usemips) { mUseMipMaps = usemips; }
    void setHasMipMaps(bool hasmips) { mHasMipMaps = hasmips; }
    void updatePickMask(S32 width, S32 height, const U8* data_in);
// [RLVa:KB] - Checked: RLVa-2.2 (@setoverlay)
    bool getMask(const LLVector2 &tc) const;
// [/RLVa:KB]
//  bool getMask(const LLVector2 &tc);

    void checkTexSize(bool forced = false) const ;

    // Sets the addressing mode used to sample the texture
    //  (such as wrapping, mirrored wrapping, and clamp)
    // Note: this actually gets set the next time the texture is bound.
    void setAddressMode(LLTexUnit::eTextureAddressMode mode);
    LLTexUnit::eTextureAddressMode getAddressMode(void) const { return mAddressMode; }

    // Sets the filtering options used to sample the texture
    //  (such as point sampling, bilinear interpolation, mipmapping, and anisotropic filtering)
    // Note: this actually gets set the next time the texture is bound.
    void setFilteringOption(LLTexUnit::eTextureFilterOptions option);
    LLTexUnit::eTextureFilterOptions getFilteringOption(void) const { return mFilterOption; }

    TextureTarget getTexTarget()const { return mTarget; }

    void init(bool usemipmaps, bool allow_compression);
    virtual void cleanup(); // Clean up the LLImageTex so it can be reinitialized.  Be careful when using this in derived class destructors

    void setNeedsAlphaAndPickMask(bool need_mask);

#if LL_IMAGEGL_THREAD_CHECK
    // thread debugging
    std::thread::id mActiveThread;
    void checkActiveThread();
#endif

    // scale down to the desired discard level using GPU
    // returns true if texture was scaled down
    // desired discard will be clamped to max discard
    // if desired discard is less than or equal to current discard, no scaling will occur
    // only works for GL_TEXTURE_2D target
    bool scaleDown(S32 desired_discard);

public:
    // Various GL/Rendering options
    S64Bytes mTextureMemory;
    mutable F32  mLastBindTime; // last time this was bound, by discard level

private:
    U32 createPickMask(S32 pWidth, S32 pHeight);
    void freePickMask();
    bool isCompressed();
    S64 formatBytes(S32 w, S32 h) const;     // bytes of one level of w x h in the format the data is in (block compressed: the texel format)

    LLPointer<LLImageRaw> mSaveData; // used for destroyResources/restoreResources
    LL::WorkQueue::weak_t mMainQueue;
    U8* mPickMask;  //downsampled bitmap approximation of alpha channel.  NULL if no alpha channel
    U16 mPickMaskWidth;
    U16 mPickMaskHeight;
    S8 mUseMipMaps;
    bool mHasExplicitFormat; // If false (default), GL format is f(mComponents)
    bool mAutoGenMips = false;

    bool mIsMask;
    U32  mAnalyzedPixels = 0;   // <FS> native-vk (LL_VK_ALPHA_KEEP, default on): the pixel count of the best level the alpha classification (mIsMask) was made from; a smaller upload does not redo it
    U32  mAnalysisCount = 0;    // <FS> native-vk diagnostic: how many alpha analyses this texture has had (a change of mIsMask at the first one is the classification, not a flip)
    bool mNeedsAlphaAndPickMask;
    bool mAnalysisAdopted = false;      // <FS> this upload's analysis came with the raw image: analyzeAlpha and updatePickMask stand down
    S8   mAlphaStride ;
    S8   mAlphaOffset ;

    bool     mImageTextureCreated ;
    U32 mTexName;
    U32 mTexId = 0;                     // see getTexId()
    void noteNameChange();
    U16      mWidth;
    U16      mHeight;
    S8       mCurrentDiscardLevel;

    bool mAllowCompression;

protected:
    TextureTarget mTarget;       // Normally GL_TEXTURE2D, sometimes something else (ex. cube maps)
    LLTexUnit::eTextureType mBindTarget;    // Normally TT_TEXTURE, sometimes something else (ex. cube maps)
    bool mHasMipMaps;
    S32 mMipLevels;

    bool mIsResident;

    S8 mComponents;
    S8 mMaxDiscardLevel;

    bool    mTexOptionsDirty;
    bool    mTexOptionsQueued = false;      // <FS> in sDirtyOptionImages

public:
    // <FS> The sampler options (address mode, filtering) of a texture are sent to the GPU side by the first bind after they changed. A thread that only records
    // draws for the queue must not be the one that does that (another recording may use the texture before it in the queue), so every image whose options
    // changed is queued here and flushDirtyTexOptions() sends them all at once, on the main thread, before the passes that bind textures are recorded.
    void markTexOptionsDirty();
    static void flushDirtyTexOptions();
private:
    static std::vector<LLImageTex*> sDirtyOptionImages;
    static std::mutex sDirtyOptionMutex;
    LLTexUnit::eTextureAddressMode      mAddressMode;   // Defaults to TAM_WRAP
    LLTexUnit::eTextureFilterOptions    mFilterOption;  // Defaults to TFO_ANISOTROPIC

    TexelFormat   mFormatInternal; // how the texture stores texels (was the GL internal format)
    PixelFormat   mFormatPrimary;  // the client pixel data layout (was the GL format)
    ComponentType mFormatType;

    bool mExternalTexture;

    // STATICS
public:
    static std::unordered_set<LLImageTex*> sImageList;
    static S32 sCount;
    static U32 sFrameCount;
    static F32 sLastFrameTime;

    // Global memory statistics
    static U32 sBindCount;                  // Tracks number of texture binds for current frame
    static U32 sUniqueCount;                // Tracks number of unique texture binds for current frame
    static bool sGlobalUseAnisotropic;
    static LLImageTex* sDefaultImageTexture ;
    static bool sAutomatedTest;
    static bool sCompressTextures;          //use GL texture compression
#if DEBUG_MISS
    bool mMissed; // Missed on last bind?
    bool getMissed() const { return mMissed; };
#else
    bool getMissed() const { return false; };
#endif

public:
    static void initClass(LLWindow* window, S32 num_catagories, bool skip_analyze_alpha = false, bool thread_texture_loads = false, bool thread_media_updates = false);
    static void allocateConversionBuffer();
    static void cleanupClass() ;

private:
    static S32 sMaxCategories;
    static bool sSkipAnalyzeAlpha;
    static U32* sManualScratch;

    //the flag to allow to call readBackRaw(...).
    //can be removed if we do not use that function at all.
    static bool sAllowReadBackRaw ;
//
//****************************************************************************************************
//The below for texture auditing use only
//****************************************************************************************************
private:
    S32 mCategory ;
public:
    void setCategory(S32 category) {mCategory = category;}
    S32  getCategory()const {return mCategory;}

    void setTexName(U32 texName) { mTexName = texName; }

    //similar to setTexName, but will call deleteTextures on mTexName if mTexName is not 0 or texname
    void syncTexName(U32 texname);

    //for debug use: show texture size distribution
    //----------------------------------------
    static S32 sCurTexSizeBar ;
    static S32 sCurTexPickSize ;

    static void setCurTexSizebar(S32 index, bool set_pick_size = true) ;
    static void resetCurTexSizebar();

//****************************************************************************************************
//End of definitions for texture auditing use only
//****************************************************************************************************

};

class LLImageTexThread : public LLSimpleton<LLImageTexThread>, LL::ThreadPool
{
public:
    // follows gSavedSettings "RenderGLMultiThreadedTextures"
    static bool sEnabledTextures;
    // follows gSavedSettings "RenderGLMultiThreadedMedia"
    static bool sEnabledMedia;

    LLImageTexThread(LLWindow* window);

    // post a function to be executed on the LLImageTex background thread
    template <typename CALLABLE>
    bool post(CALLABLE&& func)
    {
        return getQueue().post(std::forward<CALLABLE>(func));
    }

    void run() override;

private:
    LLWindow* mWindow;
    void* mContext = nullptr;
    LLAtomicBool mFinished;
};

#endif // LL_LLIMAGEGL_H
