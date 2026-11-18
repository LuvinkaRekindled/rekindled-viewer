/**
 * @file llviewertexturelist.cpp
 * @brief Object for managing the list of images within a region
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

#include "llviewerprecompiledheaders.h"
#include "llgpuzonetimer.h"

#include <sys/stat.h>

#include <atomic>
#include "llcharacter.h"    // <FS> the skeletons are cleaned before the texture priority jobs
#include "llvoavatar.h"
#include "llforkjoinpool.h"
#include "llgpuzonetimer.h"
#include "llviewertexturelist.h"
#include "fstexturearrivaltrace.h"     // <FS> the arrival trace
#include "lldrawpool.h"

#include "llagent.h"
#include "lldevicecaps.h" // fot gathering stats from GL
#include "llimagetex.h"
#include "llimagebmp.h"
#include "llimagej2c.h"
#include "llimagetga.h"
#include "llimagejpeg.h"
#include "llimagepng.h"
#include "llimageworker.h"

#include "llsdserialize.h"
#include "llsys.h"
#include "llfilesystem.h"
#include "llxmltree.h"
#include "message.h"

#include "lldrawpoolbump.h" // to init bumpmap images
#include "lltexturecache.h"
#include "lltexturefetch.h"
#include "llviewercontrol.h"
#include "llviewertexture.h"
#include "llviewermedia.h"
#include "llviewernetwork.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "llviewercamera.h"
#include "llviewerstats.h"
#include "pipeline.h"
#include "llappviewer.h"
#include "llxuiparser.h"
#include "lltracerecording.h"
#include "llviewerdisplay.h"
#include "llviewerwindow.h"
#include "llwindow.h"
#include "llprogressview.h"
#include "llvkbridge.h"
#include "llface.h"
#include "lldrawable.h"
#include "llviewerobject.h"
#include "llspatialpartition.h"
#include "llviewertexturerecs.h"
#include <algorithm>
#include "lldiag.h"        // <FS> LL_DIAG: the statistics lines are off unless LL_VK_DIAG is set

namespace
{
// <FS> H6: what the active and dormant lists did (cumulative; the log line prints the difference of two readings)
struct IdxStats
{
    U64 mToDormant = 0, mToActive = 0, mWakes = 0, mWakeGroup = 0, mWakeGroupTextures = 0;
    U64 mVisitsActive = 0, mVisitsDormant = 0, mVisitsWake = 0;
    U64 mMisses = 0, mMissAge[3] = {};          // verify: a dormant texture found wanted with nothing having woken it, by how long it had been dormant (< 1 s, < 10 s, longer)
    U64 mMissWhy[6] = {};                       // ... and why (several can hold): boost, callback, fetching, light projection, a face in view, woken in the last 5 s
    U64 mDescVisits = 0, mDescChanged = 0, mDescFiner = 0, mDescCoarser = 0, mDescAbs = 0;       // visits that ended in processTextureStats; of them the desired discard level changed (finer, coarser, levels in all)
};
IdxStats sIdx;

// <FS> H6: the worker's result for a texture against the face loop's (LL_VK_TEXREC=1)
struct RecVerify
{
    U64 mNone = 0, mStale = 0, mVolatile = 0, mBig = 0, mCountDiff = 0;       // not comparable: no result yet, one for other faces, volatile faces, more than 1024 faces, another number of faces
    U64 mCompared = 0, mKept = 0;                                               // compared (of them the loop's answer was a kept one)
    U64 mSizeDiff = 0, mWorkerBigger = 0, mKeptDiff = 0, mOccDiff = 0;         // the sizes differ by more than a quarter (of them the worker's was the larger; of them the loop's was a kept result); "all faces occluded" differs
    U64 mUsed = 0, mFellBack = 0;               // LL_VK_TEXREC=2: textures sized from the worker's result / that kept the face loop (no result, stale, volatile faces, another number of faces)
    // <FS> LL_VK_TEXREC_WHY=1 (diagnostic, 2026-11-15): why a visit kept the face loop in mode 2, and the visits split by whether the texture has an image of its own (idea 3, GPU texture need for loaded textures)
    U64 mFbNone = 0, mFbStale = 0, mFbVolatile = 0, mFbCount = 0, mFbBig = 0;
    U64 mVisLoaded = 0, mVisUnloaded = 0, mFacesLoaded = 0, mFacesUnloaded = 0, mUsedLoaded = 0, mUsedUnloaded = 0, mFbLoaded = 0, mFbUnloaded = 0;
};
RecVerify sRecV;
}

////////////////////////////////////////////////////////////////////////////

void (*LLViewerTextureList::sUUIDCallback)(void **, const LLUUID&) = NULL;

S32 LLViewerTextureList::sNumImages = 0;

// <FS:Ansariel> Fast cache stats
U32 LLViewerTextureList::sNumFastCacheReads = 0;

LLViewerTextureList gTextureList;

extern LLShaderProgram gCopyProgram;

ETexListType get_element_type(S32 priority)
{
    return (priority == LLViewerFetchedTexture::BOOST_ICON || priority == LLViewerFetchedTexture::BOOST_THUMBNAIL) ? TEX_LIST_SCALE : TEX_LIST_STANDARD;
}

///////////////////////////////////////////////////////////////////////////////

LLTextureKey::LLTextureKey()
: textureId(LLUUID::null),
textureType(TEX_LIST_STANDARD)
{
}

LLTextureKey::LLTextureKey(LLUUID id, ETexListType tex_type)
: textureId(id), textureType(tex_type)
{
}

///////////////////////////////////////////////////////////////////////////////

LLViewerTextureList::LLViewerTextureList()
    : mForceResetTextureStats(false),
    mInitialized(false)
{
}

void LLViewerTextureList::init()
{
    mInitialized = true ;
    sNumImages = 0;
    doPreloadImages();
}


void LLViewerTextureList::doPreloadImages()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    LL_DEBUGS("ViewerImages") << "Preloading images..." << LL_ENDL;

    llassert_always(mInitialized) ;
    llassert_always(mImageList.empty()) ;
    llassert_always(mUUIDMap.empty()) ;

    // Set the "missing asset" image
    LLViewerFetchedTexture::sMissingAssetImagep = LLViewerTextureManager::getFetchedTextureFromFile("missing_asset.tga", FTT_LOCAL_FILE, MIPMAP_NO, LLViewerFetchedTexture::BOOST_UI);

    // Set the "white" image
    LLViewerFetchedTexture::sWhiteImagep = LLViewerTextureManager::getFetchedTextureFromFile("white.tga", FTT_LOCAL_FILE, MIPMAP_NO, LLViewerFetchedTexture::BOOST_UI);
    LLTexUnit::sWhiteTexture = LLViewerFetchedTexture::sWhiteImagep->getTexName();
    LLUIImageList* image_list = LLUIImageList::getInstance();

    // Set default particle texture
    LLViewerFetchedTexture::sDefaultParticleImagep = LLViewerTextureManager::getFetchedTextureFromFile("pixiesmall.j2c");

    // Set the default flat normal map
    // BLANK_OBJECT_NORMAL has a version on dataserver, but it has compression artifacts
    LLViewerFetchedTexture::sFlatNormalImagep =
        LLViewerTextureManager::getFetchedTextureFromFile("flatnormal.tga",
                                                          FTT_LOCAL_FILE,
                                                          MIPMAP_NO,
                                                          LLViewerFetchedTexture::BOOST_BUMP,
                                                          LLViewerTexture::FETCHED_TEXTURE,
                                                          TexelFormat::None,
                                                          PixelFormat::None,
                                                          BLANK_OBJECT_NORMAL);

    // PBR: irradiance
    LLViewerFetchedTexture::sDefaultIrradiancePBRp = LLViewerTextureManager::getFetchedTextureFromFile("default_irradiance.png", FTT_LOCAL_FILE, MIPMAP_YES, LLViewerFetchedTexture::BOOST_UI);

    image_list->initFromFile();

    // turn off clamping and bilinear filtering for uv picking images
    //LLViewerFetchedTexture* uv_test = preloadUIImage("uv_test1.tga", LLUUID::null, false);
    //uv_test->setClamp(false, false);
    //uv_test->setMipFilterNearest(true, true);
    //uv_test = preloadUIImage("uv_test2.tga", LLUUID::null, false);
    //uv_test->setClamp(false, false);
    //uv_test->setMipFilterNearest(true, true);

    LLViewerFetchedTexture* image = LLViewerTextureManager::getFetchedTextureFromFile("silhouette.j2c", FTT_LOCAL_FILE, MIPMAP_YES, LLViewerFetchedTexture::BOOST_UI);
    if (image)
    {
        image->setAddressMode(LLTexUnit::TAM_WRAP);
        mImagePreloads.insert(image);
    }
    image = LLViewerTextureManager::getFetchedTextureFromFile("world/NoEntryLines.png", FTT_LOCAL_FILE, MIPMAP_YES, LLViewerFetchedTexture::BOOST_UI);
    if (image)
    {
        image->setAddressMode(LLTexUnit::TAM_WRAP);
        mImagePreloads.insert(image);
    }
    image = LLViewerTextureManager::getFetchedTextureFromFile("world/NoEntryPassLines.png", FTT_LOCAL_FILE, MIPMAP_YES, LLViewerFetchedTexture::BOOST_UI);
    if (image)
    {
        image->setAddressMode(LLTexUnit::TAM_WRAP);
        mImagePreloads.insert(image);
    }
    image = LLViewerTextureManager::getFetchedTextureFromFile("transparent.j2c", FTT_LOCAL_FILE, MIPMAP_YES, LLViewerFetchedTexture::BOOST_UI, LLViewerTexture::FETCHED_TEXTURE,
        TexelFormat::None, PixelFormat::None, IMG_TRANSPARENT);
    if (image)
    {
        image->setAddressMode(LLTexUnit::TAM_WRAP);
        mImagePreloads.insert(image);
    }
    image = LLViewerTextureManager::getFetchedTextureFromFile("alpha_gradient.tga", FTT_LOCAL_FILE, MIPMAP_YES, LLViewerFetchedTexture::BOOST_UI, LLViewerTexture::FETCHED_TEXTURE,
        TexelFormat::Alpha8, PixelFormat::Alpha, IMG_ALPHA_GRAD);
    if (image)
    {
        image->setAddressMode(LLTexUnit::TAM_CLAMP);
        mImagePreloads.insert(image);
    }
    image = LLViewerTextureManager::getFetchedTextureFromFile("alpha_gradient_2d.j2c", FTT_LOCAL_FILE, MIPMAP_YES, LLViewerFetchedTexture::BOOST_UI, LLViewerTexture::FETCHED_TEXTURE,
        TexelFormat::Alpha8, PixelFormat::Alpha, IMG_ALPHA_GRAD_2D);
    if (image)
    {
        image->setAddressMode(LLTexUnit::TAM_CLAMP);
        mImagePreloads.insert(image);
    }
}

static std::string get_texture_list_name()
{
    // <FS:Ansariel> OpenSim compatibility
    //if (LLGridManager::getInstance()->isInProductionGrid())
    if (LLGridManager::getInstance()->isInSLMain())
    // </FS:Ansariel>
    {
        return gDirUtilp->getExpandedFilename(LL_PATH_CACHE,
            "texture_list_" + gSavedSettings.getString("LoginLocation") + "." + gDirUtilp->getUserName() + ".xml");
    }
    else
    {
        const std::string& grid_id_str = LLGridManager::getInstance()->getGridId();
        const std::string& grid_id_lower = utf8str_tolower(grid_id_str);
        return gDirUtilp->getExpandedFilename(LL_PATH_CACHE,
            "texture_list_" + gSavedSettings.getString("LoginLocation") + "." + gDirUtilp->getUserName() + "." + grid_id_lower + ".xml");
    }
}

void LLViewerTextureList::doPrefetchImages()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    // todo: do not load without getViewerAssetUrl()
    // either fail login without caps or provide this
    // in some other way, textures won't load otherwise
    LLViewerFetchedTexture *imagep = findImage(DEFAULT_WATER_NORMAL, TEX_LIST_STANDARD);
    if (!imagep)
    {
        // add it to mImagePreloads only once
        imagep = LLViewerTextureManager::getFetchedTexture(DEFAULT_WATER_NORMAL, FTT_DEFAULT, MIPMAP_YES, LLViewerFetchedTexture::BOOST_UI);
        if (imagep)
        {
            imagep->setAddressMode(LLTexUnit::TAM_WRAP);
            mImagePreloads.insert(imagep);
        }
    }

    LLViewerTextureManager::getFetchedTexture(IMG_SHOT);
    LLViewerTextureManager::getFetchedTexture(IMG_SMOKE_POOF);
    LLViewerFetchedTexture::sSmokeImagep = LLViewerTextureManager::getFetchedTexture(IMG_SMOKE, FTT_DEFAULT, true, LLBaseTexture::BOOST_UI);
    LLViewerFetchedTexture::sSmokeImagep->setNoDelete();

    LLStandardBumpmap::addstandard();

    if (LLAppViewer::instance()->getPurgeCache())
    {
        // cache was purged, no point
        return;
    }

    // Pre-fetch textures from last logout
    LLSD imagelist;
    std::string filename = get_texture_list_name();
    llifstream file;
    file.open(filename.c_str());
    if (file.is_open())
    {
        if ( ! LLSDSerialize::fromXML(imagelist, file) )
        {
            file.close();
            LL_WARNS() << "XML parse error reading texture list '" << filename << "'" << LL_ENDL;
            LL_WARNS() << "Removing invalid texture list '" << filename << "'" << LL_ENDL;
            LLFile::remove(filename);
            return;
        }
        file.close();
    }
    S32 texture_count = 0;
    for (LLSD::array_iterator iter = imagelist.beginArray();
         iter != imagelist.endArray(); ++iter)
    {
        LLSD imagesd = *iter;
        LLUUID uuid = imagesd["uuid"];
        S32 pixel_area = imagesd["area"];
        S32 texture_type = imagesd["type"];

        if((LLViewerTexture::FETCHED_TEXTURE == texture_type || LLViewerTexture::LOD_TEXTURE == texture_type) && !LLViewerTexture::isInvisiprim(uuid))
        {
            LLViewerFetchedTexture* image = LLViewerTextureManager::getFetchedTexture(uuid, FTT_DEFAULT, MIPMAP_TRUE, LLBaseTexture::BOOST_NONE, texture_type);
            if (image)
            {
                texture_count += 1;
                image->addTextureStats((F32)pixel_area);
            }
        }
    }
    LL_DEBUGS() << "fetched " << texture_count << " images from " << filename << LL_ENDL;
}

///////////////////////////////////////////////////////////////////////////////

LLViewerTextureList::~LLViewerTextureList()
{
}

void LLViewerTextureList::shutdown()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    LL_WARNS() << "Shutdown called" << LL_ENDL;
    // clear out preloads
    mImagePreloads.clear();

    // Write out list of currently loaded textures for precaching on startup
    typedef std::set<std::pair<S32,LLViewerFetchedTexture*> > image_area_list_t;
    image_area_list_t image_area_list;
    for (image_list_t::iterator iter = mImageList.begin();
         iter != mImageList.end(); ++iter)
    {
        LLViewerFetchedTexture* image = *iter;
        if (!image->hasImageTexture() ||
            !image->getUseDiscard() ||
            image->needsAux() ||
            !image->getTargetHost().isInvalid() ||
            !image->getUrl().empty() ||
            image->isInvisiprim()
            )
        {
            continue; // avoid UI, baked, and other special images
        }
        if(!image->getBoundRecently())
        {
            continue ;
        }
        S32 desired = image->getDesiredDiscardLevel();
        if (desired >= 0 && desired < MAX_DISCARD_LEVEL)
        {
            S32 pixel_area = image->getWidth(desired) * image->getHeight(desired);
            image_area_list.insert(std::make_pair(pixel_area, image));
        }
    }

    LLSD imagelist;
    const S32 max_count = 1000;
    S32 count = 0;
    S32 image_type ;
    for (image_area_list_t::reverse_iterator riter = image_area_list.rbegin();
         riter != image_area_list.rend(); ++riter)
    {
        LLViewerFetchedTexture* image = riter->second;
        image_type = (S32)image->getType() ;
        imagelist[count]["area"] = riter->first;
        imagelist[count]["uuid"] = image->getID();
        imagelist[count]["type"] = image_type;
        if (++count >= max_count)
            break;
    }

    if (count > 0 && !gDirUtilp->getExpandedFilename(LL_PATH_CACHE, "").empty())
    {
        std::string filename = get_texture_list_name();
        llofstream file;
        file.open(filename.c_str());
        LL_DEBUGS() << "saving " << imagelist.size() << " image list entries" << LL_ENDL;
        LLSDSerialize::toPrettyXML(imagelist, file);
    }

    //
    // Clean up "loaded" callbacks.
    //
    mCallbackList.clear();

    // Flush all of the references
    while (!mCreateTextureList.empty())
    {
        mCreateTextureList.front()->mCreatePending = false;
        mCreateTextureList.pop();
    }
    mFastCacheList.clear();

    idxClear();         // <FS> H6
    LLTexRecs::stop();

    mUUIDMap.clear();

    mImageList.clear();

    mInitialized = false ; //prevent loading textures again.
}

void LLViewerTextureList::dump()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    LL_INFOS() << "LLViewerTextureList::dump()" << LL_ENDL;
    for (image_list_t::iterator it = mImageList.begin(); it != mImageList.end(); ++it)
    {
        LLViewerFetchedTexture* image = *it;

        LL_INFOS() << "priority " << image->getMaxVirtualSize()
        << " boost " << image->getBoostLevel()
        << " size " << image->getWidth() << "x" << image->getHeight()
        << " discard " << image->getDiscardLevel()
        << " desired " << image->getDesiredDiscardLevel()
        << " http://asset.siva.lindenlab.com/" << image->getID() << ".texture"
        << LL_ENDL;
    }
}

void LLViewerTextureList::destroyResources()
{
    LLImageTex::destroyResources();
}

/* Vertical tab container button image IDs
 Seem to not decode when running app in debug.

 const LLUUID BAD_IMG_ONE("1097dcb3-aef9-8152-f471-431d840ea89e");
 const LLUUID BAD_IMG_TWO("bea77041-5835-1661-f298-47e2d32b7a70");
 */

