/**
 * @file lldrawpool.h
 * @brief LLDrawPool class definition
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
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

#ifndef LL_LLDRAWPOOL_H
#define LL_LLDRAWPOOL_H

#include "v4coloru.h"
#include "v2math.h"
#include "v3math.h"
#include "llvertexbuffer.h"
#include <unordered_set>

class LLFace;
class LLViewerTexture;
class LLViewerFetchedTexture;
class LLSpatialGroup;
class LLDrawBatch;
class LLVOAvatar;
class LLShaderProgram;
class LLMeshSkinInfo;

class LLDrawPool
{
public:
    static S32 sNumDrawPools;

    enum
    {
        // Correspond to LLPipeline render type
        // Also controls render order, so passes that don't use alpha masking/blending should come before
        // other passes to preserve hierarchical Z for occlusion queries.  Occlusion queries happen just
        // before grass, so grass should be the first alpha masked pool.  Other ordering should be done
        // based on fill rate and likelihood to occlude future passes (faster, large occluders first).
        //
        POOL_SKY = 1,
        POOL_WATEREXCLUSION,
        POOL_WL_SKY,
        POOL_SIMPLE,
        POOL_FULLBRIGHT,
        POOL_BUMP,
        POOL_MATERIALS,
        POOL_GLTF_PBR,
        POOL_TERRAIN,
        POOL_GRASS,
        POOL_GLTF_PBR_ALPHA_MASK,
        POOL_TREE,
        POOL_ALPHA_MASK,
        POOL_FULLBRIGHT_ALPHA_MASK,
        POOL_AVATAR,
        POOL_CONTROL_AV, // Animesh
        POOL_GLOW,
        POOL_ALPHA_PRE_WATER,
        POOL_VOIDWATER,
        POOL_WATER,
        POOL_ALPHA_POST_WATER,
        POOL_ALPHA, // note there is no actual "POOL_ALPHA" but pre-water and post-water pools consume POOL_ALPHA faces
        NUM_POOL_TYPES,
    };

    LLDrawPool(const U32 type);
    virtual ~LLDrawPool();

    virtual bool isDead() = 0;

    S32 getId() const { return mId; }
    U32 getType() const { return mType; }

    bool getSkipRenderFlag() const { return mSkipRender;}
    void setSkipRenderFlag( bool flag ) { mSkipRender = flag; }

    virtual LLViewerTexture *getDebugTexture();
    virtual void beginRenderPass( S32 pass );
    virtual void endRenderPass( S32 pass );
    virtual S32  getNumPasses();

    virtual void beginDeferredPass(S32 pass);
    virtual void endDeferredPass(S32 pass);
    virtual S32 getNumDeferredPasses();
    virtual void renderDeferred(S32 pass = 0);

    virtual void beginPostDeferredPass(S32 pass);
    virtual void endPostDeferredPass(S32 pass);
    virtual S32 getNumPostDeferredPasses();
    virtual void renderPostDeferred(S32 pass = 0);

    virtual void beginShadowPass(S32 pass);
    virtual void endShadowPass(S32 pass);
    virtual S32 getNumShadowPasses();
    virtual void renderShadow(S32 pass = 0);

    virtual void render(S32 pass = 0) {};
    virtual void prerender() {};
    virtual U32 getVertexDataMask() { return 0; } // DEPRECATED -- draw pool doesn't actually determine vertex data mask any more
    virtual bool verify() const { return true; }        // Verify that all data in the draw pool is correct!
    virtual S32 getShaderLevel() const { return mShaderLevel; }

    static LLDrawPool* createPool(const U32 type, LLViewerTexture *tex0 = NULL);
    virtual LLViewerTexture* getTexture() = 0;
    virtual bool isFacePool() { return false; }
    virtual void resetDrawOrders() = 0;
    virtual void pushFacesPull(bool region_matrix) {}       // S6.6 (LLFacePool)

    S32 mShaderLevel;
    S32 mId;
    U32 mType;              // Type of draw pool
    bool mSkipRender;
};

// <FS> what a materials pass adds to a geometry record: which maps its draws carry
class LLCullResult;
namespace LLVkBridge { struct GeoEntry; }

struct LLGeometryPassInfo
{
    bool mNormalMap = false;
    bool mSpecularMap = false;
};

// <FS> S4 A1: the rigged draws of the diffuse and the materials passes as geometry records (kind 25, skinned pull programs): whether the skinned diffuse pull program and the packet path exist
bool ll_rigged_packets_enabled();
enum { LL_PASS_ARRAY_NORMALS = 1, LL_PASS_ARRAY_EMISSIVE = 2, LL_PASS_ARRAY_BUMP = 4, LL_PASS_ARRAY_EMBOSS = 8 };       // (J1 K4: BUMP: the tangent array and the draw's bump image, one texture)
U32 ll_pass_arrays(U32 type);       // S6 X1 / X2: the arrays a pass type needs besides position, texcoord 0, colour (fullbright: none, glow: emissive)
int ll_geometry_reject_reason(const LLDrawBatch& p, bool batch_textures, const LLGeometryPassInfo* materials, bool rigged, bool texture_matrix, U32 arrays = 1);       // S6 census: why a draw is refused by the table / packets (0: it is not)
// <FS> S4 A2: the frame's shared palette list: a palette of an (avatar, skin) is added once per frame (12 floats per joint) and named by its absolute float offset by every record that needs it (rigged geometry records,
// rigged shadow records); the new part of the list is sent (kind 27) by ll_frame_palette_flush() before a record that names it. Thread safe (the builders run on the pool). False when the old path draws nothing either.
bool ll_frame_palette(LLVOAvatar* avatar, const LLMeshSkinInfo* skin, U32& offset, U32& joints);
void ll_frame_palette_flush();
void ll_rig_census_add(int what, U64 n);          // <FS> P3 census (LL_VK_RIGCENSUS=1): rigged draws by consumer, per frame (see ll_rig_census_log)
void ll_rig_census_log();
bool ll_palette_ids_on();                        // <FS> P1 step 3: records name shared palettes by id (LL_VK_PALID, default on)
U32 ll_palette_id_alloc();                       // <FS> P1 step 3: palette ids (see lldrawpool.cpp); a palette entry owns one from its first write into a frame's list until it is destroyed
void ll_palette_id_free(U32 id);
void ll_frame_palette_prepare(U32 frame);        // <FS> P1 step 2: main thread, before the palette jobs: the list of the coming frame begins and takes its palette arena
bool ll_frame_palette_write(const F32* floats, U32 count, U32 frame, U32& offset, std::atomic<U32>& id_cell);       // a palette job: writes the palette into the prepared frame's arena (false: the request appends it later)
// DIAGNOSTIC (the ear blink hunt): the 12 floats of the first joint of the palette at a float offset of this frame's shared list (false when the offset is not in the list)
bool ll_frame_palette_first_joint(U32 offset, F32* out12);
// DIAGNOSTIC: the whole palette (12 floats a joint) at a float offset of this frame's shared list
bool ll_frame_palette_copy(U32 offset, U32 joints, std::vector<F32>& out);
U64 ll_frame_palette_frame();       // the frame the list belongs to (a build made in another frame names offsets of another list)
bool ll_palette_shared();           // LL_VK_PALETTE_SHARED (default 1; 0 = a palette copy in every record, as before), LL_VK_PALETTE_SHARED_AB=<s> alternates (marker "[phase] shared palettes now ON/off")

// <FS> avatar relative skinning (2026-11-09). A rigged palette holds world matrices in region coordinates; far above the ground (a skybox at 3800 m) a float there has a step of 0.24 mm, every skinned
// vertex lands on that lattice on its own (the jagged face and chin under DLSS). Above a height the palettes are made relative to an anchor (a 64 m grid point near the camera) and the shaders add the
// anchor back after the camera matrices (rig_transform in rows.glsl), so the large numbers meet once per draw instead of once per vertex. The anchor travels in the palette itself: a palette has one more
// joint than its mesh, a tail entry whose first row holds (anchor x, y, z, 1); all zeros when the palette is not relative. The anchor of a frame is decided once (thread safe).
struct LLRigAnchor
{
    F32 mA[3] = { 0.f, 0.f, 0.f };
    bool mActive = false;
};
LLRigAnchor ll_rig_anchor(U32 frame);      // LL_VK_RIGREBASE: 0 off, 1 (default) when the camera is high, 2 always (to test at low altitude)

class LLRenderPass : public LLDrawPool
{
public:
    // list of possible LLRenderPass types to assign a render batch to
    // NOTE: "rigged" variant MUST be non-rigged variant + 1
    // see LLVolumeGeometryManager::registerFace()
    enum
    {
        PASS_SIMPLE = NUM_POOL_TYPES,
        PASS_SIMPLE_RIGGED,
        PASS_GRASS,
        PASS_FULLBRIGHT,
        PASS_FULLBRIGHT_RIGGED,
        PASS_INVISIBLE,                         // Formerly, invisiprims.  Now, water exclusion surfaces.
        PASS_INVISIBLE_RIGGED,
        PASS_INVISI_SHINY,
        PASS_INVISI_SHINY_RIGGED,
        PASS_FULLBRIGHT_SHINY,
        PASS_FULLBRIGHT_SHINY_RIGGED,
        PASS_SHINY,
        PASS_SHINY_RIGGED,
        PASS_BUMP,
        PASS_BUMP_RIGGED,
        PASS_POST_BUMP,
        PASS_POST_BUMP_RIGGED,
        PASS_MATERIAL,
        PASS_MATERIAL_RIGGED,
        PASS_MATERIAL_ALPHA,
        PASS_MATERIAL_ALPHA_RIGGED,
        PASS_MATERIAL_ALPHA_MASK,              // Diffuse texture used as alpha mask
        PASS_MATERIAL_ALPHA_MASK_RIGGED,
        PASS_MATERIAL_ALPHA_EMISSIVE,
        PASS_MATERIAL_ALPHA_EMISSIVE_RIGGED,
        PASS_SPECMAP,
        PASS_SPECMAP_RIGGED,
        PASS_SPECMAP_BLEND,
        PASS_SPECMAP_BLEND_RIGGED,
        PASS_SPECMAP_MASK,                     // Diffuse texture used as alpha mask and specular texture(map)
        PASS_SPECMAP_MASK_RIGGED,
        PASS_SPECMAP_EMISSIVE,
        PASS_SPECMAP_EMISSIVE_RIGGED,
        PASS_NORMMAP,
        PASS_NORMMAP_RIGGED,
        PASS_NORMMAP_BLEND,
        PASS_NORMMAP_BLEND_RIGGED,
        PASS_NORMMAP_MASK,                     // Diffuse texture used as alpha mask and normal map
        PASS_NORMMAP_MASK_RIGGED,
        PASS_NORMMAP_EMISSIVE,
        PASS_NORMMAP_EMISSIVE_RIGGED,
        PASS_NORMSPEC,
        PASS_NORMSPEC_RIGGED,
        PASS_NORMSPEC_BLEND,
        PASS_NORMSPEC_BLEND_RIGGED,
        PASS_NORMSPEC_MASK,                    // Diffuse texture used as alpha mask with normal and specular map
        PASS_NORMSPEC_MASK_RIGGED,
        PASS_NORMSPEC_EMISSIVE,
        PASS_NORMSPEC_EMISSIVE_RIGGED,
        PASS_GLOW,
        PASS_GLOW_RIGGED,
        PASS_GLTF_GLOW,
        PASS_GLTF_GLOW_RIGGED,
        PASS_ALPHA,
        PASS_ALPHA_RIGGED,
        PASS_ALPHA_MASK,
        PASS_ALPHA_MASK_RIGGED,
        PASS_FULLBRIGHT_ALPHA_MASK,            // Diffuse texture used as alpha mask and fullbright
        PASS_FULLBRIGHT_ALPHA_MASK_RIGGED,
        PASS_ALPHA_INVISIBLE,
        PASS_ALPHA_INVISIBLE_RIGGED,
        PASS_GLTF_PBR,
        PASS_GLTF_PBR_RIGGED,
        PASS_GLTF_PBR_ALPHA_MASK,
        PASS_GLTF_PBR_ALPHA_MASK_RIGGED,
        NUM_RENDER_TYPES,
    };

    #if LL_PROFILER_ENABLE_RENDER_DOC
    static inline const char* lookupPassName(U32 pass)
    {
        switch (pass)
        {
            case PASS_SIMPLE:
                return "PASS_SIMPLE";
            case PASS_SIMPLE_RIGGED:
                return "PASS_SIMPLE_RIGGED";
            case PASS_GRASS:
                return "PASS_GRASS";
            case PASS_FULLBRIGHT:
                return "PASS_FULLBRIGHT";
            case PASS_FULLBRIGHT_RIGGED:
                return "PASS_FULLBRIGHT_RIGGED";
            case PASS_INVISIBLE:
                return "PASS_INVISIBLE";
            case PASS_INVISIBLE_RIGGED:
                return "PASS_INVISIBLE_RIGGED";
            case PASS_INVISI_SHINY:
                return "PASS_INVISI_SHINY";
            case PASS_INVISI_SHINY_RIGGED:
                return "PASS_INVISI_SHINY_RIGGED";
            case PASS_FULLBRIGHT_SHINY:
                return "PASS_FULLBRIGHT_SHINY";
            case PASS_FULLBRIGHT_SHINY_RIGGED:
                return "PASS_FULLBRIGHT_SHINY_RIGGED";
            case PASS_SHINY:
                return "PASS_SHINY";
            case PASS_SHINY_RIGGED:
                return "PASS_SHINY_RIGGED";
            case PASS_BUMP:
                return "PASS_BUMP";
            case PASS_BUMP_RIGGED:
                return "PASS_BUMP_RIGGED";
            case PASS_POST_BUMP:
                return "PASS_POST_BUMP";
            case PASS_POST_BUMP_RIGGED:
                return "PASS_POST_BUMP_RIGGED";
            case PASS_MATERIAL:
                return "PASS_MATERIAL";
            case PASS_MATERIAL_RIGGED:
                return "PASS_MATERIAL_RIGGED";
            case PASS_MATERIAL_ALPHA:
                return "PASS_MATERIAL_ALPHA";
            case PASS_MATERIAL_ALPHA_RIGGED:
                return "PASS_MATERIAL_ALPHA_RIGGED";
            case PASS_MATERIAL_ALPHA_MASK:
                return "PASS_MATERIAL_ALPHA_MASK";
            case PASS_MATERIAL_ALPHA_MASK_RIGGED:
                return "PASS_MATERIAL_ALPHA_MASK_RIGGED";
            case PASS_MATERIAL_ALPHA_EMISSIVE:
                return "PASS_MATERIAL_ALPHA_EMISSIVE";
            case PASS_MATERIAL_ALPHA_EMISSIVE_RIGGED:
                return "PASS_MATERIAL_ALPHA_EMISSIVE_RIGGED";
            case PASS_SPECMAP:
                return "PASS_SPECMAP";
            case PASS_SPECMAP_RIGGED:
                return "PASS_SPECMAP_RIGGED";
            case PASS_SPECMAP_BLEND:
                return "PASS_SPECMAP_BLEND";
            case PASS_SPECMAP_BLEND_RIGGED:
                return "PASS_SPECMAP_BLEND_RIGGED";
            case PASS_SPECMAP_MASK:
                return "PASS_SPECMAP_MASK";
            case PASS_SPECMAP_MASK_RIGGED:
                return "PASS_SPECMAP_MASK_RIGGED";
            case PASS_SPECMAP_EMISSIVE:
                return "PASS_SPECMAP_EMISSIVE";
            case PASS_SPECMAP_EMISSIVE_RIGGED:
                return "PASS_SPECMAP_EMISSIVE_RIGGED";
            case PASS_NORMMAP:
                return "PASS_NORMAMAP";
            case PASS_NORMMAP_RIGGED:
                return "PASS_NORMMAP_RIGGED";
            case PASS_NORMMAP_BLEND:
                return "PASS_NORMMAP_BLEND";
            case PASS_NORMMAP_BLEND_RIGGED:
                return "PASS_NORMMAP_BLEND_RIGGED";
            case PASS_NORMMAP_MASK:
                return "PASS_NORMMAP_MASK";
            case PASS_NORMMAP_MASK_RIGGED:
                return "PASS_NORMMAP_MASK_RIGGED";
            case PASS_NORMMAP_EMISSIVE:
                return "PASS_NORMMAP_EMISSIVE";
            case PASS_NORMMAP_EMISSIVE_RIGGED:
                return "PASS_NORMMAP_EMISSIVE_RIGGED";
            case PASS_NORMSPEC:
                return "PASS_NORMSPEC";
            case PASS_NORMSPEC_RIGGED:
                return "PASS_NORMSPEC_RIGGED";
            case PASS_NORMSPEC_BLEND:
                return "PASS_NORMSPEC_BLEND";
            case PASS_NORMSPEC_BLEND_RIGGED:
                return "PASS_NORMSPEC_BLEND_RIGGED";
            case PASS_NORMSPEC_MASK:
                return "PASS_NORMSPEC_MASK";
            case PASS_NORMSPEC_MASK_RIGGED:
                return "PASS_NORMSPEC_MASK_RIGGED";
            case PASS_NORMSPEC_EMISSIVE:
                return "PASS_NORMSPEC_EMISSIVE";
            case PASS_NORMSPEC_EMISSIVE_RIGGED:
                return "PASS_NORMSPEC_EMISSIVE_RIGGED";
            case PASS_GLOW:
                return "PASS_GLOW";
            case PASS_GLOW_RIGGED:
                return "PASS_GLOW_RIGGED";
            case PASS_ALPHA:
                return "PASS_ALPHA";
            case PASS_ALPHA_RIGGED:
                return "PASS_ALPHA_RIGGED";
            case PASS_ALPHA_MASK:
                return "PASS_ALPHA_MASK";
            case PASS_ALPHA_MASK_RIGGED:
                return "PASS_ALPHA_MASK_RIGGED";
            case PASS_FULLBRIGHT_ALPHA_MASK:
                return "PASS_FULLBRIGHT_ALPHA_MASK";
            case PASS_FULLBRIGHT_ALPHA_MASK_RIGGED:
                return "PASS_FULLBRIGHT_ALPHA_MASK_RIGGED";
            case PASS_ALPHA_INVISIBLE:
                return "PASS_ALPHA_INVISIBLE";
            case PASS_ALPHA_INVISIBLE_RIGGED:
                return "PASS_ALPHA_INVISIBLE_RIGGED";
            case PASS_GLTF_PBR:
                return "PASS_GLTF_PBR";
            case PASS_GLTF_PBR_RIGGED:
                return "PASS_GLTF_PBR_RIGGED";
            case PASS_GLTF_PBR_ALPHA_MASK:
                return "PASS_GLTF_PBR_ALPHA_MASK";
            case PASS_GLTF_PBR_ALPHA_MASK_RIGGED:
                return "PASS_GLTF_PBR_ALPHA_MASK_RIGGED";
            default:
                return "Unknown pass";
        }
    }
    #else
    static inline const char* lookupPassName(U32 pass) { return ""; }
    #endif

    LLRenderPass(const U32 type);
    virtual ~LLRenderPass();
    /*virtual*/ LLViewerTexture* getDebugTexture() { return NULL; }
    LLViewerTexture* getTexture() { return NULL; }
    bool isDead() { return false; }
    void resetDrawOrders() { }

    static void applyModelMatrix(const LLDrawBatch& params);
    // For rendering that doesn't use LLDrawBatch for some reason
    static void applyModelMatrix(const LLMatrix4* model_matrix);
    // S6.6: the draw infos of a render map type as one record of entries, drawn with the pull program bound now (draws its buffers cannot give to the packet path are skipped with a warning)


    // <FS> render packets for the deferred geometry passes (Vulkan mirror only): sends the static draws of a pass type as one record, with the program bound by the caller
    // (a pull program); the draws it cannot carry are returned to be drawn the old way
    static bool deferredPacketsEnabled();
    static void pushGeometryPackets(U32 type, bool rigged = false);
    // S3 (doc/vulkan/S3-design.md): the entry a geometry record gets for one draw, with the textures as pointers: textures[0..3] the diffuse ones, [4] the normal map, [5] the specular map; the entry's
    // texture fields hold 1 + the index into textures (0 = none), its matrix is 0. False: the draw takes the old path (the same decision as the builder's). Kept in step with build_geometry by the verify mode.
    static bool pbrAlphaEntry(const LLDrawBatch& params, LLVkBridge::GeoEntry& entry, bool rigged = false);       // S5c: the arrays of a draw with a glTF blend material (textures and values: the caller)
    static bool geometryEntry(const LLDrawBatch& params, const LLGeometryPassInfo* materials, LLVkBridge::GeoEntry& entry, LLViewerTexture* textures[6], bool rigged = false, bool texture_matrix = false, U32 arrays = LL_PASS_ARRAY_NORMALS);
    // the builds of the geometry records start on the async pool at the top of renderGeomDeferred (the main thread draws its own things meanwhile), pushGeometryPackets takes them
    // <FS:Beq> be defensive about UAF with skinInfo during LocalMesh
    // static bool uploadMatrixPalette(LLVOAvatar* avatar, LLMeshSkinInfo* skinInfo);
    // static bool uploadMatrixPalette(LLVOAvatar* avatar, LLMeshSkinInfo* skinInfo, const LLVOAvatar*& lastAvatar, U64& lastMeshId, bool& skipLastSkin);
    // static bool uploadMatrixPalette(LLVOAvatar* avatar, LLMeshSkinInfo* skinInfo, const LLVOAvatar*& lastAvatar, U64& lastMeshId, const LLShaderProgram*& lastAvatarShader, bool& skipLastSkin);
    // </FS:Beq>
    static bool uploadMatrixPalette(LLVOAvatar* avatar, const LLMeshSkinInfo* skinInfo);
    static bool uploadMatrixPalette(LLVOAvatar* avatar, const LLMeshSkinInfo* skinInfo, const LLVOAvatar*& lastAvatar, U64& lastMeshId, bool& skipLastSkin);
    static bool uploadMatrixPalette(LLVOAvatar* avatar, const LLMeshSkinInfo* skinInfo, const LLVOAvatar*& lastAvatar, U64& lastMeshId, const LLShaderProgram*& lastAvatarShader, bool& skipLastSkin);
};

