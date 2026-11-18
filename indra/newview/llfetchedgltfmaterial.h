/**
 * @file   llfetchedgltfmaterial.h
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


#pragma once

#include "llgltfmaterial.h"
#include "llpointer.h"
#include "llviewertexture.h"
#include "rkhandletable.h"
#include <atomic>
#include <type_traits>

class LLFetchedGLTFMaterial;
using LLGLTFMatHandles = LLHandleTable<LLFetchedGLTFMaterial>;       // <FS> C2.4: the handles of fetched glTF materials (rkhandletable.h)

class LLShaderProgram;
class LLGLTFMaterialList;
class LLTerrainMaterials;

class LLFetchedGLTFMaterial: public LLGLTFMaterial
{
    // for lifetime management
    friend class LLGLTFMaterialList;
    friend class LLTerrainMaterials;
public:
    LLFetchedGLTFMaterial();
    virtual ~LLFetchedGLTFMaterial();

    LLFetchedGLTFMaterial& operator=(const LLFetchedGLTFMaterial& rhs);
    // LLGLTFMaterial::operator== is defined, but LLFetchedGLTFMaterial::operator== is not.
    bool operator==(const LLGLTFMaterial& rhs) const = delete;

    // If this material is loaded, fire the given function
    void onMaterialComplete(std::function<void()> material_complete);

    // bind this material for rendering
    //   media_tex - optional media texture that may override the base color texture
    void bind(LLViewerTexture* media_tex = nullptr);

    bool isFetching() const { return mFetching; }
    bool isLoaded() const { return !mFetching && mFetchSuccess; }

    // <FS> C2.4 (doc/vulkan/PLAN-clean-core.md section 7): a handle for render-side data (LLGLTFMatRef), and the retire list (llcommon/rkretirelist.h): the last owner letting go parks the material for a
    // few frames, so a draw info that still reads it through its handle finds it intact
    U32 getRenderHandle();
    static LLFetchedGLTFMaterial* fromRenderHandle(U32 handle) { return LLGLTFMatHandles::resolve(handle); }
    /*virtual*/ bool retireOnLastRef() const override;
    /*virtual*/ U32 retireKind() const override { return 1; }

    void addTextureEntry(LLTextureEntry* te) override;
    void removeTextureEntry(LLTextureEntry* te) override;
    virtual bool replaceLocalTexture(const LLUUID& tracking_id, const LLUUID& old_id, const LLUUID& new_id) override;
    virtual void updateTextureTracking() override;

    // Textures used for fetching/rendering
    LLPointer<LLViewerFetchedTexture> mBaseColorTexture;
    LLPointer<LLViewerFetchedTexture> mNormalTexture;
    LLPointer<LLViewerFetchedTexture> mMetallicRoughnessTexture;
    LLPointer<LLViewerFetchedTexture> mEmissiveTexture;
    void clearFetchedTextures();

    std::set<LLTextureEntry*> mTextureEntires;

    // default material for when assets don't have one
    static LLFetchedGLTFMaterial sDefault;
protected:
    // Lifetime management

    void materialBegin();
    void materialComplete(bool success);

    // <FS> C2.4: made on the first getRenderHandle(), given back by the destructor. A copy of a material is a new object: it starts with no handle, and assigning one material to another keeps the target's own.
    struct RenderHandle
    {
        std::atomic<U32> v{0};
        RenderHandle() = default;
        RenderHandle(const RenderHandle&) {}
        RenderHandle& operator=(const RenderHandle&) { return *this; }
    };
    RenderHandle mRenderHandle;
    F64 mExpectedFlusTime; // since epoch in seconds
    bool mActive = true;
    bool mFetching = false;
    bool mFetchSuccess = false;
    std::vector<std::function<void()>> materialCompleteCallbacks;
};

// <FS> C2.4: a material reference that is a handle, not a counted pointer (the glTF counterpart of LLTexRef, llviewertexture.h). It holds no reference count and owns nothing: the owners (the texture
// entries, the material list) keep the material alive and the retire list keeps it intact for the frames that could still read it after the last owner let go. It reads like the LLPointer it replaces:
// get(), ->, *, conversion to a pointer, isNull / notNull, explicit bool (a test of "has a material" does not resolve). get() resolves the handle each time; a handle whose material is gone answers the
// default material (never nullptr while a material was assigned) and counts it (sDeadResolved: in a healthy run it stays 0).
class LLGLTFMatRef
{
public:
    LLGLTFMatRef() = default;
    LLGLTFMatRef(std::nullptr_t) {}
    LLGLTFMatRef(LLFetchedGLTFMaterial* mat) : mHandle(mat ? mat->getRenderHandle() : 0) {}
    template <class U, class = std::enable_if_t<std::is_base_of<LLFetchedGLTFMaterial, U>::value>>
    LLGLTFMatRef(const LLPointer<U>& p) : LLGLTFMatRef(static_cast<LLFetchedGLTFMaterial*>(p.get())) {}
    LLGLTFMatRef& operator=(LLFetchedGLTFMaterial* mat) { mHandle = mat ? mat->getRenderHandle() : 0; return *this; }
    template <class U, class = std::enable_if_t<std::is_base_of<LLFetchedGLTFMaterial, U>::value>>
    LLGLTFMatRef& operator=(const LLPointer<U>& p) { return *this = static_cast<LLFetchedGLTFMaterial*>(p.get()); }

    LLFetchedGLTFMaterial* get() const
    {
        if (!mHandle) return nullptr;
        LLFetchedGLTFMaterial* mat = LLGLTFMatHandles::resolve(mHandle);
        return mat ? mat : deadMaterial();
    }
    operator LLFetchedGLTFMaterial*() const { return get(); }
    LLFetchedGLTFMaterial* operator->() const { return get(); }
    LLFetchedGLTFMaterial& operator*() const { return *get(); }
    explicit operator bool() const { return mHandle != 0; }
    bool isNull() const { return mHandle == 0; }
    bool notNull() const { return mHandle != 0; }
    U32 handle() const { return mHandle; }

    static std::atomic<U64> sDeadResolved;     // resolutions of a handle whose material was gone (answered with the default material)
private:
    static LLFetchedGLTFMaterial* deadMaterial();
    U32 mHandle = 0;
};
