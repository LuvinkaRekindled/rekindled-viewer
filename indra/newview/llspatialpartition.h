/**
 * @file llspatialpartition.h
 * @brief LLSpatialGroup header file including definitions for supporting functions
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

#ifndef LL_LLSPATIALPARTITION_H
#define LL_LLSPATIALPARTITION_H

#define SG_MIN_DIST_RATIO 0.00001f

#include "lldrawable.h"
#include "lloctree.h"
#include "llpointer.h"
#include "llrefcount.h"
#include "llvertexbuffer.h"
#include "llgltypes.h"
#include "llcubemap.h"
#include "lldrawpool.h"
#include "llface.h"
#include "llviewercamera.h"
#include "llvector4a.h"
#include "llvoavatar.h"
#include "llavatarref.h"
#include "llfetchedgltfmaterial.h"

//<FS:Beq> needed to resolve render_hull dep
#include "llmodel.h"
//</FS:Beq>
#include <atomic>
#include <queue>
#include <unordered_map>

#define SG_STATE_INHERIT_MASK (OCCLUDED)
#define SG_INITIAL_STATE_MASK (static_cast<U32>(DIRTY) | static_cast<U32>(GEOM_DIRTY))

class LLViewerOctreePartition;
class LLSpatialPartition;
class LLSpatialBridge;
class LLSpatialGroup;
class LLViewerRegion;
class LLReflectionMap;

void pushVerts(LLFace* face);
//<FS:BEQ> Make helper functions externally visible for use from viewerwindow
void pushVerts(LLVolume* volume);
void drawBoxOutline(const LLVector3& pos, const LLVector3& size);
void drawBox(const LLVector3& c, const LLVector3& r);
S32 get_physics_detail(const LLVolumeParams& volume_params, const LLVector3& scale);
void renderMeshBaseHullWithOutline(LLVOVolume* volume, U32 data_mask, LLColor4& color, LLColor4& line_color);// <FS:Beq/> restore physics shape display in edit mode
void renderMeshBaseHull(LLVOVolume* volume, U32 data_mask, LLColor4& color);
void render_hull(LLModel::PhysicsMesh& mesh, const LLColor4& color);
void render_hull_with_outline(LLModel::PhysicsMesh& mesh, const LLColor4& color, const LLColor4& line_color); // <FS:Beq/> restore physics shape display in edit mode
//</FS:BEQ>

/*
    Class that represents a single Draw Call

    Make every effort to keep size minimal.
    Member ordering is important for cache coherency
*/
struct LLSceneFragment;
struct GeoRigSource;
void ll_scene_fragment_free(LLSceneFragment*& fragment);        // pipeline.cpp (S1)
class LLSpatialGroup;
void ll_scene_fragment_build(LLSpatialGroup* group);
class LLCullResult;
bool ll_scene_geo_on();                                         // pipeline.cpp (S3): LL_VK_SCENE_GEO=1 or 2
bool ll_scene_geo_serving();                                    // LL_VK_SCENE_GEO=2 and the table draws this frame (LL_VK_SCENE_GEO_AB=<seconds> alternates it)
bool ll_scene_geo_skip();                                       // the deferred geometry builders leave out the draws the table serves (serving, and not verifying)
bool ll_scene_geo_main_view();                                  // the camera / render flags of the main view (not a probe, an impostor, a shadow or a reflection render)
bool ll_scene_geo_hud_view();                                   // J1 K6 S1: the HUD pass (its own cull result: the table is told about it before the HUD's post deferred passes)
bool ll_scene_geo_hud_any();                                    // ... any of them
bool ll_scene_geo_hud_type(int tn);                             // ... and a table type the HUD pools draw from rows (the others stay in the HUD's render map)
bool ll_scene_geo_prepared(LLCullResult* cull);                 // the table was told this frame's visible fragments for this cull result
int ll_scene_geo_type_number(U32 type);                         // the pass type's number in the table's order (-1: not a table type)
bool ll_scene_geo_cull_has_type(LLCullResult* cull, U32 type);  // <FS> the result holds a draw of this pass type this frame (its render map, or the served items of its fragments)
void ll_scene_geo_palette_kick(LLCullResult* cull);               // S4 B: end of postSort: the palettes of the rigged items of the listed fragments are made on the pool (ll_scene_geo_frame takes them)
void ll_scene_geo_frame(LLCullResult* cull);                    // main view, once a frame before the passes
void ll_scene_geo_frame_end();                                  // main view, after the passes: releases of fragments that went during the frame
void ll_scene_geo_count(U32 what, U64 n);                       // 0 builder draws left out, 1 table draw records sent, 2 builds made again for want of a prepare            // pipeline.cpp (S2): after a group's rebuild

// <FS> S5a-1 (doc/vulkan/S5-design.md section 8): the plan of a group's static alpha draws, kept by its fragment (LL_VK_SCENE_ALPHA=1): the draw infos the alpha loop draws, in its order, as segments of
// draws a table can serve (mServed, equal mKey: one run) and draws it cannot (the old path draws them in place)
struct LLAlphaPlanSeg { U32 mFirst, mCount, mKey; bool mServed; U32 mRank; };      // mRank: served segments: the number of the first item among the fragment's alpha items
struct LLAlphaPlanView
{
    const LLAlphaPlanSeg* mSegs = nullptr;
    U32 mSegCount = 0;
    U32 mHandle = 0;            // the fragment's handle at the worker (0: not attached, nothing is served)
    const U32* mGlowIdx = nullptr;      // the plan indices of the served draws that have a glow buffer, ascending
    const U8* mGlowCls = nullptr;       // J1 K6 S5d: per glow index, 0 = a plain draw, 1 = a glTF material, 2 = a double sided glTF material (the class of its glow draw): the plan keeps no draw info
    const struct GeoRigSource* mItemRig = nullptr;          // J1 K2: per served rigged item (by rank): what its palette binding reads (avatar, skin, range); no draw info
    const U32* mItemWire = nullptr;
    U32* mItemBound = nullptr;  // P2: per served item (by rank), 5 U32: the binding the worker holds (epoch, palette id, joints, simplified list, its count); the rigged alpha runs send only what changed
    U32 mGlowCount = 0;
    bool mGlow = false;         // a draw of the group has an emissive buffer (the loop draws a glow pass after the group, post water)
};
int ll_alpha_plan(const LLSpatialGroup* group, LLAlphaPlanView& out, bool rigged = false);
bool ll_alpha_blend_pair(U32 id, int* src, int* dst);       // pipeline.cpp (S5b): a run key carries the id of its blend function pair in bits 8 and up (0 = the standard alpha blend)
bool ll_alpha_glow_table(bool pbr);      // pipeline.cpp (S5e): the glow second draw of the alpha items is drawn from the table (plain / glTF items)
bool ll_alpha_ab_on();           // pipeline.cpp (S5f F4): the alpha pull programs draw this frame in any view (transient records), not only the main view's fragments (LL_VK_SCENE_ALPHA=1 and the AB phase)
bool ll_alpha_serving();      // pipeline.cpp (S5a): the static main-view alpha pass is drawn from the scene table this frame      // pipeline.cpp: 0 no plan, 1 the plan (current), 2 the group's fragment is not current

