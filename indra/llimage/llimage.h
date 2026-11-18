/**
 * @file llimage.h
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

#ifndef LL_LLIMAGE_H
#define LL_LLIMAGE_H

#include "llmutex.h"
#include "llpointer.h"
#include "llstring.h"
#include "lltrace.h"
#include "lluuid.h"

#include <atomic> // <FS:Beq/> [FIRE-36494] Small memory allocation fail bugsplat reporting improvement
#include <vector>

constexpr S32 MIN_IMAGE_MIP =  2; // 4x4, only used for expand/contract power of 2
constexpr S32 MAX_IMAGE_MIP = 12; // 4096x4096

// *TODO : Use MAX_IMAGE_MIP as max discard level and modify j2c management so that the number
// of levels is read from the header's file, not inferred from its size.
constexpr S32 MAX_DISCARD_LEVEL = 5;

// JPEG2000 size constraints
// Those are declared here as they are germane to other image constraints used in the viewer
// and declared right here. Some come from the JPEG2000 spec, some conventions specific to SL.
constexpr S32 MAX_DECOMPOSITION_LEVELS = 32;    // Number of decomposition levels cannot exceed 32 according to jpeg2000 spec
constexpr S32 MIN_DECOMPOSITION_LEVELS = 5;     // the SL viewer will *crash* trying to decode images with fewer than 5 decomposition levels (unless image is small that is)
constexpr S32 MAX_PRECINCT_SIZE = 4096;         // No reason to be bigger than MAX_IMAGE_SIZE
constexpr S32 MIN_PRECINCT_SIZE = 4;            // Can't be smaller than MIN_BLOCK_SIZE
constexpr S32 MAX_BLOCK_SIZE = 64;              // Max total block size is 4096, hence 64x64 when using square blocks
constexpr S32 MIN_BLOCK_SIZE = 4;               // Min block dim is 4 according to jpeg2000 spec
constexpr S32 MIN_LAYER_SIZE = 2000;            // Size of the first quality layer (after header). Must be > to FIRST_PACKET_SIZE!!
constexpr S32 MAX_NB_LAYERS = 64;               // Max number of layers we'll entertain in SL (practical limit)

constexpr S32 MIN_IMAGE_SIZE = (1<<MIN_IMAGE_MIP); // 4, only used for expand/contract power of 2
constexpr S32 MAX_IMAGE_SIZE = (1<<MAX_IMAGE_MIP); // 4096
constexpr S32 MIN_IMAGE_AREA = MIN_IMAGE_SIZE * MIN_IMAGE_SIZE;
constexpr S32 MAX_IMAGE_AREA = MAX_IMAGE_SIZE * MAX_IMAGE_SIZE;
constexpr S32 MAX_IMAGE_COMPONENTS = 8;
constexpr S32 MAX_IMAGE_DATA_SIZE = MAX_IMAGE_AREA * MAX_IMAGE_COMPONENTS; //4096 * 4096 * 8 = 128 MB

// <FS:Beq> [FIRE-36494] Image allocation failure telemetry
struct LLImageAllocationFailureInfo
{
    S32 mRequestedSize = 0;
    S32 mWidth = 0;
    S32 mHeight = 0;
    S32 mComponents = 0;
    S32 mErrno = 0;
};
// </FS:Beq>

// Note!  These CANNOT be changed without modifying simulator code
// *TODO: change both to 1024 when SIM texture fetching is deprecated
constexpr S32 FIRST_PACKET_SIZE = 600;
constexpr S32 MAX_IMG_PACKET_SIZE = 1000;
constexpr S32 HTTP_PACKET_SIZE = 1496;

// Base classes for images.
// There are two major parts for the image:
// The compressed representation, and the decompressed representation.

class LLImageFormatted;
class LLImageRaw;
class LLColor4U;
class LLColor3;

typedef enum e_image_codec
{
    IMG_CODEC_INVALID  = 0,
    IMG_CODEC_RGB  = 1,
    IMG_CODEC_J2C  = 2,
    IMG_CODEC_BMP  = 3,
    IMG_CODEC_TGA  = 4,
    IMG_CODEC_JPEG = 5,
    IMG_CODEC_DXT  = 6,
    IMG_CODEC_PNG  = 7,
    IMG_CODEC_EOF  = 8
} EImageCodec;

//============================================================================
// library initialization class
// LLImage is frequently used in threads so do not convert it to LLSingleton

class LLImage
{
public:
    static void initClass(bool use_new_byte_range = false, S32 minimal_reverse_byte_range_percent = 75);
    static void cleanupClass();

    static const std::string& getLastThreadError();
    static void setLastError(const std::string& message);

    static bool useNewByteRange() { return sUseNewByteRange; }
    static S32  getReverseByteRangePercent() { return sMinimalReverseByteRangePercent; }

protected:
    static thread_local std::string sLastThreadErrorMessage;
    static bool sUseNewByteRange;
    static S32  sMinimalReverseByteRangePercent;
};

//============================================================================
// Image base class

class LLImageBase
:   public LLThreadSafeRefCount
{
protected:
    virtual ~LLImageBase();

    virtual void deleteData();
    virtual U8* allocateData(S32 size = -1);
    virtual U8* reallocateData(S32 size = -1);

public:
    LLImageBase();

    enum
    {
        TYPE_NORMAL = 0,
        TYPE_AVATAR_BAKE = 1,
    };

    virtual void dump();
    virtual void sanityCheck();

    U16 getWidth() const        { return mWidth; }
    U16 getHeight() const       { return mHeight; }
    S8  getComponents() const   { return mComponents; }
    S32 getDataSize() const     { return mDataSize; }

    const U8 *getData() const   ;
    U8 *getData()               ;
    bool isBufferInvalid() const;

    void setSize(S32 width, S32 height, S32 ncomponents);
    U8* allocateDataSize(S32 width, S32 height, S32 ncomponents, S32 size = -1); // setSize() + allocateData()
    void enableOverSize() {mAllowOverSize = true ;}
    void disableOverSize() {mAllowOverSize = false; }

protected:
    // special accessor to allow direct setting of mData and mDataSize by LLImageFormatted
    void setDataAndSize(U8 *data, S32 size);

public:
    static void generateMip(const U8 *indata, U8* mipdata, int width, int height, S32 nchannels);

    // <FS:Beq> [FIRE-36494] Image allocation failure telemetry
    using allocation_failure_callback_t = void (*)(const LLImageAllocationFailureInfo&);
    static void setAllocationFailureCallback(allocation_failure_callback_t callback);
    static void forceNextAllocationFailureForTesting();
    // Process-lifetime count of allocation failures strictly larger than 4 MiB.
    static U64 getNonFatalAllocationFailureCount();
    static S64 getGlobalDataBytes();            // <FS> diagnostic (ImgMem): the bytes of image data held by every LLImageBase now
    // </FS:Beq>

    // Function for calculating the download priority for textures
    // <= 0 priority means that there's no need for more data.
    static F32 calc_download_priority(F32 virtual_size, F32 visible_area, S32 bytes_sent);

    static EImageCodec getCodecFromExtension(const std::string& exten);

    //static LLTrace::MemStatHandle sMemStat;

private:
    U8 *mData;
    S32 mDataSize;

    U16 mWidth;
    U16 mHeight;

    S8 mComponents;

    bool mBadBufferAllocation;
    bool mAllowOverSize;

    // <FS:Beq> [FIRE-36494] Image allocation failure telemetry
    // Registration can occur after image worker threads have started.
    static std::atomic<allocation_failure_callback_t> sAllocationFailureCallback;
    static thread_local bool sForceAllocationFailureForTesting;
    static std::atomic<U64> sNonFatalAllocationFailureCount;
    static std::atomic<S64> sGlobalDataBytes;       // <FS> (2026-11-14, a diagnostic for the system memory spikes at a login and a teleport) the bytes of image data held by every LLImageBase right now (raw and formatted together)
    static constexpr S32 MAX_FATAL_ALLOCATION_SIZE = 4 * 1024 * 1024; // 4 MiB; shared by allocation and reallocation diagnostics.
    // </FS:Beq>
private:
    mutable LLSharedMutex mDataMutex;

    // <FS> native-vk: the render thread reads the pixels of an image that was handed over to it (see RKImageTex::PixelHandover) after the call that
    // handed them over has returned. Whatever takes the exclusive data lock (every call that reallocates, resizes or writes the pixels) first waits
    // until the render thread has let go. Shared locks (readers) do not wait.
    mutable std::atomic<S32> mPixelHolds{ 0 };
    static void (*sPixelHoldWait)(const LLImageBase*);

public:
    void addPixelHold() const { mPixelHolds.fetch_add(1, std::memory_order_acq_rel); }
    void releasePixelHold() const { mPixelHolds.fetch_sub(1, std::memory_order_acq_rel); }
    static void setPixelHoldWait(void (*fn)(const LLImageBase*)) { sPixelHoldWait = fn; }
    void waitForPixelHolds() const
    {
        if (mPixelHolds.load(std::memory_order_acquire) > 0 && sPixelHoldWait)
        {
            sPixelHoldWait(this);
        }
    }

    template<bool SHARED>
    class DataLock : public LLSharedMutexLockTemplate<SHARED>
    {
    public:
        DataLock(const LLImageBase* image)
        : LLSharedMutexLockTemplate<SHARED>(prepare(image))
        {
        }
    private:
        static LLSharedMutex* prepare(const LLImageBase* image)
        {
            if (!image)
            {
                return nullptr;
            }
            if (!SHARED)
            {
                image->waitForPixelHolds();
            }
            return &image->mDataMutex;
        }
    };
public:
    // <FS:ND> Report amount of failed buffer allocations
    static void addAllocationError();
    static U32 getAllocationErrors();
private:
    static U32 sAllocationErrors;
    // </FS:ND>
};

using LLImageDataLock = LLImageBase::DataLock<false>;
using LLImageDataSharedLock = LLImageBase::DataLock<true>;

// Raw representation of an image (used for textures, and other uncompressed formats
class LLImageRaw : public LLImageBase
{
protected:
    /*virtual*/ ~LLImageRaw();

