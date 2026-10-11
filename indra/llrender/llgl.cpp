/**
 * @file llgl.cpp
 * @brief LLGL implementation
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

// This file sets some global GL parameters, and implements some
// useful functions for GL operations.

#define LL_VK_NO_INTERCEPT    // <FS> the GL function pointers are defined and loaded in this file
#include "linden_common.h"
#include "llvkmirror.h"
#include <atomic>

#include "boost/tokenizer.hpp"

#include "llsys.h"

#include "llgl.h"
#include "llglstates.h"
#include "llrender.h"

#include "llerror.h"
#include "llerrorcontrol.h"
#include "llquaternion.h"
#include "llmath.h"
#include "m4math.h"
#include "llstring.h"
#include "llstacktrace.h"

#include "llglheaders.h"
#include "llglslshader.h"

#include "glm/glm.hpp"
#include <glm/gtc/matrix_access.hpp>
#include "glm/gtc/type_ptr.hpp"

#if LL_WINDOWS
#include "lldxhardware.h"
#endif

#if LL_SDL
#include "SDL2/SDL_video.h"

#define GLH_EXT_GET_PROC_ADDRESS SDL_GL_GetProcAddress
#define ExtensionExists(exten, unused) SDL_GL_ExtensionSupported(exten);
#endif

#ifdef _DEBUG
//#define GL_STATE_VERIFY
#endif


bool gDebugSession = false;
bool gDebugGLSession = false;
bool gClothRipple = false;
bool gHeadlessClient = false;
bool gNonInteractive = false;
bool gGLActive = false;

static const std::string HEADLESS_VENDOR_STRING("Linden Lab");
static const std::string HEADLESS_RENDERER_STRING("Headless");
static const std::string HEADLESS_VERSION_STRING("1.0");

llofstream gFailLog;


void parse_glsl_version(S32& major, S32& minor);

void ll_init_fail_log(std::string filename)
{
    gFailLog.open(filename.c_str());
}


void ll_fail(std::string msg)
{

    if (gDebugSession)
    {
        std::vector<std::string> lines;

        gFailLog << LLError::utcTime() << " " << msg << std::endl;

        gFailLog << "Stack Trace:" << std::endl;

        ll_get_stack_trace(lines);

        for(size_t i = 0; i < lines.size(); ++i)
        {
            gFailLog << lines[i] << std::endl;
        }

        gFailLog << "End of Stack Trace." << std::endl << std::endl;

        gFailLog.flush();
    }
};

void ll_close_fail_log()
{
    gFailLog.close();
}

LLMatrix4 gGLObliqueProjectionInverse;

#define LL_GL_NAME_POOLING 0

std::list<LLGLUpdate*> LLGLUpdate::sGLQ;

// <FS:Zi> Linux support
// #if (LL_WINDOWS || LL_LINUX)  && !LL_MESA_HEADLESS

LLGLManager gGLManager;

LLGLManager::LLGLManager() :
    mInited(false),
    mIsDisabled(false),
    mMaxSamples(0),
    mNumTextureImageUnits(1),
    mMaxSampleMaskWords(0),
    mMaxColorTextureSamples(0),
    mMaxDepthTextureSamples(0),
    mMaxIntegerSamples(0),
    mIsAMD(false),
    mIsNVIDIA(false),
    mIsIntel(false),
#if LL_DARWIN
    mIsMobileGF(false),
#endif
    mHasRequirements(true),
    mDriverVersionMajor(1),
    mDriverVersionMinor(0),
    mDriverVersionRelease(0),
    mGLVersion(1.0f),
    mGLSLVersionMajor(0),
    mGLSLVersionMinor(0),
    mVRAM(0),
    mVRAMDetected(0), // <FS:Beq/> add override support
    mGLMaxVertexRange(0),
    mGLMaxIndexRange(0)
{
}

// <FS> there is no OpenGL context at all. The strings the viewer reads with glGetString are the ones made from the Vulkan device in initGLNoGL.

static std::string sNoGLVendor, sNoGLRenderer, sNoGLVersion, sNoGLGlsl, sNoGLExtensions;

const GLubyte* ll_nogl_string(GLenum name)
{
    switch (name)
    {
    case GL_VENDOR: return (const GLubyte*)sNoGLVendor.c_str();
    case GL_RENDERER: return (const GLubyte*)sNoGLRenderer.c_str();
    case GL_VERSION: return (const GLubyte*)sNoGLVersion.c_str();
    case GL_SHADING_LANGUAGE_VERSION: return (const GLubyte*)sNoGLGlsl.c_str();
    case GL_EXTENSIONS: return (const GLubyte*)sNoGLExtensions.c_str();
    default: return (const GLubyte*)"";
    }
}

// glGetString where this file talks to GL directly (the part before the mirror's wrappers apply)
static const char* gl_info_string(GLenum name)
{
    return (const char*)ll_nogl_string(name);
}

//---------------------------------------------------------------------
// Global initialization for GL
//---------------------------------------------------------------------
// return false if unable (or unwilling due to old drivers) to init GL
bool LLGLManager::initGL()
{
    LL_INFOS("RenderInit") << "Initializing the graphics device (Vulkan, no OpenGL)" << LL_ENDL;
    if (mInited)
    {
        LL_ERRS("RenderInit") << "Calling init on LLGLManager after already initialized!" << LL_ENDL;
    }
    return initGLNoGL();
}

void LLGLManager::getGLInfo(LLSD& info)
{
    if (gHeadlessClient)
    {
        info["GLInfo"]["GLVendor"] = HEADLESS_VENDOR_STRING;
        info["GLInfo"]["GLRenderer"] = HEADLESS_RENDERER_STRING;
        info["GLInfo"]["GLVersion"] = HEADLESS_VERSION_STRING;
        return;
    }
    else
    {
        info["GLInfo"]["GLVendor"] = ll_safe_string(gl_info_string(GL_VENDOR));
        info["GLInfo"]["GLRenderer"] = ll_safe_string(gl_info_string(GL_RENDERER));
        info["GLInfo"]["GLVersion"] = ll_safe_string(gl_info_string(GL_VERSION));
    }

#if !LL_MESA_HEADLESS
    std::string all_exts = sNoGLExtensions;
    boost::char_separator<char> sep(" ");
    boost::tokenizer<boost::char_separator<char> > tok(all_exts, sep);
    for(boost::tokenizer<boost::char_separator<char> >::iterator i = tok.begin(); i != tok.end(); ++i)
    {
        info["GLInfo"]["GLExtensions"].append(*i);
    }
#endif
}

std::string LLGLManager::getGLInfoString()
{
    std::string info_str;

    if (gHeadlessClient)
    {
        info_str += std::string("GL_VENDOR      ") + HEADLESS_VENDOR_STRING + std::string("\n");
        info_str += std::string("GL_RENDERER    ") + HEADLESS_RENDERER_STRING + std::string("\n");
        info_str += std::string("GL_VERSION     ") + HEADLESS_VERSION_STRING + std::string("\n");
    }
    else
    {
        info_str += std::string("GL_VENDOR      ") + ll_safe_string(gl_info_string(GL_VENDOR)) + std::string("\n");
        info_str += std::string("GL_RENDERER    ") + ll_safe_string(gl_info_string(GL_RENDERER)) + std::string("\n");
        info_str += std::string("GL_VERSION     ") + ll_safe_string(gl_info_string(GL_VERSION)) + std::string("\n");
    }

#if !LL_MESA_HEADLESS
    std::string all_exts= sNoGLExtensions;
    LLStringUtil::replaceChar(all_exts, ' ', '\n');
    info_str += std::string("GL_EXTENSIONS:\n") + all_exts + std::string("\n");
#endif

    return info_str;
}

void LLGLManager::printGLInfoString()
{
    if (gHeadlessClient)
    {
        LL_INFOS("RenderInit") << "GL_VENDOR:     " << HEADLESS_VENDOR_STRING << LL_ENDL;
        LL_INFOS("RenderInit") << "GL_RENDERER:   " << HEADLESS_RENDERER_STRING << LL_ENDL;
        LL_INFOS("RenderInit") << "GL_VERSION:    " << HEADLESS_VERSION_STRING << LL_ENDL;
    }
    else
    {
        LL_INFOS("RenderInit") << "GL_VENDOR:     " << ll_safe_string(gl_info_string(GL_VENDOR)) << LL_ENDL;
        LL_INFOS("RenderInit") << "GL_RENDERER:   " << ll_safe_string(gl_info_string(GL_RENDERER)) << LL_ENDL;
        LL_INFOS("RenderInit") << "GL_VERSION:    " << ll_safe_string(gl_info_string(GL_VERSION)) << LL_ENDL;
    }

#if !LL_MESA_HEADLESS
    std::string all_exts= sNoGLExtensions;
    LLStringUtil::replaceChar(all_exts, ' ', '\n');
    LL_DEBUGS("RenderInit") << "GL_EXTENSIONS:\n" << all_exts << LL_ENDL;
#endif
}

std::string LLGLManager::getRawGLString()
{
    std::string gl_string;
    if (gHeadlessClient)
    {
        gl_string = HEADLESS_VENDOR_STRING + " " + HEADLESS_RENDERER_STRING;
    }
    else
    {
        gl_string = ll_safe_string(gl_info_string(GL_VENDOR)) + " " + ll_safe_string(gl_info_string(GL_RENDERER));
    }
    return gl_string;
}

void LLGLManager::asLLSD(LLSD& info)
{
    // Currently these are duplicates of fields in "system".
    info["gpu_vendor"] = mGLVendorShort;
    info["gpu_version"] = mDriverVersionVendorString;
    info["opengl_version"] = mGLVersionString;

    info["vram"] = LLSD::Integer(mVRAM);

    // OpenGL limits
    info["max_samples"] = mMaxSamples;
    info["num_texture_image_units"] =  mNumTextureImageUnits;
    info["max_sample_mask_words"] = mMaxSampleMaskWords;
    info["max_color_texture_samples"] = mMaxColorTextureSamples;
    info["max_depth_texture_samples"] = mMaxDepthTextureSamples;
    info["max_integer_samples"] = mMaxIntegerSamples;
    info["max_vertex_range"] = mGLMaxVertexRange;
    info["max_index_range"] = mGLMaxIndexRange;
    info["max_texture_size"] = mGLMaxTextureSize;

    // Which vendor
    info["is_ati"] = mIsAMD;  // note, do not rename is_ati to is_amd without coordinating with DW
    info["is_nvidia"] = mIsNVIDIA;
    info["is_intel"] = mIsIntel;

    info["gl_renderer"] = mGLRenderer;
}

void LLGLManager::shutdownGL()
{
    if (mInited)
    {
        mInited = false;
    }
}

// <FS> from here on the GL calls in this file are mirrored to Vulkan (see llvkgl.h)
#if defined(LL_VULKAN)
#undef LL_VK_NO_INTERCEPT
#include "llvkgl.h"
#include "llvkstate.h"
#endif

// <FS> LL_VK_NOGL start-up: what initGL and initExtensions work out from a GL context, worked out from the Vulkan device instead
bool LLGLManager::initGLNoGL()
{
    LL_INFOS("RenderInit") << "Initializing without OpenGL (LL_VK_NOGL): the renderer is Vulkan only" << LL_ENDL;

    LLVkMirror::DeviceInfo dev;
    if (!LLVkMirror::describeDevice(dev))
    {
        LL_WARNS("RenderInit") << "Vulkan could not be started: there is no OpenGL to fall back on" << LL_ENDL;
        return false;
    }
    const std::string& vendor = dev.mVendor;
    const std::string& renderer = dev.mRenderer;
    sNoGLVendor = vendor;
    sNoGLRenderer = renderer;
    sNoGLVersion = dev.mVersion;
    sNoGLGlsl = "4.60";
    sNoGLExtensions = "GL_EXT_texture_filter_anisotropic GL_ARB_texture_cube_map_array GL_ARB_buffer_storage GL_ARB_multi_draw_indirect";

    mGLVendor = vendor;
    LLStringUtil::toUpper(mGLVendor);
    mGLRenderer = renderer;
    LLStringUtil::toUpper(mGLRenderer);

    parse_gl_version(&mDriverVersionMajor, &mDriverVersionMinor, &mDriverVersionRelease, &mDriverVersionVendorString, &mGLVersionString);
    mGLVersion = mDriverVersionMajor + mDriverVersionMinor * .1f;
    parse_glsl_version(mGLSLVersionMajor, mGLSLVersionMinor);

    if (mGLVendor.find("AMD") != std::string::npos || mGLVendor.substr(0, 4) == "ATI ")
    {
        mGLVendorShort = "AMD";
        mIsAMD = true;
    }
    else if (mGLVendor.find("NVIDIA") != std::string::npos)
    {
        mGLVendorShort = "NVIDIA";
        mIsNVIDIA = true;
    }
    else if (mGLVendor.find("INTEL") != std::string::npos)
    {
        mGLVendorShort = "INTEL";
        mIsIntel = true;
    }
    else
    {
        mGLVendorShort = "MISC";
    }

    // what initExtensions sets from the version and the extension list
    mHasCubeMapArray = true;
    mHasTransformFeedback = false;
    mHasDebugOutput = false;
    mHasAnisotropic = true;
    mHasNVXGpuMemoryInfo = false;
    mHasATIMemInfo = false;
    mHasAMDAssociations = false;
    // the limits come straight from the device (C6); nothing here asks a GL context any more
    mGLMaxVertexRange = dev.mMaxVertexRange;
    mGLMaxIndexRange = dev.mMaxIndexRange;
    mGLMaxTextureSize = dev.mMaxTextureSize > 0 ? dev.mMaxTextureSize : 16384;
    mVRAM = (S32)dev.mVramMb;
    mInited = true;

    mNumTextureImageUnits = dev.mTextureImageUnits > 0 ? dev.mTextureImageUnits : 32;
    mMaxColorTextureSamples = dev.mMaxColorTextureSamples;
    mMaxDepthTextureSamples = dev.mMaxDepthTextureSamples;
    mMaxIntegerSamples = dev.mMaxIntegerSamples;
    mMaxSampleMaskWords = dev.mMaxSampleMaskWords;
    mMaxSamples = dev.mMaxSamples;
    mMaxVaryingVectors = dev.mMaxVaryingVectors > 0 ? dev.mMaxVaryingVectors : 31;
    mMaxUniformBlockSize = llclamp(dev.mMaxUniformBlockSize, 16384, 65536);
    mMaxAnisotropy = dev.mMaxAnisotropy >= 1.f ? dev.mMaxAnisotropy : 16.f;

    mMemoryBandwidthGBps = dev.mMemoryBandwidthGBps;
    mHasAccelStructure = dev.mAccelStruct;
    mHasRayQuery = dev.mRayQuery;
    mHasRayTracingPipeline = dev.mRayTracingPipeline;
    mHasRayTracing = dev.mRayTracingUsable;
    mRayTracingEnabled = dev.mRayTracingEnabled;
    mHasRayReconstruction = dev.mRayReconstruction;

    LL_INFOS("RenderInit") << "GL_VENDOR:     " << sNoGLVendor << LL_ENDL;
    LL_INFOS("RenderInit") << "GL_RENDERER:   " << sNoGLRenderer << LL_ENDL;
    LL_INFOS("RenderInit") << "GL_VERSION:    " << sNoGLVersion << " (reported to the viewer)" << LL_ENDL;
    LL_INFOS("RenderInit") << "VRAM:          " << mVRAM << " MB" << LL_ENDL;
    LL_INFOS("RenderInit") << "limits:        texture " << mGLMaxTextureSize << ", units " << mNumTextureImageUnits << ", samples " << mMaxSamples << " (color texture " << mMaxColorTextureSamples
        << ", depth texture " << mMaxDepthTextureSamples << ", integer " << mMaxIntegerSamples << "), sample mask words " << mMaxSampleMaskWords << ", varying vectors " << mMaxVaryingVectors
        << ", uniform block " << mMaxUniformBlockSize << ", draw range vertices " << mGLMaxVertexRange << " indices " << mGLMaxIndexRange << ", anisotropy " << mMaxAnisotropy << LL_ENDL;
    LL_INFOS("RenderInit") << "capabilities:  memory bandwidth " << mMemoryBandwidthGBps << " GB/s, ray tracing " << (mHasRayTracing ? "yes" : "no") << " (acceleration structure " << mHasAccelStructure
        << ", ray query " << mHasRayQuery << ", pipeline " << mHasRayTracingPipeline << ", enabled " << mRayTracingEnabled << "), DLSS ray reconstruction " << mHasRayReconstruction << LL_ENDL;

    initGLStates();
    return true;
}

void rotate_quat(LLQuaternion& rotation)
{
    F32 angle_radians, x, y, z;
    rotation.getAngleAxis(&angle_radians, &x, &y, &z);
    gGL.rotatef(angle_radians * RAD_TO_DEG, x, y, z);
}

void flush_glerror()
{
    glGetError();
}

//this function outputs gl error to the log file, does not crash the code.
void log_glerror()
{
    if (LL_UNLIKELY(!gGLManager.mInited))
    {
        return ;
    }
    //  Create or update texture to be used with this data
    GLenum error;
    error = glGetError();
    while (LL_UNLIKELY(error))
    {
        GLubyte const * gl_error_msg = nullptr;      // no glu: the error codes below are logged as numbers
        if (NULL != gl_error_msg)
        {
            LL_WARNS() << "GL Error: " << error << " GL Error String: " << gl_error_msg << LL_ENDL ;
        }
        else
        {
            // gluErrorString returns NULL for some extensions' error codes.
            // you'll probably have to grep for the number in glext.h.
            LL_WARNS() << "GL Error: UNKNOWN 0x" << std::hex << error << std::dec << LL_ENDL;
        }
        error = glGetError();
    }
}

void do_assert_glerror()
{
    //  Create or update texture to be used with this data
    GLenum error;
    error = glGetError();
    bool quit = false;
    if (LL_UNLIKELY(error))
    {
        quit = true;
        GLubyte const * gl_error_msg = nullptr;      // no glu: the error codes below are logged as numbers
        if (NULL != gl_error_msg)
        {
            LL_WARNS("RenderState") << "GL Error:" << error<< LL_ENDL;
            LL_WARNS("RenderState") << "GL Error String:" << gl_error_msg << LL_ENDL;

            if (gDebugSession)
            {
                gFailLog << "GL Error:" << gl_error_msg << std::endl;
            }
        }
        else
        {
            // gluErrorString returns NULL for some extensions' error codes.
            // you'll probably have to grep for the number in glext.h.
            LL_WARNS("RenderState") << "GL Error: UNKNOWN 0x" << std::hex << error << std::dec << LL_ENDL;

            if (gDebugSession)
            {
                gFailLog << "GL Error: UNKNOWN 0x" << std::hex << error << std::dec << std::endl;
            }
        }
    }

    if (quit)
    {
        if (gDebugSession)
        {
            ll_fail("assert_glerror failed");
        }
        else
        {
            LL_ERRS() << "One or more unhandled GL errors." << LL_ENDL;
        }
    }
}

void assert_glerror()
{
/*  if (!gGLActive)
    {
        //LL_WARNS() << "GL used while not active!" << LL_ENDL;

        if (gDebugSession)
        {
            //ll_fail("GL used while not active");
        }
    }
*/

    if (!gDebugGL)
    {
        //funny looking if for branch prediction -- gDebugGL is almost always false and assert_glerror is called often
    }
    else
    {
        do_assert_glerror();
    }
}