///////////////////////////////////////////////////////////////////////////////

LLViewerFetchedTexture* LLViewerTextureList::getImageFromFile(const std::string& filename,
                                                   FTType f_type,
                                                   bool usemipmaps,
                                                   LLViewerTexture::EBoostLevel boost_priority,
                                                   S8 texture_type,
                                                   TexelFormat internal_format,
                                                   PixelFormat primary_format,
                                                   const LLUUID& force_id)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    LL_PROFILE_ZONE_TEXT(filename.c_str(), filename.size());
    if(!mInitialized)
    {
        return NULL ;
    }

    std::string full_path = gDirUtilp->findSkinnedFilename("textures", filename);
    if (full_path.empty())
    {
        LL_WARNS() << "Failed to find local image file: " << filename << LL_ENDL;
        LLViewerTexture::EBoostLevel priority = LLBaseTexture::BOOST_UI;
        return LLViewerTextureManager::getFetchedTexture(IMG_DEFAULT, FTT_DEFAULT, true, priority);
    }

    std::string url = "file://" + full_path;

    return getImageFromUrl(url, f_type, usemipmaps, boost_priority, texture_type, internal_format, primary_format, force_id);
}

LLViewerFetchedTexture* LLViewerTextureList::getImageFromUrl(const std::string& url,
                                                   FTType f_type,
                                                   bool usemipmaps,
                                                   LLViewerTexture::EBoostLevel boost_priority,
                                                   S8 texture_type,
                                                   TexelFormat internal_format,
                                                   PixelFormat primary_format,
                                                   const LLUUID& force_id)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if(!mInitialized)
    {
        return NULL ;
    }

    // generate UUID based on hash of filename
    LLUUID new_id;
    if (force_id.notNull())
    {
        new_id = force_id;
    }
    else
    {
        new_id.generate(url);
    }

    LLPointer<LLViewerFetchedTexture> imagep = findImage(new_id, get_element_type(boost_priority));

    if (!imagep.isNull())
    {
        LLViewerFetchedTexture *texture = imagep.get();
        if (texture->getUrl().empty())
        {
            LL_WARNS() << "Requested texture " << new_id << " already exists but does not have a URL" << LL_ENDL;
        }
        else if (texture->getUrl() != url)
        {
            // This is not an error as long as the images really match -
            // e.g. could be two avatars wearing the same outfit.
            LL_DEBUGS("Avatar") << "Requested texture " << new_id
                                << " already exists with a different url, requested: " << url
                                << " current: " << texture->getUrl() << LL_ENDL;
        }

    }
    if (imagep.isNull())
    {
        switch(texture_type)
        {
        case LLViewerTexture::FETCHED_TEXTURE:
            imagep = new LLViewerFetchedTexture(url, f_type, new_id, usemipmaps);
            break ;
        case LLViewerTexture::LOD_TEXTURE:
            imagep = new LLViewerLODTexture(url, f_type, new_id, usemipmaps);
            break ;
        default:
            LL_ERRS() << "Invalid texture type " << texture_type << LL_ENDL ;
        }

        if (internal_format != TexelFormat::None && primary_format != PixelFormat::None)
        {
            imagep->setExplicitFormat(internal_format, primary_format);
        }

        addImage(imagep, get_element_type(boost_priority));

        if (boost_priority != 0)
        {
            if (boost_priority == LLViewerFetchedTexture::BOOST_UI)
            {
                imagep->dontDiscard();
            }
            if (boost_priority == LLViewerFetchedTexture::BOOST_ICON
                || boost_priority == LLViewerFetchedTexture::BOOST_THUMBNAIL)
            {
                // Agent and group Icons are downloadable content, nothing manages
                // icon deletion yet, so they should not persist
                imagep->dontDiscard();
                imagep->forceActive();
            }
            imagep->setBoostLevel(boost_priority);
        }
    }

    imagep->setImageTextureCreated(true);

    return imagep;
}

LLImageRaw* LLViewerTextureList::getRawImageFromMemory(const U8* data, U32 size, std::string_view mimetype)
{
    LLPointer<LLImageFormatted> image = LLImageFormatted::loadFromMemory(data, size, mimetype);

    if (image)
    {
        LLImageRaw* raw_image = new LLImageRaw();
        image->decode(raw_image, 0.f);
        return raw_image;
    }
    else
    {
        return nullptr;
    }
}

LLViewerFetchedTexture* LLViewerTextureList::getImageFromMemory(const U8* data, U32 size, std::string_view mimetype)
{
    LLPointer<LLImageRaw> raw_image = getRawImageFromMemory(data, size, mimetype);
    if (raw_image.notNull())
    {
        LLViewerFetchedTexture* imagep = new LLViewerFetchedTexture(raw_image, FTT_LOCAL_FILE, true);
        addImage(imagep, TEX_LIST_STANDARD);

        imagep->dontDiscard();
        imagep->setBoostLevel(LLViewerFetchedTexture::BOOST_PREVIEW);
        return imagep;
    }
    else
    {
        return nullptr;
    }
}

LLViewerFetchedTexture* LLViewerTextureList::getImage(const LLUUID &image_id,
                                                   FTType f_type,
                                                   bool usemipmaps,
                                                   LLViewerTexture::EBoostLevel boost_priority,
                                                   S8 texture_type,
                                                   TexelFormat internal_format,
                                                   PixelFormat primary_format,
                                                   LLHost request_from_host)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if(!mInitialized)
    {
        return NULL ;
    }

    // Return the image with ID image_id
    // If the image is not found, creates new image and
    // enqueues a request for transmission

    if (image_id.isNull())
    {
        return (LLViewerTextureManager::getFetchedTexture(IMG_DEFAULT, FTT_DEFAULT, true, LLBaseTexture::BOOST_UI));
    }

    LLPointer<LLViewerFetchedTexture> imagep = findImage(image_id, get_element_type(boost_priority));
    if (!imagep.isNull())
    {
        LLViewerFetchedTexture *texture = imagep.get();
        if (request_from_host.isOk() &&
            !texture->getTargetHost().isOk())
        {
            LL_WARNS() << "Requested texture " << image_id << " already exists but does not have a host" << LL_ENDL;
        }
        else if (request_from_host.isOk() &&
                 texture->getTargetHost().isOk() &&
                 request_from_host != texture->getTargetHost())
        {
            LL_WARNS() << "Requested texture " << image_id << " already exists with a different target host, requested: "
                    << request_from_host << " current: " << texture->getTargetHost() << LL_ENDL;
        }
        if (f_type != FTT_DEFAULT && imagep->getFTType() != f_type)
        {
            LL_WARNS() << "FTType mismatch: requested " << f_type << " image has " << imagep->getFTType() << LL_ENDL;
        }

    }
    if (imagep.isNull())
    {
        imagep = createImage(image_id, f_type, usemipmaps, boost_priority, texture_type, internal_format, primary_format, request_from_host) ;
    }

    imagep->setImageTextureCreated(true);

    return imagep;
}

//when this function is called, there is no such texture in the gTextureList with image_id.
LLViewerFetchedTexture* LLViewerTextureList::createImage(const LLUUID &image_id,
                                                   FTType f_type,
                                                   bool usemipmaps,
                                                   LLViewerTexture::EBoostLevel boost_priority,
                                                   S8 texture_type,
                                                   TexelFormat internal_format,
                                                   PixelFormat primary_format,
                                                   LLHost request_from_host)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    static LLCachedControl<bool> fast_cache_fetching_enabled(gSavedSettings, "FastCacheFetchEnabled", true); // <FS:Ansariel> Keep Fast Cache option

    LLPointer<LLViewerFetchedTexture> imagep ;
    switch(texture_type)
    {
    case LLViewerTexture::FETCHED_TEXTURE:
        imagep = new LLViewerFetchedTexture(image_id, f_type, request_from_host, usemipmaps);
        break ;
    case LLViewerTexture::LOD_TEXTURE:
        imagep = new LLViewerLODTexture(image_id, f_type, request_from_host, usemipmaps);
        break ;
    default:
        LL_ERRS() << "Invalid texture type " << texture_type << LL_ENDL ;
    }

    if (internal_format != TexelFormat::None && primary_format != PixelFormat::None)
    {
        imagep->setExplicitFormat(internal_format, primary_format);
    }

    // <FS:minerjr> [FIRE-35428] - Mega prim issue - fix compressed sculpted textures
    // Sculpted textures use the RGBA data for coodinates, so any compression can cause artifacts.
    if (boost_priority == LLViewerFetchedTexture::BOOST_SCULPTED)
    {
        // Disable the compression of BOOST_SCULPTED textures
        if (imagep->getImageTexture())
        {
            imagep->getImageTexture()->setAllowCompression(false);
        }
    }
    // </FS:minerjr> [FIRE-35428]

    addImage(imagep, get_element_type(boost_priority));

    if (boost_priority != 0)
    {
        if (boost_priority == LLViewerFetchedTexture::BOOST_UI)
        {
            imagep->dontDiscard();
        }
        if (boost_priority == LLViewerFetchedTexture::BOOST_ICON
            || boost_priority == LLViewerFetchedTexture::BOOST_THUMBNAIL)
        {
            // Agent and group Icons are downloadable content, nothing manages
            // icon deletion yet, so they should not persist.
            imagep->dontDiscard();
            imagep->forceActive();
        }
        imagep->setBoostLevel(boost_priority);
    }
    else
    {
        //by default, the texture can not be removed from memory even if it is not used.
        //here turn this off
        //if this texture should be set to NO_DELETE, call setNoDelete() afterwards.
        imagep->forceActive() ;
    }

    // <FS:Ansariel> Keep Fast Cache option
    // <FS:minerjr> [FIRE-35428] - Mega prim issue - fix compressed sculpted textures
    //if(fast_cache_fetching_enabled)
    // If the texture is Sculpted, don't allow it to be added to fast cache as it can affect the texture.
    if(fast_cache_fetching_enabled && boost_priority != LLViewerFetchedTexture::BOOST_SCULPTED)
    // </FS:minerjr> [FIRE-35428]
    {
        mFastCacheList.insert(imagep);
        imagep->setInFastCacheList(true);
    }
    // </FS:Ansariel>
    return imagep ;
}

void LLViewerTextureList::findTexturesByID(const LLUUID &image_id, std::vector<LLViewerFetchedTexture*> &output)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    LLTextureKey search_key(image_id, TEX_LIST_STANDARD);
    uuid_map_t::iterator iter = mUUIDMap.lower_bound(search_key);
    while (iter != mUUIDMap.end() && iter->first.textureId == image_id)
    {
        output.push_back(iter->second);
        iter++;
    }
}

LLViewerFetchedTexture *LLViewerTextureList::findImage(const LLTextureKey &search_key)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    uuid_map_t::iterator iter = mUUIDMap.find(search_key);
    if (iter == mUUIDMap.end())
        return NULL;
    return iter->second;
}

LLViewerFetchedTexture *LLViewerTextureList::findImage(const LLUUID &image_id, ETexListType tex_type)
{
    return findImage(LLTextureKey(image_id, tex_type));
}

void LLViewerTextureList::addImageToList(LLViewerFetchedTexture *image)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    assert_main_thread();
    llassert_always(mInitialized) ;
    llassert(image);
    if (image->isInImageList())
    {   // Flag is already set?
        LL_WARNS() << "LLViewerTextureList::addImageToList - image " << image->getID()  << " already in list" << LL_ENDL;
    }
    else
    {
        if (!(mImageList.insert(image)).second)
        {
            LL_WARNS() << "Error happens when insert image " << image->getID()  << " into mImageList!" << LL_ENDL ;
        }
        image->setInImageList(true);
    }
}

void LLViewerTextureList::removeImageFromList(LLViewerFetchedTexture *image)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    assert_main_thread();
    llassert_always(mInitialized) ;
    llassert(image);

    size_t count = 0;
    if (image->isInImageList())
    {
        image->setInImageList(false);
        count = mImageList.erase(image) ;
        if(count != 1)
        {
            LL_INFOS() << "Image  " << image->getID()
                << " had mInImageList set but mImageList.erase() returned " << count
                << LL_ENDL;
        }
    }
    else
    {   // Something is wrong, image is expected in list or callers should check first
        LL_INFOS() << "Calling removeImageFromList() for " << image->getID()
            << " but doesn't have mInImageList set"
            << " ref count is " << image->getNumRefs()
            << LL_ENDL;
        uuid_map_t::iterator iter = mUUIDMap.find(LLTextureKey(image->getID(), (ETexListType)image->getTextureListType()));
        if(iter == mUUIDMap.end())
        {
            LL_INFOS() << "Image  " << image->getID() << " is also not in mUUIDMap!" << LL_ENDL ;
        }
        else if (iter->second != image)
        {
            LL_INFOS() << "Image  " << image->getID() << " was in mUUIDMap but with different pointer" << LL_ENDL ;
    }
        else
    {
            LL_INFOS() << "Image  " << image->getID() << " was in mUUIDMap with same pointer" << LL_ENDL ;
        }
        count = mImageList.erase(image) ;
        llassert(count != 0);
        if(count != 0)
        {   // it was in the list already?
            LL_WARNS() << "Image  " << image->getID()
                << " had mInImageList false but mImageList.erase() returned " << count
                << LL_ENDL;
        }
    }
}

void LLViewerTextureList::addImage(LLViewerFetchedTexture *new_image, ETexListType tex_type)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (!new_image)
    {
        return;
    }
    //llassert(new_image);
    LLUUID image_id = new_image->getID();
    LLTextureKey key(image_id, tex_type);

    LLViewerFetchedTexture *image = findImage(key);
    if (image)
    {
        LL_INFOS() << "Image with ID " << image_id << " already in list" << LL_ENDL;
        if (image != new_image)
        {
            idxRemove(image);       // <FS> H6: the map is about to let go of it
        }
    }
    sNumImages++;

    addImageToList(new_image);
    mUUIDMap[key] = new_image;
    new_image->setTextureListType(tex_type);
    idxAdd(new_image);
}


void LLViewerTextureList::deleteImage(LLViewerFetchedTexture *image)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if( image)
    {
        if (image->hasCallbacks())
        {
            mCallbackList.erase(image);
        }
        LLTextureKey key(image->getID(), (ETexListType)image->getTextureListType());
        idxRemove(image);       // <FS> H6
        llverify(mUUIDMap.erase(key) == 1);
        sNumImages--;
        removeImageFromList(image);
    }
}

// <FS> H6: the lists. Every texture of mUUIDMap is on exactly one of mActive and mDormant (mListState 1 / 2, mListIndex its place); a move is a swap with the last entry of its list.
void LLViewerTextureList::idxAdd(LLViewerFetchedTexture* tex)
{
    if (!tex || tex->mListState != 0)
    {
        return;
    }
    tex->mListState = 1;
    tex->mListIndex = (S32)mActive.size();
    mActive.push_back(tex);
}

void LLViewerTextureList::idxRemove(LLViewerFetchedTexture* tex)
{
    if (!tex || tex->mListState == 0)
    {
        return;
    }
    std::vector<LLViewerFetchedTexture*>& v = tex->mListState == 1 ? mActive : mDormant;
    const S32 at = tex->mListIndex;
    if (at >= 0 && (size_t)at < v.size() && v[at] == tex)
    {
        LLViewerFetchedTexture* last = v.back();
        v[at] = last;
        last->mListIndex = at;
        v.pop_back();
    }
    tex->mListState = 0;
    tex->mListIndex = -1;
    if (tex->mWakeQueued)
    {
        tex->mWakeQueued = false;
        mWake.erase(std::remove(mWake.begin(), mWake.end(), tex), mWake.end());
    }
}

void LLViewerTextureList::idxMove(LLViewerFetchedTexture* tex, U8 state)
{
    if (!tex || tex->mListState == 0 || tex->mListState == state)
    {
        return;
    }
    idxRemove(tex);
    std::vector<LLViewerFetchedTexture*>& v = state == 1 ? mActive : mDormant;
    tex->mListState = state;
    tex->mListIndex = (S32)v.size();
    v.push_back(tex);
    if (state == 2)
    {
        tex->mDormantAtFrame = gFrameCount;
    }
}

void LLViewerTextureList::idxClear()
{
    for (LLViewerFetchedTexture* t : mActive) { t->mListState = 0; t->mListIndex = -1; t->mWakeQueued = false; }
    for (LLViewerFetchedTexture* t : mDormant) { t->mListState = 0; t->mListIndex = -1; t->mWakeQueued = false; }
    mActive.clear();
    mDormant.clear();
    mWake.clear();
    mActiveCursor = mDormantCursor = 0;
}

void LLViewerTextureList::wakeTexture(LLViewerTexture* t)
{
    LLViewerFetchedTexture* tex = static_cast<LLViewerFetchedTexture*>(t);
    if (!tex || tex->mListState != 2 || tex->mWakeQueued)
    {
        return;
    }
    tex->mWakeQueued = true;
    tex->mLastWakeFrame = gFrameCount;
    tex->bumpFaceStamp();       // (a kept face loop result is out of date: whatever woke the texture is something the loop reads, or the loop is the way to find out)
    mWake.push_back(tex);
    ++sIdx.mWakes;
}

