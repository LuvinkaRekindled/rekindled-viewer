/**
 * @file lldrawpoolbump.cpp
 * @brief LLDrawPoolBump class implementation
 *
 * $LicenseInfo:firstyear=2003&license=viewerlgpl$
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

#include "lldrawpoolbump.h"

#include "llstl.h"
#include "llviewercontrol.h"
#include "lldir.h"
#include "m3math.h"
#include "m4math.h"
#include "v4math.h"
#include "rkrenderheaders.h"
#include "llrender.h"

#include "llcubemap.h"
#include "lldrawable.h"
#include "llface.h"
#include "llsky.h"
#include "llstartup.h"
#include "lltextureentry.h"
#include "llviewercamera.h"
#include "llviewertexturelist.h"
#include "pipeline.h"
#include "llspatialpartition.h"
#include "llviewershadermgr.h"
#include "llmodel.h"

//#include "llimagebmp.h"
//#include "../tools/imdebug/imdebug.h"

// static
LLStandardBumpmap gStandardBumpmapList[TEM_BUMPMAP_COUNT];
LLRenderTarget LLBumpImageList::sRenderTarget;

// static
U32 LLStandardBumpmap::sStandardBumpmapCount = 0;

// static
LLBumpImageList gBumpImageList;

const S32 STD_BUMP_LATEST_FILE_VERSION = 1;

const U32 VERTEX_MASK_SHINY = LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_NORMAL | LLVertexBuffer::MAP_COLOR;
const U32 VERTEX_MASK_BUMP = LLVertexBuffer::MAP_VERTEX |LLVertexBuffer::MAP_TEXCOORD0 | LLVertexBuffer::MAP_TEXCOORD1;

U32 LLDrawPoolBump::sVertexMask = VERTEX_MASK_SHINY;


static LLShaderProgram* shader = NULL;
static S32 cube_channel = -1;
static S32 diffuse_channel = -1;
static S32 bump_channel = -1;
static bool shiny = false;

// static
void LLStandardBumpmap::shutdown()
{
    LLStandardBumpmap::destroyResources();
}

// static
void LLStandardBumpmap::restoreResources()
{
    addstandard();
}

// static
void LLStandardBumpmap::addstandard()
{
    if(!gTextureList.isInitialized())
    {
        //Note: loading pre-configuration sometimes triggers this call.
        //But it is safe to return here because bump images will be reloaded during initialization later.
        return ;
    }

    if (LLStartUp::getStartupState() < STATE_SEED_CAP_GRANTED)
    {
        // Not ready, need caps for images
        return;
    }

    // can't assert; we destroyResources and restoreResources a lot during *first* startup, which populates this list already, THEN we explicitly init the list as part of *normal* startup.  Sigh.  So clear the list every time before we (re-)add the standard bumpmaps.
    //llassert( LLStandardBumpmap::sStandardBumpmapCount == 0 );
    clear();
    LL_INFOS() << "Adding standard bumpmaps." << LL_ENDL;
    gStandardBumpmapList[LLStandardBumpmap::sStandardBumpmapCount++] = LLStandardBumpmap("None");       // BE_NO_BUMP
    gStandardBumpmapList[LLStandardBumpmap::sStandardBumpmapCount++] = LLStandardBumpmap("Brightness"); // BE_BRIGHTNESS
    gStandardBumpmapList[LLStandardBumpmap::sStandardBumpmapCount++] = LLStandardBumpmap("Darkness");   // BE_DARKNESS

    std::string file_name = gDirUtilp->getExpandedFilename( LL_PATH_APP_SETTINGS, "std_bump.ini" );
    LLFILE* file = LLFile::fopen( file_name, "rt" );     /*Flawfinder: ignore*/
    if( !file )
    {
        LL_WARNS() << "Could not open std_bump <" << file_name << ">" << LL_ENDL;
        return;
    }

    S32 file_version = 0;

    S32 fields_read = fscanf( file, "LLStandardBumpmap version %d", &file_version );
    if( fields_read != 1 )
    {
        fclose( file ); // <FS:ND/> Do not leak teh file handle.
        LL_WARNS() << "Bad LLStandardBumpmap header" << LL_ENDL;
        return;
    }

    if( file_version > STD_BUMP_LATEST_FILE_VERSION )
    {
        LL_WARNS() << "LLStandardBumpmap has newer version (" << file_version << ") than viewer (" << STD_BUMP_LATEST_FILE_VERSION << ")" << LL_ENDL;
        fclose( file ); // <FS:ND/> Do not leak the file handle
        return;
    }

    while( !feof(file) && (LLStandardBumpmap::sStandardBumpmapCount < (U32)TEM_BUMPMAP_COUNT) )
    {
        // *NOTE: This buffer size is hard coded into scanf() below.
        char label[2048] = "";  /* Flawfinder: ignore */
        char bump_image_id[2048] = "";  /* Flawfinder: ignore */
        fields_read = fscanf(   /* Flawfinder: ignore */
            file, "\n%2047s %2047s", label, bump_image_id);
        if( EOF == fields_read )
        {
            break;
        }
        if( fields_read != 2 )
        {
            LL_WARNS() << "Bad LLStandardBumpmap entry" << LL_ENDL;
            fclose( file ); // <FS:ND/> Do not leak the file handle
            return;
        }

//      LL_INFOS() << "Loading bumpmap: " << bump_image_id << " from viewerart" << LL_ENDL;
        gStandardBumpmapList[LLStandardBumpmap::sStandardBumpmapCount].mLabel = label;
        gStandardBumpmapList[LLStandardBumpmap::sStandardBumpmapCount].mImage =
            LLViewerTextureManager::getFetchedTexture(LLUUID(bump_image_id));
        gStandardBumpmapList[LLStandardBumpmap::sStandardBumpmapCount].mImage->setBoostLevel(LLBaseTexture::LOCAL) ;
        gStandardBumpmapList[LLStandardBumpmap::sStandardBumpmapCount].mImage->setLoadedCallback(LLBumpImageList::onSourceStandardLoaded, 0, true, false, NULL, NULL );
        gStandardBumpmapList[LLStandardBumpmap::sStandardBumpmapCount].mImage->forceToSaveRawImage(0, 30.f) ;
        LLStandardBumpmap::sStandardBumpmapCount++;
    }

    fclose( file );
}

