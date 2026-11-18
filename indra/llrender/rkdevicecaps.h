/**
 * @file rkdevicecaps.h
 * @brief LLGL definition
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

#ifndef LL_LLGL_H
#define LL_LLGL_H

// This file contains various stuff for handling gl extensions and other gl related stuff.

#include <functional>
#include <string>
#include <unordered_map>
#include <list>

#include "llerror.h"
#include "v4color.h"
#include "llstring.h"
#include "stdtypes.h"
#include "v4math.h"
#include "llplane.h"
#include "rkrenderstate.h"
#include "llgltypes.h"
#include "llinstancetracker.h"

#include "rkrenderheaders.h"
#include "glm/mat4x4.hpp"

extern bool gDebugRender;
extern bool gDebugSession;
extern bool gDebugRenderSession;
extern llofstream gFailLog;

#define LL_GL_ERRS LL_ERRS("RenderState")

void ll_init_fail_log(std::string filename);

void ll_fail(std::string msg);

void ll_close_fail_log();

class LLSD;

// Manage GL extensions...
class RKDeviceCaps
{
public:
    RKDeviceCaps();

    bool initRenderDevice();
    bool initFromDevice();     // <FS> RK_VK_NOGL: everything comes from the Vulkan device
    void shutdownRenderDevice();

#if LL_WINDOWS
#endif

    std::string getRawDeviceString(); // For sending to simulator

    bool mInited;
    bool mIsDisabled;

    // OpenGL limits
    S32 mMaxSamples;
    S32 mNumTextureImageUnits;
    S32 mMaxSampleMaskWords;
    S32 mMaxColorTextureSamples;
    S32 mMaxDepthTextureSamples;
    S32 mMaxIntegerSamples;
    S32 mVertexRangeLimit;
    S32 mIndexRangeLimit;
    S32 mTextureSizeLimit;
    F32 mMaxAnisotropy = 0.f;
    S32 mMaxUniformBlockSize = 0;
    S32 mMaxVaryingVectors = 0;

    // C6: measured by the Vulkan device at start-up (GB/s, device-local copy read + write; 0 = not measured), and what it can do for ray tracing
    F32 mMemoryBandwidthGBps = 0.f;
    bool mHasAccelStructure = false;
    bool mHasRayQuery = false;
    bool mHasRayTracingPipeline = false;
    bool mHasRayTracing = false;            // acceleration structures + ray query + device addresses: the ray tracing steps can be built on this device
    bool mRayTracingEnabled = false;        // ... and switched on at device creation (RK_VK_RT=1)
    bool mHasNeuralRender = false;          // <FS> DLSS-NR (neural rendering) can run here: DLSS on, the NGX core says yes, nvngx_dlssnr.dll next to the viewer

    // GL 4.x capabilities
    bool mHasCubeMapArray = false;
    bool mHasDebugOutput = false;
    bool mHasTransformFeedback = false;
    bool mHasAnisotropic = false;

    // Vendor-specific extensions
    bool mHasAMDAssociations = false;
    bool mHasNVXGpuMemoryInfo = false;
    bool mHasATIMemInfo = false;

    bool mIsAMD;
    bool mIsNVIDIA;
    bool mIsIntel;
    bool mIsApple = false;

    // hints to the render pipe
    U32 mDownScaleMethod = 0; // see settings.xml RenderDownScaleMethod

#if LL_DARWIN
    // Needed to distinguish problem cards on older Macs that break with Materials
    bool mIsMobileGF;
#endif

    // Whether this version of GL is good enough for SL to use
    bool mHasRequirements;

    S32 mDriverVersionMajor;
    S32 mDriverVersionMinor;
    S32 mDriverVersionRelease;
    F32 mApiVersionNumber; // e.g = 1.4
    S32 mShaderLangMajor;
    S32 mShaderLangMinor;
    std::string mDriverVersionVendorString;
    std::string mApiVersionString;

    U32 mVRAM; // VRAM in MB
    S32 mVRAMDetected; // <FS:Beq/> The amount detected/reported by the OS/Drivers. If different to mVRAM there is an override in place.
    std::string getGLInfoString();
    void printDeviceInfoString();
    void getGLInfo(LLSD& info);

    void asLLSD(LLSD& info);

    // In ALL CAPS
    std::string mVendorString;
    std::string mVendorShort;

    // In ALL CAPS
    std::string mDeviceString;

private:
};

extern RKDeviceCaps gDeviceCaps;

class LLQuaternion;
class LLMatrix4;

void rotate_quat(LLQuaternion& rotation);

////////////////////////
//
// Note: U32's are GLEnum's...
//

// Render state (blend, cull, depth, scissor, stencil, polygon offset) is set with the typed guards of rkrenderstate.h; the old LLGLState / LLGLEnable / LLGLDisable /
// LLGLDepthTest classes are gone (F3.2e).

/*
  Store and modify projection matrix to create an oblique
  projection that clips to the specified plane.  Oblique
  projections alter values in the depth buffer, so this
  class should not be used mid-renderpass.

  Restores projection matrix on destruction.
  GL_MODELVIEW_MATRIX is active whenever program execution
  leaves this class.
  Does not stack.
  Caches inverse of projection matrix used in gRenderObliqueProjectionInverse
*/
class RKRenderUserClipPlane
{
public:

    RKRenderUserClipPlane(const LLPlane& plane, const glm::mat4& modelview, const glm::mat4& projection, bool apply = true);
    ~RKRenderUserClipPlane();

    void setPlane(F32 a, F32 b, F32 c, F32 d);
    void disable();

private:
    bool mApply;

    glm::mat4 mProjection;
    glm::mat4 mModelview;
};

/*
  Modify and load projection matrix to push depth values to far clip plane.

  Restores projection matrix on destruction.
  Saves/restores matrix mode around projection manipulation.
  Does not stack.
*/
class RKRenderSquashToFarClip
{
public:
    RKRenderSquashToFarClip();
    RKRenderSquashToFarClip(const glm::mat4& projection, U32 layer = 0);

    void setProjectionMatrix(glm::mat4 projection, U32 layer);

    ~RKRenderSquashToFarClip();
};

/*
    Interface for objects that need periodic GL updates applied to them.
    Used to synchronize GL updates with GL thread.
*/
class RKRenderUpdate
{
public:

    static std::list<RKRenderUpdate*> sUpdateQueue;

    bool mInQ;
    RKRenderUpdate()
        : mInQ(false)
    {
    }
    virtual ~RKRenderUpdate()
    {
        if (mInQ)
        {
            std::list<RKRenderUpdate*>::iterator iter = std::find(sUpdateQueue.begin(), sUpdateQueue.end(), this);
            if (iter != sUpdateQueue.end())
            {
                sUpdateQueue.erase(iter);
            }
        }
    }
    virtual void updateResources() = 0;
};

extern LLMatrix4 gRenderObliqueProjectionInverse;

#include "rkrenderpresets.h"

void init_glstates();

void parse_gl_version( S32* major, S32* minor, S32* release, std::string* vendor_specific, std::string* version_string );

extern bool gClothRipple;
extern bool gHeadlessClient;
extern bool gNonInteractive;
extern bool gRenderActive;

#endif // LL_LLGL_H
