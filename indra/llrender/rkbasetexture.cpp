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


LLBaseTexture::LLBaseTexture(bool usemipmaps)
{
    init();
    mUseMipMaps = usemipmaps;
}

LLBaseTexture::LLBaseTexture(const U32 width, const U32 height, const U8 components, bool usemipmaps)
{
    init();
    mFullWidth = width ;
    mFullHeight = height ;
    mUseMipMaps = usemipmaps;
    mComponents = components ;
    setTexelsPerImage();
}

LLBaseTexture::LLBaseTexture(const LLImageRaw* raw, bool usemipmaps)
{
    init();
    mUseMipMaps = usemipmaps ;
    // Create an empty image of the specified size and width
    mImageTexp = new LLImageTex(raw, usemipmaps) ;
    mFullWidth = mImageTexp->getWidth();
    mFullHeight = mImageTexp->getHeight();
    mComponents = mImageTexp->getComponents();
    setTexelsPerImage();
}

LLBaseTexture::~LLBaseTexture()
{
    cleanup();
}

void LLBaseTexture::init()
{
    mBoostLevel = LLBaseTexture::BOOST_NONE;
    // <FS:minerjr> [FIRE-36016] - Re-added Store/Restore boost levels of selected objects
    // Added a previous boost level to allow for restoring boost after BOOST_SELECTED is applied
    mPrevBoostLevel = LLBaseTexture::BOOST_NONE;
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

void LLBaseTexture::cleanup()
{
    if(mImageTexp)
    {
        mImageTexp->cleanup();
    }
}

// virtual
void LLBaseTexture::dump()
{
    if(mImageTexp)
    {
        mImageTexp->dump();
    }
}

void LLBaseTexture::setBoostLevel(S32 level)
{
    if(mBoostLevel != level)
    {
        mBoostLevel = level ;
        if(mBoostLevel != LLBaseTexture::BOOST_NONE
           && mBoostLevel != LLBaseTexture::BOOST_ICON
           && mBoostLevel != LLBaseTexture::BOOST_THUMBNAIL
           && mBoostLevel != LLBaseTexture::BOOST_TERRAIN)
        {
            setNoDelete() ;
        }
    }
}

// <FS:minerjr> [FIRE-36016] - Re-added Store/Restore boost levels of selected objects
// Changes the current boost level back to the previous value
void LLBaseTexture::restoreBoostLevel()
{
    mBoostLevel = mPrevBoostLevel;
}

// Stores the current boost level in the previous boost
void LLBaseTexture::storeBoostLevel()
{
    mPrevBoostLevel = mBoostLevel;
}
// </FS:minerjr> [FIRE-36016]

void LLBaseTexture::forceActive()
{
    mTextureState = ACTIVE ;
}

void LLBaseTexture::setActive()
{
    if(mTextureState != NO_DELETE)
    {
        mTextureState = ACTIVE ;
    }
}

//set the texture to stay in memory
void LLBaseTexture::setNoDelete()
{
    mTextureState = NO_DELETE ;
}

void LLBaseTexture::generateImageTexture()
{
    if(mImageTexp.isNull())
    {
        mImageTexp = new LLImageTex(mFullWidth, mFullHeight, mComponents, mUseMipMaps) ;
    }
}

LLImageTex* LLBaseTexture::getImageTexture() const
{
    llassert(mImageTexp.notNull()) ;

    return mImageTexp ;
}

bool LLBaseTexture::createImageTexture()
{
    if(mImageTexp.isNull())
    {
        generateImageTexture() ;
    }

    return mImageTexp->createImageTexture() ;
}

bool LLBaseTexture::createImageTexture(S32 discard_level, const LLImageRaw* imageraw, S32 usename, bool to_create, S32 category, bool defer_copy, U32* tex_name)
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

void LLBaseTexture::setExplicitFormat(TexelFormat internal_format, PixelFormat primary_format, ComponentType type_format, const char* file, int line)
{
    llassert(mImageTexp.notNull()) ;

    mImageTexp->setExplicitFormat(internal_format, primary_format, type_format, file, line) ;
}
void LLBaseTexture::setAddressMode(LLTexUnit::eTextureAddressMode mode)
{
    llassert(mImageTexp.notNull()) ;
    mImageTexp->setAddressMode(mode) ;
}
void LLBaseTexture::setFilteringOption(LLTexUnit::eTextureFilterOptions option)
{
    llassert(mImageTexp.notNull()) ;
    mImageTexp->setFilteringOption(option) ;
}

//virtual
S32 LLBaseTexture::getWidth(S32 discard_level) const
{
    llassert(mImageTexp.notNull()) ;
    return mImageTexp->getWidth(discard_level) ;
}

//virtual
S32 LLBaseTexture::getHeight(S32 discard_level) const
{
    llassert(mImageTexp.notNull()) ;
    return mImageTexp->getHeight(discard_level) ;
}

S32 LLBaseTexture::getMaxDiscardLevel() const
{
    llassert(mImageTexp.notNull()) ;
    return mImageTexp->getMaxDiscardLevel() ;
}
S32 LLBaseTexture::getDiscardLevel() const
{
    llassert(mImageTexp.notNull()) ;
    return mImageTexp->getDiscardLevel() ;
}
S8  LLBaseTexture::getComponents() const
{
    llassert(mImageTexp.notNull()) ;

    return mImageTexp->getComponents() ;
}

U32 LLBaseTexture::getTexName() const
{
    llassert(mImageTexp.notNull()) ;

    return mImageTexp->getTexName() ;
}

bool LLBaseTexture::hasImageTexture() const
{
    if(mImageTexp.notNull())
    {
        return mImageTexp->getHasImageTexture() ;
    }
    return false ;
}

bool LLBaseTexture::getBoundRecently() const
{
    if(mImageTexp.notNull())
    {
        return mImageTexp->getBoundRecently() ;
    }
    return false ;
}

LLTexUnit::eTextureType LLBaseTexture::getTarget(void) const
{
    llassert(mImageTexp.notNull()) ;
    return mImageTexp->getTarget() ;
}

bool LLBaseTexture::setSubImage(const LLImageRaw* imageraw, S32 x_pos, S32 y_pos, S32 width, S32 height, U32 use_name)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    llassert(mImageTexp.notNull()) ;

    return mImageTexp->setSubImage(imageraw, x_pos, y_pos, width, height, 0, use_name) ;
}