void clear_glerror()
{
    glGetError();
    glGetError();
}

///////////////////////////////////////////////////////////////
//
// LLGLState
//

// Static members
std::unordered_map<LLGLenum, LLGLboolean> LLGLState::sStateMap;

GLboolean LLGLDepthTest::sDepthEnabled = GL_FALSE; // OpenGL default
GLenum LLGLDepthTest::sDepthFunc = GL_LESS; // OpenGL default
GLboolean LLGLDepthTest::sWriteEnabled = GL_TRUE; // OpenGL default

//static
void LLGLState::initClass()
{
    sStateMap[GL_DITHER] = GL_TRUE;
    // sStateMap[GL_TEXTURE_2D] = GL_TRUE;

    //make sure multisample defaults to disabled
    sStateMap[GL_MULTISAMPLE] = GL_FALSE;
}

//static
void LLGLState::restoreGL()
{
    sStateMap.clear();
    initClass();
}

//static
// Really shouldn't be needed, but seems we sometimes do.
void LLGLState::resetTextureStates()
{
    gGL.flush();
    GLint maxTextureUnits;

    glGetIntegerv(GL_MAX_TEXTURE_UNITS_ARB, &maxTextureUnits);
    for (S32 j = maxTextureUnits-1; j >=0; j--)
    {
        gGL.getTexUnit(j)->activate();
        glClientActiveTexture(GL_TEXTURE0+j);
        j == 0 ? gGL.getTexUnit(j)->enable(LLTexUnit::TT_TEXTURE) : gGL.getTexUnit(j)->disable();
    }
}