void ll_pull_draw_range(const LLVertexBuffer* vb, U32 first_index, U32 index_count);       // S6.6d
void ll_pool_draw_dropped(const char* what, size_t count);       // a draw that has no old path any more because its pull program is not ready: counted, the first one (and every 10000th) logged
void ll_render_table_debug();       // K6 B1 (lldrawpoolalpha.cpp): the Batch Size and Texture Animation debug views, drawn from the scene table
int ll_scene_geo_type_count();       // pipeline.cpp: the table's pass types by number
U32 ll_scene_geo_type_pass(int tn);

class LLFacePool : public LLDrawPool
{
public:
    typedef std::vector<LLFace*> face_array_t;

    enum
    {
        SHADER_LEVEL_SCATTERING = 2
    };

public:
    LLFacePool(const U32 type);
    virtual ~LLFacePool();

    bool isDead() { return mReferences.empty(); }

    virtual LLViewerTexture *getTexture();
    virtual void dirtyTextures(const std::set<LLViewerFetchedTexture*>& textures);

    virtual void enqueue(LLFace *face);
    virtual bool addFace(LLFace *face);
    virtual bool removeFace(LLFace *face);

    virtual bool verify() const;        // Verify that all data in the draw pool is correct!

    virtual void resetDrawOrders();
    void resetAll();