// what the group's fragment says of its draw map (made with the fragment, 2026-11-12: the debug views and the alpha sort read these, not the draw infos)
bool ll_group_has_alpha_key(const LLSpatialGroup* group);                                   // the draw map has a PASS_ALPHA entry
bool ll_group_has_rigged_alpha_key(const LLSpatialGroup* group);                            // ... a PASS_ALPHA_RIGGED entry
bool ll_group_first_draw_model(const LLSpatialGroup* group, const LLMatrix4*& model);       // the model matrix of the first draw of the draw map (false: the map is empty or the fragment is not current)

class LLDrawInfo final : public LLRefCount
{
    LL_ALIGN_NEW;
protected:
    ~LLDrawInfo();

public:
    LLDrawInfo(const LLDrawInfo& rhs) = delete;
    const LLDrawInfo& operator=(const LLDrawInfo& rhs) = delete;

    // return a hash of this LLDrawInfo as a debug color
    LLColor4U getDebugColor() const;

    LLDrawInfo(U16 start, U16 end, U32 count, U32 offset,
                LLViewerTexture* image, LLVertexBuffer* buffer,
                bool fullbright = false, U8 bump = 0);

    // return mSkinHash->mHash, or 0 if mSkinHash is null
    U64 getSkinHash();

    LLVBRef mVertexBuffer;     // <FS> C3.5: a handle (llvertexbuffer.h), not a counted pointer
    U16 mStart = 0;
    U16 mEnd = 0;
    U32 mCount = 0;
    U32 mOffset = 0;

    // <FS> C2.3 (doc/vulkan/PLAN-clean-core.md section 7): handles, not counted pointers (LLTexRef, llviewertexture.h): the draw info owns no texture; the faces and the texture lists keep it alive and the retire list
    // keeps it intact for the frames in flight
    LLTexRef     mTexture;
    LLTexRef mSpecularMap;
    LLTexRef mNormalMap;

    const LLMatrix4* mTextureMatrix = nullptr;
    const LLMatrix4* mModelMatrix = nullptr;

    LLAvatarRef mAvatar;       // <FS> C2.4d: a handle (llavatarref.h), not a counted pointer
    LLSkinRef mSkinInfo;       // <FS> C2.4c: a handle (llmodel.h), not a counted pointer (<FS:Beq/> had made it a const pointer to be defensive about UAF with skinInfo during LocalMesh)

    // <FS> C2.4a: this was a counted LLPointer<LLMaterial> that was only ever tested for null (the batching test in registerFace, the materials pull program choice in the scene table): a flag, no lifetime to carry
    bool mHasMaterial = false;
    bool mRasterFrag = false;          // <FS> J1 K6 step 3d (ray tracing only): a raster shadow pass (a probe face, a spot map) finds this glTF or texture matrix caster as an entry of the group's fragment (mRaster, mRasterTex)
    bool mAlphaFragTex = false;        // <FS> C5 J3: this alpha caster has a texture matrix and the group's fragment holds it in its texture matrix list (the cascades' shadow records carry it)
    // <FS> C4.1 alpha: this draw info's alpha caster entry went into the group's scene fragment (scene_fragment_fill_alpha): the cascades' render map job asks the fragment whether the worker holds it, no lookup by buffer ids
    bool mAlphaInFragment = false;
    bool mRigAlphaSrc = false;          // <FS> J1 K3d: this rigged alpha masked caster is read by the cascades' alpha builder from its fragment item (GeoRigSource: mAlphaOk), not from the render map list

    // PBR material parameters
    LLGLTFMatRef mGLTFMaterial;        // <FS> C2.4: a handle (llfetchedgltfmaterial.h), not a counted pointer

    LLVector4 mSpecColor = LLVector4(1.f, 1.f, 1.f, 0.5f); // XYZ = Specular RGB, W = Specular Exponent

    std::vector<LLTexRef> mTextureList;     // <FS> C2.3: handles

    LLUUID mMaterialID; // id of LLGLTFMaterial or LLMaterial applied to this draw info

    U32 mShaderMask = 0;
    bool mGeoServed = false;            // <FS> S3: the scene geometry table draws this one (its fragment holds a served entry): the deferred geometry builders leave it out while the table is serving
    F32  mEnvIntensity = 0.f;
    F32  mAlphaMaskCutoff = 0.5f;

    LLRender::eBlendFactor mBlendFuncSrc = LLRender::BF_SOURCE_ALPHA;
    LLRender::eBlendFactor  mBlendFuncDst = LLRender::BF_ONE_MINUS_SOURCE_ALPHA;
    U8   mDiffuseAlphaMode = 0;
    U8   mBump = 0;
    U8   mShiny = 0;
    bool mFullbright = false;
    bool mHasGlow = false;

    // <FS> ray scene: the draw's material in the viewer's registry (pipeline.cpp, rt_material_id): the row in the low 32 bits, its generation in the high 32 (0 = not looked up yet).
    // Atomic: the packet builders of several cascades run in parallel and meet the same draw.
    std::atomic<U64> mRtMaterial{0};
};

// <FS> J1 K1 (PLAN-clean-core.md, the J1 research note): the geometric range of a draw, all the LOD caches read of it (the shadow caster LOD and the main view's rigged LOD key on the buffer, the first index,
// the index count and a level, and read the vertex range to measure): a range of a vertex buffer, whoever holds it. Made from a draw info now, from a fragment item later. mVb null = no range.
struct GeoRange
{
    const LLVertexBuffer* mVb = nullptr;
    U32 mOffset = 0;        // first index
    U32 mCount = 0;         // index count
    U32 mStart = 0;         // first vertex
    U32 mEnd = 0;           // last vertex
};
inline GeoRange ll_geo_range(const LLDrawInfo* p)
{
    GeoRange r;
    if (p && p->mVertexBuffer.notNull())
    {
        r.mVb = p->mVertexBuffer.get();
        r.mOffset = p->mOffset;
        r.mCount = p->mCount;
        r.mStart = p->mStart;
        r.mEnd = p->mEnd;
    }
    return r;
}

// <FS> J1 K5: what the ray scene's material registry (pipeline.cpp, rt_material_id) reads of a draw, kept by a fragment's item instead of read through the draw info: the glTF material and the textures as
// handles, the batch, the flags, the cutoff, the specular values; and the row cache of the registry (the row in the low 32 bits, its generation in the high 32, 0 = not looked up yet: atomic, the packet builders of
// several cascades meet the same item). mSet: filled (only when ray scene materials are on).
struct RtMatCache
{
    std::atomic<U64> mValue{ 0 };
    RtMatCache() = default;
    RtMatCache(const RtMatCache& o) : mValue(o.mValue.load(std::memory_order_relaxed)) {}
    RtMatCache& operator=(const RtMatCache& o) { mValue.store(o.mValue.load(std::memory_order_relaxed), std::memory_order_relaxed); return *this; }
};
struct RtMatSource
{
    LLGLTFMatRef mGltf;
    LLTexRef mTexture;
    LLTexRef mList[4];
    U32 mListSize = 0;
    LLTexRef mNormalMap, mSpecularMap;
    bool mFullbright = false, mEmissiveLegacy = false, mSet = false;
    F32 mCutoff = 0.5f;
    F32 mSpec[4] = { 1.f, 1.f, 1.f, 0.5f };
    F32 mEnv = 0.f;
    mutable RtMatCache mCache;
};