// static
void LLStandardBumpmap::clear()
{
    LL_INFOS() << "Clearing standard bumpmaps." << LL_ENDL;
    for( U32 i = 0; i < LLStandardBumpmap::sStandardBumpmapCount; i++ )
    {
        gStandardBumpmapList[i].mLabel.assign("");
        gStandardBumpmapList[i].mImage = NULL;
    }
    sStandardBumpmapCount = 0;
}

// static
void LLStandardBumpmap::destroyResources()
{
    clear();
}



////////////////////////////////////////////////////////////////

LLDrawPoolBump::LLDrawPoolBump()
:  LLRenderPass(LLDrawPool::POOL_BUMP)
{
    shiny = false;
}


void LLDrawPoolBump::prerender()
{
    mShaderLevel = LLViewerShaderMgr::instance()->getShaderLevel(LLViewerShaderMgr::SHADER_OBJECT);
}

// static
S32 LLDrawPoolBump::numBumpPasses()
{
    return 1;
}


//static
void LLDrawPoolBump::bindCubeMap(LLShaderProgram* shader, S32 shader_level, S32& diffuse_channel, S32& cube_channel)
{
    LLCubeMap* cube_map = gSky.mVOSkyp ? gSky.mVOSkyp->getCubeMap() : NULL;
    if( cube_map && !LLPipeline::sReflectionProbesEnabled )
    {
        if (shader )
        {
            LLMatrix4 mat;
            mat.initRows(LLVector4(gRenderModelView+0),
                         LLVector4(gRenderModelView+4),
                         LLVector4(gRenderModelView+8),
                         LLVector4(gRenderModelView+12));
            LLVector3 vec = LLVector3(gShinyOrigin) * mat;
            LLVector4 vec4(vec, gShinyOrigin.mV[3]);
            shader->uniform4fv(LLViewerShaderMgr::SHINY_ORIGIN, 1, vec4.mV);
            if (shader_level > 1)
            {
                cube_map->setMatrix(1);
                // Make sure that texture coord generation happens for tex unit 1, as that's the one we use for
                // the cube map in the one pass shiny shaders
                cube_channel = shader->enableTexture(LLViewerShaderMgr::ENVIRONMENT_MAP, LLTexUnit::TT_CUBE_MAP);
                cube_map->enableTexture(cube_channel);
                diffuse_channel = shader->enableTexture(LLViewerShaderMgr::DIFFUSE_MAP);
            }
            else
            {
                cube_map->setMatrix(0);
                cube_channel = shader->enableTexture(LLViewerShaderMgr::ENVIRONMENT_MAP, LLTexUnit::TT_CUBE_MAP);
                diffuse_channel = -1;
                cube_map->enable(cube_channel);
            }
            gRender.getTexUnit(cube_channel)->bind(cube_map);
        }
        else
        {
            cube_channel = 0;
            diffuse_channel = -1;
            gRender.getTexUnit(0)->clear();
            cube_map->enable(0);
            cube_map->setMatrix(0);
            gRender.getTexUnit(0)->bind(cube_map);
        }
    }
}

