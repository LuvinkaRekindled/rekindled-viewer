/**
 * @file llviewertexturerecs.cpp
 * @brief See llviewertexturerecs.h.
 */

#include "llviewerprecompiledheaders.h"

#include "llviewertexturerecs.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "llface.h"
#include "lldrawable.h"
#include "llspatialpartition.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerregion.h"
#include "llviewertexture.h"

S32 LLTexRecs::sMode = []{ const char* e = getenv("LL_VK_TEXREC"); return e ? atoi(e) : 2; }();

namespace
{
enum : U32 { F_USE = 1, F_VOLATILE = 2, F_OCCLUDED = 4 };

struct Rec
{
    U32 mSlot = 0;                  // the slot handle of the texture (generation << 24 | index)
    F32 mCx = 0.f, mCy = 0.f, mCz = 0.f;        // the centre of the face, agent space
    F32 mSizeSq = 0.f, mSizeLen = 0.f;          // the half extents: squared length, length
    F32 mBound = 0.f;               // the bounding sphere radius of the face
    F32 mInvMinScale = 1.f;         // 1 / the texture scale the face is divided by
    U32 mFlags = 0;
};

struct Cam
{
    F32 mOrigin[3] = {}, mX[3] = {};
    F32 mPixelAngle = 0.f, mCosHalfFov = 0.f, mSpeed = 0.f, mAngSpeed = 0.f;
    F32 mBias = 1.f;                // LLViewerTexture::sDesiredDiscardBias
    F32 mBoost = 8.f;               // TextureCameraBoost
};

struct Batch
{
    std::vector<std::pair<U32, Rec>> mRecs;
    std::vector<std::pair<U32, U32>> mStamps;       // (slot handle, face stamp)
    Cam mCam;
    bool mHasCam = false;
};

// ---- main thread state
std::vector<Rec> sMain(1);                  // by record index (0 unused): what the worker has been told, or is about to be
std::vector<LLFace*> sFaceOf(1, nullptr);   // main thread only: the face of each record (null when free)
std::vector<U8> sIsDirty(1, 0);
std::vector<U8> sSent(1, 0);                // the worker has been sent this record in its current life
std::vector<U32> sFreeRecs;
std::vector<U32> sDirty;
std::vector<LLViewerTexture*> sTexOfSlot(1, nullptr);
std::vector<U8> sSlotGen(1, 0);
std::vector<U32> sFreeSlots;
std::vector<U8> sStampPending(1, 0);
std::vector<U32> sStampSlots;               // slot indices whose stamp is to be sent
std::vector<LLTexRecs::Result> sMainResults;
U64 sSeenSeq = 0;
LLTexRecs::Stats sStats;

// ---- shared (under sMtx)
std::mutex sMtx;
std::condition_variable sCv;
Batch sPending;
std::vector<LLTexRecs::Result> sResultsOut;
U64 sResultSeq = 0;
bool sStop = false;
std::thread* sThread = nullptr;
std::atomic<U32> sPassMsX100{ 0 }, sPassRecs{ 0 };
std::atomic<U64> sPasses{ 0 };

U32 slot_index(U32 handle) { return handle & 0xFFFFFF; }

U32 slot_of(LLViewerTexture* tex)
{
    if (tex->mRecSlot)
    {
        return tex->mRecSlot;
    }
    U32 idx;
    if (!sFreeSlots.empty())
    {
        idx = sFreeSlots.back();
        sFreeSlots.pop_back();
    }
    else
    {
        idx = (U32)sTexOfSlot.size();
        sTexOfSlot.push_back(nullptr);
        sSlotGen.push_back(0);
        sStampPending.push_back(0);
    }
    sSlotGen[idx] = (U8)(sSlotGen[idx] + 1 == 0 ? 1 : sSlotGen[idx] + 1);
    sTexOfSlot[idx] = tex;
    tex->mRecSlot = ((U32)sSlotGen[idx] << 24) | idx;
    return tex->mRecSlot;
}

// sIsDirty: 0 clean, 1 fill again from the face, 2 the record was freed (send it as it is), 3 send as it is (a flag changed)
void mark_dirty(U32 idx, U8 how = 1)
{
    if (idx && idx < sIsDirty.size())
    {
        U8& cur = sIsDirty[idx];
        if (cur == 0)
        {
            sDirty.push_back(idx);
            cur = how;
        }
        else if (cur != 2 && how != 3)      // (a freed record stays freed; "send as it is" does not weaken a refill)
        {
            cur = how;
        }
    }
}

inline U64 now_ns()
{
    return (U64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void note_stamp(U32 slot_handle)
{
    const U32 idx = slot_index(slot_handle);
    if (idx && idx < sStampPending.size() && !sStampPending[idx])
    {
        sStampPending[idx] = 1;
        sStampSlots.push_back(idx);
    }
}

// a face whose position comes from something only the main thread has
inline bool is_volatile(LLFace* f)
{
    LLViewerObject* objp = f->getViewerObject();
    LLDrawable* drawable = f->getDrawable();
    return !objp || !drawable || f->isState(LLFace::RIGGED) || f->hasMedia() || drawable->isActive() || objp->getPartitionType() == LLViewerRegion::PARTITION_PARTICLE;
}

// the record of a face as it is now
void fill(Rec& r, LLFace* f)
{
    LLViewerObject* objp = f->getViewerObject();
    LLDrawable* drawable = f->getDrawable();
    U32 flags = F_USE;
    const bool vol = is_volatile(f);
    if (vol)
    {
        flags |= F_VOLATILE;
    }
    else
    {
        const LLVector3 c = f->getPositionAgent();
        r.mCx = c.mV[0]; r.mCy = c.mV[1]; r.mCz = c.mV[2];
        LLVector4a size;
        size.setSub(f->mExtents[1], f->mExtents[0]);
        size.mul(0.5f);
        r.mSizeSq = size.dot3(size).getF32();
        r.mSizeLen = size.getLength3().getF32();
        r.mBound = f->getBoundingSphereRadius();

        static LLCachedControl<F32> scale_min(gSavedSettings, "TextureScaleMinAreaFactor", 0.0095f);
        static LLCachedControl<F32> scale_max(gSavedSettings, "TextureScaleMaxAreaFactor", 25.f);
        const S32 te_offset = f->getTEOffset();
        const LLTextureEntry* te = (te_offset < 0 || te_offset >= objp->getNumTEs()) ? nullptr : objp->getTE(te_offset);
        F32 min_scale = te ? llmin(fabsf(te->getScaleS()), fabsf(te->getScaleT())) : 1.f;
        min_scale = llclamp(min_scale * min_scale, (F32)scale_min, (F32)scale_max);
        r.mInvMinScale = 1.f / min_scale;

        LLSpatialGroup* g = drawable->getSpatialGroup();
        if (g)
        {
            const LLViewerCamera::eCameraID saved = LLViewerCamera::sCurCameraID;
            LLViewerCamera::sCurCameraID = LLViewerCamera::CAMERA_WORLD;
            if (g->isOcclusionState(LLOcclusionCullingGroup::OCCLUDED))
            {
                flags |= F_OCCLUDED;
            }
            LLViewerCamera::sCurCameraID = saved;
        }
    }
    r.mFlags = flags;
}

// LLFace::calcImportanceToCamera with the camera of the snapshot
F32 importance_to(const Cam& cam, F32 cos_angle, F32 dist)
{
    static const F32 kDist[4][2] = { { 16.1f, 1.0f }, { 32.1f, 0.5f }, { 48.1f, 0.2f }, { 96.1f, 0.05f } };
    static const F32 kAngle[4][2] = { { 0.985f, 1.0f }, { 0.94f, 0.8f }, { 0.866f, 0.64f }, { 0.0f, 0.36f } };
    if (cos_angle > cam.mCosHalfFov && dist < kDist[3][0])
    {
        if (cam.mSpeed > 10.0f || cam.mAngSpeed > 1.0f)
        {
            return 0.f;
        }
        int i = 0;
        for (; i < 4 && dist > kDist[i][0]; ++i) {}
        i = i < 3 ? i : 3;
        const F32 dist_factor = kDist[i][1];
        for (i = 0; i < 4 && cos_angle < kAngle[i][0]; ++i) {}
        i = i < 3 ? i : 3;
        return dist_factor * kAngle[i][1];
    }
    return 0.f;
}

void worker_main()
{
    std::vector<Rec> recs(1);
    std::vector<U32> stamps(1, 0), handles(1, 0);
    Cam cam;
    std::vector<LLTexRecs::Result> out;
    while (true)
    {
        Batch b;
        {
            std::unique_lock<std::mutex> lk(sMtx);
            sCv.wait_for(lk, std::chrono::milliseconds(250), [] { return sStop || sPending.mHasCam; });
            if (sStop)
            {
                return;
            }
            if (!sPending.mHasCam)
            {
                continue;
            }
            b = std::move(sPending);
            sPending = Batch();
        }
        const auto t0 = std::chrono::steady_clock::now();
        for (auto& p : b.mRecs)
        {
            if (p.first >= recs.size())
            {
                recs.resize(p.first + 1024);
            }
            recs[p.first] = p.second;
        }
        for (auto& p : b.mStamps)
        {
            const U32 idx = slot_index(p.first);
            if (idx >= stamps.size())
            {
                stamps.resize(idx + 1024, 0);
                handles.resize(idx + 1024, 0);
            }
            stamps[idx] = p.second;
            handles[idx] = p.first;
        }
        cam = b.mCam;

        const size_t slots = stamps.size();
        out.assign(slots, LLTexRecs::Result());
        for (size_t s = 0; s < slots; ++s)
        {
            out[s].mStamp = stamps[s];
            out[s].mHandle = handles[s];
        }
        U32 counted = 0;
        for (const Rec& r : recs)
        {
            if (!(r.mFlags & F_USE))
            {
                continue;
            }
            ++counted;
            const U32 s = slot_index(r.mSlot);
            if (s >= slots)
            {
                continue;
            }
            LLTexRecs::Result& res = out[s];
            ++res.mFaces;
            if (r.mFlags & F_OCCLUDED)
            {
                ++res.mOccluded;
            }
            if (r.mFlags & F_VOLATILE)
            {
                ++res.mVolatile;
                continue;
            }
            const F32 dx = r.mCx - cam.mOrigin[0], dy = r.mCy - cam.mOrigin[1], dz = r.mCz - cam.mOrigin[2];
            const F32 len = sqrtf(dx * dx + dy * dy + dz * dz);
            const F32 dist = llmax(len - r.mSizeLen, 0.001f);
            F32 cos_angle = len > 0.f ? (dx * cam.mX[0] + dy * cam.mX[1] + dz * cam.mX[2]) / len : 1.f;
            const F32 app_angle = atanf(sqrtf(r.mSizeSq) / dist);
            const F32 radius = app_angle * cam.mPixelAngle;
            const F32 area = radius * radius * 3.14159f;
            F32 importance;
            if (dist < r.mBound)
            {
                importance = 1.f;
            }
            else
            {
                importance = importance_to(cam, cos_angle, dist);
            }
            F32 vsize = area * r.mInvMinScale;
            vsize *= llmax(importance * cam.mBoost, 1.f);       // (every record is "in frustum": the face loop's flag for a plain face is always true)
            if (cam.mBias > 1.9f + importance / 2.f)
            {
                res.mMaxBiased = llmax(res.mMaxBiased, vsize);
            }
            else
            {
                res.mMaxPlain = llmax(res.mMaxPlain, vsize);
            }
        }
        const auto t1 = std::chrono::steady_clock::now();
        sPassMsX100.store((U32)(std::chrono::duration<double, std::milli>(t1 - t0).count() * 100.0), std::memory_order_relaxed);
        sPassRecs.store(counted, std::memory_order_relaxed);
        sPasses.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(sMtx);
            sResultsOut.swap(out);
            ++sResultSeq;
        }
        // at most about 30 passes a second
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
}
}   // namespace

void LLTexRecs::init()
{
    if (!sMode || sThread)
    {
        return;
    }
    sThread = new std::thread(worker_main);
}

void LLTexRecs::stop()
{
    if (!sThread)
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lk(sMtx);
        sStop = true;
    }
    sCv.notify_all();
    sThread->join();
    delete sThread;
    sThread = nullptr;
}

void LLTexRecs::faceAdded(LLViewerTexture* tex, U32 ch, LLFace* face)
{
    if (!sMode || !tex || !face || ch >= LLRender::NUM_TEXTURE_CHANNELS)
    {
        return;
    }
    init();
    const U32 slot = slot_of(tex);
    U32 idx;
    if (!sFreeRecs.empty())
    {
        idx = sFreeRecs.back();
        sFreeRecs.pop_back();
    }
    else
    {
        idx = (U32)sMain.size();
        sMain.emplace_back();
        sFaceOf.push_back(nullptr);
        sIsDirty.push_back(0);
        sSent.push_back(0);
    }
    sSent[idx] = 0;
    sMain[idx] = Rec();
    sMain[idx].mSlot = slot;
    sFaceOf[idx] = face;
    face->mRecIdx[ch] = idx;
    ++sStats.mRecords;
    mark_dirty(idx);
    note_stamp(slot);
}

void LLTexRecs::faceRemoved(LLViewerTexture* tex, U32 ch, LLFace* face)
{
    if (!sMode || !tex || !face || ch >= LLRender::NUM_TEXTURE_CHANNELS)
    {
        return;
    }
    const U32 idx = face->mRecIdx[ch];
    if (!idx || idx >= sMain.size())
    {
        return;
    }
    face->mRecIdx[ch] = 0;
    sMain[idx].mFlags = 0;          // the worker is told with the next batch
    sFaceOf[idx] = nullptr;
    note_stamp(sMain[idx].mSlot);
    mark_dirty(idx, 2);
    --sStats.mRecords;
}

void LLTexRecs::faceChanged(LLFace* face)
{
    if (!sMode || !face)
    {
        return;
    }
    ++sStats.mFaceChangedCalls;
    for (U32 ch = 0; ch < LLRender::NUM_TEXTURE_CHANNELS; ++ch)
    {
        if (face->mRecIdx[ch])
        {
            mark_dirty(face->mRecIdx[ch]);
        }
    }
}

void LLTexRecs::groupOcclusion(LLSpatialGroup* group, bool occluded)
{
    if (!sMode || !group)
    {
        return;
    }
    const U64 t0 = now_ns();
    ++sStats.mGroupEvents;
    for (LLSpatialGroup::element_iter i = group->getDataBegin(); i != group->getDataEnd(); ++i)
    {
        LLDrawable* drawable = (LLDrawable*)(*i)->getDrawable();
        if (!drawable)
        {
            continue;
        }
        for (S32 j = 0; j < drawable->getNumFaces(); ++j)
        {
            LLFace* face = drawable->getFace(j);
            if (!face)
            {
                continue;
            }
            for (U32 ch = 0; ch < LLRender::NUM_TEXTURE_CHANNELS; ++ch)
            {
                const U32 idx = face->mRecIdx[ch];
                if (idx && idx < sMain.size() && sFaceOf[idx] == face && !(sMain[idx].mFlags & F_VOLATILE))
                {
                    const bool was = (sMain[idx].mFlags & F_OCCLUDED) != 0;
                    if (was != occluded)
                    {
                        sMain[idx].mFlags = occluded ? (sMain[idx].mFlags | F_OCCLUDED) : (sMain[idx].mFlags & ~(U32)F_OCCLUDED);
                        mark_dirty(idx, 3);         // (sent as it is: nothing else about the face changed)
                    }
                }
            }
        }
    }
    sStats.mGroupNs += now_ns() - t0;
}

void LLTexRecs::textureGone(LLViewerTexture* tex)
{
    if (!tex || !tex->mRecSlot)
    {
        return;
    }
    const U32 idx = slot_index(tex->mRecSlot);
    if (idx < sTexOfSlot.size())
    {
        sTexOfSlot[idx] = nullptr;
        sFreeSlots.push_back(idx);
    }
    tex->mRecSlot = 0;
}

void LLTexRecs::textureStamped(LLViewerTexture* tex)
{
    if (sMode && tex && tex->mRecSlot)
    {
        note_stamp(tex->mRecSlot);
    }
}

void LLTexRecs::frame()
{
    if (!sMode)
    {
        return;
    }
    const U64 t0 = now_ns();
    Batch b;
    b.mRecs.reserve(sDirty.size());
    for (U32 idx : sDirty)
    {
        const U8 how = sIsDirty[idx];
        sIsDirty[idx] = 0;
        LLFace* f = sFaceOf[idx];
        if (how == 2 || !f)
        {
            b.mRecs.emplace_back(idx, sMain[idx]);          // freed: its flags are 0
            sFreeRecs.push_back(idx);
            continue;
        }
        if (how == 1)
        {
            if ((sMain[idx].mFlags & F_VOLATILE) && sSent[idx] && is_volatile(f))
            {
                ++sStats.mSkipped;                          // a moving face that stays volatile: nothing the worker uses has changed (most of the changes are these)
                continue;
            }
            Rec r = sMain[idx];
            fill(r, f);
            if (memcmp(&r, &sMain[idx], sizeof(Rec)) == 0 && sSent[idx])
            {
                ++sStats.mSkipped;                          // nothing the worker uses has changed (a volatile face that moved: its flags are the same)
                continue;
            }
            sMain[idx] = r;
        }
        sSent[idx] = 1;
        b.mRecs.emplace_back(idx, sMain[idx]);
    }
    sDirty.clear();
    for (U32 idx : sStampSlots)
    {
        sStampPending[idx] = 0;
        LLViewerTexture* tex = idx < sTexOfSlot.size() ? sTexOfSlot[idx] : nullptr;
        if (tex)
        {
            b.mStamps.emplace_back(tex->mRecSlot, tex->mFaceStamp);
        }
    }
    sStampSlots.clear();
    sStats.mDeltas += b.mRecs.size();
    ++sStats.mBatches;

    LLViewerCamera* camera = LLViewerCamera::getInstance();
    Cam& c = b.mCam;
    const LLVector3 o = camera->getOrigin(), x = camera->getXAxis();
    c.mOrigin[0] = o.mV[0]; c.mOrigin[1] = o.mV[1]; c.mOrigin[2] = o.mV[2];
    c.mX[0] = x.mV[0]; c.mX[1] = x.mV[1]; c.mX[2] = x.mV[2];
    c.mPixelAngle = LLDrawable::sCurPixelAngle;
    c.mCosHalfFov = camera->getCosHalfFov();
    c.mSpeed = camera->getAverageSpeed();
    c.mAngSpeed = camera->getAverageAngularSpeed();
    c.mBias = LLViewerTexture::sDesiredDiscardBias;
    static LLCachedControl<F32> boost(gSavedSettings, "TextureCameraBoost", 8.f);
    c.mBoost = (F32)boost;
    b.mHasCam = true;

    {
        std::lock_guard<std::mutex> lk(sMtx);
        if (sPending.mHasCam)
        {
            // the worker has not taken the last batch: the changes of this one come after it
            sPending.mRecs.insert(sPending.mRecs.end(), b.mRecs.begin(), b.mRecs.end());
            sPending.mStamps.insert(sPending.mStamps.end(), b.mStamps.begin(), b.mStamps.end());
            sPending.mCam = b.mCam;
        }
        else
        {
            sPending = std::move(b);
        }
        if (sResultSeq != sSeenSeq)
        {
            sMainResults.swap(sResultsOut);
            sSeenSeq = sResultSeq;
        }
    }
    sCv.notify_one();
    sStats.mSlots = (U32)sTexOfSlot.size();
    sStats.mFrameNs += now_ns() - t0;
    ++sStats.mFrames;
}

bool LLTexRecs::result(const LLViewerTexture* tex, Result& out)
{
    if (!tex || !tex->mRecSlot)
    {
        return false;
    }
    const U32 idx = slot_index(tex->mRecSlot);
    if (idx >= sMainResults.size() || sMainResults[idx].mHandle != tex->mRecSlot)
    {
        return false;
    }
    out = sMainResults[idx];
    return true;
}

LLTexRecs::Stats LLTexRecs::stats()
{
    Stats s = sStats;
    s.mPasses = sPasses.load(std::memory_order_relaxed);
    s.mPassMs = sPassMsX100.load(std::memory_order_relaxed) / 100.f;
    s.mPassRecords = sPassRecs.load(std::memory_order_relaxed);
    return s;
}