bool LLBaseTexture::setSubImage(const U8* datap, S32 data_width, S32 data_height, S32 x_pos, S32 y_pos, S32 width, S32 height, U32 use_name)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    llassert(mImageTexp.notNull()) ;

    return mImageTexp->setSubImage(datap, data_width, data_height, x_pos, y_pos, width, height, 0, use_name) ;
}

void LLBaseTexture::setImageTextureCreated (bool initialized)
{
    llassert(mImageTexp.notNull()) ;

    mImageTexp->setImageTextureCreated (initialized) ;
}

void  LLBaseTexture::setCategory(S32 category)
{
    llassert(mImageTexp.notNull()) ;

    mImageTexp->setCategory(category) ;
}

void LLBaseTexture::setTexName(U32 texName)
{
    llassert(mImageTexp.notNull());
    return mImageTexp->setTexName(texName);
}

void LLBaseTexture::setTarget(const TextureTarget target, const LLTexUnit::eTextureType bind_target)
{
    llassert(mImageTexp.notNull());
    return mImageTexp->setTarget(target, bind_target);
}

LLTexUnit::eTextureAddressMode LLBaseTexture::getAddressMode(void) const
{
    llassert(mImageTexp.notNull()) ;

    return mImageTexp->getAddressMode() ;
}

S32Bytes LLBaseTexture::getTextureMemory() const
{
    llassert(mImageTexp.notNull()) ;

    return mImageTexp->mTextureMemory ;
}

PixelFormat LLBaseTexture::getPrimaryFormat() const
{
    llassert(mImageTexp.notNull()) ;

    return mImageTexp->getPrimaryFormat() ;
}

bool LLBaseTexture::getIsAlphaMask() const
{
    llassert(mImageTexp.notNull()) ;

    return mImageTexp->getIsAlphaMask() ;
}

//bool LLBaseTexture::getMask(const LLVector2 &tc)
// [RLVa:KB] - Checked: RLVa-2.2 (@setoverlay)
bool LLBaseTexture::getMask(const LLVector2 &tc) const
// [/RLVa:KB]
{
    llassert(mImageTexp.notNull()) ;

    return mImageTexp->getMask(tc) ;
}

F32 LLBaseTexture::getTimePassedSinceLastBound()
{
    llassert(mImageTexp.notNull()) ;

    return mImageTexp->getTimePassedSinceLastBound() ;
}
bool LLBaseTexture::getMissed() const
{
    llassert(mImageTexp.notNull()) ;

    return mImageTexp->getMissed() ;
}

bool LLBaseTexture::isJustBound() const
{
    llassert(mImageTexp.notNull()) ;

    return mImageTexp->isJustBound() ;
}

void LLBaseTexture::forceUpdateBindStats(void) const
{
    llassert(mImageTexp.notNull()) ;

    return mImageTexp->forceUpdateBindStats() ;
}

bool LLBaseTexture::isImageTextureCreated() const
{
    llassert(mImageTexp.notNull()) ;

    return mImageTexp->isImageTextureCreated() ;
}

void LLBaseTexture::destroyImageTexture()
{
    if(mImageTexp.notNull() && mImageTexp->getHasImageTexture())
    {
        mImageTexp->destroyImageTexture() ;
        mTextureState = DELETED ;
    }
}

void LLBaseTexture::setTexelsPerImage()
{
    U32 fullwidth = llmin(mFullWidth,U32(MAX_IMAGE_SIZE_DEFAULT));
    U32 fullheight = llmin(mFullHeight,U32(MAX_IMAGE_SIZE_DEFAULT));
    mTexelsPerImage = (U32)fullwidth * fullheight;
}

static LLUUID sStubUUID;

const LLUUID& LLBaseTexture::getID() const { return sStubUUID; }
