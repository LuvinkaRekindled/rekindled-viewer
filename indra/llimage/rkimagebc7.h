/**
 * @file rkimagebc7.h
 * @brief BC7 at decode (doc/vulkan/PLAN-aurora-ideas.md section 2 stage b): the mip chain of a decoded texture, its BC7 encoding with the per-block check that keeps a
 *        texture raw when the encoding would show, and the compressed form of LLImageRaw that carries the chain to the GPU.
 *
 * Encoder: bc7enc (Richard Geldreich, MIT / public domain; indra/llimage/bc7enc, built through rkimagebc7enc.cpp / rkimagebc7decomp.cpp with warnings off), the fast settings
 * with linear weights: mode 6, no partitions, no least squares; 25-33 Mpx/s a thread on the offline check. The check, per 4x4 block of the finest level: harmed when the
 * block's mean moved by more than 6 in a channel, or the block is smooth (every channel's deviation under 12) and a texel moved by more than 12, or the block is noisy and
 * the encoding flattened or roughened its grain (deviation outside 0.6..1.4 of the original's), or its alpha is binary (0 and 255 only) and an alpha-end texel moved past 8.
 * Noise outside the UV islands (random colour fills, common in Second Life content) is judged on its mean only, which is what the eye does. More than 5 % harmed blocks keeps
 * the texture raw: on Adam's 2676 cached textures (2026-10-29) that kept 10.6 % of the textures and 4.8 % of the pixels raw, all of the visible cases fine-grained normal maps.
 */

#ifndef RK_RKIMAGEBC7_H
#define RK_RKIMAGEBC7_H

#include "stdtypes.h"
#include "llpointer.h"

#include <vector>

class LLImageRaw;

namespace RKImageBC7
{
    // RenderCompressTextures, mirrored from the main thread (read on the decode threads)
    void setEnabled(bool on);
    bool enabled();

    // the policy: at least this size, 3 or 4 components, a power of two in both dimensions
    const U32 MIN_SIZE = 64;
    const F32 MAX_HARMED_PERCENT = 5.f;
    bool eligible(U32 width, U32 height, U32 components);

    // the compressed format code LLImageRaw carries (llimage knows no GPU format names): 1 = BC7, 16 bytes a 4x4 block, levels down to 1x1
    const U8 FORMAT_BC7 = 1;

    U32 levelWidth(U32 width, U32 k);
    U32 levelHeight(U32 height, U32 k);
    U32 levelCount(U32 width, U32 height);          // levels down to 1x1 (the GPU image owns the whole chain)
    U64 blockBytes(U32 width, U32 height);          // BC7 bytes of one level of this size

    // The mip chain of a texel image: levels[0] a copy of src, each next the 2x2 box filter of the one before (what the GPU's mip generation does), down to 1x1.
    void buildChain(const U8* src, U32 width, U32 height, U32 components, std::vector<std::vector<U8>>& levels);

    // The alpha classification RKImageTex::analyzeAlpha makes from texels: -1 no alpha channel, 0 blend, 1 mask (the same histogram and rules).
    S8 classifyAlpha(const U8* data, U32 width, U32 height, U32 components);

    // Encodes every level of a chain (levels[0] the finest) and checks the finest. Returns the compressed image, the chain in the order RKImageTex::setImage(data_hasmips)
    // reads it (the smallest level first, the finest last), or null when more than max_harmed_percent of the finest level's blocks are harmed; harmed_percent says how many
    // either way. alpha_is_mask goes into the image.
    LLPointer<LLImageRaw> compress(const std::vector<std::vector<U8>>& levels, U32 width, U32 height, U32 components, F32 max_harmed_percent, F32& harmed_percent, S8 alpha_is_mask);

    // The compressed image from BC7 block levels given directly (the decoded cache's read): block_levels[0] the finest.
    LLPointer<LLImageRaw> assemble(const std::vector<std::vector<U8>>& block_levels, U32 width, U32 height, U32 components, S8 alpha_is_mask);

    // The block levels of a compressed image (the inverse of assemble: the decoded cache's write): out[0] the finest.
    bool splitLevels(const LLImageRaw* compressed, std::vector<std::vector<U8>>& out);

    // <FS> 2026-11-14 (the bake slots whose mip levels 1 and up in the GPU image are not what was uploaded): the sampled hash (rk_sparse_hash, llimage.h) of every level of a compressed chain, finest first,
    // taken from the buffer without copying it
    void levelHashes(const LLImageRaw* compressed, std::vector<U64>& out);
    // a registry of the levels of recent BC7 uploads (the last 30000 levels of 1 KB or more): a level that a GPU image holds and that was not uploaded to it can be named by the texture whose upload it was
    void registerLevels(const std::vector<U64>& hashes, const std::string& id_text, S32 discard, U32 width, U32 height);
    std::string lookupLevel(U64 hash);

    // what happened since the last call (the decoded cache's report)
    struct Stats { U32 mCompressed = 0; U32 mKeptRaw = 0; U64 mEncodeMicroseconds = 0; };
    Stats takeStats();
}

#endif // RK_RKIMAGEBC7_H