// the texture's fetch finished (PLAN-aurora-ideas.md section 6: at the hub the result waited a median 0.59 s for the slice to reach the texture, five sixths of its whole way):
// it goes in front of the next pass, whatever list it is on. The face loop's kept result stays valid (nothing about the faces changed), only updateFetch needs to run.
void LLViewerTextureList::visitNext(LLViewerTexture* t)
{
    LLViewerFetchedTexture* tex = static_cast<LLViewerFetchedTexture*>(t);
    if (!tex || tex->mListState == 0 || tex->mWakeQueued)
    {
        return;
    }
    tex->mWakeQueued = true;
    tex->mLastWakeFrame = gFrameCount;
    mWake.push_back(tex);
    ++sIdx.mWakes;
}

// a group of the world camera is not occluded any more: the textures of its faces are looked at again
void LLViewerTextureList::wakeGroupTextures(LLSpatialGroup* group)
{
    if (!sIdxMode || !group)
    {
        return;
    }
    ++sIdx.mWakeGroup;
    for (LLSpatialGroup::element_iter i = group->getDataBegin(); i != group->getDataEnd(); ++i)
    {
        LLDrawable* drawable = (LLDrawable*)(*i)->getDrawable();
        if (!drawable)
        {
            continue;
        }
        for (S32 j = 0; j < drawable->getNumFaces(); ++j)
        {
            LLFace* face = drawable->getFace(j);
            if (!face)
            {
                continue;
            }
            for (U32 ch = 0; ch < LLRender::NUM_TEXTURE_CHANNELS; ++ch)
            {
                LLViewerTexture* tex = face->getTexture(ch);
                if (tex && tex->mListState == 2)
                {
                    gTextureList.wakeTexture(tex);
                    ++sIdx.mWakeGroupTextures;
                }
            }
        }
    }
}

void LLViewerTextureList::idxLog()
{
    static F32 last_log = 0.f;
    if (gFrameTimeSeconds - last_log < 10.f)
    {
        return;
    }
    static IdxStats prev;
    if (last_log > 0.f)
    {
        LL_INFOS("VkTexIdx") << "mode " << sIdxMode << ": active " << mActive.size() << ", dormant " << mDormant.size() << " | in 10 s: visited active " << sIdx.mVisitsActive - prev.mVisitsActive << ", dormant "
                             << sIdx.mVisitsDormant - prev.mVisitsDormant << ", woken " << sIdx.mVisitsWake - prev.mVisitsWake << " | moved to dormant " << sIdx.mToDormant - prev.mToDormant << ", to active "
                             << sIdx.mToActive - prev.mToActive << " | wakes " << sIdx.mWakes - prev.mWakes << " (groups unoccluded " << sIdx.mWakeGroup - prev.mWakeGroup << " naming "
                             << sIdx.mWakeGroupTextures - prev.mWakeGroupTextures << " dormant textures) | verify misses " << sIdx.mMisses - prev.mMisses << " (dormant < 1 s "
                             << sIdx.mMissAge[0] - prev.mMissAge[0] << ", < 10 s " << sIdx.mMissAge[1] - prev.mMissAge[1] << ", longer " << sIdx.mMissAge[2] - prev.mMissAge[2] << "); why (boost, callback, fetching, light, face in view, woken lately) " << sIdx.mMissWhy[0] - prev.mMissWhy[0] << " " << sIdx.mMissWhy[1] - prev.mMissWhy[1]
                             << " " << sIdx.mMissWhy[2] - prev.mMissWhy[2] << " " << sIdx.mMissWhy[3] - prev.mMissWhy[3] << " " << sIdx.mMissWhy[4] - prev.mMissWhy[4] << " " << sIdx.mMissWhy[5] - prev.mMissWhy[5]
                             << " | desired level: visits " << sIdx.mDescVisits - prev.mDescVisits << ", changed " << sIdx.mDescChanged - prev.mDescChanged << " (finer " << sIdx.mDescFiner - prev.mDescFiner
                             << ", coarser " << sIdx.mDescCoarser - prev.mDescCoarser << ", " << sIdx.mDescAbs - prev.mDescAbs << " levels)" << LL_ENDL;
    }
    prev = sIdx;
    if (LLTexRecs::sMode)
    {
        static RecVerify pv;
        const LLTexRecs::Stats st = LLTexRecs::stats();
        static LLTexRecs::Stats ps;
        LL_INFOS("VkTexRec") << "mode " << LLTexRecs::sMode << ": " << st.mRecords << " face records in " << st.mSlots << " texture slots; in 10 s: " << st.mDeltas - ps.mDeltas << " records sent in "
                             << st.mBatches - ps.mBatches << " batches, " << st.mPasses - ps.mPasses << " worker passes (the last " << st.mPassMs << " ms over " << st.mPassRecords << " records) | the face loop against the worker: compared "
                             << sRecV.mCompared - pv.mCompared << " (loop's answer a kept one " << sRecV.mKept - pv.mKept << "), sizes differ by more than a quarter " << sRecV.mSizeDiff - pv.mSizeDiff << " (worker larger "
                             << sRecV.mWorkerBigger - pv.mWorkerBigger << ", loop's a kept one " << sRecV.mKeptDiff - pv.mKeptDiff << "), all-occluded differs " << sRecV.mOccDiff - pv.mOccDiff << " | not comparable: no result "
                             << sRecV.mNone - pv.mNone << ", stale " << sRecV.mStale - pv.mStale << ", volatile " << sRecV.mVolatile - pv.mVolatile << ", over 1024 faces " << sRecV.mBig - pv.mBig << ", other face count "
                             << sRecV.mCountDiff - pv.mCountDiff << " | main thread: frame() " << (F64)(st.mFrameNs - ps.mFrameNs) / 1.0e6 / llmax<U64>(st.mFrames - ps.mFrames, 1) << " ms a frame, group events "
                             << (F64)(st.mGroupNs - ps.mGroupNs) / 1.0e6 / llmax<U64>(st.mFrames - ps.mFrames, 1) << " ms a frame (" << st.mGroupEvents - ps.mGroupEvents << " events), faceChanged calls "
                             << st.mFaceChangedCalls - ps.mFaceChangedCalls << ", records not sent " << st.mSkipped - ps.mSkipped
                             << " | sized from the worker's result " << sRecV.mUsed - pv.mUsed << ", kept the face loop " << sRecV.mFellBack - pv.mFellBack << LL_ENDL;
        if (getenv("LL_VK_TEXREC_WHY"))
        {
            LL_INFOS("VkTexRecWhy") << "in 10 s, kept the face loop because: no result " << sRecV.mFbNone - pv.mFbNone << ", stale (another face set) " << sRecV.mFbStale - pv.mFbStale << ", volatile faces " << sRecV.mFbVolatile - pv.mFbVolatile
                                    << ", another number of faces " << sRecV.mFbCount - pv.mFbCount << ", over the face limit " << sRecV.mFbBig - pv.mFbBig
                                    << " | by whether the texture has an image of its own: LOADED visits " << sRecV.mVisLoaded - pv.mVisLoaded << " (faces in them " << sRecV.mFacesLoaded - pv.mFacesLoaded << ", from the worker "
                                    << sRecV.mUsedLoaded - pv.mUsedLoaded << ", kept the loop " << sRecV.mFbLoaded - pv.mFbLoaded << "), UNLOADED visits " << sRecV.mVisUnloaded - pv.mVisUnloaded << " (faces in them "
                                    << sRecV.mFacesUnloaded - pv.mFacesUnloaded << ", from the worker " << sRecV.mUsedUnloaded - pv.mUsedUnloaded << ", kept the loop " << sRecV.mFbUnloaded - pv.mFbUnloaded << ")" << LL_ENDL;
        }
        pv = sRecV;
        ps = st;
    }
    last_log = gFrameTimeSeconds;
}

///////////////////////////////////////////////////////////////////////////////


// <FS> Where the textures are: how many have data, how many wait for it (being fetched, waiting to be created as GL textures), how many the server refused, what the viewer thinks they cost
// and the discard bias it works against. Next to the VkTex line of the Vulkan layer (the video memory the device really holds): a face that stays on the placeholder in a busy region shows up
// here as a long queue or a count of refused textures.
static std::string memory_by_consumer_text(U64 texture_bytes, U64 os_used);     // (below)

void LLViewerTextureList::logSummary()
{
    U32 total = 0, with_gl = 0, no_data = 0, fetching = 0, missing = 0;
    for (const LLPointer<LLViewerFetchedTexture>& image : mImageList)
    {
        ++total;
        if (image->hasImageTexture())
        {
            ++with_gl;
        }
        if (image->isMissingAsset())
        {
            ++missing;
        }
        else if (image->getDiscardLevel() < 0)
        {
            ++no_data;
            if (image->isFetching())
            {
                ++fetching;
            }
        }
    }
    LL_DIAG("VkTex") << "texture list: " << total << " images, " << with_gl << " with a GL texture, " << no_data << " with no data yet (" << fetching << " being fetched), " << missing
        << " refused by the server (missing), " << mCreateTextureList.size() << " decoded and waiting to become GL textures, " << mDownScaleQueue.size() << " to downscale; discard bias "
        << LLViewerTexture::sDesiredDiscardBias << "; the viewer counts " << LLImageTex::getTextureBytesAllocated() / (1024 * 1024) << " MB of textures and "
        << LLVertexBuffer::getBytesAllocated() / (1024 * 1024) << " MB of vertex buffers against a free budget of " << LLViewerTexture::sFreeVRAMMegabytes << " MB";
    {
        U64 vk_used = 0, vk_budget = 0;
        if (LLVkBridge::videoMemory(vk_used, vk_budget))
        {
            LL_CONT << "; the OS sees " << vk_used / (1024 * 1024) << " MB of video memory in use of a budget of " << vk_budget / (1024 * 1024) << " MB";
        }
    }
    LL_CONT << LL_ENDL;
    {
        // doc/vulkan/EXPIRY-design.md section 11: how long ago a face of each texture in memory was last on screen (the dormant list only skips occluded and faceless textures, so it does not say this),
        // with the bytes the viewer counts for each age. The memory that a teleport leaves behind shows here; the bias logic does not look at it (it lowers everything's quality by the percentage over budget).
        static U32 last_frame = 0;
        static LLTimer last_time;
        const U32 frame_now = (U32)gFrameCount  /* (the counter the texture passes stamp with) */;
        const F32 seconds = last_time.getElapsedTimeF32();
        const double spf = last_frame && frame_now > last_frame ? (double)seconds / (double)(frame_now - last_frame) : 0.0166;
        last_frame = frame_now;
        last_time.reset();
        static const char* const names[6] = { "group accepted < 1 s", "1-10 s ago", "10-60 s", "60-300 s", "> 300 s", "no face / never" };
        U32 count[6] = {};
        U64 bytes[6] = {};
        U32 dormant_count = 0;
        U64 dormant_bytes = 0;
        for (const LLPointer<LLViewerFetchedTexture>& image : mImageList)
        {
            if (!image->hasImageTexture())
            {
                continue;
            }
            // the freshest of the groups its faces sit in: the latest frame any camera (world, cascades, probes, mirrors) accepted one of them. A texture with a face in no group (UI, avatar parts) counts as in use.
            S32 latest = 0;
            bool no_group = false;
            for (U32 ch = 0; ch < LLRender::NUM_TEXTURE_CHANNELS && latest < (S32)gFrameCount; ++ch)
            {
                const LLViewerTexture::ll_face_list_t* faces = image->getFaceList(ch);
                const S32 n_faces = image->getNumFaces(ch);
                for (S32 i = 0; i < n_faces && i < (S32)faces->size(); ++i)
                {
                    const LLFace* face = (*faces)[i];
                    const LLDrawable* drawable = face ? face->getDrawable() : nullptr;
                    const LLSpatialGroup* group = drawable ? drawable->getSpatialGroup() : nullptr;
                    if (!group)
                    {
                        no_group = true;
                        continue;
                    }
                    for (S32 c = 0; c < LLViewerCamera::NUM_CAMERAS; ++c)
                    {
                        latest = std::max(latest, group->getVisible((LLViewerCamera::eCameraID)c));
                    }
                }
            }
            int b = 5;
            if (no_group)
            {
                b = 0;
            }
            else if (latest > 0)
            {
                const double age = (double)(LLViewerOctreeEntryData::getCurrentFrame() - std::min(LLViewerOctreeEntryData::getCurrentFrame(), latest)) * spf;
                b = age < 1.0 ? 0 : age < 10.0 ? 1 : age < 60.0 ? 2 : age < 300.0 ? 3 : 4;
            }
            const U64 mem = (U64)image->getTextureMemory().value();
            ++count[b];
            bytes[b] += mem;
            if (image->mListState == 2)
            {
                ++dormant_count;
                dormant_bytes += mem;
            }
        }
        std::string text;
        for (int k = 0; k < 6; ++k)
        {
            text += llformat(" [%s: %u textures, %.0f MB]", names[k], count[k], (double)bytes[k] / 1048576.0);
        }
        LL_DIAG("VkTex") << "texture memory by time since a group of its faces was accepted by a camera:" << text << "; of all of them on the dormant list: " << dormant_count << " textures, " << (dormant_bytes >> 20) << " MB" << LL_ENDL;
        {
            U64 vk_used2 = 0, vk_budget2 = 0;
            U64 texture_total = 0;
            for (int k = 0; k < 6; ++k) texture_total += bytes[k];
            if (LLVkBridge::videoMemory(vk_used2, vk_budget2))
            {
                const std::string by_consumer = memory_by_consumer_text(texture_total, vk_used2);
                if (!by_consumer.empty()) LL_DIAG("VkTex") << by_consumer << LL_ENDL;
                const std::string census = LLVkBridge::accelCensus();
                if (!census.empty()) LL_DIAG("VkTex") << census << LL_ENDL;
            }
        }
    }
    {
        // census (clothing of the crowd that never loads: 65-156 images with no data and nothing fetching, in every run since before the scene table): what those images are and why nothing asks for them
        U32 vsize_zero = 0, vsize_pos = 0, with_faces = 0, wants_data = 0, deleted = 0, listed = 0;
        std::map<S32, U32> by_boost;
        std::map<S32, U32> by_ft;
        std::string samples;
        for (const LLPointer<LLViewerFetchedTexture>& image_ptr : mImageList)
        {
            LLViewerFetchedTexture* image = const_cast<LLViewerFetchedTexture*>(image_ptr.get());
            if (image->isMissingAsset() || image->getDiscardLevel() >= 0 || image->isFetching())
            {
                continue;
            }
            ++listed;
            const F32 vsize = image->getMaxVirtualSize();
            ++(vsize > 0.f ? vsize_pos : vsize_zero);
            const U32 faces = image->getNumFaces(LLRender::DIFFUSE_MAP);
            with_faces += faces ? 1 : 0;
            wants_data += image->getDesiredDiscardLevel() >= 0 && image->getDesiredDiscardLevel() < MAX_DISCARD_LEVEL ? 1 : 0;
            deleted += image->isDeleted() ? 1 : 0;
            ++by_boost[image->getBoostLevel()];
            ++by_ft[(S32)image->getFTType()];
            if (listed <= 8)
            {
                samples += llformat(" [%s boost %d type %d vsize %.0f faces %u desired %d fetch priority %u; faces:", image->getID().asString().c_str(), image->getBoostLevel(), (S32)image->getFTType(), vsize, faces,
                                    (S32)image->getDesiredDiscardLevel(), image->getFetchPriority());
                const LLViewerTexture::ll_face_list_t* list = image->getFaceList(LLRender::DIFFUSE_MAP);
                U32 shown = 0;
                // (only the first getNumFaces entries are faces: the vector has slack behind them, and removeFace leaves the old last pointer in its slot, a pointer to a face that may be freed)
                const S32 face_count = image->getNumFaces(LLRender::DIFFUSE_MAP);
                for (S32 face_index = 0; face_index < face_count && face_index < (S32)list->size(); ++face_index)
                {
                    LLFace* face = (*list)[face_index];
                    if (!face || ++shown > 3)
                    {
                        continue;
                    }
                    const LLViewerObject* object = face->getViewerObject();
                    const LLDrawable* drawable = face->getDrawable();
                    samples += llformat(" (pixel area %.1f, %s%s%s%s)", face->getPixelArea(), drawable ? (drawable->isVisible() ? "visible" : "not visible") : "no drawable",
                                        object && object->isRiggedMesh() ? ", rigged mesh" : "", object && object->isAttachment() ? ", attachment" : "", object && object->isAvatar() ? ", avatar" : "");
                }
                samples += "]";
            }
        }
        std::string boost_text, ft_text;
        for (const auto& kv : by_boost) boost_text += " " + std::to_string(kv.first) + ":" + std::to_string(kv.second);
        for (const auto& kv : by_ft) ft_text += " " + std::to_string(kv.first) + ":" + std::to_string(kv.second);
        LL_DIAG("VkTex") << "images with no data and not fetching: " << listed << " (virtual size 0: " << vsize_zero << ", above 0: " << vsize_pos << "; with faces on a diffuse map: " << with_faces << "; a desired discard level set: "
            << wants_data << "; deleted: " << deleted << "); by boost level" << boost_text << "; by fetched texture type" << ft_text << "; first ones:" << samples << LL_ENDL;
    }
    extern U32 gEarlySortResorts;
    LL_DIAG("VkTex") << "the early sort was redone in " << gEarlySortResorts << " frames since the start" << LL_ENDL;
}

