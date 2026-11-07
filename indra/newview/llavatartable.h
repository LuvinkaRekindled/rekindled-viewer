/**
 * @file llavatartable.h
 * @brief The avatar table (doc/vulkan/PLAN-avatars.md, stage A): one row per avatar, filled by ONE writer once a frame before the avatar update loop. The row holds what the five scattered
 *        "does this avatar need work this frame" gates of the update loop used to derive from the object graph, one after the other: the importance class, and the frame's mode.
 *        LL_VK_AVTABLE: 0 the old gates, 1 (default) the loop reads the row, 2 both: the loop uses the old answer and counts the disagreements with the row (log tag AvTable, every 600 frames).
 */
#ifndef LL_LLAVATARTABLE_H
#define LL_LLAVATARTABLE_H

#include "stdtypes.h"
#include <vector>

class LLVOAvatar;
class LLViewerObject;

class LLAvatarTable
{
public:
    // what the avatar update does with the avatar this frame
    enum Mode : U8
    {
        MODE_OTHER = 0,     // the update leaves before the gates (dead, exiting, friends only, render type off, not built)
        MODE_NONE,          // idleUpdate returns at once (a hidden avatar's reduced update: every 2nd or 4th frame)
        MODE_SKIP,          // updateCharacter's early out (an impostor's or a far avatar's period): motions in the hidden form, nothing else
        MODE_MATRICES,      // not visible: motions in the hidden form, the skeleton's matrices in the batch, the completion
        MODE_POSE,          // the full update
        MODE_COUNT
    };
    // importance: 0 the viewer's own, 1 seen in the main view, 2 seen by nothing (3 reserved: seen only by a probe or snapshot)
    enum Class : U8 { CLASS_OWN = 0, CLASS_SEEN = 1, CLASS_UNSEEN = 2, CLASS_COUNT = 4 };

    struct Row
    {
        U8 mMode = MODE_OTHER;
        U8 mClass = CLASS_UNSEEN;
        bool mVisible = false;          // isVisible() at the start of the frame
        bool mNeedsUpdate = false;      // computeNeedsUpdate()
        U32 mFrame = 0;
    };

    static int switchMode();                                                         // LL_VK_AVTABLE
    static void beginFrame(const std::vector<LLViewerObject*>& list, size_t count);  // the only writer: LLViewerObjectList::update, before the avatars' idleUpdate
    static const Row* row(const LLVOAvatar* av);                                     // null when the avatar has no row this frame
    static void noteActual(const LLVOAvatar* av, U8 actual);                         // mode 2: what the old gates did
};

#endif