//static
void LLDrawPoolBump::unbindCubeMap(LLShaderProgram* shader, S32 shader_level, S32& diffuse_channel, S32& cube_channel)
{
    LLCubeMap* cube_map = gSky.mVOSkyp ? gSky.mVOSkyp->getCubeMap() : NULL;
    if( cube_map && !LLPipeline::sReflectionProbesEnabled)
    {
        if (shader_level > 1)
        {
            shader->disableTexture(LLViewerShaderMgr::ENVIRONMENT_MAP, LLTexUnit::TT_CUBE_MAP);

            if (LLViewerShaderMgr::instance()->getShaderLevel(LLViewerShaderMgr::SHADER_OBJECT) > 0)
            {
                if (diffuse_channel != 0)
                {
                    shader->disableTexture(LLViewerShaderMgr::DIFFUSE_MAP);
                }
            }
        }
        // Moved below shader->disableTexture call to avoid false alarms from auto-re-enable of textures on stage 0
        // MAINT-755
        cube_map->disable();
        cube_map->restoreMatrix();
    }
}

// J1 K4: the fullbright shiny draws come from the scene table (one record of rows, the shiny pull program: alphaPullV + fullbrightShinyF); HUDs and what the table cannot carry take the old pass
static bool fullbright_shiny_table(bool rigged)
{
    if (LLPipeline::sRenderingHUDs)
    {
        // J1 K6 S1: the HUD's shiny draws from the scene table (the HUD's cull result was told to the worker before the passes)
        return !rigged && ll_scene_geo_hud_view() && ll_scene_geo_prepared(gPipeline.getCull()) && ll_scene_geo_hud_type(ll_scene_geo_type_number(LLRenderPass::PASS_FULLBRIGHT_SHINY));
    }
    return LLRenderPass::deferredPacketsEnabled() && (!rigged || ll_rigged_packets_enabled());
}

void LLDrawPoolBump::beginFullbrightShiny()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL; //LL_RECORD_BLOCK_TIME(FTM_RENDER_SHINY);

    sVertexMask = VERTEX_MASK_SHINY | LLVertexBuffer::MAP_TEXCOORD0;

    // Second pass: environment map
    // the scene table's programs (native only): the HUD's, the static and the rigged one
    shader = LLPipeline::sRenderingHUDs ? &gHUDFullbrightShinyPullProgram : (mRigged ? &gDeferredFullbrightShinyPullSkinProgram : &gDeferredFullbrightShinyPullProgram);

    // bind exposure map so fullbright shader can cancel out exposure
    S32 channel = shader->enableTexture(LLShaderMgr::EXPOSURE_MAP);
    if (channel > -1)
    {
        gRender.getTexUnit(channel)->bind(&gPipeline.mExposureMap);
    }

    LLCubeMap* cube_map = gSky.mVOSkyp ? gSky.mVOSkyp->getCubeMap() : NULL;

    if (cube_map && !LLPipeline::sReflectionProbesEnabled)
    {
        // Make sure that texture coord generation happens for tex unit 1, as that's the one we use for
        // the cube map in the one pass shiny shaders
        gRender.getTexUnit(1)->clear();
        cube_channel = shader->enableTexture(LLViewerShaderMgr::ENVIRONMENT_MAP, LLTexUnit::TT_CUBE_MAP);
        cube_map->enableTexture(cube_channel);
        diffuse_channel = shader->enableTexture(LLViewerShaderMgr::DIFFUSE_MAP);

        gRender.getTexUnit(cube_channel)->bind(cube_map);
    }

    {
        LLMatrix4 mat;
        mat.initRows(LLVector4(gRenderModelView+0),
                     LLVector4(gRenderModelView+4),
                     LLVector4(gRenderModelView+8),
                     LLVector4(gRenderModelView+12));
        shader->bind();

        LLVector3 vec = LLVector3(gShinyOrigin) * mat;
        LLVector4 vec4(vec, gShinyOrigin.mV[3]);
        shader->uniform4fv(LLViewerShaderMgr::SHINY_ORIGIN, 1, vec4.mV);

        if (LLPipeline::sReflectionProbesEnabled)
        {
            gPipeline.bindReflectionProbes(*shader);
        }
        else
        {
            gPipeline.setEnvMat(*shader);
        }
    }

    if (mShaderLevel > 1)
    { //indexed texture rendering, channel 0 is always diffuse
        diffuse_channel = 0;
    }

    shiny = true;
}

