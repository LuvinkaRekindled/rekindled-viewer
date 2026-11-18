/**
 * @file llvertexbuffer.cpp
 * @brief LLVertexBuffer implementation
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

#include "linden_common.h"

#include "llfasttimer.h"
#include "llsys.h"
#include <mutex>
#include "llvertexbuffer.h"
#include "llretirelist.h"
// #include "llrender.h"
#include "llrenderheaders.h"
#if defined(LL_VULKAN)
#include "llvkasync.h"
#include "llvkbuf.h"
#endif
#include "llrender.h"
#include "llvector4a.h"
#include "llshadermgr.h"
#include "llshaderprogram.h"
#include "llmemory.h"
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include "llvkbridge.h"

//Next Highest Power Of Two
//helper function, returns first number > v that is a power of 2, or v if v is already a power of 2
U32 nhpo2(U32 v)
{
    U32 r = 1;
    while (r < v) {
        r *= 2;
    }
    return r;
}

//which power of 2 is i?
//assumes i is a power of 2 > 0
U32 wpo2(U32 i)
{
    llassert(i > 0);
    llassert(nhpo2(i) == i);

    U32 r = 0;

    while (i >>= 1) ++r;

    return r;
}

struct CompareMappedRegion
{
    bool operator()(const LLVertexBuffer::MappedRegion& lhs, const LLVertexBuffer::MappedRegion& rhs)
    {
        return lhs.mStart < rhs.mStart;
    }
};

//============================================================================
// Pool of reusable VertexBuffer state

// batch calls to glGenBuffers
static U32 gen_buffer()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;

    U32 ret = 0;
    constexpr U32 pool_size = 4096;

    thread_local static U32 sNamePool[pool_size];
    thread_local static U32 sIndex = 0;

    if (sIndex == 0)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_VERTEX("gen buffer");
        sIndex = pool_size;
        LLVkBuf::create(pool_size, sNamePool);
    }

    ret = sNamePool[--sIndex];
    return ret;
}

static void delete_buffers(S32 count, U32* buffers)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
    // wait a few frames before actually deleting the buffers to avoid
    // synchronization issues with the GPU
    static std::vector<U32> sFreeList[4];

    if (gDeviceCaps.mInited)
    {
        U32 idx = LLImageTex::sFrameCount % 4;

        for (S32 i = 0; i < count; ++i)
        {
            sFreeList[idx].push_back(buffers[i]);
        }

        idx = (LLImageTex::sFrameCount + 3) % 4;

        if (!sFreeList[idx].empty())
        {
            LLVkBuf::destroy((S32)sFreeList[idx].size(), sFreeList[idx].data());
            sFreeList[idx].resize(0);
        }
    }
}


#define ANALYZE_VBO_POOL 0

// VBO Pool interface
class LLVBOPool
{
    public:
    virtual ~LLVBOPool() = default;
    virtual void allocate(BufferKind type, U32 size, U32& name, U8*& data) = 0;
    virtual void free(BufferKind type, U32 size, U32 name, U8* data) = 0;
    virtual U64 getVramBytesUsed() = 0;
};

// VBO Pool for Apple GPUs (as in M1/M2 etc, not Intel macs)
// Effectively disables VBO pooling
class LLAppleVBOPool final: public LLVBOPool
{
public:
    U64 mAllocated = 0;

    U64 getVramBytesUsed() override
    {
        return mAllocated;
    }

    void allocate(BufferKind type, U32 size, U32& name, U8*& data) override
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
        llassert(type == BufferKind::Vertex || type == BufferKind::Index);
        llassert(name == 0); // non zero name indicates a gl name that wasn't freed
        llassert(data == nullptr);  // non null data indicates a buffer that wasn't freed
        llassert(size >= 2);  // any buffer size smaller than a single index is nonsensical

        mAllocated += size;

        { //allocate a new buffer
            LL_PROFILE_GPU_ZONE("vbo alloc");
            // ON OS X, we don't allocate a VBO until the last possible moment
            // in unmapBuffer
            data = (U8*) ll_aligned_malloc_16(size);
        }
    }

    void free(BufferKind type, U32 size, U32 name, U8* data) override
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
        llassert(type == BufferKind::Vertex || type == BufferKind::Index);
        llassert(size >= 2);

        if (data)
        {
            ll_aligned_free_16(data);
        }

        mAllocated -= size;
        if (name)
        {
            delete_buffers(1, &name);
        }
    }
};

// VBO Pool for GPUs that benefit from VBO pooling
class LLDefaultVBOPool final : public LLVBOPool
{
public:
    typedef std::chrono::steady_clock::time_point Time;
    struct Entry
    {
        U8* mData;
        U32 mTextureHandle;
        Time mAge;
    };

    ~LLDefaultVBOPool() override
    {
        clear();
    }

    typedef std::unordered_map<U32, std::list<Entry>> Pool;

    Pool mVBOPool;
    Pool mIBOPool;

    U32 mTouchCount = 0;

    U64 mDistributed = 0;
    U64 mAllocated = 0;
    U64 mReserved = 0;
    U32 mMisses = 0;
    U32 mHits = 0;

    U64 getVramBytesUsed() override
    {
        return mAllocated + mReserved;
    }

    // increase the size to some common value (e.g. a power of two) to increase hit rate
    void adjustSize(U32& size)
    {
        // size = nhpo2(size);  // (193/303)/580 MB (distributed/allocated)/reserved in VBO Pool. Overhead: 66 percent. Hit rate: 77 percent

        //(245/276)/385 MB (distributed/allocated)/reserved in VBO Pool. Overhead: 57 percent. Hit rate: 69 percent
        //(187/209)/397 MB (distributed/allocated)/reserved in VBO Pool. Overhead: 112 percent. Hit rate: 76 percent
        U32 block_size = llmax(nhpo2(size) / 8, (U32) 16);
        size += block_size - (size % block_size);
    }

    void allocate(BufferKind type, U32 size, U32& name, U8*& data) override
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
        llassert(type == BufferKind::Vertex || type == BufferKind::Index);
        llassert(name == 0); // non zero name indicates a gl name that wasn't freed
        llassert(data == nullptr);  // non null data indicates a buffer that wasn't freed
        llassert(size >= 2);  // any buffer size smaller than a single index is nonsensical

        mDistributed += size;
        adjustSize(size);
        mAllocated += size;

        auto& pool = type == BufferKind::Index ? mIBOPool : mVBOPool;
        bool recycled = false;

        Pool::iterator iter = pool.find(size);
        if (iter == pool.end())
        { // cache miss, allocate a new buffer
            LL_PROFILE_ZONE_NAMED_CATEGORY_VERTEX("vbo pool miss");
            LL_PROFILE_GPU_ZONE("vbo alloc");

            mMisses++;
            name = gen_buffer();
            // <FS> the storage is specified for the buffer by name; nothing is bound
            LLVkAsync::bufferData(name, type, size, nullptr);

            data = (U8*)ll_aligned_malloc_16(size);
        }
        else
        {
            mHits++;
            llassert(mReserved >= size); // assert if accounting gets messed up
            recycled = true;
            mReserved -= size;

            std::list<Entry>& entries = iter->second;
            Entry& entry = entries.back();
            name = entry.mTextureHandle;
            data = entry.mData;

            entries.pop_back();
            if (entries.empty())
            {
                pool.erase(iter);
            }
        }
        if (recycled)
        {
            LLVkAsync::bufferRecycle(name);     // <FS> the name comes back without glBufferData: whatever was built over its old contents is stale
        }

        clean();
    }

    void free(BufferKind type, U32 size, U32 name, U8* data) override
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
        llassert(type == BufferKind::Vertex || type == BufferKind::Index);
        llassert(size >= 2);
        llassert(name != 0);
        llassert(data != nullptr);

        clean();

        llassert(mDistributed >= size);
        mDistributed -= size;
        adjustSize(size);
        llassert(mAllocated >= size);
        mAllocated -= size;
        mReserved += size;

        auto& pool = type == BufferKind::Index ? mIBOPool : mVBOPool;

        Pool::iterator iter = pool.find(size);

        if (iter == pool.end())
        {
            std::list<Entry> newlist;
            newlist.push_front({ data, name, std::chrono::steady_clock::now() });
            pool[size] = newlist;
        }
        else
        {
            iter->second.push_front({ data, name, std::chrono::steady_clock::now() });
        }
        {
            LLVkAsync::bufferRecycle(name);     // <FS> back in the pool: nothing may keep building over it
        }
    }

    // clean periodically (clean gets called for every alloc/free)
    void clean()
    {
        mTouchCount++;
        if (mTouchCount < 1024) // clean every 1k touches
        {
            return;
        }
        mTouchCount = 0;

        LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;

        std::unordered_map<U32, std::list<Entry>>* pools[] = { &mVBOPool, &mIBOPool };

        using namespace std::chrono_literals;

        Time cutoff = std::chrono::steady_clock::now() - 5s;

        for (auto* pool : pools)
        {
            for (Pool::iterator iter = pool->begin(); iter != pool->end(); )
            {
                auto& entries = iter->second;

                while (!entries.empty() && entries.back().mAge < cutoff)
                {
                    LL_PROFILE_ZONE_NAMED_CATEGORY_VERTEX("vbo cache timeout");
                    auto& entry = entries.back();
                    ll_aligned_free_16(entry.mData);
                    delete_buffers(1, &entry.mTextureHandle);
                    llassert(mReserved >= iter->first);
                    mReserved -= iter->first;
                    entries.pop_back();

                }

                if (entries.empty())
                {
                    iter = pool->erase(iter);
                }
                else
                {
                    ++iter;
                }
            }
        }

#if 0
        LL_INFOS() << llformat("(%d/%d)/%d MB (distributed/allocated)/total in VBO Pool. Overhead: %d percent. Hit rate: %d percent",
            mDistributed / 1000000,
            mAllocated / 1000000,
            (mAllocated + mReserved) / 1000000, // total bytes
            ((mAllocated+mReserved-mDistributed)*100)/llmax(mDistributed, (U64) 1), // overhead percent
            (mHits*100)/llmax(mMisses+mHits, (U32)1)) // hit rate percent
            << LL_ENDL;
#endif
    }

    void clear()
    {
        for (auto& entries : mIBOPool)
        {
            for (auto& entry : entries.second)
            {
                ll_aligned_free_16(entry.mData);
                delete_buffers(1, &entry.mTextureHandle);
            }
        }

        for (auto& entries : mVBOPool)
        {
            for (auto& entry : entries.second)
            {
                ll_aligned_free_16(entry.mData);
                delete_buffers(1, &entry.mTextureHandle);
            }
        }

        mReserved = 0;

        mIBOPool.clear();
        mVBOPool.clear();
    }
};

static LLVBOPool* sVBOPool = nullptr;

void LLVertexBufferData::drawWithMatrix()
{
    if (!mVB)
    {
        llassert(false);
        // Not supposed to happen, check buffer generation
        return;
    }

    if (mTexName)
    {
        gRender.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, mTexName);
    }
    else
    {
        gRender.getTexUnit(0)->bindWhite();
    }

    LLRenderMatrix::push(LLRenderMatrix::Kind::Modelview);
    LLRenderMatrix::load(LLRenderMatrix::Kind::Modelview, glm::value_ptr(mModelView));
    LLRenderMatrix::push(LLRenderMatrix::Kind::Projection);
    LLRenderMatrix::load(LLRenderMatrix::Kind::Projection, glm::value_ptr(mProjection));
    LLRenderMatrix::push(LLRenderMatrix::Kind::Texture0);
    LLRenderMatrix::load(LLRenderMatrix::Kind::Texture0, glm::value_ptr(mTexture0));

    mVB->setBuffer();
    mVB->drawArrays(mMode, 0, mCount);

    LLRenderMatrix::pop(LLRenderMatrix::Kind::Texture0);
    LLRenderMatrix::pop(LLRenderMatrix::Kind::Projection);
    LLRenderMatrix::pop(LLRenderMatrix::Kind::Modelview);
}

void LLVertexBufferData::draw()
{
    if (!mVB)
    {
        llassert(false);
        // Not supposed to happen, check buffer generation
        return;
    }

    if (mTexName)
    {
        gRender.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, mTexName);
    }
    else
    {
        gRender.getTexUnit(0)->bindWhite();
    }

    if (LLRender::sUiHashing)
    {
        LLRender::uiHashCachedDraw(mVB.get(), mMode, mCount, mTexName, mModelView);
    }

    mVB->setBuffer();
    mVB->drawArrays(mMode, 0, mCount);
}

//============================================================================

//static
U64 LLVertexBuffer::getBytesAllocated()
{
    return sVBOPool ? sVBOPool->getVramBytesUsed() : 0;
}

//============================================================================
//
//static
U32 LLVertexBuffer::sRenderBufferId = 0;
U32 LLVertexBuffer::sRenderIndicesId = 0;
U32 LLVertexBuffer::sLastMask = 0;
U64 LLVertexBuffer::sDrawCalls = 0;
U32 LLVertexBuffer::sVertexCount = 0;


//NOTE: each component must be AT LEAST 4 bytes in size to avoid a performance penalty on AMD hardware
const U32 LLVertexBuffer::sTypeSize[LLVertexBuffer::TYPE_MAX] =
{
    sizeof(LLVector4), // TYPE_VERTEX,
    sizeof(LLVector4), // TYPE_NORMAL,
    sizeof(LLVector2), // TYPE_TEXCOORD0,
    sizeof(LLVector2), // TYPE_TEXCOORD1,
    sizeof(LLVector2), // TYPE_TEXCOORD2,
    sizeof(LLVector2), // TYPE_TEXCOORD3,
    sizeof(LLColor4U), // TYPE_COLOR,
    sizeof(LLColor4U), // TYPE_EMISSIVE, only alpha is used currently
    sizeof(LLVector4), // TYPE_TANGENT,
    sizeof(F32),       // TYPE_WEIGHT,
    sizeof(LLVector4), // TYPE_WEIGHT4,
    sizeof(LLVector4), // TYPE_CLOTHWEIGHT,
    sizeof(U64),       // TYPE_JOINT,
    sizeof(LLVector4), // TYPE_TEXTURE_INDEX (actually exists as position.w), no extra data, but stride is 16 bytes
};

static const std::string vb_type_name[] =
{
    "TYPE_VERTEX",
    "TYPE_NORMAL",
    "TYPE_TEXCOORD0",
    "TYPE_TEXCOORD1",
    "TYPE_TEXCOORD2",
    "TYPE_TEXCOORD3",
    "TYPE_COLOR",
    "TYPE_EMISSIVE",
    "TYPE_TANGENT",
    "TYPE_WEIGHT",
    "TYPE_WEIGHT4",
    "TYPE_CLOTHWEIGHT",
    "TYPE_JOINT"
    "TYPE_TEXTURE_INDEX",
    "TYPE_MAX",
    "TYPE_INDEX",
};

const PrimitiveMode LLVertexBuffer::sPrimitive[LLRender::NUM_MODES] =
{
    PrimitiveMode::Triangles,
    PrimitiveMode::TriangleStrip,
    PrimitiveMode::TriangleFan,
    PrimitiveMode::Points,
    PrimitiveMode::Lines,
    PrimitiveMode::LineStrip,
    PrimitiveMode::Other,           // line loop: not drawn natively
};

//static
void LLVertexBuffer::drawArrays(U32 mode, const std::vector<LLVector3>& pos)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
    gRender.begin(mode);
    for (auto& v : pos)
    {
        gRender.vertex3fv(v.mV);
    }
    gRender.end();
    gRender.flush();
}

//static
void LLVertexBuffer::drawElements(U32 mode, const LLVector4a* pos, const LLVector2* tc, U32 num_indices, const U16* indicesp)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
    llassert(LLShaderProgram::sCurBoundShaderPtr != NULL);

    // <FS:Beq> FIRE-29679 trap empty calls that cause crashes when rezzing in OpenSim.
    if(pos == nullptr || indicesp == nullptr )
    {
        LL_WARNS() << "Called drawElements with null pos or null indices" << LL_ENDL;
        return;
    }
    // </FS:Beq>

    // <FS:Ansariel> Crash fix due to invalid calls to drawElements by Drake Arconis
    if (num_indices == 0)
    {
        LL_WARNS() << "Called drawElements with 0 indices" << LL_ENDL;
        return;
    }
    // </FS:Ansariel>


    gRender.syncMatrices();

    unbind();

    gRender.begin(mode);

    if (tc != nullptr)
    {
        for (U32 i = 0; i < num_indices; ++i)
        {
            U16 idx = indicesp[i];
            gRender.texCoord2fv(tc[idx].mV);
            gRender.vertex3fv(pos[idx].getF32ptr());
        }
    }
    else
    {
        for (U32 i = 0; i < num_indices; ++i)
        {
            U16 idx = indicesp[i];
            gRender.vertex3fv(pos[idx].getF32ptr());
        }
    }
    gRender.end();
    gRender.flush();
}

bool LLVertexBuffer::validateRange(U32 start, U32 end, U32 count, U32 indices_offset) const
{
    if (!gDebugRender)
    {
        return true;
    }

    llassert(start < mNumVerts);
    llassert(end < mNumVerts);

    if (start >= mNumVerts ||
        end >= mNumVerts)
    {
        LL_ERRS() << "Bad vertex buffer draw range: [" << start << ", " << end << "] vs " << mNumVerts << LL_ENDL;
    }

    if (indices_offset >= mNumIndices ||
        indices_offset + count > mNumIndices)
    {
        LL_ERRS() << "Bad index buffer draw range: [" << indices_offset << ", " << indices_offset+count << "]" << LL_ENDL;
    }

    {
#if 0  // not a reliable test for VBOs that are not backed by a CPU buffer
        U16* idx = (U16*) mMappedIndexData+indices_offset;
        for (U32 i = 0; i < count; ++i)
        {
            llassert(idx[i] >= start);
            llassert(idx[i] <= end);

            if (idx[i] < start || idx[i] > end)
            {
                LL_ERRS() << "Index out of range: " << idx[i] << " not in [" << start << ", " << end << "]" << LL_ENDL;
            }
        }

        LLVector4a* v = (LLVector4a*)mMappedData;

        for (U32 i = start; i <= end; ++i)
        {
            if (!v[i].isFinite3())
            {
                LL_ERRS() << "Non-finite vertex position data detected." << LL_ENDL;
            }
        }

        LLShaderProgram* shader = LLShaderProgram::sCurBoundShaderPtr;

        if (shader && shader->mFeatures.mIndexedTextureChannels > 1)
        {
            LLVector4a* v = (LLVector4a*) mMappedData;

            for (U32 i = start; i < end; i++)
            {
                U32 idx = (U32) (v[i][3]+0.25f);
                if (idx >= (U32)shader->mFeatures.mIndexedTextureChannels)
                {
                    LL_ERRS() << "Bad texture index found in vertex data stream." << LL_ENDL;
                }
            }
        }
#endif
    }

    return true;
}

#if LL_PROFILER_ENABLE_RENDER_DOC
void LLVertexBuffer::setLabel(const char* label) {
    (void)label;        // nothing to label: a buffer is not a GL object any more
}
#endif

void LLVertexBuffer::clone(LLVertexBuffer& target) const
{
    target.mTypeMask = mTypeMask;
    target.mIndicesType = mIndicesType;
    target.mIndicesStride = mIndicesStride;
    if (target.getNumVerts() != getNumVerts() ||
        target.getNumIndices() != getNumIndices())
    {
        target.allocateBuffer(getNumVerts(), getNumIndices());
    }
}

// <FS> S6.6: with a pull program bound (LL_VERTEX_PULL) a draw of this buffer is the one entry of a geometry record (kind 32): the arrays are read by address, the matrices of the stack travel in
// the record. The uniforms the program keeps besides them are written as before (syncMatrices); the state, texture units and uniforms of the draw ride in the stream in front of the record.
bool LLVertexBuffer::pullDraw(U32 mode, bool indexed, U32 first, U32 count) const
{
    LLShaderProgram* program = LLShaderProgram::sCurBoundShaderPtr;
    if (!program || !program->isVertexPull())
    {
        return false;
    }
    if (!mVertexBufferId || (mode != LLRender::TRIANGLES && mode != LLRender::TRIANGLE_STRIP)
        || (indexed && (!mIndexBufferId || mIndicesType != ComponentType::U16)) || !hasDataType(TYPE_VERTEX) || getOffset(TYPE_VERTEX) != 0)
    {
        // a pull program draws nothing through the classic path: say so (once per program and reason), so a missing mesh is not a silent one
        static std::set<std::string> told;
        const std::string why = program->mName + (!mVertexBufferId ? ": no vertex buffer" : (mode != LLRender::TRIANGLES && mode != LLRender::TRIANGLE_STRIP) ? ": mode " + std::to_string(mode)
            : (indexed && !mIndexBufferId) ? ": no index buffer" : (indexed && mIndicesType != ComponentType::U16) ? ": 32 bit indices" : !hasDataType(TYPE_VERTEX) ? ": no positions" : ": position offset");
        if (told.insert(why).second)
        {
            LL_WARNS("ShadowPackets") << "a draw of a pull program is refused (" << why << "): it is not drawn" << LL_ENDL;
        }
        return false;
    }
    gRender.syncMatrices();
    const glm::mat4 mv = gRender.getModelviewMatrix();
    const glm::mat4 mvp = gRender.getProjectionMatrix() * mv;
    const glm::mat4 n = glm::transpose(glm::inverse(mv));
    F32 matrices[44];
    memcpy(matrices, glm::value_ptr(mv), 64);
    memcpy(matrices + 16, glm::value_ptr(mvp), 64);
    for (int col = 0; col < 3; ++col)
    {
        matrices[32 + 4 * col + 0] = n[col][0];
        matrices[32 + 4 * col + 1] = n[col][1];
        matrices[32 + 4 * col + 2] = n[col][2];
        matrices[32 + 4 * col + 3] = 0.f;
    }
    LLVkBridge::GeoEntry e = {};
    e.mVertexBuffer = mVertexBufferId;
    e.mIndexBuffer = indexed ? mIndexBufferId : 0;
    e.mFirstIndex = first;
    e.mIndexCount = count;
    e.mFlags = (indexed ? 0u : (U32)LLVkBridge::GEO_NO_INDEX) | (mode == LLRender::TRIANGLE_STRIP ? (U32)LLVkBridge::GEO_STRIP : 0u);
    e.mNormalOffset = hasDataType(TYPE_NORMAL) ? getOffset(TYPE_NORMAL) : 0;
    e.mTexcoordOffset = hasDataType(TYPE_TEXCOORD0) ? getOffset(TYPE_TEXCOORD0) : 0;
    e.mColorOffset = hasDataType(TYPE_COLOR) ? getOffset(TYPE_COLOR) : 0;
    e.mTangentOffset = hasDataType(TYPE_TANGENT) ? getOffset(TYPE_TANGENT) : (hasDataType(TYPE_CLOTHWEIGHT) ? getOffset(TYPE_CLOTHWEIGHT) : 0);
    e.mTexcoord1Offset = hasDataType(TYPE_TEXCOORD1) ? getOffset(TYPE_TEXCOORD1) : 0;
    e.mTexcoord2Offset = hasDataType(TYPE_TEXCOORD2) ? getOffset(TYPE_TEXCOORD2) : 0;
    e.mWeightOffset = hasDataType(TYPE_WEIGHT) ? getOffset(TYPE_WEIGHT) : 0;
    // a texture matrix on the stack (a draw with a texture animation or a flip) is the one entry's slot 1 of the table after the matrices (the pull programs read texture_matrix0 from it)
    const glm::mat4& texture_matrix = LLRenderMatrix::get(LLRenderMatrix::Kind::Texture0);
    const bool has_texture_matrix = texture_matrix != glm::mat4(1.f);
    if (has_texture_matrix)
    {
        e.mPad3 = 1;
    }
    U8 blob[sizeof(LLVkBridge::GeoEntry) + sizeof(matrices) + 64];
    memcpy(blob, &e, sizeof(e));
    memcpy(blob + sizeof(e), matrices, sizeof(matrices));
    if (has_texture_matrix)
    {
        memcpy(blob + sizeof(e) + sizeof(matrices), glm::value_ptr(texture_matrix), 64);
    }
    LLVkBridge::ShadowPacketsHeader header;
    header.mKind = 32;
    header.mEntries = 1;
    header.mMatrices = 1;
    header.mFragment = has_texture_matrix ? 1 : 0;
    LLVkBridge::shadowPackets(header, blob, sizeof(e) + sizeof(matrices) + (has_texture_matrix ? 64 : 0));
    return true;
}

void LLVertexBuffer::drawRange(U32 mode, U32 start, U32 end, U32 count, U32 indices_offset) const
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_DRAW);
    ++sDrawCalls;
    llassert(validateRange(start, end, count, indices_offset));
    llassert(mVertexBufferId == sRenderBufferId);
    llassert(mIndexBufferId == sRenderIndicesId);
    if (pullDraw(mode, true, indices_offset, count))
    {
        return;
    }
    gRender.syncMatrices();
    LLVkAsync::drawRangeElements(sPrimitive[mode], start, end, count, mIndicesType, (const void*)(indices_offset * (size_t) mIndicesStride));
}

void LLVertexBuffer::drawRangeFast(U32 mode, U32 start, U32 end, U32 count, U32 indices_offset) const
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_DRAW);
    ++sDrawCalls;
    LLVkAsync::drawRangeElements(sPrimitive[mode], start, end, count, mIndicesType, (const void*)(indices_offset * (size_t)mIndicesStride));
}


void LLVertexBuffer::draw(U32 mode, U32 count, U32 indices_offset) const
{
    drawRange(mode, 0, mNumVerts-1, count, indices_offset);
}


void LLVertexBuffer::drawArrays(U32 mode, U32 first, U32 count) const
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_DRAW);
    ++sDrawCalls;
    llassert(first + count <= mNumVerts);
    llassert(mVertexBufferId == sRenderBufferId);
    llassert(mIndexBufferId == sRenderIndicesId);

    if (pullDraw(mode, false, first, count))
    {
        return;
    }
    gRender.syncMatrices();
    LLVkAsync::drawArrays(sPrimitive[mode], first, count);
}

//static
void LLVertexBuffer::initClass(LLWindow* window)
{
    llassert(sVBOPool == nullptr);

    if (gDeviceCaps.mIsApple)
    {
        LL_INFOS() << "VBO Pooling Disabled" << LL_ENDL;
        sVBOPool = new LLAppleVBOPool();
    }
    else
    {
        LL_INFOS() << "VBO Pooling Enabled" << LL_ENDL;
        sVBOPool = new LLDefaultVBOPool();
    }

}

//static
void LLVertexBuffer::drawNoAttributes(U32 mode, U32 first, U32 count)
{
    ++sDrawCalls;
    gRender.syncMatrices();
    LLVkAsync::drawArrays(sPrimitive[mode], first, count);
}

//static
void LLVertexBuffer::forgetBoundBuffers()
{
    sRenderBufferId = 0;
    sRenderIndicesId = 0;
}

//static
void LLVertexBuffer::unbind()
{
    LLVkAsync::drawBindingClear();
    sRenderBufferId = 0;
    sRenderIndicesId = 0;
}

//static
void LLVertexBuffer::cleanupClass()
{
    unbind();

    delete sVBOPool;
    sVBOPool = nullptr;

}

//----------------------------------------------------------------------------

// F2.4: what setBuffer worked out for this buffer last: the whole vertex binding for the shader attribute mask. The attribute pointers are a function of the layout (type mask and
// vertex count, which fix the offsets), the two buffer ids and the mask, so the binding is reused while all five are the same (the verify mode of LL_VK_DRAWREC compares every draw's
// binding with the old state machine's, so a stale one shows up there).
struct LLVertexBuffer::VkBindCache
{
    U32 mNumVerts = 0, mTypeMask = 0, mMask = 0, mArray = 0, mElem = 0;
    LLVkAsync::DrawBinding mBinding;
};

LLVertexBuffer::LLVertexBuffer(U32 typemask)
:   LLRefCount(),
    mTypeMask(typemask)
{
    //zero out offsets
    for (U32 i = 0; i < TYPE_MAX; i++)
    {
        mOffsets[i] = 0;
    }
}

// list of mapped buffers
// NOTE: must not be LLPointer<LLVertexBuffer> to avoid breaking non-ref-counted LLVertexBuffer instances
static std::vector<LLVertexBuffer*> sMappedBuffers;
// <FS> group rebuild R3b2: buffers are first mapped by the jobs that fill them, on pool threads
static std::mutex sMappedBuffersMutex;

//static
void LLVertexBuffer::flushBuffers()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
    // must only be called from main thread
    for (auto& buffer : sMappedBuffers)
    {
        buffer->_unmapBuffer();
        buffer->mMapped = false;
    }

    sMappedBuffers.resize(0);
}

//static
U32 LLVertexBuffer::calcOffsets(const U32& typemask, U32* offsets, U32 num_vertices)
{
    U32 offset = 0;
    for (U32 i=0; i<TYPE_TEXTURE_INDEX; i++)
    {
        U32 mask = 1<<i;
        if (typemask & mask)
        {
            if (offsets && LLVertexBuffer::sTypeSize[i])
            {
                offsets[i] = offset;
                offset += LLVertexBuffer::sTypeSize[i]*num_vertices;
                offset = (offset + 0xF) & ~0xF;
            }
        }
    }

    offsets[TYPE_TEXTURE_INDEX] = offsets[TYPE_VERTEX] + 12;

    return offset;
}

//static
U32 LLVertexBuffer::calcVertexSize(const U32& typemask)
{
    U32 size = 0;
    for (U32 i = 0; i < TYPE_TEXTURE_INDEX; i++)
    {
        U32 mask = 1<<i;
        if (typemask & mask)
        {
            size += LLVertexBuffer::sTypeSize[i];
        }
    }

    return size;
}

// protected, use unref()
//virtual
LLVertexBuffer::~LLVertexBuffer()
{
    LLVBHandles::release(mRenderHandle.v.load(std::memory_order_acquire));       // <FS> C3.5: handles of this buffer stop resolving
    delete mVkBindCache;
    if (mMapped)
    { // is on the mapped buffer list but doesn't need to be flushed
        mMapped = false;
        unmapBuffer();
    }

    destroyGLBuffer();
    destroyGLIndices();

    if (mMappedData)
    {
        LL_ERRS() << "Failed to clear vertex buffer's vertices" << LL_ENDL;
    }
    if (mMappedIndexData)
    {
        LL_ERRS() << "Failed to clear vertex buffer's indices" << LL_ENDL;
    }
};

//----------------------------------------------------------------------------

void LLVertexBuffer::genBuffer(U32 size)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
    llassert(sVBOPool);

    if (sVBOPool)
    {
        llassert(mSize == 0);
        llassert(mVertexBufferId == 0);
        llassert(mMappedData == nullptr);

        mSize = size;
        sVBOPool->allocate(BufferKind::Vertex, mSize, mVertexBufferId, mMappedData);
    }
}

void LLVertexBuffer::genIndices(U32 size)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
    llassert(sVBOPool);

    if (sVBOPool)
    {
        llassert(mIndicesSize == 0);
        llassert(mIndexBufferId == 0);
        llassert(mMappedIndexData == nullptr);
        mIndicesSize = size;
        sVBOPool->allocate(BufferKind::Index, mIndicesSize, mIndexBufferId, mMappedIndexData);
    }
}

bool LLVertexBuffer::createGLBuffer(U32 size)
{
    if (mVertexBufferId || mMappedData)
    {
        destroyGLBuffer();
    }

    if (size == 0)
    {
        return true;
    }

    bool success = true;

    genBuffer(size);

    if (!mMappedData)
    {
        success = false;
    }
    return success;
}

bool LLVertexBuffer::createGLIndices(U32 size)
{
    if (mIndexBufferId)
    {
        destroyGLIndices();
    }

    if (size == 0)
    {
        return true;
    }

    bool success = true;

    genIndices(size);

    if (!mMappedIndexData)
    {
        success = false;
    }
    return success;
}

void LLVertexBuffer::destroyGLBuffer()
{
    if (mVertexBufferId || mMappedData)
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
        //llassert(sVBOPool);
        if (sVBOPool)
        {
            sVBOPool->free(BufferKind::Vertex, mSize, mVertexBufferId, mMappedData);
        }

        mSize = 0;
        mVertexBufferId = 0;
        mMappedData = nullptr;
    }
}

void LLVertexBuffer::destroyGLIndices()
{
    if (mIndexBufferId || mMappedIndexData)
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
        //llassert(sVBOPool);
        if (sVBOPool)
        {
            sVBOPool->free(BufferKind::Index, mIndicesSize, mIndexBufferId, mMappedIndexData);
        }

        mIndicesSize = 0;
        mIndexBufferId = 0;
        mMappedIndexData = nullptr;
    }
}

bool LLVertexBuffer::updateNumVerts(U32 nverts)
{
    llassert(nverts >= 0);

    bool success = true;

    U32 needed_size = calcOffsets(mTypeMask, mOffsets, nverts);

    if (needed_size != mSize)
    {
        success &= createGLBuffer(needed_size);
    }

    llassert(mSize == needed_size);
    mNumVerts = nverts;
    return success;
}

bool LLVertexBuffer::updateNumIndices(U32 nindices)
{
    llassert(nindices >= 0);

    bool success = true;

    U32 needed_size = sizeof(U16) * nindices;

    if (needed_size != mIndicesSize)
    {
        success &= createGLIndices(needed_size);
    }

    llassert(mIndicesSize == needed_size);
    mNumIndices = nindices;
    return success;
}

bool LLVertexBuffer::allocateBuffer(U32 nverts, U32 nindices)
{
    if (nverts < 0 || nindices < 0)
    {
        LL_ERRS() << "Bad vertex buffer allocation: " << nverts << " : " << nindices << LL_ENDL;
    }

    bool success = true;

    success &= updateNumVerts(nverts);
    success &= updateNumIndices(nindices);

    return success;
}

//----------------------------------------------------------------------------

// if no gap between region and given range exists, expand region to cover given range and return true
// otherwise return false
bool expand_region(LLVertexBuffer::MappedRegion& region, U32 start, U32 end)
{

    // <FS> a range that touches the region (the next byte after its end, or the one before its start) joins it: the test used to call that a gap, so sequential writes (a particle store written one slot at
    // a time, eight arrays each) made a region per call and every call scanned all of them: n squared, 80-150 ms main thread stalls with a few thousand dirty slots
    if (end + 1 < region.mStart ||
        start > region.mEnd + 1)
    { //gap exists, do not merge
        return false;
    }

    region.mStart = llmin(region.mStart, start);
    region.mEnd = llmax(region.mEnd, end);

    return true;
}


// Map for data access
U8* LLVertexBuffer::mapVertexBuffer(LLVertexBuffer::AttributeType type, U32 index, S32 count)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
    _mapBuffer();
    ++mDataSerial;

    if (count == -1)
    {
        count = mNumVerts - index;
    }

    if (!gDeviceCaps.mIsApple)
    {
        U32 start = mOffsets[type] + sTypeSize[type] * index;
        U32 end = start + sTypeSize[type] * count-1;

        bool flagged = false;
        // flag region as mapped (<FS> newest region first: writes come in runs, the one just extended is the one that takes the next range)
        for (U32 i = (U32)mMappedVertexRegions.size(); i-- > 0;)
        {
            MappedRegion& region = mMappedVertexRegions[i];
            if (expand_region(region, start, end))
            {
                flagged = true;
                break;
            }
        }

        if (!flagged)
        {
            //didn't expand an existing region, make a new one
            mMappedVertexRegions.push_back({ start, end });
        }
    }
    return mMappedData+mOffsets[type]+sTypeSize[type]*index;
}


U8* LLVertexBuffer::mapIndexBuffer(U32 index, S32 count)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
    _mapBuffer();
    ++mDataSerial;

    if (count == -1)
    {
        count = mNumIndices-index;
    }

    if (!gDeviceCaps.mIsApple)
    {
        U32 start = sizeof(U16) * index;
        U32 end = start + sizeof(U16) * count-1;

        bool flagged = false;
        // flag region as mapped (<FS> newest region first, as for the vertices)
        for (U32 i = (U32)mMappedIndexRegions.size(); i-- > 0;)
        {
            MappedRegion& region = mMappedIndexRegions[i];
            if (expand_region(region, start, end))
            {
                flagged = true;
                break;
            }
        }

        if (!flagged)
        {
            //didn't expand an existing region, make a new one
            mMappedIndexRegions.push_back({ start, end });
        }
    }

    return mMappedIndexData + sizeof(U16)*index;
}

// flush the given byte range
//  target -- "target" parameter for glBufferSubData
//  start -- first byte to copy
//  end -- last byte to copy (NOT last byte + 1)
//  data -- data to be flushed
//  dst -- mMappedData or mMappedIndexData
void LLVertexBuffer::flush_vbo(BufferKind target, U32 start, U32 end, void* data, U8* dst)
{
    // the buffer is named in the record: nothing is bound for it
    if (end != 0)
    {
        // <FS> LL_VK_VBCHECK (default on, 0 = off): the positions that are about to be flushed (the array at offset 0, 16 bytes a vertex) must be finite and not beyond 1e5: the ray scene builder's "bad range"
        // warnings are whole NaN vertices that the volume fill does not write, and the stack here is the code that filled the buffer (every fill ends in an unmap). Logged with that stack (first 40).
        static const bool vb_check = getenv("LL_VK_VBCHECK") == nullptr || atoi(getenv("LL_VK_VBCHECK")) != 0;
        if (vb_check && target == BufferKind::Vertex && start < (U32)getNumVerts() * 16u)
        {
            const U32 pos_end = std::min<U32>(end, (U32)getNumVerts() * 16u - 1u);
            const U32 first_v = (start + 15u) / 16u;
            const U32 last_v = (pos_end + 1u) / 16u;        // (one past)
            LARGE_INTEGER scan_t0;
            QueryPerformanceCounter(&scan_t0);
            U32 bad = 0, first_bad = ~0u, nan_comp = 0, inf_comp = 0, huge_comp = 0;
            F32 first_vals[4] = {};
            static const bool vb_fix = getenv("LL_VK_VBFIX") == nullptr || atoi(getenv("LL_VK_VBFIX")) != 0;
            for (U32 v = first_v; v < last_v; ++v)
            {
                F32* p = (F32*)((U8*)data + (size_t)v * 16u - start);
                if (!(p[0] > -1e5f && p[0] < 1e5f && p[1] > -1e5f && p[1] < 1e5f && p[2] > -1e5f && p[2] < 1e5f))
                {
                    if (!bad) { first_bad = v; memcpy(first_vals, p, sizeof(first_vals)); }
                    ++bad;
                    for (int c = 0; c < 3; ++c)
                    {
                        if (p[c] != p[c]) { ++nan_comp; }
                        else if (!(p[c] > -1e30f && p[c] < 1e30f)) { ++inf_comp; }
                        else if (!(p[c] > -1e5f && p[c] < 1e5f)) { ++huge_comp; }
                    }
                    // <FS> LL_VK_VBFIX (default on, 0 = off): a vertex whose position is not finite or beyond 1e6 is set to the origin (a degenerate triangle) in what is sent: it would be a spike on the screen and a bad
                    // structure in the ray scene; the UI's immediate mode buffers (mask 69) fill only x and y and are left alone
                    if (vb_fix && mTypeMask != 69u && !(p[0] > -1e6f && p[0] < 1e6f && p[1] > -1e6f && p[1] < 1e6f && p[2] > -1e6f && p[2] < 1e6f))
                    {
                        p[0] = p[1] = p[2] = 0.f;
                    }
                }
            }
            {
                // <FS> what the scan costs (reported every 10 s: the vertices scanned, the time, the flushes), to decide whether it stays on
                static U64 scanned = 0, flushes = 0, scan_ticks = 0;
                static LONGLONG report_at = 0;
                LARGE_INTEGER scan_t1, freq;
                QueryPerformanceCounter(&scan_t1);
                QueryPerformanceFrequency(&freq);
                scanned += (last_v > first_v) ? (last_v - first_v) : 0;
                ++flushes;
                scan_ticks += (U64)(scan_t1.QuadPart - scan_t0.QuadPart);
                if (!report_at) { report_at = scan_t1.QuadPart; }
                if (scan_t1.QuadPart - report_at >= 10 * freq.QuadPart)
                {
                    LL_INFOS("VertexCheck") << "position scan over the last 10 s: " << flushes << " flushes, " << scanned << " vertices, " << (double)scan_ticks * 1000.0 / (double)freq.QuadPart << " ms in all" << LL_ENDL;
                    scanned = flushes = scan_ticks = 0;
                    report_at = scan_t1.QuadPart;
                }
            }
            if (bad)
            {
                // the UI's immediate mode buffers (LLRender::genBuffer: vertex, texcoord0, colour = mask 69) fill the positions' x and y only: the rest of the slot is not theirs to be checked, so they are counted apart
                static U64 flagged = 0, flagged_ui = 0;
                if (mTypeMask == 69u)
                {
                    ++flagged_ui;
                }
                else if (flagged++ < 40)
                {
                    LL_WARNS("VertexCheck") << "vertex buffer " << mVertexBufferId << " (" << getNumVerts() << " vertices, type mask " << mTypeMask << ") is flushed with " << bad << " bad positions of " << (last_v - first_v)
                                            << " flushed (first at vertex " << first_bad << " = " << first_vals[0] << ", " << first_vals[1] << ", " << first_vals[2] << ", " << first_vals[3] << "; components: " << nan_comp << " NaN, " << inf_comp << " infinite, " << huge_comp << " huge; region bytes " << start << ".." << end << "); the code that filled it:\n" << LLVkAsync::stackHere(1, 14) << LL_ENDL;
                }
            }
        }
        LL_PROFILE_ZONE_NAMED_CATEGORY_VERTEX("buffer sub data");
        LL_PROFILE_ZONE_NUM(start);
        LL_PROFILE_ZONE_NUM(end);
        LL_PROFILE_ZONE_NUM(end-start);

        constexpr U32 block_size = 65536;

        for (U32 i = start; i <= end; i += block_size)
        {
            U32 tend = llmin(i + block_size, end);
            U32 size = tend - i + 1;
            LLVkAsync::bufferSubData(target == BufferKind::Vertex ? mVertexBufferId : mIndexBufferId, i, size, (U8*)data + (i - start));
        }
    }
}

void LLVertexBuffer::unmapBuffer()
{
    flushBuffers();
}

void LLVertexBuffer::_mapBuffer()
{
    if (!mMapped)
    {
        mMapped = true;
        std::lock_guard<std::mutex> lock(sMappedBuffersMutex);
        sMappedBuffers.push_back(this);
    }
}

void LLVertexBuffer::_unmapBuffer()
{
    if (!mMapped)
    {
        return;
    }

    struct SortMappedRegion
    {
        bool operator()(const MappedRegion& lhs, const MappedRegion& rhs)
        {
            return lhs.mStart < rhs.mStart;
        }
    };

    {
        if (!mMappedVertexRegions.empty())
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_VERTEX("unmapBuffer - vertex");


            U32 start = 0;
            U32 end = 0;

            std::sort(mMappedVertexRegions.begin(), mMappedVertexRegions.end(), SortMappedRegion());

            for (U32 i = 0; i < mMappedVertexRegions.size(); ++i)
            {
                const MappedRegion& region = mMappedVertexRegions[i];
                if (region.mStart == end + 1)
                {
                    end = region.mEnd;
                }
                else
                {
                    flush_vbo(BufferKind::Vertex, start, end, (U8*)mMappedData + start, mMappedData);
                    start = region.mStart;
                    end = region.mEnd;
                }
            }

            flush_vbo(BufferKind::Vertex, start, end, (U8*)mMappedData + start, mMappedData);
            mMappedVertexRegions.clear();
        }

        if (!mMappedIndexRegions.empty())
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_VERTEX("unmapBuffer - index");

            U32 start = 0;
            U32 end = 0;

            std::sort(mMappedIndexRegions.begin(), mMappedIndexRegions.end(), SortMappedRegion());

            for (U32 i = 0; i < mMappedIndexRegions.size(); ++i)
            {
                const MappedRegion& region = mMappedIndexRegions[i];
                if (region.mStart == end + 1)
                {
                    end = region.mEnd;
                }
                else
                {
                    flush_vbo(BufferKind::Index, start, end, (U8*)mMappedIndexData + start, mMappedIndexData);
                    start = region.mStart;
                    end = region.mEnd;
                }
            }

            flush_vbo(BufferKind::Index, start, end, (U8*)mMappedIndexData + start, mMappedIndexData);
            mMappedIndexRegions.clear();
        }
    }
}

//----------------------------------------------------------------------------

template <class T,LLVertexBuffer::AttributeType type> struct VertexBufferStrider
{
    typedef LLStrider<T> strider_t;
    static bool get(LLVertexBuffer& vbo,
                    strider_t& strider,
                    S32 index, S32 count)
    {
        if (type == LLVertexBuffer::TYPE_INDEX)
        {
            U8* ptr = vbo.mapIndexBuffer(index, count);

            if (ptr == NULL)
            {
                LL_WARNS() << "mapIndexBuffer failed!" << LL_ENDL;
                return false;
            }

            strider = (T*)ptr;
            strider.setStride(0);
            return true;
        }
        else if (vbo.hasDataType(type))
        {
            U32 stride = LLVertexBuffer::sTypeSize[type];

            U8* ptr = vbo.mapVertexBuffer(type, index, count);

            if (ptr == NULL)
            {
                LL_WARNS() << "mapVertexBuffer failed!" << LL_ENDL;
                return false;
            }

            strider = (T*)ptr;
            strider.setStride(stride);
            return true;
        }
        else
        {
            LL_ERRS() << "VertexBufferStrider could not find valid vertex data." << LL_ENDL;
        }
        return false;
    }
};

bool LLVertexBuffer::getVertexStrider(LLStrider<LLVector3>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector3,TYPE_VERTEX>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getVertexStrider(LLStrider<LLVector4a>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector4a,TYPE_VERTEX>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getIndexStrider(LLStrider<U16>& strider, U32 index, S32 count)
{
    llassert(mIndicesStride == 2); // cannot access 32-bit indices with U16 strider
    llassert(mIndicesType == ComponentType::U16);
    return VertexBufferStrider<U16,TYPE_INDEX>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getTexCoord0Strider(LLStrider<LLVector2>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector2,TYPE_TEXCOORD0>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getTexCoord1Strider(LLStrider<LLVector2>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector2,TYPE_TEXCOORD1>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getTexCoord2Strider(LLStrider<LLVector2>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector2,TYPE_TEXCOORD2>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getNormalStrider(LLStrider<LLVector3>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector3,TYPE_NORMAL>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getNormalStrider(LLStrider<LLVector4a>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector4a, TYPE_NORMAL>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getTangentStrider(LLStrider<LLVector3>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector3,TYPE_TANGENT>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getTangentStrider(LLStrider<LLVector4a>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector4a,TYPE_TANGENT>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getColorStrider(LLStrider<LLColor4U>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLColor4U,TYPE_COLOR>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getEmissiveStrider(LLStrider<LLColor4U>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLColor4U,TYPE_EMISSIVE>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getWeightStrider(LLStrider<F32>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<F32,TYPE_WEIGHT>::get(*this, strider, index, count);
}

// <FS:Ansariel> Vectorized Weight4Strider and ClothWeightStrider by Drake Arconis
//bool LLVertexBuffer::getWeight4Strider(LLStrider<LLVector4>& strider, U32 index, S32 count)
//{
//  return VertexBufferStrider<LLVector4,TYPE_WEIGHT4>::get(*this, strider, index, count);
//}
//
//bool LLVertexBuffer::getClothWeightStrider(LLStrider<LLVector4>& strider, U32 index, S32 count)
//{
//  return VertexBufferStrider<LLVector4,TYPE_CLOTHWEIGHT>::get(*this, strider, index, count);
//}
bool LLVertexBuffer::getWeight4Strider(LLStrider<LLVector4a>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector4a,TYPE_WEIGHT4>::get(*this, strider, index, count);
}

bool LLVertexBuffer::getClothWeightStrider(LLStrider<LLVector4a>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector4a,TYPE_CLOTHWEIGHT>::get(*this, strider, index, count);
}
// </FS:Ansariel>

//----------------------------------------------------------------------------


// <FS> N1b: while setBuffer() writes a typed record, setupVertexBuffer() collects its attribute pointers here instead of calling GL
#if defined(LL_VULKAN)
namespace
{
    struct AttribSink
    {
        LLVkAsync::VertexAttrib mAttribs[24];
        U32 mCount = 0;
    };
    thread_local AttribSink* sAttribSink = nullptr;
}
#endif

static inline void ll_attribPointer(U32 index, S32 size, ComponentType type, bool normalized, S32 stride, const void* pointer)
{
#if defined(LL_VULKAN)
    if (sAttribSink)
    {
        llassert(sAttribSink->mCount < 24);
        sAttribSink->mAttribs[sAttribSink->mCount++] = { 0, index, size, type, stride, (U32)normalized, pointer };
        return;
    }
#endif
}

static inline void ll_attribIPointer(U32 index, S32 size, ComponentType type, S32 stride, const void* pointer)
{
#if defined(LL_VULKAN)
    if (sAttribSink)
    {
        llassert(sAttribSink->mCount < 24);
        sAttribSink->mAttribs[sAttribSink->mCount++] = { 1, index, size, type, stride, 0, pointer };
        return;
    }
#endif
}

// Set for rendering
void LLVertexBuffer::setBuffer()
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_SET_BUFFER);

    if (mMapped)
    {
        LL_WARNS_ONCE() << "Missing call to unmapBuffer or flushBuffers" << LL_ENDL;
        _unmapBuffer();
    }

    // no data may be pending
    llassert(mMappedVertexRegions.empty());
    llassert(mMappedIndexRegions.empty());

    // a shader must be bound
    llassert(LLShaderProgram::sCurBoundShaderPtr);

    U32 data_mask = LLShaderProgram::sCurBoundShaderPtr->mAttributeMask;

    // this Vertex Buffer must provide all necessary attributes for currently bound shader
    llassert_msg((data_mask & mTypeMask) == data_mask,
        "Attribute mask mismatch! mTypeMask should be a superset of data_mask.  data_mask: 0x"
                << std::hex << data_mask << " mTypeMask: 0x" << mTypeMask << " Missing: 0x" << (data_mask & ~mTypeMask) <<  std::dec);

    // the binding of the next draws is the whole thing (this buffer, the bound shader's attributes): it is worked out when the buffers or the mask are not those of the binding the
    // producer holds, and reused while this buffer's layout, buffers and mask are the same (the cache)
    if (sRenderBufferId != mVertexBufferId || sRenderIndicesId != mIndexBufferId || sLastMask != data_mask)
    {
        const VkBindCache* c = mVkBindCache;
        const bool cache_hit = c && c->mNumVerts == mNumVerts && c->mTypeMask == mTypeMask && c->mMask == data_mask && c->mArray == mVertexBufferId && c->mElem == mIndexBufferId;
        if (cache_hit)
        {
            LLVkAsync::drawBindingSet(c->mBinding);
        }
        else
        {
            AttribSink sink;
            sAttribSink = &sink;
            setupVertexBuffer();
            sAttribSink = nullptr;
            LLVkAsync::drawBinding(mVertexBufferId, mIndexBufferId, sink.mAttribs, sink.mCount);
            if (!mVkBindCache)
            {
                mVkBindCache = new VkBindCache();
            }
            mVkBindCache->mNumVerts = mNumVerts;
            mVkBindCache->mTypeMask = mTypeMask;
            mVkBindCache->mMask = data_mask;
            mVkBindCache->mArray = mVertexBufferId;
            mVkBindCache->mElem = mIndexBufferId;
            LLVkAsync::drawBindingGet(mVkBindCache->mBinding);
        }
        sRenderBufferId = mVertexBufferId;
        sRenderIndicesId = mIndexBufferId;
        sLastMask = data_mask;
    }
}


// virtual (default)
void LLVertexBuffer::setupVertexBuffer()
{
    U8* base = nullptr;

    U32 data_mask = LLShaderProgram::sCurBoundShaderPtr->mAttributeMask;

    if (data_mask & MAP_NORMAL)
    {
        AttributeType loc = TYPE_NORMAL;
        void* ptr = (void*)(base + mOffsets[TYPE_NORMAL]);
        ll_attribPointer(loc, 3, ComponentType::F32, false, LLVertexBuffer::sTypeSize[TYPE_NORMAL], ptr);
    }
    if (data_mask & MAP_TEXCOORD3)
    {
        AttributeType loc = TYPE_TEXCOORD3;
        void* ptr = (void*)(base + mOffsets[TYPE_TEXCOORD3]);
        ll_attribPointer(loc, 2, ComponentType::F32, false, LLVertexBuffer::sTypeSize[TYPE_TEXCOORD3], ptr);
    }
    if (data_mask & MAP_TEXCOORD2)
    {
        AttributeType loc = TYPE_TEXCOORD2;
        void* ptr = (void*)(base + mOffsets[TYPE_TEXCOORD2]);
        ll_attribPointer(loc, 2, ComponentType::F32, false, LLVertexBuffer::sTypeSize[TYPE_TEXCOORD2], ptr);
    }
    if (data_mask & MAP_TEXCOORD1)
    {
        AttributeType loc = TYPE_TEXCOORD1;
        void* ptr = (void*)(base + mOffsets[TYPE_TEXCOORD1]);
        ll_attribPointer(loc, 2, ComponentType::F32, false, LLVertexBuffer::sTypeSize[TYPE_TEXCOORD1], ptr);
    }
    if (data_mask & MAP_TANGENT)
    {
        AttributeType loc = TYPE_TANGENT;
        void* ptr = (void*)(base + mOffsets[TYPE_TANGENT]);
        ll_attribPointer(loc, 4, ComponentType::F32, false, LLVertexBuffer::sTypeSize[TYPE_TANGENT], ptr);
    }
    if (data_mask & MAP_TEXCOORD0)
    {
        AttributeType loc = TYPE_TEXCOORD0;
        void* ptr = (void*)(base + mOffsets[TYPE_TEXCOORD0]);
        ll_attribPointer(loc, 2, ComponentType::F32, false, LLVertexBuffer::sTypeSize[TYPE_TEXCOORD0], ptr);
    }
    if (data_mask & MAP_COLOR)
    {
        AttributeType loc = TYPE_COLOR;
        //bind emissive instead of color pointer if emissive is present
        void* ptr = (data_mask & MAP_EMISSIVE) ? (void*)(base + mOffsets[TYPE_EMISSIVE]) : (void*)(base + mOffsets[TYPE_COLOR]);
        ll_attribPointer(loc, 4, ComponentType::U8, true, LLVertexBuffer::sTypeSize[TYPE_COLOR], ptr);
    }
    if (data_mask & MAP_EMISSIVE)
    {
        AttributeType loc = TYPE_EMISSIVE;
        void* ptr = (void*)(base + mOffsets[TYPE_EMISSIVE]);
        ll_attribPointer(loc, 4, ComponentType::U8, true, LLVertexBuffer::sTypeSize[TYPE_EMISSIVE], ptr);

        if (!(data_mask & MAP_COLOR))
        { //map emissive to color channel when color is not also being bound to avoid unnecessary shader swaps
            loc = TYPE_COLOR;
            ll_attribPointer(loc, 4, ComponentType::U8, true, LLVertexBuffer::sTypeSize[TYPE_EMISSIVE], ptr);
        }
    }
    if (data_mask & MAP_WEIGHT)
    {
        AttributeType loc = TYPE_WEIGHT;
        void* ptr = (void*)(base + mOffsets[TYPE_WEIGHT]);
        ll_attribPointer(loc, 1, ComponentType::F32, false, LLVertexBuffer::sTypeSize[TYPE_WEIGHT], ptr);
    }
    if (data_mask & MAP_WEIGHT4)
    {
        AttributeType loc = TYPE_WEIGHT4;
        void* ptr = (void*)(base + mOffsets[TYPE_WEIGHT4]);
        ll_attribPointer(loc, 4, ComponentType::F32, false, LLVertexBuffer::sTypeSize[TYPE_WEIGHT4], ptr);
    }
    if (data_mask & MAP_JOINT)
    {
        AttributeType loc = TYPE_JOINT;
        void* ptr = (void*)(base + mOffsets[TYPE_JOINT]);
        ll_attribIPointer(loc, 4, ComponentType::U16, LLVertexBuffer::sTypeSize[TYPE_JOINT], ptr);
    }
    if (data_mask & MAP_CLOTHWEIGHT)
    {
        AttributeType loc = TYPE_CLOTHWEIGHT;
        void* ptr = (void*)(base + mOffsets[TYPE_CLOTHWEIGHT]);
        ll_attribPointer(loc, 4, ComponentType::F32, true, LLVertexBuffer::sTypeSize[TYPE_CLOTHWEIGHT], ptr);
    }
    if (data_mask & MAP_TEXTURE_INDEX)
    {
        AttributeType loc = TYPE_TEXTURE_INDEX;
        void* ptr = (void*)(base + mOffsets[TYPE_VERTEX] + 12);
        ll_attribIPointer(loc, 1, ComponentType::U32, LLVertexBuffer::sTypeSize[TYPE_VERTEX], ptr);
    }
    if (data_mask & MAP_VERTEX)
    {
        AttributeType loc = TYPE_VERTEX;
        void* ptr = (void*)(base + mOffsets[TYPE_VERTEX]);
        ll_attribPointer(loc, 3, ComponentType::F32, false, LLVertexBuffer::sTypeSize[TYPE_VERTEX], ptr);
    }
}

void LLVertexBuffer::setPositionData(const LLVector4a* data)
{
    flush_vbo(BufferKind::Vertex, 0, sizeof(LLVector4a) * getNumVerts()-1, (U8*) data, mMappedData);
}

void LLVertexBuffer::setTexCoord0Data(const LLVector2* data)
{
    flush_vbo(BufferKind::Vertex, mOffsets[TYPE_TEXCOORD0], mOffsets[TYPE_TEXCOORD0] + sTypeSize[TYPE_TEXCOORD0] * getNumVerts() - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setTexCoord1Data(const LLVector2* data)
{
    flush_vbo(BufferKind::Vertex, mOffsets[TYPE_TEXCOORD1], mOffsets[TYPE_TEXCOORD1] + sTypeSize[TYPE_TEXCOORD1] * getNumVerts() - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setColorData(const LLColor4U* data)
{
    flush_vbo(BufferKind::Vertex, mOffsets[TYPE_COLOR], mOffsets[TYPE_COLOR] + sTypeSize[TYPE_COLOR] * getNumVerts() - 1, (U8*) data, mMappedData);
}

void LLVertexBuffer::setNormalData(const LLVector4a* data)
{
    flush_vbo(BufferKind::Vertex, mOffsets[TYPE_NORMAL], mOffsets[TYPE_NORMAL] + sTypeSize[TYPE_NORMAL] * getNumVerts() - 1, (U8*) data, mMappedData);
}

void LLVertexBuffer::setTangentData(const LLVector4a* data)
{
    flush_vbo(BufferKind::Vertex, mOffsets[TYPE_TANGENT], mOffsets[TYPE_TANGENT] + sTypeSize[TYPE_TANGENT] * getNumVerts() - 1, (U8*) data, mMappedData);
}

void LLVertexBuffer::setWeight4Data(const LLVector4a* data)
{
    flush_vbo(BufferKind::Vertex, mOffsets[TYPE_WEIGHT4], mOffsets[TYPE_WEIGHT4] + sTypeSize[TYPE_WEIGHT4] * getNumVerts() - 1, (U8*) data, mMappedData);
}

void LLVertexBuffer::setJointData(const U64* data)
{
    flush_vbo(BufferKind::Vertex, mOffsets[TYPE_JOINT], mOffsets[TYPE_JOINT] + sTypeSize[TYPE_JOINT] * getNumVerts() - 1, (U8*) data, mMappedData);
}

void LLVertexBuffer::setIndexData(const U16* data)
{
    flush_vbo(BufferKind::Index, 0, sizeof(U16) * getNumIndices() - 1, (U8*) data, mMappedIndexData);
}

void LLVertexBuffer::setIndexData(const U32* data)
{
    if (mIndicesType != ComponentType::U32)
    { // HACK -- vertex buffers are initialized as 16-bit indices, but can be switched to 32-bit indices
        mIndicesType = ComponentType::U32;
        mIndicesStride = 4;
        mNumIndices /= 2;
    }
    flush_vbo(BufferKind::Index, 0, sizeof(U32) * getNumIndices() - 1, (U8*)data, mMappedIndexData);
}

void LLVertexBuffer::setPositionData(const LLVector4a* data, U32 offset, U32 count)
{
    flush_vbo(BufferKind::Vertex, offset * sizeof(LLVector4a), (offset + count) * sizeof(LLVector4a) - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setNormalData(const LLVector4a* data, U32 offset, U32 count)
{
    flush_vbo(BufferKind::Vertex, mOffsets[TYPE_NORMAL] + offset * sTypeSize[TYPE_NORMAL], mOffsets[TYPE_NORMAL] + (offset + count) * sTypeSize[TYPE_NORMAL] - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setTexCoord0Data(const LLVector2* data, U32 offset, U32 count)
{
    flush_vbo(BufferKind::Vertex, mOffsets[TYPE_TEXCOORD0] + offset * sTypeSize[TYPE_TEXCOORD0], mOffsets[TYPE_TEXCOORD0] + (offset + count) * sTypeSize[TYPE_TEXCOORD0] - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setTexCoord1Data(const LLVector2* data, U32 offset, U32 count)
{
    flush_vbo(BufferKind::Vertex, mOffsets[TYPE_TEXCOORD1] + offset * sTypeSize[TYPE_TEXCOORD1], mOffsets[TYPE_TEXCOORD1] + (offset + count) * sTypeSize[TYPE_TEXCOORD1] - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setColorData(const LLColor4U* data, U32 offset, U32 count)
{
    flush_vbo(BufferKind::Vertex, mOffsets[TYPE_COLOR] + offset * sTypeSize[TYPE_COLOR], mOffsets[TYPE_COLOR] + (offset + count) * sTypeSize[TYPE_COLOR] - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setTangentData(const LLVector4a* data, U32 offset, U32 count)
{
    flush_vbo(BufferKind::Vertex, mOffsets[TYPE_TANGENT] + offset * sTypeSize[TYPE_TANGENT], mOffsets[TYPE_TANGENT] + (offset + count) * sTypeSize[TYPE_TANGENT] - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setWeight4Data(const LLVector4a* data, U32 offset, U32 count)
{
    flush_vbo(BufferKind::Vertex, mOffsets[TYPE_WEIGHT4] + offset * sTypeSize[TYPE_WEIGHT4], mOffsets[TYPE_WEIGHT4] + (offset + count) * sTypeSize[TYPE_WEIGHT4] - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setJointData(const U64* data, U32 offset, U32 count)
{
    flush_vbo(BufferKind::Vertex, mOffsets[TYPE_JOINT] + offset * sTypeSize[TYPE_JOINT], mOffsets[TYPE_JOINT] + (offset + count) * sTypeSize[TYPE_JOINT] - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setIndexData(const U16* data, U32 offset, U32 count)
{
    flush_vbo(BufferKind::Index, offset * sizeof(U16), (offset + count) * sizeof(U16) - 1, (U8*)data, mMappedIndexData);
}

void LLVertexBuffer::setIndexData(const U32* data, U32 offset, U32 count)
{
    if (mIndicesType != ComponentType::U32)
    { // HACK -- vertex buffers are initialized as 16-bit indices, but can be switched to 32-bit indices
        mIndicesType = ComponentType::U32;
        mIndicesStride = 4;
        mNumIndices /= 2;
    }
    flush_vbo(BufferKind::Index, offset * sizeof(U32), (offset + count) * sizeof(U32) - 1, (U8*)data, mMappedIndexData);
}






// ---- <FS> C3.5: the handle, the retire hook and the stand-in of the vertex buffer (llvertexbuffer.h)
std::atomic<U64> LLVBRef::sDeadResolved{0};

U32 LLVertexBuffer::getRenderHandle() const
{
    U32 handle = mRenderHandle.v.load(std::memory_order_acquire);
    if (handle)
    {
        return handle;
    }
    const U32 fresh = LLVBHandles::acquire(const_cast<LLVertexBuffer*>(this));
    if (!fresh)
    {
        return 0;
    }
    U32 expected = 0;
    if (!mRenderHandle.v.compare_exchange_strong(expected, fresh, std::memory_order_acq_rel))
    {
        LLVBHandles::release(fresh);       // another thread asked first
        return expected;
    }
    return fresh;
}

bool LLVertexBuffer::retireOnLastRef() const
{
    return LLRetireList::park(const_cast<LLVertexBuffer*>(this));
}

// static
LLVertexBuffer* LLVBRef::deadBuffer()
{
    ++sDeadResolved;
    static LLVertexBuffer* dead = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX);       // (never destroyed; never allocated: no GPU buffer, id 0)
    return dead;
}
