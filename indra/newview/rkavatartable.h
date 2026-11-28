/**
 * @file rkavatartable.h
 * @brief The avatar table (doc/vulkan/PLAN-avatars.md, stage A): one row per avatar, filled by ONE writer once a frame before the avatar update loop. The row holds what the five scattered
 *        "does this avatar need work this frame" gates of the update loop used to derive from the object graph, one after the other: the importance class, and the frame's mode.
 *        The loop reads the row (an avatar without a row this frame keeps the old gates).
 */
#ifndef RK_RKAVATARTABLE_H
#define RK_RKAVATARTABLE_H

#include "stdtypes.h"
#include <vector>

class LLVOAvatar;
class LLViewerObject;

class RKAvatarTable
{
public:
    // what the avatar update does with the avatar this frame
    enum Mode : U8
    {
        MODE_OTHER = 0,     // the update leaves before the gates (dead, exiting, friends only, render type off, not built)
        MODE_NONE,          // idleUpdate returns at once (a hidden avatar's reduced update: every 2nd or 4th frame)
        MODE_SKIP,          // updateCharacter's early out (an impostor's or a far avatar's period): motions in the hidden form, nothing else
        MODE_MATRICES,      // not visible: motions in the hidden form, the skeleton's matrices in the batch, the completion
        MODE_POSE           // the full update
    };
    // importance: 0 the viewer's own, 1 seen in the main view, 2 seen by nothing (3 reserved: seen only by a probe or snapshot)
    enum Class : U8 { CLASS_OWN = 0, CLASS_SEEN = 1, CLASS_UNSEEN = 2, CLASS_COUNT = 4 };

    // <FS> ownership map step C: what has changed for this avatar, decided once a frame (LLVOAvatar::pendingEvents) and read by the handlers, which used to ask it every frame for every avatar: a handler runs only when
    // its bit is set. Each bit is a superset of when its handler does anything; an avatar that leaves before the update gates (not built, dead) has all of them.
    enum Events : U8
    {
        EV_SIT = 1,             // a seat to sit on or to get off (updateCharacter: sitOnObject, getOffObject)
        EV_APPEARANCE = 2,      // the overall appearance changed, or a jelly-doll (its sit / stand motion is chosen every frame): updateOverallAppearance
        EV_TEXTURE = 4,         // the visibility differs from the baked texture callbacks' state, or the first full load is still counting: checkTextureLoading
        EV_LOADING = 8,         // not fully loaded: idleUpdateLoadingEffect (the cloud, the first visible handlers)
        EV_DEBUG = 16,          // the debug text: a debug setting, a text object, the own avatar and animesh (their overrides add text)
        EV_SKELETON = 32,       // what the flat joint arrays are made from changed (LLCharacter::skeletonChanged): ensureJointLocals
        EV_ALL = 63
    };

    // what the presentation of the avatar has to do this frame (step 2: one bit at a time, each decided here and read by the piece)
    enum Bits : U16
    {
        BIT_ATTACHMENT_LOOP = 2,        // the attachment loop and the detailed presentation (lip sync, loading effect, wind) run this frame: the full update of a visible avatar (what updateCharacter's result was)
        BIT_NAME_TAG = 1,               // idleUpdateNameTag: the tag can be on the screen (the avatar is in the camera's frustum, occlusion does NOT count: tags show through walls), or it is the viewer's own, or chat / typing shows
    };

