/**
 * @file rksettingsrename.h
 * @brief Settings that carried an FS prefix before the Rekindled rename (2026-11-18) and their new RK names.
 *
 * A user_settings file written by an older build still has the old keys. LLControlGroup::loadFromFile reads
 * such a key as the new name (the new name wins when both are in the file), so a saved value carries over
 * and the next save writes the new key only. Table: doc/vulkan/RENAME-LL-TO-RK.md.
 */
#ifndef RK_RKSETTINGSRENAME_H
#define RK_RKSETTINGSRENAME_H

#include <cstring>

namespace RKSettingsRename
{
struct Entry { const char* mOld; const char* mNew; };

inline const Entry* table(size_t& count)
{
    static const Entry sTable[] = {
        { "FSParallelShadowPrep", "RKParallelShadowPrep" },
        { "FSParallelShadowPrepAB", "RKParallelShadowPrepAB" },
        { "FSEarlySort", "RKEarlySort" },
        { "FSAsyncBuilders", "RKAsyncBuilders" },
        { "FSParallelShadowPrepCheck", "RKParallelShadowPrepCheck" },
        { "FSShadowCascadeAvatarCull", "RKShadowCascadeAvatarCull" },
        { "FSShadowCascadeCullMargin", "RKShadowCascadeCullMargin" },
        { "FSShadowCascadeCullMinSunElevation", "RKShadowCascadeCullMinSunElevation" },
        { "FSProbeUpdateInterval", "RKProbeUpdateInterval" },
        { "FSTextureUpdateDivisor", "RKTextureUpdateDivisor" },
        { "FSVkSnapVariant", "RKVkSnapVariant" },
        { "FSTextureUpdateBurstDivisor", "RKTextureUpdateBurstDivisor" },
        { "FSTextureUpdateBurstSeconds", "RKTextureUpdateBurstSeconds" },
        { "FSRtShadowFarCascadeInterval", "RKRtShadowFarCascadeInterval" },
        { "FSShadowFarCascadeInterval", "RKShadowFarCascadeInterval" },
        { "FSShadowFarCascadeFirst", "RKShadowFarCascadeFirst" },
        { "FSShadowFarCascadeMaxCameraMove", "RKShadowFarCascadeMaxCameraMove" },
        { "FSShadowFarCascadeMaxCameraTurn", "RKShadowFarCascadeMaxCameraTurn" },
        { "FSGpuZoneTimers", "RKGpuZoneTimers" },
        { "FSVulkanAsync", "RKVulkanAsync" },
        { "FSVkDLSS", "RKVkDLSS" },
        { "FSVkRayTracing", "RKVkRayTracing" },
        { "FSVkMirrorQuality", "RKVkMirrorQuality" },
        { "FSVkRTWater", "RKVkRTWater" },
        { "FSVkPresentMode", "RKVkPresentMode" },
        { "FSRtNameByDistance", "RKRtNameByDistance" },
        { "FSUIRecordCache", "RKUIRecordCache" },
        { "FSUIRecordCacheHz", "RKUIRecordCacheHz" },
        { "FSAvatarFarUpdateDistance", "RKAvatarFarUpdateDistance" },
        { "FSImpostorNearDistance", "RKImpostorNearDistance" },
        { "FSImpostorMaxDistance", "RKImpostorMaxDistance" },
        { "FSOverlayDepthMode", "RKOverlayDepthMode" },
        { "FSVkDLSSQuality", "RKVkDLSSQuality" },
        { "FSVkAmdDenoiseShadows", "RKVkAmdDenoiseShadows" },
        { "FSVkAmdDenoise", "RKVkAmdDenoise" },
        { "FSVkDLSSScale", "RKVkDLSSScale" },
        { "FSDecodedTextureCache", "RKDecodedTextureCache" },
        { "FSTextureFetchGuessLevel", "RKTextureFetchGuessLevel" },
        { "FSTextureFetchGuessSize", "RKTextureFetchGuessSize" },
        { "FSTextureFetchHighWater", "RKTextureFetchHighWater" },
        { "FSTextureFetchWake", "RKTextureFetchWake" },
        { "FSTextureArrivalTrace", "RKTextureArrivalTrace" },
        { "FSDecodedTextureCacheSizeMB", "RKDecodedTextureCacheSizeMB" },
        { "FSVkNeuralRenderMode", "RKVkNeuralRenderMode" },
        { "FSSkipStaticRiggedAttachmentUpdates", "RKSkipStaticRiggedAttachmentUpdates" },
        { "FSSkipStaticRigidAttachmentUpdates", "RKSkipStaticRigidAttachmentUpdates" },
        { "FSStaggerFarRiggedAttachments", "RKStaggerFarRiggedAttachments" },
        { "FSReduceHiddenAvatarUpdates", "RKReduceHiddenAvatarUpdates" },
        { "FSTextureUpdateSteadyFactor", "RKTextureUpdateSteadyFactor" },
        { "FSTextureUpdateSteadySeconds", "RKTextureUpdateSteadySeconds" },
        { "FSWaterTraceAdaptive", "RKWaterTraceAdaptive" },
        { "FSSnapshotWarmupFrames", "RKSnapshotWarmupFrames" },
        { "FSCASWithUpscaler", "RKCASWithUpscaler" },
        { "FSGBufferSlimNormal", "RKGBufferSlimNormal" },
        { "FSTexAnimOnGPU", "RKTexAnimOnGPU" },
        { "FSSkipHiddenTextureAnim", "RKSkipHiddenTextureAnim" },
        { "FSSkipUnchangedBBoxOverride", "RKSkipUnchangedBBoxOverride" },
        { "FSParallelShadowPointCloud", "RKParallelShadowPointCloud" },
        { "FSObjectExpiry", "RKObjectExpiry" },
        { "FSFlexibleStayInGroup", "RKFlexibleStayInGroup" },
        { "FSSSRWaterTrace", "RKSSRWaterTrace" },
        { "FSSSRSurfaceTrace", "RKSSRSurfaceTrace" },
        { "FSVulkanRenderer", "RKVulkanRenderer" },
        { "FSParallelAvatarMotion", "RKParallelAvatarMotion" },
        { "FSRekindledStartPage", "RKRekindledStartPage" },
        { "FSHeroProbeIgnoreOthersAttachments", "RKHeroProbeIgnoreOthersAttachments" },
        { "FSHeroProbeMaxDistance", "RKHeroProbeMaxDistance" },
        { "FSAudioHRTF", "RKAudioHRTF" },
        { "FSVkNeuralRender", "RKVkNeuralRender" },
        { "FSVkUpscaler", "RKVkUpscaler" },
        { "FSVkGlossyMirror", "RKVkGlossyMirror" },
    };
    count = sizeof(sTable) / sizeof(sTable[0]);
    return sTable;
}

// The new name for an old one, or nullptr when the name was never renamed.
inline const char* newNameFor(const char* old_name)
{
    if (old_name[0] != 'F' || old_name[1] != 'S')
    {
        return nullptr;
    }
    size_t count = 0;
    const Entry* t = table(count);
    for (size_t i = 0; i < count; ++i)
    {
        if (std::strcmp(t[i].mOld, old_name) == 0)
        {
            return t[i].mNew;
        }
    }
    return nullptr;
}
}

#endif
