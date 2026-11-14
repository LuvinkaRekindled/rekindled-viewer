/**
 * @file llvkdenoise.cpp
 * @brief The AMD FidelityFX Denoiser 1.3 for the traced reflections (see llvkdenoise.h, doc/vulkan/PLAN-amd-denoiser.md)
 */

#include "linden_common.h"

#if defined(LL_VULKAN)

#include "llvkdenoise.h"

#if defined(LL_FFX_DENOISER)
#include <vulkan/vulkan.h>
#include <FidelityFX/host/ffx_denoiser.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#endif

#include <cstdlib>
#include <cstring>

namespace LLVkDenoise
{

using namespace LLVk;       // the loaded Vulkan entry points live there

#if defined(LL_FFX_DENOISER)

namespace
{
bool gReady = false;
VkPhysicalDevice gPhysical = VK_NULL_HANDLE;
VkDevice gDevice = VK_NULL_HANDLE;
VkDeviceContext gDeviceContext = {};        // (the FidelityFX device points at it: it has to stay)
FfxInterface gBackend = {};
void* gScratch = nullptr;

bool gContextMade = false;
FfxDenoiserContext gContext = {};
U32 gWidth = 0, gHeight = 0;
Image gRadianceB, gVarianceA, gVarianceB;       // the denoiser's own images of this size
Buffer gTiles, gArgs;                           // the tile list (every 8x8 tile of the picture) and the indirect arguments of its three dispatches (one workgroup for a tile)
bool gFirst = true;                             // the first dispatch after the context was made: the history is empty

bool ffx_ok(FfxErrorCode code, const char* what)
{
    if (code == FFX_OK)
    {
        return true;
    }
    static U32 reported = 0;
    if (reported++ < 10)
    {
        LL_WARNS("VkDenoise") << what << " failed: FidelityFX error " << (S32)code << LL_ENDL;
    }
    return false;
}

void release_context()
{
    if (gContextMade)
    {
        Context::instance().waitIdle();         // (the denoiser's resources must not be in use: a resize is rare)
        ffxDenoiserContextDestroy(&gContext);
        gContextMade = false;
    }
    Context& ctx = Context::instance();
    ctx.destroyImage(gRadianceB);
    ctx.destroyImage(gVarianceA);
    ctx.destroyImage(gVarianceB);
    ctx.destroyBuffer(gTiles);
    ctx.destroyBuffer(gArgs);
    gRadianceB = Image();
    gVarianceA = Image();
    gVarianceB = Image();
    gTiles = Buffer();
    gArgs = Buffer();
    gWidth = gHeight = 0;
}

// the FidelityFX resource of an image: the state it is in now (the caller leaves every image shader-read-only; the backend moves it where a pass needs it and back)
FfxResource image_resource(const Image& image, bool storage, const wchar_t* name)
{
    VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = image.mFormat;
    info.extent = { image.mExtent.width, image.mExtent.height, 1 };
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | (storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
    const FfxResourceDescription desc = ffxGetImageResourceDescriptionVK(image.mImage, info, FFX_RESOURCE_USAGE_READ_ONLY);
    return ffxGetResourceVK((void*)image.mImage, desc, name, FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
}

FfxResource buffer_resource(const Buffer& buffer, VkBufferUsageFlags usage, FfxResourceStates state, const wchar_t* name)
{
    VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    info.size = buffer.mSize;
    info.usage = usage;
    const FfxResourceDescription desc = ffxGetBufferResourceDescriptionVK(buffer.mBuffer, info, FFX_RESOURCE_USAGE_UAV);
    return ffxGetResourceVK((void*)buffer.mBuffer, desc, name, state);
}

// column major 4x4 products: out = a * b
void multiply(const float* a, const float* b, float* out)
{
    float r[16];
    for (int col = 0; col < 4; ++col)
    {
        for (int row = 0; row < 4; ++row)
        {
            float v = 0.f;
            for (int k = 0; k < 4; ++k)
            {
                v += a[k * 4 + row] * b[col * 4 + k];
            }
            r[col * 4 + row] = v;
        }
    }
    memcpy(out, r, sizeof(r));
}
}

bool init(VkInstance instance, VkPhysicalDevice physical, VkDevice device)
{
    (void)instance;
    if (gReady)
    {
        return true;
    }
    if (!vkGetDeviceProcAddr)
    {
        return false;
    }
    gPhysical = physical;
    gDevice = device;
    gDeviceContext.vkDevice = device;
    gDeviceContext.vkPhysicalDevice = physical;
    gDeviceContext.vkDeviceProcAddr = vkGetDeviceProcAddr;
    const size_t scratch_size = ffxGetScratchMemorySizeVK(physical, FFX_DENOISER_CONTEXT_COUNT);
    gScratch = calloc(1, scratch_size);
    if (!gScratch)
    {
        return false;
    }
    const FfxDevice ffx_device = ffxGetDeviceVK(&gDeviceContext);
    if (!ffx_ok(ffxGetInterfaceVK(&gBackend, ffx_device, gScratch, scratch_size, FFX_DENOISER_CONTEXT_COUNT), "ffxGetInterfaceVK"))
    {
        free(gScratch);
        gScratch = nullptr;
        return false;
    }
    gReady = true;
    LL_INFOS("VkDenoise") << "the AMD FidelityFX denoiser is ready (effect version " << ffxDenoiserGetEffectVersion() << ")" << LL_ENDL;
    return true;
}

bool ready()
{
    return gReady;
}

void shutdown()
{
    if (!gReady)
    {
        return;
    }
    release_context();
    free(gScratch);
    gScratch = nullptr;
    gReady = false;
}

bool prepare(U32 width, U32 height, std::vector<Image*>& own)
{
    if (!gReady || !width || !height)
    {
        return false;
    }
    if (width != gWidth || height != gHeight)
    {
        release_context();
        Context& ctx = Context::instance();
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!ctx.createImage2D(width, height, 1, 1, VK_FORMAT_R16G16B16A16_SFLOAT, usage, gRadianceB)
            || !ctx.createImage2D(width, height, 1, 1, VK_FORMAT_R16_SFLOAT, usage, gVarianceA)
            || !ctx.createImage2D(width, height, 1, 1, VK_FORMAT_R16_SFLOAT, usage, gVarianceB))
        {
            release_context();
            return false;
        }
        // every 8x8 tile of the picture is denoised: the tile list names them by the pixel of their corner (x in the low 16 bits, y in the high ones), the three dispatches read one workgroup for a tile
        const U32 tiles_x = (width + 7) / 8, tiles_y = (height + 7) / 8;
        const U32 tile_count = tiles_x * tiles_y;
        if (!ctx.createBuffer((VkDeviceSize)tile_count * sizeof(U32), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, Context::Memory::Upload, gTiles)
            || !ctx.createBuffer(12 * sizeof(U32), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, Context::Memory::Upload, gArgs)
            || !gTiles.mMapped || !gArgs.mMapped)
        {
            release_context();
            return false;
        }
        U32* tiles = (U32*)gTiles.mMapped;
        for (U32 y = 0; y < tiles_y; ++y)
        {
            for (U32 x = 0; x < tiles_x; ++x)
            {
                tiles[y * tiles_x + x] = ((y * 8) << 16) | (x * 8);
            }
        }
        U32* args = (U32*)gArgs.mMapped;
        memset(args, 0, 12 * sizeof(U32));
        args[3] = tile_count;       // (the three passes dispatch at byte offset 12)
        args[4] = 1;
        args[5] = 1;

        FfxDenoiserContextDescription desc = {};
        desc.flags = FFX_DENOISER_REFLECTIONS;
        desc.windowSize.width = width;
        desc.windowSize.height = height;
        desc.normalsHistoryBufferFormat = FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT;
        desc.backendInterface = gBackend;
        if (!ffx_ok(ffxDenoiserContextCreate(&gContext, &desc), "ffxDenoiserContextCreate"))
        {
            release_context();
            return false;
        }
        gContextMade = true;
        gWidth = width;
        gHeight = height;
        gFirst = true;
        LL_INFOS("VkDenoise") << "the reflection denoiser context was made for " << width << "x" << height << " (" << tile_count << " tiles)" << LL_ENDL;
    }
    own.push_back(&gRadianceB);
    own.push_back(&gVarianceA);
    own.push_back(&gVarianceB);
    return true;
}

bool reflections(VkCommandBuffer cmd, const ReflectionParams& p)
{
    if (!gReady || !gContextMade || p.mWidth != gWidth || p.mHeight != gHeight
        || !p.mRadiance || !p.mOutput || !p.mDepth || !p.mNormals || !p.mRoughness || !p.mMotion)
    {
        return false;
    }
    FfxDenoiserReflectionsDispatchDescription d = {};
    d.commandList = ffxGetCommandListVK(cmd);
    d.depthHierarchy = image_resource(*p.mDepth, false, L"dnsr_depth");
    d.motionVectors = image_resource(*p.mMotion, false, L"dnsr_motion");
    d.normal = image_resource(*p.mNormals, false, L"dnsr_normal");
    d.radianceA = image_resource(*p.mRadiance, true, L"dnsr_radiance_a");
    d.radianceB = image_resource(gRadianceB, true, L"dnsr_radiance_b");
    d.varianceA = image_resource(gVarianceA, true, L"dnsr_variance_a");
    d.varianceB = image_resource(gVarianceB, true, L"dnsr_variance_b");
    d.extractedRoughness = image_resource(*p.mRoughness, false, L"dnsr_roughness");
    d.denoiserTileList = buffer_resource(gTiles, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, FFX_RESOURCE_STATE_UNORDERED_ACCESS, L"dnsr_tile_list");
    d.indirectArgumentsBuffer = buffer_resource(gArgs, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, FFX_RESOURCE_STATE_INDIRECT_ARGUMENT, L"dnsr_indirect_args");
    d.output = image_resource(*p.mOutput, true, L"dnsr_output");
    d.renderSize.width = p.mWidth;
    d.renderSize.height = p.mHeight;
    // the motion is in pixels, previous minus current, in the texture's own v direction; the denoiser adds motion * scale to the texture coordinate
    d.motionVectorScale.x = 1.f / (float)p.mWidth;
    d.motionVectorScale.y = 1.f / (float)p.mHeight;

    // the viewer's OpenGL conventions -> the denoiser's: it takes the projection of a texture coordinate and a depth buffer value as (2u - 1, 1 - 2v, depth); the viewer's are (2u - 1, 2v - 1, 2 depth - 1)
    //   inverse projection: view = invProj_gl * S * (x, y, depth, 1) with S = [1 0 0 0; 0 -1 0 0; 0 0 2 -1; 0 0 0 1]
    //   previous view projection: the denoiser's clip = T * (gl clip) with T = [1 0 0 0; 0 -1 0 0; 0 0 .5 .5; 0 0 0 1]
    const float S[16] = { 1, 0, 0, 0,   0, -1, 0, 0,   0, 0, 2, 0,   0, 0, -1, 1 };     // column major
    const float T[16] = { 1, 0, 0, 0,   0, -1, 0, 0,   0, 0, 0.5f, 0,   0, 0, 0.5f, 1 };
    multiply(p.mInvProjection, S, d.invProjection);
    memcpy(d.invView, p.mInvView, sizeof(d.invView));
    multiply(T, p.mPrevViewProjection, d.prevViewProjection);

    d.normalsUnpackMul = 1.f;
    d.normalsUnpackAdd = 0.f;
    d.isRoughnessPerceptual = true;
    d.roughnessChannel = 0;
    d.temporalStabilityFactor = p.mTemporalStability;
    d.roughnessThreshold = p.mRoughnessThreshold;
    d.frameIndex = p.mFrameIndex;
    d.reset = p.mReset || gFirst;
    gFirst = false;
    return ffx_ok(ffxDenoiserContextDispatchReflections(&gContext, &d), "ffxDenoiserContextDispatchReflections");
}

#else   // no FidelityFX denoiser libraries in this build

bool init(VkInstance, VkPhysicalDevice, VkDevice) { return false; }
bool ready() { return false; }
void shutdown() {}
bool prepare(U32, U32, std::vector<Image*>&) { return false; }
bool reflections(VkCommandBuffer, const ReflectionParams&) { return false; }

#endif

}

#endif // LL_VULKAN