public:
    LLImageRaw();
    LLImageRaw(U16 width, U16 height, S8 components);
    LLImageRaw(const U8* data, U16 width, U16 height, S8 components);
    LLImageRaw(U8 *data, U16 width, U16 height, S8 components, bool no_copy = false);
    // Construct using createFromFile (used by tools)
    //LLImageRaw(const std::string& filename, bool j2c_lowest_mip_only = false);

    /*virtual*/ void deleteData();
    /*virtual*/ U8* allocateData(S32 size = -1);
    /*virtual*/ U8* reallocateData(S32 size);

    // use in conjunction with "no_copy" constructor to release data pointer before deleting
    // so that deletion of this LLImageRaw will not free the memory at the "data" parameter
    // provided to "no_copy" constructor
    void releaseData();


    bool resize(U16 width, U16 height, S8 components);

    //U8 * getSubImage(U32 x_pos, U32 y_pos, U32 width, U32 height) const;
    bool setSubImage(U32 x_pos, U32 y_pos, U32 width, U32 height,
                     const U8 *data, U32 stride = 0, bool reverse_y = false);

    void clear(U8 r=0, U8 g=0, U8 b=0, U8 a=255);

    void verticalFlip();

    // Returns true if the image is not fully opaque
    bool checkHasTransparentPixels();
    // if the alpha channel is all 100% opaque, delete it
    // returns true if alpha channel was deleted
    bool optimizeAwayAlpha();
    // Create an alpha channel if this image doesn't have one
    bool makeAlpha();

    static S32 biasedDimToPowerOfTwo(S32 curr_dim, S32 max_dim = MAX_IMAGE_SIZE);
    static S32 expandDimToPowerOfTwo(S32 curr_dim, S32 max_dim = MAX_IMAGE_SIZE);
    static S32 contractDimToPowerOfTwo(S32 curr_dim, S32 min_dim = MIN_IMAGE_SIZE);
    void expandToPowerOfTwo(S32 max_dim = MAX_IMAGE_SIZE, bool scale_image = true);
    void contractToPowerOfTwo(S32 max_dim = MAX_IMAGE_SIZE, bool scale_image = true);
    void biasedScaleToPowerOfTwo(S32 max_dim = MAX_IMAGE_SIZE);
    bool scale(S32 new_width, S32 new_height, bool scale_image = true);
    LLPointer<LLImageRaw> scaled(S32 new_width, S32 new_height);

    // Fill the buffer with a constant color
    void fill( const LLColor4U& color );

    // Multiply this raw image by the given color
    void tint( const LLColor3& color );

    // Copy operations

    //duplicate this raw image if refCount > 1.
    LLPointer<LLImageRaw> duplicate();

    // Src and dst can be any size.  Src and dst can each have 3 or 4 components.
    void copy( const LLImageRaw* src );

    // Src and dst are same size.  Src and dst have same number of components.
    void copyUnscaled( const LLImageRaw* src );

    // Src and dst are same size.  Src has 4 components.  Dst has 3 components.
    void copyUnscaled4onto3( const LLImageRaw* src );

    // Src and dst are same size.  Src has 3 components.  Dst has 4 components.
    void copyUnscaled3onto4( const LLImageRaw* src );

    // Src and dst are same size.  Src has 1 component.  Dst has 4 components.
    // Alpha component is set to source alpha mask component.
    // RGB components are set to fill color.
    void copyUnscaledAlphaMask( const LLImageRaw* src, const LLColor4U& fill);

    // Src and dst can be any size.  Src and dst have same number of components.
    void copyScaled( const LLImageRaw* src );


    // Composite operations

    // Src and dst can be any size.  Src and dst can each have 3 or 4 components.
    void composite( const LLImageRaw* src );

    // Emissive operations used by minimap
    // Roughly emulates GLTF emissive texture, but is not GLTF-compliant
    // *TODO: Remove in favor of shader
    void addEmissive(LLImageRaw* src);
    void addEmissiveScaled(LLImageRaw* src);
    void addEmissiveUnscaled(LLImageRaw* src);