void LLGLState::dumpStates()
{
    LL_INFOS("RenderState") << "GL States:" << LL_ENDL;
    for (std::unordered_map<LLGLenum, LLGLboolean>::iterator iter = sStateMap.begin();
         iter != sStateMap.end(); ++iter)
    {
        LL_INFOS("RenderState") << llformat(" 0x%04x : %s",(S32)iter->first,iter->second?"true":"false") << LL_ENDL;
    }
}

void LLGLState::checkStates(GLboolean writeAlpha)
{
    if (!gDebugGL)
    {
        return;
    }

    GLint srcRGB, dstRGB, srcAlpha, dstAlpha;
    glGetIntegerv(GL_BLEND_SRC_RGB, &srcRGB);
    glGetIntegerv(GL_BLEND_DST_RGB, &dstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &srcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &dstAlpha);
    llassert_always(srcRGB == GL_SRC_ALPHA);
    llassert_always(srcAlpha == GL_SRC_ALPHA);
    llassert_always(dstRGB == GL_ONE_MINUS_SRC_ALPHA);
    llassert_always(dstAlpha == GL_ONE_MINUS_SRC_ALPHA);

    // disable for now until usage is consistent
    //GLboolean colorMask[4];
    //glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
    //llassert_always(colorMask[0]);
    //llassert_always(colorMask[1]);
    //llassert_always(colorMask[2]);
    // llassert_always(colorMask[3] == writeAlpha);

    for (std::unordered_map<LLGLenum, LLGLboolean>::iterator iter = sStateMap.begin();
         iter != sStateMap.end(); ++iter)
    {
        LLGLenum state = iter->first;
        LLGLboolean cur_state = iter->second;
        LLGLboolean gl_state = glIsEnabled(state);
        if(cur_state != gl_state)
        {
            dumpStates();
            LL_GL_ERRS << llformat("LLGLState error. State: 0x%04x",state) << LL_ENDL;
        }
    }
}

