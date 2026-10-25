/**
 * @file llimagetex.cpp
 * @brief Generic GL image handler
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


// TODO: create 2 classes for images w/ and w/o discard levels?

#include "linden_common.h"
#include <chrono>
#include <atomic>

#include "llimagetex.h"

#include "llerror.h"
#include "llfasttimer.h"
#include "llimage.h"

#include "llmath.h"
#include "lldevicecaps.h"
#include "llshaderprogram.h"
#include "llrender.h"
#include "llwindow.h"
#include "llframetimer.h"
#include "llvkasync.h"
#include "llvkstate.h"
#include "llvktex.h"
#include "llvertexbuffer.h"
#include "llrendertarget.h"
#include <unordered_set>

extern LL_COMMON_API bool on_main_thread();

#if !LL_IMAGEGL_THREAD_CHECK
#define checkActiveThread()
#endif

//----------------------------------------------------------------------------
const F32 MIN_TEXTURE_LIFETIME = 10.f;
const F32 CONVERSION_SCRATCH_BUFFER_GL_VERSION = 3.29f;

//which power of 2 is i?
//assumes i is a power of 2 > 0
U32 wpo2(U32 i);


U32 LLImageTex::sFrameCount = 0;


// texture memory accounting (for macOS)
static LLMutex sTexMemMutex;
static std::unordered_map<U32, U64> sTextureAllocs;
static U64 sTextureBytes = 0;

// track a texture alloc on the currently bound texture.
// asserts that no currently tracked alloc exists
void LLImageTexMemory::alloc_tex_image(U32 width, U32 height, TexelFormat intformat, U32 count)
{
    U32 texName = gRender.getTexUnit(0)->getCurrTexture();      // allocations are always done on tex unit 0
    U64 size = LLImageTex::dataFormatBytes(intformat, width, height);
    size *= count;

    llassert(size >= 0);

    sTexMemMutex.lock();

    // it is a precondition that no existing allocation exists for this texture
    llassert(sTextureAllocs.find(texName) == sTextureAllocs.end());

    sTextureAllocs[texName] = size;
    sTextureBytes += size;

    sTexMemMutex.unlock();
}

// track texture free on given texName
void LLImageTexMemory::free_tex_image(U32 texName)
{
    sTexMemMutex.lock();
    auto iter = sTextureAllocs.find(texName);
    if (iter != sTextureAllocs.end()) // sometimes a texName will be "freed" before allocated (e.g. first call to setManualImage for a given texName)
    {
        llassert(iter->second <= sTextureBytes); // sTextureBytes MUST NOT go below zero

        sTextureBytes -= iter->second;

        sTextureAllocs.erase(iter);
    }

    sTexMemMutex.unlock();
}

// track texture free on given texNames
void LLImageTexMemory::free_tex_images(U32 count, const U32* texNames)
{
    for (U32 i = 0; i < count; ++i)
    {
        free_tex_image(texNames[i]);
    }
}

// track texture free on currently bound texture
void LLImageTexMemory::free_cur_tex_image()
{
    U32 texName = gRender.getTexUnit(0)->getCurrTexture();      // frees are always done on tex unit 0
    free_tex_image(texName);
}

using namespace LLImageTexMemory;

// static
U64 LLImageTex::getTextureBytesAllocated()
{
    return sTextureBytes;
}

//statics

U32 LLImageTex::sUniqueCount             = 0;
U32 LLImageTex::sBindCount               = 0;
S32 LLImageTex::sCount                   = 0;

bool LLImageTex::sGlobalUseAnisotropic   = false;
F32 LLImageTex::sLastFrameTime           = 0.f;
LLImageTex* LLImageTex::sDefaultImageTexture = NULL ;
bool LLImageTex::sCompressTextures = false;
std::unordered_set<LLImageTex*> LLImageTex::sImageList;


bool LLImageTexThread::sEnabledTextures = false;
bool LLImageTexThread::sEnabledMedia = false;

//****************************************************************************************************
//The below for texture auditing use only
//****************************************************************************************************
//-----------------------
//debug use
S32 LLImageTex::sCurTexSizeBar = -1 ;
S32 LLImageTex::sCurTexPickSize = -1 ;
S32 LLImageTex::sMaxCategories = 1 ;

//optimization for when we don't need to calculate mIsMask
bool LLImageTex::sSkipAnalyzeAlpha;
U32* LLImageTex::sManualScratch = nullptr;


//------------------------
//****************************************************************************************************
//End for texture auditing use only
//****************************************************************************************************

//**************************************************************************************
//below are functions for debug use
//do not delete them even though they are not currently being used.

void LLImageTex::checkTexSize(bool forced) const
{
    if ((forced || gDebugRender) && mTarget == TextureTarget::Tex2D)
    {
        S32 texname;
        texname = (S32)mTexName;
        bool error = false;
        if (texname != mTexName)
        {
            LL_INFOS() << "Bound: " << texname << " Should bind: " << mTexName << " Default: " << LLImageTex::sDefaultImageTexture->getTexName() << LL_ENDL;

            error = true;
            if (gDebugSession)
            {
                gFailLog << "Invalid texture bound!" << std::endl;
            }
            else
            {
                LL_ERRS() << "Invalid texture bound!" << LL_ENDL;
            }
        }
        S32 x = 0, y = 0 ;
        x = LLVkTex::levelParameter(mTexName, mTarget, 0, LevelQuery::Width);
        y = LLVkTex::levelParameter(mTexName, mTarget, 0, LevelQuery::Height);
        llcallstacks << "w: " << x << " h: " << y << llcallstacksendl ;

        if(!x || !y)
        {
            return ;
        }
        if(x != (mWidth >> mCurrentDiscardLevel) || y != (mHeight >> mCurrentDiscardLevel))
        {
            error = true;
            if (gDebugSession)
            {
                gFailLog << "wrong texture size and discard level!" <<
                    mWidth << " Height: " << mHeight << " Current Level: " << (S32)mCurrentDiscardLevel << std::endl;
            }
            else
            {
                LL_ERRS() << "wrong texture size and discard level: width: " <<
                    mWidth << " Height: " << mHeight << " Current Level: " << (S32)mCurrentDiscardLevel << LL_ENDL ;
            }
        }

        if (error)
        {
            ll_fail("LLImageTex::checkTexSize failed.");
        }
    }
}
//end of debug functions
//**************************************************************************************

//----------------------------------------------------------------------------
bool is_little_endian()
{
    S32 a = 0x12345678;
    U8 *c = (U8*)(&a);

    return (*c == 0x78) ;
}

//static
void LLImageTex::initClass(LLWindow* window, S32 num_catagories, bool skip_analyze_alpha /* = false */, bool thread_texture_loads /* = false */, bool thread_media_updates /* = false */)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    sSkipAnalyzeAlpha = skip_analyze_alpha;

    if (thread_texture_loads || thread_media_updates)
    {
        LLImageTexThread::createInstance(window);
        LLImageTexThread::sEnabledTextures = gDeviceCaps.mApiVersionNumber > 3.95f ? thread_texture_loads : false;
        LLImageTexThread::sEnabledMedia = gDeviceCaps.mApiVersionNumber > 3.95f ? thread_media_updates : false;
    }
}

void LLImageTex::allocateConversionBuffer()
{
    if (gDeviceCaps.mApiVersionNumber < CONVERSION_SCRATCH_BUFFER_GL_VERSION)
    {
        try
        {
            sManualScratch = new U32[MAX_IMAGE_AREA];
        }
        catch (std::bad_alloc&)
        {
            LLError::LLUserWarningMsg::showOutOfMemory();
            LL_ERRS() << "Failed to allocate sManualScratch" << LL_ENDL;
        }
    }
}

//static
void LLImageTex::cleanupClass()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    LLImageTexThread::deleteSingleton();

    delete[] sManualScratch;
}


//static
S32 LLImageTex::dataFormatBits(TexelFormat f)
{
    switch (f)
    {
    case TexelFormat::BC1_RGB:
    case TexelFormat::BC1_RGBA:
    case TexelFormat::BC1_SRGBA:                    return 4;
    case TexelFormat::BC2:
    case TexelFormat::BC3:
    case TexelFormat::BC7:
    case TexelFormat::BC2_SRGBA:
    case TexelFormat::BC3_SRGBA:                    return 8;
    case TexelFormat::GenericCompressed:            return 32;
    case TexelFormat::Lum8:
    case TexelFormat::Alpha8:
    case TexelFormat::R8:                           return 8;
    case TexelFormat::LumAlpha8:
    case TexelFormat::RG8:
    case TexelFormat::R16F:                         return 16;
    case TexelFormat::RGB8:
    case TexelFormat::SRGB8:
    case TexelFormat::Depth:                        return 24;
    case TexelFormat::RGBA8:
    case TexelFormat::SRGBA8:
    case TexelFormat::R11G11B10F:
    case TexelFormat::RGB10A2:
    case TexelFormat::RG16F:
    case TexelFormat::RG16:
    case TexelFormat::R32F:                         return 32;
    case TexelFormat::RGBA16:
    case TexelFormat::RGBA16F:
    case TexelFormat::RG32F:                        return 64;
    case TexelFormat::RGBA32F:                      return 128;
    default:
        LL_ERRS() << "LLImageTex::Unknown format: " << (S32)f << LL_ENDL;
        return 0;
    }
}

//static
S32 LLImageTex::dataFormatBits(PixelFormat f)
{
    switch (f)
    {
    case PixelFormat::R:
    case PixelFormat::Lum:
    case PixelFormat::Alpha:                        return 8;
    case PixelFormat::RG:
    case PixelFormat::LumAlpha:                     return 16;
    case PixelFormat::RGB:
    case PixelFormat::BGR:
    case PixelFormat::Depth:                        return 24;
    case PixelFormat::RGBA:
    case PixelFormat::BGRA:                         return 32;      // BGRA: used for media textures
    default:
        LL_ERRS() << "LLImageTex::Unknown format: " << (S32)f << LL_ENDL;
        return 0;
    }
}

