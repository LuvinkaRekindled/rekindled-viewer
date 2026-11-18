/**
 * @file lltoolmorph.cpp
 * @brief A tool to manipulate faces..
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

#include "llviewerprecompiledheaders.h"

// File includes
#include "lltoolmorph.h"
#include "llrender.h"

// Library includes
#include "llaudioengine.h"
#include "llviewercontrol.h"
#include "llfontgl.h"
#include "llwearable.h"
#include "sound_ids.h"
#include "v3math.h"
#include "v3color.h"

// Viewer includes
#include "llagent.h"
#include "lldrawable.h"
#include "lldrawpoolavatar.h"
#include "llface.h"
#include "llmorphview.h"
#include "llresmgr.h"
#include "llselectmgr.h"
#include "llsky.h"
#include "lltexlayer.h"
#include "lltoolmgr.h"
#include "llui.h"
#include "llviewercamera.h"
#include "llviewertexturelist.h"
#include "llviewerobject.h"
#include "llviewerwearable.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"
#include "pipeline.h"


//static
LLVisualParamHint::instance_list_t LLVisualParamHint::sInstances;
bool LLVisualParamReset::sDirty = false;

//-----------------------------------------------------------------------------
// LLVisualParamHint()
//-----------------------------------------------------------------------------

// static
LLVisualParamHint::LLVisualParamHint(
    S32 pos_x, S32 pos_y,
    S32 width, S32 height,
    LLViewerJointMesh *mesh,
    LLViewerVisualParam *param,
    LLWearable *wearable,
    F32 param_weight,
    LLJoint* jointp)
    :
    LLViewerDynamicTexture(width, height, 3, LLViewerDynamicTexture::ORDER_MIDDLE, true ),
    mNeedsUpdate( true ),
    mIsVisible( false ),
    mJointMesh( mesh ),
    mVisualParam( param ),
    mWearablePtr( wearable ),
    mVisualParamWeight( param_weight ),
    mAllowsUpdates( true ),
    mDelayFrames( 0 ),
    mRect( pos_x, pos_y + height, pos_x + width, pos_y ),
    mLastParamWeight(0.f),
    mCamTargetJoint(jointp)
{
    LLVisualParamHint::sInstances.insert( this );
    mBackgroundp = LLUI::getUIImage("avatar_thumb_bkgrnd.png");
    if (mBackgroundp)
    {
        LLViewerFetchedTexture* tex = dynamic_cast<LLViewerFetchedTexture*>(mBackgroundp->getImage().get());
        if (tex)
        {
            tex->setKnownDrawSize(width, height);
        }

        mBackgroundLoadedConnection = mBackgroundp->addLoadedCallback([this]()
        {
            mNeedsUpdate = true;
            mDelayFrames = 0;
        });
    }

    llassert(width != 0);
    llassert(height != 0);
}

//-----------------------------------------------------------------------------
// ~LLVisualParamHint()
//-----------------------------------------------------------------------------
LLVisualParamHint::~LLVisualParamHint()
{
    LLVisualParamHint::sInstances.erase( this );
    mBackgroundLoadedConnection.disconnect();
}

//virtual
S8 LLVisualParamHint::getType() const
{
    return LLViewerDynamicTexture::LL_VISUAL_PARAM_HINT ;
}

//-----------------------------------------------------------------------------
// static
// requestHintUpdates()
// Requests updates for all instances (excluding two possible exceptions)  Grungy but efficient.
//-----------------------------------------------------------------------------
void LLVisualParamHint::requestHintUpdates( LLVisualParamHint* exception1, LLVisualParamHint* exception2 )
{
    S32 delay_frames = 0;
    for (instance_list_t::iterator iter = sInstances.begin();
         iter != sInstances.end(); ++iter)
    {
        LLVisualParamHint* instance = *iter;
        if( (instance != exception1) && (instance != exception2) )
        {
            if( instance->mAllowsUpdates )
            {
                instance->mNeedsUpdate = true;
                instance->mDelayFrames = delay_frames;
                delay_frames++;
            }
            else
            {
                instance->mNeedsUpdate = true;
                instance->mDelayFrames = 0;
            }
        }
    }
}

bool LLVisualParamHint::needsRender()
{
    // needsRender gets called once per frame from
    // LLViewerDynamicTexture::updateAllInstances()
    return mNeedsUpdate && mDelayFrames-- <= 0 && !gAgentAvatarp->getIsAppearanceAnimating() && mAllowsUpdates;
}

void LLVisualParamHint::preRender(bool clear_depth)
{
    LL_DEBUGS("ParamHint") << "preRender: param=" << mVisualParam->getName()
        << " id=" << mVisualParam->getID()
        << " weight=" << mVisualParamWeight
        << " dirtyMesh=" << gAgentAvatarp->mDirtyMesh
        << " pixelArea=" << gAgentAvatarp->mAdjustedPixelArea
        << " appearanceAnimating=" << gAgentAvatarp->getIsAppearanceAnimating()
        << LL_ENDL;
    LLViewerWearable* wearable = dynamic_cast<LLViewerWearable*>(mWearablePtr);
    if (wearable)
    {
        wearable->setVolatile(true);
    }
    mLastParamWeight = mVisualParam->getWeight();
    // <FS:Ansariel> [Legacy Bake]
    //mWearablePtr->setVisualParamWeight(mVisualParam->getID(), mVisualParamWeight);
    //gAgentAvatarp->setVisualParamWeight(mVisualParam->getID(), mVisualParamWeight);
    mWearablePtr->setVisualParamWeight(mVisualParam->getID(), mVisualParamWeight, false);
    gAgentAvatarp->setVisualParamWeight(mVisualParam->getID(), mVisualParamWeight, false);
    // </FS:Ansariel> [Legacy Bake]
    gAgentAvatarp->setVisualParamWeight("Blink_Left", 0.f);
    gAgentAvatarp->setVisualParamWeight("Blink_Right", 0.f);
    gAgentAvatarp->updateComposites();
    // Calling LLCharacter version, as we don't want position/height changes to cause the avatar to jump
    // up and down when we're doing preview renders. -Nyx
    gAgentAvatarp->LLCharacter::updateVisualParams();

    if (gAgentAvatarp->mDrawable.notNull())
    {
        gAgentAvatarp->updateGeometry(gAgentAvatarp->mDrawable);
        gAgentAvatarp->updateLOD();

        if (gAgentAvatarp->mDrawable->isState(LLDrawable::REBUILD_GEOMETRY)
            || gAgentAvatarp->mDirtyMesh > 0)
        {
            // updateLOD marks meshes for an update.
            LL_DEBUGS("ParamHint") << "preRender: updateMeshData"
                << " param=" << mVisualParam->getName()
                << " dirtyMesh=" << gAgentAvatarp->mDirtyMesh
                << " rebuildGeom=" << gAgentAvatarp->mDrawable->isState(LLDrawable::REBUILD_GEOMETRY)
                << LL_ENDL;
            // Clear states and force update so that renderSkinned inside generateImpostor
            // does not re-run updateMeshData() with a partially consistent mesh state,
            // which would reassign mFace pointers on some meshes while others are
            // mid-draw with stale vertex offsets.
            gAgentAvatarp->updateMeshData();
        }
    }
    else
    {
        LL_WARNS() << "Attempting to update avatar's geometry, but drawable doesn't exist yet" << LL_ENDL;
    }

    LLViewerDynamicTexture::preRender(clear_depth);
}

//-----------------------------------------------------------------------------
// render()
//-----------------------------------------------------------------------------
bool LLVisualParamHint::render()
{
    LLVisualParamReset::sDirty = true;

    gRender.pushUIMatrix();
    gRender.loadUIIdentity();

    RKRenderMatrix::push(RKRenderMatrix::Kind::Projection);
    RKRenderMatrix::identity(RKRenderMatrix::Kind::Projection);
    RKRenderMatrix::ortho(RKRenderMatrix::Kind::Projection, 0.0f, (F32)mFullWidth, 0.0f, (F32)mFullHeight, -1.0f, 1.0f);

    RKRenderMatrix::push(RKRenderMatrix::Kind::Modelview);
    RKRenderMatrix::identity(RKRenderMatrix::Kind::Modelview);

    gUIProgram.bind();

    RKRenderPresetUIDefault gls_ui;
    //LLGLState::verify(true);
    mBackgroundp->draw(0, 0, mFullWidth, mFullHeight);

    RKRenderMatrix::pop(RKRenderMatrix::Kind::Projection);

    RKRenderMatrix::pop(RKRenderMatrix::Kind::Modelview);

    mNeedsUpdate = false;
    mIsVisible = true;

    LLQuaternion avatar_rotation;
    LLJoint* root_joint = gAgentAvatarp->getRootJoint();
    if( root_joint )
    {
        avatar_rotation = root_joint->getWorldRotation();
    }

    LLVector3 target_joint_pos = mCamTargetJoint->getWorldPosition();

    LLVector3 target_offset( 0, 0, mVisualParam->getCameraElevation() );
    LLVector3 target_pos = target_joint_pos + (target_offset * avatar_rotation);

    F32 cam_angle_radians = mVisualParam->getCameraAngle() * DEG_TO_RAD;

    static LLCachedControl<bool> auto_camera_position(gSavedSettings, "AppearanceCameraMovement");
    if (!auto_camera_position)
    {
        cam_angle_radians += F_PI;
    }

    LLVector3 camera_snapshot_offset(
        mVisualParam->getCameraDistance() * cosf( cam_angle_radians ),
        mVisualParam->getCameraDistance() * sinf( cam_angle_radians ),
        mVisualParam->getCameraElevation() );
    LLVector3 camera_pos = target_joint_pos + (camera_snapshot_offset * avatar_rotation);

    gRender.flush();

    LLViewerCamera::getInstance()->setAspect((F32)mFullWidth / (F32)mFullHeight);
    LLViewerCamera::getInstance()->setOriginAndLookAt(
        camera_pos,         // camera
        LLVector3::z_axis,  // up
        target_pos );       // point of interest

    LLViewerCamera::getInstance()->setPerspective(false, mOrigin.mX, mOrigin.mY, mFullWidth, mFullHeight, false);

    llassert(!LLPipeline::sImpostorRender); // We are rendering this from UI, shouldn't be set
    if (gAgentAvatarp->mDrawable.notNull() && !LLPipeline::sImpostorRender)
    {
        LL_DEBUGS("ParamHint") << "render: generating impostor"
            << " param=" << mVisualParam->getName()
            << " drawableState=0x" << std::hex << gAgentAvatarp->mDrawable->getState() << std::dec
            << " dirtyMesh=" << gAgentAvatarp->mDirtyMesh
            << " mMeshValid=" << gAgentAvatarp->mMeshValid
            << LL_ENDL;
        RKRenderState::DepthScope gls_depth(true, true);
        gRender.flush();
        gRender.setSceneBlendType(LLRender::BT_REPLACE);
        gPipeline.generateImpostor(gAgentAvatarp, true);
        gRender.setSceneBlendType(LLRender::BT_ALPHA);
        gRender.flush();
    }

    gAgentAvatarp->setVisualParamWeight(mVisualParam->getID(), mLastParamWeight);
    // <FS:Ansariel> [Legacy Bake]
    //mWearablePtr->setVisualParamWeight(mVisualParam->getID(), mLastParamWeight);
    mWearablePtr->setVisualParamWeight(mVisualParam->getID(), mLastParamWeight, false);
    LLViewerWearable* wearable = dynamic_cast<LLViewerWearable*>(mWearablePtr);
    if (wearable)
    {
        wearable->setVolatile(false);
    }

    gAgentAvatarp->updateVisualParams();
    gRender.color4f(1,1,1,1);
    mImageTexp->setImageTextureCreated(true);
    gRender.popUIMatrix();

    return true;
}


//-----------------------------------------------------------------------------
// draw()
//-----------------------------------------------------------------------------
void LLVisualParamHint::draw(F32 alpha)
{
    if (!mIsVisible) return;

    gRender.getTexUnit(0)->bind(this);

    gRender.color4f(1.f, 1.f, 1.f, alpha);

    RKRenderPresetUIDefault gls_ui;
    gRender.begin(LLRender::TRIANGLES);
    {
        gRender.texCoord2i(0, 1);
        gRender.vertex2i(0, mFullHeight);
        gRender.texCoord2i(0, 0);
        gRender.vertex2i(0, 0);
        gRender.texCoord2i(1, 0);
        gRender.vertex2i(mFullWidth, 0);

        gRender.texCoord2i(0, 1);
        gRender.vertex2i(0, mFullHeight);
        gRender.texCoord2i(1, 0);
        gRender.vertex2i(mFullWidth, 0);
        gRender.texCoord2i(1, 1);
        gRender.vertex2i(mFullWidth, mFullHeight);
    }
    gRender.end();

    gRender.getTexUnit(0)->bindWhite();
}

//-----------------------------------------------------------------------------
// LLVisualParamReset()
//-----------------------------------------------------------------------------
LLVisualParamReset::LLVisualParamReset() : LLViewerDynamicTexture(1, 1, 1, ORDER_RESET, false)
{
}

//virtual
S8 LLVisualParamReset::getType() const
{
    return LLViewerDynamicTexture::LL_VISUAL_PARAM_RESET ;
}

//-----------------------------------------------------------------------------
// render()
//-----------------------------------------------------------------------------
bool LLVisualParamReset::render()
{
    if (sDirty)
    {
        gAgentAvatarp->updateComposites();
        gAgentAvatarp->updateVisualParams();
        gAgentAvatarp->updateGeometry(gAgentAvatarp->mDrawable);
        sDirty = false;
    }

    return false;
}