// <FS> doc/vulkan/EXPIRY-design.md: where the video memory the OS reports goes. Textures are the per-texture sums of the images with a GL texture; the rest are the arena blocks (bytes alive, bytes in use), the
// acceleration structure blocks and the structures in them, the idle blocks the pool holds, and the vertex buffers the viewer counts. "Remainder" is what these do not name (render targets, DLSS, mirrored
// textures, staging, the driver's own).
static std::string memory_by_consumer_text(U64 texture_bytes, U64 os_used)
{
    LLVkBridge::MemoryByConsumer m;
    if (!LLVkBridge::memoryByConsumer(m))
    {
        return std::string();
    }
    const U64 vb = (U64)LLVertexBuffer::getBytesAllocated();
    const U64 named = texture_bytes + m.mBlockBytes + m.mHostBytes + m.mAccelBytes + vb;
    auto mb = [](U64 b) { return (unsigned long long)(b >> 20); };
    return llformat("memory by consumer (MB): OS %llu = textures %llu + arena blocks %llu (in use %llu, %u blocks) + host-written %llu (in use %llu, %u blocks, in system memory or video) + acceleration structure blocks %llu (in use %llu, structures %llu, idle in the pool %llu, %u blocks) + vertex buffers %llu + remainder %lld",
        mb(os_used), mb(texture_bytes), mb(m.mBlockBytes), mb(m.mBlockUsed), m.mBlocks, mb(m.mHostBytes), mb(m.mHostUsed), m.mHostBlocks, mb(m.mAccelBytes), mb(m.mAccelUsed), mb(m.mRtStructureBytes), mb(m.mAccelPooledBytes), m.mAccelBlocks,
        mb(vb), (long long)(os_used >> 20) - (long long)(named >> 20));
}

void LLViewerTextureList::updateImages(F32 max_time)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    LLTexRetire::pump();        // <FS> C2.2: destroy the textures parked long enough
    {
        // <FS> diagnostic (2026-11-14, Adam: system memory bumps of several GB at a login and at a teleport while video memory had room; remove once the cause is known): where the image data sits. A line (tag
        // ImgMem) when the image data changed by 50 MB or more since the last line, else every 10 s: all image data held by LLImageBase (compressed and raw), the compressed part, the decode queue, the decoded
        // images waiting to become textures, the fetch requests, and the textures the viewer counts.
        static LLTimer img_mem_timer;
        static S64 img_mem_last = 0;
        static U32 img_mem_quiet = 0;
        if (img_mem_timer.getElapsedTimeF32() >= 1.f)
        {
            img_mem_timer.reset();
            const S64 total = LLImageBase::getGlobalDataBytes();
            if (llabs(total - img_mem_last) >= 50 * 1024 * 1024 || ++img_mem_quiet >= 10)
            {
                img_mem_quiet = 0;
                img_mem_last = total;
                const S64 mb = 1024 * 1024;
                const S64 formatted = LLImageFormatted::sGlobalFormattedMemory;
                LLImageDecodeThread* decode = LLAppViewer::getImageDecodeThread();
                LLTextureFetch* fetch = LLAppViewer::getTextureFetch();
                U64 host_used = 0, host_ours = 0;
                LLVkBridge::hostMemory(host_used, host_ours);
                LL_DIAG("ImgMem") << "Vulkan host heap " << host_used / (1024 * 1024) << " MB used by this process (ours " << host_ours / (1024 * 1024) << " MB) | image data " << total / mb << " MB (compressed " << formatted / mb << " MB, raw about " << (total - formatted) / mb << " MB), decode queue " << (decode ? decode->getPending() : 0)
                    << ", decoded and waiting to become textures " << mCreateTextureList.size() << ", fetch requests " << (fetch ? fetch->getNumRequests() : 0) << ", textures counted " << LLImageTex::getTextureBytesAllocated() / mb << " MB" << LL_ENDL;
            }
        }
    }
    {
        static U32 calls = 0;
        if (++calls >= 1800)
        {
            calls = 0;
            logSummary();
        }
    }
    static bool cleared = false;
    if(gTeleportDisplay)
    {
        if(!cleared)
        {
            clearFetchingRequests();
            gPipeline.clearRebuildGroups();
            cleared = true;
            return;
        }
        // ARRIVING is a delay to let things decode, cache and process,
        // so process textures like normal despite gTeleportDisplay
        if (gAgent.getTeleportState() != LLAgent::TELEPORT_ARRIVING)
        {
            return;
        }
    }
    else
    {
        cleared = false;
    }

    LLAppViewer::getTextureFetch()->setTextureBandwidth((F32)LLTrace::get_frame_recording().getPeriodMeanPerSec(LLStatViewer::TEXTURE_NETWORK_DATA_RECEIVED).value());

    {
        using namespace LLStatViewer;
        sample(NUM_IMAGES, sNumImages);
        sample(NUM_RAW_IMAGES, LLImageRaw::sRawImageCount);
        sample(FORMATTED_MEM, F64Bytes(LLImageFormatted::sGlobalFormattedMemory));
    }

    // make sure each call below gets at least its "fair share" of time
    F32 min_time = max_time * 0.33f;
    F32 remaining_time = max_time;

    //loading from fast cache
    { LL_CPU_ZONE("textures: fast cache"); remaining_time -= updateImagesLoadingFastCache(remaining_time); }
    remaining_time = llmax(remaining_time, min_time);

    //dispatch to texture fetch threads
    { LL_CPU_ZONE("textures: fetch dispatch"); remaining_time -= updateImagesFetchTextures(remaining_time); }
    remaining_time = llmax(remaining_time, min_time);

    //handle results from decode threads
    { LL_CPU_ZONE("textures: create from decoded"); updateImagesCreateTextures(remaining_time); }

    LL_CPU_ZONE("textures: loaded callbacks + stats");
    bool didone = false;
    for (image_list_t::iterator iter = mCallbackList.begin();
        iter != mCallbackList.end(); )
    {
        //trigger loaded callbacks on local textures immediately
        LLViewerFetchedTexture* image = *iter++;
        if (!image->getUrl().empty())
        {
            // Do stuff to handle callbacks, update priorities, etc.
            didone = image->doLoadedCallbacks();
        }
        else if (!didone)
        {
            // Do stuff to handle callbacks, update priorities, etc.
            didone = image->doLoadedCallbacks();
        }
    }

    updateImagesUpdateStats();
}

void LLViewerTextureList::clearFetchingRequests()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (LLAppViewer::getTextureFetch()->getNumRequests() == 0)
    {
        return;
    }

    LLAppViewer::getTextureFetch()->deleteAllRequests();

    for (image_list_t::iterator iter = mImageList.begin();
         iter != mImageList.end(); ++iter)
    {
        LLViewerFetchedTexture* imagep = *iter;
        imagep->forceToDeleteRequest() ;
    }
}

extern bool gCubeSnapshot;

namespace
{
// <FS> native-vk: LL_VK_TEXSTAMP (default 1; 0 is off) keeps the result of the face loop of updateImageDecodePriority for textures whose faces are all static and unchanged while the camera stays in one cell of a coarse
// grid (and the bias in one bucket), for LL_VK_TEXSTAMP_AGE seconds (default 8); =2 runs the loop anyway and counts the cases where it finds something other than the kept result.
const S32 sStampMode = []{ const char* e = getenv("LL_VK_TEXSTAMP"); return e ? atoi(e) : 1; }();
const F32 sStampAge = []{ const char* e = getenv("LL_VK_TEXSTAMP_AGE"); return e ? (F32)atof(e) : 8.f; }();
struct StampStats
{
    U64 mHit = 0, mMissNew = 0, mMissFaces = 0, mMissCamera = 0, mMissBias = 0, mMissAge = 0, mVolatile = 0, mVerified = 0, mMismatch = 0, mMismatchFresh = 0, mMismatchOld = 0, mMismatchFaceCount = 0, mOffScreen = 0;
    U64 mWhyTe = 0, mWhyGeom = 0, mWhyNeither = 0;
    U64 mWhyGeomKind[5] = {};       // a face that moved without telling the texture, by kind of object: attachment, avatar, tree or grass, prim, other
};
StampStats sStamp;
F32 sStampEpochTime = 0.f;      // gFrameTimeSeconds when the camera epoch last changed: a result is only kept once the camera has stood still for a moment (the face areas it read were then made under that pose, not up to 0.1 s earlier)
}

// <FS> LL_VK_TEX_PRE (default 0 = off; 1 = on; the face loops otherwise stay on this thread, 2 = the loops run here as before and are compared with the precomputed ones): the face loops of the textures of a slice that the worker's record
// and the kept result cannot answer run as pool jobs before the slice's per-texture pass; updateImageDecodePriority then takes the precomputed values where it would have run the loop. A texture with a face that
// is due a pixel area refresh (calcPixelArea writes the face, which two textures can share) is left to this thread.
struct FaceLoopPre
{
    bool mValid = false;
    U32 mFaceCount = 0;
    F32 mMaxVsize = 0.f;
    bool mOnScreen = false;
    U32 mFacesSeen = 0, mFacesOccluded = 0;
    bool mVolatile = false;
};
extern thread_local bool gFSTexPreJob;          // llface.cpp: set while a texture priority job runs
extern std::atomic<U32> gFSTexPreDirtyReads;    // llface.cpp: dirty joints a job met in calcPixelArea of a rigged face
static const FaceLoopPre* sCurrentPre = nullptr;     // main thread: the precomputed loop of the texture being looked at
static const int sPreMode = []{ const char* e = getenv("LL_VK_TEX_PRE"); return e ? atoi(e) : 1; }();       // on by default since 2026-11-01 (plan 8.26: the rigged faces are in the jobs, their joints cleaned first); 0 = the loops on this thread, 2 = verify
static U64 sPreCandidates = 0, sPreComputed = 0, sPreUsed = 0, sPreCompared = 0, sPreDiffer = 0, sPreFrames = 0, sPreEntries = 0;