static S64 level_bytes(S32 bits, S32 width, S32 height, bool block4)
{
    if (block4)
    {
        if (width < 4) width = 4;
        if (height < 4) height = 4;
    }
    S64 bytes(((S64)width * (S64)height * (S64)bits + 7) >> 3);
    return (bytes + 3) & ~3;
}

static bool is_s3tc(TexelFormat f)
{
    return f == TexelFormat::BC1_RGBA || f == TexelFormat::BC1_SRGBA || f == TexelFormat::BC2 || f == TexelFormat::BC2_SRGBA || f == TexelFormat::BC3 || f == TexelFormat::BC3_SRGBA;
}

//static
S64 LLImageTex::dataFormatBytes(TexelFormat dataformat, S32 width, S32 height)
{
    return level_bytes(dataFormatBits(dataformat), width, height, is_s3tc(dataformat));
}

//static
S64 LLImageTex::dataFormatBytes(PixelFormat dataformat, S32 width, S32 height)
{
    return level_bytes(dataFormatBits(dataformat), width, height, false);
}

//static
S32 LLImageTex::dataFormatComponents(PixelFormat dataformat)
{
    switch (dataformat)
    {
      case PixelFormat::Lum:
      case PixelFormat::Alpha:
      case PixelFormat::R:                      return 1;
      case PixelFormat::LumAlpha:
      case PixelFormat::RG:                     return 2;
      case PixelFormat::RGB:
      case PixelFormat::BGR:                    return 3;
      case PixelFormat::RGBA:
      case PixelFormat::BGRA:                   return 4;       // BGRA: used for media textures
      default:
        LL_ERRS() << "LLImageTex::Unknown format: " << (S32)dataformat << LL_ENDL;
        return 0;
    }
}

//----------------------------------------------------------------------------

// static
void LLImageTex::updateStats(F32 current_time)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    sLastFrameTime = current_time;
}

//----------------------------------------------------------------------------

//static
void LLImageTex::destroyResources()
{
    for (S32 stage = 0; stage < gDeviceCaps.mNumTextureImageUnits; stage++)
    {
        gRender.getTexUnit(stage)->bindWhite();
    }
}

std::vector<LLImageTex*> LLImageTex::sDirtyOptionImages;
std::mutex LLImageTex::sDirtyOptionMutex;

void LLImageTex::markTexOptionsDirty()
{
    mTexOptionsDirty = true;
    std::lock_guard<std::mutex> lock(sDirtyOptionMutex);
    if (!mTexOptionsQueued)
    {
        mTexOptionsQueued = true;
        sDirtyOptionImages.push_back(this);
    }
}

//static
void LLImageTex::flushDirtyTexOptions()
{
    static U64 sCalls = 0, sImages = 0, sApplied = 0, sMaxApplied = 0, sNs = 0;
    const auto start = std::chrono::steady_clock::now();
    U64 applied = 0;
    std::lock_guard<std::mutex> lock(sDirtyOptionMutex);
    sImages += sDirtyOptionImages.size();
    for (LLImageTex* image : sDirtyOptionImages)
    {
        image->mTexOptionsQueued = false;
        if (image->mTexOptionsDirty && image->mTexName)
        {
            gRender.getTexUnit(0)->applyTexOptions(image);
            ++applied;
        }
    }
    sDirtyOptionImages.clear();
    sApplied += applied;
    sMaxApplied = std::max(sMaxApplied, applied);
    sNs += (U64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    if (++sCalls % 600 == 0)
    {
        LL_INFOS("TexOptFlush") << "per frame over 600 frames: " << (double)sImages / 600.0 << " images queued, " << (double)sApplied / 600.0 << " applied (max "
            << sMaxApplied << " in one frame), " << (double)sNs / 1.0e6 / 600.0 << " ms" << LL_ENDL;
        sImages = sApplied = sMaxApplied = sNs = 0;
    }
}

//static
void LLImageTex::dirtyTexOptions()
{
    for (auto& glimage : sImageList)
    {
        glimage->markTexOptionsDirty();
    }

}
//----------------------------------------------------------------------------

//for server side use only.
//static
bool LLImageTex::create(LLPointer<LLImageTex>& dest, bool usemipmaps)
{
    dest = new LLImageTex(usemipmaps);
    return true;
}

//for server side use only.
bool LLImageTex::create(LLPointer<LLImageTex>& dest, U32 width, U32 height, U8 components, bool usemipmaps)
{
    dest = new LLImageTex(width, height, components, usemipmaps);
    return true;
}

//for server side use only.
bool LLImageTex::create(LLPointer<LLImageTex>& dest, const LLImageRaw* imageraw, bool usemipmaps)
{
    dest = new LLImageTex(imageraw, usemipmaps);
    return true;
}

//----------------------------------------------------------------------------

LLImageTex::LLImageTex(bool usemipmaps/* = true*/, bool allow_compression/* = true*/)
:   mSaveData(0), mExternalTexture(false)
{
    init(usemipmaps, allow_compression);
    setSize(0, 0, 0);
    sImageList.insert(this);
    sCount++;
}

LLImageTex::LLImageTex(U32 width, U32 height, U8 components, bool usemipmaps/* = true*/, bool allow_compression/* = true*/)
:   mSaveData(0), mExternalTexture(false)
{
    llassert( components <= 4 );
    init(usemipmaps, allow_compression);
    setSize(width, height, components);
    sImageList.insert(this);
    sCount++;
}

LLImageTex::LLImageTex(const LLImageRaw* imageraw, bool usemipmaps/* = true*/, bool allow_compression/* = true*/)
:   mSaveData(0), mExternalTexture(false)
{
    init(usemipmaps, allow_compression);
    setSize(0, 0, 0);
    sImageList.insert(this);
    sCount++;

    createImageTexture(0, imageraw);
}

LLImageTex::LLImageTex(
    U32 texName,
    U32 components,
    TextureTarget target,
    TexelFormat formatInternal,
    PixelFormat formatPrimary,
    ComponentType formatType,
    LLTexUnit::eTextureAddressMode addressMode,
    const char* file, int line)
{
    LLVkFormat::noteFormat("LLImageTex", file, line, (U8)formatInternal, (U8)formatPrimary, (U8)formatType);
    init(false, true);
    mTexName = texName;
    noteNameChange();
    markTexOptionsDirty();
    mTarget = target;
    mComponents = components;
    mAddressMode = addressMode;
    mFormatType = formatType;
    mFormatInternal = formatInternal;
    mFormatPrimary = formatPrimary;
}


static std::mutex sNameEventMutex;
static std::vector<U32> sNameEventIds;
static std::atomic<U32> sNextTexId{1};
static std::atomic<bool> sNameEventsWanted{false};

void LLImageTex::wantNameEvents()
{
    sNameEventsWanted = true;
}

void LLImageTex::noteNameChange()
{
    if (mTexId == 0)
    {
        mTexId = sNextTexId.fetch_add(1);
    }
    if (!sNameEventsWanted)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(sNameEventMutex);
    sNameEventIds.push_back(mTexId);
}

void LLImageTex::takeNameEvents(std::vector<U32>& ids)
{
    ids.clear();
    std::lock_guard<std::mutex> lock(sNameEventMutex);
    ids.swap(sNameEventIds);
}

LLImageTex::~LLImageTex()
{
    if (mTexOptionsQueued)
    {
        std::lock_guard<std::mutex> lock(sDirtyOptionMutex);
        sDirtyOptionImages.erase(std::remove(sDirtyOptionImages.begin(), sDirtyOptionImages.end(), this), sDirtyOptionImages.end());
    }
    if (!mExternalTexture && gDeviceCaps.mInited)
    {
        LLImageTex::cleanup();
        sImageList.erase(this);
        freePickMask();
        sCount--;
    }
}

void LLImageTex::init(bool usemipmaps, bool allow_compression)
{
#if LL_IMAGEGL_THREAD_CHECK
    mActiveThread = LLThread::currentID();
#endif

    // keep these members in the same order as declared in llimagehl.h
    // so that it is obvious by visual inspection if we forgot to
    // init a field.

    mTextureMemory = S64Bytes(0);
    mLastBindTime = 0.f;

    mPickMask = NULL;
    mPickMaskWidth = 0;
    mPickMaskHeight = 0;
    mUseMipMaps = usemipmaps;
    mHasExplicitFormat = false;

    mIsMask = false;
    mNeedsAlphaAndPickMask = true ;
    mAlphaStride = 0 ;
    mAlphaOffset = 0 ;

    mImageTextureCreated = false ;
    mTexName = 0;
    noteNameChange();
    mWidth = 0;
    mHeight = 0;
    mCurrentDiscardLevel = -1;

    mAllowCompression = allow_compression;

    mTarget = TextureTarget::Tex2D;
    mBindTarget = LLTexUnit::TT_TEXTURE;
    mHasMipMaps = false;
    mMipLevels = -1;

    mIsResident = 0;

    mComponents = 0;
    mMaxDiscardLevel = MAX_DISCARD_LEVEL;

    mTexOptionsDirty = true;
    mAddressMode = LLTexUnit::TAM_WRAP;
    mFilterOption = LLTexUnit::TFO_ANISOTROPIC;

    mFormatInternal = TexelFormat::None;
    mFormatPrimary = PixelFormat::None;
    mFormatType = ComponentType::U8;

#ifdef DEBUG_MISS
    mMissed = false;
#endif

    mCategory = -1;

    // Sometimes we have to post work for the main thread.
    mMainQueue = LL::WorkQueue::getInstance("mainloop");
}

void LLImageTex::cleanup()
{
    if (!gDeviceCaps.mIsDisabled)
    {
        destroyImageTexture();
    }
    freePickMask();

    mSaveData = NULL; // deletes data
}

//----------------------------------------------------------------------------

//this function is used to check the size of a texture image.
//so dim should be a positive number
static bool check_power_of_two(S32 dim)
{
    if(dim < 0)
    {
        return false ;
    }
    if(!dim)//0 is a power-of-two number
    {
        return true ;
    }
    return !(dim & (dim - 1)) ;
}

//static
bool LLImageTex::checkSize(S32 width, S32 height)
{
    return check_power_of_two(width) && check_power_of_two(height);
}

bool LLImageTex::setSize(S32 width, S32 height, S32 ncomponents, S32 discard_level)
{
    if (width != mWidth || height != mHeight || ncomponents != mComponents)
    {
        // Check if dimensions are a power of two!
        if (!checkSize(width, height))
        {
            LL_WARNS() << llformat("Texture has non power of two dimension: %dx%d",width,height) << LL_ENDL;
            return false;
        }

        mWidth = width;
        mHeight = height;
        mComponents = ncomponents;
        if (ncomponents > 0)
        {
            mMaxDiscardLevel = 0;
            while (width > 1 && height > 1 && mMaxDiscardLevel < MAX_DISCARD_LEVEL)
            {
                mMaxDiscardLevel++;
                width >>= 1;
                height >>= 1;
            }

            if(discard_level > 0)
            {
                mMaxDiscardLevel = llmax(mMaxDiscardLevel, (S8)discard_level);
                // <FS:minerjr> [FIRE-35361] RenderMaxTextureResolution caps texture resolution lower than intended
                // 2K textures could set the mMaxDiscardLevel above MAX_DISCARD_LEVEL, which would
                // cause them to not be down-scaled so they would get stuck at 0 discard all the time.
                mMaxDiscardLevel = llmin(mMaxDiscardLevel, (S8)MAX_DISCARD_LEVEL);
                // </FS:minerjr> [FIRE-35361]
            }
        }
        else
        {
            mMaxDiscardLevel = MAX_DISCARD_LEVEL;
        }
    }

    return true;
}

//----------------------------------------------------------------------------

// virtual
void LLImageTex::dump()
{
    LL_INFOS() << "mMaxDiscardLevel " << S32(mMaxDiscardLevel)
            << " mLastBindTime " << mLastBindTime
            << " mTarget " << S32(mTarget)
            << " mBindTarget " << S32(mBindTarget)
            << " mUseMipMaps " << S32(mUseMipMaps)
            << " mHasMipMaps " << S32(mHasMipMaps)
            << " mCurrentDiscardLevel " << S32(mCurrentDiscardLevel)
            << " mFormatInternal " << S32(mFormatInternal)
            << " mFormatPrimary " << S32(mFormatPrimary)
            << " mFormatType " << S32(mFormatType)
            << " mHasExplicitFormat " << S32(mHasExplicitFormat)
#if DEBUG_MISS
            << " mMissed " << mMissed
#endif
            << LL_ENDL;

    LL_INFOS() << " mTextureMemory " << mTextureMemory
            << " mTexNames " << mTexName
            << " mIsResident " << S32(mIsResident)
            << LL_ENDL;
}

//----------------------------------------------------------------------------
void LLImageTex::forceUpdateBindStats(void) const
{
    mLastBindTime = sLastFrameTime;
}

bool LLImageTex::updateBindStats() const
{
    if (mTexName != 0)
    {
#ifdef DEBUG_MISS
        mMissed = ! getIsResident(true);
#endif
        sBindCount++;
        if (mLastBindTime != sLastFrameTime)
        {
            // we haven't accounted for this texture yet this frame
            sUniqueCount++;
            mLastBindTime = sLastFrameTime;

            return true ;
        }
    }
    return false ;
}

F32 LLImageTex::getTimePassedSinceLastBound()
{
    return sLastFrameTime - mLastBindTime ;
}

void LLImageTex::setExplicitFormat( TexelFormat internal_format, PixelFormat primary_format, ComponentType type_format, const char* file, int line )
{
    LLVkFormat::noteFormat("setExplicitFormat", file, line, (U8)internal_format, (U8)primary_format, (U8)(type_format == ComponentType::None ? ComponentType::U8 : type_format));
    // Note: must be called before createTexture()
    // Note: it's up to the caller to ensure that the format matches the number of components.
    mHasExplicitFormat = true;
    mFormatInternal = internal_format;
    mFormatPrimary = primary_format;
    if(type_format == ComponentType::None)
        mFormatType = ComponentType::U8;
    else
        mFormatType = type_format;

    calcAlphaChannelOffsetAndStride() ;
}

//----------------------------------------------------------------------------

// <FS> native-vk (C4): see the declaration in llimagetex.h
LLImageTex::PixelHandover::PixelHandover(const LLImageRaw* raw)
:   mActive(false)
{
    if (!raw || raw->isBufferInvalid() || !raw->getData() || raw->getDataSize() <= 0)
    {
        return;
    }
    static const bool hook_set = []() {
        LLImageBase::setPixelHoldWait([](const LLImageBase*) { LLVkAsync::waitForPixelHolds(); });
        return true;
    }();
    (void)hook_set;
    LLVkAsync::PixelHold hold = { raw->getData(), (size_t)raw->getDataSize(), raw,
                                  [](const void* o) { LLImageRaw* i = const_cast<LLImageRaw*>((const LLImageRaw*)o); i->ref(); i->addPixelHold(); },
                                  [](const void* o) { LLImageRaw* i = const_cast<LLImageRaw*>((const LLImageRaw*)o); i->releasePixelHold(); i->unref(); } };
    LLVkAsync::setPixelHold(&hold);
    mActive = true;
}

LLImageTex::PixelHandover::~PixelHandover()
{
    if (mActive)
    {
        LLVkAsync::setPixelHold(nullptr);
    }
}

void LLImageTex::setImage(const LLImageRaw* imageraw)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    llassert((imageraw->getWidth() == getWidth(mCurrentDiscardLevel)) &&
             (imageraw->getHeight() == getHeight(mCurrentDiscardLevel)) &&
             (imageraw->getComponents() == getComponents()));
    const U8* rawdata = imageraw->getData();
    setImage(rawdata, false);
}

