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
void ll_scene_fragment_free(LLSceneFragment*& fragment);        // pipeline.cpp (S1)
class LLSpatialGroup;
void ll_scene_fragment_build(LLSpatialGroup* group);
class LLCullResult;
bool ll_scene_geo_on();                                         // pipeline.cpp (S3): LL_VK_SCENE_GEO=1 or 2
bool ll_scene_geo_verify_on();                                  // ... with LL_VK_SCENE_VERIFY=1
bool ll_scene_geo_serving();                                    // LL_VK_SCENE_GEO=2 and the table draws this frame (LL_VK_SCENE_GEO_AB=<seconds> alternates it)
bool ll_scene_geo_skip();                                       // the deferred geometry builders leave out the draws the table serves (serving, and not verifying)
bool ll_scene_geo_main_view();                                  // the camera / render flags of the main view (not a probe, an impostor, a shadow or a reflection render)
bool ll_scene_geo_prepared(LLCullResult* cull);                 // the table was told this frame's visible fragments for this cull result
int ll_scene_geo_type_number(U32 type);                         // the pass type's number in the table's order (-1: not a table type)
void ll_scene_geo_palette_kick(LLCullResult* cull);               // S4 B: end of postSort: the palettes of the rigged items of the listed fragments are made on the pool (ll_scene_geo_frame takes them)
void ll_scene_geo_frame(LLCullResult* cull);                    // main view, once a frame before the passes
void ll_scene_geo_frame_end();                                  // main view, after the passes: releases of fragments that went during the frame
void ll_scene_geo_count(U32 what, U64 n);                       // 0 builder draws left out, 1 table draw records sent, 2 builds made again for want of a prepare            // pipeline.cpp (S2): after a group's rebuild

