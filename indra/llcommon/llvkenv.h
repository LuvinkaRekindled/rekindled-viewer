/**
 * @file llvkenv.h
 * @brief The defaults of the stage 8 stack of doc/vulkan/PLAN-visible-set-owner.md (Rekindled)
 *
 * ll_vk_env(name) is getenv(name) for the switches of the stack: the value in the environment if there is one,
 * else the stack's default (the worker owns the visible set: the table, its derive for the main view, the kept pass,
 * the derived stamps, the avatar, bridge and animesh region partitions, the cascades from the worker's sets).
 * LL_VK_STACK=0 turns every default off (the old path, for a before and after); a switch set in the environment
 * always wins. The old path goes when the plan's stage 11 deletes it.
 */
#ifndef LL_LLVKENV_H
#define LL_LLVKENV_H

#include <cstdlib>
#include <cstring>

inline const char* ll_vk_env(const char* name)
{
    if (const char* v = std::getenv(name))
    {
        return v;
    }
    static const bool stack = !(std::getenv("LL_VK_STACK") && std::atoi(std::getenv("LL_VK_STACK")) == 0);
    if (!stack)
    {
        return nullptr;
    }
    static const struct { const char* mName; const char* mValue; } defaults[] =
    {
        { "LL_VK_CULL_SHADOW", "1" },   // the group table and the worker's copy of it
        { "LL_VK_CULL_OWN", "1" },      // the main view is drawn from the worker's own derive
        { "LL_VK_LOOPC", "1" },         // the sort's loop (c) removed for the covered groups (mesh registry, check skip, LOD candidates)
        { "LL_VK_CONSUMERS", "1" },     // the consumers' lists from the worker
        { "LL_VK_CULL_ALPHA", "4" },    // the alpha lists from the worker's
        { "LL_VK_DIST", "1" },          // the distance and the alpha dirtiness are the worker's
        { "LL_VK_KEPT", "2" },          // the walk of the covered partitions replaced by the worker's kept set
        { "LL_VK_KSTAMP", "1" },        // the world stamp derived from the kept set
        { "LL_VK_CULL_COMPARE", "0" },  // no comparison with the walk
        { "LL_VK_CASC_KEPT", "3" },     // the sun cascades and the spot passes take their groups from the worker's sets
        { "LL_VK_COVER_AV", "1" },      // the avatar, bridge and animated object region partitions are covered too
    };
    for (const auto& d : defaults)
    {
        if (std::strcmp(name, d.mName) == 0)
        {
            return d.mValue;
        }
    }
    return nullptr;
}

#endif