// <FS> J1 K2: what the per frame readers of a RIGGED fragment item need, kept with the item instead of read through its draw info every frame: the avatar and the skin (handles: a gone one reads as null),
// the buffer (held, so the range stays valid) and the range. The palette bindings, the LOD choices and their census read this; the draw info is only read when the fragment is made.
struct GeoRigSource
{
    LLAvatarRef mAvatar;
    LLSkinRef mSkin;
    LLPointer<LLVertexBuffer> mVb;
    U32 mOffset = 0, mCount = 0, mStart = 0, mEnd = 0;
    // J1 K3d: what a cascade's alpha caster entry needs besides that (the draw's textures as handles, the batch of textures if it has more than one, the texture matrix, the cutoff): the alpha builder makes the
    // entry of a rigged alpha masked caster from this, without the draw info. mAlphaOk: the draw is one a record can carry (shadow_alpha_packet_ok when the fragment was made)
    LLTexRef mTextures[16];             // (a batch of up to sixteen textures: the shadow entries are made in windows of four, see LLVkBridge::kAlphaWindowShift)
    LLTexRef mTexture;
    LLGLTFMatRef mGltf;                 // (the rigged glTF casters: the material is read when the entry is made, as the list path does)
    U32 mTextureListSize = 0;
    const LLMatrix4* mTextureMatrix = nullptr;
    F32 mAlphaCutoff = 0.5f;
    bool mAlphaOk = false;
    RtMatSource mMat;                   // J1 K5: the ray scene material source (filled only with ray scene materials)
    GeoRange range() const
    {
        GeoRange r;
        r.mVb = mVb.get();
        r.mOffset = mOffset;
        r.mCount = mCount;
        r.mStart = mStart;
        r.mEnd = mEnd;
        return r;
    }
};
inline GeoRigSource ll_geo_rig_source(const LLDrawInfo* p)
{
    GeoRigSource s;
    if (p)
    {
        s.mAvatar = p->mAvatar;
        s.mSkin = p->mSkinInfo;
        s.mVb = p->mVertexBuffer;
        s.mOffset = p->mOffset;
        s.mCount = p->mCount;
        s.mStart = p->mStart;
        s.mEnd = p->mEnd;
        s.mTexture = p->mTexture;
        s.mGltf = p->mGLTFMaterial;
        s.mTextureListSize = (U32)p->mTextureList.size();
        for (size_t t = 0; t < p->mTextureList.size() && t < 16; ++t)
        {
            s.mTextures[t] = p->mTextureList[t];
        }
        s.mTextureMatrix = p->mTextureMatrix;
        s.mAlphaCutoff = p->mAlphaMaskCutoff;
    }
    return s;
}

class LLSpatialGroup;
// <FS> C5 J2: one draw of a particle group: a run of rows of the group's row buffer (the GPU particle path, PLAN-gpu-particles.md) with one texture and blend state. A particle group keeps these instead of draw
// infos: the alpha pass makes its entries from them directly (ll_particle_transient), nothing is allocated per rebuild or classified per draw
struct LLParticleRun
{
    LLPointer<LLViewerTexture> mTexture;
    U32 mFirst = 0;                     // the first row
    U32 mRows = 0;                      // how many rows (6 vertices each, made by the pull shader)
    U8 mBlendSrc = 0, mBlendDst = 0;    // LLRender::eBlendFactor
    bool mFullbright = false;
    bool mGlow = false;                 // a particle in the run glows: the glow pass draws it
};

// <FS> C4.1: the groups that have (or had, until the next prune) a state bit the rebuild loops of the shadow preparation look for (MESH_DIRTY, GEOM_DIRTY, NEW_DRAWINFO), kept as they are set, so that those loops
// ask the few groups instead of reading the state word of every visible group of every cascade
namespace LLDirtyGroups
{
    void add(LLSpatialGroup* group);
    void forget(LLSpatialGroup* group);                         // a group that is destroyed
    std::vector<LLSpatialGroup*> snapshot();
    void prune();                                               // main thread: groups with none of the bits left leave
}

LL_ALIGN_PREFIX(16)
class LLSpatialGroup : public LLOcclusionCullingGroup
{
    using super = LLOcclusionCullingGroup;
    friend class LLSpatialPartition;
    friend class LLOctreeStateCheck;
public:

    LLSpatialGroup(const LLSpatialGroup& rhs) = delete;
    const LLSpatialGroup& operator=(const LLSpatialGroup& rhs) = delete;

    static U32 sNodeCount;
    static bool sNoDelete; //deletion of spatial groups and draw info not allowed if true

    typedef std::vector<LLPointer<LLSpatialGroup> > sg_vector_t;
    typedef std::vector<LLPointer<LLSpatialBridge> > bridge_list_t;
    typedef std::vector<LLPointer<LLDrawInfo> > drawmap_elem_t;
    typedef std::unordered_map<U32, drawmap_elem_t > draw_map_t;
    typedef std::vector<LLPointer<LLVertexBuffer> > buffer_list_t;
    typedef std::unordered_map<LLFace*, buffer_list_t> buffer_texture_map_t;
    typedef std::unordered_map<U32, buffer_texture_map_t> buffer_map_t;

    struct CompareDistanceGreater
    {
        bool operator()(const LLSpatialGroup* const& lhs, const LLSpatialGroup* const& rhs)
        {
            return lhs->row().distance > rhs->row().distance;
        }
    };

    struct CompareUpdateUrgency
    {
        bool operator()(const LLPointer<LLSpatialGroup> lhs, const LLPointer<LLSpatialGroup> rhs)
        {
            return lhs->getUpdateUrgency() > rhs->getUpdateUrgency();
        }
    };

    struct CompareDepthGreater
    {
        bool operator()(const LLSpatialGroup* const& lhs, const LLSpatialGroup* const& rhs)
        {
            return lhs->row().depth > rhs->row().depth;
        }
    };

    struct CompareRenderOrder
    {
        bool operator()(const LLSpatialGroup* const& lhs, const LLSpatialGroup* const& rhs)
        {
            if (lhs->mAvatarp != rhs->mAvatarp)
            {
                return lhs->mAvatarp < rhs->mAvatarp;
            }

            return lhs->mRenderOrder > rhs->mRenderOrder;
        }
    };

    typedef enum
    {
        GEOM_DIRTY              = LLViewerOctreeGroup::INVALID_STATE,
        ALPHA_DIRTY             = (GEOM_DIRTY << 1),
        IN_IMAGE_QUEUE          = (ALPHA_DIRTY << 1),
        IMAGE_DIRTY             = (IN_IMAGE_QUEUE << 1),
        MESH_DIRTY              = (IMAGE_DIRTY << 1),
        NEW_DRAWINFO            = (MESH_DIRTY << 1),
        IN_BUILD_Q1             = (NEW_DRAWINFO << 1),
        IN_BUILD_Q2             = (IN_BUILD_Q1 << 1),
        FS_RELEASED             = (IN_BUILD_Q2 << 1),       // <FS> Released (EXPIRY-design section 4): the draw infos and scene fragment were dropped because no camera accepted the group for a long time; the vertex data stays
        FS_WAKING               = (FS_RELEASED << 1),       // <FS> a camera accepted a Released group: it waits in the paced wake queue (LLPipeline::wakeGroups)
        STATE_MASK              = 0x0000FFFF,
    } eSpatialState;