void LLDrawPoolBump::renderFullbrightShiny()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL; //LL_RECORD_BLOCK_TIME(FTM_RENDER_SHINY);

    // J1 K6 A3: the scene table's draw record (the shiny pull program is bound), nothing else draws this pass
    LLRenderState::BlendScope blend_enable(true);
    pushGeometryPackets(mRigged ? LLRenderPass::PASS_FULLBRIGHT_SHINY_RIGGED : LLRenderPass::PASS_FULLBRIGHT_SHINY, mRigged);
}

void LLDrawPoolBump::endFullbrightShiny()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL; //LL_RECORD_BLOCK_TIME(FTM_RENDER_SHINY);

    LLCubeMap* cube_map = gSky.mVOSkyp ? gSky.mVOSkyp->getCubeMap() : NULL;
    if( cube_map && !LLPipeline::sReflectionProbesEnabled )
    {
        cube_map->disable();
        if (shader->mFeatures.hasReflectionProbes)
        {
            gPipeline.unbindReflectionProbes(*shader);
        }
        shader->unbind();
    }

    diffuse_channel = -1;
    cube_channel = 0;
    shiny = false;
}

//static
bool LLDrawPoolBump::bindBumpMap(LLFace* face, S32 channel)
{
    const LLTextureEntry* te = face->getTextureEntry();
    if (te)
    {
        U8 bump_code = te->getBumpmap();
        return bindBumpMap(bump_code, face->getTexture(), channel);
    }

    return false;
}

//static
LLViewerTexture* LLDrawPoolBump::bumpImageFor(U8 bump_code, LLViewerTexture* texture, bool generate)
{
    LLViewerFetchedTexture* tex = LLViewerTextureManager::staticCastToFetchedTexture(texture) ;
    if(!tex)
    {
        //if the texture is not a fetched texture
        return nullptr;
    }

    LLViewerTexture* bump = NULL;

    switch( bump_code )
    {
    case BE_NO_BUMP:
        break;
    case BE_BRIGHTNESS:
    case BE_DARKNESS:
        bump = generate ? gBumpImageList.getBrightnessDarknessImage( tex, bump_code ) : gBumpImageList.getBrightnessDarknessEntry( tex, bump_code );
        break;

    default:
        if( bump_code < LLStandardBumpmap::sStandardBumpmapCount )
        {
            bump = gStandardBumpmapList[bump_code].mImage;
            if (generate)
            {
                gBumpImageList.addTextureStats(bump_code, tex->getID(), tex->getMaxVirtualSize());
            }
        }
        break;
    }
    return bump;
}

//static
bool LLDrawPoolBump::bindBumpMap(U8 bump_code, LLViewerTexture* texture, S32 channel)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    //Note: texture atlas does not support bump texture now.
    LLViewerTexture* bump = bumpImageFor(bump_code, texture, true);

    if (bump)
    {
        if (channel == -2)
        {
            gRender.getTexUnit(1)->bindFast(bump);
            gRender.getTexUnit(0)->bindFast(bump);
        }
        else
        {
            // NOTE: do not use bindFast here (see SL-16222)
            gRender.getTexUnit(channel)->bind(bump);
        }

        return true;
    }

    return false;
}