void LLViewerTextureList::updateImageDecodePriority(LLViewerFetchedTexture* imagep, bool flush_images)
{
    llassert(!gCubeSnapshot);

    // <FS> H6: this pass's own addTextureStats calls do not wake a dormant texture (see LLViewerTexture::addTextureStats)
    struct ListUpdateScope { ListUpdateScope() { sInListUpdate = true; } ~ListUpdateScope() { sInListUpdate = false; } } list_update_scope;
    bool idx_dormant = false;
    constexpr F32 BIAS_TRS_OUT_OF_SCREEN = 1.5f;
    constexpr F32 BIAS_TRS_ON_SCREEN = 1.f;

    if (imagep->getBoostLevel() < LLViewerFetchedTexture::BOOST_HIGH)  // don't bother checking face list for boosted textures
    {
        static LLCachedControl<F32> texture_scale_min(gSavedSettings, "TextureScaleMinAreaFactor", 0.0095f);
        static LLCachedControl<F32> texture_scale_max(gSavedSettings, "TextureScaleMaxAreaFactor", 25.f);

        F32 max_vsize = 0.f;
        bool on_screen = false;

        U32 face_count = 0;
        U32 max_faces_to_check = 1024;
        U32 faces_seen = 0, faces_occluded = 0;     // <FS> H6: the faces the loop looked at, and how many of them are in an occluded group
        bool all_occluded = false;

        // <FS> native-vk (LL_VK_TEXSTAMP): the face loop is skipped while what it read is unchanged (see sStampMode above)
        bool stamp_use = false, stamp_verify = false, stamp_volatile = false;
        U64 why_te = 1469598103934665603ull, why_geom = 1469598103934665603ull;
        S32 why_kind = 4;
        const LLTextureEntry* why_te_entry = nullptr;
        const S32 stamp_bias = (S32)(LLViewerTexture::sDesiredDiscardBias * 20.f);
        LLViewerTexture::FaceLoopCache& stamp = imagep->mFaceCache;
        if (sStampMode)
        {
            if (!stamp.mValid) ++sStamp.mMissNew;
            else if (stamp.mFaceStamp != imagep->mFaceStamp) ++sStamp.mMissFaces;
            else if (stamp.mCamEpoch != sCameraEpoch) ++sStamp.mMissCamera;
            else if (stamp.mBias != stamp_bias) ++sStamp.mMissBias;
            else if (gFrameTimeSeconds - stamp.mTime >= sStampAge) ++sStamp.mMissAge;
            else if (sStampMode == 1) { stamp_use = true; ++sStamp.mHit; }
            else { stamp_verify = true; ++sStamp.mHit; }
        }

        const FaceLoopPre* pre = (sCurrentPre && sCurrentPre->mValid) ? sCurrentPre : nullptr;       // <FS> LL_VK_TEX_PRE
        bool loop_ran = false;

        // get adjusted bias based on image resolution
        LLImageTex* img = imagep->getImageTexture();
        F32 max_discard = F32(img ? img->getMaxDiscardLevel() : MAX_DISCARD_LEVEL);
        F32 bias = llclamp(max_discard - 2.f, 1.f, LLViewerTexture::sDesiredDiscardBias);

        // convert bias into a vsize scaler
        bias = (F32) llroundf(powf(4, bias - 1.f));

        // <FS> H6 (LL_VK_TEXREC=2): the worker's result for this texture replaces the face loop when it was made for the faces the texture has now (the stamp), none of them is volatile and the
        // number of faces is what the worker counted
        bool rec_use = false;
        LLTexRecs::Result rec_res;
        const bool rec_have = LLTexRecs::sMode == 2 && LLTexRecs::result(imagep, rec_res);
        if (rec_have && rec_res.mStamp == imagep->mFaceStamp && !rec_res.mVolatile)
        {
            U32 fc = 0;
            for (U32 i = 0; i < LLRender::NUM_TEXTURE_CHANNELS; ++i)
            {
                fc += imagep->getNumFaces(i);
            }
            if (fc == rec_res.mFaces && fc <= max_faces_to_check)
            {
                rec_use = true;
                face_count = fc;
                max_vsize = llmin(llmax(rec_res.mMaxPlain, rec_res.mMaxBiased / bias), LLViewerFetchedTexture::sMaxVirtualSize);
                on_screen = fc > 0;                 // (every plain face counts as on screen: see calcPixelArea)
                all_occluded = fc > 0 && rec_res.mOccluded == fc;
                ++sRecV.mUsed;
            }
        }
        if (LLTexRecs::sMode == 2 && !rec_use) ++sRecV.mFellBack;
        static const bool rec_why = getenv("LL_VK_TEXREC_WHY") != nullptr;
        if (rec_why && LLTexRecs::sMode == 2)
        {
            const bool loaded = img && img->getTexName() != 0;        // (no image of its own yet: the records name the shared default image)
            U32 fcnt = 0;
            for (U32 i = 0; i < LLRender::NUM_TEXTURE_CHANNELS; ++i)
            {
                fcnt += imagep->getNumFaces(i);
            }
            ++(loaded ? sRecV.mVisLoaded : sRecV.mVisUnloaded);
            (loaded ? sRecV.mFacesLoaded : sRecV.mFacesUnloaded) += fcnt;
            if (rec_use)
            {
                ++(loaded ? sRecV.mUsedLoaded : sRecV.mUsedUnloaded);
            }
            else
            {
                ++(loaded ? sRecV.mFbLoaded : sRecV.mFbUnloaded);
                if (!rec_have) ++sRecV.mFbNone;
                else if (rec_res.mStamp != imagep->mFaceStamp) ++sRecV.mFbStale;
                else if (rec_res.mVolatile) ++sRecV.mFbVolatile;
                else if (fcnt != rec_res.mFaces) ++sRecV.mFbCount;
                else ++sRecV.mFbBig;
            }
        }

        LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
        if (rec_use)
        {
        }
        else if (stamp_use)
        {
            max_vsize = stamp.mVSize;
            all_occluded = stamp.mAllOccluded;
            on_screen = stamp.mOnScreen;
            face_count = stamp.mFaceCount;
        }
        else if (pre && sPreMode == 1)
        {
            // <FS> the loop ran as a pool job
            face_count = pre->mFaceCount;
            max_vsize = pre->mMaxVsize;
            on_screen = pre->mOnScreen;
            faces_seen = pre->mFacesSeen;
            faces_occluded = pre->mFacesOccluded;
            stamp_volatile = pre->mVolatile;
            ++sPreUsed;
        }
        else
        { LL_CPU_ZONE("textures: decode priority: face loop");
        loop_ran = true;
        for (U32 i = 0; i < LLRender::NUM_TEXTURE_CHANNELS; ++i)
        {
            face_count += imagep->getNumFaces(i);
            S32 faces_to_check = (face_count > max_faces_to_check) ? 0 : imagep->getNumFaces(i);

            for (S32 fi = 0; fi < faces_to_check; ++fi)
            {
                LLFace* face = (*(imagep->getFaceList(i)))[fi];

                if (face && face->getViewerObject())
                {
                    ++faces_seen;
                    {
                        LLDrawable* drw = face->getDrawable();
                        LLSpatialGroup* grp = drw ? drw->getSpatialGroup() : nullptr;
                        if (grp && grp->isOcclusionState(LLOcclusionCullingGroup::OCCLUDED)) ++faces_occluded;
                    }
                    F32 radius;
                    F32 cos_angle_to_view_dir;

                    if (sStampMode && (face->isState(LLFace::RIGGED) || face->hasMedia() || !face->getDrawable() || face->getDrawable()->isActive()
                                       || face->getViewerObject()->getPartitionType() == LLViewerRegion::PARTITION_PARTICLE))
                    {
                        stamp_volatile = true;        // its position comes from something that moves without telling the texture
                    }

                    if ((gFrameCount - face->row().lastTextureUpdate) > 10)
                    { // only call calcPixelArea at most once every 10 frames for a given face
                        // this helps eliminate redundant calls to calcPixelArea for faces that have multiple textures
                        // assigned to them, such as is the case with GLTF materials or Blinn-Phong materials
                        face->row().inFrustum = face->calcPixelArea(cos_angle_to_view_dir, radius);
                        face->row().lastTextureUpdate = gFrameCount;
                    }

                    F32 vsize = face->getPixelArea();

                    on_screen |= face->row().inFrustum;

                    // Scale desired texture resolution higher or lower depending on texture scale
                    //
                    // Minimum usage examples: a 1024x1024 texture with aplhabet (texture atlas),
                    // runing string shows one letter at a time. If texture has ten 100px symbols
                    // per side, minimal scale is (100/1024)^2 = 0.0095
                    //
                    // Maximum usage examples: huge chunk of terrain repeats texture
                    // TODO: make this work with the GLTF texture transforms
                    S32 te_offset = face->getTEOffset();  // offset is -1 if not inited
                    LLViewerObject* objp = face->getViewerObject();
                    const LLTextureEntry* te = (te_offset < 0 || te_offset >= objp->getNumTEs()) ? nullptr : objp->getTE(te_offset);
                    if (sStampMode == 2)
                    {
                        if (te)
                        {
                            const F32 sc[2] = { te->getScaleS(), te->getScaleT() };
                            for (F32 f : sc) { U32 bits; memcpy(&bits, &f, 4); why_te = (why_te ^ bits) * 1099511628211ull; }
                        }
                        const LLVector3 c = face->getPositionAgent();
                        const F32 gv[9] = { c.mV[0], c.mV[1], c.mV[2], face->row().extents[0].getF32ptr()[0], face->row().extents[0].getF32ptr()[1], face->row().extents[0].getF32ptr()[2],
                                            face->row().extents[1].getF32ptr()[0], face->row().extents[1].getF32ptr()[1], face->row().extents[1].getF32ptr()[2] };
                        for (F32 f : gv) { U32 bits; memcpy(&bits, &f, 4); why_geom = (why_geom ^ bits) * 1099511628211ull; }
                        const LLViewerObject* wo = face->getViewerObject();
                        why_kind = wo->isAttachment() ? 0 : (wo->isAvatar() ? 1 : ((wo->getPCode() == LL_PCODE_LEGACY_TREE || wo->getPCode() == LL_PCODE_LEGACY_GRASS) ? 2 : (wo->getPCode() == LL_PCODE_VOLUME ? 3 : 4)));
                        why_te_entry = te;
                    }
                    F32 min_scale = te ? llmin(fabsf(te->getScaleS()), fabsf(te->getScaleT())) : 1.f;
                    min_scale = llclamp(min_scale * min_scale, texture_scale_min(), texture_scale_max());
                    vsize /= min_scale;

                    // apply bias to offscreen faces all the time, but only to onscreen faces when bias is large
                    // use mImportanceToCamera to make bias switch a bit more gradual
                    if (!face->row().inFrustum || LLViewerTexture::sDesiredDiscardBias > 1.9f + face->row().importanceToCamera / 2.f)
                    {
                        vsize /= bias;
                    }

                    // boost resolution of textures that are important to the camera
                    if (face->row().inFrustum)
                    {
                        static LLCachedControl<F32> texture_camera_boost(gSavedSettings, "TextureCameraBoost", 8.f);
                        vsize *= llmax(face->row().importanceToCamera*texture_camera_boost, 1.f);
                    }

                    max_vsize = llmax(max_vsize, vsize);

                    // addTextureStats limits size to sMaxVirtualSize
                    if (max_vsize >= LLViewerFetchedTexture::sMaxVirtualSize
                        && (on_screen || LLViewerTexture::sDesiredDiscardBias <= BIAS_TRS_ON_SCREEN))
                    {
                        break;
                    }
                }
            }

            if (max_vsize >= LLViewerFetchedTexture::sMaxVirtualSize
                && (on_screen || LLViewerTexture::sDesiredDiscardBias <= BIAS_TRS_ON_SCREEN))
            {
                break;
            }
        }
        } // (face loop zone)
        if (loop_ran && pre && sPreMode == 2)
        {
            // <FS> LL_VK_TEX_PRE=2: the loop above ran here; the pool's answer for the same texture has to be the same
            ++sPreCompared;
            U32 pre_bits, loop_bits;
            memcpy(&pre_bits, &pre->mMaxVsize, 4);
            memcpy(&loop_bits, &max_vsize, 4);
            // (the pool's threads keep denormals, this thread flushes them to zero: a size of 1e-42 and one of 0 are the same size)
            const bool same_size = pre_bits == loop_bits || (fabsf(pre->mMaxVsize) < 1.0e-30f && fabsf(max_vsize) < 1.0e-30f);
            const bool same = pre->mFaceCount == face_count && same_size && pre->mOnScreen == on_screen && pre->mFacesSeen == faces_seen
                && pre->mFacesOccluded == faces_occluded && (!sStampMode || pre->mVolatile == stamp_volatile);
            if (!same)
            {
                ++sPreDiffer;
                static U32 said = 0;
                if (said++ < 40)
                {
                    LL_INFOS("TexPre") << "precomputed loop differs for " << imagep->getID() << ": faces " << pre->mFaceCount << "/" << face_count << ", size " << pre->mMaxVsize << "/" << max_vsize << ", on screen "
                                       << pre->mOnScreen << "/" << on_screen << ", seen " << pre->mFacesSeen << "/" << faces_seen << ", occluded " << pre->mFacesOccluded << "/" << faces_occluded
                                       << ", volatile " << pre->mVolatile << "/" << stamp_volatile << LL_ENDL;
                }
            }
        }

        if (!stamp_use && !rec_use)
        {
            all_occluded = face_count > 0 && face_count <= max_faces_to_check && faces_seen == face_count && faces_occluded == faces_seen;
        }
        if (sStampMode && !stamp_use && !rec_use)
        {
            // (a difference only counts when it matters: one side is a size the priority treats as seen, and the two are not within a quarter of each other or the screen flag changed)
            const F32 big = llmax(max_vsize, stamp.mVSize);
            if (stamp_verify && stamp.mValid && big >= 64.f && (on_screen != stamp.mOnScreen || fabsf(max_vsize - stamp.mVSize) > 0.25f * big))
            {
                ++sStamp.mMismatch;
                if (why_te != stamp.mTeHash) ++sStamp.mWhyTe;
                else if (why_geom != stamp.mGeomHash) { ++sStamp.mWhyGeom; ++sStamp.mWhyGeomKind[why_kind]; }
                else ++sStamp.mWhyNeither;
                if (face_count != stamp.mFaceCount) ++sStamp.mMismatchFaceCount;
                else if (gFrameTimeSeconds - stamp.mTime < 0.5f) ++sStamp.mMismatchFresh;
                else ++sStamp.mMismatchOld;
                static U32 said = 0;
                if (said++ < 60 && face_count == stamp.mFaceCount && gFrameTimeSeconds - stamp.mTime >= 0.5f)
                {
                    LL_INFOS("VkTexStamp") << "kept result differs for " << imagep->getID() << ": size " << stamp.mVSize << " -> " << max_vsize << ", on screen " << stamp.mOnScreen << " -> " << on_screen
                                           << ", faces " << stamp.mFaceCount << " -> " << face_count << ", age " << gFrameTimeSeconds - stamp.mTime << " s" << LL_ENDL;
                }
            }
            if (stamp_verify) ++sStamp.mVerified;
            // (a texture with faces whose loop found none on screen is not kept: for a static face that flag is true after a real evaluation, so a false one is a stale first reading)
            const bool unsettled = face_count > 0 && !on_screen && !stamp_volatile;
            if (unsettled) ++sStamp.mOffScreen;
            stamp.mValid = !stamp_volatile && !unsettled && face_count <= max_faces_to_check && gFrameTimeSeconds - sStampEpochTime > 0.3f;
            if (stamp_volatile) ++sStamp.mVolatile;
            stamp.mFaceStamp = imagep->mFaceStamp;
            stamp.mCamEpoch = sCameraEpoch;
            stamp.mBias = stamp_bias;
            stamp.mTime = gFrameTimeSeconds;
            stamp.mVSize = max_vsize;
            stamp.mOnScreen = on_screen;
            stamp.mFaceCount = face_count;
            stamp.mAllOccluded = all_occluded;
            stamp.mTeHash = why_te;
            stamp.mGeomHash = why_geom;
        }

        if (LLTexRecs::sMode == 1)
        {
            // <FS> H6: what the worker found for this texture (from the records of its faces) against what the face loop (or the kept result of it) found
            LLTexRecs::Result rr;
            if (!LLTexRecs::result(imagep, rr)) ++sRecV.mNone;
            else if (rr.mStamp != imagep->mFaceStamp) ++sRecV.mStale;
            else if (rr.mVolatile) ++sRecV.mVolatile;
            else if (face_count > max_faces_to_check) ++sRecV.mBig;
            else if (rr.mFaces != face_count) ++sRecV.mCountDiff;
            else
            {
                const F32 expect = llmin(llmax(rr.mMaxPlain, rr.mMaxBiased / bias), LLViewerFetchedTexture::sMaxVirtualSize);
                const F32 got = llmin(max_vsize, LLViewerFetchedTexture::sMaxVirtualSize);
                const bool occ_rec = rr.mFaces > 0 && rr.mOccluded == rr.mFaces;
                const F32 hi = llmax(expect, got), lo = llmin(expect, got);
                ++sRecV.mCompared;
                sRecV.mKept += stamp_use;
                if (occ_rec != all_occluded) ++sRecV.mOccDiff;
                if (hi >= 64.f && lo < 0.75f * hi)
                {
                    ++sRecV.mSizeDiff;
                    sRecV.mWorkerBigger += expect > got;
                    sRecV.mKeptDiff += stamp_use;
                }
            }
        }

        if (face_count > max_faces_to_check)
        { // this texture is used in so many places we should just boost it and not bother checking its vsize
            // this is especially important because the above is not time sliced and can hit multiple ms for a single texture
            max_vsize = MAX_IMAGE_AREA;
        }

        if (imagep->getType() == LLViewerTexture::LOD_TEXTURE && imagep->getBoostLevel() == LLViewerTexture::BOOST_NONE)
        { // conditionally reset max virtual size for unboosted LOD_TEXTURES
          // this is an alternative to decaying mMaxVirtualSize over time
          // that keeps textures from continously downrezzing and uprezzing in the background

            if (LLViewerTexture::sDesiredDiscardBias > BIAS_TRS_OUT_OF_SCREEN ||
                (!on_screen && LLViewerTexture::sDesiredDiscardBias > BIAS_TRS_ON_SCREEN))
            {
                imagep->mMaxVirtualSize = 0.f;
            }
        }

        imagep->addTextureStats(max_vsize);

        // <FS> H6: a texture nobody is looking at and nothing is asking for goes on the dormant list: no face on screen (none at all, or every face in an occluded group), no boost, no callback, not being
        // fetched, no light projection, and not woken in the last 5 s (a texture that something keeps calling addTextureStats on would go back and forth every frame)
        if (sIdxMode)
        {
            const bool why[6] = { imagep->getBoostLevel() != LLViewerTexture::BOOST_NONE, imagep->hasCallbacks(), imagep->isFetching(), imagep->getNumVolumes(LLRender::LIGHT_TEX) != 0,
                                  !(face_count == 0 || all_occluded), gFrameCount - imagep->mLastWakeFrame <= 300 };
            idx_dormant = !(why[0] || why[1] || why[2] || why[3] || why[4] || why[5]);
            if (imagep->mListState == 2 && !idx_dormant)
            {
                for (int k = 0; k < 6; ++k) sIdx.mMissWhy[k] += why[k];       // (for the verify count below: what made the texture wanted)
            }
        }
    }

    if (sIdxMode && imagep->mListState != 0)
    {
        if (idx_dormant && imagep->mListState == 1)
        {
            idxMove(imagep, 2);
            ++sIdx.mToDormant;
        }
        else if (!idx_dormant && imagep->mListState == 2)
        {
            if (sIdxMode == 2)
            {
                // verify: this texture was on the slow list, and nothing woke it: the walk of mode 1 would have found it late
                ++sIdx.mMisses;
                const U32 age = gFrameCount - imagep->mDormantAtFrame;
                ++sIdx.mMissAge[age < 60 ? 0 : (age < 600 ? 1 : 2)];
            }
            idxMove(imagep, 1);
            ++sIdx.mToActive;
        }
    }

#if 0
    imagep->setDebugText(llformat("%d/%d - %d/%d -- %d/%d",
        (S32)sqrtf(max_vsize),
        (S32)sqrtf(imagep->mMaxVirtualSize),
        imagep->getDiscardLevel(),
        imagep->getDesiredDiscardLevel(),
        imagep->getWidth(),
        imagep->getFullWidth()));
#endif

    // make sure to addTextureStats for any spotlights that are using this texture
    LL_CPU_ZONE("textures: decode priority: spotlights, refs, flush checks");
    for (S32 vi = 0; vi < imagep->getNumVolumes(LLRender::LIGHT_TEX); ++vi)
    {
        LLVOVolume* volume = (*imagep->getVolumeList(LLRender::LIGHT_TEX))[vi];
        volume->updateSpotLightPriority();
    }

    F32 max_inactive_time = 20.f; // inactive time before deleting saved raw image
    S32 min_refs = 3; // 1 for mImageList, 1 for mUUIDMap, and 1 for "entries" in updateImagesFetchTextures

    F32 lazy_flush_timeout = 30.f; // delete unused images after 30 seconds

    //
    // Flush formatted images using a lazy flush
    //
    S32 num_refs = imagep->getNumRefs();
    if (num_refs <= min_refs && flush_images)
    {
        if (imagep->getLastReferencedTimer()->getElapsedTimeF32() > lazy_flush_timeout)
        {
            // Remove the unused image from the image list
            deleteImage(imagep);
            return;
        }
    }
    else
    {
        // still referenced outside of image list, reset timer
        imagep->getLastReferencedTimer()->reset();

        if (imagep->hasSavedRawImage())
        {
            if (imagep->getElapsedLastReferencedSavedRawImageTime() > max_inactive_time)
            {
                imagep->destroySavedRawImage();
            }
        }

        if (imagep->isDeleted())
        {
            return;
        }
    }

    if (!imagep->isInImageList())
    {
        return;
    }
    if (imagep->isInFastCacheList())
    {
        return; //wait for loading from the fast cache.
    }

    LL_CPU_ZONE("textures: decode priority: processTextureStats");
    const S32 desired_before = imagep->getDesiredDiscardLevel();
    imagep->processTextureStats();
    {
        // <FS> H6: how often a visit changes the resolution a texture wants, and which way (a smaller discard level is a finer picture)
        const S32 desired_after = imagep->getDesiredDiscardLevel();
        ++sIdx.mDescVisits;
        if (desired_after != desired_before)
        {
            ++sIdx.mDescChanged;
            (desired_after < desired_before ? sIdx.mDescFiner : sIdx.mDescCoarser) += 1;
            sIdx.mDescAbs += (U64)abs(desired_after - desired_before);
        }
    }
}

F32 LLViewerTextureList::updateImagesCreateTextures(F32 max_time)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (gDeviceCaps.mIsDisabled) return 0.0f;

    //
    // Create GL textures for all textures that need them (images which have been
    // decoded, but haven't been pushed into GL).
    //

    LLTimer create_timer;
    S32 n_created = 0;              // <FS> arrival trace: this frame's create loop
    S64 bytes_created = 0;

    while (!mCreateTextureList.empty())
    {
        // Hold a smart pointer to keep the texture alive throughout processing,
        // even if side effects (e.g. pipeline rebuilds, GL operations) indirectly
        // cause other references to be released. (see: #5426)
        LLPointer<LLViewerFetchedTexture> imagep = mCreateTextureList.front();
        mCreateTextureList.pop();

        if (!imagep)
        {
            continue;
        }

        llassert(imagep->mCreatePending);

        // desired discard may change while an image is being decoded. If the texture in VRAM is sufficient
        // for the current desired discard level, skip the texture creation.  This happens more often than it probably
        // should
        bool redundant_load = imagep->hasImageTexture() && imagep->getDiscardLevel() <= imagep->getDesiredDiscardLevel();

        if (!redundant_load)
        {
           imagep->createTexture();
           ++n_created;
           if (imagep->getRawImage()) bytes_created += imagep->getRawImage()->getDataSize();
        }

        imagep->postCreateTexture();
        imagep->mCreatePending = false;

        if (imagep->hasImageTexture() && imagep->getDiscardLevel() < imagep->getDesiredDiscardLevel() &&
           (imagep->getDesiredDiscardLevel() <= MAX_DISCARD_LEVEL))
        {
            // NOTE: this may happen if the desired discard reduces while a decode is in progress and does not
            // necessarily indicate a problem, but if log occurrences excede that of dsiplay_stats: FPS,
            // something has probably gone wrong.
            LL_WARNS_ONCE("Texture") << "Texture will be downscaled immediately after loading." << LL_ENDL;
            imagep->scaleDown();
        }

        if (create_timer.getElapsedTimeF32() > max_time)
        {
            if (FSTextureArrivalTrace::enabled()) FSTextureArrivalTrace::note("createbreak", LLUUID::null, (S32)mCreateTextureList.size(), 0, 0, create_timer.getElapsedTimeF32() * 1000.f);
            break;
        }
    }

    if (n_created && FSTextureArrivalTrace::enabled())
    {
        FSTextureArrivalTrace::note("createloop", LLUUID::null, n_created, (S32)mCreateTextureList.size(), bytes_created, create_timer.getElapsedTimeF32() * 1000.f);
    }
    // <FS> native-vk: the downscale draws into a render target and copies the result into the texture's new storage. A minimized or hidden window has no frame to
    // record that into (the draw and the copy are dropped), which left the texture replaced by an empty one: hair and other textures were black after a minimize
    // until they were fetched again. So the queue waits (the entries stay: a restored window needs no downscale and each is then a no-op).
    const bool can_render = gViewerWindow && gViewerWindow->getWindow()->getVisible() && !gViewerWindow->getWindow()->getMinimized();
    if (can_render && !mDownScaleQueue.empty() && gPipeline.mDownResMap.isComplete())
    {
        LLRenderState::BlendScope blend(false);
        gRender.setColorMask(true, true);

        // just in case we downres textures, bind downresmap and copy program
        RenderPassScope pass(gPipeline.mDownResMap);
        gCopyProgram.bind();
        gPipeline.mScreenTriangleVB->setBuffer();

        // give time to downscaling first -- if mDownScaleQueue is not empty, we're running out of memory and need
        // to free up memory by discarding off screen textures quickly

        // do at least 5 and make sure we don't get too far behind even if it violates
        // the time limit.  If we don't downscale quickly the viewer will hit swap and may
        // freeze.
        S32 min_count = (S32)mCreateTextureList.size() / 20 + 5;

        create_timer.reset();
        while (!mDownScaleQueue.empty())
        {
            LLViewerFetchedTexture* image = mDownScaleQueue.front();
            llassert(image->mDownScalePending);

            LLImageTex* img = image->getImageTexture();
            if (img && img->getHasImageTexture())
            {
                img->scaleDown(image->getDesiredDiscardLevel());
            }

            image->mDownScalePending = false;
            mDownScaleQueue.pop();

            if (create_timer.getElapsedTimeF32() > max_time && --min_count <= 0)
            {
                break;
            }
        }

        gCopyProgram.unbind();
    }

    return create_timer.getElapsedTimeF32();
}