protected:
    // Src and dst can be any size.  Src has 4 components.  Dst has 3 components.
    void compositeScaled4onto3( const LLImageRaw* src );

    // Src and dst are same size.  Src has 4 components.  Dst has 3 components.
    void compositeUnscaled4onto3( const LLImageRaw* src );

    // Src and dst can be any size.  Src has 3 components.  Dst has 4 components.
    void copyScaled3onto4( const LLImageRaw* src );

    // Src and dst can be any size.  Src has 4 components.  Dst has 3 components.
    void copyScaled4onto3( const LLImageRaw* src );

    // Create an image from a local file (generally used in tools)
    //bool createFromFile(const std::string& filename, bool j2c_lowest_mip_only = false);

    void copyLineScaled( const U8* in, U8* out, S32 in_pixel_len, S32 out_pixel_len, S32 in_pixel_step, S32 out_pixel_step );
    void compositeRowScaled4onto3( const U8* in, U8* out, S32 in_pixel_len, S32 out_pixel_len );

    static U8 fastFractionalMult(U8 a, U8 b);

    void setDataAndSize(U8 *data, S32 width, S32 height, S8 components) ;

public:
    // <FS> BC7 at decode (rkimagebc7.h, doc/vulkan/PLAN-aurora-ideas.md section 2 stage b): a compressed image holds the whole mip chain, the smallest level first and the finest
    // last; width, height and components are the finest level's, getDataSize() the chain's bytes. The pixel operations of this class refuse a compressed image with a warning:
    // the needs-raw policy of the texture fetch keeps them from ever meeting one.
    bool isCompressed() const { return mCompressedFormat != 0; }
    U8 getCompressedFormat() const { return mCompressedFormat; }        // RKImageBC7::FORMAT_BC7
    U8 getLevelCount() const { return mLevels; }
    S8 getAlphaIsMask() const { return mAlphaIsMask; }                  // -1 unknown or no alpha, 0 blend, 1 mask: the classification RKImageTex::analyzeAlpha would have made
    // <FS> the upload analysis (PLAN-aurora-ideas.md section 6.2): what RKImageTex::analyzeAlpha and updatePickMask would compute from these texels on the main thread at upload,
    // computed instead where the texels already are (the decode pool), and adopted by the texture at create time. Any pixel operation on this image drops it.
    void analyzeForUpload();                                            // texels: the alpha class (4 or 2 components) and the pick mask (4 components); a compressed image: nothing to do
    bool hasUploadAnalysis() const { return mUploadAnalysed; }
    const std::vector<U8>& getPickMask() const { return mPickMask; }  // RKImageTex's layout: a bit per 2x2 block, rows of width/2 + 1, set where alpha > 32; empty when there is none
    U16 getPickMaskWidth() const { return mPickMaskWidth; }            // RKImageTex::mPickMaskWidth (width/2)
    U16 getPickMaskHeight() const { return mPickMaskHeight; }
    const U8* getFinestLevelData() const;                                 // the last level of the chain (RKImageTex::setImage(data_hasmips) walks down from it)
    // takes ownership of data (ll_aligned_malloc_16, size bytes)
    void setCompressedChain(U8* data, S32 size, U16 width, U16 height, S8 components, U8 format, U8 levels, S8 alpha_is_mask);