    LLSpatialGroup(OctreeNode* node, LLSpatialPartition* part);

    bool isHUDGroup() const;

    void clearDrawMap();
    void validate();

    void setState(U32 state, S32 mode);
    void clearState(U32 state, S32 mode);
    void clearState(U32 state)     {row().state &= ~state;}

    LLSpatialGroup* getParent();

    bool addObject(LLDrawable *drawablep);
    bool removeObject(LLDrawable *drawablep, bool from_octree = false);
    bool updateInGroup(LLDrawable *drawablep, bool immediate = false); // Update position if it's in the group
    void expandExtents(const LLVector4a* addingExtents, const LLXformMatrix& currentTransform);
    void shift(const LLVector4a &offset);

    // TODO: this no longer appears to be called, figure out if it's important and if not remove it
    void destroyGLState(bool keep_occlusion = false);

    void updateDistance(LLCamera& camera);
    F32 getUpdateUrgency() const;
    bool changeLOD();
    void rebuildGeom();
    void rebuildMesh();

    void setState(U32 state)       { row().state |= state; if (state & (MESH_DIRTY | GEOM_DIRTY | NEW_DRAWINFO)) { LLDirtyGroups::add(this); } }
    void dirtyGeom() { setState(GEOM_DIRTY); }
    void dirtyMesh() { setState(MESH_DIRTY); }

    void drawObjectBox(LLColor4 col);

    LLDrawable* lineSegmentIntersect(const LLVector4a& start, const LLVector4a& end,
        bool pick_transparent,
        bool pick_rigged,
        bool pick_unselectable,
        bool pick_reflection_probe,
        S32* face_hit,                          // return the face hit
        LLVector4a* intersection = NULL,         // return the intersection point
        LLVector2* tex_coord = NULL,            // return the texture coordinates of the intersection point
        LLVector4a* normal = NULL,               // return the surface normal at the intersection point
        LLVector4a* tangent = NULL             // return the surface tangent at the intersection point
    );


    LLSpatialPartition* getSpatialPartition() {return (LLSpatialPartition*)mSpatialPartition;}

     //LISTENER FUNCTIONS
    virtual void handleInsertion(const TreeNode* node, LLViewerOctreeEntry* face);
    virtual void handleRemoval(const TreeNode* node, LLViewerOctreeEntry* face);
    virtual void handleDestruction(const TreeNode* node);
    virtual void handleChildAddition(const OctreeNode* parent, OctreeNode* child);

    // LLViewerOctreeGroup
    virtual void rebound();

public:
protected:
    virtual ~LLSpatialGroup();

public:
    LLPointer<LLVertexBuffer> mVertexBuffer;
    draw_map_t mDrawMap;
    std::vector<LLParticleRun> mParticleRuns;       // <FS> C5 J2: a particle group's draws (rows of mVertexBuffer); mDrawMap[PASS_ALPHA] exists (empty) while there are any, so the alpha group lists find the group
    // <FS> S1 phase 0 (doc/vulkan/PLAN-retained-records.md): bumped whenever the draw infos are cleared (every creation of draw infos follows a clear), so the same serial on two frames
    // means the same draw info set. mSceneSeenSerial: what the main view saw last (LL_VK_SCENE_STATS counters only).
    U32 mDrawMapSerial = 1;
    S32 mReleaseDeadline = 0;                  // <FS> expiry (cold mode 3): the frame at which the group's geometry is looked at for release (0 = not queued: LLPipeline::scheduleRelease / releaseForget, pipeline.cpp)
    U32 mChurnFrame = 0, mChurnSerial = 0;     // <FS> census only (LL_VK_VISCENSUS): the frame this group was last in the main view's render map loop and its mDrawMapSerial then
    U32 mSceneSeenSerial = 0;
    U32 mSceneSeenFrame = 0;
    // <FS> S1: the retained shadow fragment of this group (pipeline.cpp), valid while its serial is mDrawMapSerial; freed with the group
    struct LLSceneFragment* mSceneFragment = nullptr;
    U64 mGeoServedMask = 0;             // <FS> S3 G4 (S4 B: the bits after the static types are the rigged variants): the bits (kGeoTypes order) of the pass types whose draw infos in this group are ALL drawn by the scene table: postSort does not list those
    U32 mGeoServedTris = 0;             // ... their index count (the triangle statistic)
    U8 mGeoAlphaBits = 0;               // <FS> J1 K6 S5a: 1 = the draw map has PASS_ALPHA draws, 2 = PASS_ALPHA_RIGGED (set with mGeoAllServed: the alpha group lists are all postSort takes from an all-served group)
    bool mGeoAllServed = false;         // <FS> R2 (S-retire-views-design.md): every non-empty draw map type of the group is served by the table: postSort has nothing to put in the render map for it
    // <FS> C4.1: the sun cascades' render map job asks this before it walks the group's draw map: bits 0-3 the answer (1 nothing to push, 2 the group has an alpha draw map entry, 4 a rigged alpha one, 8 resident), the rest a key of what the
    // answer was made from (draw map serial, fragment state, render type mask, the job's switches). Written by the jobs of several cascades at once: one atomic word.
    std::atomic<U64> mMapPlan{ 0 };
    std::atomic<bool> mInDirtyReg{ false };        // <FS> C4.1: listed in LLDirtyGroups
    U32 mDirtyIdx = 0;
    U32 mGeoHandle = 0;                 // <FS> S3 G3: the worker's handle of this group's scene geometry fragment (0 = none, or the table does not hold it): what postSort lists for the passes
    static U32 sGuardFragmentOnly;      // <FS> clearDrawMap calls that found a fragment and an empty draw map (the normal case since the map is emptied after the build): the builder guard fired for the fragment alone
    static U32 sDrawMapsCleared;        // non empty draw maps cleared (group rebuilds), counted per frame by LL_VK_SCENE_STATS

    bridge_list_t mBridgeList;
    buffer_map_t mBufferMap; //used by volume buffers to attempt to reuse vertex buffers

    U32 mGeometryBytes; //used by volumes to track how many bytes of geometry data are in this node
    F32 mSurfaceArea; //used by volumes to track estimated surface area of geometry in this node
    F32 mBuilt;

    // <FS> C1b.2: distance, depth, lastUpdateDistance, lastUpdateTime, pixelArea, radius, objectBoxSize, viewAngle and lastUpdateViewAngle are in this group's row (row().distance ...)

    //used by LLVOAVatar to set render order in alpha draw pool to preserve legacy render order behavior
    LLVOAvatar* mAvatarp = nullptr;
    U32 mRenderOrder = 0;
    // Reflection Probe associated with this node (if any)
    LLPointer<LLReflectionMap> mReflectionProbe = nullptr;
} LL_ALIGN_POSTFIX(16);