F32 LLViewerTextureList::updateImagesLoadingFastCache(F32 max_time)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (gDeviceCaps.mIsDisabled) return 0.0f;
    if(mFastCacheList.empty())
    {
        return 0.f;
    }

    //
    // loading texture raw data from the fast cache directly.
    //

    LLTimer timer;
    image_list_t::iterator enditer = mFastCacheList.begin();
    {
        // Prelock fast cache mutex to avoid waiting multiple times.
        LLMutexTrylock fast_cache_lock(LLAppViewer::getTextureCache()->getFastCacheMutex());
        if (!fast_cache_lock.isLocked())
        {
            // Cache is busy, skip this update cycle to avoid blocking the main thread.
            //
            // Generally fast cache operations are brief and rare in comparison to writing
            // main texture body, but if disk is busy, it can get stuck for multiple
            // seconds, waiting for that long is not practical.
            // But some variant of a timed try lock for 0.1ms or less might be optimal.
            return 0.0f;
        }
        for (image_list_t::iterator iter = mFastCacheList.begin();
            iter != mFastCacheList.end();)
        {
            image_list_t::iterator curiter = iter++;
            enditer = iter;
            LLViewerFetchedTexture* imagep = *curiter;
            imagep->loadFromFastCache();
            // <FS:Ansariel> Fast cache stats
            sNumFastCacheReads++;
            // </FS:Ansariel>
            if (timer.getElapsedTimeF32() > max_time)
                break;
        }
    }
    mFastCacheList.erase(mFastCacheList.begin(), enditer);
    return timer.getElapsedTimeF32();
}

void LLViewerTextureList::forceImmediateUpdate(LLViewerFetchedTexture* imagep)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if(!imagep || gCubeSnapshot)
    {
        return ;
    }

    imagep->processTextureStats();

    return ;
}

bool LLViewerTextureList::sViewSteady = false;
S32 LLViewerTextureList::sIdxMode = []{ const char* e = getenv("LL_VK_TEXIDX"); return e ? atoi(e) : 1; }();
bool LLViewerTextureList::sInListUpdate = false;
U32 LLViewerTextureList::sCameraEpoch = 0;


void LLViewerTextureList::updateCameraEpoch()
{
    if (!sStampMode)
    {
        return;
    }
    LLViewerCamera* cam = LLViewerCamera::getInstance();
    const LLVector3 o = cam->getOrigin(), a = cam->getAtAxis(), u = cam->getUpAxis();
    const S32 v[] = { (S32)floorf(o.mV[0] * 10.f), (S32)floorf(o.mV[1] * 10.f), (S32)floorf(o.mV[2] * 10.f),
                      (S32)floorf(a.mV[0] * 100.f), (S32)floorf(a.mV[1] * 100.f), (S32)floorf(a.mV[2] * 100.f),
                      (S32)floorf(u.mV[0] * 100.f), (S32)floorf(u.mV[1] * 100.f), (S32)floorf(u.mV[2] * 100.f),
                      (S32)(cam->getView() * 1000.f), (S32)(cam->getAspect() * 1000.f), (S32)(LLDrawable::sCurPixelAngle * 100.f),
                      (S32)gViewerWindow->getWindowWidthRaw(), (S32)gViewerWindow->getWindowHeightRaw() };
    U64 h = 1469598103934665603ull;
    for (S32 x : v)
    {
        h = (h ^ (U64)(U32)x) * 1099511628211ull;
    }
    static U64 last = 0;
    if (h != last)
    {
        last = h;
        ++sCameraEpoch;
        sStampEpochTime = gFrameTimeSeconds;
    }
    static F32 last_log = 0.f;
    if (gFrameTimeSeconds - last_log >= 10.f)
    {
        static StampStats prev;
        const StampStats& n = sStamp;
        if (last_log > 0.f)
        {
            LL_INFOS("VkTexStamp") << "mode " << sStampMode << ": face loops kept " << n.mHit - prev.mHit << ", run because new " << n.mMissNew - prev.mMissNew << ", faces changed "
                                   << n.mMissFaces - prev.mMissFaces << ", camera moved " << n.mMissCamera - prev.mMissCamera << ", bias changed " << n.mMissBias - prev.mMissBias << ", too old "
                                   << n.mMissAge - prev.mMissAge << "; textures not kept (moving, rigged, particle or media faces) " << n.mVolatile - prev.mVolatile
                                   << ", with faces but none on screen " << n.mOffScreen - prev.mOffScreen << "; verified " << n.mVerified - prev.mVerified << ", differed " << n.mMismatch - prev.mMismatch << " (faces changed in count " << n.mMismatchFaceCount - prev.mMismatchFaceCount
                                   << ", same faces within 0.5 s " << n.mMismatchFresh - prev.mMismatchFresh << ", same faces older " << n.mMismatchOld - prev.mMismatchOld << "); what differed: texture entry scale " << n.mWhyTe - prev.mWhyTe << ", face position or extents " << n.mWhyGeom - prev.mWhyGeom
                                   << " (attachment " << n.mWhyGeomKind[0] - prev.mWhyGeomKind[0] << ", avatar " << n.mWhyGeomKind[1] - prev.mWhyGeomKind[1] << ", tree or grass " << n.mWhyGeomKind[2] - prev.mWhyGeomKind[2]
                                   << ", prim " << n.mWhyGeomKind[3] - prev.mWhyGeomKind[3] << ", other " << n.mWhyGeomKind[4] - prev.mWhyGeomKind[4] << "), neither " << n.mWhyNeither - prev.mWhyNeither
                                   << ", camera epoch " << sCameraEpoch << LL_ENDL;
        }
        prev = n;
        last_log = gFrameTimeSeconds;
    }
}

F32 LLViewerTextureList::updateImagesFetchTextures(F32 max_time)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    typedef std::vector<LLPointer<LLViewerFetchedTexture> > entries_list_t;
    entries_list_t entries;

    updateCameraEpoch();
    LLTexRecs::frame();         // <FS> H6: the records that changed go to the worker, the results it has finished come back
    // update N textures at beginning of mImageList
    U32 update_count = 0;
    static const S32 MIN_UPDATE_COUNT = gSavedSettings.getS32("TextureFetchUpdateMinCount");       // default: 32

    // NOTE:  a texture may be deleted as a side effect of some of these updates
    // Deletion rules check ref count, so be careful not to hold any LLPointer references to the textures here other than the one in entries.

    //update MIN_UPDATE_COUNT or 5% of other textures, whichever is greater
    // <FS> the share of textures refreshed per frame is 1/FSTextureUpdateDivisor (the viewer default was 20 = 5%)
    static LLCachedControl<U32> fs_update_divisor(gSavedSettings, "FSTextureUpdateDivisor", 20);
    static LLCachedControl<U32> fs_burst_divisor(gSavedSettings, "FSTextureUpdateBurstDivisor", 20);
    static LLCachedControl<F32> fs_burst_seconds(gSavedSettings, "FSTextureUpdateBurstSeconds", 3.f);
    // The slow rate saves CPU while the view is steady, but new textures then wait a long time for their first request. So the
    // fast (stock) rate is used for a few seconds whenever the view changes: a new region, or the camera has moved or turned
    // noticeably since the last time it was checked.
    {
        static LLViewerRegion* last_region = nullptr;
        static LLVector3 last_origin;
        static LLVector3 last_at;
        static F64 burst_until = 0.0;
        const F64 now = LLFrameTimer::getElapsedSeconds();
        LLViewerRegion* region = gAgent.getRegion();
        const LLVector3 origin = LLViewerCamera::getInstance()->getOrigin();
        const LLVector3 at = LLViewerCamera::getInstance()->getAtAxis();
        bool changed = region != last_region;
        // <FS> new textures arriving (an object or an avatar rezzing) end the steady-view slowdown below (the normal rate then applies, not the fast burst)
        static size_t last_texture_count = 0;
        const bool new_textures = mUUIDMap.size() > last_texture_count;
        last_texture_count = mUUIDMap.size();
        if (!changed && (origin - last_origin).magVec() > 2.f)
        {
            changed = true;
        }
        if (!changed && (at * last_at) < 0.96f)     // about 16 degrees
        {
            changed = true;
        }
        if (changed)
        {
            last_region = region;
            last_origin = origin;
            last_at = at;
            burst_until = now + fs_burst_seconds;
        }
        // <FS> a view that has not changed for a while (no new region, camera still) refreshes the textures at a slower rate again: nothing it would find has changed, and the
        // fast rate comes back as soon as the camera moves or turns. LLViewerTextureList::sViewSteady is also read by the object texture priority pass.
        static LLCachedControl<U32> fs_steady_factor(gSavedSettings, "FSTextureUpdateSteadyFactor", 2);
        static LLCachedControl<F32> fs_steady_seconds(gSavedSettings, "FSTextureUpdateSteadySeconds", 10.f);
        static F64 last_change_time = 0.0;
        if (changed || new_textures)
        {
            last_change_time = now;
        }
        sViewSteady = fs_steady_factor > 1 && now >= burst_until && (now - last_change_time) > fs_steady_seconds;
        U32 divisor = now < burst_until ? llmin((U32) fs_burst_divisor, (U32) fs_update_divisor) : (U32) fs_update_divisor;
        if (sViewSteady)
        {
            divisor *= llmax((U32) fs_steady_factor, 1u);
        }
        update_count = llmax((U32) MIN_UPDATE_COUNT, (U32) mUUIDMap.size() / llmax(divisor, 1u));
    }
    if (sIdxMode)
    {
        idxLog();
    }
    // </FS>
    if (LLViewerTexture::sDesiredDiscardBias > 1.f
        && LLViewerTexture::sBiasTexturesUpdated < (U32)mUUIDMap.size())
    {
        // We are over memory target. Bias affects discard rates, so update
        // existing textures agresively to free memory faster.
        update_count = (S32)(update_count * LLViewerTexture::sDesiredDiscardBias);

        // This isn't particularly precise and can overshoot, but it doesn't need
        // to be, just making sure it did a full circle and doesn't get stuck updating
        // at bias = 4 with 4 times the rate permanently.
        LLViewerTexture::sBiasTexturesUpdated += update_count;
    }
    update_count = llmin(update_count, (U32) mUUIDMap.size());

    if (sIdxMode)
    { // <FS> H6: the textures that were woken first, then a slice of the active list and a slice of the dormant one (mode 2 walks the dormant ones as often as the old walk did, to find what wakes missed)
        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("vtluift - copy lists");
        const size_t n_active = mActive.size(), n_dormant = mDormant.size(), total = n_active + n_dormant;
        entries.reserve(update_count + mWake.size());
        for (LLViewerFetchedTexture* tex : mWake)
        {
            tex->mWakeQueued = false;
            idxMove(tex, 1);
            if (tex->getImageTexture())
            {
                entries.push_back(tex);
                ++sIdx.mVisitsWake;
            }
        }
        mWake.clear();
        if (total > 0)
        {
            const size_t active_take = llmin(n_active, (size_t)((U64)update_count * n_active / total) + 1);
            size_t dormant_take = (size_t)((U64)update_count * n_dormant / total);
            if (sIdxMode == 1)
            {
                dormant_take = llmin(n_dormant, dormant_take / 8 + 1);      // the slow list: an eighth of the rate
            }
            else
            {
                dormant_take = llmin(n_dormant, dormant_take + 1);
            }
            for (size_t k = 0; k < active_take && !mActive.empty(); ++k)
            {
                LLViewerFetchedTexture* tex = mActive[(mActiveCursor + k) % mActive.size()];
                if (tex->getImageTexture())
                {
                    entries.push_back(tex);
                    ++sIdx.mVisitsActive;
                }
            }
            if (!mActive.empty()) mActiveCursor = (mActiveCursor + active_take) % mActive.size();
            for (size_t k = 0; k < dormant_take && !mDormant.empty(); ++k)
            {
                LLViewerFetchedTexture* tex = mDormant[(mDormantCursor + k) % mDormant.size()];
                if (tex->getImageTexture())
                {
                    entries.push_back(tex);
                    ++sIdx.mVisitsDormant;
                }
            }
            if (!mDormant.empty()) mDormantCursor = (mDormantCursor + dormant_take) % mDormant.size();
        }
    }
    else
    { // copy entries out of UUID map to avoid iterator invalidation from deletion inside updateImageDecodeProiroty or updateFetch below
        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("vtluift - copy");

        // copy entries out of UUID map for updating
        entries.reserve(update_count);
        uuid_map_t::iterator iter = mUUIDMap.upper_bound(mLastUpdateKey);
        while (update_count-- > 0)
        {
            if (iter == mUUIDMap.end())
            {
                iter = mUUIDMap.begin();
            }

            if (iter->second->getImageTexture())
            {
                entries.push_back(iter->second);
            }
            ++iter;
        }
    }

    LLTimer timer;

    // <FS> LL_VK_TEX_PRE: the face loops of the slice on the pool (see FaceLoopPre)
    static std::vector<FaceLoopPre> pres;
    pres.clear();
    LLForkJoinPool* pre_pool = (sPreMode != 0 && sStampMode != 2 && !gCubeSnapshot && entries.size() >= 96 && !LLGpuZoneTimer::isActive()) ? LLForkJoinPool::shared() : nullptr;
    if (pre_pool)
    {
        LL_CPU_ZONE("textures: decode priority: face loops on the pool");
        pres.assign(entries.size(), FaceLoopPre());
        static LLCachedControl<F32> pre_scale_min(gSavedSettings, "TextureScaleMinAreaFactor", 0.0095f);
        static LLCachedControl<F32> pre_scale_max(gSavedSettings, "TextureScaleMaxAreaFactor", 25.f);
        const F32 scale_min = pre_scale_min, scale_max = pre_scale_max;
        const S32 pre_stamp_bias = (S32)(LLViewerTexture::sDesiredDiscardBias * 20.f);
        const U32 max_faces_to_check = 1024;
        std::atomic<U64> candidates{ 0 }, computed{ 0 };
        // a job that meets a due rigged face makes the avatar's joints clean first (once a frame per avatar, the first job to ask does it and the others wait: LLVOAvatar::cleanJointMatricesOnce), so
        // LLJoint::getWorldMatrix only reads. LL_VK_TEX_PRE_RIGGED=0 leaves the rigged faces to this thread, as in 8.19.
        static const bool rigged_in_jobs = !getenv("LL_VK_TEX_PRE_RIGGED") || atoi(getenv("LL_VK_TEX_PRE_RIGGED")) != 0;
        const bool skeletons_clean = rigged_in_jobs;
        // the loop of updateImageDecodePriority for one texture, without its writes (the faces are not touched); false when the texture needs none or has a face that is due a pixel area refresh
        const auto precompute = [&](LLViewerFetchedTexture* imagep, FaceLoopPre& out) -> bool
        {
            if (imagep->getBoostLevel() >= LLViewerFetchedTexture::BOOST_HIGH)
            {
                return false;
            }
            if (sStampMode == 1)
            {
                const LLViewerTexture::FaceLoopCache& stamp = imagep->mFaceCache;
                if (stamp.mValid && stamp.mFaceStamp == imagep->mFaceStamp && stamp.mCamEpoch == sCameraEpoch && stamp.mBias == pre_stamp_bias && gFrameTimeSeconds - stamp.mTime < sStampAge)
                {
                    return false;       // the kept result answers
                }
            }
            if (LLTexRecs::sMode == 2)
            {
                LLTexRecs::Result rec_res;
                if (LLTexRecs::result(imagep, rec_res) && rec_res.mStamp == imagep->mFaceStamp && !rec_res.mVolatile)
                {
                    U32 fc = 0;
                    for (U32 i = 0; i < LLRender::NUM_TEXTURE_CHANNELS; ++i)
                    {
                        fc += imagep->getNumFaces(i);
                    }
                    if (fc == rec_res.mFaces && fc <= max_faces_to_check)
                    {
                        return false;   // the worker's record answers
                    }
                }
            }
            candidates.fetch_add(1, std::memory_order_relaxed);
            LLImageTex* img = imagep->getImageTexture();
            F32 max_discard = F32(img ? img->getMaxDiscardLevel() : MAX_DISCARD_LEVEL);
            F32 bias = llclamp(max_discard - 2.f, 1.f, LLViewerTexture::sDesiredDiscardBias);
            bias = (F32) llroundf(powf(4, bias - 1.f));
            constexpr F32 BIAS_TRS_ON_SCREEN = 1.f;
            F32 max_vsize = 0.f;
            bool on_screen = false;
            U32 face_count = 0, faces_seen = 0, faces_occluded = 0;
            bool is_volatile = false;
            for (U32 i = 0; i < LLRender::NUM_TEXTURE_CHANNELS; ++i)
            {
                face_count += imagep->getNumFaces(i);
                S32 faces_to_check = (face_count > max_faces_to_check) ? 0 : imagep->getNumFaces(i);
                for (S32 fi = 0; fi < faces_to_check; ++fi)
                {
                    LLFace* face = (*(imagep->getFaceList(i)))[fi];
                    if (face && face->getViewerObject())
                    {
                        ++faces_seen;
                        {
                            LLDrawable* drw = face->getDrawable();
                            LLSpatialGroup* grp = drw ? drw->getSpatialGroup() : nullptr;
                            if (grp && grp->isOcclusionState(LLOcclusionCullingGroup::OCCLUDED)) ++faces_occluded;
                        }
                        if (sStampMode && (face->isState(LLFace::RIGGED) || face->hasMedia() || !face->getDrawable() || face->getDrawable()->isActive()
                                           || face->getViewerObject()->getPartitionType() == LLViewerRegion::PARTITION_PARTICLE))
                        {
                            is_volatile = true;
                        }
                        {
                            // a face that is due a pixel area refresh is claimed (0xFFFFFFFF in mLastTextureUpdate while it is worked out; to everyone else that reads as "being done") by exactly one job;
                            // a texture whose face another job has claimed is left to the main thread, where the face is fresh by then. A rigged face whose rig table needs an update (a write to the volume
                            // face, which meshes share) is left to the main thread too.
                            std::atomic_ref<U32> stamp_of_face(face->row().lastTextureUpdate);
                            U32 last = stamp_of_face.load(std::memory_order_acquire);
                            if (last == 0xFFFFFFFFu)
                            {
                                return false;
                            }
                            if ((gFrameCount - last) > 10)
                            {
                                if (face->isState(LLFace::RIGGED))
                                {
                                    // calcPixelArea of a rigged face reads the joints' world matrices (LLJoint::getWorldMatrix recomputes a dirty joint and writes it: two jobs for faces of one avatar would
                                    // tear it, which shows as a spike in the skin) and may update the rig table (shared between meshes). The skeletons were cleaned before the jobs (see below), so the matrices
                                    // are only read; a face whose rig table needs an update stays with the main thread.
                                    LLViewerObject* rigged_obj = face->getViewerObject();
                                    LLVolume* rigged_volume = rigged_obj ? rigged_obj->getVolume() : nullptr;
                                    if (!skeletons_clean || !rigged_volume || !rigged_obj->getAvatar() || face->getTEOffset() < 0 || face->getTEOffset() >= rigged_volume->getNumVolumeFaces()
                                        || rigged_volume->getVolumeFace(face->getTEOffset()).mJointRiggingInfoTab.needsUpdate())
                                    {
                                        return false;
                                    }
                                    rigged_obj->getAvatar()->cleanJointMatricesOnce(gFrameCount);
                                }
                                if (!stamp_of_face.compare_exchange_strong(last, 0xFFFFFFFFu, std::memory_order_acq_rel))
                                {
                                    return false;
                                }
                                F32 area_radius, area_cos;
                                face->row().inFrustum = face->calcPixelArea(area_cos, area_radius);
                                stamp_of_face.store(gFrameCount, std::memory_order_release);
                            }
                        }
                        F32 vsize = face->getPixelArea();
                        on_screen |= face->row().inFrustum;
                        S32 te_offset = face->getTEOffset();
                        LLViewerObject* objp = face->getViewerObject();
                        const LLTextureEntry* te = (te_offset < 0 || te_offset >= objp->getNumTEs()) ? nullptr : objp->getTE(te_offset);
                        F32 min_scale = te ? llmin(fabsf(te->getScaleS()), fabsf(te->getScaleT())) : 1.f;
                        min_scale = llclamp(min_scale * min_scale, scale_min, scale_max);
                        vsize /= min_scale;
                        if (!face->row().inFrustum || LLViewerTexture::sDesiredDiscardBias > 1.9f + face->row().importanceToCamera / 2.f)
                        {
                            vsize /= bias;
                        }
                        if (face->row().inFrustum)
                        {
                            static LLCachedControl<F32> texture_camera_boost(gSavedSettings, "TextureCameraBoost", 8.f);
                            vsize *= llmax(face->row().importanceToCamera * texture_camera_boost, 1.f);
                        }
                        max_vsize = llmax(max_vsize, vsize);
                        if (max_vsize >= LLViewerFetchedTexture::sMaxVirtualSize
                            && (on_screen || LLViewerTexture::sDesiredDiscardBias <= BIAS_TRS_ON_SCREEN))
                        {
                            break;
                        }
                    }
                }
                if (max_vsize >= LLViewerFetchedTexture::sMaxVirtualSize
                    && (on_screen || LLViewerTexture::sDesiredDiscardBias <= BIAS_TRS_ON_SCREEN))
                {
                    break;
                }
            }
            out.mFaceCount = face_count;
            out.mMaxVsize = max_vsize;
            out.mOnScreen = on_screen;
            out.mFacesSeen = faces_seen;
            out.mFacesOccluded = faces_occluded;
            out.mVolatile = is_volatile;
            out.mValid = true;
            computed.fetch_add(1, std::memory_order_relaxed);
            return true;
        };
        static const size_t chunk = getenv("LL_VK_TEX_PRE_CHUNK") ? (size_t)std::max(1, atoi(getenv("LL_VK_TEX_PRE_CHUNK"))) : 8;       // (a chunk with a few rigged faces, a joint walk each, ran long beside the others at 32)
        const size_t chunks = (entries.size() + chunk - 1) / chunk;
        const std::function<void(size_t)> job = [&](size_t c)
        {
            struct JobFlag { bool mPrev = gFSTexPreJob; JobFlag() { gFSTexPreJob = true; } ~JobFlag() { gFSTexPreJob = mPrev; } } job_flag;
            const size_t end = std::min(entries.size(), (c + 1) * chunk);
            for (size_t i = c * chunk; i < end; ++i)
            {
                LLViewerFetchedTexture* imagep = entries[i];
                if (imagep && imagep->getNumRefs() > 1)
                {
                    precompute(imagep, pres[i]);
                }
            }
        };
        pre_pool->runLabeled("textures: priority pre-pass", chunks, job);
        sPreCandidates += candidates.load();
        sPreComputed += computed.load();
        sPreEntries += entries.size();
        if (++sPreFrames % 3000 == 0)
        {
            LL_INFOS("TexPre") << "texture face loops on the pool, per frame over " << sPreFrames << " frames: " << (double)sPreEntries / sPreFrames << " textures in the slice, " << (double)sPreCandidates / sPreFrames
                               << " needed a loop, " << (double)sPreComputed / sPreFrames << " computed by jobs, " << (double)sPreUsed / sPreFrames << " used, dirty joints met by the jobs " << gFSTexPreDirtyReads.load() << (sPreMode == 2 ? "; compared " : "")
                               << (sPreMode == 2 ? std::to_string(sPreCompared) + ", differed " + std::to_string(sPreDiffer) : std::string()) << LL_ENDL;
        }
    }

    for (size_t entry_index = 0; entry_index < entries.size(); ++entry_index)
    {
        auto& imagep = entries[entry_index];
        mLastUpdateKey = LLTextureKey(imagep->getID(), (ETexListType)imagep->getTextureListType());

        if (imagep->getNumRefs() > 1) // make sure this image hasn't been deleted before attempting to update (may happen as a side effect of some other image updating)
        {
            sCurrentPre = pres.empty() ? nullptr : &pres[entry_index];
            { LL_CPU_ZONE("textures: decode priority (per texture)"); updateImageDecodePriority(imagep); }
            sCurrentPre = nullptr;
            { LL_CPU_ZONE("textures: updateFetch (per texture)"); imagep->updateFetch(); }
        }

        if (timer.getElapsedTimeF32() > max_time)
        {
            break;
        }
    }

    return timer.getElapsedTimeF32();
}

