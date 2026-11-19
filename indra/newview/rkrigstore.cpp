/**
 * @file rkrigstore.cpp
 * @brief D4 stage 1a: the shared rigged geometry store as a check (see rkrigstore.h)
 */
#include "llviewerprecompiledheaders.h"

#include "rkrigstore.h"

#include "lldrawable.h"      // (before llface.h)
#include "llface.h"
#include "llviewerobject.h"
#include "llmaterial.h"
#include "llvolume.h"
#include "llvertexbuffer.h"
#include "llmatrix4a.h"
#include "llmeshrepository.h"
#include "lltextureentry.h"
#include "llrender.h"
#include "rkdiag.h"

#include <glm/matrix.hpp>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

bool rk_rigstore_on()
{
    return rk_rigstore_mode() == 1;
}

namespace
{
struct Key
{
    LLUUID mAsset;
    S32 mDetail = 0;        // the volume's detail times 1000
    U64 mSkin = 0;
    U32 mFace = 0;
    bool operator==(const Key& o) const { return mAsset == o.mAsset && mDetail == o.mDetail && mSkin == o.mSkin && mFace == o.mFace; }
};
struct KeyHash
{
    size_t operator()(const Key& k) const
    {
        size_t h = (size_t)k.mAsset.getDigest64();
        h = h * 1099511628211ull ^ (size_t)k.mSkin;
        h = h * 1099511628211ull ^ (size_t)(U32)k.mDetail;
        h = h * 1099511628211ull ^ (size_t)k.mFace;
        return h;
    }
};
// what a shared store would hold for one face of one mesh at one detail with one skin: the asset's arrays, none of the instance's
struct Entry
{
    S32 mVerts = 0, mIndices = 0;
    std::vector<F32> mPos, mNorm, mWeights, mUv, mTan;  // 3, 3, 4, 2 and 4 floats a vertex (the tangents: only when the volume face has them)
    std::vector<U16> mIdx;
    U64 mBytes = 0;
    std::atomic<U64> mUses{ 0 };
};

std::mutex sMutex;
std::unordered_map<Key, std::shared_ptr<Entry>, KeyHash> sStore;
std::unordered_set<const LLFace*> sFacesSeen;       // distinct face objects (a rebuild of the same face is another compare, not another instance); the address of a freed face can come back: an upper bound is not claimed, this is approximate
std::unordered_map<const Entry*, std::unordered_set<const LLFace*>> sUsers;     // the faces that used each entry

struct Stats
{
    std::atomic<U64> mFaces{ 0 }, mSkipped{ 0 }, mEntries{ 0 }, mReused{ 0 }, mFaceBytes{ 0 }, mStoreBytes{ 0 }, mClassFaces{ 0 }, mMatFaces{ 0 }, mNoNormals{ 0 }, mDistinctBytes{ 0 };
    std::atomic<U64> mCompared[7]{}, mDiffer[7]{};      // pos, normal, weights, indices, uv (all channels), colour, tangent
    std::atomic<U64> mReason[7]{}, mReasonOnly[7]{}, mReasonNone{ 0 }, mAlphaAndMaterial{ 0 }, mAlphaAndGltf{ 0 };       // texgen, alpha pool, material, glTF, bump, texture animation, texture matrix
    std::atomic<U32> mLogged{ 0 };
} sStats;
const char* const kArray[7] = { "position", "normal", "weight", "index", "texcoord", "colour", "tangent" };

std::shared_ptr<Entry> build_entry(const LLVolumeFace& vf, const LLMeshSkinInfo* skin)
{
    std::shared_ptr<Entry> e = std::make_shared<Entry>();
    const S32 n = vf.mNumVertices, m = vf.mNumIndices;
    e->mVerts = n;
    e->mIndices = m;
    e->mPos.resize((size_t)3 * n);
    e->mNorm.resize((size_t)3 * n);
    e->mUv.resize((size_t)2 * n);
    if (vf.mWeights)
    {
        e->mWeights.resize((size_t)4 * n);
    }
    if (vf.mTangents)
    {
        e->mTan.resize((size_t)4 * n);
    }
    e->mIdx.assign(vf.mIndices, vf.mIndices + m);
    const LLMatrix4a& bind = skin->mBindShapeMatrix;
    LLMatrix4a normal_matrix;
    {
        glm::mat4 g = glm::make_mat4((F32*)bind.getF32ptr());
        g = glm::transpose(glm::inverse(g));
        normal_matrix.loadu(glm::value_ptr(g));
    }
    for (S32 i = 0; i < n; ++i)
    {
        LLVector4a p, q;
        bind.affineTransform(vf.mPositions[i], p);
        normal_matrix.rotate(vf.mNormals[i], q);
        const F32* pf = p.getF32ptr();
        const F32* qf = q.getF32ptr();
        for (int k = 0; k < 3; ++k)
        {
            e->mPos[(size_t)3 * i + k] = pf[k];
            e->mNorm[(size_t)3 * i + k] = qf[k];
        }
        if (vf.mTangents)
        {
            LLVector4a t;
            normal_matrix.rotate(vf.mTangents[i], t);
            const F32* tf = t.getF32ptr();
            const F32* sf = vf.mTangents[i].getF32ptr();
            for (int k = 0; k < 3; ++k)
            {
                e->mTan[(size_t)4 * i + k] = tf[k];
            }
            e->mTan[(size_t)4 * i + 3] = sf[3];     // (the handedness stays the source's)
        }
        e->mUv[(size_t)2 * i] = vf.mTexCoords[i].mV[0];
        e->mUv[(size_t)2 * i + 1] = vf.mTexCoords[i].mV[1];
        if (vf.mWeights)
        {
            const F32* wf = vf.mWeights[i].getF32ptr();
            for (int k = 0; k < 4; ++k)
            {
                e->mWeights[(size_t)4 * i + k] = wf[k];
            }
        }
    }
    e->mBytes = (U64)n * (3 + 3 + 2 + (vf.mWeights ? 4 : 0) + (vf.mTangents ? 4 : 0)) * sizeof(F32) + (U64)m * sizeof(U16);
    return e;
}

inline bool differs(F32 a, F32 b)
{
    if (std::isnan(a) && std::isnan(b))
    {
        return false;       // (a mesh with a bad normal has it on both sides: the face got the same value the store would hold)
    }
    const F32 scale = std::max(1.f, std::max(fabsf(a), fabsf(b)));
    return !(fabsf(a - b) <= 1e-5f * scale);
}

void note_mismatch(int array, const Key& key, S32 vertex, F32 store, F32 face)
{
    const U32 n = ++sStats.mLogged;
    if (n <= 12)
    {
        LL_WARNS("RigStore") << "rigged face differs from the store: " << kArray[array] << " of vertex " << vertex << ", mesh " << key.mAsset << " detail " << key.mDetail << " face " << key.mFace
                             << " skin " << key.mSkin << ": store " << store << ", face " << face << LL_ENDL;
    }
}
}