private:
    U8 mCompressedFormat = 0;
    U8 mLevels = 0;
    S8 mAlphaIsMask = -1;
    bool mUploadAnalysed = false;
    std::vector<U8> mPickMask;
    U16 mPickMaskWidth = 0, mPickMaskHeight = 0;
public:
    static S32 sRawImageCount;
    // <FS:Techwolf Lupindo> texture comment metadata reader
    std::string mComment;
    // </FS:Techwolf Lupindo>

private:
    static bool validateSrcAndDst(std::string func, const LLImageRaw* src, const LLImageRaw* dst);
};

// Compressed representation of image.
// Subclass from this class for the different representations (J2C, bmp)
class LLImageFormatted : public LLImageBase
{
public:
    static LLImageFormatted* createFromType(S8 codec);
    static LLImageFormatted* loadFromMemory(const U8* data, U32 size, std::string_view mimetype);
    static LLImageFormatted* createFromExtension(const std::string& instring);
    static LLImageFormatted* createFromMimeType(std::string_view mimetype);
    static S8 getCodecFromMimeType(std::string_view mimetype);

protected:
    /*virtual*/ ~LLImageFormatted();

public:
    LLImageFormatted(S8 codec);

    // LLImageBase
    /*virtual*/ void deleteData();
    /*virtual*/ U8* allocateData(S32 size = -1);
    /*virtual*/ U8* reallocateData(S32 size);