class LLGeometryManager
{
public:
    std::vector<LLFace*> mFaceList;
    virtual ~LLGeometryManager() { }
    virtual void rebuildGeom(LLSpatialGroup* group) = 0;
    virtual void rebuildMesh(LLSpatialGroup* group) = 0;
    virtual void getGeometry(LLSpatialGroup* group) = 0;
    virtual void addGeometryCount(LLSpatialGroup* group, U32 &vertex_count, U32 &index_count);
};

class LLSpatialPartition: public LLViewerOctreePartition, public LLGeometryManager
{
public:
    LLSpatialPartition(U32 data_mask,  bool render_by_group, LLViewerRegion* regionp);
    virtual ~LLSpatialPartition();

    LLSpatialGroup *put(LLDrawable *drawablep, bool was_visible = false);
    bool remove(LLDrawable *drawablep, LLSpatialGroup *curp);

    LLDrawable* lineSegmentIntersect(const LLVector4a& start, const LLVector4a& end,
                                     bool pick_transparent,
                                     bool pick_rigged,
                                     bool pick_unselectable,
                                     bool pick_reflection_probe,
                                     S32* face_hit,                          // return the face hit
                                     LLVector4a* intersection = NULL,         // return the intersection point
                                     LLVector2* tex_coord = NULL,            // return the texture coordinates of the intersection point
                                     LLVector4a* normal = NULL,               // return the surface normal at the intersection point
                                     LLVector4a* tangent = NULL             // return the surface tangent at the intersection point
        );


    // If the drawable moves, move it here.
    virtual void move(LLDrawable *drawablep, LLSpatialGroup *curp, bool immediate = false);
    virtual void shift(const LLVector4a &offset);

    virtual F32 calcDistance(LLSpatialGroup* group, LLCamera& camera);
    virtual F32 calcPixelArea(LLSpatialGroup* group, LLCamera& camera);

    virtual void rebuildGeom(LLSpatialGroup* group);
    virtual void rebuildMesh(LLSpatialGroup* group);

    bool visibleObjectsInFrustum(LLCamera& camera);
    /*virtual*/ S32 cull(LLCamera &camera, bool do_occlusion=false); // Cull on arbitrary frustum
    S32 cull(LLCamera &camera, std::vector<LLDrawable *>* results, bool for_select); // Cull on arbitrary frustum
    // <FS> stage 3a: cull(camera) in pieces (see LLViewerOctreeCull::traverseTop); the main view only (no shadow culler)
    void cullSplit(LLCamera& camera, S32 levels, const std::function<bool(const OctreeNode*)>& descend_more, const std::function<void(const LLViewerOctreeCull::SplitTask&)>& on_task);
    void cullTask(LLCamera& camera, const LLViewerOctreeCull::SplitTask& task);
    // <FS> C4.5 S1a (LL_VK_VISSET=2): the membership the main view's cull gives this partition, worked out again by a walk of the same verdicts that changes nothing (no stamp, no list, no queue): the groups the cull would
    // list (drawable groups, visible groups) and per visited node the mRes it was entered with and what happened to it. The root is rebounded by the caller.
    struct CullCollect
    {
        struct NodeInfo { S8 mEntryRes = 0; S8 mResOut = 0; U8 mOutcome = 0; U8 mMember = 0; };      // entry / out: the walk's mRes when the node was entered and after its own frustum test; outcome: 1 dropped as occluded, 2 outside the frustum, 3 passed; member: 0 no, 1 drawable group, 2 visible group
        struct Item { LLSpatialGroup* mGroup; U8 mKind; S32 mLod = 0; };      // in the order the walk reached them: kind 0 a node dropped as occluded (markOccluder), 1 a drawable group, 2 a visible group (markNotCulled)
        std::vector<LLSpatialGroup*> mDrawable, mVisible;
        std::vector<Item> mOrder;
        std::unordered_map<const void*, NodeInfo> mNodes;
    };
    void collectCull(LLCamera& camera, CullCollect& out);
    void collectSubtree(LLCamera& camera, const OctreeNode* node, S32 entry_res, CullCollect& out);       // the same from one node, entered with mRes = entry_res (out is added to, not cleared)
    void collectCullShadow(LLCamera& camera, CullCollect& out);                                          // the same for a sun cascade's cull (LLOctreeCullShadow: no occlusion, the shadow frustum tests)
    void collectSubtreeShadow(LLCamera& camera, const OctreeNode* node, S32 entry_res, CullCollect& out);
    // <FS> the cascades' point cloud (LL_VK_PC_SPLIT): getVisibleExtents in pieces, the way cullSplit and cullTask cut the main cull. min and max are the caller's accumulators (the roots are rebounded by the caller,
    // on its own thread, before any of these run: rebound() writes); empty is cleared when the walk found a visible group.
    // <FS> the cascades' cull (LL_VK_SC_SPLIT): LLOctreeCullShadow over a whole partition, and in pieces (the roots are rebounded by the caller before any of these run)
    void cullShadowWhole(LLCamera& camera);
    void cullShadowSplit(LLCamera& camera, S32 levels, const std::function<bool(const OctreeNode*)>& descend_more, const std::function<void(const LLViewerOctreeCull::SplitTask&)>& on_task);
    void cullShadowTask(LLCamera& camera, const LLViewerOctreeCull::SplitTask& task);
    void visibleExtentsWhole(LLCamera& camera, LLVector4a& min, LLVector4a& max, bool& empty);
    void visibleExtentsSplit(LLCamera& camera, LLVector4a& min, LLVector4a& max, bool& empty, S32 levels, const std::function<bool(const OctreeNode*)>& descend_more, const std::function<void(const LLViewerOctreeCull::SplitTask&)>& on_task);
    void visibleExtentsTask(LLCamera& camera, const LLViewerOctreeCull::SplitTask& task, LLVector4a& min, LLVector4a& max, bool& empty);

    bool isVisible(const LLVector3& v);
    bool isHUDPartition() ;

    LLSpatialBridge* asBridge() { return mBridge; }
    bool isBridge() { return asBridge() != NULL; }

    void renderPhysicsShapes(bool depth_only);
    void renderDebug();
    void renderIntersectingBBoxes(LLCamera* camera);
    void restoreResources();

    bool getVisibleExtents(LLCamera& camera, LLVector3& visMin, LLVector3& visMax);

public:
    LLSpatialBridge* mBridge; // NULL for non-LLSpatialBridge instances, otherwise, mBridge == this
                            // use a pointer instead of making "isBridge" and "asBridge" virtual so it's safe
                            // to call asBridge() from the destructor

    bool mInfiniteFarClip; // if true, frustum culling ignores far clip plane
    const bool mRenderByGroup;
    U32 mVertexDataMask;
    F32 mSlopRatio; //percentage distance must change before drawables receive LOD update (default is 0.25);
    bool mDepthMask; //if true, objects in this partition will be written to depth during alpha rendering
};