bool LLImageTex::setImage(const U8* data_in, bool data_hasmips /* = false */, S32 usename /* = 0 */)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    const bool is_compressed = isCompressed();

    if (mUseMipMaps)
    {
        //set has mip maps to true before binding image so tex parameters get set properly
        gRender.getTexUnit(0)->unbind(mBindTarget);

        mHasMipMaps = true;
        markTexOptionsDirty();
        setFilteringOption(LLTexUnit::TFO_ANISOTROPIC);
    }
    else
    {
        mHasMipMaps = false;
    }

    gRender.getTexUnit(0)->bind(this, false, false, usename);
    const U32 edit_name = usename ? (U32)usename : mTexName;

    if (data_in == nullptr)
    {
        S32 w = getWidth();
        S32 h = getHeight();
        LLImageTex::setManualImage(edit_name, mTarget, 0, mFormatInternal, w, h,
            mFormatPrimary, mFormatType, (void*)data_in, mAllowCompression);
    }
    else if (mUseMipMaps)
    {
        if (data_hasmips)
        {
            // NOTE: data_in points to largest image; smaller images
            // are stored BEFORE the largest image
            for (S32 d=mCurrentDiscardLevel; d<=mMaxDiscardLevel; d++)
            {

                S32 w = getWidth(d);
                S32 h = getHeight(d);
                S32 gl_level = d-mCurrentDiscardLevel;

                mMipLevels = llmax(mMipLevels, gl_level);

                if (d > mCurrentDiscardLevel)
                {
                    data_in -= formatBytes(w, h); // see above comment
                }
                if (is_compressed)
                {
                    S32 tex_size = (S32)formatBytes(w, h);
                    LLVkTex::compressedImage2D(edit_name, mTarget, gl_level, mFormatInternal, w, h, tex_size, (void *)data_in);
                }
                else
                {

                    LLImageTex::setManualImage(edit_name, mTarget, gl_level, mFormatInternal, w, h, mFormatPrimary, ComponentType::U8, (void*)data_in, mAllowCompression);
                    if (gl_level == 0)
                    {
                        analyzeAlpha(data_in, w, h);
                    }
                    updatePickMask(w, h, data_in);


                }
            }
        }
        else if (!is_compressed)
        {
            if (mAutoGenMips)
            {
                {

                    S32 w = getWidth(mCurrentDiscardLevel);
                    S32 h = getHeight(mCurrentDiscardLevel);

                    mMipLevels = wpo2(llmax(w, h));

                    //use legacy mipmap generation mode (note: making this condional can cause rendering issues)
                    // -- but making it not conditional triggers deprecation warnings when core profile is enabled
                    //      (some rendering issues while core profile is enabled are acceptable at this point in time)
                    if (!LLRender::sGLCoreProfile)
                    {
                    }

                    LLImageTex::setManualImage(edit_name, mTarget, 0, mFormatInternal,
                                 w, h,
                                 mFormatPrimary, mFormatType,
                                 data_in, mAllowCompression);
                    analyzeAlpha(data_in, w, h);

                    updatePickMask(w, h, data_in);


                    if (LLRender::sGLCoreProfile)
                    {
                        LL_PROFILE_GPU_ZONE("generate mip map");
                        LLVkTex::generateMipmap(edit_name);
                    }
                }
            }
            else
            {
                // Create mips by hand
                // ~4x faster than gluBuild2DMipmaps
                S32 width = getWidth(mCurrentDiscardLevel);
                S32 height = getHeight(mCurrentDiscardLevel);
                S32 nummips = mMaxDiscardLevel - mCurrentDiscardLevel + 1;
                S32 w = width, h = height;


                const U8* new_data = 0;
                (void)new_data;

                const U8* prev_mip_data = 0;
                const U8* cur_mip_data = 0;
#ifdef SHOW_ASSERT
                S32 cur_mip_size = 0;
#endif
                mMipLevels = nummips;

                for (int m=0; m<nummips; m++)
                {
                    if (m==0)
                    {
                        cur_mip_data = data_in;
#ifdef SHOW_ASSERT
                        cur_mip_size = width * height * mComponents;
#endif
                    }
                    else
                    {
                        S32 bytes = w * h * mComponents;
#ifdef SHOW_ASSERT
                        llassert(prev_mip_data);
                        llassert(cur_mip_size == bytes*4);
#endif
                        U8* new_data = new(std::nothrow) U8[bytes];
                        if (!new_data)
                        {

                            if (prev_mip_data)
                            {
                                if (prev_mip_data != cur_mip_data)
                                    delete[] prev_mip_data;
                                prev_mip_data = nullptr;
                            }
                            if (cur_mip_data)
                            {
                                delete[] cur_mip_data;
                                cur_mip_data = nullptr;
                            }

                            mImageTextureCreated = false;
                            return false;
                        }
                        else
                        {

#ifdef SHOW_ASSERT
                            llassert(prev_mip_data);
                            llassert(cur_mip_size == bytes * 4);
#endif

                            LLImageBase::generateMip(prev_mip_data, new_data, w, h, mComponents);
                            cur_mip_data = new_data;
#ifdef SHOW_ASSERT
                            cur_mip_size = bytes;
#endif
                        }

                    }
                    llassert(w > 0 && h > 0 && cur_mip_data);
                    (void)cur_mip_data;
                    {

                        LLImageTex::setManualImage(edit_name, mTarget, m, mFormatInternal, w, h, mFormatPrimary, mFormatType, cur_mip_data, mAllowCompression);
                        if (m == 0)
                        {
                            analyzeAlpha(data_in, w, h);
                        }
                        if (m == 0)
                        {
                            updatePickMask(w, h, cur_mip_data);
                        }

                    }
                    if (prev_mip_data && prev_mip_data != data_in)
                    {
                        delete[] prev_mip_data;
                    }
                    prev_mip_data = cur_mip_data;
                    w >>= 1;
                    h >>= 1;
                }
                if (prev_mip_data && prev_mip_data != data_in)
                {
                    delete[] prev_mip_data;
                    prev_mip_data = NULL;
                }
            }
        }
        else
        {
            LL_ERRS() << "Compressed Image has mipmaps but data does not (can not auto generate compressed mips)" << LL_ENDL;
        }
    }
    else
    {
        mMipLevels = 0;
        S32 w = getWidth();
        S32 h = getHeight();
        if (is_compressed)
        {
            S32 tex_size = (S32)formatBytes(w, h);
            LLVkTex::compressedImage2D(edit_name, mTarget, 0, mFormatInternal, w, h, tex_size, (void *)data_in);
        }
        else
        {

            LLImageTex::setManualImage(edit_name, mTarget, 0, mFormatInternal, w, h,
                         mFormatPrimary, mFormatType, (void *)data_in, mAllowCompression);
            analyzeAlpha(data_in, w, h);

            updatePickMask(w, h, data_in);



        }
    }
    mImageTextureCreated = true;
    return true;
}

