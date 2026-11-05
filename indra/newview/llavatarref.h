/**
 * @file llavatarref.h
 * @brief LLAvatarRef: an avatar reference that is a handle, not a counted pointer.
 *
 * doc/vulkan/PLAN-clean-core.md step C2.4d (the avatar counterpart of LLTexRef, LLGLTFMatRef, LLSkinRef). It holds no reference count and owns nothing: the avatar's owners (the object list, gAgentAvatarp, the
 * attachments) keep it alive and the retire list (kind 3) keeps it intact for the frames that could still read it after the last owner let go. get() resolves the handle each time and answers nullptr for an
 * avatar that is gone (counted: sDeadResolved, in the retire log line; healthy runs: 0). isNull / notNull / explicit bool / == nullptr / != nullptr are HANDLE based: "an avatar was assigned". That is what
 * classifies a draw as rigged (rigged != (p.mAvatar != nullptr)); a draw whose avatar is gone stays classed as rigged, its palette upload then fails (null avatar) and it is skipped, instead of being drawn
 * as an unskinned static mesh. isSelf() is null safe.
 */

#ifndef LL_LLAVATARREF_H
#define LL_LLAVATARREF_H

#include "llvoavatar.h"
#include <atomic>
#include <type_traits>

class LLAvatarRef
{
public:
    LLAvatarRef() = default;
    LLAvatarRef(std::nullptr_t) {}
    LLAvatarRef(LLVOAvatar* avatar) : mHandle(avatar ? avatar->getRenderHandle() : 0) {}
    template <class P, class = std::enable_if_t<!std::is_same<std::decay_t<P>, LLAvatarRef>::value && !std::is_convertible<P, LLVOAvatar*>::value, decltype(std::declval<const P&>().get())>>
    LLAvatarRef(const P& p) : LLAvatarRef(static_cast<LLVOAvatar*>(p.get())) {}
    LLAvatarRef& operator=(LLVOAvatar* avatar) { mHandle = avatar ? avatar->getRenderHandle() : 0; return *this; }
    LLAvatarRef& operator=(std::nullptr_t) { mHandle = 0; return *this; }
    template <class P, class = std::enable_if_t<!std::is_same<std::decay_t<P>, LLAvatarRef>::value && !std::is_convertible<P, LLVOAvatar*>::value, decltype(std::declval<const P&>().get())>>
    LLAvatarRef& operator=(const P& p) { return *this = static_cast<LLVOAvatar*>(p.get()); }

    LLVOAvatar* get() const
    {
        if (!mHandle) return nullptr;
        LLVOAvatar* avatar = LLAvatarHandles::resolve(mHandle);
        if (!avatar) ++sDeadResolved;
        return avatar;
    }
    operator LLVOAvatar*() const { return get(); }
    LLVOAvatar* operator->() const { return get(); }
    LLVOAvatar& operator*() const { return *get(); }
    bool isNull() const { return mHandle == 0; }
    bool notNull() const { return mHandle != 0; }
    explicit operator bool() const { return mHandle != 0; }
    bool operator==(std::nullptr_t) const { return mHandle == 0; }
    bool operator!=(std::nullptr_t) const { return mHandle != 0; }
    bool isSelf() const { const LLVOAvatar* avatar = get(); return avatar && avatar->isSelf(); }
    U32 handle() const { return mHandle; }

    static std::atomic<U64> sDeadResolved;     // resolutions of a handle whose avatar was gone
private:
    U32 mHandle = 0;
};

#endif // LL_LLAVATARREF_H
