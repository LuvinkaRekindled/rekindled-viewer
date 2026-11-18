/**
 * @file rkvkupscale.cpp
 * @brief The upscaler of the Vulkan mirror over AMD FSR (see rkvkupscale.h)
 */

#include "linden_common.h"

#if defined(LL_VULKAN)

#include "rkvkupscale.h"
#include "rkvkfsr.h"
#include "rkvkngx.h"

#include <atomic>
#include <cmath>

namespace LLVkUpscale
{

namespace
{
bool gFsr = false;

std::atomic<float> sLodBias(0.f);       // read by the Vulkan thread when it makes a sampler, written when the render scale changes
bool sLodBiasLocked = false;            // LL_VK_DLSS_LODBIAS is set: it stays

// the value at start: the environment (the scale the viewer started with, or an explicit bias)
void initLodBias()
{
    static const bool done = []() {
        char buf[32] = {};
        if (GetEnvironmentVariableA("LL_VK_DLSS_LODBIAS", buf, sizeof(buf)) > 0)
        {
            sLodBias.store((float)atof(buf));
            sLodBiasLocked = true;
        }
        else if (requested() && GetEnvironmentVariableA("LL_VK_DLSS_SCALE", buf, sizeof(buf)) > 0)
        {
            const float scale = (float)atof(buf);
            if (scale > 0.1f && scale < 1.f)
            {
                sLodBias.store(log2f(scale));
            }
        }
        return true;
    }();
    (void)done;
}
}

bool requested()
{
    static const bool on = GetEnvironmentVariableA("LL_VK_DLSS", nullptr, 0) > 0;
    return on;
}

void addInstanceExtensions(std::vector<const char*>& out)
{
    LLVkNgx::addInstanceExtensions(out);        // (DLSS-NR, local builds only: nothing in a release build)
}

void addDeviceExtensions(VkInstance instance, VkPhysicalDevice device, std::vector<const char*>& out)
{
    LLVkNgx::addDeviceExtensions(instance, device, out);
    LLVkFsr::addDeviceExtensions(device, out);
}

bool init(VkInstance instance, VkPhysicalDevice physical, VkDevice device)
{
    if (gFsr)
    {
        return true;
    }
    gFsr = LLVkFsr::init(instance, physical, device);
    LL_INFOS("VkUpscale") << "upscaler: " << backendName() << LL_ENDL;
    if (gFsr)
    {
        LLVkNgx::init(instance, physical, device);      // (DLSS-NR, local builds only; not fatal)
    }
    return gFsr;
}

bool ready()
{
    return gFsr;
}

void shutdown()
{
    LLVkNgx::shutdown();
    LLVkFsr::shutdown();
    gFsr = false;
}

const char* backendName()
{
    return gFsr ? "FSR" : "none";
}

bool backendIsFsr()
{
    return gFsr;
}

bool evaluate(VkCommandBuffer cmd, const Params& p)
{
    return gFsr && LLVkFsr::evaluate(cmd, p);
}

float lodBias()
{
    initLodBias();
    const float bias = sLodBias.load();
    return (gFsr && bias < 0.f) ? bias - 1.f : bias;        // (FSR asks for one more stop when it upscales)
}

void setLodBias(float bias)
{
    initLodBias();
    if (!sLodBiasLocked)
    {
        sLodBias.store(bias);
    }
}

VkImageLayout inputLayout(const LLVk::Image& image)
{
    return LLVkFsr::inputLayout(image);
}

VkImageLayout outputLayout()
{
    return LLVkFsr::outputLayout();
}

}

#endif // LL_VULKAN
