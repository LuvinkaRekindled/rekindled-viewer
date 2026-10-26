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

#include "llviewertexturelist.h"
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
        pv = sRecV;
        ps = st;
    }
    last_log = gFrameTimeSeconds;
}

///////////////////////////////////////////////////////////////////////////////


// <FS> Where the textures are: how many have data, how many wait for it (being fetched, waiting to be created as GL textures), how many the server refused, what the viewer thinks they cost
// and the discard bias it works against. Next to the VkTex line of the Vulkan layer (the video memory the device really holds): a face that stays on the placeholder in a busy region shows up
// here as a long queue or a count of refused textures.
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
    LL_INFOS("VkTex") << "texture list: " << total << " images, " << with_gl << " with a GL texture, " << no_data << " with no data yet (" << fetching << " being fetched), " << missing
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
        // doc/vulkan/EXPIRY-design.md section 11: how long the textures in memory have been dormant (no face in view needing them), with the bytes the viewer counts for each age. The memory that a teleport leaves
        // behind shows here; the bias logic does not look at it (it lowers everything's quality by the percentage over budget).
        static U32 last_frame = 0;
        static LLTimer last_time;
        const U32 frame_now = (U32)LLFrameTimer::getFrameCount();
        const F32 seconds = last_time.getElapsedTimeF32();
        const double spf = last_frame && frame_now > last_frame ? (double)seconds / (double)(frame_now - last_frame) : 0.0166;
        last_frame = frame_now;
        last_time.reset();
        static const char* const names[6] = { "in use", "dormant < 10 s", "10-60 s", "60-300 s", "> 300 s", "not listed" };
        U32 count[6] = {};
        U64 bytes[6] = {};
        for (const LLPointer<LLViewerFetchedTexture>& image : mImageList)
        {
            if (!image->hasImageTexture())
            {
                continue;
            }
            int b = 5;
            if (image->mListState == 1)
            {
                b = 0;
            }
            else if (image->mListState == 2)
            {
                const double age = (double)(frame_now - std::min(frame_now, image->mDormantAtFrame)) * spf;
                b = age < 10.0 ? 1 : age < 60.0 ? 2 : age < 300.0 ? 3 : 4;
            }
            ++count[b];
            bytes[b] += (U64)image->getTextureMemory().value();
        }
        std::string text;
        for (int k = 0; k < 6; ++k)
        {
            text += llformat(" [%s: %u textures, %.0f MB]", names[k], count[k], (double)bytes[k] / 1048576.0);
        }
        LL_INFOS("VkTex") << "texture memory by dormant age:" << text << LL_ENDL;
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
                for (LLFace* face : *list)
                {
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
        LL_INFOS("VkTex") << "images with no data and not fetching: " << listed << " (virtual size 0: " << vsize_zero << ", above 0: " << vsize_pos << "; with faces on a diffuse map: " << with_faces << "; a desired discard level set: "
            << wants_data << "; deleted: " << deleted << "); by boost level" << boost_text << "; by fetched texture type" << ft_text << "; first ones:" << samples << LL_ENDL;
    }
    extern U32 gEarlySortResorts;
    LL_INFOS("VkTex") << LLRenderPass::geometryBuildStats() << "; the early sort was redone in " << gEarlySortResorts << " frames since the start" << LL_ENDL;
}