U32 type_width_from_pixtype(ComponentType pixtype)
{
    U32 type_width = 0;
    switch (pixtype)
    {
    case ComponentType::U8:
    case ComponentType::S8:
        type_width = 1;
        break;
    case ComponentType::U16:
    case ComponentType::S16:
    case ComponentType::F16:
        type_width = 2;
        break;
    case ComponentType::U32:
    case ComponentType::S32:
    case ComponentType::F32:
        type_width = 4;
        break;
    default:
        LL_ERRS() << "Unknown type: " << (S32)pixtype << LL_ENDL;
    }
    return type_width;
}

bool should_stagger_image_set(bool compressed)
{
#if LL_DARWIN
    return !compressed && on_main_thread() && gDeviceCaps.mIsAMD;
#else
    // glTexSubImage2D doesn't work with compressed textures on select tested Nvidia GPUs on Windows 10 -Cosmic,2023-03-08
    // Setting media textures off-thread seems faster when not using sub_image_lines (Nvidia/Windows 10) -Cosmic,2023-03-31
    return !compressed && on_main_thread() && !gDeviceCaps.mIsIntel;
#endif
}

// Equivalent to calling glSetSubImage2D(target, miplevel, x_offset, y_offset, width, height, pixformat, pixtype, src), assuming the total width of the image is data_width
// However, instead there are multiple calls to glSetSubImage2D on smaller slices of the image
void sub_image_lines(U32 texname, TextureTarget target, S32 miplevel, S32 x_offset, S32 y_offset, S32 width, S32 height, PixelFormat pixformat, ComponentType pixtype, const U8* src, S32 data_width)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    LL_PROFILE_ZONE_NUM(width);
    LL_PROFILE_ZONE_NUM(height);

    U32 components = LLImageTex::dataFormatComponents(pixformat);
    U32 type_width = type_width_from_pixtype(pixtype);

    const U32 line_width = data_width * components * type_width;
    const U32 y_offset_end = y_offset + height;

    if (width == data_width && height % 32 == 0)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("subimage - batched lines");

        // full width, batch multiple lines at a time
        // set batch size based on width
        U32 batch_size = 32;

        if (width > 1024)
        {
            batch_size = 8;
        }
        else if (width > 512)
        {
            batch_size = 16;
        }

        // full width texture, do 32 lines at a time
        for (U32 y_pos = y_offset; y_pos < y_offset_end; y_pos += batch_size)
        {
            // If this keeps crashing, pass down data_size, looks like it is using
            // imageraw->getData(); for data, but goes way over allocated size limit
            LLVkTex::subImage2D(texname, target, miplevel, x_offset, y_pos, width, batch_size, pixformat, pixtype, src);
            src += line_width * batch_size;
        }
    }
    else
    {
        // partial width or strange height
        for (U32 y_pos = y_offset; y_pos < y_offset_end; y_pos += 1)
        {
            // If this keeps crashing, pass down data_size, looks like it is using
            // imageraw->getData(); for data, but goes way over allocated size limit
            LLVkTex::subImage2D(texname, target, miplevel, x_offset, y_pos, width, 1, pixformat, pixtype, src);
            src += line_width;
        }
    }
}

bool LLImageTex::setSubImage(const U8* datap, S32 data_width, S32 data_height, S32 x_pos, S32 y_pos, S32 width, S32 height, bool force_fast_update /* = false */, U32 use_name)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (!width || !height)
    {
        return true;
    }
    U32 tex_name = use_name != 0 ? use_name : mTexName;
    if (0 == tex_name)
    {
        // *TODO: Re-enable warning?  Ran into thread locking issues? DK 2011-02-18
        //LL_WARNS() << "Setting subimage on image without GL texture" << LL_ENDL;
        return false;
    }
    if (datap == NULL)
    {
        // *TODO: Re-enable warning?  Ran into thread locking issues? DK 2011-02-18
        //LL_WARNS() << "Setting subimage on image with NULL datap" << LL_ENDL;
        return false;
    }

    // HACK: allow the caller to explicitly force the fast path (i.e. using glTexSubImage2D here instead of calling setImage) even when updating the full texture.
    if (!force_fast_update && x_pos == 0 && y_pos == 0 && width == getWidth() && height == getHeight() && data_width == width && data_height == height)
    {
        setImage(datap, false, tex_name);
    }
    else
    {
        if (mUseMipMaps)
        {
            dump();
            LL_ERRS() << "setSubImage called with mipmapped image (not supported)" << LL_ENDL;
        }
        llassert_always(mCurrentDiscardLevel == 0);
        llassert_always(x_pos >= 0 && y_pos >= 0);

        if (((x_pos + width) > getWidth()) ||
            (y_pos + height) > getHeight())
        {
            dump();
            LL_ERRS() << "Subimage not wholly in target image!"
                   << " x_pos " << x_pos
                   << " y_pos " << y_pos
                   << " width " << width
                   << " height " << height
                   << " getWidth() " << getWidth()
                   << " getHeight() " << getHeight()
                   << LL_ENDL;
        }

        if ((x_pos + width) > data_width ||
            (y_pos + height) > data_height)
        {
            dump();
            LL_ERRS() << "Subimage not wholly in source image!"
                   << " x_pos " << x_pos
                   << " y_pos " << y_pos
                   << " width " << width
                   << " height " << height
                   << " source_width " << data_width
                   << " source_height " << data_height
                   << LL_ENDL;
        }




        const U8* sub_datap = datap + (y_pos * data_width + x_pos) * getComponents();
        // Update the GL texture
        bool res = gRender.getTexUnit(0)->bindManual(mBindTarget, tex_name);
        if (!res) LL_ERRS() << "LLImageTex::setSubImage(): bindTexture failed" << LL_ENDL;

        const bool use_sub_image = should_stagger_image_set(isCompressed());
        if (!use_sub_image)
        {
            // *TODO: Why does this work here, in setSubImage, but not in
            // setManualImage? Maybe because it only gets called with the
            // dimensions of the full image?  Or because the image is never
            // compressed?
            LLVkTex::subImage2D(tex_name, mTarget, 0, x_pos, y_pos, width, height, mFormatPrimary, mFormatType, sub_datap, data_width);
        }
        else
        {
            sub_image_lines(tex_name, mTarget, 0, x_pos, y_pos, width, height, mFormatPrimary, mFormatType, sub_datap, data_width);
        }
        gRender.getTexUnit(0)->clear();


        mImageTextureCreated = true;
    }
    return true;
}

bool LLImageTex::setSubImage(const LLImageRaw* imageraw, S32 x_pos, S32 y_pos, S32 width, S32 height, bool force_fast_update /* = false */, U32 use_name)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    return setSubImage(imageraw->getData(), imageraw->getWidth(), imageraw->getHeight(), x_pos, y_pos, width, height, force_fast_update, use_name);
}

// Copy sub image from frame buffer
bool LLImageTex::setSubImageFromFrameBuffer(S32 fb_x, S32 fb_y, S32 x_pos, S32 y_pos, S32 width, S32 height)
{
    if (gRender.getTexUnit(0)->bind(this, false, true))
    {
        LLVkTex::copySubImage(mTexName, TextureTarget::Tex2D, 0, fb_x, fb_y, 0, x_pos, y_pos, width, height, LLRenderTarget::currentId());
        mImageTextureCreated = true;
        return true;
    }
    else
    {
        return false;
    }
}