//static
void LLDrawPoolBump::beginBump()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL; //LL_RECORD_BLOCK_TIME(FTM_RENDER_BUMP);
    sVertexMask = VERTEX_MASK_BUMP;
    // Optional second pass: emboss bump map

    shader = mRigged ? &gDeferredEmbossPullSkinProgram : &gDeferredEmbossPullProgram;       // J1 K4: the rows' program (native only)

    shader->bind();

    gRender.setSceneBlendType(LLRender::BT_MULT_X2);
}

// J1 K4: the post deferred emboss draws come from the scene table in the main view (rows with the bump image's slot, the emboss pull program); every other view, and what the rows cannot carry, draws the old way
static bool emboss_table(bool rigged)
{
    // J1 K6 S1: the HUD's post bump pass too (no rigged draws in a HUD; the HUD's cull result was told to the worker before the passes)
    const bool view_ok = !LLPipeline::sRenderingHUDs || (!rigged && ll_scene_geo_hud_view() && ll_scene_geo_hud_type(ll_scene_geo_type_number(LLRenderPass::PASS_POST_BUMP)));
    return view_ok && LLRenderPass::deferredPacketsEnabled() && ll_scene_geo_main_view() && ll_scene_geo_prepared(gPipeline.getCull())
        && (!rigged || ll_rigged_packets_enabled());
}

//static
void LLDrawPoolBump::renderBump()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL; //LL_RECORD_BLOCK_TIME(FTM_RENDER_BUMP);
    LLRenderState::DepthScope gls_depth(true, false, LLRenderState::CompareOp::LessEqual);
    LLRenderState::BlendScope blend(true);
    gRender.diffuseColor4f(1,1,1,1);
    /// Get rid of z-fighting with non-bump pass.
    LLRenderState::PolygonOffsetScope polyOffset(true);
    gRender.setPolygonOffset(-1.0f, -1.0f);
    // J1 K6 A3: the scene table's draw record (the emboss pull program is bound)
    pushGeometryPackets(mRigged ? LLRenderPass::PASS_POST_BUMP_RIGGED : LLRenderPass::PASS_POST_BUMP, mRigged);
}

//static
void LLDrawPoolBump::endBump(U32 pass)
{
    LLShaderProgram::unbind();

    gRender.setSceneBlendType(LLRender::BT_ALPHA);
}

S32 LLDrawPoolBump::getNumDeferredPasses()
{
    return 1;
}

// J1 K4: the deferred bump draws come from the scene table (rows with the diffuse and bump image slots, the bump pull program) in the main view, whose fragments the table was told; every other view, and what
// the rows cannot carry, draws the old way
static bool bump_table(bool rigged)
{
    return !LLPipeline::sRenderingHUDs && LLRenderPass::deferredPacketsEnabled() && ll_scene_geo_main_view() && ll_scene_geo_prepared(gPipeline.getCull())
        && (!rigged || ll_rigged_packets_enabled());
}

void LLDrawPoolBump::renderDeferred(S32 pass)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL; //LL_RECORD_BLOCK_TIME(FTM_RENDER_BUMP);

    // J1 K6 A3: the deferred bump draws are scene table records (rows with the diffuse and bump image slots, the bump pull programs)
    for (int i = 0; i < 2; ++i)
    {
        const bool rigged = i == 1;
        if (!bump_table(rigged))
        {
            continue;
        }
        (rigged ? &gDeferredBumpPullSkinProgram : &gDeferredBumpPullProgram)->bind();
        pushGeometryPackets(rigged ? LLRenderPass::PASS_BUMP_RIGGED : LLRenderPass::PASS_BUMP, rigged);
        LLShaderProgram::sCurBoundShaderPtr->unbind();
    }
}


void LLDrawPoolBump::renderPostDeferred(S32 pass)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;

    S32 num_passes = LLPipeline::sRenderingHUDs ? 1 : 2; // skip rigged pass when rendering HUDs

    for (int i = 0; i < num_passes; ++i)
    { // two passes -- static and rigged
        mRigged = (i == 1);

        // J1 K6 A3: render shiny and bump (emboss) from the scene table; a pass whose table is not available draws nothing
        if (fullbright_shiny_table(mRigged))
        {
            beginFullbrightShiny();
            renderFullbrightShiny();
            endFullbrightShiny();
        }

        if (emboss_table(mRigged))
        {
            beginBump();
            renderBump();
            endBump();
        }
    }
}


////////////////////////////////////////////////////////////////
// List of bump-maps created from other textures.