void LLViewerTextureList::updateImages(F32 max_time)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
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
// <FS> H6 (doc/vulkan/PLAN-texture-handles.md): LL_VK_TEXFB (default 1: measure and report, change nothing) compares what the draws measured of each texture's need (LLVkBridge::textureNeed: the whole texture
// would cover 1 / area pixels) with the result of the face loop, per texture the loop looked at.
const S32 sFbMode = []{ const char* e = getenv("LL_VK_TEXFB"); return e ? atoi(e) : 0; }();
struct FbStats
{
    U64 mLooked = 0;            // textures with faces the loop looked at
    U64 mNoData = 0;            // ... with no measurement at all
    U64 mCpuOnlyBig = 0;        // the loop says on screen with an area of 64 pixels or more; the draws did not sample the texture in the last second
    U64 mGpuOnly = 0;           // the draws sampled it in the last second; the loop says it is not on screen
    U64 mBoth = 0;              // both
    U64 mDiff[9] = {};          // both: log4(loop area / measured area), rounded, -4 or less to 4 or more
    U64 mNoName = 0;
    U64 mCpuOnlyKind[5] = {};   // the on-screen ones without a measurement, by the kind of object of the texture's first face: attachment, avatar, tree or grass, prim, other
    U64 mBothKind[5] = {};
    U64 mCpuOnlyRigged = 0;     // ... of them with a rigged face
    U64 mCpuOnlyBoost = 0;      // ... with a boost level above none
    U64 mUsed = 0, mCapped = 0; // LL_VK_TEXFB=3: textures sized by the measurement / never measured and capped
    U64 mUnOcc = 0;             // on screen by the loop, not sampled: every face looked at is in an occluded group
    U64 mUnNoFace = 0;          // ... no face with a drawable
    U64 mUnPool[32] = {};       // ... otherwise: the draw pool of the first face that is not occluded
};
FbStats sFb;
F32 sStampEpochTime = 0.f;      // gFrameTimeSeconds when the camera epoch last changed: a result is only kept once the camera has stood still for a moment (the face areas it read were then made under that pose, not up to 0.1 s earlier)
}

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
        if (LLTexRecs::sMode == 2 && LLTexRecs::result(imagep, rec_res) && rec_res.mStamp == imagep->mFaceStamp && !rec_res.mVolatile)
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
        else
        { LL_CPU_ZONE("textures: decode priority: face loop");
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

                    if ((gFrameCount - face->mLastTextureUpdate) > 10)
                    { // only call calcPixelArea at most once every 10 frames for a given face
                        // this helps eliminate redundant calls to calcPixelArea for faces that have multiple textures
                        // assigned to them, such as is the case with GLTF materials or Blinn-Phong materials
                        face->mInFrustum = face->calcPixelArea(cos_angle_to_view_dir, radius);
                        face->mLastTextureUpdate = gFrameCount;
                    }

                    F32 vsize = face->getPixelArea();

                    on_screen |= face->mInFrustum;

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
                        const F32 gv[9] = { c.mV[0], c.mV[1], c.mV[2], face->mExtents[0].getF32ptr()[0], face->mExtents[0].getF32ptr()[1], face->mExtents[0].getF32ptr()[2],
                                            face->mExtents[1].getF32ptr()[0], face->mExtents[1].getF32ptr()[1], face->mExtents[1].getF32ptr()[2] };
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
                    if (!face->mInFrustum || LLViewerTexture::sDesiredDiscardBias > 1.9f + face->mImportanceToCamera / 2.f)
                    {
                        vsize /= bias;
                    }

                    // boost resolution of textures that are important to the camera
                    if (face->mInFrustum)
                    {
                        static LLCachedControl<F32> texture_camera_boost(gSavedSettings, "TextureCameraBoost", 8.f);
                        vsize *= llmax(face->mImportanceToCamera*texture_camera_boost, 1.f);
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

        if (sFbMode && face_count > 0 && face_count <= max_faces_to_check)
        {
            ++sFb.mLooked;
            S32 kind = 4;
            bool rigged = false;
            for (U32 ch = 0; ch < LLRender::NUM_TEXTURE_CHANNELS && kind == 4 && !rigged; ++ch)
            {
                if (imagep->getNumFaces(ch) > 0)
                {
                    LLFace* f0 = (*(imagep->getFaceList(ch)))[0];
                    LLViewerObject* o0 = f0 ? f0->getViewerObject() : nullptr;
                    if (o0)
                    {
                        kind = o0->isAttachment() ? 0 : (o0->isAvatar() ? 1 : ((o0->getPCode() == LL_PCODE_LEGACY_TREE || o0->getPCode() == LL_PCODE_LEGACY_GRASS) ? 2 : (o0->getPCode() == LL_PCODE_VOLUME ? 3 : 4)));
                        rigged = f0->isState(LLFace::RIGGED);
                        break;
                    }
                }
            }
            // the on-screen textures nobody sampled: why? all the faces (up to 16 looked at) in occluded groups, or the pool of the first face that is not occluded
            auto note_unmeasured = [&]()
            {
                ++sFb.mCpuOnlyBig; ++sFb.mCpuOnlyKind[kind]; sFb.mCpuOnlyRigged += rigged; sFb.mCpuOnlyBoost += imagep->getBoostLevel() > 0;
                U32 looked = 0, occluded = 0, visible_pool = 0xFFFF;
                for (U32 ch = 0; ch < LLRender::NUM_TEXTURE_CHANNELS && looked < 16; ++ch)
                {
                    for (S32 fi = 0; fi < imagep->getNumFaces(ch) && looked < 16; ++fi)
                    {
                        LLFace* f = (*(imagep->getFaceList(ch)))[fi];
                        if (!f || !f->getViewerObject() || !f->getDrawable())
                        {
                            continue;
                        }
                        ++looked;
                        LLSpatialGroup* g = f->getDrawable()->getSpatialGroup();
                        if (g && g->isOcclusionState(LLOcclusionCullingGroup::OCCLUDED))
                        {
                            ++occluded;
                        }
                        else if (visible_pool == 0xFFFF)
                        {
                            visible_pool = f->getPoolType();
                        }
                    }
                }
                if (!looked) ++sFb.mUnNoFace;
                else if (occluded == looked) ++sFb.mUnOcc;
                else ++sFb.mUnPool[std::min<U32>(visible_pool, 31)];
            };
            const LLImageTex* it = imagep->getImageTexture();
            LLVkBridge::TextureNeed need;
            if (!it || !it->getTexName())
            {
                ++sFb.mNoName;
            }
            else if (!LLVkBridge::textureNeed(it->getTexName(), need))
            {
                ++sFb.mNoData;
                if (on_screen && max_vsize >= 64.f) note_unmeasured();
            }
            else if (need.mAge >= 1.f)
            {
                if (on_screen && max_vsize >= 64.f) note_unmeasured();
            }
            else if (!on_screen)
            {
                ++sFb.mGpuOnly;
            }
            else
            {
                ++sFb.mBoth;
                ++sFb.mBothKind[kind];
                const F32 gpu = llmin(1.f / llmax(need.mWindow, 1.0e-12f), LLViewerFetchedTexture::sMaxVirtualSize);
                const F32 d = logf(llmax(max_vsize, 1.f) / llmax(gpu, 1.f)) / logf(4.f);
                ++sFb.mDiff[llclamp((S32)floorf(d + 0.5f) + 4, 0, 8)];
            }
        }

        if (sFbMode == 3 && face_count > 0 && face_count <= max_faces_to_check)
        {
            // <FS> H6 proof of concept (LL_VK_TEXFB=3): the size of a texture on the screen is what the draws measured (whole texture = 1 / area pixels), not what the faces would cover: a texture that was not
            // drawn in the last second keeps what it was measured at, divided by the bias like an off-screen face; a texture never measured is capped at a modest size until a draw has sampled it
            const LLImageTex* it3 = imagep->getImageTexture();
            LLVkBridge::TextureNeed need3;
            if (it3 && it3->getTexName())
            {
                if (LLVkBridge::textureNeed(it3->getTexName(), need3))
                {
                    const bool seen = need3.mAge < 1.f;
                    const F32 area = seen ? need3.mWindow : need3.mKnown;
                    F32 vs = llmin(1.f / llmax(area, 1.0e-12f), LLViewerFetchedTexture::sMaxVirtualSize);
                    if (!seen || LLViewerTexture::sDesiredDiscardBias > 2.f)
                    {
                        vs /= bias;
                    }
                    max_vsize = vs;
                    on_screen = seen;
                    ++sFb.mUsed;
                }
                else
                {
                    max_vsize = llmin(max_vsize, 256.f * 256.f);
                    ++sFb.mCapped;
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
            break;
        }
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
    if (sFbMode)
    {
        static F32 last_fb_log = 0.f;
        if (gFrameTimeSeconds - last_fb_log >= 10.f)
        {
            static FbStats prev;
            if (last_fb_log > 0.f)
            {
                U64 textures = 0, readings = 0;
                LLVkBridge::textureNeedStats(textures, readings);
                static U64 prev_readings = 0;
                LL_INFOS("VkTexFb") << "need measured by the draws (10 s): " << textures << " textures have a measurement, " << readings - prev_readings << " readings; the face loop looked at " << sFb.mLooked - prev.mLooked
                                    << " textures: no measurement " << sFb.mNoData - prev.mNoData << " (name 0: " << sFb.mNoName - prev.mNoName << "), on screen by the loop with 64 pixels or more but not sampled in the last second "
                                    << sFb.mCpuOnlyBig - prev.mCpuOnlyBig << ", sampled but not on screen by the loop " << sFb.mGpuOnly - prev.mGpuOnly << ", both " << sFb.mBoth - prev.mBoth
                                    << "; log4(loop / measured) -4.." << 4 << ": " << sFb.mDiff[0] - prev.mDiff[0] << " " << sFb.mDiff[1] - prev.mDiff[1] << " " << sFb.mDiff[2] - prev.mDiff[2] << " " << sFb.mDiff[3] - prev.mDiff[3]
                                    << " [" << sFb.mDiff[4] - prev.mDiff[4] << "] " << sFb.mDiff[5] - prev.mDiff[5] << " " << sFb.mDiff[6] - prev.mDiff[6] << " " << sFb.mDiff[7] - prev.mDiff[7] << " " << sFb.mDiff[8] - prev.mDiff[8]
                                    << "; on screen by the loop, not sampled, by object (attachment, avatar, tree or grass, prim, other): " << sFb.mCpuOnlyKind[0] - prev.mCpuOnlyKind[0] << " " << sFb.mCpuOnlyKind[1] - prev.mCpuOnlyKind[1] << " "
                                    << sFb.mCpuOnlyKind[2] - prev.mCpuOnlyKind[2] << " " << sFb.mCpuOnlyKind[3] - prev.mCpuOnlyKind[3] << " " << sFb.mCpuOnlyKind[4] - prev.mCpuOnlyKind[4] << ", rigged " << sFb.mCpuOnlyRigged - prev.mCpuOnlyRigged
                                    << ", boosted " << sFb.mCpuOnlyBoost - prev.mCpuOnlyBoost << "; both, by object: " << sFb.mBothKind[0] - prev.mBothKind[0] << " " << sFb.mBothKind[1] - prev.mBothKind[1] << " " << sFb.mBothKind[2] - prev.mBothKind[2]
                                    << " " << sFb.mBothKind[3] - prev.mBothKind[3] << " " << sFb.mBothKind[4] - prev.mBothKind[4] << "; sized by the measurement " << sFb.mUsed - prev.mUsed << ", never measured and capped " << sFb.mCapped - prev.mCapped << LL_ENDL;
                std::ostringstream why;
                why << "; not sampled, by cause: every face in an occluded group " << sFb.mUnOcc - prev.mUnOcc << ", no face with a drawable " << sFb.mUnNoFace - prev.mUnNoFace << ", otherwise by pool (type:count)";
                for (U32 pt = 0; pt < 32; ++pt)
                {
                    if (sFb.mUnPool[pt] != prev.mUnPool[pt]) why << " " << pt << ":" << sFb.mUnPool[pt] - prev.mUnPool[pt];
                }
                LL_INFOS("VkTexFb") << why.str() << LL_ENDL;
                prev_readings = readings;
            }
            prev = sFb;
            last_fb_log = gFrameTimeSeconds;
        }
    }

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

    for (auto& imagep : entries)
    {
        mLastUpdateKey = LLTextureKey(imagep->getID(), (ETexListType)imagep->getTextureListType());

        if (imagep->getNumRefs() > 1) // make sure this image hasn't been deleted before attempting to update (may happen as a side effect of some other image updating)
        {
            { LL_CPU_ZONE("textures: decode priority (per texture)"); updateImageDecodePriority(imagep); }
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