// static
void LLImageTex::generateTextures(S32 numTextures, U32 *textures)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    static constexpr U32 pool_size = 1024;
    static thread_local U32 name_pool[pool_size]; // pool of texture names
    static thread_local U32 name_count = 0; // number of available names in the pool

    if (name_count == 0)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("iglgt - reup pool");
        // pool is emtpy, refill it
        LLVkTex::create(pool_size, name_pool);
        name_count = pool_size;
    }

    if ((U32)numTextures <= name_count)
    {
        //copy teture names off the end of the pool
        memcpy(textures, name_pool + name_count - numTextures, sizeof(U32) * numTextures);
        name_count -= numTextures;
    }
    else
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("iglgt - pool miss");
        LLVkTex::create(numTextures, textures);
    }
}

// <FS> native-vk (blink after load): the number of frames a dropped texture name waits before it is deleted was 3 (the stock value, for the GPU). About 46 draws per run still named a name that had been deleted
// (a white frame each, counted in VkTexCopy: draw samples of a deleted name), so scene rows and cached draw data can hold a name longer than that. LL_VK_TEX_DELETE_DELAY sets it (default 8, at most 30).
static const int DELETE_DELAY = []{ const char* e = getenv("LL_VK_TEX_DELETE_DELAY"); const int v = e ? atoi(e) : 8; return v < 3 ? 3 : (v > 30 ? 30 : v); }();
static std::vector<U32> sFreeList[31+1];

// static
void LLImageTex::updateClass()
{
    sFrameCount++;

    // wait a few frames before actually deleting the textures to avoid
    // synchronization issues with the GPU
    // (the list that was filled DELETE_DELAY frames ago: (frame + DELAY) mod (DELAY + 1) is the list of the frame before, so the names went one frame after being dropped whatever the constant said)
    U32 idx = (sFrameCount+1) % (DELETE_DELAY+1);

    if (!sFreeList[idx].empty())
    {
        free_tex_images((S32) sFreeList[idx].size(), sFreeList[idx].data());
        LLVkTex::destroy((S32)sFreeList[idx].size(), sFreeList[idx].data());
        sFreeList[idx].resize(0);
    }
}

// static
void LLImageTex::deleteTextures(S32 numTextures, const U32 *textures)
{
    if (gDeviceCaps.mInited)
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
        U32 idx = sFrameCount % (DELETE_DELAY+1);
        for (S32 i = 0; i < numTextures; ++i)
        {
            sFreeList[idx].push_back(textures[i]);
        }
    }
}

// static
// static
void LLImageTex::setManualImage(U32 texname, TextureTarget target, S32 miplevel, TexelFormat intformat, S32 width, S32 height, PixelFormat pixformat, ComponentType pixtype, const void* pixels, bool allow_compression, const char* file, int line)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    LLVkFormat::noteFormat("setManualImage", file, line, (U8)intformat, (U8)pixformat, (U8)pixtype);
    if (LLRender::sGLCoreProfile)
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
        if (gDeviceCaps.mApiVersionNumber >= CONVERSION_SCRATCH_BUFFER_GL_VERSION)
        {
            if (pixformat == PixelFormat::Alpha)
            { //alpha only formats are deprecated, convert to R8 with a swizzle
                const TextureSwizzle mask[] = { TextureSwizzle::Zero, TextureSwizzle::Zero, TextureSwizzle::Zero, TextureSwizzle::R };
                LLVkTex::swizzle(texname, mask);
                pixformat = PixelFormat::R;
                intformat = TexelFormat::R8;
            }

            if (pixformat == PixelFormat::Lum)
            { //luminance is deprecated, convert to R8 with a swizzle
                const TextureSwizzle mask[] = { TextureSwizzle::R, TextureSwizzle::R, TextureSwizzle::R, TextureSwizzle::One };
                LLVkTex::swizzle(texname, mask);
                pixformat = PixelFormat::R;
                intformat = TexelFormat::R8;
            }

            if (pixformat == PixelFormat::LumAlpha)
            { //luminance alpha is deprecated, convert to RG8 with a swizzle
                const TextureSwizzle mask[] = { TextureSwizzle::R, TextureSwizzle::R, TextureSwizzle::R, TextureSwizzle::G };
                LLVkTex::swizzle(texname, mask);
                pixformat = PixelFormat::RG;
                intformat = TexelFormat::RG8;
            }
        }
        else
        {
            if (pixformat == PixelFormat::Alpha && pixtype == ComponentType::U8)
            { //alpha only is deprecated, convert to RGBA
                if (pixels != nullptr)
                {
                    U32 pixel_count = (U32)(width * height);
                    for (U32 i = 0; i < pixel_count; i++)
                    {
                        U8* pix = (U8*)&sManualScratch[i];
                        pix[0] = pix[1] = pix[2] = 0;
                        pix[3] = ((U8*)pixels)[i];
                    }

                    pixels = sManualScratch;
                }

                pixformat = PixelFormat::RGBA;
                intformat = TexelFormat::RGBA8;
            }

            if (pixformat == PixelFormat::LumAlpha && pixtype == ComponentType::U8)
            { //luminance alpha is deprecated, convert to RGBA
                if (pixels != nullptr)
                {
                    U32 pixel_count = (U32)(width * height);
                    for (U32 i = 0; i < pixel_count; i++)
                    {
                        U8 lum = ((U8*)pixels)[i * 2 + 0];
                        U8 alpha = ((U8*)pixels)[i * 2 + 1];

                        U8* pix = (U8*)&sManualScratch[i];
                        pix[0] = pix[1] = pix[2] = lum;
                        pix[3] = alpha;
                    }

                    pixels = sManualScratch;
                }

                pixformat = PixelFormat::RGBA;
                intformat = TexelFormat::RGBA8;
            }

            if (pixformat == PixelFormat::Lum && pixtype == ComponentType::U8)
            { //luminance is deprecated, convert to RGB
                if (pixels != nullptr)
                {
                    U32 pixel_count = (U32)(width * height);
                    for (U32 i = 0; i < pixel_count; i++)
                    {
                        U8 lum = ((U8*)pixels)[i];

                        U8* pix = (U8*)&sManualScratch[i];
                        pix[0] = pix[1] = pix[2] = lum;
                        pix[3] = 255;
                    }

                    pixels = sManualScratch;
                }
                pixformat = PixelFormat::RGBA;
                intformat = TexelFormat::RGB8;
            }
        }
    }

    // F3.6e: texture compression (RenderCompressTextures). The GL way named a generic compressed internal format per component layout; of those the old decode knew only
    // COMPRESSED_RGBA and COMPRESSED_RGB (stored as RGBA8), the other seven were "no Vulkan format" (the upload was dropped). Now: RGBA and RGB are the generic compressed
    // storage, every other format stays as it is.
    const bool compress = LLImageTex::sCompressTextures && allow_compression;
    if (compress && (intformat == TexelFormat::RGBA8 || intformat == TexelFormat::RGB8))
    {
        intformat = TexelFormat::GenericCompressed;
    }

    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("glTexImage2D");
        LL_PROFILE_ZONE_NUM(width);
        LL_PROFILE_ZONE_NUM(height);

        free_cur_tex_image();
        const bool use_sub_image = should_stagger_image_set(compress);
        if (!use_sub_image)
        {
            LL_PROFILE_ZONE_NAMED("glTexImage2D alloc + copy");
            LLVkTex::image2D(texname, target, miplevel, intformat, width, height, pixformat, pixtype, pixels);
        }
        else
        {
            // break up calls to a manageable size for the GL command buffer
            {
                LL_PROFILE_ZONE_NAMED("glTexImage2D alloc");
                LLVkTex::image2D(texname, target, miplevel, intformat, width, height, pixformat, pixtype, nullptr);
            }

            U8* src = (U8*)(pixels);
            if (src)
            {
                LL_PROFILE_ZONE_NAMED("glTexImage2D copy");
                sub_image_lines(texname, target, miplevel, 0, 0, width, height, pixformat, pixtype, src, width);
            }
        }
        alloc_tex_image(width, height, intformat, 1);
    }
}

//create an empty GL texture: just create a texture name
//the texture is assiciate with some image by calling glTexImage outside LLImageTex
bool LLImageTex::createImageTexture()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    checkActiveThread();

    if (gDeviceCaps.mIsDisabled)
    {
        LL_WARNS() << "Trying to create a texture while GL is disabled!" << LL_ENDL;
        return false;
    }

    mImageTextureCreated = false ; //do not save this texture when gl is destroyed.

    llassert(gDeviceCaps.mInited);

    if(mTexName)
    {
        LLImageTex::deleteTextures(1, (reinterpret_cast<U32*>(&mTexName))) ;
        mTexName = 0;
        noteNameChange();
    }


    LLImageTex::generateTextures(1, &mTexName);
    noteNameChange();
    if (!mTexName)
    {
        LL_WARNS() << "LLImageTex::createImageTexture failed to make an empty texture" << LL_ENDL;
        return false;
    }

    return true ;
}

