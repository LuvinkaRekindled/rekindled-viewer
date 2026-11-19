/**
 * @file rkrigstore.h
 * @brief D4 (doc/vulkan/PLAN-avatars-100.md): shared rigged face buffers
 *
 * RK_VK_RIGSTORE:
 *  1  (stage 1a, diagnostic) builds, for every rigged face the fill writes, the entry a shared store would hold with code of its own and compares it with what the face's own vertex buffer holds.
 *  3  (stage 1b-3, the key check, diagnostic) the faces are filled as always; the first face of each cache key leaves a snapshot of its arrays, every later face with the same key is compared with it
 *     array by array (positions, normals, texcoords of all channels, colour, emissive, tangents, weights, indices). 0 differences means the key names everything the bake depends on.
 *  2  (stage 1b-3, the cache) a rigged face of the class (default texgen, opaque, no glTF, no bump, no texture animation, not selected) whose key is already held by a buffer attaches to that buffer
 *     and is not filled: its buffer pointer is the shared buffer, its geometry and index start are 0. The first face of a key fills a buffer of its own (the existing getGeometryVolume) and the registry
 *     keeps it. A face of the cache is never written again (LLFace::mRigStore makes getGeometryVolume a no-op for it; rebuildMesh marks its group's geometry dirty instead). Everything downstream
 *     (draw infos, entry makers, fallbacks, shaders) sees a buffer that holds exactly what the face's own fill would have written.
 * The key: mesh asset, detail, skin hash, face index, the vertex type mask, the packed colour (with the shiny-in-alpha rule), the glow, and the three texture coordinate transforms (the texture
 * entry's, the material's normal map and specular map).
 */
#ifndef RK_RKRIGSTORE_H
#define RK_RKRIGSTORE_H

#include "stdtypes.h"
#include "llpointer.h"
#include "llvertexbuffer.h"

class LLFace;
class LLVolume;

int rk_rigstore_mode();
bool rk_rigstore_on();      // mode 1
// mode 1: a pool thread or the main thread, right after LLFace::getGeometryVolume filled the face's arrays (te_idx: the volume face index)
void rk_rigstore_compare(LLFace* face, const LLVolume& volume, U32 te_idx);

// mode 2, main thread (genDrawInfo): the faces of the class
bool rk_rigcache_eligible(LLFace* face, bool distance_sort);
// 0: not handled (the face goes the normal way), 1: a hit (the face is attached to the shared buffer: no fill), 2: a miss (a buffer was allocated and registered for this face: it fills it, buffer returned)
int rk_rigcache_plan(LLFace* face, U32 mask, bool batch_textures, LLPointer<LLVertexBuffer>& buffer);
void rk_rigcache_release(LLFace* face);     // the face leaves the cache (it is not eligible any more)
void rk_rigcache_sweep();                   // entries that no face holds are given up (about every 300 frames)
void rk_rigstore_note_blocked();            // getGeometryVolume was asked to write a cache face: it did not

// mode 3: a pool thread or the main thread, right after the fill
void rk_rigcache_verify(LLFace* face, U32 mask);

#endif // RK_RKRIGSTORE_H
