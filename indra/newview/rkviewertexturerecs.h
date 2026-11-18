/**
 * @file rkviewertexturerecs.h
 * @brief The size a texture needs on the screen, worked out off the main thread from plain records of its faces.
 *
 * doc/vulkan/PLAN-texture-handles.md section 17. The face loop of LLViewerTextureList::updateImageDecodePriority reads LLFace, LLDrawable and LLViewerObject, which belong to the main thread, so it
 * cannot run anywhere else. The main thread keeps one small record per (face, texture channel) as plain data (where the face is, how big, the texture scale, whether it is in an occluded group), tells
 * the worker what changed (a delta log, like the handle map's), and the worker works out, for every texture, the largest size any of its faces asks for. It owns a private copy of the records: nothing
 * is shared except the two queues. The main thread takes the results when it looks at a texture: a result carries the face stamp of the texture it was made for, so one made for another set of faces
 * is not used.
 *
 * Faces whose position comes from something only the main thread has (rigged, media, particles, a moving object) are marked volatile: their textures keep the face loop.
 */

#ifndef LL_LLVIEWERTEXTURERECS_H
#define LL_LLVIEWERTEXTURERECS_H

#include "stdtypes.h"

class LLFace;
class LLViewerTexture;
class LLSpatialGroup;

class RKTexRecs
{
public:
    // what the worker found for one texture (a slot of the registry)
    struct Result
    {
        F32 mMaxPlain = 0.f;        // the largest size of the faces that are not divided by the texture's bias scale
        F32 mMaxBiased = 0.f;       // ... of the faces that are (the caller divides it by the scale, which depends on the texture)
        U32 mFaces = 0;             // faces counted (all channels)
        U32 mOccluded = 0;          // ... of them in an occluded group
        U32 mVolatile = 0;          // ... of them volatile (any: the result is not used)
        U32 mStamp = 0;             // the texture's face stamp when the worker last heard of a change of its faces
        U32 mHandle = 0;            // the slot handle this was made for
    };

    static void init();
    static void stop();

    // the face loop's own events (LLViewerTexture::addFace / removeFace, LLFace::stampTextures ...), main thread
    static void faceAdded(LLViewerTexture* tex, U32 ch, LLFace* face);
    static void faceRemoved(LLViewerTexture* tex, U32 ch, LLFace* face);
    static void faceChanged(LLFace* face);                          // anything the record holds may have changed
    static void groupOcclusion(LLSpatialGroup* group, bool occluded);   // a group of the world camera became occluded / visible: its faces' records follow
    static void textureGone(LLViewerTexture* tex);
    static void textureStamped(LLViewerTexture* tex);              // the texture's face stamp went up: the worker is told the new value with the next batch

    // once a frame, main thread, before the textures are looked at: fills the records that changed, hands them and the camera to the worker, takes the results it has finished
    static void frame();

    // the latest result for the texture, false when there is none (the worker has not seen the texture yet)
    static bool result(const LLViewerTexture* tex, Result& out);

    // for the log: records, slots, what the last pass cost
    struct Stats
    {
        U32 mRecords = 0, mSlots = 0;
        U64 mDeltas = 0, mBatches = 0, mPasses = 0;
        F32 mPassMs = 0.f;
        U32 mPassRecords = 0;
        U64 mFrameNs = 0, mGroupNs = 0, mFrames = 0;                     // main thread time in frame() and in the group events, frames
        U64 mGroupEvents = 0, mFaceChangedCalls = 0, mSkipped = 0;       // group events, faceChanged calls, records not sent because nothing changed
    };
    static Stats stats();
};

#endif // LL_LLVIEWERTEXTURERECS_H
