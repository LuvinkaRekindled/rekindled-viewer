/**
 * @file llvkupscale.cpp
 * @brief The upscaler of the Vulkan mirror over DLSS and FSR (see llvkupscale.h)
 */

#include "linden_common.h"

#if defined(LL_VULKAN)

#include "llvkupscale.h"
#include "llvkfsr.h"

namespace LLVkUpscale
{

namespace
{
enum Backend { NONE, DLSS, FSR };
Backend gBackend = NONE;

// LL_VK_UPSCALER: "dlss" or "fsr" forces that backend, anything else (or nothing) lets DLSS go first
Backend forced()
{
    static const Backend mode = []() {
        char buf[16] = {};
        if (GetEnvironmentVariableA("LL_VK_UPSCALER", buf, sizeof(buf)) > 0)
        {
            if (_stricmp(buf, "fsr") == 0) return FSR;
            if (_stricmp(buf, "dlss") == 0) return DLSS;
        }
        return NONE;
    }();
    return mode;
}
}

bool requested()
{
    return LLVkDlss::requested();
}

void addInstanceExtensions(std::vector<const char*>& out)
{
    if (forced() != FSR)
    {
        LLVkDlss::addInstanceExtensions(out);
    }
}

void addDeviceExtensions(VkInstance instance, VkPhysicalDevice device, std::vector<const char*>& out)
{
    if (forced() != FSR)
    {
        LLVkDlss::addDeviceExtensions(instance, device, out);
    }
    if (forced() != DLSS)
    {
        LLVkFsr::addDeviceExtensions(device, out);
    }
}

bool init(VkInstance instance, VkPhysicalDevice physical, VkDevice device)
{
    if (gBackend != NONE)
    {
        return true;
    }
    if (forced() != FSR && LLVkDlss::init(instance, physical, device))
    {
        gBackend = DLSS;
    }
    else if (forced() != DLSS && LLVkFsr::init(instance, physical, device))
    {
        gBackend = FSR;
    }
    LL_INFOS("VkUpscale") << "upscaler: " << backendName() << LL_ENDL;
    return gBackend != NONE;
}

bool ready()
{
    return gBackend != NONE;
}

void shutdown()
{
    LLVkDlss::shutdown();
    LLVkFsr::shutdown();
    gBackend = NONE;
}

const char* backendName()
{
    return gBackend == DLSS ? "DLSS" : (gBackend == FSR ? "FSR" : "none");
}

bool backendIsFsr()
{
    return gBackend == FSR;
}

bool rayReconstructionAvailable()
{
    return gBackend == DLSS && LLVkDlss::rayReconstructionAvailable();
}

bool evaluate(VkCommandBuffer cmd, const Params& p)
{
    switch (gBackend)
    {
    case DLSS: return LLVkDlss::evaluate(cmd, p);
    case FSR: return LLVkFsr::evaluate(cmd, p);
    default: return false;
    }
}

float lodBias()
{
    const float bias = LLVkDlss::lodBias();         // (the one value both backends share: log2 of the render scale, set by the pipeline)
    return (gBackend == FSR && bias < 0.f) ? bias - 1.f : bias;
}

void setLodBias(float bias)
{
    LLVkDlss::setLodBias(bias);
}

VkImageLayout inputLayout(const LLVk::Image& image)
{
    return gBackend == FSR ? LLVkFsr::inputLayout(image) : LLVkDlss::inputLayout(image);
}

VkImageLayout outputLayout()
{
    return gBackend == FSR ? LLVkFsr::outputLayout() : LLVkDlss::outputLayout();
}

}

#endif // LL_VULKAN