void LLViewerTextureList::updateImagesUpdateStats()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (mForceResetTextureStats)
    {
        for (image_list_t::iterator iter = mImageList.begin();
             iter != mImageList.end(); )
        {
            LLViewerFetchedTexture* imagep = *iter++;
            imagep->resetTextureStats();
        }
        mForceResetTextureStats = false;
    }
}

void LLViewerTextureList::decodeAllImages(F32 max_time)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    LLTimer timer;

    //loading from fast cache
    max_time -= updateImagesLoadingFastCache(max_time);

    // Update texture stats and priorities
    std::vector<LLPointer<LLViewerFetchedTexture> > image_list;
    for (image_list_t::iterator iter = mImageList.begin();
         iter != mImageList.end(); )
    {
        LLViewerFetchedTexture* imagep = *iter++;
        image_list.push_back(imagep);
        imagep->setInImageList(false) ;
    }

    llassert_always(image_list.size() == mImageList.size()) ;
    mImageList.clear();
    for (std::vector<LLPointer<LLViewerFetchedTexture> >::iterator iter = image_list.begin();
         iter != image_list.end(); ++iter)
    {
        LLViewerFetchedTexture* imagep = *iter;
        imagep->processTextureStats();
        addImageToList(imagep);
    }
    image_list.clear();

    // Update fetch (decode)
    for (image_list_t::iterator iter = mImageList.begin();
         iter != mImageList.end(); )
    {
        LLViewerFetchedTexture* imagep = *iter++;
        imagep->updateFetch();
    }
    std::shared_ptr<LL::WorkQueue> main_queue = LLImageTexThread::sEnabledTextures ? LL::WorkQueue::getInstance("mainloop") : NULL;
    // Run threads
    size_t fetch_pending = 0;
    while (1)
    {
        LLAppViewer::instance()->getTextureCache()->update(1); // unpauses the texture cache thread
        LLAppViewer::instance()->getImageDecodeThread()->update(1); // unpauses the image thread
        fetch_pending = LLAppViewer::instance()->getTextureFetch()->update(1); // unpauses the texture fetch thread

        if (LLImageTexThread::sEnabledTextures)
        {
            main_queue->runFor(std::chrono::milliseconds(1));
            fetch_pending += main_queue->size();
        }

        if (fetch_pending == 0 || timer.getElapsedTimeF32() > max_time)
        {
            break;
        }
    }
    // Update fetch again
    for (image_list_t::iterator iter = mImageList.begin();
         iter != mImageList.end(); )
    {
        LLViewerFetchedTexture* imagep = *iter++;
        imagep->updateFetch();
    }
    max_time -= timer.getElapsedTimeF32();
    max_time = llmax(max_time, .001f);
    F32 create_time = updateImagesCreateTextures(max_time);

    LL_DEBUGS("ViewerImages") << "decodeAllImages() took " << timer.getElapsedTimeF32() << " seconds. "
    << " fetch_pending " << fetch_pending
    << " create_time " << create_time
    << LL_ENDL;
}

bool LLViewerTextureList::createUploadFile(LLPointer<LLImageRaw> raw_image,
                                           const std::string& out_filename,
                                           const S32 max_image_dimentions,
                                           const S32 min_image_dimentions)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    LLImageDataSharedLock lock(raw_image);

    // make a copy, since convertToUploadFile scales raw image
    LLPointer<LLImageRaw> scale_image = new LLImageRaw(
        raw_image->getData(),
        raw_image->getWidth(),
        raw_image->getHeight(),
        raw_image->getComponents());

    LLPointer<LLImageJ2C> compressedImage = LLViewerTextureList::convertToUploadFile(scale_image, max_image_dimentions);
    if (compressedImage->getWidth() < min_image_dimentions || compressedImage->getHeight() < min_image_dimentions)
    {
        std::string reason = llformat("Images below %d x %d pixels are not allowed. Actual size: %d x %dpx",
                                      min_image_dimentions,
                                      min_image_dimentions,
                                      compressedImage->getWidth(),
                                      compressedImage->getHeight());
        compressedImage->setLastError(reason);
        return false;
    }
    if (compressedImage.isNull())
    {
        compressedImage->setLastError("Couldn't convert the image to jpeg2000.");
        LL_INFOS() << "Couldn't convert to j2c, file : " << out_filename << LL_ENDL;
        return false;
    }
    if (!compressedImage->save(out_filename))
    {
        compressedImage->setLastError("Couldn't create the jpeg2000 image for upload.");
        LL_INFOS() << "Couldn't create output file : " << out_filename << LL_ENDL;
        return false;
    }
    return true;
}

bool LLViewerTextureList::createUploadFile(const std::string& filename,
                                         const std::string& out_filename,
                                         const U8 codec,
                                         const S32 max_image_dimentions,
                                         const S32 min_image_dimentions,
                                         bool force_square)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    try
    {
    // Load the image
    LLPointer<LLImageFormatted> image = LLImageFormatted::createFromType(codec);
    if (image.isNull())
    {
        LL_WARNS() << "Couldn't open the image to be uploaded." << LL_ENDL;
        return false;
    }
    if (!image->load(filename))
    {
        image->setLastError("Couldn't load the image to be uploaded.");
        return false;
    }

    // calcDataSizeJ2C assumes maximum size is 2048 and for bigger images can
    // assign discard to bring imige to needed size, but upload does the scaling
    // as needed, so just reset discard.
    // Assume file is full and has 'discard' 0 data.
    // Todo: probably a better idea to have some setMaxDimentions in J2C
    // called when loading from a local file
    image->setDiscardLevel(0);

    // Decompress or expand it in a raw image structure
    LLPointer<LLImageRaw> raw_image = new LLImageRaw;
    if (!image->decode(raw_image, 0.0f))
    {
        image->setLastError("Couldn't decode the image to be uploaded.");
        return false;
    }
    // Check the image constraints
    if ((image->getComponents() != 3) && (image->getComponents() != 4))
    {
        image->setLastError("Image files with less than 3 or more than 4 components are not supported.");
        return false;
    }
    if (image->getWidth() < min_image_dimentions || image->getHeight() < min_image_dimentions)
    {
        std::string reason = llformat("Images below %d x %d pixels are not allowed. Actual size: %d x %dpx",
            min_image_dimentions,
            min_image_dimentions,
            image->getWidth(),
            image->getHeight());
        image->setLastError(reason);
        return false;
    }
    // Convert to j2c (JPEG2000) and save the file locally
    LLPointer<LLImageJ2C> compressedImage = convertToUploadFile(raw_image, max_image_dimentions, force_square);
    if (compressedImage.isNull())
    {
        image->setLastError("Couldn't convert the image to jpeg2000.");
        LL_INFOS() << "Couldn't convert to j2c, file : " << filename << LL_ENDL;
        return false;
    }
    if (!compressedImage->save(out_filename))
    {
        image->setLastError("Couldn't create the jpeg2000 image for upload.");
        LL_INFOS() << "Couldn't create output file : " << out_filename << LL_ENDL;
        return false;
    }
    // Test to see if the encode and save worked
    LLPointer<LLImageJ2C> integrity_test = new LLImageJ2C;
    if (!integrity_test->loadAndValidate( out_filename ))
    {
        image->setLastError("The created jpeg2000 image is corrupt.");
        LL_INFOS() << "Image file : " << out_filename << " is corrupt" << LL_ENDL;
        return false;
    }
    }
    catch (...)
    {
        LOG_UNHANDLED_EXCEPTION("");
        return false;
    }
    return true;
}

// note: modifies the argument raw_image!!!!
LLPointer<LLImageJ2C> LLViewerTextureList::convertToUploadFile(LLPointer<LLImageRaw> raw_image, const S32 max_image_dimentions, bool force_square, bool force_lossless)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    LLImageDataLock lock(raw_image);

    if (force_square)
    {
        S32 biggest_side = llmax(raw_image->getWidth(), raw_image->getHeight());
        S32 square_size = raw_image->biasedDimToPowerOfTwo(biggest_side, max_image_dimentions);

        raw_image->scale(square_size, square_size);
    }
    else
    {
        raw_image->biasedScaleToPowerOfTwo(max_image_dimentions);
    }
    LLPointer<LLImageJ2C> compressedImage = new LLImageJ2C();

    if (force_lossless ||
        (gSavedSettings.getBOOL("LosslessJ2CUpload") &&
            (raw_image->getWidth() * raw_image->getHeight() <= LL_IMAGE_REZ_LOSSLESS_CUTOFF * LL_IMAGE_REZ_LOSSLESS_CUTOFF)))
    {
        compressedImage->setReversible(true);
    }


    if (gSavedSettings.getBOOL("Jpeg2000AdvancedCompression"))
    {
        // This test option will create jpeg2000 images with precincts for each level, RPCL ordering
        // and PLT markers. The block size is also optionally modifiable.
        // Note: the images hence created are compatible with older versions of the viewer.
        // Read the blocks and precincts size settings
        S32 block_size = gSavedSettings.getS32("Jpeg2000BlocksSize");
        S32 precinct_size = gSavedSettings.getS32("Jpeg2000PrecinctsSize");
        LL_INFOS() << "Advanced JPEG2000 Compression: precinct = " << precinct_size << ", block = " << block_size << LL_ENDL;
        compressedImage->initEncode(*raw_image, block_size, precinct_size, 0);
    }

    if (!compressedImage->encode(raw_image, 0.0f))
    {
        LL_INFOS() << "convertToUploadFile : encode returns with error!!" << LL_ENDL;
        // Clear up the pointer so we don't leak that one
        compressedImage = NULL;
    }

    return compressedImage;
}

///////////////////////////////////////////////////////////////////////////////

