/**
 * @file rkbasetexture.cpp
 * @brief Opengl texture implementation
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
#include "linden_common.h"
#include "rkbasetexture.h"


RKBaseTexture::RKBaseTexture(bool usemipmaps)
{
    init();
    mUseMipMaps = usemipmaps;
}

RKBaseTexture::RKBaseTexture(const U32 width, const U32 height, const U8 components, bool usemipmaps)
{
    init();
    mFullWidth = width ;
    mFullHeight = height ;
    mUseMipMaps = usemipmaps;
    mComponents = components ;
    setTexelsPerImage();
}

RKBaseTexture::RKBaseTexture(const LLImageRaw* raw, bool usemipmaps)
{
    init();
    mUseMipMaps = usemipmaps ;
    // Create an empty image of the specified size and width
    mImageTexp = new RKImageTex(raw, usemipmaps) ;
    mFullWidth = mImageTexp->getWidth();
    mFullHeight = mImageTexp->getHeight();
    mComponents = mImageTexp->getComponents();
    setTexelsPerImage();
}

RKBaseTexture::~RKBaseTexture()
{
    cleanup();
}

void RKBaseTexture::init()
{
    mBoostLevel = RKBaseTexture::BOOST_NONE;
    // <FS:minerjr> [FIRE-36016] - Re-added Store/Restore boost levels of selected objects
    // Added a previous boost level to allow for restoring boost after BOOST_SELECTED is applied
    mPrevBoostLevel = RKBaseTexture::BOOST_NONE;
    // </FS:minerjr> [FIRE-36016]

    mFullWidth = 0;
    mFullHeight = 0;
    mTexelsPerImage = 0 ;
    mUseMipMaps = false ;
    mComponents = 0 ;

    mTextureState = NO_DELETE ;
    mDontDiscard = false;
    mNeedsImageTexture = false ;
}

void RKBaseTexture::cleanup()
{
    if(mImageTexp)
    {
        mImageTexp->cleanup();
    }
}

// virtual
void RKBaseTexture::dump()
{
    if(mImageTexp)
    {
        mImageTexp->dump();
    }
}

void RKBaseTexture::setBoostLevel(S32 level)
{
    if(mBoostLevel != level)
    {
        mBoostLevel = level ;
        if(mBoostLevel != RKBaseTexture::BOOST_NONE
           && mBoostLevel != RKBaseTexture::BOOST_ICON
           && mBoostLevel != RKBaseTexture::BOOST_THUMBNAIL
           && mBoostLevel != RKBaseTexture::BOOST_TERRAIN)
        {
            setNoDelete() ;
        }
    }
}

// <FS:minerjr> [FIRE-36016] - Re-added Store/Restore boost levels of selected objects
// Changes the current boost level back to the previous value
void RKBaseTexture::restoreBoostLevel()
{
    mBoostLevel = mPrevBoostLevel;
}

// Stores the current boost level in the previous boost
void RKBaseTexture::storeBoostLevel()
{
    mPrevBoostLevel = mBoostLevel;
}
// </FS:minerjr> [FIRE-36016]

void RKBaseTexture::forceActive()
{
    mTextureState = ACTIVE ;
}

void RKBaseTexture::setActive()
{
    if(mTextureState != NO_DELETE)
    {
        mTextureState = ACTIVE ;
    }
}

//set the texture to stay in memory
void RKBaseTexture::setNoDelete()
{
    mTextureState = NO_DELETE ;
}

void RKBaseTexture::generateImageTexture()
{
    if(mImageTexp.isNull())
    {
        mImageTexp = new RKImageTex(mFullWidth, mFullHeight, mComponents, mUseMipMaps) ;
    }
}

RKImageTex* RKBaseTexture::getImageTexture() const
{
    llassert(mImageTexp.notNull()) ;

    return mImageTexp ;
}

bool RKBaseTexture::createImageTexture()
{
    if(mImageTexp.isNull())
    {
        generateImageTexture() ;
    }

    return mImageTexp->createImageTexture() ;
}

bool RKBaseTexture::createImageTexture(S32 discard_level, const LLImageRaw* imageraw, S32 usename, bool to_create, S32 category, bool defer_copy, U32* tex_name)
{
    llassert(mImageTexp.notNull());

    bool ret = mImageTexp->createImageTexture(discard_level, imageraw, usename, to_create, category, defer_copy, tex_name) ;

    if(ret)
    {
        mFullWidth = mImageTexp->getCurrentWidth() ;
        mFullHeight = mImageTexp->getCurrentHeight() ;
        mComponents = mImageTexp->getComponents() ;
        setTexelsPerImage();
    }

    return ret ;
}

void RKBaseTexture::setExplicitFormat(TexelFormat internal_format, PixelFormat primary_format, ComponentType type_format, const char* file, int line)
{
    llassert(mImageTexp.notNull()) ;

    mImageTexp->setExplicitFormat(internal_format, primary_format, type_format, file, line) ;
}
void RKBaseTexture::setAddressMode(LLTexUnit::eTextureAddressMode mode)
{
    llassert(mImageTexp.notNull()) ;
    mImageTexp->setAddressMode(mode) ;
}
void RKBaseTexture::setFilteringOption(LLTexUnit::eTextureFilterOptions option)
{
    llassert(mImageTexp.notNull()) ;
    mImageTexp->setFilteringOption(option) ;
}

//virtual
S32 RKBaseTexture::getWidth(S32 discard_level) const
{
    llassert(mImageTexp.notNull()) ;
    return mImageTexp->getWidth(discard_level) ;
}

//virtual
S32 RKBaseTexture::getHeight(S32 discard_level) const
{
    llassert(mImageTexp.notNull()) ;
    return mImageTexp->getHeight(discard_level) ;
}

S32 RKBaseTexture::getMaxDiscardLevel() const
{
    llassert(mImageTexp.notNull()) ;
    return mImageTexp->getMaxDiscardLevel() ;
}
S32 RKBaseTexture::getDiscardLevel() const
{
    llassert(mImageTexp.notNull()) ;
    return mImageTexp->getDiscardLevel() ;
}
S8  RKBaseTexture::getComponents() const
{
    llassert(mImageTexp.notNull()) ;

    return mImageTexp->getComponents() ;
}

U32 RKBaseTexture::getTexName() const
{
    llassert(mImageTexp.notNull()) ;

    return mImageTexp->getTexName() ;
}

bool RKBaseTexture::hasImageTexture() const
{
    if(mImageTexp.notNull())
    {
        return mImageTexp->getHasImageTexture() ;
    }
    return false ;
}

bool RKBaseTexture::getBoundRecently() const
{
    if(mImageTexp.notNull())
    {
        return mImageTexp->getBoundRecently() ;
    }
    return false ;
}

LLTexUnit::eTextureType RKBaseTexture::getTarget(void) const
{
    llassert(mImageTexp.notNull()) ;
    return mImageTexp->getTarget() ;
}

bool RKBaseTexture::setSubImage(const LLImageRaw* imageraw, S32 x_pos, S32 y_pos, S32 width, S32 height, U32 use_name)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    llassert(mImageTexp.notNull()) ;

    return mImageTexp->setSubImage(imageraw, x_pos, y_pos, width, height, 0, use_name) ;
}

bool RKBaseTexture::setSubImage(const U8* datap, S32 data_width, S32 data_height, S32 x_pos, S32 y_pos, S32 width, S32 height, U32 use_name)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    llassert(mImageTexp.notNull()) ;

    return mImageTexp->setSubImage(datap, data_width, data_height, x_pos, y_pos, width, height, 0, use_name) ;
}

void RKBaseTexture::setImageTextureCreated (bool initialized)
{
    llassert(mImageTexp.notNull()) ;

    mImageTexp->setImageTextureCreated (initialized) ;
}

void  RKBaseTexture::setCategory(S32 category)
{
    llassert(mImageTexp.notNull()) ;

    mImageTexp->setCategory(category) ;
}

void RKBaseTexture::setTexName(U32 texName)
{
    llassert(mImageTexp.notNull());
    return mImageTexp->setTexName(texName);
}

void RKBaseTexture::setTarget(const TextureTarget target, const LLTexUnit::eTextureType bind_target)
{
    llassert(mImageTexp.notNull());
    return mImageTexp->setTarget(target, bind_target);
}

LLTexUnit::eTextureAddressMode RKBaseTexture::getAddressMode(void) const
{
    llassert(mImageTexp.notNull()) ;

    return mImageTexp->getAddressMode() ;
}

S32Bytes RKBaseTexture::getTextureMemory() const
{
    llassert(mImageTexp.notNull()) ;

    return mImageTexp->mTextureMemory ;
}

PixelFormat RKBaseTexture::getPrimaryFormat() const
{
    llassert(mImageTexp.notNull()) ;

    return mImageTexp->getPrimaryFormat() ;
}

bool RKBaseTexture::getIsAlphaMask() const
{
    llassert(mImageTexp.notNull()) ;

    return mImageTexp->getIsAlphaMask() ;
}

//bool RKBaseTexture::getMask(const LLVector2 &tc)
// [RLVa:KB] - Checked: RLVa-2.2 (@setoverlay)
bool RKBaseTexture::getMask(const LLVector2 &tc) const
// [/RLVa:KB]
{
    llassert(mImageTexp.notNull()) ;

    return mImageTexp->getMask(tc) ;
}

F32 RKBaseTexture::getTimePassedSinceLastBound()
{
    llassert(mImageTexp.notNull()) ;

    return mImageTexp->getTimePassedSinceLastBound() ;
}
bool RKBaseTexture::getMissed() const
{
    llassert(mImageTexp.notNull()) ;

    return mImageTexp->getMissed() ;
}

bool RKBaseTexture::isJustBound() const
{
    llassert(mImageTexp.notNull()) ;

    return mImageTexp->isJustBound() ;
}

void RKBaseTexture::forceUpdateBindStats(void) const
{
    llassert(mImageTexp.notNull()) ;

    return mImageTexp->forceUpdateBindStats() ;
}

bool RKBaseTexture::isImageTextureCreated() const
{
    llassert(mImageTexp.notNull()) ;

    return mImageTexp->isImageTextureCreated() ;
}

void RKBaseTexture::destroyImageTexture()
{
    if(mImageTexp.notNull() && mImageTexp->getHasImageTexture())
    {
        mImageTexp->destroyImageTexture() ;
        mTextureState = DELETED ;
    }
}

void RKBaseTexture::setTexelsPerImage()
{
    U32 fullwidth = llmin(mFullWidth,U32(MAX_IMAGE_SIZE_DEFAULT));
    U32 fullheight = llmin(mFullHeight,U32(MAX_IMAGE_SIZE_DEFAULT));
    mTexelsPerImage = (U32)fullwidth * fullheight;
}

static LLUUID sStubUUID;

const LLUUID& RKBaseTexture::getID() const { return sStubUUID; }
