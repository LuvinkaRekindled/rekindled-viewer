/**
 * @file llcubemaparray.cpp
 * @brief LLCubeMap class implementation
 *
 * $LicenseInfo:firstyear=2022&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2022, Linden Research, Inc.
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

#include "llworkerthread.h"

#include "llcubemaparray.h"

#include "v4coloru.h"
#include "v3math.h"
#include "v3dmath.h"
#include "m3math.h"
#include "m4math.h"

#include "llrender.h"
#include "llshaderprogram.h"

#include "llrenderheaders.h"
#include "llvktex.h"

//#pragma optimize("", off)

using namespace LLImageTexMemory;

// MUST match order of OpenGL face-layers
TextureTarget LLCubeMapArray::sTargets[6] =
{
    TextureTarget::CubeFacePX,
    TextureTarget::CubeFaceNX,
    TextureTarget::CubeFacePY,
    TextureTarget::CubeFaceNY,
    TextureTarget::CubeFacePZ,
    TextureTarget::CubeFaceNZ
};

LLVector3 LLCubeMapArray::sLookVecs[6] =
{
        LLVector3(1, 0, 0),
        LLVector3(-1, 0, 0),
        LLVector3(0, 1, 0),
        LLVector3(0, -1, 0),
        LLVector3(0, 0, 1),
        LLVector3(0, 0, -1)
};

LLVector3 LLCubeMapArray::sUpVecs[6] =
{
    LLVector3(0, -1, 0),
    LLVector3(0, -1, 0),
    LLVector3(0, 0, 1),
    LLVector3(0, 0, -1),
    LLVector3(0, -1, 0),
    LLVector3(0, -1, 0)
};

LLVector3 LLCubeMapArray::sClipToCubeLookVecs[6] =
{
        LLVector3(0, 0, -1), //GOOD
        LLVector3(0, 0, 1), //GOOD

        LLVector3(1, 0, 0), // GOOD
        LLVector3(1, 0, 0), // GOOD

        LLVector3(1, 0, 0),
        LLVector3(-1, 0, 0),
};

LLVector3 LLCubeMapArray::sClipToCubeUpVecs[6] =
{
    LLVector3(-1, 0, 0), //GOOD
    LLVector3(1, 0, 0), //GOOD

    LLVector3(0, 1, 0), // GOOD
    LLVector3(0, -1, 0), // GOOD

    LLVector3(0, 0, -1),
    LLVector3(0, 0, 1)
};

LLCubeMapArray::LLCubeMapArray()
    : mTextureStage(0)
{

}

LLCubeMapArray::LLCubeMapArray(LLCubeMapArray& lhs, U32 width, U32 count) : mTextureStage(0)
{
    mWidth = width;
    mCount = count;

    // Allocate a new cubemap array with the same criteria as the incoming cubemap array
    allocate(mWidth, lhs.mImage->getComponents(), count, lhs.mImage->getUseMipMaps(), lhs.mHDR);

    // Copy each cubemap from the incoming array to the new array
    U32 min_count = std::min(count, lhs.mCount);
    // <FS> the image read back below is that of the NEW array (mImage), the same for every layer (nothing in the loop changes what it reads), and a readback makes the main thread wait for the whole worker queue
    // (76 waits, 500 ms in the 10 s of a login, worst 228 ms: LL_VK_SYNC_LOG): it is read back and scaled once, and written to each layer
    if (min_count > 0)
    {
        PixelFormat components = PixelFormat::RGB;
        if (mImage->getComponents() == 4)
            components = PixelFormat::RGBA;
        // S32 format = GL_RGB; // <FS:Beq/> unused

        // <FS> what was read back was the new array's own level 0 (nothing was ever copied from lhs), which holds no data yet: the layers are written black, without the wait for the worker (the probes are
        // rendered again after a resize). A true copy of the old array would be a copy on the GPU (not done).
        LLPointer<LLImageRaw> scaled_image = new LLImageRaw(mWidth, mWidth, mImage->getComponents());
        scaled_image->clear(0, 0, 0, 0);
        for (U32 i = 0; i < min_count * 6; ++i)
        {
            LLVkTex::subImage3D(mImage->getTexName(), TextureTarget::CubeArray, 0, 0, 0, i, mWidth, mWidth, 1, components, ComponentType::U8, scaled_image->getData());
        }
    }
}

LLCubeMapArray::~LLCubeMapArray()
{
}

void LLCubeMapArray::allocate(U32 resolution, U32 components, U32 count, bool use_mips, bool hdr)
{
    U32 texname = 0;
    mWidth = resolution;
    mCount = count;

    mHDR = hdr;

    LLImageTex::generateTextures(1, &texname);

    mImage = new LLImageTex(resolution, resolution, components, use_mips);
    mImage->setTexName(texname);
    mImage->setTarget(sTargets[0], LLTexUnit::TT_CUBE_MAP_ARRAY);

    mImage->setUseMipMaps(use_mips);
    mImage->setHasMipMaps(use_mips);

    bind(0);
    free_cur_tex_image();

    TexelFormat format = components == 4 ? TexelFormat::RGBA16F : TexelFormat::R11G11B10F;
    if (!hdr)
    {
        format = components == 4 ? TexelFormat::RGBA8 : TexelFormat::RGB8;
    }
    U32 mip = 0;
    U32 mip_resolution = resolution;
    while (mip_resolution >= 1)
    {
        LLVkTex::image3D(texname, TextureTarget::CubeArray, mip, format, mip_resolution, mip_resolution, count * 6,
            PixelFormat::RGBA, ComponentType::U8, nullptr);

        if (!use_mips)
        {
            break;
        }
        mip_resolution /= 2;
        ++mip;
    }

    alloc_tex_image(resolution, resolution, format, count * 6);

    mImage->setAddressMode(LLTexUnit::TAM_CLAMP);

    if (use_mips)
    {
        mImage->setFilteringOption(LLTexUnit::TFO_ANISOTROPIC);
        //glGenerateMipmap(GL_TEXTURE_CUBE_MAP_ARRAY);  // <=== latest AMD drivers do not appreciate this method of allocating mipmaps
    }
    else
    {
        mImage->setFilteringOption(LLTexUnit::TFO_BILINEAR);
    }

    unbind();
}

void LLCubeMapArray::bind(S32 stage)
{
    mTextureStage = stage;
    gRender.getTexUnit(stage)->bindManual(LLTexUnit::TT_CUBE_MAP_ARRAY, getTextureHandle(), mImage->getUseMipMaps());
}

void LLCubeMapArray::unbind()
{
    gRender.getTexUnit(mTextureStage)->bindNone(LLTexUnit::TT_CUBE_MAP_ARRAY);
    mTextureStage = -1;
}

U32 LLCubeMapArray::getTextureHandle()
{
    return mImage->getTexName();
}

void LLCubeMapArray::destroyResources()
{
    mImage = NULL;
}