// class for creating bridges between spatial partitions
class LLSpatialBridge : public LLDrawable, public LLSpatialPartition
{
protected:
    ~LLSpatialBridge();

public:
    typedef std::vector<LLPointer<LLSpatialBridge> > bridge_vector_t;

    LLSpatialBridge(LLDrawable* root, bool render_by_group, U32 data_mask, LLViewerRegion* regionp);

    void destroyTree();

    virtual bool isSpatialBridge() const        { return true; }
    virtual void updateSpatialExtents();
    void exactWorldBox(LLVector4a& newMin, LLVector4a& newMax);      // <FS> stage C: what updateSpatialExtents made of the contents' bounds and the bridge's matrix
    bool sphereInsideBox(const F32* boxMin, const F32* boxMax, F32 tolerance);     // <FS> stage C: the bounding sphere of the contents (the octree root's bounds as they are, the bridge's matrix) lies inside the box: a few multiplications, safe at any rotation
    bool mConservative = false;                                      // <FS> stage C (LL_VK_ATTCONS): the bridge holds mConsMin / mConsMax (the avatar's box, world space) instead of its contents' exact box
    F32 mConsMin[3] = { 0.f, 0.f, 0.f }, mConsMax[3] = { 0.f, 0.f, 0.f };
    virtual void updateBinRadius();
    virtual void setVisible(LLCamera& camera_in, std::vector<LLDrawable*>* results = NULL, bool for_select = false);
    virtual void updateDistance(LLCamera& camera_in, bool force_update);
    virtual void makeActive();
    virtual void move(LLDrawable *drawablep, LLSpatialGroup *curp, bool immediate = false);
    virtual bool updateMove();
    void moveInPartition();     // <FS> view rework A1: the octree half of updateMove
    virtual void shiftPos(const LLVector4a& vec);
    virtual void cleanupReferences();
    virtual LLSpatialPartition* asPartition()       { return this; }

    //transform agent space camera into this Spatial Bridge's coordinate frame
    virtual LLCamera transformCamera(LLCamera& camera);

    //transform agent space bounding box into this Spatial Bridge's coordinate frame
    void transformExtents(const LLVector4a* src, LLVector4a* dst);
    LLDrawable* mDrawable;
};

class LLCullResult
{
public:
    LLCullResult();

    typedef std::vector<LLSpatialGroup*> sg_list_t;
    typedef std::vector<LLDrawable*> drawable_list_t;
    typedef std::vector<LLSpatialBridge*> bridge_list_t;

    typedef LLSpatialGroup** sg_iterator;
    typedef LLSpatialBridge** bridge_iterator;
    typedef LLDrawable** drawable_iterator;

    void clear();
    void clearRenderLists();       // J1 K6 B: the draw lists, the alpha group lists and what the render map job makes, not the visible groups (a serial shadow pass makes them again from the fragments)
    void clearLists();      // <FS> only the lists a cull fills

    sg_iterator beginVisibleGroups();
    sg_iterator endVisibleGroups();

    sg_iterator beginAlphaGroups();
    sg_iterator endAlphaGroups();

    sg_iterator beginRiggedAlphaGroups();
    sg_iterator endRiggedAlphaGroups();

    bool hasOcclusionGroups() { return mOcclusionGroupsSize > 0; }
    sg_iterator beginOcclusionGroups();
    sg_iterator endOcclusionGroups();

    sg_iterator beginDrawableGroups();
    sg_iterator endDrawableGroups();

    drawable_iterator beginVisibleList();
    drawable_iterator endVisibleList();

    bridge_iterator beginVisibleBridge();
    bridge_iterator endVisibleBridge();

    void pushVisibleGroup(LLSpatialGroup* group);
    // <FS> LL_VK_SHADOW_HULL: keeps, in order, the visible groups the predicate answers true for; returns how many it took out
    template <class Keep> U32 filterVisibleGroups(Keep keep)
    {
        U32 kept = 0;
        for (U32 i = 0; i < mVisibleGroupsSize; ++i)
        {
            LLSpatialGroup* group = mVisibleGroups[i];
            if (keep(group))
            {
                mVisibleGroups[kept++] = group;
            }
        }
        const U32 removed = mVisibleGroupsSize - kept;
        mVisibleGroupsSize = kept;
        mVisibleGroupsEnd = &mVisibleGroups[0] + kept;
        return removed;
    }
    void pushAlphaGroup(LLSpatialGroup* group);
    void pushRiggedAlphaGroup(LLSpatialGroup* group);
    void pushOcclusionGroup(LLSpatialGroup* group);
    void pushDrawableGroup(LLSpatialGroup* group);
    void pushDrawable(LLDrawable* drawable);
    void pushBridge(LLSpatialBridge* bridge);

    U32 getVisibleGroupsSize()      { return mVisibleGroupsSize; }
    U32 getAlphaGroupsSize()        { return mAlphaGroupsSize; }
    U32 getRiggedAlphaGroupsSize() { return mRiggedAlphaGroupsSize; }
    U32 getDrawableGroupsSize()     { return mDrawableGroupsSize; }
    U32 getVisibleListSize()        { return mVisibleListSize; }
    U32 getVisibleBridgeSize()      { return mVisibleBridgeSize; }

    // <FS> S2: the visible groups whose static shadow casters are not resident on the worker (no current resident fragment): the only ones the main thread still names per pass.
    // Filled by the shadow render map job (shadowPrepRenderMap) when LL_VK_SCENE_RESIDENT=1.
    std::vector<LLSpatialGroup*> mSceneGroups;
    bool mAlphaResSkipped = false;      // <FS> J1 K3c: the render map job left out the alpha casters the worker holds as resident rows (the viewer's filter for them has nothing to drop)
    bool mRigAlphaOn = false;           // <FS> J1 K3d: the render map job named the rigged alpha masked casters of the groups' fragments in mRigSources too (their draw infos are not in the lists, mode 1)
    bool mRigFromFragments = false;     // <FS> J1 K3a: the cascade's render map job made this result and named the rigged casters of the groups' fragments in mRigSources (a result made by another path has none: its lists hold every rigged draw)
    std::vector<std::pair<U32, const GeoRigSource*>> mRigSources;       // <FS> J1 K3a: a cascade's rigged casters that its group's fragment names (pass type of the rigged variant, the item's source), in the order the draw infos would have been pushed; the skin builder reads them
    std::vector<LLSpatialGroup*> mAlphaFragGroups;     // <FS> C5 J3: a cascade's groups whose alpha casters the render map job left out because the group's fragment names them and the worker does not hold them yet (renderShadow makes their entries from the fragments)
    // <FS> C4.4: the visible groups of a shadow cascade's cull whose changeLOD() answered true when the cull walk stamped them (the row was hot there), in the walk's order: the only ones the mark job
    // (shadowPrepMark) still has to look at (a few dozen of ~1800 a frame). Filled by LLPipeline::markNotCulled for the cascade culls; cleared with the lists.
    std::vector<LLSpatialGroup*> mLodGroups;
    bool mGeoElided = false;                            // S3 G4: postSort left the table's draws out of this cull result's render map (the passes must be prepared)
    std::vector<U32> mGeoHandles;                       // S3 G3 (LL_VK_SCENE_GEO=2 serving): the scene geometry handles of the groups that passed postSort's filters: the list the worker draws from

private:

    template <class T, class V> void pushBack(T &head, U32& count, V* val);

    U32                 mVisibleGroupsSize;
    U32                 mAlphaGroupsSize;
    U32                 mRiggedAlphaGroupsSize;
    U32                 mOcclusionGroupsSize;
    U32                 mDrawableGroupsSize;
    U32                 mVisibleListSize;
    U32                 mVisibleBridgeSize;

    U32                 mVisibleGroupsAllocated;
    U32                 mAlphaGroupsAllocated;
    U32                 mRiggedAlphaGroupsAllocated;
    U32                 mOcclusionGroupsAllocated;
    U32                 mDrawableGroupsAllocated;
    U32                 mVisibleListAllocated;
    U32                 mVisibleBridgeAllocated;

    sg_list_t           mVisibleGroups;
    sg_iterator         mVisibleGroupsEnd;
    sg_list_t           mAlphaGroups;
    sg_iterator         mAlphaGroupsEnd;
    sg_list_t           mRiggedAlphaGroups;
    sg_iterator         mRiggedAlphaGroupsEnd;
    sg_list_t           mOcclusionGroups;
    sg_iterator         mOcclusionGroupsEnd;
    sg_list_t           mDrawableGroups;
    sg_iterator         mDrawableGroupsEnd;
    drawable_list_t     mVisibleList;
    drawable_iterator   mVisibleListEnd;
    bridge_list_t       mVisibleBridge;
    bridge_iterator     mVisibleBridgeEnd;

};


//spatial partition for water (implemented in LLVOWater.cpp)
class LLWaterPartition : public LLSpatialPartition
{
public:
    LLWaterPartition(LLViewerRegion* regionp);
    virtual void getGeometry(LLSpatialGroup* group) {  }
    virtual void addGeometryCount(LLSpatialGroup* group, U32 &vertex_count, U32& index_count) { }
};

//spatial partition for hole and edge water (implemented in LLVOWater.cpp)
class LLVoidWaterPartition : public LLWaterPartition
{
public:
    LLVoidWaterPartition(LLViewerRegion* regionp);
};

//spatial partition for terrain (impelmented in LLVOSurfacePatch.cpp)
class LLTerrainPartition : public LLSpatialPartition
{
public:
    LLTerrainPartition(LLViewerRegion* regionp);
    virtual void getGeometry(LLSpatialGroup* group);
};

//spatial partition for trees
class LLTreePartition : public LLSpatialPartition
{
public:
    LLTreePartition(LLViewerRegion* regionp);
    virtual void getGeometry(LLSpatialGroup* group) { }
    virtual void addGeometryCount(LLSpatialGroup* group, U32 &vertex_count, U32& index_count) { }

};

//spatial partition for particles (implemented in LLVOPartGroup.cpp)
class LLParticlePartition : public LLSpatialPartition
{
public:
    LLParticlePartition(LLViewerRegion* regionp);
    virtual void rebuildGeom(LLSpatialGroup* group);
    virtual void getGeometry(LLSpatialGroup* group);
    virtual void addGeometryCount(LLSpatialGroup* group, U32 &vertex_count, U32& index_count);
    virtual F32 calcPixelArea(LLSpatialGroup* group, LLCamera& camera);
protected:
    bool rebuildRows(LLSpatialGroup* group);        // <FS> GPU particles stage 1: one row a particle for the pull shaders (false: the group needs the quads, nothing was changed)
    U32 mRenderPass;
};

class LLHUDParticlePartition : public LLParticlePartition
{
public:
    LLHUDParticlePartition(LLViewerRegion* regionp);
};

//spatial partition for grass (implemented in LLVOGrass.cpp)
class LLGrassPartition : public LLSpatialPartition
{
public:
    LLGrassPartition(LLViewerRegion* regionp);
    virtual void getGeometry(LLSpatialGroup* group);
    virtual void addGeometryCount(LLSpatialGroup* group, U32 &vertex_count, U32& index_count);
protected:
    U32 mRenderPass;
};

//class for wrangling geometry out of volumes (implemented in LLVOVolume.cpp)
class LLVolumeGeometryManager: public LLGeometryManager
{
 public:
    typedef enum
    {
        NONE = 0,
        BATCH_SORT,
        DISTANCE_SORT
    } eSortType;

    LLVolumeGeometryManager();
    virtual ~LLVolumeGeometryManager();
    virtual void rebuildGeom(LLSpatialGroup* group);
    virtual void rebuildMesh(LLSpatialGroup* group);
    virtual void getGeometry(LLSpatialGroup* group);
    virtual void addGeometryCount(LLSpatialGroup* group, U32& vertex_count, U32& index_count);
    // <FS> group rebuild R3b2: the fill and registration of one render batch as a task of its own. genDrawInfo plans the batches of a kind and allocates their buffers (the main thread), and either runs the
    // tasks at once or hands them to the caller (deferred), which runs the tasks of all the kinds of a group together, on the pool when LL_VK_REBUILD_PARALLEL is set, and appends their draw maps to the group's in order.
    struct FillTask
    {
        LLSpatialGroup* mGroup = nullptr;
        U32 mMask = 0;
        bool mHudGroup = false;
        bool mBatchTextures = false;
        bool mDistanceSort = false;
        bool mBakeSunlight = false;
        bool mPrepared = false;         // the caller did the faces' own setup (indices, vertex buffer, texture stamps) already: the fill runs on a pool thread
        bool mSerialOnly = false;       // a face of an animated child: updateRelativeXform writes its object, which the faces of other batches can share
        LLFace** mBegin = nullptr;
        LLFace** mEnd = nullptr;
        std::vector<LLFace*> mFaces;    // the faces when the task waits for a batch (R3b3): the scratch the face lists were sorted into is the next group's
        LLPointer<LLVertexBuffer> mBuffer;
        LLSpatialGroup::draw_map_t mMap;
    };
    void prepareBatch(FillTask& task);
    void fillBatch(FillTask& task);
    void runFillTasks(LLSpatialGroup* group, std::vector<FillTask>& tasks);
    U32 genDrawInfo(LLSpatialGroup* group, U32 mask, LLFace** faces, U32 face_count, bool distance_sort = false, bool batch_textures = false, bool rigged = false, std::vector<FillTask>* deferred = nullptr);
    // drawmap: the draw map to register into (default: the group's own). The fill of a render batch registers into one of its own that is appended to the group's when the batch is done (R3b1: a draw info only ever grows with faces of its own vertex buffer, which is its own batch)
    void registerFace(LLSpatialGroup* group, LLFace* facep, U32 type, LLSpatialGroup::draw_map_t* drawmap = nullptr);

private:
    static void allocateFaces(U32 pMaxFaceCount);
    static void freeFaces();
    static void ensureFaces();      // <FS> group rebuild R1: this thread's scratch, made on first use