    void destroy();

    void buildEdges();

    void addFaceReference(LLFace *facep);
    void removeFaceReference(LLFace *facep);

    void printDebugInfo() const;

    bool isFacePool() { return true; }

    // S6.6: the faces of the pool as ONE record of geometry entries (kind 32) drawn with the pull program bound now (region_matrix: each face's region render matrix is its model matrix, else none)
    void pushFacesPull(bool region_matrix);
    void pushWholeBufferFacesPull();        // <FS> the faces whose buffer is drawn whole (the Linden trees: index 0 to the buffer's last index) under the region's render matrix, as one record

    friend class LLFace;
    friend class LLPipeline;
public:
    face_array_t    mDrawFace;
    face_array_t    mMoveFace;
    face_array_t    mReferences;

public:
    class LLOverrideFaceColor
    {
    public:
        LLOverrideFaceColor(LLDrawPool* pool)
            : mOverride(sOverrideFaceColor), mPool(pool)
        {
            sOverrideFaceColor = true;
        }
        LLOverrideFaceColor(LLDrawPool* pool, const LLColor4& color)
            : mOverride(sOverrideFaceColor), mPool(pool)
        {
            sOverrideFaceColor = true;
            setColor(color);
        }
        LLOverrideFaceColor(LLDrawPool* pool, const LLColor4U& color)
            : mOverride(sOverrideFaceColor), mPool(pool)
        {
            sOverrideFaceColor = true;
            setColor(color);
        }
        LLOverrideFaceColor(LLDrawPool* pool, F32 r, F32 g, F32 b, F32 a)
            : mOverride(sOverrideFaceColor), mPool(pool)
        {
            sOverrideFaceColor = true;
            setColor(r, g, b, a);
        }
        ~LLOverrideFaceColor()
        {
            sOverrideFaceColor = mOverride;
        }
        void setColor(const LLColor4& color);
        void setColor(const LLColor4U& color);
        void setColor(F32 r, F32 g, F32 b, F32 a);
        bool mOverride;
        LLDrawPool* mPool;
        static bool sOverrideFaceColor;
    };
};