///////////////////////////////////////////////////////////////////////

LLGLState::LLGLState(LLGLenum state, S32 enabled) :
    mState(state), mWasEnabled(false), mIsEnabled(false)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;

    if (mState)
    {
        mWasEnabled = sStateMap[state];
        setEnabled(enabled);
    }
}

void LLGLState::setEnabled(S32 enabled)
{
    if (!mState)
    {
        return;
    }
    if (enabled == CURRENT_STATE)
    {
        enabled = sStateMap[mState] == GL_TRUE ? ENABLED_STATE : DISABLED_STATE;
    }
    else if (enabled == ENABLED_STATE && sStateMap[mState] != GL_TRUE)
    {
        gGL.flush();
        LLVkState::setEnable(mState, true);
        sStateMap[mState] = GL_TRUE;
    }
    else if (enabled == DISABLED_STATE && sStateMap[mState] != GL_FALSE)
    {
        gGL.flush();
        LLVkState::setEnable(mState, false);
        sStateMap[mState] = GL_FALSE;
    }
    mIsEnabled = enabled;
}

LLGLState::~LLGLState()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    if (mState)
    {
        if (gDebugGL)
        {
            if (!gDebugSession)
            {
                llassert_always(sStateMap[mState] == glIsEnabled(mState));
            }
            else
            {
                if (sStateMap[mState] != glIsEnabled(mState))
                {
                    ll_fail("GL enabled state does not match expected");
                }
            }
        }

        if (mIsEnabled != mWasEnabled)
        {
            gGL.flush();
            if (mWasEnabled)
            {
                LLVkState::setEnable(mState, true);
                sStateMap[mState] = GL_TRUE;
            }
            else
            {
                LLVkState::setEnable(mState, false);
                sStateMap[mState] = GL_FALSE;
            }
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

void LLGLManager::initGLStates()
{
    //gl states moved to classes in llglstates.h
    LLGLState::initClass();
}

////////////////////////////////////////////////////////////////////////////////

void parse_gl_version( S32* major, S32* minor, S32* release, std::string* vendor_specific, std::string* version_string )
{
    // GL_VERSION returns a null-terminated string with the format:
    // <major>.<minor>[.<release>] [<vendor specific>]

    const char* version = gl_info_string(GL_VERSION);
    *major = 0;
    *minor = 0;
    *release = 0;
    vendor_specific->assign("");

    if( !version )
    {
        return;
    }

    version_string->assign(version);

    std::string ver_copy( version );
    S32 len = (S32)strlen( version );   /* Flawfinder: ignore */
    S32 i = 0;
    S32 start;
    // Find the major version
    start = i;
    for( ; i < len; i++ )
    {
        if( '.' == version[i] )
        {
            break;
        }
    }
    std::string major_str = ver_copy.substr(start,i-start);
    LLStringUtil::convertToS32(major_str, *major);

    if( '.' == version[i] )
    {
        i++;
    }

    // Find the minor version
    start = i;
    for( ; i < len; i++ )
    {
        if( ('.' == version[i]) || isspace(version[i]) )
        {
            break;
        }
    }
    std::string minor_str = ver_copy.substr(start,i-start);
    LLStringUtil::convertToS32(minor_str, *minor);

    // Find the release number (optional)
    if( '.' == version[i] )
    {
        i++;

        start = i;
        for( ; i < len; i++ )
        {
            if( isspace(version[i]) )
            {
                break;
            }
        }

        std::string release_str = ver_copy.substr(start,i-start);
        LLStringUtil::convertToS32(release_str, *release);
    }

    // Skip over any white space
    while( version[i] && isspace( version[i] ) )
    {
        i++;
    }

    // Copy the vendor-specific string (optional)
    if( version[i] )
    {
        vendor_specific->assign( version + i );
    }
}


void parse_glsl_version(S32& major, S32& minor)
{
    // GL_SHADING_LANGUAGE_VERSION returns a null-terminated string with the format:
    // <major>.<minor>[.<release>] [<vendor specific>]

    const char* version = gl_info_string(GL_SHADING_LANGUAGE_VERSION);
    major = 0;
    minor = 0;

    if( !version )
    {
        return;
    }

    std::string ver_copy( version );
    S32 len = (S32)strlen( version );   /* Flawfinder: ignore */
    S32 i = 0;
    S32 start;
    // Find the major version
    start = i;
    for( ; i < len; i++ )
    {
        if( '.' == version[i] )
        {
            break;
        }
    }
    std::string major_str = ver_copy.substr(start,i-start);
    LLStringUtil::convertToS32(major_str, major);

    if( '.' == version[i] )
    {
        i++;
    }

    // Find the minor version
    start = i;
    for( ; i < len; i++ )
    {
        if( ('.' == version[i]) || isspace(version[i]) )
        {
            break;
        }
    }
    std::string minor_str = ver_copy.substr(start,i-start);
    LLStringUtil::convertToS32(minor_str, minor);
}

LLGLUserClipPlane::LLGLUserClipPlane(const LLPlane& p, const glm::mat4& modelview, const glm::mat4& projection, bool apply)
{
    mApply = apply;

    if (mApply)
    {
        mModelview = modelview;
        mProjection = projection;

        //flip incoming LLPlane to get consistent behavior compared to frustum culling
        setPlane(-p[0], -p[1], -p[2], -p[3]);
    }
}

void LLGLUserClipPlane::disable()
{
    if (mApply)
    {
        gGL.matrixMode(LLRender::MM_PROJECTION);
        gGL.popMatrix();
        gGL.matrixMode(LLRender::MM_MODELVIEW);
    }
    mApply = false;
}

void LLGLUserClipPlane::setPlane(F32 a, F32 b, F32 c, F32 d)
{
    const glm::mat4& P = mProjection;
    const glm::mat4& M = mModelview;

    glm::mat4 invtrans_MVP = glm::transpose(glm::inverse(P*M));
    glm::vec4 oplane(a,b,c,d);
    glm::vec4 cplane = invtrans_MVP * oplane;

    cplane /= fabs(cplane[2]); // normalize such that depth is not scaled
    cplane[3] -= 1;

    if(cplane[2] < 0)
        cplane *= -1;

    glm::mat4 suffix = glm::identity<glm::mat4>();
    suffix = glm::row(suffix, 2, cplane);
    glm::mat4 newP = suffix * P;
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.pushMatrix();
    gGL.loadMatrix(glm::value_ptr(newP));
    gGLObliqueProjectionInverse = LLMatrix4(glm::value_ptr(glm::transpose(glm::inverse(newP))));
    gGL.matrixMode(LLRender::MM_MODELVIEW);
}

LLGLUserClipPlane::~LLGLUserClipPlane()
{
    disable();
}

LLGLDepthTest::LLGLDepthTest(GLboolean depth_enabled, GLboolean write_enabled, GLenum depth_func)
: mPrevDepthEnabled(sDepthEnabled), mPrevDepthFunc(sDepthFunc), mPrevWriteEnabled(sWriteEnabled)
{
    stop_glerror();
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    checkState();

    if (!depth_enabled)
    { // always disable depth writes if depth testing is disabled
      // GL spec defines this as a requirement, but some implementations allow depth writes with testing disabled
      // The proper way to write to depth buffer with testing disabled is to enable testing and use a depth_func of GL_ALWAYS
        write_enabled = GL_FALSE;
    }

    if (depth_enabled != sDepthEnabled)
    {
        gGL.flush();
        LLVkState::setEnable(GL_DEPTH_TEST, depth_enabled != 0);
        sDepthEnabled = depth_enabled;
    }
    if (depth_func != sDepthFunc)
    {
        gGL.flush();
        LLVkState::setDepthFunc(depth_func);
        sDepthFunc = depth_func;
    }
    if (write_enabled != sWriteEnabled)
    {
        gGL.flush();
        LLVkState::setDepthMask(write_enabled != 0);
        sWriteEnabled = write_enabled;
    }
}

LLGLDepthTest::~LLGLDepthTest()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    checkState();
    if (sDepthEnabled != mPrevDepthEnabled )
    {
        gGL.flush();
        LLVkState::setEnable(GL_DEPTH_TEST, mPrevDepthEnabled != 0);
        sDepthEnabled = mPrevDepthEnabled;
    }
    if (sDepthFunc != mPrevDepthFunc)
    {
        gGL.flush();
        LLVkState::setDepthFunc(mPrevDepthFunc);
        sDepthFunc = mPrevDepthFunc;
    }
    if (sWriteEnabled != mPrevWriteEnabled )
    {
        gGL.flush();
        LLVkState::setDepthMask(mPrevWriteEnabled != 0);
        sWriteEnabled = mPrevWriteEnabled;
    }
}

void LLGLDepthTest::checkState()
{
    if (gDebugGL)
    {
        GLint func = 0;
        GLboolean mask = GL_FALSE;

        glGetIntegerv(GL_DEPTH_FUNC, &func);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &mask);

        if (glIsEnabled(GL_DEPTH_TEST) != sDepthEnabled ||
            sWriteEnabled != mask ||
            sDepthFunc != func)
        {
            if (gDebugSession)
            {
                gFailLog << "Unexpected depth testing state." << std::endl;
            }
            else
            {
                LL_GL_ERRS << "Unexpected depth testing state." << LL_ENDL;
            }
        }
    }
}