// <FS:Ansariel> OpenSim compatibility
// static
void LLViewerTextureList::receiveImageHeader(LLMessageSystem *msg, void **user_data)
{
    static LLCachedControl<bool> log_texture_traffic(gSavedSettings,"LogTextureNetworkTraffic", false) ;

    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    // Receive image header, copy into image object and decompresses
    // if this is a one-packet image.

    LLUUID id;

    char ip_string[256];
    u32_to_ip_string(msg->getSenderIP(),ip_string);

    U32Bytes received_size ;
    if (msg->getReceiveCompressedSize())
    {
        received_size = (U32Bytes)msg->getReceiveCompressedSize() ;
    }
    else
    {
        received_size = (U32Bytes)msg->getReceiveSize() ;
    }
    add(LLStatViewer::TEXTURE_NETWORK_DATA_RECEIVED, received_size);
    add(LLStatViewer::TEXTURE_PACKETS, 1);

    U8 codec;
    U16 packets;
    U32 totalbytes;
    msg->getUUIDFast(_PREHASH_ImageID, _PREHASH_ID, id);
    msg->getU8Fast(_PREHASH_ImageID, _PREHASH_Codec, codec);
    msg->getU16Fast(_PREHASH_ImageID, _PREHASH_Packets, packets);
    msg->getU32Fast(_PREHASH_ImageID, _PREHASH_Size, totalbytes);

    S32 data_size = msg->getSizeFast(_PREHASH_ImageData, _PREHASH_Data);
    if (!data_size)
    {
        return;
    }
    if (data_size < 0)
    {
        // msg->getSizeFast() is probably trying to tell us there
        // was an error.
        LL_ERRS() << "image header chunk size was negative: "
        << data_size << LL_ENDL;
        return;
    }

    // this buffer gets saved off in the packet list
    U8 *data = new U8[data_size];
    msg->getBinaryDataFast(_PREHASH_ImageData, _PREHASH_Data, data, data_size);

    LLViewerFetchedTexture *image = LLViewerTextureManager::getFetchedTexture(id, FTT_DEFAULT, true, LLBaseTexture::BOOST_NONE, LLViewerTexture::LOD_TEXTURE);
    if (!image)
    {
        delete [] data;
        return;
    }
    if(log_texture_traffic)
    {
        gTotalTextureBytesPerBoostLevel[image->getBoostLevel()] += received_size ;
    }

    //image->getLastPacketTimer()->reset();
    bool res = LLAppViewer::getTextureFetch()->receiveImageHeader(msg->getSender(), id, codec, packets, totalbytes, data_size, data);
    if (!res)
    {
        delete[] data;
    }
}

// static
void LLViewerTextureList::receiveImagePacket(LLMessageSystem *msg, void **user_data)
{
    static LLCachedControl<bool> log_texture_traffic(gSavedSettings,"LogTextureNetworkTraffic", false) ;

    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    // Receives image packet, copy into image object,
    // checks if all packets received, decompresses if so.

    LLUUID id;
    U16 packet_num;

    char ip_string[256];
    u32_to_ip_string(msg->getSenderIP(),ip_string);

    U32Bytes received_size ;
    if (msg->getReceiveCompressedSize())
    {
        received_size = (U32Bytes)msg->getReceiveCompressedSize() ;
    }
    else
    {
        received_size = (U32Bytes)msg->getReceiveSize() ;
    }

    add(LLStatViewer::TEXTURE_NETWORK_DATA_RECEIVED, F64Bytes(received_size));
    add(LLStatViewer::TEXTURE_PACKETS, 1);

    //llprintline("Start decode, image header...");
    msg->getUUIDFast(_PREHASH_ImageID, _PREHASH_ID, id);
    msg->getU16Fast(_PREHASH_ImageID, _PREHASH_Packet, packet_num);
    S32 data_size = msg->getSizeFast(_PREHASH_ImageData, _PREHASH_Data);

    if (!data_size)
    {
        return;
    }
    if (data_size < 0)
    {
        // msg->getSizeFast() is probably trying to tell us there
        // was an error.
        LL_ERRS() << "image data chunk size was negative: "
        << data_size << LL_ENDL;
        return;
    }
    if (data_size > MTUBYTES)
    {
        LL_ERRS() << "image data chunk too large: " << data_size << " bytes" << LL_ENDL;
        return;
    }
    U8 *data = new U8[data_size];
    msg->getBinaryDataFast(_PREHASH_ImageData, _PREHASH_Data, data, data_size);

    LLViewerFetchedTexture *image = LLViewerTextureManager::getFetchedTexture(id, FTT_DEFAULT, true, LLBaseTexture::BOOST_NONE, LLViewerTexture::LOD_TEXTURE);
    if (!image)
    {
        delete [] data;
        return;
    }
    if(log_texture_traffic)
    {
        gTotalTextureBytesPerBoostLevel[image->getBoostLevel()] += received_size ;
    }

    //image->getLastPacketTimer()->reset();
    bool res = LLAppViewer::getTextureFetch()->receiveImagePacket(msg->getSender(), id, packet_num, data_size, data);
    if (!res)
    {
        delete[] data;
    }
}
// </FS:Ansariel>

// We've been that the asset server does not contain the requested image id.
// static
void LLViewerTextureList::processImageNotInDatabase(LLMessageSystem *msg,void **user_data)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    LLUUID image_id;
    msg->getUUIDFast(_PREHASH_ImageID, _PREHASH_ID, image_id);

    LLViewerFetchedTexture* image = gTextureList.findImage( image_id, TEX_LIST_STANDARD);
    if( image )
    {
        LL_WARNS() << "Image not in db" << LL_ENDL;
        image->setIsMissingAsset();
    }

    image = gTextureList.findImage(image_id, TEX_LIST_SCALE);
    if (image)
    {
        LL_WARNS() << "Icon not in db" << LL_ENDL;
        image->setIsMissingAsset();
    }
}


///////////////////////////////////////////////////////////////////////////////

// explicitly cleanup resources, as this is a singleton class with process
// lifetime so ability to perform std::map operations in destructor is not
// guaranteed.
void LLUIImageList::cleanUp()
{
    mUIImages.clear();
    mUITextureList.clear() ;
}

LLUIImagePtr LLUIImageList::getUIImageByID(const LLUUID& image_id, S32 priority)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    // use id as image name
    std::string image_name = image_id.asString();

    // look for existing image
    uuid_ui_image_map_t::iterator found_it = mUIImages.find(image_name);
    if (found_it != mUIImages.end())
    {
        return found_it->second;
    }

    const bool use_mips = false;
    const LLRect scale_rect = LLRect::null;
    const LLRect clip_rect = LLRect::null;
    return loadUIImageByID(image_id, use_mips, scale_rect, clip_rect, (LLViewerTexture::EBoostLevel)priority);
}

LLUIImagePtr LLUIImageList::getUIImage(const std::string& image_name, S32 priority)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    // look for existing image
    uuid_ui_image_map_t::iterator found_it = mUIImages.find(image_name);
    if (found_it != mUIImages.end())
    {
        return found_it->second;
    }

    const bool use_mips = false;
    const LLRect scale_rect = LLRect::null;
    const LLRect clip_rect = LLRect::null;
    return loadUIImageByName(image_name, image_name, use_mips, scale_rect, clip_rect, (LLViewerTexture::EBoostLevel)priority);
}

LLUIImagePtr LLUIImageList::loadUIImageByName(const std::string& name, const std::string& filename,
                                              bool use_mips, const LLRect& scale_rect, const LLRect& clip_rect, LLViewerTexture::EBoostLevel boost_priority,
                                              LLUIImage::EScaleStyle scale_style)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (boost_priority == LLBaseTexture::BOOST_NONE)
    {
        boost_priority = LLBaseTexture::BOOST_UI;
    }
    LLViewerFetchedTexture* imagep = LLViewerTextureManager::getFetchedTextureFromFile(filename, FTT_LOCAL_FILE, MIPMAP_NO, boost_priority);
    return loadUIImage(imagep, name, use_mips, scale_rect, clip_rect, scale_style);
}

LLUIImagePtr LLUIImageList::loadUIImageByID(const LLUUID& id,
                                            bool use_mips, const LLRect& scale_rect, const LLRect& clip_rect, LLViewerTexture::EBoostLevel boost_priority,
                                            LLUIImage::EScaleStyle scale_style)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (boost_priority == LLBaseTexture::BOOST_NONE)
    {
        boost_priority = LLBaseTexture::BOOST_UI;
    }
    LLViewerFetchedTexture* imagep = LLViewerTextureManager::getFetchedTexture(id, FTT_DEFAULT, MIPMAP_NO, boost_priority);
    return loadUIImage(imagep, id.asString(), use_mips, scale_rect, clip_rect, scale_style);
}

LLUIImagePtr LLUIImageList::loadUIImage(LLViewerFetchedTexture* imagep, const std::string& name, bool use_mips, const LLRect& scale_rect, const LLRect& clip_rect,
                                        LLUIImage::EScaleStyle scale_style)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (!imagep) return NULL;

    imagep->setAddressMode(LLTexUnit::TAM_CLAMP);

    //don't compress UI images
    imagep->getImageTexture()->setAllowCompression(false);

    LLUIImagePtr new_imagep = new LLUIImage(name, imagep);
    new_imagep->setScaleStyle(scale_style);

    if (imagep->getBoostLevel() != LLBaseTexture::BOOST_ICON
        && imagep->getBoostLevel() != LLBaseTexture::BOOST_THUMBNAIL
        && imagep->getBoostLevel() != LLBaseTexture::BOOST_PREVIEW)
    {
        // Don't add downloadable content into this list
        // all UI images are non-deletable and list does not support deletion
        imagep->setNoDelete();
        mUIImages.insert(std::make_pair(name, new_imagep));
        mUITextureList.push_back(imagep);
    }

    //Note:
    //Some other textures such as ICON also through this flow to be fetched.
    //But only UI textures need to set this callback.
    if(imagep->getBoostLevel() == LLBaseTexture::BOOST_UI)
    {
        LLUIImageLoadData* datap = new LLUIImageLoadData;
        datap->mImageName = name;
        datap->mImageScaleRegion = scale_rect;
        datap->mImageClipRegion = clip_rect;

        imagep->setLoadedCallback(onUIImageLoaded, 0, false, false, datap, NULL);
    }
    return new_imagep;
}

LLUIImagePtr LLUIImageList::preloadUIImage(const std::string& name, const std::string& filename, bool use_mips, const LLRect& scale_rect, const LLRect& clip_rect, LLUIImage::EScaleStyle scale_style)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    // look for existing image
    uuid_ui_image_map_t::iterator found_it = mUIImages.find(name);
    if (found_it != mUIImages.end())
    {
        // image already loaded!
        LL_ERRS() << "UI Image " << name << " already loaded." << LL_ENDL;
    }

    return loadUIImageByName(name, filename, use_mips, scale_rect, clip_rect, LLBaseTexture::BOOST_UI, scale_style);
}

//static
void LLUIImageList::onUIImageLoaded( bool success, LLViewerFetchedTexture *src_vi, LLImageRaw* src, LLImageRaw* src_aux, S32 discard_level, bool final, void* user_data )
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if(!success || !user_data)
    {
        return;
    }

    LLUIImageLoadData* image_datap = (LLUIImageLoadData*)user_data;
    std::string ui_image_name = image_datap->mImageName;
    LLRect scale_rect = image_datap->mImageScaleRegion;
    LLRect clip_rect = image_datap->mImageClipRegion;
    if (final)
    {
        delete image_datap;
    }

    LLUIImageList* instance = getInstance();

    uuid_ui_image_map_t::iterator found_it = instance->mUIImages.find(ui_image_name);
    if (found_it != instance->mUIImages.end())
    {
        LLUIImagePtr imagep = found_it->second;

        // for images grabbed from local files, apply clipping rectangle to restore original dimensions
        // from power-of-2 gl image
        if (success && imagep.notNull() && src_vi && (src_vi->getUrl().compare(0, 7, "file://")==0))
        {
            F32 full_width = (F32)src_vi->getFullWidth();
            F32 full_height = (F32)src_vi->getFullHeight();
            F32 clip_x = (F32)src_vi->getOriginalWidth() / full_width;
            F32 clip_y = (F32)src_vi->getOriginalHeight() / full_height;
            if (clip_rect != LLRect::null)
            {
                imagep->setClipRegion(LLRectf(llclamp((F32)clip_rect.mLeft / full_width, 0.f, 1.f),
                                            llclamp((F32)clip_rect.mTop / full_height, 0.f, 1.f),
                                            llclamp((F32)clip_rect.mRight / full_width, 0.f, 1.f),
                                            llclamp((F32)clip_rect.mBottom / full_height, 0.f, 1.f)));
            }
            else
            {
                imagep->setClipRegion(LLRectf(0.f, clip_y, clip_x, 0.f));
            }
            if (scale_rect != LLRect::null)
            {
                imagep->setScaleRegion(
                    LLRectf(llclamp((F32)scale_rect.mLeft / (F32)imagep->getWidth(), 0.f, 1.f),
                        llclamp((F32)scale_rect.mTop / (F32)imagep->getHeight(), 0.f, 1.f),
                        llclamp((F32)scale_rect.mRight / (F32)imagep->getWidth(), 0.f, 1.f),
                        llclamp((F32)scale_rect.mBottom / (F32)imagep->getHeight(), 0.f, 1.f)));
            }

            imagep->onImageLoaded();
        }
    }
}

namespace LLInitParam
{
    template<>
    struct TypeValues<LLUIImage::EScaleStyle> : public TypeValuesHelper<LLUIImage::EScaleStyle>
    {
        static void declareValues()
        {
            declare("scale_inner",  LLUIImage::SCALE_INNER);
            declare("scale_outer",  LLUIImage::SCALE_OUTER);
        }
    };
}

struct UIImageDeclaration : public LLInitParam::Block<UIImageDeclaration>
{
    Mandatory<std::string>      name;
    Optional<std::string>       file_name;
    Optional<bool>              preload;
    Optional<LLRect>            scale;
    Optional<LLRect>            clip;
    Optional<bool>              use_mips;
    Optional<LLUIImage::EScaleStyle> scale_type;

    UIImageDeclaration()
    :   name("name"),
        file_name("file_name"),
        preload("preload", false),
        scale("scale"),
        clip("clip"),
        use_mips("use_mips", false),
        scale_type("scale_type", LLUIImage::SCALE_INNER)
    {}
};

struct UIImageDeclarations : public LLInitParam::Block<UIImageDeclarations>
{
    Mandatory<S32>  version;
    Multiple<UIImageDeclaration> textures;

    UIImageDeclarations()
    :   version("version"),
        textures("texture")
    {}
};

bool LLUIImageList::initFromFile()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    // Look for textures.xml in all the right places. Pass
    // constraint=LLDir::ALL_SKINS because we want to overlay textures.xml
    // from all the skins directories.
    std::vector<std::string> textures_paths =
        gDirUtilp->findSkinnedFilenames(LLDir::TEXTURES, "textures.xml", LLDir::ALL_SKINS);
    std::vector<std::string>::const_iterator pi(textures_paths.begin()), pend(textures_paths.end());
    if (pi == pend)
    {
        LL_WARNS() << "No textures.xml found in skins directories" << LL_ENDL;
        return false;
    }

    // The first (most generic) file gets special validations
    LLXMLNodePtr root;
    if (!LLXMLNode::parseFile(*pi, root, NULL))
    {
        LL_WARNS() << "Unable to parse UI image list file " << *pi << LL_ENDL;
        return false;
    }
    if (!root->hasAttribute("version"))
    {
        LL_WARNS() << "No valid version number in UI image list file " << *pi << LL_ENDL;
        return false;
    }

    UIImageDeclarations images;
    LLXUIParser parser;
    parser.readXUI(root, images, *pi);

    // add components defined in the rest of the skin paths
    while (++pi != pend)
    {
        LLXMLNodePtr update_root;
        if (LLXMLNode::parseFile(*pi, update_root, NULL))
        {
            parser.readXUI(update_root, images, *pi);
        }
    }

    if (!images.validateBlock()) return false;

    std::map<std::string, UIImageDeclaration> merged_declarations;
    for (LLInitParam::ParamIterator<UIImageDeclaration>::const_iterator image_it = images.textures.begin();
        image_it != images.textures.end();
        ++image_it)
    {
        merged_declarations[image_it->name].overwriteFrom(*image_it);
    }

    enum e_decode_pass
    {
        PASS_DECODE_NOW,
        PASS_DECODE_LATER,
        NUM_PASSES
    };

    for (S32 cur_pass = PASS_DECODE_NOW; cur_pass < NUM_PASSES; cur_pass++)
    {
        for (std::map<std::string, UIImageDeclaration>::const_iterator image_it = merged_declarations.begin();
            image_it != merged_declarations.end();
            ++image_it)
        {
            const UIImageDeclaration& image = image_it->second;
            std::string file_name = image.file_name.isProvided() ? image.file_name() : image.name();

            // load high priority textures on first pass (to kick off decode)
            enum e_decode_pass decode_pass = image.preload ? PASS_DECODE_NOW : PASS_DECODE_LATER;
            if (decode_pass != cur_pass)
            {
                continue;
            }
            preloadUIImage(image.name, file_name, image.use_mips, image.scale, image.clip, image.scale_type);
        }

        // <FS:PP> Speed optimisation
        // if (!gSavedSettings.getBOOL("NoPreload"))
        static LLCachedControl<bool> no_preload(gSavedSettings, "NoPreload");
        if (!no_preload())
        // </FS:PP>
        {
            if (cur_pass == PASS_DECODE_NOW)
            {
                // init fetching and decoding of preloaded images
                gTextureList.decodeAllImages(9.f);
            }
            else
            {
                // decodeAllImages needs two passes to refresh stats and priorities on second pass
                gTextureList.decodeAllImages(1.f);
            }
        }
    }
    return true;
}