//const LLUUID TEST_BUMP_ID("3d33eaf2-459c-6f97-fd76-5fce3fc29447");

void LLBumpImageList::init()
{
    llassert( mBrightnessEntries.size() == 0 );
    llassert( mDarknessEntries.size() == 0 );

    LLStandardBumpmap::restoreResources();
}

void LLBumpImageList::clear()
{
    LL_INFOS() << "Clearing dynamic bumpmaps." << LL_ENDL;
    // these will be re-populated on-demand
    mBrightnessEntries.clear();
    mDarknessEntries.clear();

    sRenderTarget.release();

    LLStandardBumpmap::clear();
}

void LLBumpImageList::shutdown()
{
    clear();
    LLStandardBumpmap::shutdown();
}

void LLBumpImageList::destroyResources()
{
    clear();
    LLStandardBumpmap::destroyResources();
}

void LLBumpImageList::restoreResources()
{
    if(!gTextureList.isInitialized())
    {
        //safe to return here because bump images will be reloaded during initialization later.
        return ;
    }

    LLStandardBumpmap::restoreResources();
    // Images will be recreated as they are needed.
}


LLBumpImageList::~LLBumpImageList()
{
    // Shutdown should have already been called.
    llassert( mBrightnessEntries.size() == 0 );
    llassert( mDarknessEntries.size() == 0 );
}


// Note: Does nothing for entries in gStandardBumpmapList that are not actually standard bump images (e.g. none, brightness, and darkness)
void LLBumpImageList::addTextureStats(U8 bump, const LLUUID& base_image_id, F32 virtual_size)
{
    bump &= TEM_BUMP_MASK;
    LLViewerFetchedTexture* bump_image = gStandardBumpmapList[bump].mImage;
    if( bump_image )
    {
        bump_image->addTextureStats(virtual_size);
    }
}


void LLBumpImageList::updateImages()
{
    llassert(LLCoros::on_main_thread_main_coro()); // This code is not thread safe

    for (bump_image_map_t::iterator iter = mBrightnessEntries.begin(); iter != mBrightnessEntries.end(); )
    {
        LLViewerTexture* image = iter->second;
        if( image )
        {
            bool destroy = true;
            if( image->hasImageTexture())
            {
                if( image->getBoundRecently() )
                {
                    destroy = false;
                }
                else
                {
                    image->destroyImageTexture();
                }
            }

            if( destroy )
            {
                //LL_INFOS() << "*** Destroying bright " << (void*)image << LL_ENDL;
                iter = mBrightnessEntries.erase(iter);   // deletes the image thanks to reference counting
                continue;
            }
        }
        ++iter;
    }

    for (bump_image_map_t::iterator iter = mDarknessEntries.begin(); iter != mDarknessEntries.end(); )
    {
        bump_image_map_t::iterator curiter = iter++;
        LLViewerTexture* image = curiter->second;
        if( image )
        {
            bool destroy = true;
            if( image->hasImageTexture())
            {
                if( image->getBoundRecently() )
                {
                    destroy = false;
                }
                else
                {
                    image->destroyImageTexture();
                }
            }

            if( destroy )
            {
                //LL_INFOS() << "*** Destroying dark " << (void*)image << LL_ENDL;;
                mDarknessEntries.erase(curiter);  // deletes the image thanks to reference counting
            }
        }
    }
}


// Note: the caller SHOULD NOT keep the pointer that this function returns.  It may be updated as more data arrives.
LLViewerTexture* LLBumpImageList::getBrightnessDarknessEntry(LLViewerFetchedTexture* src_image, U8 bump_code)
{
    bump_image_map_t& entries_list = bump_code == BE_BRIGHTNESS ? mBrightnessEntries : mDarknessEntries;
    bump_image_map_t::iterator iter = entries_list.find(src_image->getID());
    if (iter == entries_list.end() || iter->second.isNull())
    {
        entries_list[src_image->getID()] = LLViewerTextureManager::getLocalTexture(true);       // (as onSourceUpdated makes an entry: the image is generated when the pass draws)
        iter = entries_list.find(src_image->getID());
    }
    return iter->second;
}

