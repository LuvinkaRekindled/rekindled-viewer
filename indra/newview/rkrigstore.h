/**
 * @file rkrigstore.h
 * @brief D4 stage 1a (doc/vulkan/PLAN-avatars-100.md): the shared store of rigged geometry, as a check
 *
 * A rigged face's positions, normals and weights come only from the skin's bind shape matrix and the mesh asset; what is baked per instance is the colour, the texture coordinate transform and the
 * batch's texture index (the w of the position). RK_VK_RIGSTORE=1 (diagnostic) builds, for every rigged face the fill writes, the entry a shared store would hold for it (key: mesh asset, detail, skin hash,
 * face index) with code of its own, and compares it with what the face's own vertex buffer holds: positions (xyz), normals, weights, indices, and for the faces of the stage 1 class (default texgen, opaque,
 * no material, no glTF, no bump) the colour and the texture coordinates through the matrix form of the texture entry's transform. Nothing is drawn from the store. The log line (tag RigStore) says how many
 * faces were compared, how many entries the store holds, how often an entry was used again by another face (the sharing, by bytes), and how many floats differed.
 */
#ifndef RK_RKRIGSTORE_H
#define RK_RKRIGSTORE_H

#include "stdtypes.h"

class LLFace;
class LLVolume;

bool rk_rigstore_on();
// a pool thread or the main thread, right after LLFace::getGeometryVolume filled the face's arrays (te_idx: the volume face index)
void rk_rigstore_compare(LLFace* face, const LLVolume& volume, U32 te_idx);

#endif // RK_RKRIGSTORE_H