    struct Row
    {
        U8 mMode = MODE_OTHER;
        U8 mClass = CLASS_UNSEEN;
        bool mVisible = false;          // isVisible() at the start of the frame (the cull's stamp: occlusion counts)
        bool mNeedsUpdate = false;      // computeNeedsUpdate()
        bool mInFrustum = true;         // the avatar's extents (grown by the room a name tag needs) against the main camera's frustum, no occlusion
        bool mFinal = false;            // endFrame has run: the facts below are the frame's final ones (the readers of the render side take them)
        bool mImpostor = false;         // isImpostor() after the avatars' update (its inputs: update period, muted, too slow, RLV silhouette, exclusion settings are final then)
        bool mTooSlow = false;          // isTooSlow()
        bool mMuted = false;            // isVisuallyMuted()
        U8 mAppearance = 0;             // getOverallAppearance() (LLVOAvatar::AvatarOverallAppearance: normal, jellydoll, invisible)
        bool mVoice = false;            // PLAN-avatars.md B3d: the avatar is a participant of a voice channel (LLVoiceClient::getVoiceEnabled), taken once a frame from the participant list, not asked of the voice client per avatar
        bool mSpeaking = false;         // ... and the voice client says it is speaking (LLVoiceClient::getIsSpeaking), asked for the participants only
        bool mShowSpeaking = false;     // <FS> step 27 (D): speaking and not on the mute list (or the own avatar): what the voice visualizer shows (isInMuteList writes a cache: asked here, on the main thread, for the speakers only)
        F32 mPower = 0.f;               // ... and its voice power (LLVoiceClient::getCurrentPower), asked once a frame for the speakers
        U16 mBits = 0;
        U32 mFrame = 0;
        // <FS> ownership map step B: who runs what this frame, decided once here (LLVOAvatar::planPoolJob) and read by idleUpdate and updateCharacter, which used to decide it each for itself
        bool mRootJob = false;          // the root position and rotation (and the below water fact) are made at the start of the avatar's pool job
        bool mFrontJob = false;         // and so are the moves the avatar's idleUpdate used to make first (the drawable's move, the object's interpolation): the job's writes to shared state are one ledger replayed in the completion
        U8 mWave = 0;                   // <FS> the frame order by parents: 0 = nothing it waits for; an animated object worn by an avatar is one wave after that avatar (its placement reads the host's final pose). Made once a frame in beginFrame; every reader takes it from here, not from the live question
        U8 mEvents = EV_ALL;            // step C
        S8 mCanDefer = -1;              // canDeferMotionUpdate() when it was asked for the plan (-1: not asked: the live answer is used)
    };

    static void beginFrame(const std::vector<LLViewerObject*>& list, size_t count);  // the only writer: LLViewerObjectList::update, before the avatars' idleUpdate
    static const Row* row(const LLVOAvatar* av);                                     // null when the avatar has no row this frame
    static void endFrame();                                                          // after the avatars' update (the motion batch and the completions): the facts the render side reads
    static const Row* finalRow(const LLVOAvatar* av);                                // the row once endFrame has made it final this frame, else null (the caller derives the answer as before)
    static void noteCheck(int which, bool same);                                     // RK_VK_AVTABLE_VERIFY=1: the cached answer (0 impostor, 1 too slow, 2 visually muted, 3 overall appearance) against the live one
    static bool attachmentLoop(const LLVOAvatar* av, bool from_update);              // step 3: the loop's gate, from the row (the result of updateCharacter is only compared with it)
    static bool voiceProximal();                                                     // B3d: LLVoiceClient::inProximalChannel() once a frame (false without voice participants)
    static bool voiceEnabled(const LLVOAvatar* av);                                  // B3d: the avatar's voice enabled fact from the row (the voice client asked when there is no row)
    static bool voiceSpeaking(const LLVOAvatar* av);                                 // B3d: the speaking fact from the row
    static bool voiceShown(const LLVOAvatar* av);                                    // step 27: voice enabled for the avatar and (calls shown or the proximal channel): what the visualizer and the lip sync run on (settings read once a frame)
    static bool voiceShowSpeaking(const LLVOAvatar* av);                             // step 27 (D): speaking and not muted (or own), from the row
    static F32 voicePower(const LLVOAvatar* av);                                     // step 27 (D): the voice power from the row
    static bool voiceVisualizerOn();                                                 // step 27: the VoiceVisualizerEnabled setting, read once a frame
    static U8 wave(const LLVOAvatar* av);                                            // the frame order by parents: the avatar's wave from the row (0 without a row: placed where it always was)
    static U8 events(const LLVOAvatar* av);                                        // step C: the row's event bits (all of them without a row)
    static bool rootJob(const LLVOAvatar* av);                                       // step B: the row's plan
    static bool frontJob(const LLVOAvatar* av);
    static S8 canDefer(const LLVOAvatar* av);
    static bool nameTagOpen(const LLVOAvatar* av);                                   // step 2: may idleUpdateNameTag be left out this frame? (counts, logs every 600 frames)
};

#endif