LLViewerTexture* LLBumpImageList::getBrightnessDarknessImage(LLViewerFetchedTexture* src_image, U8 bump_code )
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    llassert( (bump_code == BE_BRIGHTNESS) || (bump_code == BE_DARKNESS) );

    LLViewerTexture* bump = nullptr;

    bump_image_map_t* entries_list = nullptr;

    switch( bump_code )
    {
    case BE_BRIGHTNESS:
        entries_list = &mBrightnessEntries;
        break;
    case BE_DARKNESS:
        entries_list = &mDarknessEntries;
        break;
    default:
        llassert(0);
        return nullptr;
    }

    bump_image_map_t::iterator iter = entries_list->find(src_image->getID());
    if (iter != entries_list->end() && iter->second.notNull())
    {
        bump = iter->second;
    }

    if (bump == nullptr ||
        src_image->getWidth() != bump->getWidth() ||
        src_image->getHeight() != bump->getHeight())
    {
        onSourceUpdated(src_image, (EBumpEffect) bump_code);
    }

    return (*entries_list)[src_image->getID()];
}


void LLBumpImageList::onSourceStandardLoaded( bool success, LLViewerFetchedTexture* src_vi, LLImageRaw* src, LLImageRaw* aux_src, S32 discard_level, bool final, void* userdata)
{
    if (success && LLPipeline::sRenderDeferred)
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
        LLPointer<LLImageRaw> nrm_image = new LLImageRaw(src->getWidth(), src->getHeight(), 4);
        {
            generateNormalMapFromAlpha(src, nrm_image);
        }
        src_vi->setExplicitFormat(TexelFormat::RGBA8, PixelFormat::RGBA);
        {
            if (!src_vi->createImageTexture(src_vi->getDiscardLevel(), nrm_image))
            {
                LL_WARNS() << "Failed to create bump image texture for image " << src_vi->getID() << LL_ENDL;
            }
        }
    }
}

void LLBumpImageList::generateNormalMapFromAlpha(LLImageRaw* src, LLImageRaw* nrm_image)
{
    LLImageDataSharedLock lockIn(src);
    LLImageDataLock lockOut(nrm_image);

    U8* nrm_data = nrm_image->getData();
    S32 resX = src->getWidth();
    S32 resY = src->getHeight();

    const U8* src_data = src->getData();

    S32 src_cmp = src->getComponents();

    // <FS:PP> Attempt to speed up things a little
    // F32 norm_scale = gSavedSettings.getF32("RenderNormalMapScale");
    static LLCachedControl<F32> RenderNormalMapScale(gSavedSettings, "RenderNormalMapScale");
    F32 norm_scale = RenderNormalMapScale;
    // </FS:PP>

    U32 idx = 0;
    //generate normal map from pseudo-heightfield
    for (S32 j = 0; j < resY; ++j)
    {
        for (S32 i = 0; i < resX; ++i)
        {
            S32 rX = (i+1)%resX;
            S32 rY = (j+1)%resY;
            S32 lX = (i-1)%resX;
            S32 lY = (j-1)%resY;

            if (lX < 0)
            {
                lX += resX;
            }
            if (lY < 0)
            {
                lY += resY;
            }

            F32 cH = (F32) src_data[(j*resX+i)*src_cmp+src_cmp-1];

            LLVector3 right = LLVector3(norm_scale, 0, (F32) src_data[(j*resX+rX)*src_cmp+src_cmp-1]-cH);
            LLVector3 left = LLVector3(-norm_scale, 0, (F32) src_data[(j*resX+lX)*src_cmp+src_cmp-1]-cH);
            LLVector3 up = LLVector3(0, -norm_scale, (F32) src_data[(lY*resX+i)*src_cmp+src_cmp-1]-cH);
            LLVector3 down = LLVector3(0, norm_scale, (F32) src_data[(rY*resX+i)*src_cmp+src_cmp-1]-cH);

            LLVector3 norm = right%down + down%left + left%up + up%right;

            norm.normVec();

            norm *= 0.5f;
            norm += LLVector3(0.5f,0.5f,0.5f);

            idx = (j*resX+i)*4;
            nrm_data[idx+0]= (U8) (norm.mV[0]*255);
            nrm_data[idx+1]= (U8) (norm.mV[1]*255);
            nrm_data[idx+2]= (U8) (norm.mV[2]*255);
            nrm_data[idx+3]= src_data[(j*resX+i)*src_cmp+src_cmp-1];
        }
    }
}