bool LLImageTex::createImageTexture(S32 discard_level, const LLImageRaw* imageraw, S32 usename/*=0*/, bool to_create, S32 category, bool defer_copy, U32* tex_name)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    checkActiveThread();

    if (gDeviceCaps.mIsDisabled)
    {
        LL_WARNS() << "Trying to create a texture while GL is disabled!" << LL_ENDL;
        return false;
    }

    llassert(gDeviceCaps.mInited);

    if (!imageraw || imageraw->isBufferInvalid())
    {
        LL_WARNS() << "Trying to create a texture from invalid image data" << LL_ENDL;
        mImageTextureCreated = false;
        return false;
    }

    if (discard_level < 0)
    {
        llassert(mCurrentDiscardLevel >= 0);
        discard_level = mCurrentDiscardLevel;
    }
    discard_level = llmin(discard_level, MAX_DISCARD_LEVEL);

    // Actual image width/height = raw image width/height * 2^discard_level
    S32 raw_w = imageraw->getWidth() ;
    S32 raw_h = imageraw->getHeight() ;

    S32 w = raw_w << discard_level;
    S32 h = raw_h << discard_level;

    // setSize may call destroyImageTexture if the size does not match
    if (!setSize(w, h, imageraw->getComponents(), discard_level))
    {
        LL_WARNS() << "Trying to create a texture with incorrect dimensions!" << LL_ENDL;
        mImageTextureCreated = false;
        return false;
    }

    if (mHasExplicitFormat &&
        ((mFormatPrimary == PixelFormat::RGBA && mComponents < 4) ||
         (mFormatPrimary == PixelFormat::RGB  && mComponents < 3)))

    {
        LL_WARNS()  << "Incorrect format: " << std::hex << (S32)mFormatPrimary << " components: " << (U32)mComponents <<  LL_ENDL;
        mHasExplicitFormat = false;
    }

    if( !mHasExplicitFormat )
    {
        switch (mComponents)
        {
        case 1:
            // Use luminance alpha (for fonts)
            mFormatInternal = TexelFormat::Lum8;
            mFormatPrimary = PixelFormat::Lum;
            mFormatType = ComponentType::U8;
            break;
        case 2:
            // Use luminance alpha (for fonts)
            mFormatInternal = TexelFormat::LumAlpha8;
            mFormatPrimary = PixelFormat::LumAlpha;
            mFormatType = ComponentType::U8;
            break;
        case 3:
            mFormatInternal = TexelFormat::RGB8;
            mFormatPrimary = PixelFormat::RGB;
            mFormatType = ComponentType::U8;
            break;
        case 4:
            mFormatInternal = TexelFormat::RGBA8;
            mFormatPrimary = PixelFormat::RGBA;
            mFormatType = ComponentType::U8;
            break;
        default:
            LL_ERRS() << "Bad number of components for texture: " << (U32)getComponents() << LL_ENDL;
        }

        calcAlphaChannelOffsetAndStride() ;
    }

    if(!to_create) //not create a gl texture
    {
        destroyImageTexture();
        mCurrentDiscardLevel = discard_level;
        mLastBindTime = sLastFrameTime;
        mImageTextureCreated = false;
        return true ;
    }

    setCategory(category);
    const U8* rawdata = imageraw->getData();
    return createImageTexture(discard_level, rawdata, false, usename, defer_copy, tex_name);
}

bool LLImageTex::createImageTexture(S32 discard_level, const U8* data_in, bool data_hasmips, S32 usename, bool defer_copy, U32* tex_name)
// Call with void data, vmem is allocated but unitialized
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    LL_PROFILE_GPU_ZONE("createImageTexture");
    checkActiveThread();

    bool main_thread = on_main_thread();

    if (defer_copy)
    {
        data_in = nullptr;
    }
    else
    {
        llassert(data_in);
    }


    if (discard_level < 0)
    {
        llassert(mCurrentDiscardLevel >= 0);
        discard_level = mCurrentDiscardLevel;
    }
    discard_level = llclamp(discard_level, 0, (S32)mMaxDiscardLevel);
    discard_level = llmin(discard_level, MAX_DISCARD_LEVEL);

    if (main_thread // <--- always force creation of new_texname when not on main thread ...
        && !defer_copy // <--- ... or defer copy is set
        && mTexName != 0 && discard_level == mCurrentDiscardLevel)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("cglt - early setImage");
        // This will only be true if the size has not changed
        if (tex_name != nullptr)
        {
            *tex_name = mTexName;
        }
        return setImage(data_in, data_hasmips);
    }

    U32 new_texname = 0;
    if (usename != 0)
    {
        llassert(main_thread);
        new_texname = usename;
    }
    else
    {
        LLImageTex::generateTextures(1, &new_texname);
        {
            gRender.getTexUnit(0)->bind(this, false, false, new_texname);
            const LLVkFormat::Setting levels[2] = { LLVkFormat::setBaseLevel(0), LLVkFormat::setMaxLevel(mMaxDiscardLevel - discard_level) };
            LLVkTex::params(new_texname, levels, 2);
        }
    }

    if (tex_name != nullptr)
    {
        *tex_name = new_texname;
    }

    if (mUseMipMaps)
    {
        mAutoGenMips = true;
    }

    mCurrentDiscardLevel = discard_level;

    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("cglt - late setImage");
        if (!setImage(data_in, data_hasmips, new_texname))
        {
            return false;
        }
    }

    // Set texture options to our defaults.
    gRender.getTexUnit(0)->setHasMipMaps(mHasMipMaps);
    gRender.getTexUnit(0)->setTextureAddressMode(mAddressMode);
    gRender.getTexUnit(0)->setTextureFilteringOption(mFilterOption);

    // things will break if we don't unbind after creation
    gRender.getTexUnit(0)->unbind(mBindTarget);

    //if we're on the image loading thread, the new image takes its place on the main thread (syncTexName)
    if (!defer_copy)
    {
        if (!main_thread)
        {
            syncToMainThread(new_texname);
        }
        else
        {
            //not on background thread: the image takes its place at once (a texture that has an image keeps its name: see syncTexName)
            syncTexName(new_texname);
        }
    }


    mTextureMemory = (S64Bytes)getMipBytes(mCurrentDiscardLevel);

    // mark this as bound at this point, so we don't throw it out immediately
    mLastBindTime = sLastFrameTime;

    checkActiveThread();
    return true;
}

void LLImageTex::syncToMainThread(U32 new_tex_name)
{
    LL_PROFILE_ZONE_SCOPED;
    llassert(!on_main_thread());

    // The uploads this thread issued are in the command stream before the main thread is told about the new name (below), and the stream is
    // replayed in order: nothing has to wait for them (no fences).

    ref();
    LL::WorkQueue::postMaybe(
        mMainQueue,
        [=, this]()
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("cglt - delete callback");
            syncTexName(new_tex_name);
            unref();
        });

}


// <FS> native-vk (texture handles H2): the texture's name is its id for as long as it has a GL texture, so a new resolution does not make a new name any more. The loader thread prepared the image under
// a name of its own (`texname`); here, on the main thread, it takes the place of the image of mTexName (LLVkTex::commitImage; `texname` goes with it). A texture without a name yet simply takes
// `texname` as its name (the first image).
void LLImageTex::syncTexName(U32 texname)
{
    if (texname != 0)
    {
        if (mTexName != 0 && mTexName != texname)
        {
            LLVkTex::commitImage(mTexName, texname);
        }
        else
        {
            mTexName = texname;
        }
        noteNameChange();
        markTexOptionsDirty();
    }
}

bool LLImageTex::readBackRaw(S32 discard_level, LLImageRaw* imageraw, bool compressed_ok) const
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    if (discard_level < 0)
    {
        discard_level = mCurrentDiscardLevel;
    }

    if (mTexName == 0 || discard_level < mCurrentDiscardLevel || discard_level > mMaxDiscardLevel )
    {
        return false;
    }

    S32 gl_discard = discard_level - mCurrentDiscardLevel;

    //explicitly unbind texture
    gRender.getTexUnit(0)->unbind(mBindTarget);
    llverify(gRender.getTexUnit(0)->bindManual(mBindTarget, mTexName));

    //debug code, leave it there commented.
    //checkTexSize() ;

    S32 glwidth = 0;
    glwidth = LLVkTex::levelParameter(mTexName, mTarget, gl_discard, LevelQuery::Width);
    if (glwidth == 0)
    {
        // No mip data smaller than current discard level
        return false;
    }

    S32 width = getWidth(discard_level);
    S32 height = getHeight(discard_level);
    S32 ncomponents = getComponents();
    if (ncomponents == 0)
    {
        return false;
    }
    if(width < glwidth)
    {
        LL_WARNS() << "texture size is smaller than it should be." << LL_ENDL ;
        LL_WARNS() << "width: " << width << " glwidth: " << glwidth << " mWidth: " << mWidth <<
            " mCurrentDiscardLevel: " << (S32)mCurrentDiscardLevel << " discard_level: " << (S32)discard_level << LL_ENDL ;
        return false ;
    }

    if (width <= 0 || width > 2048 || height <= 0 || height > 2048 || ncomponents < 1 || ncomponents > 4)
    {
        LL_ERRS() << llformat("LLImageTex::readBackRaw: bogus params: %d x %d x %d",width,height,ncomponents) << LL_ENDL;
    }

    S32 is_compressed = 0;
    if (compressed_ok)
    {
        is_compressed = LLVkTex::levelParameter(mTexName, mTarget, gl_discard, LevelQuery::Compressed);
    }

    LLImageDataLock lock(imageraw);

    if (is_compressed)
    {
        S32 glbytes;
        glbytes = LLVkTex::levelParameter(mTexName, mTarget, gl_discard, LevelQuery::CompressedSize);
        if(!imageraw->allocateDataSize(width, height, ncomponents, glbytes))
        {
            constexpr S64 MAX_GL_BYTES = 2048 * 2048;
            if (glbytes > 0 && glbytes <= MAX_GL_BYTES)
            {
                LLError::LLUserWarningMsg::showOutOfMemory();
                LL_ERRS() << "Memory allocation failed for reading back texture. Data size: " << glbytes << LL_ENDL;
            }
            else
            {
                LL_WARNS() << "Memory allocation failed for reading back texture. Data size is: " << glbytes << LL_ENDL;
                LL_WARNS() << "width: " << width << "height: " << height << "components: " << ncomponents << LL_ENDL;
            }
            return false ;
        }

        LLVkTex::getCompressedImage(mTexName, mTarget, gl_discard, (void*)(imageraw->getData()));
    }
    else
    {
        if(!imageraw->allocateDataSize(width, height, ncomponents))
        {
            constexpr F32 MAX_IMAGE_SIZE = 2048 * 2048;
            F32 size = (F32)width * (F32)height * (F32)ncomponents;
            if (size > 0 && size <= MAX_IMAGE_SIZE)
            {
                LLError::LLUserWarningMsg::showOutOfMemory();
                LL_ERRS() << "Memory allocation failed for reading back texture. Data size: " << size << LL_ENDL;
            }
            else
            {
                LL_WARNS() << "Memory allocation failed for reading back texture." << LL_ENDL;
                LL_WARNS() << "width: " << width << "height: " << height << "components: " << ncomponents << LL_ENDL;
            }
            return false ;
        }

        LLVkTex::getImage(mTexName, TextureTarget::Tex2D, gl_discard, mFormatPrimary, mFormatType, (void*)(imageraw->getData()));
    }

    return true ;
}