    static int32_t sInstanceCount;
    static thread_local LLFace** sFullbrightFaces[2];      // <FS> group rebuild R1: the scratch of one rebuild, one set per thread (the rebuilds of two groups can then run at once)
    static thread_local LLFace** sBumpFaces[2];
    static thread_local LLFace** sSimpleFaces[2];
    static thread_local LLFace** sNormFaces[2];
    static thread_local LLFace** sSpecFaces[2];
    static thread_local LLFace** sNormSpecFaces[2];
    static thread_local LLFace** sPbrFaces[2];
    static thread_local LLFace** sAlphaFaces[2];
};

// <FS> group rebuild R3b3: the fills of the groups a loop rebuilds, launched on the pool once instead of once a group (LL_VK_REBUILD_PARALLEL=2). While a scope is open, a volume group's rebuild plans its batches and allocates
// their buffers as it always did, and hands the fill tasks to the batch; the group's state is as after a rebuild at once, but its draw map and the scene fragment wait for flush(), which the scope's owner calls after the loop
// (and which a rebuild or a mesh rebuild of a pending group calls first). Main thread only.
class LLRebuildBatch
{
public:
    struct Scope;
    static LLRebuildBatch* current() { return sCurrent; }
    bool pending(const LLSpatialGroup* group) const { return mIndex.find(group) != mIndex.end(); }
    void add(LLVolumeGeometryManager* manager, LLSpatialGroup* group, std::vector<LLVolumeGeometryManager::FillTask>& tasks);
    void fragmentLater(LLSpatialGroup* group);
    void flush();
    LLSpatialGroup* mJustDeferred = nullptr;    // the group whose rebuild was just handed over: LLSpatialGroup::rebuildGeom builds its fragment later
private:
    struct Entry
    {
        LLVolumeGeometryManager* mManager = nullptr;
        LLSpatialGroup* mGroup = nullptr;
        std::vector<LLVolumeGeometryManager::FillTask> mTasks;
        bool mFragment = false;
    };
    std::vector<Entry> mEntries;
    std::unordered_map<const LLSpatialGroup*, size_t> mIndex;
    static thread_local LLRebuildBatch* sCurrent;
};

struct LLRebuildBatch::Scope
{
    LLRebuildBatch mBatch;
    bool mOwner;
    Scope();
    ~Scope();
    void flush() { if (mOwner) mBatch.flush(); }
};

//spatial partition that uses volume geometry manager (implemented in LLVOVolume.cpp)
class LLVolumePartition : public LLSpatialPartition, public LLVolumeGeometryManager
{
public:
    LLVolumePartition(LLViewerRegion* regionp);
    virtual void rebuildGeom(LLSpatialGroup* group) { LLVolumeGeometryManager::rebuildGeom(group); }
    virtual void getGeometry(LLSpatialGroup* group) { LLVolumeGeometryManager::getGeometry(group); }
    virtual void rebuildMesh(LLSpatialGroup* group) { LLVolumeGeometryManager::rebuildMesh(group); }
    virtual void addGeometryCount(LLSpatialGroup* group, U32 &vertex_count, U32& index_count) { LLVolumeGeometryManager::addGeometryCount(group, vertex_count, index_count); }
};

//spatial bridge that uses volume geometry manager (implemented in LLVOVolume.cpp)
class LLVolumeBridge : public LLSpatialBridge, public LLVolumeGeometryManager
{
public:
    LLVolumeBridge(LLDrawable* drawable, LLViewerRegion* regionp);
    virtual void rebuildGeom(LLSpatialGroup* group) { LLVolumeGeometryManager::rebuildGeom(group); }
    virtual void getGeometry(LLSpatialGroup* group) { LLVolumeGeometryManager::getGeometry(group); }
    virtual void rebuildMesh(LLSpatialGroup* group) { LLVolumeGeometryManager::rebuildMesh(group); }
    virtual void addGeometryCount(LLSpatialGroup* group, U32 &vertex_count, U32& index_count) { LLVolumeGeometryManager::addGeometryCount(group, vertex_count, index_count); }
};

class LLAvatarBridge : public LLVolumeBridge
{
public:
    LLAvatarBridge(LLDrawable* drawablep, LLViewerRegion* regionp);
};

class LLControlAVBridge : public LLVolumeBridge
{
    using super = LLVolumeBridge;
public:
    LLControlAVBridge(LLDrawable* drawablep, LLViewerRegion* regionp);
};

class LLHUDBridge : public LLVolumeBridge
{
public:
    LLHUDBridge(LLDrawable* drawablep, LLViewerRegion* regionp);
    virtual void shiftPos(const LLVector4a& vec);
    virtual F32 calcPixelArea(LLSpatialGroup* group, LLCamera& camera);
};

//spatial partition that holds nothing but spatial bridges
class LLBridgePartition : public LLSpatialPartition
{
public:
    LLBridgePartition(LLViewerRegion* regionp);
    virtual void getGeometry(LLSpatialGroup* group) { }
    virtual void addGeometryCount(LLSpatialGroup* group, U32 &vertex_count, U32& index_count) {  }
};

class LLAvatarPartition : public LLBridgePartition
{
public:
    LLAvatarPartition(LLViewerRegion* regionp);
};

class LLControlAVPartition : public LLBridgePartition
{
public:
    LLControlAVPartition(LLViewerRegion* regionp);
};

class LLHUDPartition : public LLBridgePartition
{
public:
    LLHUDPartition(LLViewerRegion* regionp);
    virtual void shift(const LLVector4a &offset);
};

extern const F32 SG_BOX_SIDE;
extern const F32 SG_BOX_OFFSET;
extern const F32 SG_BOX_RAD;

extern const F32 SG_OBJ_SIDE;
extern const F32 SG_MAX_OBJ_RAD;



// <FS> C4.5 S1b: the retained set of the main view's cull (llspatialpartition.cpp): update applies the events since the last frame (all partitions are rebuilt when the camera changed), verify compares the result with a
// full collecting walk of every partition and logs each difference
namespace LLVisSet
{
    // set 0 is the main view's, 1 to 4 are the sun cascades' (C4.5 R2: the cascades' cull replays a retained set the same way)
    void update(LLCamera& camera, const std::vector<LLSpatialPartition*>& parts, bool camera_changed, bool will_replay, int set = 0);
    void verify(LLCamera& camera, const std::vector<LLSpatialPartition*>& parts);
    bool replayable(LLSpatialPartition* part, int set = 0);                  // the partition's retained set is current for this frame: the cull replays it instead of walking
    size_t replayCount(LLSpatialPartition* part, int set = 0);               // the entries of its replay list
    void replay(LLSpatialPartition* part, LLCamera& camera, size_t begin, size_t end, int set = 0);      // what the walk would have done to the groups at [begin, end) of the replay list, in order (a cull job: runs under the job's scope)
    void finishFrame();                // after the cull of the frame: the groups that joined a set this frame get its slot (on a frame the sets claimed this was done by the update)
    U32 checkStamps(S32 frame, int set = 0);
    void checkOrder(LLCamera& camera, const std::vector<LLSpatialPartition*>& parts, int set);       // diagnostic: the retained order of every valid set against a collecting walk; the first differences are logged with the events of the groups concerned        // after a frame whose real walk also ran: the members' stored stamps (written by the walk) against the frame the replay claimed; returns the mismatches
}

#endif //LL_LLSPATIALPARTITION_H