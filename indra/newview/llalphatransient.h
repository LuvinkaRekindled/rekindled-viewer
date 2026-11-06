/**
 * @file llalphatransient.h
 * @brief S5f F3 (doc/vulkan/S5-design.md section 17): one alpha draw the scene table does not keep, as the worker's transient record (kind 32) takes it: the entry (arrays, flags), the run key, the
 *        textures it names (as pointers: the caller makes the GL names), its PBR values.
 */
#ifndef LL_LLALPHATRANSIENT_H
#define LL_LLALPHATRANSIENT_H

#include "llvkbridge.h"

class LLDrawInfo;
class LLViewerTexture;
class LLVertexBuffer;
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

// pipeline.cpp: can the table's pull programs draw this draw as a transient entry (the same conditions as a fragment item), and if so its entry, key, textures and values
bool ll_alpha_transient(LLDrawInfo* draw, bool rigged, bool depth_only, LLAlphaTransient& out);

// C5 J2: the same for a run of a particle group (rows of the group's buffer); false = not drawn
bool ll_particle_transient(const LLParticleRun& run, const LLVertexBuffer* rows, bool depth_only, LLAlphaTransient& out);

#endif