void rk_rigstore_compare(LLFace* face, const LLVolume& volume, U32 te_idx)
{
    ++sStats.mFaces;
    LLVertexBuffer* vb = face->getVertexBuffer();
    const LLMeshSkinInfo* skin = face->mSkinInfo.get();
    if (!vb || !skin || (S32)te_idx >= volume.getNumVolumeFaces())
    {
        ++sStats.mSkipped;
        return;
    }
    const LLVolumeFace& vf = volume.getVolumeFace((S32)te_idx);
    const S32 n = std::min<S32>(vf.mNumVertices, (S32)face->getGeomCount());
    const S32 m = std::min<S32>(vf.mNumIndices, (S32)face->getIndicesCount());
    if (n <= 0 || !vf.mPositions || !vf.mNormals || !vf.mTexCoords)
    {
        ++sStats.mSkipped;
        return;
    }
    Key key;
    key.mAsset = volume.getParams().getSculptID();
    key.mDetail = (S32)lroundf(volume.getDetail() * 1000.f);
    key.mSkin = skin->mHash;
    key.mFace = te_idx;

    std::shared_ptr<Entry> entry;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        auto it = sStore.find(key);
        if (it != sStore.end())
        {
            entry = it->second;
        }
    }
    if (entry)
    {
        ++sStats.mReused;
    }
    else
    {
        std::shared_ptr<Entry> made = build_entry(vf, skin);
        std::lock_guard<std::mutex> lock(sMutex);
        auto ins = sStore.emplace(key, made);
        entry = ins.first->second;
        if (ins.second)
        {
            ++sStats.mEntries;
            sStats.mStoreBytes += entry->mBytes;
        }
        else
        {
            ++sStats.mReused;
        }
    }
    ++entry->mUses;
    sStats.mFaceBytes += entry->mBytes;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        if (sFacesSeen.insert(face).second)
        {
            sStats.mDistinctBytes += entry->mBytes;     // (the bytes of the distinct faces' shared arrays: what the faces hold, once each)
        }
        sUsers[entry.get()].insert(face);
    }
    if (entry->mVerts != vf.mNumVertices || entry->mIndices != vf.mNumIndices)
    {
        // the same key with another vertex count: the key does not name the geometry (counted as a position difference)
        ++sStats.mDiffer[0];
        note_mismatch(0, key, -1, (F32)entry->mVerts, (F32)vf.mNumVertices);
        return;
    }

    // positions (xyz; the w is the batch's texture index) and normals
    // (every array is asked for only when the face's buffer has it: a fullbright face has no normals, and a strider on a missing array dereferences an invalid pointer)
    if (vb->hasDataType(LLVertexBuffer::TYPE_VERTEX))
    {
        LLStrider<LLVector4a> pos;
        if (vb->getVertexStrider(pos, face->getGeomIndex(), face->getGeomCount()))
        {
            for (S32 i = 0; i < n; ++i)
            {
                const F32* pf = pos[i].getF32ptr();
                for (int k = 0; k < 3; ++k)
                {
                    ++sStats.mCompared[0];
                    if (differs(entry->mPos[(size_t)3 * i + k], pf[k]))
                    {
                        ++sStats.mDiffer[0];
                        note_mismatch(0, key, i, entry->mPos[(size_t)3 * i + k], pf[k]);
                    }
                }
            }
        }
    }
    if (vb->hasDataType(LLVertexBuffer::TYPE_NORMAL))
    {
        LLStrider<LLVector4a> norm;
        if (vb->getNormalStrider(norm, face->getGeomIndex(), face->getGeomCount()))
        {
            for (S32 i = 0; i < n; ++i)
            {
                const F32* nf = norm[i].getF32ptr();
                for (int k = 0; k < 3; ++k)
                {
                    ++sStats.mCompared[1];
                    if (differs(entry->mNorm[(size_t)3 * i + k], nf[k]))
                    {
                        ++sStats.mDiffer[1];
                        note_mismatch(1, key, i, entry->mNorm[(size_t)3 * i + k], nf[k]);
                    }
                }
            }
        }
    }
    else
    {
        ++sStats.mNoNormals;
    }
    // weights
    if (!entry->mWeights.empty() && vb->hasDataType(LLVertexBuffer::TYPE_WEIGHT4))
    {
        LLStrider<LLVector4a> wght;
        if (vb->getWeight4Strider(wght, face->getGeomIndex(), face->getGeomCount()))
        {
            for (S32 i = 0; i < n; ++i)
            {
                const F32* wf = wght[i].getF32ptr();
                for (int k = 0; k < 4; ++k)
                {
                    ++sStats.mCompared[2];
                    if (differs(entry->mWeights[(size_t)4 * i + k], wf[k]))
                    {
                        ++sStats.mDiffer[2];
                        note_mismatch(2, key, i, entry->mWeights[(size_t)4 * i + k], wf[k]);
                    }
                }
            }
        }
    }
    // tangents (a face with a normal or specular map)
    if (!entry->mTan.empty() && vb->hasDataType(LLVertexBuffer::TYPE_TANGENT))
    {
        LLStrider<LLVector4a> tan;
        if (vb->getTangentStrider(tan, face->getGeomIndex(), face->getGeomCount()))
        {
            for (S32 i = 0; i < n; ++i)
            {
                const F32* tf = tan[i].getF32ptr();
                for (int k = 0; k < 4; ++k)
                {
                    ++sStats.mCompared[6];
                    if (differs(entry->mTan[(size_t)4 * i + k], tf[k]))
                    {
                        ++sStats.mDiffer[6];
                        note_mismatch(6, key, i, entry->mTan[(size_t)4 * i + k], tf[k]);
                    }
                }
            }
        }
    }
    // indices: the face's carry the offset of its first vertex in the batch's buffer
    {
        LLStrider<U16> idx;
        if (vb->getIndexStrider(idx, face->getIndicesStart(), face->getIndicesCount()))
        {
            const U16 base = (U16)face->getGeomIndex();
            for (S32 i = 0; i < m; ++i)
            {
                ++sStats.mCompared[3];
                if ((U16)(entry->mIdx[i] + base) != idx[i])
                {
                    ++sStats.mDiffer[3];
                    note_mismatch(3, key, i, (F32)(U16)(entry->mIdx[i] + base), (F32)idx[i]);
                }
            }
        }
    }
    // the stage 1 class: default texgen, opaque (not in the alpha pool), no material, no glTF, no bump: the colour and the texture coordinates are the texture entry's alone
    const LLTextureEntry* te = face->getTextureEntry();
    // the census of why a face is outside the class (RK_VK_RIGSTORE=1): each condition counted when true, and counted again when it is the only one true (what lifting it alone would add)
    if (te)
    {
        const bool c[7] = { te->getTexGen() != LLTextureEntry::TEX_GEN_DEFAULT, face->isInAlphaPool(), !te->getMaterialParams().isNull(), te->getGLTFRenderMaterial() != nullptr, te->getBumpmap() != 0,
                            face->isState(LLFace::TEXTURE_ANIM), face->mTextureMatrix != nullptr };
        int count = 0, only = -1;
        for (int k = 0; k < 7; ++k)
        {
            if (c[k])
            {
                ++sStats.mReason[k];
                ++count;
                only = k;
            }
        }
        if (count == 0)
        {
            ++sStats.mReasonNone;
        }
        else if (count == 1)
        {
            ++sStats.mReasonOnly[only];
        }
        if (c[1] && c[2])
        {
            ++sStats.mAlphaAndMaterial;
        }
        if (c[1] && c[3])
        {
            ++sStats.mAlphaAndGltf;
        }
    }
    // the class of stage 1 as the census put it: a legacy material or none, default texgen, not in the alpha pool, no glTF, no bump, no texture animation
    if (te && te->getTexGen() == LLTextureEntry::TEX_GEN_DEFAULT && !face->isInAlphaPool() && te->getGLTFRenderMaterial() == nullptr && te->getBumpmap() == 0
        && !face->isState(LLFace::TEXTURE_ANIM) && !face->mTextureMatrix)
    {
        ++sStats.mClassFaces;
        const LLMaterial* mat = te->getMaterialParams().get();
        sStats.mMatFaces += mat ? 1 : 0;
        F32 os = 0.f, ot = 0.f, ms = 1.f, mt = 1.f;
        const F32 r = te->getRotation();
        te->getOffset(&os, &ot);
        te->getScale(&ms, &mt);
        const F32 c = cosf(r), s = sinf(r);
        LLStrider<LLVector2> tc;
        if (vb->hasDataType(LLVertexBuffer::TYPE_TEXCOORD0) && vb->getTexCoord0Strider(tc, face->getGeomIndex(), face->getGeomCount()))
        {
            for (S32 i = 0; i < n; ++i)
            {
                // the matrix form of the face's xform: about the centre of the face, rotated, scaled, then offset
                const F32 u = entry->mUv[(size_t)2 * i] - 0.5f, v = entry->mUv[(size_t)2 * i + 1] - 0.5f;
                const F32 eu = (c * u + s * v) * ms + os + 0.5f;
                const F32 ev = (-s * u + c * v) * mt + ot + 0.5f;
                ++sStats.mCompared[4];
                if (differs(eu, tc[i].mV[0]))
                {
                    ++sStats.mDiffer[4];
                    note_mismatch(4, key, i, eu, tc[i].mV[0]);
                }
                ++sStats.mCompared[4];
                if (differs(ev, tc[i].mV[1]))
                {
                    ++sStats.mDiffer[4];
                    note_mismatch(4, key, i, ev, tc[i].mV[1]);
                }
            }
        }
        LLStrider<LLColor4U> col;
        if (vb->hasDataType(LLVertexBuffer::TYPE_COLOR) && vb->getColorStrider(col, face->getGeomIndex(), face->getGeomCount()))
        {
            const LLColor4U want(te->getColor());
            for (S32 i = 0; i < n; ++i)
            {
                for (int k = 0; k < 3; ++k)
                {
                    ++sStats.mCompared[5];
                    if (abs((int)want.mV[k] - (int)col[i].mV[k]) > 1)
                    {
                        ++sStats.mDiffer[5];
                        note_mismatch(5, key, i, (F32)want.mV[k], (F32)col[i].mV[k]);
                    }
                }
            }
        }
        if (mat)
        {
            // the normal map's and the specular map's texture coordinates: the same transform of the base coordinates with the material's own offset, repeat and rotation
            auto channel = [&](bool normal_map)
            {
                F32 mo = 0.f, mp = 0.f, mrs = 1.f, mrt = 1.f, mr = 0.f;
                if (normal_map)
                {
                    mat->getNormalOffset(mo, mp);
                    mat->getNormalRepeat(mrs, mrt);
                    mr = mat->getNormalRotation();
                }
                else
                {
                    mat->getSpecularOffset(mo, mp);
                    mat->getSpecularRepeat(mrs, mrt);
                    mr = mat->getSpecularRotation();
                }
                const F32 mc = cosf(mr), ms2 = sinf(mr);
                LLStrider<LLVector2> dst;
                const LLVertexBuffer::AttributeType type = normal_map ? LLVertexBuffer::TYPE_TEXCOORD1 : LLVertexBuffer::TYPE_TEXCOORD2;
                if (!vb->hasDataType(type))
                {
                    return;
                }
                const bool got = normal_map ? vb->getTexCoord1Strider(dst, face->getGeomIndex(), face->getGeomCount()) : vb->getTexCoord2Strider(dst, face->getGeomIndex(), face->getGeomCount());
                if (!got)
                {
                    return;
                }
                for (S32 i = 0; i < n; ++i)
                {
                    const F32 u = entry->mUv[(size_t)2 * i] - 0.5f, v = entry->mUv[(size_t)2 * i + 1] - 0.5f;
                    const F32 eu = (mc * u + ms2 * v) * mrs + mo + 0.5f;
                    const F32 ev = (-ms2 * u + mc * v) * mrt + mp + 0.5f;
                    ++sStats.mCompared[4];
                    if (differs(eu, dst[i].mV[0]))
                    {
                        ++sStats.mDiffer[4];
                        note_mismatch(4, key, i, eu, dst[i].mV[0]);
                    }
                    ++sStats.mCompared[4];
                    if (differs(ev, dst[i].mV[1]))
                    {
                        ++sStats.mDiffer[4];
                        note_mismatch(4, key, i, ev, dst[i].mV[1]);
                    }
                }
            };
            channel(true);
            channel(false);
        }
    }

    const U64 faces = sStats.mFaces.load();
    if (faces % 4000 == 0)
    {
        std::string diff;
        for (int a = 0; a < 7; ++a)
        {
            diff += std::string(kArray[a]) + " " + std::to_string(sStats.mDiffer[a].load()) + "/" + std::to_string(sStats.mCompared[a].load()) + (a < 6 ? ", " : "");
        }
        static const char* const kReason[7] = { "non-default texgen", "alpha pool", "material", "glTF", "bump", "texture animation", "texture matrix" };
        std::string reasons;
        for (int k = 0; k < 7; ++k)
        {
            reasons += std::string(kReason[k]) + " " + std::to_string(sStats.mReason[k].load()) + " (alone " + std::to_string(sStats.mReasonOnly[k].load()) + ")" + (k < 6 ? ", " : "");
        }
        RK_DIAG("RigStore") << "why rigged faces are outside the class, of " << faces << " compares: none (in the class) " << sStats.mReasonNone.load() << "; " << reasons << "; alpha and material " << sStats.mAlphaAndMaterial.load()
                            << ", alpha and glTF " << sStats.mAlphaAndGltf.load() << LL_ENDL;
        size_t distinct_faces = 0, used_entries = 0, multi_entries = 0;
        {
            std::lock_guard<std::mutex> lock(sMutex);
            distinct_faces = sFacesSeen.size();
            used_entries = sUsers.size();
            for (const auto& kv : sUsers)
            {
                multi_entries += kv.second.size() >= 2 ? 1 : 0;
            }
        }
        RK_DIAG("RigStore") << "rigged faces compared with the store: " << faces << " (" << sStats.mSkipped.load() << " skipped, " << sStats.mClassFaces.load() << " of the stage 1 class, " << sStats.mMatFaces.load() << " of them with a material); store entries " << sStats.mEntries.load()
                            << ", faces without normals " << sStats.mNoNormals.load() << ", compares that found their entry already there " << sStats.mReused.load() << ", distinct face objects " << distinct_faces << " using "
                            << used_entries << " entries (" << multi_entries << " used by two or more distinct faces), distinct faces' shared arrays " << sStats.mDistinctBytes.load() / 1048576.0 << " MB against the store's "
                            << sStats.mStoreBytes.load() / 1048576.0 << " MB (every compare: " << sStats.mFaceBytes.load() / 1048576.0 << " MB); differing / compared: " << diff << LL_ENDL;
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// D4 stage 1b-3: the cache of baked rigged face buffers (see rkrigstore.h)
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
namespace
{
struct CKey
{
    LLUUID mAsset;
    S32 mDetail = 0;
    U64 mSkin = 0;
    U32 mFace = 0, mMask = 0, mColour = 0, mGlowAndFlags = 0;
    F32 mX[3][5] = {};          // the texture coordinate transforms: the texture entry's, the material's normal map and specular map: rotation, offset s and t, repeat s and t
    bool operator==(const CKey& o) const
    {
        return mAsset == o.mAsset && mDetail == o.mDetail && mSkin == o.mSkin && mFace == o.mFace && mMask == o.mMask && mColour == o.mColour && mGlowAndFlags == o.mGlowAndFlags && memcmp(mX, o.mX, sizeof(mX)) == 0;
    }
};
struct CKeyHash
{
    size_t operator()(const CKey& k) const
    {
        size_t h = (size_t)k.mAsset.getDigest64();
        auto mix = [&h](size_t v) { h = h * 1099511628211ull ^ v; };
        mix((size_t)k.mSkin);
        mix((size_t)(U32)k.mDetail);
        mix(k.mFace);
        mix(k.mMask);
        mix(k.mColour);
        mix(k.mGlowAndFlags);
        for (int a = 0; a < 3; ++a)
        {
            for (int b = 0; b < 5; ++b)
            {
                U32 bits;
                memcpy(&bits, &k.mX[a][b], 4);
                mix(bits);
            }
        }
        return h;
    }
};

// everything the bake of a rigged face's arrays depends on (llface.cpp getGeometryVolume): the asset and the skin (positions, normals, tangents, weights, indices, base texcoords), the colour with the shiny in alpha
// rule, the glow, and the three texture transforms. The texture itself is not in it (it is the draw's, not the buffer's), nor is the texture index (0 for a face that is not batched).
bool make_key(const LLFace* face, U32 mask, CKey& k)
{
    const LLTextureEntry* te = face->getTextureEntry();
    LLViewerObject* vobj = face->getViewerObject();
    const LLVolume* volume = vobj ? vobj->getVolume() : nullptr;
    const LLMeshSkinInfo* skin = face->mSkinInfo.get();
    if (!te || !volume || !skin)
    {
        return false;
    }
    k.mAsset = volume->getParams().getSculptID();
    k.mDetail = (S32)lroundf(volume->getDetail() * 1000.f);
    k.mSkin = skin->mHash;
    k.mFace = (U32)face->getTEOffset();
    k.mMask = mask;
    const LLMaterial* mat = te->getMaterialParams().get();
    LLColor4U colour(te->getColor());
    if (!face->isInAlphaPool() && te->getGLTFRenderMaterial() == nullptr && (!mat || mat->getSpecularID().isNull()))
    {
        static const F32 kShinyAlpha[4] = { 0.0000f, 0.25f, 0.5f, 0.75f };
        colour.mV[3] = (U8)(kShinyAlpha[te->getShiny() & 3] * 255);       // (the shininess goes in the alpha of the colour when there is no specular map: getGeometryVolume)
    }
    k.mColour = colour.asRGBA();
    const U32 glow = (U32)llclamp((S32)(te->getGlow() * 255), 0, 255);
    U32 flags = 0;
    if (mat)
    {
        flags |= 1u;
        flags |= mat->getNormalID().isNull() ? 0u : 2u;
        flags |= mat->getSpecularID().isNull() ? 0u : 4u;
    }
    flags |= face->isInAlphaPool() ? 8u : 0u;       // (the colour's alpha channel is baked by another rule in the alpha pool)
    k.mGlowAndFlags = glow | (flags << 8);
    F32 os = 0.f, ot = 0.f, ms = 1.f, mt = 1.f;
    te->getOffset(&os, &ot);
    te->getScale(&ms, &mt);
    const F32 tx[5] = { te->getRotation(), os, ot, ms, mt };
    memcpy(k.mX[0], tx, sizeof(tx));
    if (mat)
    {
        F32 a = 0.f, b = 0.f, c = 1.f, d = 1.f;
        mat->getNormalOffset(a, b);
        mat->getNormalRepeat(c, d);
        const F32 nx[5] = { mat->getNormalRotation(), a, b, c, d };
        memcpy(k.mX[1], nx, sizeof(nx));
        mat->getSpecularOffset(a, b);
        mat->getSpecularRepeat(c, d);
        const F32 sx[5] = { mat->getSpecularRotation(), a, b, c, d };
        memcpy(k.mX[2], sx, sizeof(sx));
    }
    return true;
}

std::unordered_map<CKey, LLPointer<LLVertexBuffer>, CKeyHash> sRegistry;       // main thread only: the cache's buffers (a strong pointer each: a buffer lives while a face holds it or the registry does; the sweep gives up those nobody holds)
U32 sLastSweep = 0;
struct CacheStats
{
    std::atomic<U64> mHits{ 0 }, mMisses{ 0 }, mIneligible{ 0 }, mMismatch{ 0 }, mReleased{ 0 }, mSwept{ 0 }, mBlocked{ 0 }, mHitVerts{ 0 }, mFillVerts{ 0 }, mPlans{ 0 };
} sCache;
}

extern U32 gFrameCount;

int rk_rigstore_mode()
{
    static const int mode = getenv("RK_VK_RIGSTORE") ? atoi(getenv("RK_VK_RIGSTORE")) : 0;
    return mode;
}

bool rk_rigcache_eligible(LLFace* face, bool distance_sort)
{
    if (distance_sort || !face->isState(LLFace::RIGGED) || face->mSkinInfo.isNull() || face->getGeomCount() == 0)
    {
        return false;
    }
    const LLTextureEntry* te = face->getTextureEntry();
    if (!te || te->getTexGen() != LLTextureEntry::TEX_GEN_DEFAULT || te->getGLTFRenderMaterial() != nullptr || te->getBumpmap() != 0)
    {
        return false;
    }
    if (!distance_sort && (face->isInAlphaPool() || te->getColor().mV[3] < 0.999f))
    {
        return false;       // (an opaque kind's face is not transparent; the alpha kind's faces are, and are cached too since stage 2)
    }
    if (face->isState(LLFace::TEXTURE_ANIM) || face->mTextureMatrix || face->hasSelectionBuffer())
    {
        return false;
    }
    LLViewerObject* vobj = face->getViewerObject();
    LLDrawable* drawable = face->getDrawable();
    return vobj && !vobj->isSelected() && drawable && !drawable->isState(LLDrawable::ANIMATED_CHILD);
}

int rk_rigcache_plan(LLFace* face, U32 mask, bool batch_textures, LLPointer<LLVertexBuffer>& buffer)
{
    CKey key;
    if (!make_key(face, mask, key))
    {
        ++sCache.mIneligible;
        return 0;
    }
    const U32 verts = face->getGeomCount(), indices = face->getIndicesCount();
    auto it = sRegistry.find(key);
    if (it != sRegistry.end())
    {
        LLVertexBuffer* vb = it->second.get();
        if (vb->getNumVerts() != verts || vb->getNumIndices() != indices)
        {
            ++sCache.mMismatch;         // (the key does not name the geometry: the face goes the normal way)
            return 0;
        }
        face->setVertexBuffer(vb);
        face->setGeomIndex(0);
        face->setIndicesIndex(0);
        face->setTextureIndex(batch_textures ? 0 : FACE_DO_NOT_BATCH_TEXTURES);
        face->mRigStore = true;
        ++sCache.mHits;
        sCache.mHitVerts += verts;
        buffer = nullptr;
        return 1;
    }
    LLPointer<LLVertexBuffer> vb = new LLVertexBuffer(mask);
    if (!vb->allocateBuffer(verts, indices))
    {
        ++sCache.mIneligible;
        return 0;
    }
    sRegistry.emplace(key, vb);
    buffer = vb;
    face->setTextureIndex(batch_textures ? 0 : FACE_DO_NOT_BATCH_TEXTURES);
    face->mRigStore = true;
    ++sCache.mMisses;
    sCache.mFillVerts += verts;
    const U64 plans = ++sCache.mPlans + sCache.mHits.load();
    (void)plans;
    static const bool diag = getenv("RK_VK_DIAG") != nullptr;
    if (diag && (sCache.mMisses.load() % 500) == 0)
    {
        RK_DIAG("RigCache") << "rigged face buffers: " << sCache.mHits.load() << " faces attached to a cached buffer (" << sCache.mHitVerts.load() << " vertices not filled), " << sCache.mMisses.load() << " buffers made and filled ("
                            << sCache.mFillVerts.load() << " vertices), " << sRegistry.size() << " in the registry, released " << sCache.mReleased.load() << ", swept " << sCache.mSwept.load() << ", writes blocked by the guard "
                            << sCache.mBlocked.load() << ", key mismatches " << sCache.mMismatch.load() << ", not handled " << sCache.mIneligible.load() << LL_ENDL;
    }
    return 2;
}

void rk_rigcache_release(LLFace* face)
{
    if (face->mRigStore)
    {
        face->setVertexBuffer(nullptr);
        face->mRigStore = false;
        ++sCache.mReleased;
    }
}

void rk_rigcache_sweep()
{
    if (gFrameCount - sLastSweep < 300)
    {
        return;
    }
    sLastSweep = gFrameCount;
    for (auto it = sRegistry.begin(); it != sRegistry.end(); )
    {
        if (it->second->getNumRefs() == 1)
        {
            it = sRegistry.erase(it);
            ++sCache.mSwept;
        }
        else
        {
            ++it;
        }
    }
}

void rk_rigstore_note_blocked()
{
    ++sCache.mBlocked;
}

// ---- mode 3: the key check ----
namespace
{
struct Snap
{
    U32 mVerts = 0;
    U32 mReal = 0;                  // the vertices the volume face really has (the rest of the face's range is padding no array write fills, except the positions)
    std::vector<F32> mA[9];         // position (3), normal (3), texcoord 0 (2), colour (4), emissive (4), tangent (4), weights (4), texcoord 1 (2), texcoord 2 (2)
    std::vector<U16> mIdx;          // minus the face's first vertex
};
std::mutex sVerifyMutex;
std::unordered_map<CKey, std::shared_ptr<Snap>, CKeyHash> sSnaps;
struct VerifyStats
{
    std::atomic<U64> mFaces{ 0 }, mFirst{ 0 }, mCountDiffer{ 0 };
    std::atomic<U64> mCompared[10]{}, mDiffer[10]{}, mTailDiffer[10]{};
    std::atomic<U32> mLogged{ 0 };
} sVerify;
const char* const kVArr[10] = { "position", "normal", "texcoord0", "colour", "emissive", "tangent", "weight", "texcoord1", "texcoord2", "index" };

bool read_snap(LLFace* face, Snap& s)
{
    LLVertexBuffer* vb = face->getVertexBuffer();
    if (!vb)
    {
        return false;
    }
    const U32 gi = face->getGeomIndex(), gc = face->getGeomCount();
    s.mVerts = gc;
    {
        const LLVolume* volume = face->getViewerObject() ? face->getViewerObject()->getVolume() : nullptr;
        s.mReal = gc;
        if (volume && face->getTEOffset() >= 0 && face->getTEOffset() < volume->getNumVolumeFaces())
        {
            s.mReal = std::min<U32>(gc, (U32)volume->getVolumeFace(face->getTEOffset()).mNumVertices);
        }
    }
    if (vb->hasDataType(LLVertexBuffer::TYPE_VERTEX))
    {
        LLStrider<LLVector4a> p;
        if (vb->getVertexStrider(p, gi, gc))
        {
            s.mA[0].resize((size_t)3 * gc);
            for (U32 i = 0; i < gc; ++i) { const F32* f = p[i].getF32ptr(); for (int k = 0; k < 3; ++k) { s.mA[0][(size_t)3 * i + k] = f[k]; } }
        }
    }
    if (vb->hasDataType(LLVertexBuffer::TYPE_NORMAL))
    {
        LLStrider<LLVector4a> p;
        if (vb->getNormalStrider(p, gi, gc))
        {
            s.mA[1].resize((size_t)3 * gc);
            for (U32 i = 0; i < gc; ++i) { const F32* f = p[i].getF32ptr(); for (int k = 0; k < 3; ++k) { s.mA[1][(size_t)3 * i + k] = f[k]; } }
        }
    }
    auto read_uv = [&](LLVertexBuffer::AttributeType type, int slot)
    {
        LLStrider<LLVector2> t;
        if (!vb->hasDataType(type))
        {
            return;
        }
        const bool got = type == LLVertexBuffer::TYPE_TEXCOORD0 ? vb->getTexCoord0Strider(t, gi, gc) : (type == LLVertexBuffer::TYPE_TEXCOORD1 ? vb->getTexCoord1Strider(t, gi, gc) : vb->getTexCoord2Strider(t, gi, gc));
        if (!got)
        {
            return;
        }
        s.mA[slot].resize((size_t)2 * gc);
        for (U32 i = 0; i < gc; ++i) { s.mA[slot][(size_t)2 * i] = t[i].mV[0]; s.mA[slot][(size_t)2 * i + 1] = t[i].mV[1]; }
    };
    read_uv(LLVertexBuffer::TYPE_TEXCOORD0, 2);
    read_uv(LLVertexBuffer::TYPE_TEXCOORD1, 7);
    read_uv(LLVertexBuffer::TYPE_TEXCOORD2, 8);
    if (vb->hasDataType(LLVertexBuffer::TYPE_COLOR))
    {
        LLStrider<LLColor4U> c;
        if (vb->getColorStrider(c, gi, gc))
        {
            s.mA[3].resize((size_t)4 * gc);
            for (U32 i = 0; i < gc; ++i) { for (int k = 0; k < 4; ++k) { s.mA[3][(size_t)4 * i + k] = (F32)c[i].mV[k]; } }
        }
    }
    if (vb->hasDataType(LLVertexBuffer::TYPE_EMISSIVE))
    {
        LLStrider<LLColor4U> c;
        if (vb->getEmissiveStrider(c, gi, gc))
        {
            s.mA[4].resize((size_t)4 * gc);
            for (U32 i = 0; i < gc; ++i) { for (int k = 0; k < 4; ++k) { s.mA[4][(size_t)4 * i + k] = (F32)c[i].mV[k]; } }
        }
    }
    if (vb->hasDataType(LLVertexBuffer::TYPE_TANGENT))
    {
        LLStrider<LLVector4a> p;
        if (vb->getTangentStrider(p, gi, gc))
        {
            s.mA[5].resize((size_t)4 * gc);
            for (U32 i = 0; i < gc; ++i) { const F32* f = p[i].getF32ptr(); for (int k = 0; k < 4; ++k) { s.mA[5][(size_t)4 * i + k] = f[k]; } }
        }
    }
    if (vb->hasDataType(LLVertexBuffer::TYPE_WEIGHT4))
    {
        LLStrider<LLVector4a> p;
        if (vb->getWeight4Strider(p, gi, gc))
        {
            s.mA[6].resize((size_t)4 * gc);
            for (U32 i = 0; i < gc; ++i) { const F32* f = p[i].getF32ptr(); for (int k = 0; k < 4; ++k) { s.mA[6][(size_t)4 * i + k] = f[k]; } }
        }
    }
    LLStrider<U16> idx;
    if (vb->getIndexStrider(idx, face->getIndicesStart(), face->getIndicesCount()))
    {
        s.mIdx.resize(face->getIndicesCount());
        for (U32 i = 0; i < face->getIndicesCount(); ++i) { s.mIdx[i] = (U16)(idx[i] - (U16)gi); }
    }
    return true;
}
}

void rk_rigcache_verify(LLFace* face, U32 mask)
{
    CKey key;
    if (!rk_rigcache_eligible(face, face->isInAlphaPool()) || !make_key(face, mask, key))
    {
        return;
    }
    const U64 faces = ++sVerify.mFaces;
    std::shared_ptr<Snap> snap = std::make_shared<Snap>();
    if (!read_snap(face, *snap))
    {
        return;
    }
    std::shared_ptr<Snap> ref;
    {
        std::lock_guard<std::mutex> lock(sVerifyMutex);
        auto it = sSnaps.find(key);
        if (it == sSnaps.end())
        {
            sSnaps.emplace(key, snap);
            ++sVerify.mFirst;
            return;
        }
        ref = it->second;
    }
    if (ref->mVerts != snap->mVerts)
    {
        ++sVerify.mCountDiffer;
        return;
    }
    for (int a = 0; a < 9; ++a)
    {
        if (ref->mA[a].size() != snap->mA[a].size())
        {
            ++sVerify.mDiffer[a];
            continue;
        }
        static const int kComps[9] = { 3, 3, 2, 4, 4, 4, 4, 2, 2 };
        const size_t real_floats = (size_t)std::min(ref->mReal, snap->mReal) * kComps[a];
        for (size_t i = 0; i < ref->mA[a].size(); ++i)
        {
            if (i >= real_floats)
            {
                sVerify.mTailDiffer[a] += differs(ref->mA[a][i], snap->mA[a][i]) ? 1 : 0;       // (the padding after the real vertices: not what a draw reads)
                continue;
            }
            ++sVerify.mCompared[a];
            if (differs(ref->mA[a][i], snap->mA[a][i]))
            {
                ++sVerify.mDiffer[a];
                if (++sVerify.mLogged <= 12)
                {
                    LL_WARNS("RigCache") << "faces with the same key differ: " << kVArr[a] << " float " << i << ": first face " << ref->mA[a][i] << ", this face " << snap->mA[a][i] << ", mesh " << key.mAsset << " detail " << key.mDetail
                                         << " face " << key.mFace << " skin " << key.mSkin << " mask " << key.mMask << LL_ENDL;
                }
            }
        }
    }
    for (size_t i = 0; i < ref->mIdx.size() && i < snap->mIdx.size(); ++i)
    {
        ++sVerify.mCompared[9];
        sVerify.mDiffer[9] += ref->mIdx[i] != snap->mIdx[i] ? 1 : 0;
    }
    if (faces % 4000 == 0)
    {
        std::string text;
        for (int a = 0; a < 10; ++a)
        {
            text += std::string(kVArr[a]) + " " + std::to_string(sVerify.mDiffer[a].load()) + "/" + std::to_string(sVerify.mCompared[a].load()) + (a < 9 ? " (tail " + std::to_string(sVerify.mTailDiffer[a].load()) + "), " : "");
        }
        RK_DIAG("RigCache") << "faces with the same key compared: " << faces << " faces seen, " << sVerify.mFirst.load() << " keys, vertex count differs " << sVerify.mCountDiffer.load() << "; differing / compared: " << text << LL_ENDL;
    }
}