    /*virtual*/ void dump();
    /*virtual*/ void sanityCheck();

    // New methods
    // subclasses must return a prefered file extension (lowercase without a leading dot)
    virtual std::string getExtension() = 0;
    // calcHeaderSize() returns the maximum size of header;
    //   0 indicates we don't have a header and have to read the entire file
    virtual S32 calcHeaderSize() { return 0; };
    // calcDataSize() returns how many bytes to read to load discard_level (including header)
    virtual S32 calcDataSize(S32 discard_level);
    // calcDiscardLevelBytes() returns the smallest valid discard level based on the number of input bytes
    virtual S32 calcDiscardLevelBytes(S32 bytes);
    // getRawDiscardLevel() by default returns mDiscardLevel, but may be overridden (LLImageJ2C)
    virtual S8  getRawDiscardLevel() { return mDiscardLevel; }

    bool load(const std::string& filename, int load_size = 0);
    bool save(const std::string& filename);

    virtual bool updateData() = 0; // pure virtual
    void setData(U8 *data, S32 size);
    void appendData(U8 *data, S32 size);

    // Loads first 4 channels.
    virtual bool decode(LLImageRaw* raw_image, F32 decode_time) = 0;
    // Subclasses that can handle more than 4 channels should override this function.
    virtual bool decodeChannels(LLImageRaw* raw_image, F32 decode_time, S32 first_channel, S32 max_channel);

    virtual bool encode(const LLImageRaw* raw_image, F32 encode_time) = 0;

    S8 getCodec() const;
    bool isDecoding() const { return mDecoding; }
    bool isDecoded()  const { return mDecoded; }
    void setDiscardLevel(S8 discard_level) { mDiscardLevel = discard_level; }
    S8 getDiscardLevel() const { return mDiscardLevel; }
    S8 getLevels() const { return mLevels; }
    void setLevels(S8 nlevels) { mLevels = nlevels; }

    // setLastError needs to be deferred for J2C images since it may be called from a DLL
    virtual void resetLastError();
    virtual void setLastError(const std::string& message, const std::string& filename = std::string());

protected:
    bool copyData(U8 *data, S32 size); // calls updateData()

protected:
    S8 mCodec;
    S8 mDecoding;
    S8 mDecoded;  // unused, but changing LLImage layout requires recompiling static Mac/Linux libs. 2009-01-30 JC
    S8 mDiscardLevel;   // Current resolution level worked on. 0 = full res, 1 = half res, 2 = quarter res, etc...
    S8 mLevels;         // Number of resolution levels in that image. Min is 1. 0 means unknown.

public:
    static S32 sGlobalFormattedMemory;
};

// <FS> 2026-11-14 (Adam: the eyes and ears bake slots show another texture's image): a sampled hash of a buffer (size, then up to 4096 evenly spaced 8 byte words): the same function hashes what a server bake's decode
// made (the BakeTex log, lltexturefetch.cpp) and what a texture object last uploaded (RKImageTex::getUploadHash, printed with the bake slots by Show Textures), so the two can be compared
inline U64 rk_sparse_hash(const U8* data, size_t size)
{
    U64 h = 1469598103934665603ull ^ (U64)size;
    if (!data || size < 8)
    {
        return h;
    }
    const size_t words = size / 8;
    const size_t step = words / 4096 > 1 ? words / 4096 : 1;
    for (size_t i = 0; i < words; i += step)
    {
        U64 w;
        memcpy(&w, data + i * 8, 8);
        h = (h ^ w) * 1099511628211ull;
    }
    return h;
}

#endif