// <FS> S5a-1 (doc/vulkan/S5-design.md section 8): the plan of a group's static alpha draws, kept by its fragment (LL_VK_SCENE_ALPHA=1): the draw infos the alpha loop draws, in its order, as segments of
// draws a table can serve (mServed, equal mKey: one run) and draws it cannot (the old path draws them in place)
struct LLAlphaPlanSeg { U32 mFirst, mCount, mKey; bool mServed; U32 mRank; };      // mRank: served segments: the number of the first item among the fragment's alpha items
struct LLAlphaPlanView
{
    const class LLDrawInfo* const* mOrder = nullptr;
    U32 mOrderCount = 0;
    const LLAlphaPlanSeg* mSegs = nullptr;
    U32 mSegCount = 0;
    U32 mHandle = 0;            // the fragment's handle at the worker (0: not attached, nothing is served)
    bool mDirect = false;       // the draw map holds exactly these draw infos in this order (no draw of an avatar among them): a segment can be stepped over by its draw count
    const U32* mGlowIdx = nullptr;      // the plan indices of the served draws that have a glow buffer, ascending
    const class LLDrawInfo* const* mItemInfo = nullptr;     // S5d: per served item (by rank): its draw info and its number in the fragment's wire list (the palette bindings of the rigged items name it)
    const U32* mItemWire = nullptr;
    U32 mGlowCount = 0;
    bool mGlow = false;         // a draw of the group has an emissive buffer (the loop draws a glow pass after the group, post water)
};
int ll_alpha_plan(const LLSpatialGroup* group, LLAlphaPlanView& out, bool rigged = false);
bool ll_alpha_blend_pair(U32 id, int* src, int* dst);       // pipeline.cpp (S5b): a run key carries the id of its blend function pair in bits 8 and up (0 = the standard alpha blend)
bool ll_alpha_glow_table(bool pbr);      // pipeline.cpp (S5e): the glow second draw of the alpha items is drawn from the table (plain / glTF items)
bool ll_alpha_ab_on();           // pipeline.cpp (S5f F4): the alpha pull programs draw this frame in any view (transient records), not only the main view's fragments (LL_VK_SCENE_ALPHA=1 and the AB phase)
bool ll_alpha_serving();      // pipeline.cpp (S5a): the static main-view alpha pass is drawn from the scene table this frame      // pipeline.cpp: 0 no plan, 1 the plan (current), 2 the group's fragment is not current

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


    void validate();

    // return mSkinHash->mHash, or 0 if mSkinHash is null
    U64 getSkinHash();

    LLPointer<LLVertexBuffer> mVertexBuffer;
    U16 mStart = 0;
    U16 mEnd = 0;
    U32 mCount = 0;
    U32 mOffset = 0;

    LLPointer<LLViewerTexture>     mTexture;
    LLPointer<LLViewerTexture> mSpecularMap;
    LLPointer<LLViewerTexture> mNormalMap;

    const LLMatrix4* mSpecularMapMatrix = nullptr;
    const LLMatrix4* mNormalMapMatrix = nullptr;
    const LLMatrix4* mTextureMatrix = nullptr;
    const LLMatrix4* mModelMatrix = nullptr;

    LLPointer<LLVOAvatar> mAvatar = nullptr;
    LLConstPointer<LLMeshSkinInfo> mSkinInfo;// <FS:Beq/> be defensive about UAF with skinInfo during LocalMesh

    // Material pointer here is likely for debugging only and are immaterial (zing!)
    LLPointer<LLMaterial> mMaterial;

    // PBR material parameters
    LLPointer<LLFetchedGLTFMaterial> mGLTFMaterial;

    LLVector4 mSpecColor = LLVector4(1.f, 1.f, 1.f, 0.5f); // XYZ = Specular RGB, W = Specular Exponent

    std::vector<LLPointer<LLViewerTexture> > mTextureList;

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
    // <FS> S7 B0 (doc/vulkan/S7-design.md section 13): the union of the extents of the faces this draw covers (agent space for a static drawable, the drawable's own space for an active one: the model
    // matrix maps it); only filled for volume faces while LL_VK_VISCENSUS is on (LLVolumeGeometryManager::registerFace)
    LLVector4a mExtents[2];
    bool mHasExtents = false;

    // <FS> ray scene: the draw's material in the viewer's registry (pipeline.cpp, rt_material_id): the row in the low 32 bits, its generation in the high 32 (0 = not looked up yet).
    // Atomic: the packet builders of several cascades run in parallel and meet the same draw.
    std::atomic<U64> mRtMaterial{0};

    struct CompareTexture
    {
        bool operator()(const LLDrawInfo& lhs, const LLDrawInfo& rhs)
        {
            return lhs.mTexture > rhs.mTexture;
        }
    };

    struct CompareTexturePtr
    { //sort by texture
        bool operator()(const LLPointer<LLDrawInfo>& lhs, const LLPointer<LLDrawInfo>& rhs)
        {
            // sort by pointer, sort NULL down to the end
            return lhs.get() != rhs.get()
                        && (lhs.isNull() || (rhs.notNull() && lhs->mTexture.get() > rhs->mTexture.get()));
        }
    };

    struct CompareVertexBuffer
    { //sort by texture
        bool operator()(const LLPointer<LLDrawInfo>& lhs, const LLPointer<LLDrawInfo>& rhs)
        {
            // sort by pointer, sort NULL down to the end
            return lhs.get() != rhs.get()
                    && (lhs.isNull() || (rhs.notNull() && lhs->mVertexBuffer.get() > rhs->mVertexBuffer.get()));
        }
    };

    struct CompareTexturePtrMatrix
    {
        bool operator()(const LLPointer<LLDrawInfo>& lhs, const LLPointer<LLDrawInfo>& rhs)
        {
            return lhs.get() != rhs.get()
                        && (lhs.isNull() || (rhs.notNull() && (lhs->mTexture.get() > rhs->mTexture.get() ||
                                                               (lhs->mTexture.get() == rhs->mTexture.get() && lhs->mModelMatrix > rhs->mModelMatrix))));
        }

    };

    struct CompareMatrixTexturePtr
    {
        bool operator()(const LLPointer<LLDrawInfo>& lhs, const LLPointer<LLDrawInfo>& rhs)
        {
            return lhs.get() != rhs.get()
                && (lhs.isNull() || (rhs.notNull() && (lhs->mModelMatrix > rhs->mModelMatrix ||
                                                       (lhs->mModelMatrix == rhs->mModelMatrix && lhs->mTexture.get() > rhs->mTexture.get()))));
        }

    };

    struct CompareBump
    {
        bool operator()(const LLPointer<LLDrawInfo>& lhs, const LLPointer<LLDrawInfo>& rhs)
        {
            // sort by mBump value, sort NULL down to the end
            return lhs.get() != rhs.get()
                        && (lhs.isNull() || (rhs.notNull() && lhs->mBump > rhs->mBump));
        }
    };
};

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
            return lhs->mDistance > rhs->mDistance;
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
            return lhs->mDepth > rhs->mDepth;
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
        STATE_MASK              = 0x0000FFFF,
    } eSpatialState;

    LLSpatialGroup(OctreeNode* node, LLSpatialPartition* part);

    bool isHUDGroup() const;

    void clearDrawMap();
    void validate();
    void validateDrawMap();

    void setState(U32 state, S32 mode);
    void clearState(U32 state, S32 mode);
    void clearState(U32 state)     {mState &= ~state;}

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

    void setState(U32 state)       {mState |= state;}
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
    LL_ALIGN_16(LLVector4a mViewAngle);
    LL_ALIGN_16(LLVector4a mLastUpdateViewAngle);