void LLImageTex::destroyImageTexture()
{
    checkActiveThread();

    if (mTexName != 0)
    {
        if(mTextureMemory != S64Bytes(0))
        {
            mTextureMemory = (S64Bytes)0;
        }

        LLImageTex::deleteTextures(1, &mTexName);
        mCurrentDiscardLevel = -1 ; //invalidate mCurrentDiscardLevel.
        mTexName = 0;
        noteNameChange();
        mImageTextureCreated = false ;
    }
}

//force to invalidate the gl texture, most likely a sculpty texture
void LLImageTex::forceToInvalidateGLTexture()
{
    checkActiveThread();
    mAnalyzedPixels = 0;        // (new content: the classification starts over)
    if (mTexName != 0)
    {
        destroyImageTexture();
    }
    else
    {
        mCurrentDiscardLevel = -1 ; //invalidate mCurrentDiscardLevel.
    }
}

//----------------------------------------------------------------------------

void LLImageTex::setAddressMode(LLTexUnit::eTextureAddressMode mode)
{
    if (mAddressMode != mode)
    {
        markTexOptionsDirty();
        mAddressMode = mode;
    }

    if (mTexName != 0)
    {
        // the sampler parameters belong to the texture: no texture unit is involved (they used to be applied only when the image was bound on the active unit)
        gRender.flush();
        LLTexUnit::applyAddressMode(mTexName, getTarget(), mode);
        mTexOptionsDirty = false;
    }
}

void LLImageTex::setFilteringOption(LLTexUnit::eTextureFilterOptions option)
{
    if (mFilterOption != option)
    {
        markTexOptionsDirty();
        mFilterOption = option;
    }

    if (mTexName != 0)
    {
        gRender.flush();
        LLTexUnit::applyFilteringOption(mTexName, getTarget(), option, mHasMipMaps);
        mTexOptionsDirty = false;
    }
}

bool LLImageTex::getIsResident(bool test_now)
{
    if (test_now)
    {
        if (mTexName != 0)
        {
            mIsResident = true;     // every texture the viewer has is resident
        }
        else
        {
            mIsResident = false;
        }
    }

    return mIsResident;
}

S32 LLImageTex::getHeight(S32 discard_level) const
{
    if (discard_level < 0)
    {
        discard_level = mCurrentDiscardLevel;
    }
    S32 height = mHeight >> discard_level;
    if (height < 1) height = 1;
    return height;
}

S32 LLImageTex::getWidth(S32 discard_level) const
{
    if (discard_level < 0)
    {
        discard_level = mCurrentDiscardLevel;
    }
    S32 width = mWidth >> discard_level;
    if (width < 1) width = 1;
    return width;
}

S64 LLImageTex::getBytes(S32 discard_level) const
{
    if (discard_level < 0)
    {
        discard_level = mCurrentDiscardLevel;
    }
    S32 w = mWidth>>discard_level;
    S32 h = mHeight>>discard_level;
    if (w == 0) w = 1;
    if (h == 0) h = 1;
    return formatBytes(w, h);
}

S64 LLImageTex::getMipBytes(S32 discard_level) const
{
    if (discard_level < 0)
    {
        discard_level = mCurrentDiscardLevel;
    }
    S32 w = mWidth>>discard_level;
    S32 h = mHeight>>discard_level;
    S64 res = formatBytes(w, h);
    if (mUseMipMaps)
    {
        while (w > 1 && h > 1)
        {
            w >>= 1; if (w == 0) w = 1;
            h >>= 1; if (h == 0) h = 1;
            res += formatBytes(w, h);
        }
    }
    return res;
}

bool LLImageTex::isJustBound() const
{
    return sLastFrameTime - mLastBindTime < 0.5f;
}

bool LLImageTex::getBoundRecently() const
{
    return (bool)(sLastFrameTime - mLastBindTime < MIN_TEXTURE_LIFETIME);
}

bool LLImageTex::getIsAlphaMask() const
{
    llassert_always(!sSkipAnalyzeAlpha);
    return mIsMask;
}

void LLImageTex::setTarget(const TextureTarget target, const LLTexUnit::eTextureType bind_target)
{
    mTarget = target;
    mBindTarget = bind_target;
}

const S8 INVALID_OFFSET = -99 ;
void LLImageTex::setNeedsAlphaAndPickMask(bool need_mask)
{
    if(mNeedsAlphaAndPickMask != need_mask)
    {
        mNeedsAlphaAndPickMask = need_mask;
        mAnalyzedPixels = 0;

        if(mNeedsAlphaAndPickMask)
        {
            mAlphaOffset = 0 ;
        }
        else //do not need alpha mask
        {
            mAlphaOffset = INVALID_OFFSET ;
            mIsMask = false;
        }
    }
}

void LLImageTex::calcAlphaChannelOffsetAndStride()
{
    if(mAlphaOffset == INVALID_OFFSET)//do not need alpha mask
    {
        return ;
    }

    mAlphaStride = -1 ;
    switch (mFormatPrimary)
    {
    case PixelFormat::Lum:
    case PixelFormat::Alpha:
        mAlphaStride = 1;
        break;
    case PixelFormat::LumAlpha:
        mAlphaStride = 2;
        break;
    case PixelFormat::R:
    case PixelFormat::RGB:
        mNeedsAlphaAndPickMask = false;
        mIsMask = false;
        return; //no alpha channel.
    case PixelFormat::RGBA:
    case PixelFormat::BGRA:
        mAlphaStride = 4;
        break;
    default:
        break;
    }

    // 8 bit components: the alpha channel is the last one. (The packed 8-8-8-8 types of the media plugins arrive as 8 bit components with BGRA, see componentTypeFromGL.)
    mAlphaOffset = -1 ;
    if (mFormatType == ComponentType::U8)
    {
        mAlphaOffset = mAlphaStride - 1 ;
    }

    if( mAlphaStride < 1 || //unsupported format
        mAlphaOffset < 0 || //unsupported type
        (mFormatPrimary == PixelFormat::BGRA && mFormatType != ComponentType::U8)) //unknown situation
    {
        LL_WARNS() << "Cannot analyze alpha for image with format type " << std::hex << (S32)mFormatType << std::dec << LL_ENDL;

        mNeedsAlphaAndPickMask = false ;
        mIsMask = false;
    }
}

void LLImageTex::analyzeAlpha(const void* data_in, U32 w, U32 h)
{
    if(!data_in || sSkipAnalyzeAlpha || !mNeedsAlphaAndPickMask)
    {
        return ;
    }

    // <FS> native-vk (blink after load): the mask or not decision made from a bigger level stands when a smaller one is uploaded (a reload after a downgrade climbs through the small levels, and
    // each of them could turn a cut-out into a blend and back, so the surface changed draw pass at every step). A same size or bigger upload decides again, as before.
    static const bool alpha_keep = []{ const char* e = getenv("LL_VK_ALPHA_KEEP"); return !e || atoi(e) != 0; }();
    if (alpha_keep && w * h < mAnalyzedPixels)
    {
        return;
    }
    mAnalyzedPixels = w * h;

    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    U32 length = w * h;
    U32 alphatotal = 0;

    U32 sample[16];
    memset(sample, 0, sizeof(U32)*16);

    // generate histogram of quantized alpha.
    // also add-in the histogram of a 2x2 box-sampled version.  The idea is
    // this will mid-skew the data (and thus increase the chances of not
    // being used as a mask) from high-frequency alpha maps which
    // suffer the worst from aliasing when used as alpha masks.
    if (w >= 2 && h >= 2)
    {
        llassert(w % 2 == 0);
        llassert(h % 2 == 0);
        const U8* rowstart = ((const U8*) data_in) + mAlphaOffset;
        for (U32 y = 0; y < h; y += 2)
        {
            const U8* current = rowstart;
            for (U32 x = 0; x < w; x += 2)
            {
                const U32 s1 = current[0];
                alphatotal += s1;
                const U32 s2 = current[w * mAlphaStride];
                alphatotal += s2;
                current += mAlphaStride;
                const U32 s3 = current[0];
                alphatotal += s3;
                const U32 s4 = current[w * mAlphaStride];
                alphatotal += s4;
                current += mAlphaStride;

                ++sample[s1/16];
                ++sample[s2/16];
                ++sample[s3/16];
                ++sample[s4/16];

                const U32 asum = (s1+s2+s3+s4);
                alphatotal += asum;
                sample[asum/(16*4)] += 4;
            }

            rowstart += 2 * w * mAlphaStride;
        }
        length *= 2; // we sampled everything twice, essentially
    }
    else
    {
        const U8* current = ((const U8*) data_in) + mAlphaOffset;
        for (U32 i = 0; i < length; i++)
        {
            const U32 s1 = *current;
            alphatotal += s1;
            ++sample[s1/16];
            current += mAlphaStride;
        }
    }

    // if more than 1/16th of alpha samples are mid-range, this
    // shouldn't be treated as a 1-bit mask

    // also, if all of the alpha samples are clumped on one half
    // of the range (but not at an absolute extreme), then consider
    // this to be an intentional effect and don't treat as a mask.

    U32 midrangetotal = 0;
    for (U32 i = 2; i < 13; i++)
    {
        midrangetotal += sample[i];
    }
    U32 lowerhalftotal = 0;
    for (U32 i = 0; i < 8; i++)
    {
        lowerhalftotal += sample[i];
    }
    U32 upperhalftotal = 0;
    for (U32 i = 8; i < 16; i++)
    {
        upperhalftotal += sample[i];
    }

    const bool was_mask = mIsMask;
    if (midrangetotal > length/48 || // lots of midrange, or
        (lowerhalftotal == length && alphatotal != 0) || // all close to transparent but not all totally transparent, or
        (upperhalftotal == length && alphatotal != 255*length)) // all close to opaque but not all totally opaque
    {
        mIsMask = false; // not suitable for masking
    }
    else
    {
        mIsMask = true;
    }

    {
        // <FS> native-vk diagnostic (blink after load): how often the mask classification changes from one uploaded level to the next
        static U64 analyses = 0, flips = 0, flips_seen = 0, real_flips = 0, first_mask = 0, first_total = 0;
        static LLTimer log_timer;
        ++analyses;
        ++mAnalysisCount;
        if (mAnalysisCount == 1)
        {
            ++first_total;
            first_mask += mIsMask ? 1 : 0;
        }
        else if (was_mask != mIsMask)
        {
            ++real_flips;
            if (real_flips <= 800)
            {
                // every real flip: which texture, which way, at which size, the nth analysis of it (a texture that flips back and forth moves its faces between the mask and the blend pools)
                LL_INFOS("VkAlphaFlip") << "texture " << mTexId << " " << was_mask << " -> " << mIsMask << " at " << w << "x" << h << " analysis " << mAnalysisCount << " (mid-range " << midrangetotal << " of " << length << ")" << LL_ENDL;
            }
        }
        if (was_mask != mIsMask)
        {
            ++flips;
            if (flips_seen++ < 30)
            {
                LL_INFOS("VkAlpha") << "alpha mask flag of texture " << mTexId << " changed " << was_mask << " -> " << mIsMask << " at " << w << "x" << h << " (mid-range " << midrangetotal << " of " << length << ")" << LL_ENDL;
            }
        }
        if (log_timer.getElapsedTimeF32() >= 10.f)
        {
            log_timer.reset();
            LL_INFOS("VkAlpha") << "alpha analyses " << analyses << ", mask flag changes " << flips << " (totals so far); first analyses " << first_total << " (" << first_mask << " masks), flips after the first analysis " << real_flips << LL_ENDL;
        }
    }
}