LLGLSquashToFarClip::LLGLSquashToFarClip()
{
    glm::mat4 proj = get_current_projection();
    setProjectionMatrix(proj, 0);
}

LLGLSquashToFarClip::LLGLSquashToFarClip(const glm::mat4& P, U32 layer)
{
    setProjectionMatrix(P, layer);
}

void LLGLSquashToFarClip::setProjectionMatrix(glm::mat4 projection, U32 layer)
{
    F32 depth = 0.99999f - 0.0001f * layer;

    glm::vec4 P_row_3 = glm::row(projection, 3) * depth;
    projection = glm::row(projection, 2, P_row_3);

    LLRender::eMatrixMode last_matrix_mode = gGL.getMatrixMode();

    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.pushMatrix();
    gGL.loadMatrix(glm::value_ptr(projection));

    gGL.matrixMode(last_matrix_mode);
}

LLGLSquashToFarClip::~LLGLSquashToFarClip()
{
    LLRender::eMatrixMode last_matrix_mode = gGL.getMatrixMode();

    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.popMatrix();

    gGL.matrixMode(last_matrix_mode);
}



LLGLSPipelineSkyBox::LLGLSPipelineSkyBox()
: mCullFace(GL_CULL_FACE)
, mSquashClip()
{
}

LLGLSPipelineSkyBox::~LLGLSPipelineSkyBox()
{
}

LLGLSPipelineDepthTestSkyBox::LLGLSPipelineDepthTestSkyBox(bool depth_test, bool depth_write)
: LLGLSPipelineSkyBox()
, mDepth(depth_test ? GL_TRUE : GL_FALSE, depth_write ? GL_TRUE : GL_FALSE, GL_LEQUAL)
{

}

LLGLSPipelineBlendSkyBox::LLGLSPipelineBlendSkyBox(bool depth_test, bool depth_write)
: LLGLSPipelineDepthTestSkyBox(depth_test, depth_write)
, mBlend(GL_BLEND)
{
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
}

#if LL_WINDOWS
// Expose desired use of high-performance graphics processor to Optimus driver and to AMD driver
// https://docs.nvidia.com/gameworks/content/technologies/desktop/optimus.htm
extern "C"
{
    __declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif


