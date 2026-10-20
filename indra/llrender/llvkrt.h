/**
 * @file llvkrt.h
 * @brief Ray tracing scene for the native Vulkan renderer: bottom-level structures per vertex/index buffer pair, a top-level structure per frame, and the descriptor
 *        set shaders trace against (inline ray queries). See doc/vulkan/PLAN-rt-shadows.md.
 *
 * Worker thread only, like the buffer table it reads (LLVkWorker::getBufferInfo).
 */

#ifndef LL_LLVKRT_H
#define LL_LLVKRT_H

#if defined(LL_VULKAN)

#include "llvkcontext.h"
#include "llvkbridge.h"

#include <string>

namespace LLVk
{
namespace Rt
{

// live switches of rt-debug.txt the viewer reads (main thread): gate=0/1 (do not trace avatars that are still loading nor masked casters without texture data), bodyname=0/1
bool loadingGates();
S32 glossyReflections();   // glossyrt: ray traced reflections on glossy surfaces of the lighting pass
S32 waterReflections();    // waterrt=1: ray traced water reflections
S32 hitRays();             // hitrays=N: shadow rays per local light at a traced hit (-1 not set)
S32 hitQuality();          // hitq=N: how a hit of the traced glossy mirror is lit (0 flat diffuse, 1 main pass lighting at the hit)
S32 debugView();        // view=N: the material debug view mode to show (0 = the setting RenderRTMaterialDebug)
bool bodyRename();

// LL_VK_RT_SHADOWS: 0 off, 1 ray query shadows replace the cascades, 2 split screen (left cascades, right rays), 3 disagreement map. Decided once, before the shaders compile.
U32 shadowMode();

// A shadow caster draw as the shadow passes name it. mKind 0 = opaque casters, 1 = alpha masked or blended. mModel: 16 floats column-major (null = identity),
// mMatrixId: stable identity of the matrix (the object it belongs to), the same value for the same object on every frame.
// what the alpha test of a masked caster needs (the fields of ShadowAlphaEntry): flags 1 = fullbright (the vertex colour's alpha does not count), 2 = tree style (no vertex alpha,
// no dithering), 4 = one texture (no index in position.w), 8 = texcoords present, 16 = colours present, 64 = a plain cut at the cutoff (glTF alpha mask)
struct AlphaInfo
{
    U32 mFlags = 0;
    U32 mTexcoordOffset = 0, mColorOffset = 0;      // byte offsets of the arrays in the vertex buffer
    U32 mTextures[4] = {};                          // GL names of the textures the vertices' texture index picks from, 0 = none (white)
    F32 mCutoff = 0.f;
    F32 mUv[6] = { 1.f, 0.f, 0.f, 1.f, 0.f, 0.f };   // texture coordinate transform: u' = mUv[0] * u + mUv[2] * v + mUv[4], v' = mUv[1] * u + mUv[3] * v + mUv[5]
    U32 mNormalOffset = 0;                          // byte offset of the normal array (0 = none); ray scene materials
    U32 mMaterial = 0;                              // the viewer's material id (RtMaterialDef), 0 = none. Opaque casters carry an AlphaInfo too when materials are on (flags 8 / 16 say which arrays exist)
};

struct Caster
{
    U32 mVb = 0, mIb = 0;               // GL buffer names
    U32 mFirst = 0, mCount = 0;         // index range
    U32 mMatrixId = 0;
    U32 mKind = 0;
    const F32* mModel = nullptr;
    const AlphaInfo* mAlpha = nullptr;   // kind 1
    U32 mPass = 0;                       // shadow pass (0-3 sun cascades, 4-5 spot, 6 other)
    LLVkWorker::BufferInfo mVi, mIi;         // where the vertex and index buffers live now (the handler has them at hand: no lookups later)
};

// A rigged caster draw (kind 2 shadow packets): its vertices are bind pose, skinned by a compute pass every frame into a copy the avatar's structure is built from.
// mPalette is the device address of the draw's matrix palette (12 floats per joint), valid until the frame's memory is reused, i.e. for this frame only.
struct SkinCaster
{
    U32 mAvatarId = 0, mSkinId = 0;     // stable identities (the avatar, and the skin of the draw)
    U32 mVb = 0, mIb = 0;               // GL buffer names
    U32 mFirst = 0, mCount = 0;         // index range
    U32 mVertexFirst = 0, mVertexCount = 0;
    U32 mWeightOffset = 0;              // byte offset of the weight4 array in the vertex buffer
    U32 mJoints = 0;
    U64 mPalette = 0;
    const F32* mModel = nullptr;        // 16 floats column-major, null = identity
    U32 mPass = 0;
    F32 mAvatarPos[3] = {};             // where the avatar is (the probe of the skinned data compares with it)
    U32 mSelf = 0;                      // the viewer's own avatar
    U32 mAvatarKind = 0;                // bit 1 animated object, 2 impostor, 4 own, 8 jelly doll, 16 invisible, 32 too slow
    U32 mKind = 0;                      // 0 = opaque, 1 = alpha masked (mAlpha)
    U32 mMode = 0;                      // how the vertices are skinned: 0 = weight4 array + palette, 1 = avatar body (one weight float per vertex, palette of 15 joints), 2 = rigid under mModel
    const AlphaInfo* mAlpha = nullptr;
    LLVkWorker::BufferInfo mVi, mIi;
};
void noteSkinned(const SkinCaster& c);

// the ray scene keeps a table of materials (LL_VK_RT_MATERIALS=1): the viewer's definitions (kind 10 records) are stored here, and every geometry of the scene gets a record that names its material
bool materialsOn();
void updateMaterials(const LLVkBridge::RtMaterialDef* defs, U32 count);
// the bindless slot of a pinned texture (by pin index, see LLVkWorker::rtPinAcquire) changed: the GPU reads it from the pin table
void setPinSlot(U32 index, U32 slot);

// the slot of a texture (by GL name) in this frame's bindless table
typedef U32 (*TextureSlotFn)(U32 gl_name);
// asks the memory system for what a slot lookup of this texture will read: stage 0 the texture's own state, stage 1 its table entries (needs stage 0 to have arrived)
typedef void (*TexturePrefetchFn)(U32 gl_name, U32 stage);

// the device can do it and the mode is on (the descriptor set layout exists, structures are built)
bool active();

// Creates the descriptor pool and sets and an empty structure per frame slot (start-up, after the renderer's layouts exist).
bool init(VkDescriptorSetLayout set_layout);
void shutdown();

// A handle on the group a caster was filed under (S2, resident casters): kept by the caller, so that the next frames' notes of an unchanged caster skip the lookups (noteAgain).
// It names the group by its address and row; the key is kept to catch an address that was reused by another group.
struct CasterRef
{
    void* mGroup = nullptr;
    U32 mRow = ~0u;
    U32 mVb = 0, mIb = 0, mMatrixId = 0;
};

// One caster draw of a shadow pass (shadow packets). Several passes of a frame name the same casters: the repeats are dropped. ref: filled with the handle of the group.
void noteCaster(const Caster& c, CasterRef* ref = nullptr);
// The note of a caster the group already knows (same buffers as at the last full note, same model matrix: the caller checks): sets the pass stamp and the first-of-the-frame bookkeeping.
// False when the handle does not name the group any more (it expired and was erased): the caller notes the caster in full.
bool noteAgain(const CasterRef& ref, U32 pass);
// a pass rendered this frame (the pass stamps of the groups are read against it) even though all its casters were named by an earlier pass of the frame
void passRendered(U32 pass);

// Builds what the casters noted since the last call need (a budget per call), then the top-level structure for this view: the instances are transformed into eye space
// (view * model), the space the lighting shaders work in. Recorded into the current frame outside any render pass. view: 16 floats column-major.
void buildScene(const F32* view, TextureSlotFn slot_of, TexturePrefetchFn prefetch = nullptr);

// the set to bind as set 3 for the frame slot (always valid once active())
VkDescriptorSet descriptorSet(U32 frame_slot);
// the dynamic offset into the set's geometry table (binding 1) that belongs to the latest scene call of the frame
U32 tableOffset(U32 frame_slot);

// one log line: structures, memory, builds, refits, time
std::string report(double frames);
void resetStats();

} // namespace Rt
} // namespace LLVk

#endif // LL_VULKAN
#endif // LL_LLVKRT_H