protected:
    virtual ~LLSpatialGroup();

public:
    LLPointer<LLVertexBuffer> mVertexBuffer;
    draw_map_t mDrawMap;
    // <FS> S1 phase 0 (doc/vulkan/PLAN-retained-records.md): bumped whenever the draw infos are cleared (every creation of draw infos follows a clear), so the same serial on two frames
    // means the same draw info set. mSceneSeenSerial: what the main view saw last (LL_VK_SCENE_STATS counters only).
    U32 mDrawMapSerial = 1;
    U32 mSceneSeenSerial = 0;
    U32 mSceneSeenFrame = 0;
    // <FS> S1: the retained shadow fragment of this group (pipeline.cpp), valid while its serial is mDrawMapSerial; freed with the group
    struct LLSceneFragment* mSceneFragment = nullptr;
    U64 mGeoServedMask = 0;             // <FS> S3 G4 (S4 B: the bits after the static types are the rigged variants): the bits (kGeoTypes order) of the pass types whose draw infos in this group are ALL drawn by the scene table: postSort does not list those
    U32 mGeoServedTris = 0;             // ... their index count (the triangle statistic)
    bool mGeoAllServed = false;         // <FS> R2 (S-retire-views-design.md): every non-empty draw map type of the group is served by the table: postSort has nothing to put in the render map for it
    U32 mGeoHandle = 0;                 // <FS> S3 G3: the worker's handle of this group's scene geometry fragment (0 = none, or the table does not hold it): what postSort lists for the passes
    static U32 sDrawMapsCleared;        // non empty draw maps cleared (group rebuilds), counted per frame by LL_VK_SCENE_STATS

    bridge_list_t mBridgeList;
    buffer_map_t mBufferMap; //used by volume buffers to attempt to reuse vertex buffers

    F32 mObjectBoxSize; //cached mObjectBounds[1].getLength3()
    U32 mGeometryBytes; //used by volumes to track how many bytes of geometry data are in this node
    F32 mSurfaceArea; //used by volumes to track estimated surface area of geometry in this node
    F32 mBuilt;

    F32 mDistance;
    F32 mDepth;
    F32 mLastUpdateDistance;
    F32 mLastUpdateTime;

    F32 mPixelArea;
    F32 mRadius;

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
    virtual void updateBinRadius();
    virtual void setVisible(LLCamera& camera_in, std::vector<LLDrawable*>* results = NULL, bool for_select = false);
    virtual void updateDistance(LLCamera& camera_in, bool force_update);
    virtual void makeActive();
    virtual void move(LLDrawable *drawablep, LLSpatialGroup *curp, bool immediate = false);
    virtual bool updateMove();
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
    typedef std::vector<LLDrawInfo*> drawinfo_list_t;

    typedef LLSpatialGroup** sg_iterator;
    typedef LLSpatialBridge** bridge_iterator;
    typedef LLDrawInfo** drawinfo_iterator;
    typedef LLDrawable** drawable_iterator;

    // Helper function for taking advantage of _mm_prefetch when iterating over cull results
    static inline void increment_iterator(LLCullResult::drawinfo_iterator& i, const LLCullResult::drawinfo_iterator& end)
    {
        ++i;

        if (i != end)
        {
            _mm_prefetch((char*)(*i)->mVertexBuffer.get(), _MM_HINT_NTA);

            auto* ni = i + 1;
            if (ni != end)
            {
                _mm_prefetch((char*)*ni, _MM_HINT_NTA);
            }
        }
    }

    void clear();

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

    drawinfo_iterator beginRenderMap(U32 type);
    drawinfo_iterator endRenderMap(U32 type);

    void pushVisibleGroup(LLSpatialGroup* group);
    void pushAlphaGroup(LLSpatialGroup* group);
    void pushRiggedAlphaGroup(LLSpatialGroup* group);
    void pushOcclusionGroup(LLSpatialGroup* group);
    void pushDrawableGroup(LLSpatialGroup* group);
    void pushDrawable(LLDrawable* drawable);
    void pushBridge(LLSpatialBridge* bridge);
    void pushDrawInfo(U32 type, LLDrawInfo* draw_info);

    U32 getVisibleGroupsSize()      { return mVisibleGroupsSize; }
    U32 getAlphaGroupsSize()        { return mAlphaGroupsSize; }
    U32 getRiggedAlphaGroupsSize() { return mRiggedAlphaGroupsSize; }
    U32 getDrawableGroupsSize()     { return mDrawableGroupsSize; }
    U32 getVisibleListSize()        { return mVisibleListSize; }
    U32 getVisibleBridgeSize()      { return mVisibleBridgeSize; }
    U32 getRenderMapSize(U32 type)  { return mRenderMapSize[type]; }

    void assertDrawMapsEmpty();

    // <FS> S2: the visible groups whose static shadow casters are not resident on the worker (no current resident fragment): the only ones the main thread still names per pass.
    // Filled by the shadow render map job (shadowPrepRenderMap) when LL_VK_SCENE_RESIDENT=1.
    std::vector<LLSpatialGroup*> mSceneGroups;
    // <FS> S3: the visible groups that passed postSort's filters (not dead, not occluded, not auto-hidden), after their rebuild: the groups whose deferred geometry the scene tables draw
    // (LL_VK_SCENE_GEO=1). Filled by postSort.
    std::vector<LLSpatialGroup*> mGeoGroups;
    std::vector<U32> mGeoSerials;                       // their mDrawMapSerial when postSort listed them
    bool mGeoElided = false;                            // S3 G4: postSort left the table's draws out of this cull result's render map (the passes must be prepared)
    std::vector<U32> mGeoHandles;                       // S3 G3 (LL_VK_SCENE_GEO=2 serving): the scene geometry handles of the groups that passed postSort's filters: the list the worker draws from
    // verify only (LL_VK_SCENE_GEO with LL_VK_SCENE_VERIFY=1): who pushed each draw info into the render map: the group, its mDrawMapSerial then, and where (1 postSort, 2 the shadow render map job)
    struct GeoOwner { LLSpatialGroup* mGroup; U32 mSerial; U8 mSource; };
    std::unordered_map<const LLDrawInfo*, GeoOwner> mGeoOwner;

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

    U32                 mRenderMapSize[LLRenderPass::NUM_RENDER_TYPES];

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
    drawinfo_list_t     mRenderMap[LLRenderPass::NUM_RENDER_TYPES];
    U32                 mRenderMapAllocated[LLRenderPass::NUM_RENDER_TYPES];
    drawinfo_iterator mRenderMapEnd[LLRenderPass::NUM_RENDER_TYPES];

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
    U32 genDrawInfo(LLSpatialGroup* group, U32 mask, LLFace** faces, U32 face_count, bool distance_sort = false, bool batch_textures = false, bool rigged = false);
    void registerFace(LLSpatialGroup* group, LLFace* facep, U32 type);

private:
    void allocateFaces(U32 pMaxFaceCount);
    void freeFaces();

    static int32_t sInstanceCount;
    static LLFace** sFullbrightFaces[2];
    static LLFace** sBumpFaces[2];
    static LLFace** sSimpleFaces[2];
    static LLFace** sNormFaces[2];
    static LLFace** sSpecFaces[2];
    static LLFace** sNormSpecFaces[2];
    static LLFace** sPbrFaces[2];
    static LLFace** sAlphaFaces[2];
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


#endif //LL_LLSPATIALPARTITION_H