// <FS> Shadow draw statistics (render packets, stage P0): how the draws of the shadow passes split up, and how often consecutive static untextured draws change vertex buffer
struct LLShadowDrawStats
{
    U64 mPasses = 0, mTotalDraws = 0;
    U64 mStaticUntex = 0, mStaticVBChanges = 0, mStaticDistinctVB = 0;
    U64 mRiggedUntex = 0, mTextured = 0, mGLTF = 0;
    U64 mStaticMatrixChanges = 0;
    U64 mPacketEntries = 0, mPacketFallback = 0, mPacketRecords = 0;      // shadow packets: draws sent in records, draws left to the old path, records
    U64 mAlphaFragEntries = 0, mAlphaFragGroups = 0;      // J1 K3c: entries made from the fragments of groups the worker does not hold (and the groups they came from)
    U64 mAlphaEntries = 0, mAlphaFallback = 0, mAlphaRecords = 0, mAlphaTextures = 0;      // the same for the alpha masked casters; distinct textures bound for them
    U64 mSkinEntries = 0, mSkinFallback = 0, mSkinRecords = 0, mSkinPalettes = 0;      // the same for the rigged untextured casters; distinct palettes sent
    U64 mFbGLTF = 0, mFbLayout = 0, mFbRigged = 0, mFbManyTextures = 0, mFbTextureMatrix = 0;      // why alpha masked casters were left to the old path (every pass)
    U64 mRtOpaque = 0, mRtMask = 0, mRtBlend = 0, mRtTextureMatrix = 0, mRtRecords = 0;      // ray tracing only: casters the old path draws, named to the ray scene (glTF opaque, glTF mask, glTF blend, textured with a texture matrix)
    const void* mLastVB = nullptr;
    const void* mLastMatrix = nullptr;
    std::unordered_set<const void*> mPassVBs;
    U32 mLastReport = 0;

    // Render packets P1: the table of one static shadow pass (what a packet record would carry), built next to the normal draws and checked, not used
    struct Caster
    {
        const LLVertexBuffer* mVB;
        U32 mStart, mEnd, mCount, mIndexOffset;
        const void* mMatrix;
    };
    std::vector<Caster> mTable;
    U64 mTablePasses = 0, mTableEntries = 0, mTableMax = 0;
    U64 mBadNames = 0, mWideIndices = 0, mPositionNotFirst = 0, mNoPosition = 0, mBadRange = 0, mBadIndexRange = 0, mNullMatrix = 0, mDistinctPairs = 0;
    void beginStaticPass() { mLastVB = nullptr; mLastMatrix = nullptr; mPassVBs.clear(); mTable.clear(); }
    void endStaticPass();
};
extern LLShadowDrawStats gShadowDrawStats;

// <FS> geometry packet statistics (logged with the DeferredDraws report)
struct LLGeometryPacketStats
{
    U64 mEntries = 0, mFallback = 0, mRecords = 0, mTextures = 0, mMatrices = 0;
};
extern LLGeometryPacketStats gGeometryPacketStats;


#endif //LL_LLDRAWPOOL_H