// static
void LLBumpImageList::onSourceUpdated(LLViewerTexture* src, EBumpEffect bump_code)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;

    const LLUUID& src_id = src->getID();

    bump_image_map_t& entries_list(bump_code == BE_BRIGHTNESS ? gBumpImageList.mBrightnessEntries : gBumpImageList.mDarknessEntries);
    bump_image_map_t::iterator iter = entries_list.find(src_id);

    if (iter == entries_list.end())
    { //make sure an entry exists for this image
        entries_list[src_id] = LLViewerTextureManager::getLocalTexture(true);
        iter = entries_list.find(src_id);
    }

    //---------------------------------------------------
    // immediately assign bump to a smart pointer in case some local smart pointer
    // accidentally releases it.
    LLPointer<LLViewerTexture> bump = iter->second;

    if (bump->getWidth() != src->getWidth() ||
        bump->getHeight() != src->getHeight()) // bump not cached yet or has changed resolution
    {
        //convert to normal map
        LL_PROFILE_ZONE_NAMED("bil - create normal map");

        bump->setExplicitFormat(TexelFormat::RGBA8, PixelFormat::RGBA);

        LLImageTex* src_img = src->getImageTexture();
        LLImageTex* dst_img = bump->getImageTexture();
        if (!dst_img->setSize(src->getWidth(), src->getHeight(), 4, 0))
        {
            LL_WARNS() << "Failed to setSize for image " << bump->getID() << LL_ENDL;
        }
        dst_img->setUseMipMaps(true);
        dst_img->setDiscardLevel(0);
        dst_img->createImageTexture();

        gRender.getTexUnit(0)->bind(bump);

        LLImageTex::setManualImage(dst_img->getTexName(), TextureTarget::Tex2D, 0, TexelFormat::RGBA8, dst_img->getWidth(), dst_img->getHeight(), PixelFormat::RGBA, ComponentType::U8, nullptr, false);

        U32 tex_name = dst_img->getTexName();
        // point render target at empty buffer
        sRenderTarget.setColorAttachment(bump->getImageTexture(), tex_name);

        // generate normal map in empty texture
        {
            RenderPassScope pass_920(sRenderTarget);

            LLRenderState::DepthScope depth(false);
            LLRenderState::CullScope cull(false);
            LLRenderState::BlendScope blend(false);
            gRender.setColorMask(true, true);

            LLShaderProgram* shader = LLShaderProgram::sCurBoundShaderPtr;
            gNormalMapGenProgram.bind();

            static LLStaticHashedString sNormScale("norm_scale");
            static LLStaticHashedString sStepX("stepX");
            static LLStaticHashedString sStepY("stepY");
            static LLStaticHashedString sBumpCode("bump_code");

            // <FS:PP> Speed optimisation
            // gNormalMapGenProgram.uniform1f(sNormScale, gSavedSettings.getF32("RenderNormalMapScale"));
            static LLCachedControl<F32> RenderNormalMapScale(gSavedSettings, "RenderNormalMapScale");
            gNormalMapGenProgram.uniform1f(sNormScale, RenderNormalMapScale());
            // </FS:PP>
            gNormalMapGenProgram.uniform1f(sStepX, 1.f / bump->getWidth());
            gNormalMapGenProgram.uniform1f(sStepY, 1.f / bump->getHeight());
            gNormalMapGenProgram.uniform1i(sBumpCode, bump_code);

            gRender.getTexUnit(0)->bind(src);

            gRender.begin(LLRender::TRIANGLE_STRIP);
            gRender.texCoord2f(0, 0);
            gRender.vertex2f(0, 0);

            gRender.texCoord2f(0, 1);
            gRender.vertex2f(0, 1);

            gRender.texCoord2f(1, 0);
            gRender.vertex2f(1, 0);

            gRender.texCoord2f(1, 1);
            gRender.vertex2f(1, 1);

            gRender.end();

            gRender.flush();

            pass_920.end();
            sRenderTarget.releaseColorAttachment();

            if (shader)
            {
                shader->bind();
            }
        }

        // generate mipmap
        gRender.getTexUnit(0)->bind(bump);
        gRender.generateMipmap(bump->getImageTexture()->getTexName());
        gRender.getTexUnit(0)->bindWhite();
    }

    iter->second = bump; // derefs (and deletes) old image
    //---------------------------------------------------

}