//----------------------------------------------------------------------------
U32 LLImageTex::createPickMask(S32 pWidth, S32 pHeight)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    freePickMask();
    U32 pick_width = pWidth/2 + 1;
    U32 pick_height = pHeight/2 + 1;

    U32 size = pick_width * pick_height;
    size = (size + 7) / 8; // pixelcount-to-bits
    mPickMask = new U8[size];
    mPickMaskWidth = pick_width - 1;
    mPickMaskHeight = pick_height - 1;

    memset(mPickMask, 0, sizeof(U8) * size);

    return size;
}

//----------------------------------------------------------------------------
void LLImageTex::freePickMask()
{
    if (mPickMask != NULL)
    {
        delete [] mPickMask;
    }
    mPickMask = NULL;
    mPickMaskWidth = mPickMaskHeight = 0;
}

bool LLImageTex::isCompressed()
{
    // *NOTE: Not all compressed formats are included here (the S3TC family). The GL code asked the PRIMARY format for this (the caller passed the compressed token as both formats);
    // nothing in the viewer does that, the typed way asks the storage format.
    return is_s3tc(mFormatInternal);
}

S64 LLImageTex::formatBytes(S32 w, S32 h) const
{
    return is_s3tc(mFormatInternal) ? dataFormatBytes(mFormatInternal, w, h) : dataFormatBytes(mFormatPrimary, w, h);
}

//----------------------------------------------------------------------------
void LLImageTex::updatePickMask(S32 width, S32 height, const U8* data_in)
{
    if(!mNeedsAlphaAndPickMask)
    {
        return ;
    }

    if (mFormatType != ComponentType::U8 ||
        (mFormatPrimary != PixelFormat::RGBA))
    {
        //cannot generate a pick mask for this texture
        freePickMask();
        return;
    }


#ifdef SHOW_ASSERT
    const U32 pickSize = createPickMask(width, height);
#else // SHOW_ASSERT
    createPickMask(width, height);
#endif // SHOW_ASSERT

    U32 pick_bit = 0;

    for (S32 y = 0; y < height; y += 2)
    {
        for (S32 x = 0; x < width; x += 2)
        {
            U8 alpha = data_in[(y*width+x)*4+3];

            if (alpha > 32)
            {
                U32 pick_idx = pick_bit/8;
                U32 pick_offset = pick_bit%8;
                llassert(pick_idx < pickSize);

                mPickMask[pick_idx] |= 1 << pick_offset;
            }

            ++pick_bit;
        }
    }
}

//bool LLImageTex::getMask(const LLVector2 &tc)
// [RLVa:KB] - Checked: RLVa-2.2 (@setoverlay)
bool LLImageTex::getMask(const LLVector2 &tc) const
// [/RLVa:KB]
{
    bool res = true;

    if (mPickMask)
    {
        F32 u,v;
        if (LL_LIKELY(tc.isFinite()))
        {
            u = tc.mV[0] - floorf(tc.mV[0]);
            v = tc.mV[1] - floorf(tc.mV[1]);
        }
        else
        {
            LL_WARNS_ONCE("render") << "Ugh, non-finite u/v in mask pick" << LL_ENDL;
            u = v = 0.f;
            // removing assert per EXT-4388
            // llassert(false);
        }

        if (LL_UNLIKELY(u < 0.f || u > 1.f ||
                v < 0.f || v > 1.f))
        {
            LL_WARNS_ONCE("render") << "Ugh, u/v out of range in image mask pick" << LL_ENDL;
            u = v = 0.f;
            // removing assert per EXT-4388
            // llassert(false);
        }

        S32 x = llfloor(u * mPickMaskWidth);
        S32 y = llfloor(v * mPickMaskHeight);

        if (LL_UNLIKELY(x > mPickMaskWidth))
        {
            LL_WARNS_ONCE("render") << "Ooh, width overrun on pick mask read, that coulda been bad." << LL_ENDL;
            x = llmax((U16)0, mPickMaskWidth);
        }
        if (LL_UNLIKELY(y > mPickMaskHeight))
        {
            LL_WARNS_ONCE("render") << "Ooh, height overrun on pick mask read, that woulda been bad." << LL_ENDL;
            y = llmax((U16)0, mPickMaskHeight);
        }

        S32 idx = y*mPickMaskWidth+x;
        S32 offset = idx%8;

        res = (mPickMask[idx/8] & (1 << offset)) != 0;
    }

    return res;
}

void LLImageTex::setCurTexSizebar(S32 index, bool set_pick_size)
{
    sCurTexSizeBar = index ;

    if(set_pick_size)
    {
        sCurTexPickSize = (1 << index) ;
    }
    else
    {
        sCurTexPickSize = -1 ;
    }
}
void LLImageTex::resetCurTexSizebar()
{
    sCurTexSizeBar = -1 ;
    sCurTexPickSize = -1 ;
}

bool LLImageTex::scaleDown(S32 desired_discard)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    if (mTarget != TextureTarget::Tex2D
        || mFormatInternal == TexelFormat::None // not initialized
        )
    {
        return false;
    }

    desired_discard = llmin(desired_discard, mMaxDiscardLevel);

    if (desired_discard <= mCurrentDiscardLevel)
    {
        return false;
    }

    S32 mip = desired_discard - mCurrentDiscardLevel;

    S32 desired_width = getWidth(desired_discard);
    S32 desired_height = getHeight(desired_discard);

    // <FS> native-vk: a texture with a mip chain gives up its top levels in the worker (a copy of the levels below into a smaller image). A texture without mip maps is not shrunk here (the draw into a
    // target and the copy back left white textures, and no texture the viewer shrinks lacks them): false, and the viewer deals with it as with any texture that cannot be made smaller.
    if (mHasMipMaps && mip > 0)
    {
        free_tex_image(mTexName);
        LLVkTex::dropLevels(mTexName, mip);
        alloc_tex_image(desired_width, desired_height, mFormatInternal, 1);
        markTexOptionsDirty();
        mCurrentDiscardLevel = desired_discard;
        return true;
    }
    return false;
}


//----------------------------------------------------------------------------
#if LL_IMAGEGL_THREAD_CHECK
void LLImageTex::checkActiveThread()
{
    llassert(mActiveThread == LLThread::currentID());
}
#endif

//----------------------------------------------------------------------------


// Manual Mip Generation
/*
        S32 width = getWidth(discard_level);
        S32 height = getHeight(discard_level);
        S32 w = width, h = height;
        S32 nummips = 1;
        while (w > 4 && h > 4)
        {
            w >>= 1; h >>= 1;
            nummips++;
        }
        w = width, h = height;
        const U8* prev_mip_data = 0;
        const U8* cur_mip_data = 0;
        for (int m=0; m<nummips; m++)
        {
            if (m==0)
            {
                cur_mip_data = rawdata;
            }
            else
            {
                S32 bytes = w * h * mComponents;
                U8* new_data = new U8[bytes];
                LLImageBase::generateMip(prev_mip_data, new_data, w, h, mComponents);
                cur_mip_data = new_data;
            }
            llassert(w > 0 && h > 0 && cur_mip_data);
            U8 test = cur_mip_data[w*h*mComponents-1];
            {
                LLImageTex::setManualImage(edit_name, mTarget, m, mFormatInternal, w, h, mFormatPrimary, mFormatType, cur_mip_data);
            }
            if (prev_mip_data && prev_mip_data != rawdata)
            {
                delete prev_mip_data;
            }
            prev_mip_data = cur_mip_data;
            w >>= 1;
            h >>= 1;
        }
        if (prev_mip_data && prev_mip_data != rawdata)
        {
            delete prev_mip_data;
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,  nummips);
*/

LLImageTexThread::LLImageTexThread(LLWindow* window)
    // We want exactly one thread.
    : LL::ThreadPool("LLImageTex", 1)
    , mWindow(window)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    mFinished = false;

    mContext = mWindow->createSharedContext();

    // <FS:ND> If context creating is not supported (SDL1), mark texture thread disabled and exit
    if( !mContext )
    {
        sEnabledTextures = false;
        mFinished = true;
        return;
    }
    // </FS:ND>

    LL::ThreadPool::start();
}

void LLImageTexThread::run()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    // We must perform setup on this thread before actually servicing our
    // WorkQueue, likewise cleanup afterwards.
    mWindow->makeContextCurrent(mContext);
    gRender.init(false);
    LL::ThreadPool::run();
    gRender.shutdown();
    mWindow->destroySharedContext(mContext);
}

