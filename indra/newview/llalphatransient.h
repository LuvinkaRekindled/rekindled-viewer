/**
 * @file llalphatransient.h
 * @brief S5f F3 (doc/vulkan/S5-design.md section 17): one alpha draw the scene table does not keep, as the worker's transient record (kind 32) takes it: the entry (arrays, flags), the run key, the
 *        textures it names (as pointers: the caller makes the GL names), its PBR values.
 */
#ifndef LL_LLALPHATRANSIENT_H
#define LL_LLALPHATRANSIENT_H

#include "llvkbridge.h"

class LLDrawBatch;
class LLViewerTexture;
class LLVertexBuffer;
class LLSpatialGroup;
class LLVOAvatar;
class LLMeshSkinInfo;
class LLMatrix4;
struct LLParticleRun;

struct LLAlphaTransient
{
    LLVkBridge::GeoEntry mEntry = {};
    U32 mKey = 0;                       // the run key (pipeline.cpp: program in bits 0-7, blend pair id in 8-15, bit 16 double sided, bit 17 rigged)
    bool mPbr = false;                  // a glTF draw: mPbrValues are its row
    bool mUseTexMatrix = false;         // the draw has a texture matrix the entry takes (the caller numbers it: entry.mPad3)
    float mPbrValues[40] = {};
    LLViewerTexture* mTextures[6] = {}; // textures[k] belongs to entry.mTextures[0..3], mNormalMap, mSpecularMap (the caller replaces the numbers by GL names)
};


// what the pass reads besides the entry when it makes the transient record of a draw: the model matrix and the texture matrix (the pass numbers them), the avatar and skin of a rigged draw (its palette),
// the glow class of its emissive buffer (255 = none, 0 plain, 1 glTF, 2 double sided glTF). From a fragment's transient item: no draw info is read.
struct LLAlphaTransientDraw
{
    const LLMatrix4* mModel = nullptr;
    const LLMatrix4* mTexMat = nullptr;
    LLVOAvatar* mAvatar = nullptr;
    const LLMeshSkinInfo* mSkin = nullptr;
    unsigned char mGlowCls = 255;
    LLVertexBuffer* mVb = nullptr;      // the draw's range (the Highlight Transparent view draws it with the highlight program)
    U32 mStart = 0, mEnd = 0, mCount = 0, mOffset = 0;
};
// the group's fragment keeps one transient item per classifiable alpha draw of a set (static / rigged), in the group's order: -1 when the fragment is not current (or has no plan), else the count
int ll_alpha_transient_count(const LLSpatialGroup* group, bool rigged);
// the group has a static alpha blended draw (its fragment's plan counts them)
bool ll_group_has_static_alpha(const LLSpatialGroup* group);
// what the pass reads of item `index` besides the entry (no context test: the debug views use it)
bool ll_alpha_transient_draw(const LLSpatialGroup* group, bool rigged, U32 index, LLAlphaTransientDraw& draw);
// item `index` as ll_alpha_transient gave it for the draw (false: the context cannot draw it)
bool ll_alpha_transient_item(const LLSpatialGroup* group, bool rigged, U32 index, bool depth_only, LLAlphaTransient& out, LLAlphaTransientDraw& draw);

// C5 J2: the same for a run of a particle group (rows of the group's buffer); false = not drawn
bool ll_particle_transient(const LLParticleRun& run, const LLVertexBuffer* rows, bool depth_only, LLAlphaTransient& out);

#endif
