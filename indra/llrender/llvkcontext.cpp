/**
 * @file llvkcontext.cpp
 * @brief Vulkan device layer for the experimental Vulkan backend (see llvkcontext.h)
 */

#include "linden_common.h"

#include "llvkcontext.h"
#include "llvkupscale.h"
#include "llvkdenoise.h"

#if defined(LL_VULKAN)

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <cstring>
#include <unordered_map>
#include <atomic>
#include <thread>
#include <cstdarg>
#include "llvkplatform.h"
#if LL_WINDOWS
#include <dbghelp.h>
#else
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#endif

namespace LLVk
{

#define LL_VK_GLOBAL_FN(name) PFN_##name name = nullptr;
#define LL_VK_INSTANCE_FN(name) PFN_##name name = nullptr;
#define LL_VK_DEVICE_FN(name) PFN_##name name = nullptr;
#include "llvkfunctions.inl"
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
const char* gDebugProgram = nullptr;
bool gDeviceLost = false;
static std::thread::id sFrameThread;        // the thread that last began a frame (GPU-LIFETIMES.md 5.8)

namespace
{

#if LL_WINDOWS
// LL_VK_AV_LOG=<file>: a vectored exception handler that writes the call stack (names from the PDB next to the exe) of the first access violations to the file, whichever module they
// fault in. The driver's own crashes (nvoglv64.dll) left no trace in the viewer's log; the stack names the Vulkan call that was running.
LONG CALLBACK av_stack_handler(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    static std::atomic<int> count{ 0 };
    if (count.fetch_add(1) >= 6)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    char path[MAX_PATH] = {};
    if (GetEnvironmentVariableA("LL_VK_AV_LOG", path, sizeof(path)) == 0)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    HANDLE file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    auto put = [&](const char* fmt, ...) {
        char line[1024];
        va_list args;
        va_start(args, fmt);
        const int n = _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, args);
        va_end(args);
        DWORD written = 0;
        WriteFile(file, line, n > 0 ? (DWORD)n : 0, &written, nullptr);
    };
    typedef BOOL(WINAPI* SymInitializeT)(HANDLE, PCSTR, BOOL);
    typedef BOOL(WINAPI* SymFromAddrT)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
    typedef BOOL(WINAPI* StackWalk64T)(DWORD, HANDLE, HANDLE, LPSTACKFRAME64, PVOID, PREAD_PROCESS_MEMORY_ROUTINE64, PFUNCTION_TABLE_ACCESS_ROUTINE64, PGET_MODULE_BASE_ROUTINE64, PTRANSLATE_ADDRESS_ROUTINE64);
    typedef DWORD(WINAPI* SymSetOptionsT)(DWORD);
    HMODULE dbg = LoadLibraryA("dbghelp.dll");
    const HANDLE process = GetCurrentProcess();
    put("=== access violation %d: thread %lu, code address %p, accessing %p (%s)\r\n", count.load(), GetCurrentThreadId(), ep->ExceptionRecord->ExceptionAddress,
        ep->ExceptionRecord->NumberParameters > 1 ? (void*)ep->ExceptionRecord->ExceptionInformation[1] : nullptr,
        ep->ExceptionRecord->NumberParameters > 0 ? (ep->ExceptionRecord->ExceptionInformation[0] == 0 ? "read" : (ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "write" : "execute")) : "?");
    if (dbg)
    {
        SymInitializeT sym_init = (SymInitializeT)GetProcAddress(dbg, "SymInitialize");
        SymSetOptionsT sym_opts = (SymSetOptionsT)GetProcAddress(dbg, "SymSetOptions");
        SymFromAddrT sym_from = (SymFromAddrT)GetProcAddress(dbg, "SymFromAddr");
        if (sym_init && sym_opts && sym_from)
        {
            sym_opts(0x2 /*UNDNAME*/ | 0x4 /*DEFERRED_LOADS*/);
            static bool inited = false;
            if (!inited)
            {
                inited = true;
                sym_init(process, nullptr, TRUE);
            }
            CONTEXT ctx = *ep->ContextRecord;
            for (int i = 0; i < 40; ++i)
            {
                const DWORD64 pc = ctx.Rip;
                if (!pc)
                {
                    break;
                }
                char buf[sizeof(SYMBOL_INFO) + 256] = {};
                PSYMBOL_INFO sym = (PSYMBOL_INFO)buf;
                sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                sym->MaxNameLen = 255;
                DWORD64 disp = 0;
                HMODULE m = nullptr;
                char modname[MAX_PATH] = "?";
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pc, &m))
                {
                    GetModuleFileNameA(m, modname, sizeof(modname));
                }
                const char* base = strrchr(modname, '\\');
                if (sym_from(process, pc, &disp, sym))
                {
                    put("  #%02d %s!%s+0x%llx\r\n", i, base ? base + 1 : modname, sym->Name, (unsigned long long)disp);
                }
                else
                {
                    put("  #%02d %s+0x%llx\r\n", i, base ? base + 1 : modname, (unsigned long long)(pc - (DWORD64)m));
                }
                // the OS unwinder (the function tables of every module, symbols or not)
                DWORD64 image_base = 0;
                PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(pc, &image_base, nullptr);
                if (entry)
                {
                    PVOID handler_data = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, pc, entry, &ctx, &handler_data, &establisher, nullptr);
                }
                else
                {
                    ctx.Rip = *(DWORD64*)ctx.Rsp;       // a leaf function: the return address is on top of the stack
                    ctx.Rsp += 8;
                }
            }
        }
    }
    CloseHandle(file);
    return EXCEPTION_CONTINUE_SEARCH;
}

#endif // LL_WINDOWS (the access violation stack handler)

bool vk_ok(VkResult r, const char* what)
{
    if (r != VK_SUCCESS)
    {
        if (r == VK_ERROR_DEVICE_LOST)
        {
            if (!gDeviceLost)
            {
                gDeviceLost = true;
                Context::instance().reportDeviceLost();
            }
        }
        LL_WARNS("Vulkan") << what << " failed: VkResult " << (S32)r << LL_ENDL;
        return false;
    }
    return true;
}

bool has_extension(const std::vector<VkExtensionProperties>& list, const char* name)
{
    for (const auto& e : list)
    {
        if (strcmp(e.extensionName, name) == 0)
        {
            return true;
        }
    }
    return false;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
{
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
    {
        LL_WARNS("VulkanValidation") << "ERROR [draw: " << (gDebugProgram ? gDebugProgram : "-") << "]: " << data->pMessage << LL_ENDL;
    }
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
    {
        LL_WARNS("VulkanValidation") << "[draw: " << (gDebugProgram ? gDebugProgram : "-") << "] " << data->pMessage << LL_ENDL;
    }
    return VK_FALSE;
}

// LL_VK_ADDR_LOG (on; =0 turns it off): VK_EXT_device_address_binding_report. Every bind / unbind of GPU memory to a buffer, image or acceleration structure goes into a ring, so the
// address of a device fault can be traced to the resource that held it and the frame it was given up in (the report lists them)
struct AddrEvent
{
    U64 mFrame;
    U64 mMicros;
    U64 mBase;
    U64 mSize;
    U64 mHandle;
    U32 mObjectType;
    U8 mBind;           // 1 bind, 0 unbind
    U8 mInternal;
};
// <FS> what each image is (written when it is made, read only by the device lost report): a ring of the last 16384 images
struct ImageTag
{
    U64 mHandle;
    U64 mFrame;
    char mText[96];
};
std::mutex sImgTagMutex;
std::vector<ImageTag> sImgTags;
U64 sImgTagCount = 0;
constexpr size_t IMG_TAG_RING = 16384;

// "[what it was, made in frame N]" for a VkImage handle, or "" when the ring no longer has it
std::string image_tag_text(U64 handle, U64 now_frame)
{
    std::lock_guard<std::mutex> lock(sImgTagMutex);
    const U64 total = sImgTagCount;
    const U64 first = total > IMG_TAG_RING ? total - IMG_TAG_RING : 0;
    for (U64 i = total; i > first; --i)
    {
        const ImageTag& t = sImgTags[(i - 1) % IMG_TAG_RING];
        if (t.mHandle == handle)
        {
            return std::string(" [") + t.mText + ", made " + std::to_string(now_frame - t.mFrame) + " frames before the loss]";
        }
    }
    return std::string();
}

std::mutex sAddrMutex;
std::vector<AddrEvent> sAddrRing;
U64 sAddrCount = 0;
const size_t ADDR_RING = 1u << 20;
// on unless LL_VK_ADDR_LOG=0: the ring costs a few microseconds per memory bind and is what names the owner of a stale address after a device lost
static bool addr_log_wanted()
{
    char v[8] = {};
    const DWORD n = GetEnvironmentVariableA("LL_VK_ADDR_LOG", v, sizeof(v));
    return !(n > 0 && v[0] == '0');
}


U64 addr_micros()
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (U64)(c.QuadPart * 1000000ll / f.QuadPart);
}

VKAPI_ATTR VkBool32 VKAPI_CALL address_binding_callback(VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT,
                                                        const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
{
    for (const VkBaseInStructure* p = reinterpret_cast<const VkBaseInStructure*>(data->pNext); p; p = p->pNext)
    {
        if (p->sType != VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT)
        {
            continue;
        }
        const VkDeviceAddressBindingCallbackDataEXT* d = reinterpret_cast<const VkDeviceAddressBindingCallbackDataEXT*>(p);
        AddrEvent e = {};
        e.mFrame = Context::instance().getFrameNumber();
        e.mMicros = addr_micros();
        e.mBase = (U64)d->baseAddress;
        e.mSize = (U64)d->size;
        e.mBind = d->bindingType == VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT ? 1 : 0;
        e.mInternal = (d->flags & VK_DEVICE_ADDRESS_BINDING_INTERNAL_OBJECT_BIT_EXT) ? 1 : 0;
        if (data->objectCount > 0 && data->pObjects)
        {
            e.mObjectType = (U32)data->pObjects[0].objectType;
            e.mHandle = (U64)data->pObjects[0].objectHandle;
        }
        std::lock_guard<std::mutex> lock(sAddrMutex);
        if (sAddrRing.empty())
        {
            sAddrRing.resize(ADDR_RING);
        }
        sAddrRing[sAddrCount % ADDR_RING] = e;
        ++sAddrCount;
    }
    return VK_FALSE;
}

const char* addr_object_name(U32 t)
{
    switch (t)
    {
    case VK_OBJECT_TYPE_BUFFER: return "buffer";
    case VK_OBJECT_TYPE_IMAGE: return "image";
    case VK_OBJECT_TYPE_DEVICE_MEMORY: return "memory";
    case VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR: return "accel";
    default: return "other";
    }
}

// the feature structures the backend queries and enables, chained together
struct FeatureChain
{
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    VkPhysicalDeviceVulkan11Features f11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT };
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vids = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT };

    // C6: ray tracing. Chained after the others only for extensions the device has (a struct of an extension the device lacks must not be in the chain)
    VkPhysicalDeviceAccelerationStructureFeaturesKHR accel = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
    VkPhysicalDeviceRayQueryFeaturesKHR rayQuery = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtPipeline = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR };
    VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR posFetch = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_POSITION_FETCH_FEATURES_KHR };
    VkPhysicalDeviceRayTracingMaintenance1FeaturesKHR maint1 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_MAINTENANCE_1_FEATURES_KHR };
    VkPhysicalDeviceRayTracingInvocationReorderFeaturesNV reorder = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_NV };
    VkPhysicalDeviceOpacityMicromapFeaturesEXT omm = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT };
    VkPhysicalDeviceClusterAccelerationStructureFeaturesNV cluster = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CLUSTER_ACCELERATION_STRUCTURE_FEATURES_NV };

    FeatureChain()
    {
        f2.pNext = &f11;
        f11.pNext = &f12;
        f12.pNext = &f13;
        f13.pNext = &eds3;
        eds3.pNext = &vids;
    }
    FeatureChain(const FeatureChain&) = delete;
    FeatureChain& operator=(const FeatureChain&) = delete;

    // appends the ray tracing structs of the extensions in 'rt' (the ones for the vendor / later extensions only when 'all' is set: they are queried, not enabled)
    void addRayTracing(const Capabilities::RayTracing& rt, bool all)
    {
        void** tail = &vids.pNext;
        auto link = [&](bool have, void* s, void** s_next) { if (have) { *tail = s; tail = s_next; } };
        link(rt.mExtAccel, &accel, &accel.pNext);
        link(rt.mExtRayQuery, &rayQuery, &rayQuery.pNext);
        link(rt.mExtPipeline, &rtPipeline, &rtPipeline.pNext);
        link(rt.mExtPositionFetch, &posFetch, &posFetch.pNext);
        link(rt.mExtMaintenance1, &maint1, &maint1.pNext);
        link(all && rt.mExtReorder, &reorder, &reorder.pNext);
        link(all && rt.mExtOpacityMicromap, &omm, &omm.pNext);
        link(all && rt.mExtCluster, &cluster, &cluster.pNext);
    }
};

// ray tracing is switched on at device creation only on request (LL_VK_RT=1): until the renderer uses it, nothing about the device should differ from a build without it
bool rayTracingRequested()
{
    // LL_VK_RT_SHADOWS (ray query sun shadows) needs the device features as well
    static const bool on = []() {
        char b[8] = {}, c[8] = {};
        return (GetEnvironmentVariableA("LL_VK_RT", b, sizeof(b)) > 0 && b[0] != '0') || (GetEnvironmentVariableA("LL_VK_RT_SHADOWS", c, sizeof(c)) > 0 && c[0] != '0');
    }();
    return on;
}

// the largest sample count in a VkSampleCountFlags
U32 max_sample_count(VkSampleCountFlags counts)
{
    U32 best = 1;
    for (U32 c : { 2u, 4u, 8u, 16u, 32u, 64u }) if (counts & c) best = c;
    return best;
}

const char* const REQUIRED_DEVICE_EXTENSIONS[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
    VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME,
    VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
};

// returns a description of the first missing requirement, or nullptr if the device qualifies
const char* missing_feature(const FeatureChain& c)
{
    if (!c.f13.dynamicRendering) return "dynamicRendering";
    if (!c.f13.synchronization2) return "synchronization2";
    if (!c.f12.hostQueryReset) return "hostQueryReset";
    if (!c.eds3.extendedDynamicState3PolygonMode) return "extendedDynamicState3PolygonMode";
    if (!c.eds3.extendedDynamicState3ColorBlendEnable) return "extendedDynamicState3ColorBlendEnable";
    if (!c.eds3.extendedDynamicState3ColorBlendEquation) return "extendedDynamicState3ColorBlendEquation";
    if (!c.eds3.extendedDynamicState3ColorWriteMask) return "extendedDynamicState3ColorWriteMask";
    if (!c.vids.vertexInputDynamicState) return "vertexInputDynamicState";
    if (!c.f2.features.fillModeNonSolid) return "fillModeNonSolid";
    if (!c.f2.features.independentBlend) return "independentBlend";
    if (!c.f2.features.imageCubeArray) return "imageCubeArray";
    // <FS> the scene table (every draw of the world): bindless sampled images, indirect draws, buffer addresses
    if (!c.f12.runtimeDescriptorArray) return "descriptor indexing (runtimeDescriptorArray)";
    if (!c.f12.descriptorBindingPartiallyBound) return "descriptor indexing (descriptorBindingPartiallyBound)";
    if (!c.f12.descriptorBindingSampledImageUpdateAfterBind) return "descriptor indexing (descriptorBindingSampledImageUpdateAfterBind)";
    if (!c.f12.shaderSampledImageArrayNonUniformIndexing) return "descriptor indexing (shaderSampledImageArrayNonUniformIndexing)";
    if (!c.f12.bufferDeviceAddress) return "bufferDeviceAddress";
    if (!c.f2.features.multiDrawIndirect) return "multiDrawIndirect";
    if (!c.f2.features.drawIndirectFirstInstance) return "drawIndirectFirstInstance";
    return nullptr;
}

} // anonymous namespace

//-----------------------------------------------------------------------------------------------------
// Context
//-----------------------------------------------------------------------------------------------------

VkImageAspectFlags aspectForFormat(VkFormat format)
{
    switch (format)
    {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    default:
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

size_t Context::pendingDestroys() const
{
    size_t pending_next = 0;
    {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mDeferredNextMutex));
        pending_next = mDeferredNext.size();
    }
    size_t n = 0;
    for (const Frame& f : mFrames) n += f.mDeferred.size() + f.mDeferredLate.size();
    return n + pending_next;
}

U64 Context::deviceLocalBytes() const
{
    if (!mAllocator)
    {
        return 0;
    }
    const VkPhysicalDeviceMemoryProperties* props = nullptr;
    vmaGetMemoryProperties(mAllocator, &props);
    U64 total = 0;
    for (U32 i = 0; props && i < props->memoryHeapCount; ++i)
    {
        if (props->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
        {
            total += props->memoryHeaps[i].size;
        }
    }
    return total;
}

bool Context::videoMemory(U64& usage_bytes, U64& budget_bytes)
{
    if (!mAllocator)
    {
        return false;
    }
    const VkPhysicalDeviceMemoryProperties* props = nullptr;
    vmaGetMemoryProperties(mAllocator, &props);
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
    vmaGetHeapBudgets(mAllocator, budgets);
    int best = -1;
    for (U32 i = 0; props && i < props->memoryHeapCount; ++i)
    {
        if ((props->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && (best < 0 || props->memoryHeaps[i].size > props->memoryHeaps[best].size))
        {
            best = (int)i;
        }
    }
    if (best < 0 || budgets[best].budget == 0)
    {
        return false;
    }
    usage_bytes = budgets[best].usage;
    budget_bytes = budgets[best].budget;
    return true;
}
// <FS> diagnostic (ImgMem): the largest host (not device local) heap: what the OS counts for this process in it, and the bytes we allocated in it
bool Context::hostMemory(U64& usage_bytes, U64& allocated_by_us)
{
    if (!mAllocator)
    {
        return false;
    }
    const VkPhysicalDeviceMemoryProperties* props = nullptr;
    vmaGetMemoryProperties(mAllocator, &props);
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
    vmaGetHeapBudgets(mAllocator, budgets);
    int best = -1;
    for (U32 i = 0; props && i < props->memoryHeapCount; ++i)
    {
        if (!(props->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && (best < 0 || props->memoryHeaps[i].size > props->memoryHeaps[best].size))
        {
            best = (int)i;
        }
    }
    if (best < 0)
    {
        return false;
    }
    usage_bytes = budgets[best].usage;
    allocated_by_us = budgets[best].statistics.allocationBytes;
    return true;
}


std::string Context::memoryBudgetText()
{
    if (!mAllocator)
    {
        return "no allocator";
    }
    const VkPhysicalDeviceMemoryProperties* props = nullptr;
    vmaGetMemoryProperties(mAllocator, &props);
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
    vmaGetHeapBudgets(mAllocator, budgets);
    std::string text;
    for (U32 i = 0; props && i < props->memoryHeapCount; ++i)
    {
        text += "heap " + std::to_string(i) + ((props->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " (video) " : " (host) ")
              + std::to_string(budgets[i].usage / (1024 * 1024)) + "/" + std::to_string(budgets[i].budget / (1024 * 1024)) + " MB, allocated by us "
              + std::to_string(budgets[i].statistics.allocationBytes / (1024 * 1024)) + " MB in " + std::to_string(budgets[i].statistics.allocationCount) + " allocations; ";
    }
    return text;
}

void Context::setObjectName(VkObjectType type, U64 handle, const std::string& name)
{
    if (mMessenger == VK_NULL_HANDLE || mDevice == VK_NULL_HANDLE)
    {
        return;
    }
    static PFN_vkSetDebugUtilsObjectNameEXT set_name = nullptr;
    if (!set_name)
    {
        set_name = (PFN_vkSetDebugUtilsObjectNameEXT)vkGetInstanceProcAddr(mInstance, "vkSetDebugUtilsObjectNameEXT");
    }
    if (set_name)
    {
        VkDebugUtilsObjectNameInfoEXT info = { VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT };
        info.objectType = type;
        info.objectHandle = handle;
        info.pObjectName = name.c_str();
        set_name(mDevice, &info);
    }
}

Context& Context::instance()
{
    static Context* sContext = new Context;         // never destroyed (see sGL in llvkworker.cpp): late releases at process exit still reach it
    return *sContext;
}

bool Context::loadLoader()
{
#if LL_WINDOWS
    const char* loader_name = "vulkan-1.dll";
#else
    const char* loader_name = "libvulkan.so.1";
#endif
    mLoader = LoadLibraryA(loader_name);
    if (!mLoader)
    {
        LL_WARNS("Vulkan") << loader_name << " not found" << LL_ENDL;
        return false;
    }
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)GetProcAddress(mLoader, "vkGetInstanceProcAddr");
    if (!vkGetInstanceProcAddr)
    {
        return false;
    }
#define LL_VK_GLOBAL_FN(name) name = (PFN_##name)vkGetInstanceProcAddr(nullptr, #name);
#define LL_VK_INSTANCE_FN(name)
#define LL_VK_DEVICE_FN(name)
#include "llvkfunctions.inl"
    return vkCreateInstance && vkEnumerateInstanceExtensionProperties;
}

bool Context::createInstance()
{
    U32 version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion)
    {
        vkEnumerateInstanceVersion(&version);
    }
    LL_INFOS("Vulkan") << "Vulkan loader version " << VK_API_VERSION_MAJOR(version) << "." << VK_API_VERSION_MINOR(version) << "."
                       << VK_API_VERSION_PATCH(version) << LL_ENDL;
    if (version < VK_API_VERSION_1_3)
    {
        LL_WARNS("Vulkan") << "Vulkan 1.3 is required" << LL_ENDL;
        return false;
    }

    U32 count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());

#if LL_WINDOWS
    std::vector<const char*> wanted = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
#else
    // the surface extensions the window system needs (SDL knows which: X11 or Wayland)
    std::vector<const char*> wanted = { VK_KHR_SURFACE_EXTENSION_NAME };
    {
        unsigned sdl_count = 0;
        if (mWindow && SDL_Vulkan_GetInstanceExtensions((SDL_Window*)mWindow, &sdl_count, nullptr) && sdl_count)
        {
            std::vector<const char*> sdl_names(sdl_count);
            SDL_Vulkan_GetInstanceExtensions((SDL_Window*)mWindow, &sdl_count, sdl_names.data());
            for (const char* n : sdl_names)
            {
                if (strcmp(n, VK_KHR_SURFACE_EXTENSION_NAME) != 0)
                {
                    wanted.push_back(n);
                }
            }
        }
        else
        {
            LL_WARNS("Vulkan") << "SDL did not name the Vulkan surface extensions: " << SDL_GetError() << LL_ENDL;
        }
    }
#endif
    if (LLVkUpscale::requested())
    {
        LLVkUpscale::addInstanceExtensions(wanted);
    }
    for (const char* name : wanted)
    {
        if (!has_extension(available, name))
        {
            LL_WARNS("Vulkan") << "missing instance extension " << name << LL_ENDL;
            return false;
        }
    }
    const bool want_validation = GetEnvironmentVariableA("LL_VK_VALIDATION", nullptr, 0) > 0;
    const bool want_addr_log = addr_log_wanted();
    // LL_VK_LABELS=1: every GPU stamp also inserts a debug label with its name, so a capture in Nsight Graphics (GPU Trace) shows where the passes begin
    const bool want_labels = GetEnvironmentVariableA("LL_VK_LABELS", nullptr, 0) > 0;
    const bool debug_utils = (want_validation || want_addr_log || want_labels) && has_extension(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (debug_utils)
    {
        wanted.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    // validation only when asked for (the layer must be findable through VK_LAYER_PATH)
    std::vector<const char*> layers;
    if (want_validation)
    {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        LL_INFOS("Vulkan") << "validation layer requested" << LL_ENDL;
    }

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "Firestorm";
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = (U32)wanted.size();
    ci.ppEnabledExtensionNames = wanted.data();
    ci.enabledLayerCount = (U32)layers.size();
    ci.ppEnabledLayerNames = layers.data();
    if (!vk_ok(vkCreateInstance(&ci, nullptr, &mInstance), "vkCreateInstance"))
    {
        return false;
    }

#define LL_VK_GLOBAL_FN(name)
#define LL_VK_INSTANCE_FN(name) name = (PFN_##name)vkGetInstanceProcAddr(mInstance, #name);
#define LL_VK_DEVICE_FN(name)
#include "llvkfunctions.inl"

    if (debug_utils)
    {
        auto create_messenger = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(mInstance, "vkCreateDebugUtilsMessengerEXT");
        if (create_messenger && want_addr_log)
        {
            VkDebugUtilsMessengerCreateInfoEXT ai = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
            ai.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            ai.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT;
            ai.pfnUserCallback = address_binding_callback;
            VkDebugUtilsMessengerEXT m = VK_NULL_HANDLE;
            create_messenger(mInstance, &ai, nullptr, &m);
            LL_INFOS("Vulkan") << "device address binding log requested (LL_VK_ADDR_LOG)" << LL_ENDL;
        }
        if (create_messenger && want_validation)
        {
            VkDebugUtilsMessengerCreateInfoEXT mi = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
            mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            mi.pfnUserCallback = debug_callback;
            create_messenger(mInstance, &mi, nullptr, &mMessenger);
        }
    }

#if LL_WINDOWS
    VkWin32SurfaceCreateInfoKHR sci = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    sci.hinstance = GetModuleHandleW(nullptr);
    sci.hwnd = mWindow;
    return vk_ok(vkCreateWin32SurfaceKHR(mInstance, &sci, nullptr, &mSurface), "vkCreateWin32SurfaceKHR");
#else
    if (!SDL_Vulkan_CreateSurface((SDL_Window*)mWindow, mInstance, &mSurface))
    {
        LL_WARNS("Vulkan") << "SDL_Vulkan_CreateSurface failed: " << SDL_GetError() << LL_ENDL;
        return false;
    }
    return true;
#endif
}

bool Context::pickDevice()
{
    U32 count = 0;
    vkEnumeratePhysicalDevices(mInstance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(mInstance, &count, devices.data());

    S32 best_score = -1;
    std::string reasons;        // <FS> every device that was turned down and why, for the message the user gets when none is left
    auto rejected = [&](const char* device, const std::string& why)
    {
        LL_INFOS("Vulkan") << "  rejected: " << why << LL_ENDL;
        reasons += std::string("  ") + device + ": " + why + "\n";
    };
    if (devices.empty())
    {
        reasons += "  Vulkan reports no graphics device at all (the graphics driver may be missing or too old).\n";
    }
    for (VkPhysicalDevice dev : devices)
    {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(dev, &props);
        LL_INFOS("Vulkan") << "device: " << props.deviceName << " (api " << VK_API_VERSION_MAJOR(props.apiVersion) << "."
                           << VK_API_VERSION_MINOR(props.apiVersion) << "." << VK_API_VERSION_PATCH(props.apiVersion) << ")" << LL_ENDL;
        if (getenv("LL_VK_TEST_NO_DEVICE"))
        {
            rejected(props.deviceName, "(test) LL_VK_TEST_NO_DEVICE is set");        // (tries the start-up message without a device that fails)
            continue;
        }
        if (props.apiVersion < VK_API_VERSION_1_3)
        {
            rejected(props.deviceName, "its driver supports Vulkan " + std::to_string(VK_API_VERSION_MAJOR(props.apiVersion)) + "." + std::to_string(VK_API_VERSION_MINOR(props.apiVersion))
                + ", Rekindled needs Vulkan 1.3");
            continue;
        }

        U32 ext_count = 0;
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &ext_count, nullptr);
        std::vector<VkExtensionProperties> exts(ext_count);
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &ext_count, exts.data());
        const char* missing_ext = nullptr;
        for (const char* name : REQUIRED_DEVICE_EXTENSIONS)
        {
            if (!has_extension(exts, name))
            {
                missing_ext = name;
                break;
            }
        }
        if (missing_ext)
        {
            rejected(props.deviceName, std::string("the Vulkan extension ") + missing_ext + " is missing");
            continue;
        }

        FeatureChain chain;
        Capabilities::RayTracing rt;
        rt.mExtAccel = has_extension(exts, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        rt.mExtDeferredHost = has_extension(exts, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        rt.mExtRayQuery = has_extension(exts, VK_KHR_RAY_QUERY_EXTENSION_NAME);
        rt.mExtPipeline = has_extension(exts, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
        rt.mExtPositionFetch = has_extension(exts, VK_KHR_RAY_TRACING_POSITION_FETCH_EXTENSION_NAME);
        rt.mExtMaintenance1 = has_extension(exts, VK_KHR_RAY_TRACING_MAINTENANCE_1_EXTENSION_NAME);
        rt.mExtReorder = has_extension(exts, VK_NV_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME);
        rt.mExtOpacityMicromap = has_extension(exts, VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME);
        rt.mExtCluster = has_extension(exts, VK_NV_CLUSTER_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        chain.addRayTracing(rt, true);
        vkGetPhysicalDeviceFeatures2(dev, &chain.f2);
        if (const char* missing = missing_feature(chain))
        {
            rejected(props.deviceName, std::string("the Vulkan feature ") + missing + " is missing");
            continue;
        }

        U32 qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> families(qcount);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qcount, families.data());
        for (U32 i = 0; i < qcount; ++i)
        {
            const VkQueueFlags f = families[i].queueFlags;
            LL_INFOS("Vulkan") << "  queue family " << i << ": " << families[i].queueCount << " queue(s)" << ((f & VK_QUEUE_GRAPHICS_BIT) ? " graphics" : "") << ((f & VK_QUEUE_COMPUTE_BIT) ? " compute" : "")
                               << ((f & VK_QUEUE_TRANSFER_BIT) ? " transfer" : "") << ((f & VK_QUEUE_SPARSE_BINDING_BIT) ? " sparse" : "") << ((f & VK_QUEUE_VIDEO_DECODE_BIT_KHR) ? " video-decode" : "")
                               << ((f & VK_QUEUE_OPTICAL_FLOW_BIT_NV) ? " optical-flow" : "") << LL_ENDL;
        }
        S32 family = -1;
        for (U32 i = 0; i < qcount; ++i)
        {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, mSurface, &present);
            const VkQueueFlags need = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
            if ((families[i].queueFlags & need) == need && present)
            {
                family = (S32)i;
                break;
            }
        }
        if (family < 0)
        {
            rejected(props.deviceName, "it has no queue that can draw, compute and present to the window");
            continue;
        }

        S32 score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 100 : 10;
        if (score > best_score)
        {
            best_score = score;
            mPhysical = dev;
            mQueueFamily = (U32)family;

            mCaps = Capabilities();
            strncpy(mCaps.mDeviceName, props.deviceName, sizeof(mCaps.mDeviceName) - 1);
            mCaps.mApiVersion = props.apiVersion;
            mCaps.mDriverVersion = props.driverVersion;
            mCaps.mVendorId = props.vendorID;
            mCaps.mDiscrete = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
            mCaps.mMemoryBudget = has_extension(exts, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
            mCaps.mCheckpoints = has_extension(exts, VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
            mCaps.mDeviceFault = has_extension(exts, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
            mCaps.mWideLines = chain.f2.features.wideLines != VK_FALSE;
            mCaps.mDepthClamp = chain.f2.features.depthClamp != VK_FALSE && chain.eds3.extendedDynamicState3DepthClampEnable != VK_FALSE;
            mCaps.mAnisotropy = chain.f2.features.samplerAnisotropy != VK_FALSE;
            mCaps.mBC = chain.f2.features.textureCompressionBC != VK_FALSE;
            mCaps.mGeometryShader = chain.f2.features.geometryShader != VK_FALSE;
            mCaps.mMultiDrawIndirect = chain.f2.features.multiDrawIndirect != VK_FALSE;
            mCaps.mBufferDeviceAddress = chain.f12.bufferDeviceAddress != VK_FALSE;
            mCaps.mDrawIndirectFirstInstance = chain.f2.features.drawIndirectFirstInstance != VK_FALSE;
            mCaps.mDrawIndirectCount = chain.f12.drawIndirectCount != VK_FALSE;
            mCaps.mMaxAnisotropy = props.limits.maxSamplerAnisotropy;
            mCaps.mTimestampPeriodNs = props.limits.timestampPeriod;
            mCaps.mTimestampValidBits = families[(U32)family].timestampValidBits;
            mCaps.mUniformAlignment = props.limits.minUniformBufferOffsetAlignment;
            mCaps.mMaxImageDim2D = props.limits.maxImageDimension2D;
            mCaps.mMaxImageDimCube = props.limits.maxImageDimensionCube;
            mCaps.mMaxImageDim3D = props.limits.maxImageDimension3D;
            mCaps.mMaxArrayLayers = props.limits.maxImageArrayLayers;
            mCaps.mMaxColorAttachments = props.limits.maxColorAttachments;
            mCaps.mMaxVertexAttribs = props.limits.maxVertexInputAttributes;
            mCaps.mMaxUniformRange = props.limits.maxUniformBufferRange;
            mCaps.mMaxSampledImagesPerStage = props.limits.maxPerStageDescriptorSampledImages;
            mCaps.mLineWidthRange[0] = props.limits.lineWidthRange[0];
            mCaps.mLineWidthRange[1] = props.limits.lineWidthRange[1];
            mCaps.mMaxViewport[0] = props.limits.maxViewportDimensions[0];
            mCaps.mMaxViewport[1] = props.limits.maxViewportDimensions[1];
            mCaps.mTimelineSemaphore = chain.f12.timelineSemaphore != VK_FALSE;
            mCaps.mGraphicsQueues = families[(U32)family].queueCount;
            for (U32 i = 0; i < qcount; ++i)
            {
                const VkQueueFlags f = families[i].queueFlags;
                if (!(f & VK_QUEUE_GRAPHICS_BIT) && (f & VK_QUEUE_COMPUTE_BIT) && mCaps.mComputeFamily < 0)
                {
                    mCaps.mComputeFamily = (S32)i;
                    mCaps.mComputeQueues = families[i].queueCount;
                }
                // a transfer-only family: not a graphics / compute one, not video decode or optical flow
                if ((f & VK_QUEUE_TRANSFER_BIT) && !(f & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_VIDEO_DECODE_BIT_KHR | VK_QUEUE_OPTICAL_FLOW_BIT_NV)) && mCaps.mTransferFamily < 0)
                {
                    mCaps.mTransferFamily = (S32)i;
                    mCaps.mTransferQueues = families[i].queueCount;
                }
            }
            LL_INFOS("Vulkan") << "queues: renderer family " << family << " has " << mCaps.mGraphicsQueues << "; async compute family " << mCaps.mComputeFamily << " (" << mCaps.mComputeQueues
                << " queues); transfer-only family " << mCaps.mTransferFamily << " (" << mCaps.mTransferQueues << "); timeline semaphores " << mCaps.mTimelineSemaphore << LL_ENDL;
            mCaps.mMaxSampleCount = max_sample_count(props.limits.framebufferColorSampleCounts);
            // C6: the limits a GL driver reports that have no one-to-one Vulkan limit
            mCaps.mMaxColorTextureSamples = max_sample_count(props.limits.sampledImageColorSampleCounts);
            mCaps.mMaxDepthTextureSamples = max_sample_count(props.limits.sampledImageDepthSampleCounts);
            mCaps.mMaxIntegerSamples = max_sample_count(props.limits.sampledImageIntegerSampleCounts);
            mCaps.mMaxSampleMaskWords = std::max(1u, (mCaps.mMaxSampleCount + 31) / 32);      // one 32 bit word per 32 samples
            // GL counts the vectors a vertex shader hands on without the position (a driver with 128 output components reports 31)
            mCaps.mMaxVaryingVectors = std::min(props.limits.maxVertexOutputComponents, props.limits.maxFragmentInputComponents) / 4 - 1;
            // GL_MAX_ELEMENTS_VERTICES / INDICES are a recommendation for glDrawRangeElements (NVIDIA says 1048575), not a limit; Vulkan only has the largest index value.
            // The viewer batches grass quads up to this size, so it must not read 0.
            mCaps.mRecommendedMaxVertices = mCaps.mRecommendedMaxIndices = (U32)std::min<U64>(1048575u, props.limits.maxDrawIndexedIndexValue);

            VkPhysicalDevicePushDescriptorPropertiesKHR push = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR };
            VkPhysicalDeviceVulkan12Properties p12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES };
            VkPhysicalDeviceAccelerationStructurePropertiesKHR accel_props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR };
            VkPhysicalDeviceRayTracingPipelinePropertiesKHR rt_pipe_props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR };
            VkPhysicalDeviceProperties2 props2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
            props2.pNext = &push;
            push.pNext = &p12;
            void** props_tail = &p12.pNext;
            if (rt.mExtAccel)
            {
                *props_tail = &accel_props;
                props_tail = &accel_props.pNext;
            }
            if (rt.mExtPipeline)
            {
                *props_tail = &rt_pipe_props;
                props_tail = &rt_pipe_props.pNext;
            }
            vkGetPhysicalDeviceProperties2(dev, &props2);
            mCaps.mMaxPushDescriptors = push.maxPushDescriptors;

            // C6: ray tracing as the device reports it (the feature structs were filled by the query above)
            rt.mAccelStruct = rt.mExtAccel && rt.mExtDeferredHost && chain.accel.accelerationStructure != VK_FALSE;
            rt.mRayQuery = rt.mExtRayQuery && chain.rayQuery.rayQuery != VK_FALSE;
            rt.mPipeline = rt.mExtPipeline && chain.rtPipeline.rayTracingPipeline != VK_FALSE;
            rt.mPositionFetch = rt.mExtPositionFetch && chain.posFetch.rayTracingPositionFetch != VK_FALSE;
            rt.mMaintenance1 = rt.mExtMaintenance1 && chain.maint1.rayTracingMaintenance1 != VK_FALSE;
            rt.mReorder = rt.mExtReorder && chain.reorder.rayTracingInvocationReorder != VK_FALSE;
            rt.mOpacityMicromap = rt.mExtOpacityMicromap && chain.omm.micromap != VK_FALSE;
            rt.mClusterAccel = rt.mExtCluster && chain.cluster.clusterAccelerationStructure != VK_FALSE;
            rt.mUsable = rt.mAccelStruct && rt.mRayQuery && mCaps.mBufferDeviceAddress;
            if (rt.mExtAccel)
            {
                rt.mMaxGeometryCount = accel_props.maxGeometryCount;
                rt.mMaxInstanceCount = accel_props.maxInstanceCount;
                rt.mMaxPrimitiveCount = accel_props.maxPrimitiveCount;
                rt.mScratchAlignment = accel_props.minAccelerationStructureScratchOffsetAlignment;
            }
            if (rt.mExtPipeline)
            {
                rt.mMaxRecursionDepth = rt_pipe_props.maxRayRecursionDepth;
            }
            mCaps.mRt = rt;
            LL_INFOS("Vulkan") << "ray tracing: acceleration structure " << rt.mAccelStruct << ", ray query " << rt.mRayQuery << ", pipeline " << rt.mPipeline
                << ", position fetch " << rt.mPositionFetch << ", maintenance1 " << rt.mMaintenance1 << ", invocation reorder " << rt.mReorder << ", opacity micromap "
                << rt.mOpacityMicromap << ", cluster acceleration structure " << rt.mClusterAccel << " -> usable " << rt.mUsable << "; limits: geometries " << rt.mMaxGeometryCount
                << ", instances " << rt.mMaxInstanceCount << ", primitives " << rt.mMaxPrimitiveCount << ", scratch alignment " << rt.mScratchAlignment
                << ", recursion " << rt.mMaxRecursionDepth << (rayTracingRequested() ? "" : " (not enabled on the device: LL_VK_RT=1 does that)") << LL_ENDL;

            // <FS> bindless sampled images for the render packets: the features, and a table size that leaves room for the push set and the uniform set
            if (chain.f12.runtimeDescriptorArray && chain.f12.descriptorBindingPartiallyBound && chain.f12.descriptorBindingSampledImageUpdateAfterBind
                && chain.f12.shaderSampledImageArrayNonUniformIndexing && mCaps.mBufferDeviceAddress)
            {
                auto room = [](U32 limit, U32 reserve) -> U32 { return limit > reserve ? limit - reserve : 0; };
                // the limits count every table of the set together: the 2D table gets what is left after the three small ones
                const U32 others = 3 * mCaps.mBindlessSmall;
                auto left = [&](U32 limit, U32 reserve) -> U32 { const U32 r = room(limit, reserve); return r > others ? r - others : 0; };
                const U32 n = std::min<U32>({ 65536u,
                    left(p12.maxPerStageDescriptorUpdateAfterBindSamplers, 96), left(p12.maxPerStageDescriptorUpdateAfterBindSampledImages, 96),
                    left(p12.maxPerStageUpdateAfterBindResources, 160), left(p12.maxDescriptorSetUpdateAfterBindSamplers, 96),
                    left(p12.maxDescriptorSetUpdateAfterBindSampledImages, 96) });
                mCaps.mBindlessSlots = n >= 1024 ? n : 0;
                mCaps.mBindlessUpdateUnused = chain.f12.descriptorBindingUpdateUnusedWhilePending != VK_FALSE;
            }
            LL_INFOS("Vulkan") << "descriptor indexing: runtime arrays " << (chain.f12.runtimeDescriptorArray ? 1 : 0) << ", partially bound "
                << (chain.f12.descriptorBindingPartiallyBound ? 1 : 0) << ", sampled image update after bind " << (chain.f12.descriptorBindingSampledImageUpdateAfterBind ? 1 : 0)
                << ", non-uniform sampled image indexing " << (chain.f12.shaderSampledImageArrayNonUniformIndexing ? 1 : 0) << ", update-after-bind limits per stage samplers "
                << p12.maxPerStageDescriptorUpdateAfterBindSamplers << " sampled images " << p12.maxPerStageDescriptorUpdateAfterBindSampledImages << " resources "
                << p12.maxPerStageUpdateAfterBindResources << " -> bindless slots " << mCaps.mBindlessSlots << LL_ENDL;
        }
    }

    if (mPhysical != VK_NULL_HANDLE && mCaps.mBindlessSlots == 0)
    {
        // <FS> the features are there, but the limits leave no room for the table of textures (at least 1024 sampled images per shader stage after the other tables)
        reasons += std::string("  ") + mCaps.mDeviceName + ": its limit on sampled images per shader stage is too small for bindless textures\n";
        mPhysical = VK_NULL_HANDLE;
    }
    if (mPhysical == VK_NULL_HANDLE)
    {
        LL_WARNS("Vulkan") << "no suitable Vulkan device found" << LL_ENDL;
        mFailureReason = "What was found:\n" + reasons;
        return false;
    }
    LL_INFOS("Vulkan") << "using " << mCaps.mDeviceName << ", queue family " << mQueueFamily << ", timestamp period "
                       << mCaps.mTimestampPeriodNs << " ns, push descriptors " << mCaps.mMaxPushDescriptors << LL_ENDL;
    return true;
}

bool Context::createDevice()
{
#if LL_WINDOWS
    if (GetEnvironmentVariableA("LL_VK_AV_LOG", nullptr, 0) > 0)
    {
        AddVectoredExceptionHandler(1, av_stack_handler);
    }
#endif
    FeatureChain chain;
    // C6: ray tracing goes onto the device only when asked for and the device can do it. The extension structs of the vendor / later extensions stay out
    // (reported by pickDevice, not enabled): only what the first ray tracing steps use, which is what a device creation check should cover.
    const bool rt_on = rayTracingRequested() && mCaps.mRt.mUsable;
    if (rt_on)
    {
        Capabilities::RayTracing want = mCaps.mRt;
        want.mExtReorder = want.mExtOpacityMicromap = want.mExtCluster = false;
        chain.addRayTracing(want, false);
    }
    else if (rayTracingRequested())
    {
        LL_WARNS("Vulkan") << "LL_VK_RT is set but this device cannot do ray queries (acceleration structure " << mCaps.mRt.mAccelStruct << ", ray query " << mCaps.mRt.mRayQuery << ")" << LL_ENDL;
    }
    vkGetPhysicalDeviceFeatures2(mPhysical, &chain.f2);

    const float priority = 1.f;
    // <FS> E2: the ray scene work on the compute queue (LL_VK_RT_EARLY=3, the default since 2026-11-05: 0.35 ms of GPU a frame less than building at the lighting, same-session A/B): needs the compute-only
    // family and timeline semaphores, and ray tracing on; LL_VK_RT_EARLY=0 / 1 / 2 turn it off (the live key rtearly= in rt-debug.txt picks the build point of each frame while it is set up)
    {
        char b[8] = {};
        mAsyncOn = rt_on && mCaps.mComputeFamily >= 0 && mCaps.mTimelineSemaphore && (GetEnvironmentVariableA("LL_VK_RT_EARLY", b, sizeof(b)) == 0 || b[0] == '3');
        LL_INFOS("Vulkan") << "E2 (ray scene on the compute queue): " << (mAsyncOn ? "on" : "off") << LL_ENDL;
    }
    VkDeviceQueueCreateInfo qcis[2] = { { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO }, { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO } };
    qcis[0].queueFamilyIndex = mQueueFamily;
    qcis[0].queueCount = 1;
    qcis[0].pQueuePriorities = &priority;
    qcis[1].queueFamilyIndex = (U32)std::max<S32>(mCaps.mComputeFamily, 0);
    qcis[1].queueCount = 1;
    qcis[1].pQueuePriorities = &priority;

    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext = &chain.f2;     // enables every supported feature in the chain (all required ones are present)
    dci.queueCreateInfoCount = mAsyncOn ? 2 : 1;
    dci.pQueueCreateInfos = qcis;
    std::vector<const char*> device_extensions(REQUIRED_DEVICE_EXTENSIONS, REQUIRED_DEVICE_EXTENSIONS + LL_ARRAY_SIZE(REQUIRED_DEVICE_EXTENSIONS));
    if (rt_on)
    {
        const Capabilities::RayTracing& rt = mCaps.mRt;
        device_extensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        device_extensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        device_extensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
        if (rt.mExtPipeline) device_extensions.push_back(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
        if (rt.mExtPositionFetch) device_extensions.push_back(VK_KHR_RAY_TRACING_POSITION_FETCH_EXTENSION_NAME);
        if (rt.mExtMaintenance1) device_extensions.push_back(VK_KHR_RAY_TRACING_MAINTENANCE_1_EXTENSION_NAME);
    }
    if (mCaps.mCheckpoints)
    {
        device_extensions.push_back(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
    }
    VkPhysicalDeviceAddressBindingReportFeaturesEXT addr_features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ADDRESS_BINDING_REPORT_FEATURES_EXT };
    if (addr_log_wanted())
    {
        U32 n = 0;
        vkEnumerateDeviceExtensionProperties(mPhysical, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> have(n);
        vkEnumerateDeviceExtensionProperties(mPhysical, nullptr, &n, have.data());
        if (has_extension(have, VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME))
        {
            device_extensions.push_back(VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME);
            addr_features.reportAddressBinding = VK_TRUE;
            VkBaseOutStructure* tail = reinterpret_cast<VkBaseOutStructure*>(&chain.f2);
            while (tail->pNext)
            {
                tail = tail->pNext;
            }
            tail->pNext = reinterpret_cast<VkBaseOutStructure*>(&addr_features);
        }
    }
    // LL_VK_ROBUST2=1: VK_EXT_robustness2 (robust image access: an access outside the image or view reads zeros instead of memory; null descriptors read zeros). A/B for the login device lost.
    VkPhysicalDeviceRobustness2FeaturesEXT robust2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT };
    if (GetEnvironmentVariableA("LL_VK_ROBUST2", nullptr, 0) > 0)
    {
        U32 n = 0;
        vkEnumerateDeviceExtensionProperties(mPhysical, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> have(n);
        vkEnumerateDeviceExtensionProperties(mPhysical, nullptr, &n, have.data());
        if (has_extension(have, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME))
        {
            device_extensions.push_back(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
            robust2.robustBufferAccess2 = VK_TRUE;
            robust2.robustImageAccess2 = VK_TRUE;
            robust2.nullDescriptor = VK_TRUE;
            VkBaseOutStructure* tail = reinterpret_cast<VkBaseOutStructure*>(&chain.f2);
            while (tail->pNext)
            {
                tail = tail->pNext;
            }
            tail->pNext = reinterpret_cast<VkBaseOutStructure*>(&robust2);
            LL_INFOS("Vulkan") << "robustness2 enabled (LL_VK_ROBUST2): robust buffer and image access, null descriptors" << LL_ENDL;
        }
    }
    VkPhysicalDeviceFaultFeaturesEXT fault_features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT };
    if (mCaps.mDeviceFault)
    {
        device_extensions.push_back(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
        fault_features.deviceFault = VK_TRUE;
        VkBaseOutStructure* tail = reinterpret_cast<VkBaseOutStructure*>(&chain.f2);
        while (tail->pNext)
        {
            tail = tail->pNext;
        }
        tail->pNext = reinterpret_cast<VkBaseOutStructure*>(&fault_features);
    }
    // <FS> device lost diagnostics (2026-11-09): NVIDIA's VK_NV_device_diagnostics_config makes the driver keep the resources it hands out by address, so the Aftermath decode of a page fault names what used to live
    // at the faulting address (an image freed lately, with its debug name "GL texture N"); shader debug info lets the decoder map the faulting instruction when it is given the SPIR-V. LL_VK_DIAG=0 turns it off.
    VkDeviceDiagnosticsConfigCreateInfoNV diag_config = { VK_STRUCTURE_TYPE_DEVICE_DIAGNOSTICS_CONFIG_CREATE_INFO_NV };
    {
#if defined(LL_LOCAL_NV_DIAG)
        static const bool diag_on = getenv("LL_VK_DIAG") == nullptr || atoi(getenv("LL_VK_DIAG")) != 0;
#else
        static const bool diag_on = false;      // a release build asks the driver for nothing NVIDIA specific (REKINDLED_LOCAL_NVIDIA turns it on for a local build)
#endif
        U32 n = 0;
        vkEnumerateDeviceExtensionProperties(mPhysical, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> have_all(n);
        vkEnumerateDeviceExtensionProperties(mPhysical, nullptr, &n, have_all.data());
        if (diag_on && has_extension(have_all, VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME))
        {
            device_extensions.push_back(VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME);
            diag_config.flags = VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_SHADER_DEBUG_INFO_BIT_NV | VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_RESOURCE_TRACKING_BIT_NV;
            VkBaseOutStructure* tail = reinterpret_cast<VkBaseOutStructure*>(&chain.f2);
            while (tail->pNext)
            {
                tail = tail->pNext;
            }
            tail->pNext = reinterpret_cast<VkBaseOutStructure*>(&diag_config);
            LL_INFOS("Vulkan") << "device diagnostics (VK_NV_device_diagnostics_config): resource tracking and shader debug info are on (LL_VK_DIAG=0 turns them off)" << LL_ENDL;
        }
    }
    if (LLVkUpscale::requested())
    {
        LLVkUpscale::addDeviceExtensions(mInstance, mPhysical, device_extensions);
    }
    dci.enabledExtensionCount = (U32)device_extensions.size();
    dci.ppEnabledExtensionNames = device_extensions.data();
    if (!vk_ok(vkCreateDevice(mPhysical, &dci, nullptr, &mDevice), "vkCreateDevice"))
    {
        return false;
    }
    mCaps.mRt.mEnabled = rt_on;
    if (rt_on)
    {
        LL_INFOS("Vulkan") << "ray tracing extensions and features are enabled on the device (LL_VK_RT)" << LL_ENDL;
    }

#define LL_VK_GLOBAL_FN(name)
#define LL_VK_INSTANCE_FN(name)
#define LL_VK_DEVICE_FN(name) name = (PFN_##name)vkGetDeviceProcAddr(mDevice, #name);
#include "llvkfunctions.inl"

    vkGetDeviceQueue(mDevice, mQueueFamily, 0, &mQueue);
    if (mAsyncOn)
    {
        vkGetDeviceQueue(mDevice, (U32)mCaps.mComputeFamily, 0, &mComputeQueue);
        VkSemaphoreTypeCreateInfo ti = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
        ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        ti.initialValue = 0;
        VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        si.pNext = &ti;
        if (!mComputeQueue || !vk_ok(vkCreateSemaphore(mDevice, &si, nullptr, &mAsyncSem), "timeline semaphore"))
        {
            LL_WARNS("Vulkan") << "E2: no compute queue or timeline semaphore: off" << LL_ENDL;
            mAsyncOn = false;
        }
    }

    VmaVulkanFunctions funcs = {};
    funcs.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    funcs.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo ai = {};
    ai.physicalDevice = mPhysical;
    ai.device = mDevice;
    ai.instance = mInstance;
    ai.vulkanApiVersion = VK_API_VERSION_1_3;
    ai.pVulkanFunctions = &funcs;
    if (mCaps.mBufferDeviceAddress)
    {
        ai.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    }
    if (!vk_ok(vmaCreateAllocator(&ai, &mAllocator), "vmaCreateAllocator"))
    {
        return false;
    }
    if (LLVkUpscale::requested())
    {
        LLVkUpscale::init(mInstance, mPhysical, mDevice);      // not fatal: without it the viewer just does not upscale
        LLVkDenoise::init(mInstance, mPhysical, mDevice);      // not fatal either (the AMD reflection denoiser: PLAN-amd-denoiser.md)
    }
    return true;
}

bool Context::createFrames()
{
    LL_INFOS("Vulkan") << "frame slots in flight: " << frameCount() << LL_ENDL;
    for (U32 slot = 0; slot < frameCount(); ++slot)
    {
        Frame& f = mFrames[slot];
        VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pi.queueFamilyIndex = mQueueFamily;
        if (!vk_ok(vkCreateCommandPool(mDevice, &pi, nullptr, &f.mPool), "vkCreateCommandPool"))
        {
            return false;
        }
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = f.mPool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (!vk_ok(vkAllocateCommandBuffers(mDevice, &ai, &f.mCmd), "vkAllocateCommandBuffers"))
        {
            return false;
        }
        f.mCmds[0] = f.mCmd;
        if (mAsyncOn)
        {
            // the two further command buffers of the splits, and the compute pool and command buffer of the slot
            ai.commandBufferCount = 2;
            if (!vk_ok(vkAllocateCommandBuffers(mDevice, &ai, &f.mCmds[1]), "vkAllocateCommandBuffers (splits)"))
            {
                return false;
            }
            VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            cpi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            cpi.queueFamilyIndex = (U32)mCaps.mComputeFamily;
            VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            if (!vk_ok(vkCreateCommandPool(mDevice, &cpi, nullptr, &f.mComputePool), "vkCreateCommandPool (compute)"))
            {
                return false;
            }
            cai.commandPool = f.mComputePool;
            cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cai.commandBufferCount = 1;
            if (!vk_ok(vkAllocateCommandBuffers(mDevice, &cai, &f.mComputeCmd), "vkAllocateCommandBuffers (compute)"))
            {
                return false;
            }
        }
        if (mCaps.mTimestampValidBits > 0)
        {
            VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = 2 + MAX_GPU_STAMPS;
            if (vkCreateQueryPool(mDevice, &qi, nullptr, &f.mTsPool) != VK_SUCCESS)
            {
                f.mTsPool = VK_NULL_HANDLE;
            }
        }
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (!vk_ok(vkCreateFence(mDevice, &fi, nullptr, &f.mFence), "vkCreateFence")
            || !vk_ok(vkCreateSemaphore(mDevice, &si, nullptr, &f.mAcquire), "vkCreateSemaphore"))
        {
            return false;
        }
    }
    return true;
}

bool Context::init(HWND present_window)
{
    if (mReady)
    {
        return true;
    }
    mWindow = present_window;

    mFailureReason.clear();
    bool ok = false;
    if (!loadLoader())
    {
        mFailureReason = "The Vulkan runtime (vulkan-1.dll) was not found. It comes with the graphics driver: please install or update the driver.\n";
    }
    else if (!createInstance())
    {
        mFailureReason = "Vulkan could not be started (the instance could not be created). Please install or update the graphics driver.\n";
    }
    else if (!pickDevice())
    {
        if (mFailureReason.empty())
        {
            mFailureReason = "No suitable graphics device was found.\n";
        }
    }
    else if (!createDevice() || !createFrames() || !selfTest())
    {
        mFailureReason = "The graphics device could not be set up (the details are in the log file).\n";
    }
    else
    {
        ok = true;
    }
    if (!ok)
    {
        LL_WARNS("Vulkan") << "Vulkan context initialisation failed" << LL_ENDL;
        const std::string reason = mFailureReason;
        shutdown();
        mFailureReason = reason;
        return false;
    }
    mReady = true;
    measureBandwidth();
    LL_INFOS("Vulkan") << "Vulkan context ready" << LL_ENDL;
    return true;
}

void Context::destroyFrameResources(Frame& f)
{
    for (auto& fn : f.mDeferredLate)
    {
        fn();
    }
    f.mDeferredLate.clear();
    for (auto& fn : f.mDeferred)
    {
        fn();
    }
    f.mDeferred.clear();
    for (Buffer& b : f.mStagingBlocks)
    {
        if (b.valid())
        {
            vmaDestroyBuffer(mAllocator, b.mBuffer, b.mAllocation);
        }
    }
    f.mStagingBlocks.clear();
    if (f.mTsPool) vkDestroyQueryPool(mDevice, f.mTsPool, nullptr);
    if (f.mComputePool) vkDestroyCommandPool(mDevice, f.mComputePool, nullptr);
    if (f.mAcquire) vkDestroySemaphore(mDevice, f.mAcquire, nullptr);
    if (f.mFence) vkDestroyFence(mDevice, f.mFence, nullptr);
    if (f.mPool) vkDestroyCommandPool(mDevice, f.mPool, nullptr);
    f = Frame();
}

void Context::shutdown()
{
    if (mDevice != VK_NULL_HANDLE)
    {
        vkDeviceWaitIdle(mDevice);
        LLVkDenoise::shutdown();
        LLVkUpscale::shutdown();
        {
            std::vector<std::function<void()>> leftovers;
            {
                std::lock_guard<std::mutex> lock(mDeferredNextMutex);
                leftovers.swap(mDeferredNext);
            }
            for (auto& fn : leftovers)
            {
                fn();
            }
        }
        for (Frame& f : mFrames)
        {
            destroyFrameResources(f);
        }
        if (mImmediateFence) vkDestroyFence(mDevice, mImmediateFence, nullptr);
        if (mImmediatePool) vkDestroyCommandPool(mDevice, mImmediatePool, nullptr);
        mImmediateFence = VK_NULL_HANDLE;
        mImmediatePool = VK_NULL_HANDLE;
        mImmediateCmd = VK_NULL_HANDLE;
        if (mAsyncSem)
        {
            vkDestroySemaphore(mDevice, mAsyncSem, nullptr);
            mAsyncSem = VK_NULL_HANDLE;
        }
        arenaShutdown();
        if (mAllocator)
        {
            vmaDestroyAllocator(mAllocator);
            mAllocator = nullptr;
        }
        vkDestroyDevice(mDevice, nullptr);
        mDevice = VK_NULL_HANDLE;
    }
    if (mInstance != VK_NULL_HANDLE)
    {
        if (mSurface != VK_NULL_HANDLE)
        {
            vkDestroySurfaceKHR(mInstance, mSurface, nullptr);
            mSurface = VK_NULL_HANDLE;
        }
        if (mMessenger != VK_NULL_HANDLE)
        {
            auto destroy_messenger = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(mInstance, "vkDestroyDebugUtilsMessengerEXT");
            if (destroy_messenger)
            {
                destroy_messenger(mInstance, mMessenger, nullptr);
            }
            mMessenger = VK_NULL_HANDLE;
        }
        vkDestroyInstance(mInstance, nullptr);
        mInstance = VK_NULL_HANDLE;
    }
    mPhysical = VK_NULL_HANDLE;
    mQueue = VK_NULL_HANDLE;
    mReady = false;
    mInFrame = false;
    mFrameSlot = 0;
    mFrameNumber = 0;
    mCompletedBelow = 0;
}

void Context::waitIdle()
{
    if (mDevice != VK_NULL_HANDLE)
    {
        vkDeviceWaitIdle(mDevice);
    }
}

namespace
{
std::vector<std::string>& stamp_names()
{
    static std::vector<std::string> names = { "(uploads and copies before the first pass)" };
    return names;
}
}

U32 Context::stampTag(const std::string& name)
{
    auto& names = stamp_names();
    for (U32 i = 0; i < (U32)names.size(); ++i)
    {
        if (names[i] == name) return i;
    }
    names.push_back(name);
    return (U32)names.size() - 1;
}

U32 Context::drawStampTag(const std::string& text)
{
    static std::unordered_map<std::string, U32> index;
    static U32 full_tag = 0;
    auto& names = stamp_names();
    auto found = index.find(text);
    if (found != index.end())
    {
        return found->second;
    }
    if (names.size() >= 200000)
    {
        if (!full_tag)
        {
            names.push_back("draw (the table of draw descriptions is full)");
            full_tag = ((U32)names.size() - 1) | STAMP_DRAW;
        }
        return full_tag;
    }
    names.push_back(text);
    const U32 tag = ((U32)names.size() - 1) | STAMP_DRAW;
    index.emplace(text, tag);
    return tag;
}

std::string Context::stampTagName(U32 tag)
{
    auto& names = stamp_names();
    tag &= ~STAMP_DRAW;
    return tag < names.size() ? names[tag] : "?";
}

#if LL_WINDOWS
// A device lost leaves through the viewer's own message box, not an exception, so Windows writes no dump: the process memory (the draw ring, the geometry table, the arena, the ray scene state) is
// what a later look with the debugger needs. Private read / write memory only (the heaps and stacks, not the images or the mapped GPU memory): written once, after the report.
static void write_device_lost_dump()
{
    static bool done = false;
    if (done)
    {
        return;
    }
    done = true;
    char dir[MAX_PATH] = {};
    if (GetEnvironmentVariableA("APPDATA", dir, sizeof(dir)) == 0 || GetEnvironmentVariableA("LL_VK_NO_DUMP", nullptr, 0) > 0)
    {
        return;
    }
    HMODULE dbg = LoadLibraryA("dbghelp.dll");
    typedef BOOL(WINAPI * WriteDumpFn)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    WriteDumpFn write = dbg ? (WriteDumpFn)GetProcAddress(dbg, "MiniDumpWriteDump") : nullptr;
    if (!write)
    {
        return;
    }
    char path[MAX_PATH + 64] = {};
    snprintf(path, sizeof(path), "%s\\Rekindled_x64\\logs\\vk-device-lost-%lu.dmp", dir, (unsigned long)GetCurrentProcessId());
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    const BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), file,
                          (MINIDUMP_TYPE)(MiniDumpWithPrivateReadWriteMemory | MiniDumpWithDataSegs | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules), nullptr, nullptr, nullptr);
    const DWORD dump_error = ok ? 0 : GetLastError();
    LARGE_INTEGER size = {};
    GetFileSizeEx(file, &size);
    CloseHandle(file);
    if (!ok)
    {
        LL_WARNS("Vulkan") << "device lost: the process dump failed with error 0x" << std::hex << (unsigned long)dump_error << std::dec << " (0x8007012b = part of the memory could not be read)" << LL_ENDL;
    }
    LL_WARNS("Vulkan") << "device lost: process dump " << (ok ? "written" : "FAILED") << " (" << (unsigned long long)(size.QuadPart >> 20) << " MB, "
                       << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() << " ms): " << path << LL_ENDL;
}

#else
static void write_device_lost_dump() {}         // Windows only: a process dump through dbghelp
#endif

void Context::reportDeviceLost()
{
    reportCheckpoints("device lost");
    write_device_lost_dump();
}

std::string Context::describeBinding(U64 handle, bool buffer) const
{
    std::lock_guard<std::mutex> lock(sAddrMutex);
    if (sAddrCount == 0 || !handle)
    {
        return "no address log";
    }
    const U64 first = sAddrCount > ADDR_RING ? sAddrCount - ADDR_RING : 0;
    char buf[200];
    for (U64 i = sAddrCount; i > first; --i)
    {
        const AddrEvent& e = sAddrRing[(i - 1) % ADDR_RING];
        if (e.mHandle == handle && e.mObjectType == (buffer ? (U32)VK_OBJECT_TYPE_BUFFER : (U32)VK_OBJECT_TYPE_IMAGE))
        {
            snprintf(buf, sizeof(buf), "memory %s at 0x%llx + 0x%llx (%lld frames ago)", e.mBind ? "bound" : "UNBOUND", (unsigned long long)e.mBase, (unsigned long long)e.mSize, (long long)(mFrameNumber - e.mFrame));
            return buf;
        }
    }
    return buffer ? "no binding event for the buffer" : "no binding event for the image";
}

void Context::reportAddressHistory(const std::vector<VkDeviceFaultAddressInfoEXT>& addresses)
{
    std::lock_guard<std::mutex> lock(sAddrMutex);
    if (sAddrCount == 0)
    {
        return;
    }
    const U64 total = sAddrCount;
    const U64 first = total > ADDR_RING ? total - ADDR_RING : 0;
    const U64 now_us = addr_micros();
    LL_WARNS("Vulkan") << "  address binding log: " << total << " events (" << (total - first) << " kept), frame " << mFrameNumber << LL_ENDL;
    for (const auto& a : addresses)
    {
        const U64 at = (U64)a.reportedAddress;
        const U64 lo = at & ~0xFFFull, hi = lo + 0x1000;        // page precision
        // live bindings after replaying the kept events, and every event that touched the page
        std::map<U64, AddrEvent> live;
        std::vector<AddrEvent> touched;
        for (U64 i = first; i < total; ++i)
        {
            const AddrEvent& e = sAddrRing[i % ADDR_RING];
            if (e.mBind)
            {
                live[e.mBase] = e;
            }
            else
            {
                live.erase(e.mBase);
            }
            if (e.mBase < hi && e.mBase + e.mSize > lo)
            {
                touched.push_back(e);
            }
        }
        LL_WARNS("Vulkan") << "  fault page 0x" << std::hex << lo << std::dec << ": " << touched.size() << " events touched it" << LL_ENDL;
        for (size_t k = touched.size() > 200 ? touched.size() - 200 : 0; k < touched.size(); ++k)
        {
            const AddrEvent& e = touched[k];
            LL_WARNS("Vulkan") << "    " << (e.mBind ? "BIND  " : "UNBIND") << " " << addr_object_name(e.mObjectType) << (e.mInternal ? " (internal)" : "") << " handle 0x" << std::hex << e.mHandle
                               << " range 0x" << e.mBase << " + 0x" << e.mSize << std::dec << ", frame " << e.mFrame << " (" << (double)(now_us - e.mMicros) / 1000.0 << " ms ago, "
                               << (mFrameNumber - e.mFrame) << " frames ago)" << (e.mObjectType == (U32)VK_OBJECT_TYPE_IMAGE ? image_tag_text(e.mHandle, mFrameNumber) : std::string()) << LL_ENDL;
        }
        // is it live now, and the neighbours
        // the faults cluster at 0x1_1xxxxxxx: a valid low address with bit 32 set? look the page up as it would be with the high bits cleared
        for (U64 alt : { at & 0xFFFFFFFFull, at & ~(1ull << 32) })
        {
            if (alt == at)
            {
                continue;
            }
            const U64 alt_lo = alt & ~0xFFFull;
            std::vector<AddrEvent> alt_events;
            U64 bound_now = 0;
            for (U64 i = first; i < total; ++i)
            {
                const AddrEvent& e = sAddrRing[i % ADDR_RING];
                if (e.mBase < alt_lo + 0x1000 && e.mBase + e.mSize > alt_lo)
                {
                    alt_events.push_back(e);
                }
            }
            for (const auto& l : live)
            {
                if (l.second.mBase < alt_lo + 0x1000 && l.second.mBase + l.second.mSize > alt_lo)
                {
                    ++bound_now;
                }
            }
            LL_WARNS("Vulkan") << "    same page with the address as 0x" << std::hex << alt << std::dec << ": " << alt_events.size() << " events, " << bound_now << " live bindings now" << LL_ENDL;
            for (size_t k = alt_events.size() > 12 ? alt_events.size() - 12 : 0; k < alt_events.size(); ++k)
            {
                const AddrEvent& e = alt_events[k];
                LL_WARNS("Vulkan") << "      " << (e.mBind ? "BIND  " : "UNBIND") << " " << addr_object_name(e.mObjectType) << " handle 0x" << std::hex << e.mHandle << " range 0x" << e.mBase << " + 0x" << e.mSize << std::dec
                                   << ", " << (mFrameNumber - e.mFrame) << " frames ago" << LL_ENDL;
            }
        }
        auto it = live.upper_bound(at);
        if (it != live.begin())
        {
            auto prev = std::prev(it);
            const bool inside = at < prev->second.mBase + prev->second.mSize;
            LL_WARNS("Vulkan") << "    now: " << (inside ? "INSIDE a live binding" : "in no live binding") << "; the one below: " << addr_object_name(prev->second.mObjectType) << " 0x" << std::hex << prev->second.mBase << " + 0x" << prev->second.mSize
                               << " (the page is 0x" << (at - prev->second.mBase) << " from its start)" << std::dec << LL_ENDL;
        }
        if (it != live.end())
        {
            LL_WARNS("Vulkan") << "    the live one above: " << addr_object_name(it->second.mObjectType) << " 0x" << std::hex << it->second.mBase << " + 0x" << it->second.mSize << std::dec << LL_ENDL;
        }
        if (&a == &addresses[0])
        {
            // the last 40 unbinds overall (what was freed just before the loss)
            LL_WARNS("Vulkan") << "  the last unbinds (newest last):" << LL_ENDL;
            std::vector<AddrEvent> recent;
            for (U64 i = total; i > first && recent.size() < 40; --i)
            {
                const AddrEvent& e = sAddrRing[(i - 1) % ADDR_RING];
                if (!e.mBind)
                {
                    recent.push_back(e);
                }
            }
            for (size_t k = recent.size(); k > 0; --k)
            {
                const AddrEvent& e = recent[k - 1];
                LL_WARNS("Vulkan") << "    UNBIND " << addr_object_name(e.mObjectType) << " handle 0x" << std::hex << e.mHandle << " range 0x" << e.mBase << " + 0x" << e.mSize << std::dec << ", " << (mFrameNumber - e.mFrame) << " frames ago" << (e.mObjectType == (U32)VK_OBJECT_TYPE_IMAGE ? image_tag_text(e.mHandle, mFrameNumber) : std::string()) << LL_ENDL;
            }
            // and the last 40 binds of images and buffers over 64 KB (a texture or mesh that arrived just before the loss)
            LL_WARNS("Vulkan") << "  the last binds of 64 KB or more (newest last):" << LL_ENDL;
            std::vector<AddrEvent> binds;
            for (U64 i = total; i > first && binds.size() < 40; --i)
            {
                const AddrEvent& e = sAddrRing[(i - 1) % ADDR_RING];
                if (e.mBind && e.mSize >= 0x10000)
                {
                    binds.push_back(e);
                }
            }
            for (size_t k = binds.size(); k > 0; --k)
            {
                const AddrEvent& e = binds[k - 1];
                LL_WARNS("Vulkan") << "    BIND " << addr_object_name(e.mObjectType) << " handle 0x" << std::hex << e.mHandle << " range 0x" << e.mBase << " + 0x" << e.mSize << std::dec << ", " << (mFrameNumber - e.mFrame) << " frames ago" << LL_ENDL;
            }
        }
    }
}

// the size of the window's drawable area in pixels (what the swapchain has to match)
bool windowClientSize(HWND window, U32& width, U32& height)
{
#if LL_WINDOWS
    RECT rc = {};
    if (!GetClientRect(window, &rc))
    {
        return false;
    }
    width = (U32)(rc.right - rc.left);
    height = (U32)(rc.bottom - rc.top);
    return true;
#else
    int w = 0, h = 0;
    SDL_Vulkan_GetDrawableSize((SDL_Window*)window, &w, &h);
    width = (U32)(w > 0 ? w : 0);
    height = (U32)(h > 0 ? h : 0);
    return w > 0 && h > 0;
#endif
}

unsigned Context::flightThread()
{
    return (unsigned)GetCurrentThreadId() & 0xFFFFu;
}

void Context::dumpFlight(unsigned count, const char* why)
{
    static const char* const names[FL_KINDS] = { "?", "arena alloc", "arena free req", "arena free done", "buf create", "buf destroy", "buf write", "buf rename", "flush", "geo prepare", "geo cmds", "geo draw",
        "geo retain", "geo release", "geo buf event", "palette segment", "frame begin", "frame submit", "snapshot", "rt note", "rt scene", "misc" };
    const unsigned long long next = mFlightNext.load();
    const unsigned long long have = std::min<unsigned long long>(next, FLIGHT_EVENTS);
    const unsigned long long n = std::min<unsigned long long>(count, have);
    LL_WARNS("Vulkan") << "flight recorder (" << why << "): the last " << n << " of " << next << " events, oldest first; fields: [frame thread kind a b c d] in hex" << LL_ENDL;
    std::string line;
    unsigned per_line = 0;
    for (unsigned long long k = next - n; k < next; ++k)
    {
        const FlightEvent& e = mFlight[k & (FLIGHT_EVENTS - 1)];
        char buf[220];
        snprintf(buf, sizeof(buf), "[%llu t%x %s %llx %llx %llx %llx] ", e.mFrame, e.mThread, e.mKind < FL_KINDS ? names[e.mKind] : "?", e.a, e.b, e.c, e.d);
        line += buf;
        if (++per_line == 4)
        {
            LL_WARNS("Vulkan") << line << LL_ENDL;
            line.clear();
            per_line = 0;
        }
    }
    if (!line.empty())
    {
        LL_WARNS("Vulkan") << line << LL_ENDL;
    }
}

void Context::reportCheckpoints(const char* when)
{
    {
        static bool dumped_stuck = false;
        if (strcmp(when, "device lost") == 0)
        {
            dumpFlight(1500, when);
        }
        else if (!dumped_stuck)
        {
            dumped_stuck = true;
            dumpFlight(700, when);
        }
    }
    {
        U64 used = 0, budget = 0;
        if (videoMemory(used, budget))
        {
            LL_WARNS("Vulkan") << when << ": video memory used " << (used >> 20) << " MB of a budget of " << (budget >> 20) << " MB" << LL_ENDL;
        }
    }
    if (mCaps.mDeviceFault && vkGetDeviceFaultInfoEXT && strcmp(when, "device lost") == 0)       // only after the loss: asked while a frame is merely stuck, the driver itself crashed (access violation in nvoglv64.dll)
    {
        VkDeviceFaultCountsEXT counts = { VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT };
        const VkResult r = vkGetDeviceFaultInfoEXT(mDevice, &counts, nullptr);
        if (r == VK_SUCCESS || r == VK_INCOMPLETE)
        {
            std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
            std::vector<VkDeviceFaultVendorInfoEXT> vendors(counts.vendorInfoCount);
            LL_WARNS("Vulkan") << when << ": device fault counts: addresses " << counts.addressInfoCount << ", vendor records " << counts.vendorInfoCount << ", vendor binary " << (unsigned long long)counts.vendorBinarySize << " bytes (not requested)" << LL_ENDL;
            // the driver's own crash blob (what NVIDIA's tools read to name the shader and the resource): asked for with a buffer of the size it reported (an earlier build left the size set with a null
            // pointer and the driver wrote through it); written to the logs folder below
            const bool want_binary = counts.vendorBinarySize > 0 && counts.vendorBinarySize <= (64u << 20);
            std::vector<U8> vendor_binary(want_binary ? (size_t)counts.vendorBinarySize : 0);
            if (!want_binary)
            {
                counts.vendorBinarySize = 0;
            }
            VkDeviceFaultInfoEXT info = { VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT };
            info.pAddressInfos = addresses.empty() ? nullptr : addresses.data();
            info.pVendorInfos = vendors.empty() ? nullptr : vendors.data();
            info.pVendorBinaryData = vendor_binary.empty() ? nullptr : vendor_binary.data();
            const VkResult r2 = vkGetDeviceFaultInfoEXT(mDevice, &counts, &info);
            LL_WARNS("Vulkan") << when << ": vkGetDeviceFaultInfoEXT (second call) answered " << (int)r2 << LL_ENDL;
            if ((r2 == VK_SUCCESS || r2 == VK_INCOMPLETE) && !vendor_binary.empty())
            {
                char dir[MAX_PATH] = {};
                if (GetEnvironmentVariableA("APPDATA", dir, sizeof(dir)) > 0)
                {
                    char path[MAX_PATH + 64] = {};
                    snprintf(path, sizeof(path), "%s\\Rekindled_x64\\logs\\gpu-fault-frame%llu.bin", dir, (unsigned long long)mFrameNumber);
                    if (FILE* f = fopen(path, "wb"))
                    {
                        fwrite(vendor_binary.data(), 1, vendor_binary.size(), f);
                        fclose(f);
                        LL_WARNS("Vulkan") << when << ": the driver's crash blob (" << vendor_binary.size() << " bytes) is in " << path << LL_ENDL;
                    }
                }
            }
            if (r2 == VK_SUCCESS || r2 == VK_INCOMPLETE)
            {
                if (mBindlessAudit)
                {
                    LL_WARNS("Vulkan") << "  bindless audit (frame " << mFrameNumber << "): " << mBindlessAudit() << LL_ENDL;
                }
                LL_WARNS("Vulkan") << when << ": device fault: " << info.description << "; " << addresses.size() << " addresses, " << vendors.size() << " vendor records" << LL_ENDL;
                for (const auto& a : addresses)
                {
                    LL_WARNS("Vulkan") << "  fault address type " << (int)a.addressType << " (1 read invalid, 2 write invalid, 3 execute invalid, 4-6 instruction pointer) at 0x" << std::hex << (unsigned long long)a.reportedAddress
                                       << " precision 0x" << (unsigned long long)a.addressPrecision << std::dec << LL_ENDL;
                }
                {
                    // which arena block (if any) holds each faulting address: inside a block = a range that was freed (or never written); in none = not an arena address at all
                    std::unique_lock<std::mutex> lock(mArenaMutex, std::try_to_lock);
                    for (const auto& a : addresses)
                    {
                        const unsigned long long at = (unsigned long long)a.reportedAddress;
                        if ((int)a.addressType >= 4)
                        {
                            continue;       // an instruction pointer (shader code), not data
                        }
                        bool found = false;
                        for (size_t k = 0; lock.owns_lock() && k < mArenaBlocks.size(); ++k)
                        {
                            const ArenaBlock* b = mArenaBlocks[k];
                            if (b && b->mAddress && at + 1 > b->mAddress && at < b->mAddress + b->mSize + 0x1000)
                            {
                                found = true;
                                LL_WARNS("Vulkan") << "  address 0x" << std::hex << at << std::dec << " is in arena block " << k << " (" << (b->mAccel ? "accel" : (b->mHost ? "host" : (b->mShared ? "shared" : "device"))) << ", " << (b->mSize >> 20) << " MB, starts at 0x" << std::hex << (unsigned long long)b->mAddress << std::dec << ", offset " << (at - (unsigned long long)b->mAddress) << ")" << LL_ENDL;
                                LL_WARNS("Vulkan") << "    that block was made in frame " << b->mCreatedFrame << ", " << (mFrameNumber - b->mCreatedFrame) << " frames before this report (frame " << mFrameNumber << "); it holds "
                                                   << b->mAllocs << " ranges, " << (b->mUsed >> 20) << " MB in use" << LL_ENDL;
                                for (const auto& describer : mAddressDescribers)
                                {
                                    const std::string what = describer ? describer(at) : std::string();
                                    if (!what.empty())
                                    {
                                        LL_WARNS("Vulkan") << "    which is " << what << LL_ENDL;
                                    }
                                }
                            }
                        }
                        if (!found)
                        {
                            LL_WARNS("Vulkan") << "  address 0x" << std::hex << at << std::dec << " is in no arena block" << (lock.owns_lock() ? "" : " (arena busy: not checked)") << LL_ENDL;
                            {
                                const std::string dedicated = describeDedicated(at);
                                if (!dedicated.empty())
                                {
                                    LL_WARNS("Vulkan") << "    which is " << dedicated << LL_ENDL;
                                }
                            }
                            for (const auto& describer : mAddressDescribers)
                            {
                                const std::string what = describer ? describer(at) : std::string();
                                if (!what.empty())
                                {
                                    LL_WARNS("Vulkan") << "    which is " << what << LL_ENDL;
                                }
                            }
                        }
                    }
                    for (size_t k = 0; lock.owns_lock() && k < mArenaBlocks.size(); ++k)
                    {
                        const ArenaBlock* b = mArenaBlocks[k];
                        if (b)
                        {
                            LL_WARNS("Vulkan") << "  arena block " << k << ": " << (b->mAccel ? "accel" : (b->mHost ? "host" : (b->mShared ? "shared" : "device"))) << ", " << (b->mSize >> 20) << " MB at 0x" << std::hex << (unsigned long long)b->mAddress << std::dec << ", " << b->mAllocs << " ranges" << LL_ENDL;
                        }
                    }
                }
                reportAddressHistory(addresses);
                for (const auto& v : vendors)
                {
                    LL_WARNS("Vulkan") << "  vendor fault: " << v.description << ", code " << (unsigned long long)v.vendorFaultCode << ", data 0x" << std::hex << (unsigned long long)v.vendorFaultData << std::dec << LL_ENDL;
                }
            }
        }
        else
        {
            LL_WARNS("Vulkan") << when << ": vkGetDeviceFaultInfoEXT answered " << (int)r << LL_ENDL;
        }
    }
    if (!mCaps.mCheckpoints || !vkGetQueueCheckpointData2NV)
    {
        LL_WARNS("Vulkan") << when << ": this driver keeps no checkpoints" << LL_ENDL;
        return;
    }
    U32 count = 0;
    vkGetQueueCheckpointData2NV(mQueue, &count, nullptr);
    std::vector<VkCheckpointData2NV> data(count, VkCheckpointData2NV{ VK_STRUCTURE_TYPE_CHECKPOINT_DATA_2_NV });
    if (count)
    {
        vkGetQueueCheckpointData2NV(mQueue, &count, data.data());
    }
    // each entry: the latest checkpoint the GPU got to in the given pipeline stage(s); the marker is the pass tag + 1 (see gpuStamp)
    LL_WARNS("Vulkan") << when << ": " << count << " GPU checkpoints (the last pass reached, per pipeline stage):" << LL_ENDL;
    U64 started_serial = 0, completed_serial = 0;
    for (U32 i = 0; i < count; ++i)
    {
        const U64 marker = (U64)(uintptr_t)data[i].pCheckpointMarker;
        const unsigned long long stage = (unsigned long long)data[i].stage;
        const bool serial = (marker >> 62) == 1;
        const U64 number = marker & ((1ull << 62) - 1);
        const U32 tag = (U32)marker;
        if (serial)
        {
            if (stage & 0x1) started_serial = number;
            if (stage & 0x2000) completed_serial = number;
        }
        const std::string what = serial ? (mDrawDescriber ? mDrawDescriber(number) : std::string("draw ") + std::to_string(number)) : (tag ? stampTagName(tag - 1) : std::string("(none)"));
        LL_WARNS("Vulkan") << "  stage 0x" << std::hex << stage << std::dec << (stage & 0x1 ? " (top of pipe: the latest STARTED)" : (stage & 0x2000 ? " (bottom of pipe: the latest COMPLETED)" : "")) << ": "
                           << what << LL_ENDL;
        if (mSlotDescriber)
        {
            // the texture slots the draw names (per-draw stamps carry them in the text), with what the bindless table says about each
            const size_t at = what.find("texture slots");
            if (at != std::string::npos)
            {
                const char* p = what.c_str() + at + 13;
                for (int k = 0; k < 16; ++k)
                {
                    char* end = nullptr;
                    const unsigned long v = strtoul(p, &end, 10);
                    if (end == p) break;
                    LL_WARNS("Vulkan") << "      its " << mSlotDescriber((U32)v) << LL_ENDL;
                    p = end;
                }
            }
        }
    }
    if (mDrawDescriber && (completed_serial || started_serial))
    {
        // Markers are set BEFORE their draws and a marker completes when everything before it has: the draw named by the last COMPLETED marker is the first one that did not finish, and with a
        // fault it is the one the fault is in. The draws after it (up to the last started) are shown for context.
        const U64 culprit = completed_serial ? completed_serial : started_serial;
        LL_WARNS("Vulkan") << "THE FAULT IS MOST LIKELY IN: " << mDrawDescriber(culprit) << LL_ENDL;
        if (mSlotDescriber)
        {
            // the texture slots of that draw, with what the table says about each
            const std::string text = mDrawDescriber(culprit);
            const size_t at = text.find("texture slots");
            if (at != std::string::npos)
            {
                const char* p = text.c_str() + at + 13;
                for (int k = 0; k < 16; ++k)
                {
                    char* end = nullptr;
                    const unsigned long v = strtoul(p, &end, 10);
                    if (end == p) break;
                    LL_WARNS("Vulkan") << "    that draw's " << mSlotDescriber((U32)v) << LL_ENDL;
                    p = end;
                }
            }
        }
        for (U64 s = culprit > 6 ? culprit - 6 : 1; s <= std::max(culprit, started_serial) && s < culprit + 10; ++s)
        {
            LL_WARNS("Vulkan") << "    " << (s == culprit ? ">>> " : "    ") << mDrawDescriber(s) << LL_ENDL;
        }
    }
    // The whole pass the markers are in, from the host's list of what was recorded: every draw with its description, the one the GPU had started and the last one it completed marked.
    U32 started = 0, completed = 0;
    for (U32 i = 0; i < count; ++i)
    {
        const U32 tag = (U32)(uintptr_t)data[i].pCheckpointMarker;
        if (data[i].stage & 0x1) started = tag;
        if (data[i].stage & 0x2000) completed = tag;
    }
    U32 passes_shown = 0;
    for (U32 slot = 0; slot < frameCount() && passes_shown < 2; ++slot)
    {
        const Frame& f = mFrames[slot];
        const size_t n = f.mStampTags.size();
        for (size_t i = 0; i < n && passes_shown < 2; ++i)
        {
            if (!(f.mStampTags[i] & STAMP_DRAW) || (started && (f.mStampTags[i] & ~STAMP_DRAW) + 1 != started))
            {
                continue;
            }
            size_t first = i, last = i;
            while (first > 0 && (f.mStampTags[first - 1] & STAMP_DRAW))
            {
                --first;
            }
            while (last + 1 < n && (f.mStampTags[last + 1] & STAMP_DRAW))
            {
                ++last;
            }
            const std::string pass = first > 0 ? stampTagName(f.mStampTags[first - 1]) : std::string("(before the first pass)");
            LL_WARNS("Vulkan") << "pass [" << pass << "] in frame slot " << slot << ": " << (last - first + 1) << " draws stamped; the one the GPU had STARTED is number " << (i - first + 1) << ":" << LL_ENDL;
            for (size_t k = first; k <= last && k < first + 120; ++k)
            {
                LL_WARNS("Vulkan") << "    " << (k == i ? "STARTED   -> " : (completed && (f.mStampTags[k] & ~STAMP_DRAW) + 1 == completed ? "completed  -> " : "              ")) << stampTagName(f.mStampTags[k]) << LL_ENDL;
            }
            ++passes_shown;
            break;
        }
    }
}

void Context::serialCheckpoint(U64 serial)
{
    if (mInFrame && mCaps.mCheckpoints && vkCmdSetCheckpointNV)
    {
        vkCmdSetCheckpointNV(mFrames[mFrameSlot].mCmd, (const void*)(uintptr_t)((1ull << 62) | serial));
    }
}

void Context::drawCheckpoint(U32 tag)
{
    if (mInFrame && mCaps.mCheckpoints && vkCmdSetCheckpointNV)
    {
        vkCmdSetCheckpointNV(mFrames[mFrameSlot].mCmd, (const void*)(uintptr_t)((tag & ~STAMP_DRAW) + 1));
    }
}

void Context::gpuStamp(U32 tag)
{
    if (!mInFrame || mStampsOff)
    {
        return;
    }
    Frame& f = mFrames[mFrameSlot];
    if ((tag & STAMP_DRAW) && f.mStampTags.size() + 512 >= MAX_GPU_STAMPS)
    {
        return;         // draw stamps leave room for the passes' own
    }
    if (mStampHook && !(tag & STAMP_DRAW))
    {
        mStampHook(tag);
    }
    if (mCaps.mCheckpoints && vkCmdSetCheckpointNV)
    {
        vkCmdSetCheckpointNV(f.mCmd, (const void*)(uintptr_t)((tag & ~STAMP_DRAW) + 1));
    }
    {
        static const bool labels = GetEnvironmentVariableA("LL_VK_LABELS", nullptr, 0) > 0;
        if (labels && !(tag & STAMP_DRAW))
        {
            static PFN_vkCmdInsertDebugUtilsLabelEXT insert_label = (PFN_vkCmdInsertDebugUtilsLabelEXT)vkGetInstanceProcAddr(mInstance, "vkCmdInsertDebugUtilsLabelEXT");
            if (insert_label)
            {
                const std::string name = stampTagName(tag);
                VkDebugUtilsLabelEXT label = { VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT };
                label.pLabelName = name.c_str();
                insert_label(f.mCmd, &label);
            }
        }
    }
    const U32 n = (U32)f.mStampTags.size();
    if (!f.mTsPool || n >= MAX_GPU_STAMPS)
    {
        return;
    }
    vkCmdWriteTimestamp2(f.mCmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, f.mTsPool, 2 + n);
    f.mStampTags.push_back(tag);
}

VkCommandBuffer Context::beginFrame(bool wait)
{
    llassert(!mInFrame);
    Frame& f = mFrames[mFrameSlot];

    if (!wait && vkGetFenceStatus(mDevice, f.mFence) != VK_SUCCESS)
    {
        return VK_NULL_HANDLE;
    }
    const auto wait_start = std::chrono::steady_clock::now();
    {
        // The wait is where a GPU job that does not end shows itself (the driver resets the device after about 2 s: VK_ERROR_DEVICE_LOST, and everything the GPU knew is gone). While the frame
        // is stuck, the timestamps its passes already wrote say which pass the GPU is in: logged at 1 s and at 1.7 s, before the reset takes the evidence.
        int reports = 0;
        for (;;)
        {
            const VkResult wr = vkWaitForFences(mDevice, 1, &f.mFence, VK_TRUE, 250ull * 1000 * 1000);
            if (wr == VK_SUCCESS)
            {
                break;
            }
            if (wr != VK_TIMEOUT)
            {
                vk_ok(wr, "vkWaitForFences");      // device lost: logged once, with the checkpoints when the driver has them
                break;
            }
            const double waited_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wait_start).count();
            if ((reports == 0 && waited_ms >= 1000.0) || (reports == 1 && waited_ms >= 1700.0))
            {
                ++reports;
                const U32 n = (U32)f.mStampTags.size();
                std::string where = "no per-pass stamps (LL_VK_PHASES is off)";
                if (f.mTsPool && f.mTsPending && n)
                {
                    std::vector<U64> r((size_t)(2 + n) * 2);        // per query: the value and its availability
                    vkGetQueryPoolResults(mDevice, f.mTsPool, 0, 2 + n, r.size() * sizeof(U64), r.data(), 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
                    int last = -1;
                    for (U32 i = 0; i < n; ++i)
                    {
                        if (r[(2 + i) * 2 + 1] != 0)
                        {
                            last = (int)i;
                        }
                    }
                    int pass_i = last;
                    while (pass_i >= 0 && (f.mStampTags[pass_i] & STAMP_DRAW))
                    {
                        --pass_i;
                    }
                    const std::string pass = pass_i >= 0 ? stampTagName(f.mStampTags[pass_i]) : std::string("the work before the first pass");
                    where = std::to_string(last + 1) + " of " + std::to_string(n) + " stamps reached; the GPU is inside [" + pass + "]";
                    if (last + 1 < (int)n && (f.mStampTags[last + 1] & STAMP_DRAW))
                    {
                        // a draw stamp is written after the draw it names: the first one that never completed is the draw the GPU is stuck in
                        where += ", STUCK IN THE DRAW: [" + stampTagName(f.mStampTags[last + 1]) + "]";
                        if (last >= 0 && (f.mStampTags[last] & STAMP_DRAW))
                        {
                            where += "; the draw before it finished: [" + stampTagName(f.mStampTags[last]) + "]";
                        }
                    }
                    else
                    {
                        where += last + 1 < (int)n ? ", which ends at [" + stampTagName(f.mStampTags[last + 1]) + "]" : std::string(", the last pass of the frame");
                    }
                }
                LL_WARNS("VkPhases") << "GPU frame (slot " << mFrameSlot << ") still running after " << (S32)waited_ms << " ms: " << where << LL_ENDL;
                if (reports == 1)
                {
                    if (mHangReporter)
                    {
                        mHangReporter();    // (the first report of the hang only: the driver resets the device soon)
                    }
                    reportCheckpoints("frame stuck");       // the driver may answer now, before it resets the device
                }
            }
        }
    }
    gSyncStats.mFenceWaitMicros.fetch_add((U64)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - wait_start).count(), std::memory_order_relaxed);
    if (mFrameNumber >= frameCount())
    {
        // <FS> the frame that used this slot last (frameCount() frames back) has been waited for, as every frame before it was by the frames before this one: frameFinished answers from this number,
        // because the fence that says so is reset on the next line
        mCompletedBelow = std::max(mCompletedBelow, mFrameNumber - frameCount() + 1);
    }
    vkResetFences(mDevice, 1, &f.mFence);

    if (f.mTsPending && f.mTsPool)
    {
        f.mTsPending = false;
        const U32 n = (U32)f.mStampTags.size();
        std::vector<U64> r((size_t)(2 + n) * 2);        // per query: the value and its availability
        const VkResult res = vkGetQueryPoolResults(mDevice, f.mTsPool, 0, 2 + n, r.size() * sizeof(U64), r.data(), 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        bool all = res == VK_SUCCESS;
        for (U32 q = 0; all && q < 2 + n; ++q) all = r[q * 2 + 1] != 0;
        if (all)
        {
            const U32 bits = mCaps.mTimestampValidBits;
            const U64 mask = bits >= 64 ? ~0ull : ((1ull << bits) - 1);
            auto to_micros = [&](U64 from, U64 to) { return (U64)((double)((to - from) & mask) * (double)mCaps.mTimestampPeriodNs / 1000.0); };
            const U64 start = r[0], end = r[2];
            const U64 micros = to_micros(start, end);
            gSyncStats.mGpuFrames.fetch_add(1, std::memory_order_relaxed);
            gSyncStats.mGpuMicros.fetch_add(micros, std::memory_order_relaxed);
            U64 prev = gSyncStats.mGpuMaxMicros.load(std::memory_order_relaxed);
            while (micros > prev && !gSyncStats.mGpuMaxMicros.compare_exchange_weak(prev, micros, std::memory_order_relaxed)) {}
            if (f.mOffscreen) { mPassStats.mOffMicros += micros; ++mPassStats.mOffFrames; mPassStats.mOffMax = std::max(mPassStats.mOffMax, micros); }
            else
            {
                mPassStats.mMainMicros += micros; ++mPassStats.mMainFrames; mPassStats.mMainMax = std::max(mPassStats.mMainMax, micros);
                // how the GPU time of the presented frames is spread (the 150 ms log below only sees the worst): counts per band, logged every 600 frames
                static const U64 kBands[] = { 12000, 16700, 25000, 40000, 75000, 150000 };
                static U32 bands[7] = {};
                static U32 seen = 0;
                U32 band = 0;
                while (band < 6 && micros > kBands[band]) ++band;
                ++bands[band];
                if (++seen == 600)
                {
                    LL_INFOS("VkPhases") << "GPU time of 600 presented frames: up to 12 ms " << bands[0] << ", to 16.7 " << bands[1] << ", to 25 " << bands[2] << ", to 40 " << bands[3]
                        << ", to 75 " << bands[4] << ", to 150 " << bands[5] << ", over 150 " << bands[6] << LL_ENDL;
                    memset(bands, 0, sizeof(bands));
                    seen = 0;
                }
            }
            if (n)
            {
                // the time from one pass's stamp to the next is that pass's; the first stretch (uploads, copies) and the last pass run to the frame's own start and end
                U64 last = start;
                U32 last_tag = 0;
                const bool slow = micros > 150000;          // a frame the GPU needed more than 150 ms for: say which passes took it
                std::vector<std::pair<U64, U32>> frame_passes;
                std::vector<std::pair<U64, U32>> frame_draws;       // the draws of a slow frame by the time up to their stamp (a draw stamp follows the draw): the slowest ones are named below
                static const U32 builds_tag = stampTag("rt scene builds");
                U64 builds_us = 0;                          // the ray scene's builds in this frame: the pacing of the next ones (llvkrt.cpp) reads it
                for (U32 i = 0; i < n; ++i)
                {
                    const U64 stamp = r[(2 + i) * 2];
                    const U64 part = to_micros(last, stamp);
                    mPassStats.mMicros[last_tag] += part;
                    builds_us += last_tag == builds_tag ? part : 0;
                    if (slow && (f.mStampTags[i] & STAMP_DRAW))
                    {
                        frame_draws.push_back({ part, f.mStampTags[i] });
                    }
                    if (slow) frame_passes.push_back({ part, last_tag });
                    last = stamp;
                    if (!(f.mStampTags[i] & STAMP_DRAW))
                    {
                        last_tag = f.mStampTags[i];             // (a draw stamp ends a draw of the pass: the time stays the pass's)
                    }
                }
                mPassStats.mMicros[last_tag] += to_micros(last, end);
                builds_us += last_tag == builds_tag ? to_micros(last, end) : 0;
                mRtBuildMicros[mFrameSlot].store(builds_us, std::memory_order_relaxed);
                if (slow)
                {
                    frame_passes.push_back({ to_micros(last, end), last_tag });
                    std::sort(frame_passes.rbegin(), frame_passes.rend());
                    static U32 logged_slow = 0;
                    if (logged_slow++ < 400)         // (20 were used up by one teleport arrival: the spikes of the settled scene, 2026-10-25, need their passes too)
                    {
                        std::string text;
                        for (size_t k = 0; k < frame_passes.size() && k < 6; ++k)
                        {
                            text += "[" + stampTagName(frame_passes[k].second) + "] " + std::to_string((double)frame_passes[k].first / 1000.0) + " ms | ";
                        }
                        LL_WARNS("VkPhases") << "slow GPU frame (" << (f.mOffscreen ? "off-screen" : "presented") << "): " << (double)micros / 1000.0 << " ms; longest passes: " << text << LL_ENDL;
                        if (!frame_draws.empty())
                        {
                            std::sort(frame_draws.rbegin(), frame_draws.rend());
                            std::string draws;
                            for (size_t k = 0; k < frame_draws.size() && k < 3; ++k)
                            {
                                draws += "[" + stampTagName(frame_draws[k].second) + "] " + std::to_string((double)frame_draws[k].first / 1000.0) + " ms | ";
                            }
                            LL_WARNS("VkPhases") << "slow GPU frame: slowest draws (" << frame_draws.size() << " draws stamped): " << draws << LL_ENDL;
                        }
                    }
                }
            }
        }
    }
    f.mStampTags.clear();
    f.mOffscreen = mNextOffscreen;
    mNextOffscreen = false;

    // What was queued two rounds of this slot ago is destroyed now; what was queued last round waits one more (the frame it was queued in has completed, but a copy or a draw that names it can have
    // been recorded into one of the frames after it: pending writes are flushed late, a rename's copy goes into whichever frame is recording when the buffers are flushed).
    {
        std::vector<std::function<void()>> due;
        due.swap(f.mDeferredLate);
        for (auto& fn : due)
        {
            fn();
        }
    }
    f.mDeferredLate.swap(f.mDeferred);
    f.mDeferred.clear();
    {
        // what was given up since the last frame ended goes with this frame: destroyed when this slot comes round again
        std::lock_guard<std::mutex> lock(mDeferredNextMutex);
        for (auto& fn : mDeferredNext)
        {
            f.mDeferred.push_back(std::move(fn));
        }
        mDeferredNext.clear();
    }

    // keep the first staging block, free any extras that were needed
    for (size_t i = 1; i < f.mStagingBlocks.size(); ++i)
    {
        vmaDestroyBuffer(mAllocator, f.mStagingBlocks[i].mBuffer, f.mStagingBlocks[i].mAllocation);
    }
    if (f.mStagingBlocks.size() > 1)
    {
        f.mStagingBlocks.resize(1);
    }
    f.mStagingUsed = 0;

    vkResetCommandPool(mDevice, f.mPool, 0);
    if (mAsyncOn)
    {
        f.mCmdIndex = 0;
        f.mCmd = f.mCmds[0];
        f.mWaitValue = 0;
        f.mComputeDone = 0;
        vkResetCommandPool(mDevice, f.mComputePool, 0);
    }

    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.mCmd, &bi);
    if (f.mTsPool)
    {
        vkCmdResetQueryPool(f.mCmd, f.mTsPool, 0, 2 + MAX_GPU_STAMPS);
        vkCmdWriteTimestamp2(f.mCmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, f.mTsPool, 0);
    }
    mInFrame = true;
    sFrameThread = std::this_thread::get_id();      // (GPU-LIFETIMES.md 5.8: the thread that records frames; destroyLater checks its callers against it)
    {
        static const U32 start_tag = stampTag("frame start");
        gpuStamp(start_tag);            // (the first stamp of the frame: a hang reported with no stamp reached is before this)
    }
    return f.mCmd;
}

void Context::checkpointOnly(U32 tag)
{
    if (mInFrame && mCaps.mCheckpoints && vkCmdSetCheckpointNV)
    {
        vkCmdSetCheckpointNV(mFrames[mFrameSlot].mCmd, (const void*)(uintptr_t)((tag & ~STAMP_DRAW) + 1));
    }
}

void Context::endFrame(VkSemaphore wait_semaphore, VkSemaphore signal_semaphore)
{
    llassert(mInFrame);
    Frame& f = mFrames[mFrameSlot];
    if (f.mTsPool)
    {
        vkCmdWriteTimestamp2(f.mCmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, f.mTsPool, 1);
        f.mTsPending = true;
    }
    vkEndCommandBuffer(f.mCmd);

    VkCommandBufferSubmitInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    cbi.commandBuffer = f.mCmd;

    VkSemaphoreSubmitInfo waits[2] = { { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO }, { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO } };
    U32 wait_count = 0;
    if (wait_semaphore != VK_NULL_HANDLE)
    {
        waits[wait_count].semaphore = wait_semaphore;
        waits[wait_count].stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        ++wait_count;
    }
    static const int break_order_graphics = getenv("LL_VK_BREAK_ORDER") ? atoi(getenv("LL_VK_BREAK_ORDER")) : 0;      // <FS> =2: the frame's last submit does not wait for the compute work (see submitCompute)
    if ((f.mWaitValue || f.mComputeDone) && break_order_graphics != 2)
    {
        // E2: the ray scene the compute queue builds for this frame (the lighting reads it); the last submit of the frame waits for the compute work even when the join was not reached
        waits[wait_count].semaphore = mAsyncSem;
        waits[wait_count].value = std::max(f.mWaitValue, f.mComputeDone);
        waits[wait_count].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        ++wait_count;
        f.mWaitValue = 0;
        f.mComputeDone = 0;
    }
    VkSemaphoreSubmitInfo signal = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    signal.semaphore = signal_semaphore;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSubmitInfo2 si = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    si.waitSemaphoreInfoCount = wait_count;
    si.pWaitSemaphoreInfos = waits;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = signal_semaphore != VK_NULL_HANDLE ? 1 : 0;
    si.pSignalSemaphoreInfos = &signal;
    vk_ok(vkQueueSubmit2(mQueue, 1, &si, f.mFence), "vkQueueSubmit2");
    flight(FL_FRAME_SUBMIT, (unsigned long long)mFrameSlot, (unsigned long long)mFrameNumber, (unsigned long long)(uintptr_t)f.mCmd);

    mInFrame = false;
    mFrameSlot = (mFrameSlot + 1) % frameCount();
    ++mFrameNumber;
}

// <FS> E2: see the declarations in llvkcontext.h and doc/vulkan/PLAN-async-compute.md
bool Context::splitFrame(bool signal, U64& value)
{
    value = 0;
    if (!mAsyncOn || !mInFrame)
    {
        return false;
    }
    Frame& f = mFrames[mFrameSlot];
    if (f.mCmdIndex + 1 >= 3)
    {
        return false;       // (the frame has only three command buffers)
    }
    vkEndCommandBuffer(f.mCmd);
    VkCommandBufferSubmitInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    cbi.commandBuffer = f.mCmd;
    VkSemaphoreSubmitInfo sig = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    VkSubmitInfo2 si = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    if (signal)
    {
        value = ++mAsyncValue;
        sig.semaphore = mAsyncSem;
        sig.value = value;
        sig.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        si.signalSemaphoreInfoCount = 1;
        si.pSignalSemaphoreInfos = &sig;
    }
    // (the part waits for the previous values it was told to wait for: this part is cut before the wait is set)
    const bool ok = vk_ok(vkQueueSubmit2(mQueue, 1, &si, VK_NULL_HANDLE), "split submit");
    flight(FL_FRAME_SUBMIT, (unsigned long long)mFrameSlot, (unsigned long long)mFrameNumber, (unsigned long long)(uintptr_t)f.mCmd);
    ++f.mCmdIndex;
    f.mCmd = f.mCmds[f.mCmdIndex];
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.mCmd, &bi);
    return ok;
}

VkCommandBuffer Context::beginCompute()
{
    if (!mAsyncOn || !mInFrame)
    {
        return VK_NULL_HANDLE;
    }
    Frame& f = mFrames[mFrameSlot];
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.mComputeCmd, &bi);
    return f.mComputeCmd;
}

bool Context::submitCompute(VkCommandBuffer cmd, U64 wait_value, U64& signal_value)
{
    signal_value = 0;
    if (!mAsyncOn || cmd == VK_NULL_HANDLE)
    {
        return false;
    }
    vkEndCommandBuffer(cmd);
    VkCommandBufferSubmitInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    cbi.commandBuffer = cmd;
    VkSemaphoreSubmitInfo wait = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    VkSemaphoreSubmitInfo sig = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    VkSubmitInfo2 si = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    // <FS> LL_VK_BREAK_ORDER=1 (a test of the test, doc/vulkan/GPU-LIFETIMES.md section 9): the compute work does not wait for the uploads of its own frame. Never for use: the stress lap must show it.
    static const int break_order = getenv("LL_VK_BREAK_ORDER") ? atoi(getenv("LL_VK_BREAK_ORDER")) : 0;
    if (wait_value && break_order != 1)
    {
        wait.semaphore = mAsyncSem;
        wait.value = wait_value;
        wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        si.waitSemaphoreInfoCount = 1;
        si.pWaitSemaphoreInfos = &wait;
    }
    signal_value = ++mAsyncValue;
    sig.semaphore = mAsyncSem;
    sig.value = signal_value;
    sig.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &sig;
    mFrames[mFrameSlot].mComputeDone = signal_value;
    return vk_ok(vkQueueSubmit2(mComputeQueue, 1, &si, VK_NULL_HANDLE), "compute submit");
}

void Context::setNextSubmitWait(U64 value)
{
    if (mInFrame)
    {
        mFrames[mFrameSlot].mWaitValue = value;
    }
}

bool Context::flushFrameAndWait()
{
    if (!mInFrame || !mImmediateFence)
    {
        if (!mImmediateFence)
        {
            immediateSubmit([](VkCommandBuffer) {});     // creates the immediate pool and fence
        }
        if (!mInFrame || !mImmediateFence)
        {
            return false;
        }
    }
    Frame& f = mFrames[mFrameSlot];
    vkEndCommandBuffer(f.mCmd);

    VkCommandBufferSubmitInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    cbi.commandBuffer = f.mCmd;
    VkSubmitInfo2 si = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    VkSemaphoreSubmitInfo async_wait = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    if (f.mWaitValue || f.mComputeDone)
    {
        // E2: this part of the frame reads the scene the compute queue builds
        async_wait.semaphore = mAsyncSem;
        async_wait.value = std::max(f.mWaitValue, f.mComputeDone);
        async_wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        si.waitSemaphoreInfoCount = 1;
        si.pWaitSemaphoreInfos = &async_wait;
        f.mWaitValue = 0;
        f.mComputeDone = 0;
    }
    vkResetFences(mDevice, 1, &mImmediateFence);
    bool ok = vk_ok(vkQueueSubmit2(mQueue, 1, &si, mImmediateFence), "flush submit")
        && vk_ok(vkWaitForFences(mDevice, 1, &mImmediateFence, VK_TRUE, 10ull * 1000 * 1000 * 1000), "flush wait");

    // the work above has finished: the pool can be reset and the same command buffer recorded again
    vkResetCommandPool(mDevice, f.mPool, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.mCmd, &bi);
    if (f.mTsPool)
    {
        f.mStampTags.clear();       // the reset below wipes the stamps of the first part
        vkCmdResetQueryPool(f.mCmd, f.mTsPool, 0, 2 + MAX_GPU_STAMPS);
        vkCmdWriteTimestamp2(f.mCmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, f.mTsPool, 0);
    }
    return ok;
}

// <FS> doc/vulkan/GPU-LIFETIMES-revised.md, finding 5.1. A frame is finished when something waited for it (mCompletedBelow, set where beginFrame has waited for the slot's last frame) or when its fence says so.
// The test used to be "mFrameNumber - n >= frameCount(): its slot has been reused, which waited for it". That is right while a frame is being recorded, and one frame early BETWEEN two frames: the frame
// number is the next frame's already, but its beginFrame has not waited yet, so the frame frameCount() back still owns its slot and may still be on the GPU. The Hi-Z poll after a presented frame asks
// in exactly that state (after_frame -> hiz_poll): it published verdicts the GPU had not written yet, and the main thread read the slot's old contents as the new serial's.
// The render worker's thread only (as every caller is).
bool Context::frameFinished(U64 n)
{
    if (n >= mFrameNumber)
    {
        return false;       // being recorded, or not begun
    }
    if (n < mCompletedBelow)
    {
        return true;        // waited for
    }
    const U32 count = frameCount();
    if (mFrameNumber - n > count)
    {
        return true;        // (cannot be reached: every such frame is below mCompletedBelow; kept as the old answer for a frame that old)
    }
    // one of the last frameCount() frames, the one being recorded not among them: its slot still holds its own fence
    const bool done = vkGetFenceStatus(mDevice, mFrames[n % count].mFence) == VK_SUCCESS;
    if (done)
    {
        if (n == mCompletedBelow)
        {
            ++mCompletedBelow;      // (only the next one in order: nothing is concluded about the frames before a finished one)
        }
    }
    else if (mFrameNumber - n >= count)
    {
        // the old test called this frame finished
        ++mEarlyFinishedAvoided;
        if (mEarlyFinishedAvoided <= 5 || mEarlyFinishedAvoided % 2000 == 0)
        {
            LL_INFOS("Vulkan") << "frameFinished: frame " << n << " was still on the GPU when asked between two frames (frame number " << mFrameNumber
                               << "; the old test called it finished): " << mEarlyFinishedAvoided << " so far" << LL_ENDL;
        }
    }
    return done;
}

bool Context::isStagingBuffer(VkBuffer buffer) const
{
    for (const Frame& f : mFrames)
    {
        for (const Buffer& b : f.mStagingBlocks)
        {
            if (b.mBuffer == buffer)
            {
                return true;
            }
        }
    }
    return false;
}

StagingAlloc Context::allocStaging(VkDeviceSize size, VkDeviceSize alignment)
{
    Frame& f = mFrames[mFrameSlot];
    if (alignment < 1)
    {
        alignment = 1;
    }

    auto fits = [&]() {
        if (f.mStagingBlocks.empty())
        {
            return false;
        }
        VkDeviceSize start = (f.mStagingUsed + alignment - 1) / alignment * alignment;
        return start + size <= f.mStagingBlocks.back().mSize;
    };

    if (!fits())
    {
        Buffer block;
        const VkDeviceSize block_size = std::max<VkDeviceSize>(size + alignment, 32ull * 1024 * 1024);
        if (!createBuffer(block_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Memory::Upload, block))
        {
            return StagingAlloc();
        }
        f.mStagingBlocks.push_back(block);
        f.mStagingUsed = 0;
    }

    Buffer& block = f.mStagingBlocks.back();
    flight(FL_MISC, 3ull, (unsigned long long)mFrameSlot, (unsigned long long)(uintptr_t)block.mBuffer, ((unsigned long long)f.mStagingBlocks.size() << 48) | (unsigned long long)size);
    VkDeviceSize start = (f.mStagingUsed + alignment - 1) / alignment * alignment;
    f.mStagingUsed = start + size;

    StagingAlloc a;
    a.mBuffer = block.mBuffer;
    a.mOffset = start;
    a.mPtr = (U8*)block.mMapped + start;
    return a;
}

// A thing given up while a frame is being recorded is safe to destroy when that frame has completed. A thing given up BETWEEN frames (the worker handles buffer writes, renames and deletes there) is
// also named by the commands the next frame will record (a pending copy from the range a rename left, the table rows of a buffer that went): it was attached to the frame that had just ended,
// and was destroyed when that slot came round again, while the frame after it could still be on the GPU (a snapshot's frames follow each other without a pause, and the GPU runs two behind).
// Now it waits for the next frame to be recorded and goes with that one.
void Context::destroyLater(std::function<void()> fn)
{
    if (sFrameThread != std::thread::id() && std::this_thread::get_id() != sFrameThread)
    {
        // GPU-LIFETIMES.md finding 5.8: the in-frame list below has no lock, it is safe only for the thread that records the frame (healthy = 0)
        static std::atomic<U64> off_thread{ 0 };
        const U64 n = ++off_thread;
        if (n <= 5 || n % 1000 == 0)
        {
            LL_WARNS("Vulkan") << "destroyLater called from a thread that is not the frame thread (" << n << " so far, in a frame: " << (mInFrame ? "yes" : "no") << ")" << LL_ENDL;
        }
    }
    if (mInFrame)
    {
        mFrames[mFrameSlot].mDeferred.push_back(std::move(fn));
        return;
    }
    std::lock_guard<std::mutex> lock(mDeferredNextMutex);
    mDeferredNext.push_back(std::move(fn));
}

bool Context::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Memory memory, Buffer& out)
{
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size;
    bi.usage = usage;
    if (mCaps.mBufferDeviceAddress)
    {
        bi.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;      // every buffer can be read by address (render packets)
    }
    const uint32_t async_families[2] = { mQueueFamily, (uint32_t)std::max<S32>(mCaps.mComputeFamily, 0) };
    if (mAsyncOn)
    {
        // E2: the compute queue reads and writes what the ray scene builds read and write
        bi.sharingMode = VK_SHARING_MODE_CONCURRENT;
        bi.queueFamilyIndexCount = 2;
        bi.pQueueFamilyIndices = async_families;
    }
    else
    {
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    VmaAllocationCreateInfo ai = {};
    switch (memory)
    {
    case Memory::Upload:
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;
    case Memory::UploadDevice:
        // written by the CPU, read by the GPU every frame: video memory the CPU can map (resizable BAR) when there is room
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        ai.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        ai.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        break;
    case Memory::Readback:
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;
    default:
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        break;
    }

    VmaAllocationInfo info = {};
    out = Buffer();
    if (!vk_ok(vmaCreateBuffer(mAllocator, &bi, &ai, &out.mBuffer, &out.mAllocation, &info), "vmaCreateBuffer"))
    {
        out = Buffer();
        return false;
    }
    out.mMapped = info.pMappedData;
    out.mSize = size;
    if (mCaps.mBufferDeviceAddress)
    {
        VkBufferDeviceAddressInfo address_info = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        address_info.buffer = out.mBuffer;
        out.mAddress = vkGetBufferDeviceAddress(mDevice, &address_info);
        if (out.mAddress)
        {
            std::lock_guard<std::mutex> lock(mDedicatedMutex);
            mDedicated.push_back({ (unsigned long long)out.mAddress, (unsigned long long)size, (unsigned)usage, (unsigned)memory, (unsigned long long)mFrameNumber, 0ull, true });
            flight(FL_BUF_CREATE, (unsigned long long)out.mAddress, (unsigned long long)size, (unsigned long long)usage, (unsigned long long)memory);
        }
    }
    if (memory == Memory::UploadDevice)
    {
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            VkMemoryPropertyFlags flags = 0;
            vmaGetAllocationMemoryProperties(mAllocator, out.mAllocation, &flags);
            LL_INFOS("Vulkan") << "CPU-written GPU buffers live in " << ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? "device-local (video) memory" : "host memory")
                               << " (memory property flags 0x" << std::hex << flags << std::dec << ")" << LL_ENDL;
        }
    }
    return true;
}

void Context::destroyBuffer(Buffer& buffer, bool immediate)
{
    if (!buffer.valid())
    {
        return;
    }
    VkBuffer b = buffer.mBuffer;
    VmaAllocation a = buffer.mAllocation;
    VmaAllocator alloc = mAllocator;
    if (buffer.mAddress)
    {
        flight(FL_BUF_DESTROY, (unsigned long long)buffer.mAddress, (unsigned long long)buffer.mSize);
        std::lock_guard<std::mutex> lock(mDedicatedMutex);
        for (size_t k = mDedicated.size(); k-- > 0; )
        {
            if (mDedicated[k].mAlive && mDedicated[k].mAddress == (unsigned long long)buffer.mAddress)
            {
                mDedicated[k].mAlive = false;
                mDedicated[k].mDestroyed = (unsigned long long)mFrameNumber;
                break;
            }
        }
        if (mDedicated.size() > 6000)
        {
            size_t dead = 0;
            for (const DedicatedBuffer& d : mDedicated) { dead += d.mAlive ? 0 : 1; }
            if (dead > 1000)
            {
                size_t drop = dead - 600;         // the oldest destroyed ones go
                std::vector<DedicatedBuffer> keep;
                keep.reserve(mDedicated.size());
                for (const DedicatedBuffer& d : mDedicated)
                {
                    if (!d.mAlive && drop) { --drop; continue; }
                    keep.push_back(d);
                }
                mDedicated.swap(keep);
            }
        }
    }
    if (immediate)
    {
        vmaDestroyBuffer(alloc, b, a);
    }
    else
    {
        destroyLater([=]() { vmaDestroyBuffer(alloc, b, a); });
    }
    buffer = Buffer();
}

std::string Context::describeDedicated(unsigned long long at)
{
    std::unique_lock<std::mutex> lock(mDedicatedMutex, std::try_to_lock);
    if (!lock.owns_lock())
    {
        return std::string();
    }
    std::string out;
    for (size_t k = mDedicated.size(); k-- > 0; )
    {
        const DedicatedBuffer& d = mDedicated[k];
        if (at + 0x40000 >= d.mAddress && at < d.mAddress + d.mSize + 0x40000)
        {
            out += std::string(out.empty() ? "" : "; ") + (d.mAlive ? "dedicated buffer ALIVE" : "dedicated buffer DESTROYED at frame " + std::to_string(d.mDestroyed)) + ": " + std::to_string(d.mSize) + " bytes at 0x"
                + [&]() { char b[24]; snprintf(b, sizeof(b), "%llx", d.mAddress); return std::string(b); }() + ", offset " + std::to_string((long long)(at - d.mAddress)) + (at >= d.mAddress + d.mSize ? " (PAST THE END)" : "")
                + ", usage 0x" + [&]() { char b[16]; snprintf(b, sizeof(b), "%x", d.mUsage); return std::string(b); }() + ", memory " + std::to_string(d.mMemory) + ", created at frame " + std::to_string(d.mCreated);
        }
    }
    return out;
}

//-----------------------------------------------------------------------------------------------------
// buffer arena
//-----------------------------------------------------------------------------------------------------

namespace
{
// regular blocks (a range bigger than a quarter of one gets a block of its own)
VkDeviceSize arena_block_bytes()
{
    static const VkDeviceSize v = []() {
        char b[16] = {};
        // <FS> native-vk (S7 measurement, tex-pts15 against tex-pts17): 64 MB blocks leave about half the unused space in the acceleration structure blocks (390 MB instead of 800 MB) and keep the used video
        // memory under the texture budget (10.7 GB instead of 11.7 GB: the discard bias 1.1 instead of 4.0); a range over a quarter of a block gets a block of its own, so there are more of those (about 15 a second
        // in a busy scene, 0.07 ms/frame in vmaCreateBuffer at the worst). LL_VK_ARENA_BLOCK_MB=256 is the old size.
        const U32 mb = GetEnvironmentVariableA("LL_VK_ARENA_BLOCK_MB", b, sizeof(b)) > 0 ? (U32)atoi(b) : 64;
        return (VkDeviceSize)std::max<U32>(mb, 16) * 1024 * 1024;
    }();
    return v;
}
}

// <FS> the dedicated acceleration structure blocks come in size classes (1.25 steps from 16 MB) so that one that went empty fits the next range of its class; LL_VK_AS_POOL_MB is what the pool may hold (0: no pool, the
// blocks are made for the range and given back with it, as before)
static VkDeviceSize as_pool_bytes()
{
    static const VkDeviceSize v = []() {
        char b[16] = {};
        const U32 mb = GetEnvironmentVariableA("LL_VK_AS_POOL_MB", b, sizeof(b)) > 0 ? (U32)atoi(b) : 512;
        return (VkDeviceSize)mb << 20;
    }();
    return v;
}

static VkDeviceSize as_class_bytes(VkDeviceSize n)
{
    static const U32 kMB[] = { 16, 20, 25, 32, 40, 50, 64, 80, 100, 128, 160, 200, 256, 320, 400, 512 };
    for (U32 mb : kMB)
    {
        if (n <= ((VkDeviceSize)mb << 20))
        {
            return (VkDeviceSize)mb << 20;
        }
    }
    return (n + 255) / 256 * 256;
}

// The host block gate (doc/vulkan/PLAN-texture-handles.md section 17.15, Aftermath of 2026-10-31): a new host-written block is written once by the GPU (a fill that is submitted without waiting) and nothing is placed in it until that fill
// has finished; arenaPrepareTick keeps a spare block made and filled ahead of need, so a request normally finds a ready block. The sudden device losses were a vertex shader reading a page of a host-written block made one to three
// seconds before that the GPU could not translate (Error_DMA_PageFault), in video and in system memory; the shaders reach these blocks by device address only, so nothing in a frame's commands names the allocation. Stress laps
// (LL_VK_ARENA_BLOCK_MB=16): 1 loss in 3 without the gate, none in 7 with it or with a blocking fill. Normal laps: 2 losses in 2 without, none with. Cost in the default placement: none measured.
// LL_VK_HOST_BLOCK_GATE=0 switches it off. LL_VK_HOST_BLOCK_SPARE (default 1) is how many blocks with at least half their room free are kept; LL_VK_HOST_BLOCK_FILL_KB (default 0 = the whole block) is how much the fill writes.
static bool host_block_gate()
{
    static const bool v = []() { char b[8] = {}; return GetEnvironmentVariableA("LL_VK_HOST_BLOCK_GATE", b, sizeof(b)) == 0 || atoi(b) != 0; }();
    return v;
}

static U32 host_block_spare()
{
    static const U32 v = []() { char b[8] = {}; return GetEnvironmentVariableA("LL_VK_HOST_BLOCK_SPARE", b, sizeof(b)) > 0 ? (U32)std::max(atoi(b), 1) : 1u; }();
    return v;
}

void Context::arenaStartReadyFill(ArenaBlock* block)
{
    block->mReady = false;
    VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pi.queueFamilyIndex = mQueueFamily;
    VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    bool ok = vk_ok(vkCreateCommandPool(mDevice, &pi, nullptr, &block->mReadyPool), "ready fill pool")
        && vk_ok(vkCreateFence(mDevice, &fi, nullptr, &block->mReadyFence), "ready fill fence");
    if (ok)
    {
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = block->mReadyPool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        ok = vk_ok(vkAllocateCommandBuffers(mDevice, &ai, &cmd), "ready fill command buffer");
    }
    if (ok)
    {
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        // LL_VK_HOST_BLOCK_FILL_KB (default 0 = the whole block): how much the fill writes; a command that names the allocation may be all the driver needs, and a whole 64 MB block is a PCIe transfer
        static const VkDeviceSize fill_bytes = []() { char b[16] = {}; return GetEnvironmentVariableA("LL_VK_HOST_BLOCK_FILL_KB", b, sizeof(b)) > 0 ? (VkDeviceSize)std::max(atoi(b), 0) * 1024 : (VkDeviceSize)0; }();
        vkCmdFillBuffer(cmd, block->mBuffer, 0, fill_bytes ? std::min<VkDeviceSize>(fill_bytes, block->mSize) : VK_WHOLE_SIZE, 0u);
        vkEndCommandBuffer(cmd);
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        ok = vk_ok(vkQueueSubmit(mQueue, 1, &si, block->mReadyFence), "ready fill submit");
    }
    if (!ok)
    {
        // (no fill: the block is used as before)
        if (block->mReadyFence) { vkDestroyFence(mDevice, block->mReadyFence, nullptr); block->mReadyFence = VK_NULL_HANDLE; }
        if (block->mReadyPool) { vkDestroyCommandPool(mDevice, block->mReadyPool, nullptr); block->mReadyPool = VK_NULL_HANDLE; }
        block->mReady = true;
    }
}

bool Context::arenaFinishReady(ArenaBlock* block, bool wait)
{
    if (block->mReady)
    {
        return true;
    }
    if (block->mReadyFence)
    {
        const VkResult r = wait ? vkWaitForFences(mDevice, 1, &block->mReadyFence, VK_TRUE, 10ull * 1000 * 1000 * 1000) : vkGetFenceStatus(mDevice, block->mReadyFence);
        if (r != VK_SUCCESS)
        {
            return false;
        }
        vkDestroyFence(mDevice, block->mReadyFence, nullptr);
        block->mReadyFence = VK_NULL_HANDLE;
    }
    if (block->mReadyPool)
    {
        vkDestroyCommandPool(mDevice, block->mReadyPool, nullptr);
        block->mReadyPool = VK_NULL_HANDLE;
    }
    block->mReady = true;
    if (!wait)
    {
        ++mGatePrepared;        // (it was ready before anyone had to wait for it)
    }
    return true;
}

Context::ArenaBlock* Context::arenaNewBlock(VkDeviceSize min_size, ArenaKind kind, bool dedicated)
{
    const bool shared = kind == ArenaKind::Shared, host = kind == ArenaKind::Host, accel = kind == ArenaKind::Accel;
    ArenaBlock* block = new ArenaBlock();
    block->mSize = dedicated ? ((accel && as_pool_bytes()) ? as_class_bytes(min_size) : (min_size + 255) / 256 * 256) : std::max(accel ? std::min<VkDeviceSize>(arena_block_bytes(), 128ull << 20) : arena_block_bytes(), min_size);
    block->mShared = shared;
    block->mHost = host;
    block->mAccel = accel;
    block->mDedicated = dedicated;

    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = block->mSize;
    // vertex / index reads, copies in and out (a renamed range is copied from its old place), shaders reading by address (packets, skinning) and, once the device has it, acceleration structure builds reading the same ranges
    bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (mCaps.mBufferDeviceAddress)
    {
        bi.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    if (mCaps.mRt.mEnabled)
    {
        bi.usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        if (accel)
        {
            bi.usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR;
        }
    }
    const uint32_t families[2] = { mQueueFamily, (uint32_t)std::max<S32>(mCaps.mComputeFamily, 0) };
    if ((shared || mAsyncOn) && mCaps.mComputeFamily >= 0)
    {
        bi.sharingMode = VK_SHARING_MODE_CONCURRENT;
        bi.queueFamilyIndexCount = 2;
        bi.pQueueFamilyIndices = families;
    }
    else
    {
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    VmaAllocationCreateInfo ai = {};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ai.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    if (host)
    {
        // as Memory::UploadDevice: written by the CPU, read by the GPU every frame; video memory the CPU can map (resizable BAR) when there is room
        ai.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        ai.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        // LL_VK_HOST_BLOCK_VRAM=0 (doc/vulkan/PLAN-texture-handles.md section 17.15): the block lives in system memory, the GPU reads it over PCIe. Five sudden device losses on 2026-10-25 faulted on a page-sized
        // invalid read in the first megabyte of a host-written block that was made one to three seconds before; every one of those blocks lived in video memory.
        static const bool host_in_vram = []() { char b[8] = {}; return GetEnvironmentVariableA("LL_VK_HOST_BLOCK_VRAM", b, sizeof(b)) == 0 || atoi(b) != 0; }();
        if (host_in_vram)
        {
            ai.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        }
        else
        {
            ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        }
    }
    VmaAllocationInfo block_info = {};
    LARGE_INTEGER t_start, t_end, t_freq;
    QueryPerformanceFrequency(&t_freq);
    QueryPerformanceCounter(&t_start);
    if (!vk_ok(vmaCreateBuffer(mAllocator, &bi, &ai, &block->mBuffer, &block->mAllocation, &block_info), "arena block"))
    {
        delete block;
        return nullptr;
    }
    QueryPerformanceCounter(&t_end);
    {
        // <FS> LL_VK_SLOW_ALLOC_MS (default 20, 0 = off): a block that takes this long to create stalls the thread that creates it (the worker, in the middle of a frame): the GPU idles meanwhile and the frame's
        // passes read as hundreds of ms long (2026-11-01: three frames of 270 ms, the GPU at 30-50%, 15 blocks created in 8 s when the camera turned to a side that had no structures yet)
        static const double slow_ms = (double)(getenv("LL_VK_SLOW_ALLOC_MS") ? atoi(getenv("LL_VK_SLOW_ALLOC_MS")) : 20);
        const double took_ms = 1000.0 * (double)(t_end.QuadPart - t_start.QuadPart) / (double)t_freq.QuadPart;
        if (slow_ms > 0.0 && took_ms >= slow_ms)
        {
            LL_WARNS("Vulkan") << "a " << (block->mSize >> 20) << " MB buffer arena block took " << took_ms << " ms to create (" << (accel ? "acceleration structure" : "buffer") << (dedicated ? ", dedicated" : "") << ", frame " << mFrameNumber << ")" << LL_ENDL;
        }
    }
    block->mCreatedFrame = mFrameNumber;
    if (accel)
    {
        ++mArenaCounters.mAccelBlocksCreated;
        mArenaCounters.mAccelCreatedBytes += block->mSize;
        mArenaCounters.mAccelCreateMicros += (U64)((t_end.QuadPart - t_start.QuadPart) * 1000000 / t_freq.QuadPart);
        if (dedicated)
        {
            ++mArenaCounters.mAccelDedicatedCreated;
        }
    }
    block->mMapped = block_info.pMappedData;
    VmaVirtualBlockCreateInfo vi = {};
    vi.size = block->mSize;
    if (vmaCreateVirtualBlock(&vi, &block->mVirtual) != VK_SUCCESS)
    {
        vmaDestroyBuffer(mAllocator, block->mBuffer, block->mAllocation);
        delete block;
        return nullptr;
    }
    if (mCaps.mBufferDeviceAddress)
    {
        VkBufferDeviceAddressInfo address_info = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        address_info.buffer = block->mBuffer;
        block->mAddress = vkGetBufferDeviceAddress(mDevice, &address_info);
    }
    if (host)
    {
        if (host_block_gate())
        {
            arenaStartReadyFill(block);
        }
    }
    bool placed = false;
    for (ArenaBlock*& slot : mArenaBlocks)
    {
        if (!slot) { slot = block; placed = true; break; }
    }
    if (!placed)
    {
        mArenaBlocks.push_back(block);
    }
    ++mArenaCounters.mBlocksCreated;
    flight(FL_MISC, 1ull, (unsigned long long)block->mAddress, (unsigned long long)block->mSize, ((unsigned long long)(accel ? 1 : 0) << 2) | ((unsigned long long)(host ? 1 : 0) << 1) | (dedicated ? 1ull : 0ull));
    if (host)
    {
        VkMemoryPropertyFlags flags = 0;
        vmaGetAllocationMemoryProperties(mAllocator, block->mAllocation, &flags);
        LL_INFOS("Vulkan") << "buffer arena: host-written block lives in " << ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? "device-local (video) memory" : "host memory") << LL_ENDL;
    }
    LL_INFOS("Vulkan") << "buffer arena: new " << (dedicated ? "dedicated " : "") << (shared ? "shared " : "") << (host ? "host-written " : "") << (accel ? "acceleration structure " : "") << "block of " << (block->mSize >> 20) << " MB ("
                       << (U32)std::count_if(mArenaBlocks.begin(), mArenaBlocks.end(), [](ArenaBlock* b) { return b != nullptr; }) << " blocks now)" << LL_ENDL;
    return block;
}

void Context::arenaDestroyBlock(ArenaBlock* block)
{
    arenaFinishReady(block, true);
    for (ArenaBlock*& slot : mArenaBlocks)
    {
        if (slot == block) slot = nullptr;
    }
    flight(FL_MISC, 2ull, (unsigned long long)block->mAddress, (unsigned long long)block->mSize, ((unsigned long long)(block->mAccel ? 1 : 0) << 2) | ((unsigned long long)(block->mHost ? 1 : 0) << 1) | (block->mDedicated ? 1ull : 0ull));
    vmaDestroyVirtualBlock(block->mVirtual);
    vmaDestroyBuffer(mAllocator, block->mBuffer, block->mAllocation);
    ++mArenaCounters.mBlocksReleased;
    if (block->mAccel)
    {
        ++mArenaCounters.mAccelBlocksReleased;
        if (block->mDedicated)
        {
            ++mArenaCounters.mAccelDedicatedReleased;
            mArenaCounters.mAccelDedicatedLifeFrames += mFrameNumber - block->mCreatedFrame;
        }
    }
    delete block;
}

bool Context::arenaAlloc(VkDeviceSize size, VkDeviceSize alignment, ArenaKind kind, ArenaRef& out)
{
    out = ArenaRef();
    if (size == 0 || mAllocator == nullptr)
    {
        return false;
    }
    alignment = std::max<VkDeviceSize>(alignment, 4);
    if (kind == ArenaKind::Shared && mCaps.mComputeFamily < 0)
    {
        kind = ArenaKind::Device;
    }
    const bool shared = kind == ArenaKind::Shared, host = kind == ArenaKind::Host, accel = kind == ArenaKind::Accel;
    std::lock_guard<std::mutex> lock(mArenaMutex);
    const bool dedicated = size > arena_block_bytes() / 4;
    if (accel)
    {
        ++mArenaCounters.mAccelAllocs;
        mArenaCounters.mAccelAllocBytes += size;
        const U32 mb = (U32)(size >> 20);
        ++mArenaCounters.mAccelBucket[mb < 1 ? 0 : mb < 8 ? 1 : mb < 32 ? 2 : mb < 64 ? 3 : mb < 128 ? 4 : 5];
    }
    auto place = [&](ArenaBlock* b) {
        VmaVirtualAllocationCreateInfo ci = {};
        ci.size = size;
        ci.alignment = alignment;
        VmaVirtualAllocation alloc = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        if (vmaVirtualAllocate(b->mVirtual, &ci, &alloc, &offset) != VK_SUCCESS)
        {
            return false;
        }
        ++b->mAllocs;
        b->mUsed += size;
        out.mBuffer = b->mBuffer;
        out.mOffset = offset;
        out.mSize = size;
        out.mAddress = b->mAddress ? b->mAddress + offset : 0;
        out.mMapped = b->mMapped ? (U8*)b->mMapped + offset : nullptr;
        out.mAlloc = alloc;
        flight(FL_ARENA_ALLOC, (unsigned long long)out.mAddress, (unsigned long long)size, ((unsigned long long)kind << 32) | (unsigned long long)offset, (unsigned long long)alignment);
        for (size_t i = 0; i < mArenaBlocks.size(); ++i)
        {
            if (mArenaBlocks[i] == b) { out.mBlock = (U32)i; break; }
        }
        return true;
    };
    if (!dedicated)
    {
        // LL_VK_ARENA_FULLEST (default on, 0 = the first block with room, as before): the fullest block that still has room for the range goes first. With first fit the old blocks fill unevenly and stay
        // half empty for good (the club run of 2026-10-25 had 48 acceleration structure blocks with 1.0 GB in use of 2.6 GB); filling the fullest first lets the sparse ones drain, and an empty block is given back
        static const bool fullest = []() { char b[8] = {}; return GetEnvironmentVariableA("LL_VK_ARENA_FULLEST", b, sizeof(b)) == 0 || atoi(b) != 0; }();
        if (fullest)
        {
            arenaTrimPool();
            ArenaBlock* best = nullptr;
            VkDeviceSize best_free = ~0ull;
            for (ArenaBlock* b : mArenaBlocks)
            {
                if (b && !b->mDedicated && !b->mPooled && b->mShared == shared && b->mHost == host && b->mAccel == accel && arenaFinishReady(b, false))
                {
                    const VkDeviceSize free_bytes = b->mSize > b->mUsed ? b->mSize - b->mUsed : 0;
                    if (free_bytes >= size + alignment && free_bytes < best_free)
                    {
                        best = b;
                        best_free = free_bytes;
                    }
                }
            }
            if (best && place(best))
            {
                return true;
            }
        }
        for (ArenaBlock* b : mArenaBlocks)
        {
            if (b && !b->mDedicated && b->mShared == shared && b->mHost == host && b->mAccel == accel && arenaFinishReady(b, false) && place(b))
            {
                return true;
            }
        }
        if (host)
        {
            // a block that is still being filled and has room: wait for it (a few ms) rather than make another one
            for (ArenaBlock* b : mArenaBlocks)
            {
                if (b && !b->mReady && !b->mDedicated && b->mHost && b->mSize > b->mUsed + size + alignment)
                {
                    LARGE_INTEGER w0, w1, wf;
                    QueryPerformanceFrequency(&wf);
                    QueryPerformanceCounter(&w0);
                    const bool done = arenaFinishReady(b, true);
                    QueryPerformanceCounter(&w1);
                    ++mGateLate;
                    mGateLateMs += (double)(w1.QuadPart - w0.QuadPart) * 1000.0 / (double)wf.QuadPart;
                    if (done && place(b))
                    {
                        return true;
                    }
                }
            }
        }
    }
    if (dedicated && accel && as_pool_bytes())
    {
        // <FS> a dedicated acceleration structure block that went empty and waits in the pool: the smallest one that holds the range and is not more than 1.5 times its class
        const VkDeviceSize need = size + alignment;
        const VkDeviceSize limit = as_class_bytes(need) * 3 / 2;
        ArenaBlock* best = nullptr;
        for (ArenaBlock* b : mArenaBlocks)
        {
            if (b && b->mPooled && b->mSize >= need && b->mSize <= limit && (!best || b->mSize < best->mSize))
            {
                best = b;
            }
        }
        if (best)
        {
            best->mPooled = false;
            mPoolBytes -= best->mSize;
            if (place(best))
            {
                ++mArenaCounters.mAccelPoolHits;
                return true;
            }
            best->mPooled = true;                   // (it could not hold it after all: back in the pool)
            mPoolBytes += best->mSize;
        }
        ++mArenaCounters.mAccelPoolMisses;
    }
    ArenaBlock* fresh = arenaNewBlock(size + alignment, kind, dedicated);
    if (fresh && !fresh->mReady)
    {
        // made because the request could not wait for a spare: the fill is waited for here, the case the spare is there to avoid
        LARGE_INTEGER w0, w1, wf;
        QueryPerformanceFrequency(&wf);
        QueryPerformanceCounter(&w0);
        arenaFinishReady(fresh, true);
        QueryPerformanceCounter(&w1);
        ++mGateLate;
        mGateLateMs += (double)(w1.QuadPart - w0.QuadPart) * 1000.0 / (double)wf.QuadPart;
    }
    if (!fresh || !place(fresh))
    {
        LL_WARNS("Vulkan") << "buffer arena: no room for " << size << " bytes" << LL_ENDL;
        return false;
    }
    return true;
}

void Context::arenaTrimPool()
{
    if (mFrameNumber - mPoolLastTrim < 60)
    {
        return;
    }
    mPoolLastTrim = mFrameNumber;
    for (size_t i = 0; i < mArenaBlocks.size(); ++i)
    {
        ArenaBlock* b = mArenaBlocks[i];
        if (b && b->mPooled && mFrameNumber - b->mIdleSince > 900)         // about 15 seconds without a taker
        {
            mPoolBytes -= b->mSize;
            ++mArenaCounters.mAccelPoolTrimmed;
            arenaDestroyBlock(b);
        }
        else if (b && !b->mPooled && !b->mDedicated && b->mAllocs == 0 && mFrameNumber - b->mEmptyFrame > 600)
        {
            // an empty regular block that nothing has taken for about 10 seconds: given back, unless it is the one spare of its kind (the next allocation finds it)
            bool other_empty = false;
            for (ArenaBlock* o : mArenaBlocks)
            {
                if (o && o != b && !o->mDedicated && !o->mPooled && o->mShared == b->mShared && o->mHost == b->mHost && o->mAccel == b->mAccel && o->mAllocs == 0)
                {
                    other_empty = true;
                    break;
                }
            }
            if (other_empty)
            {
                ++mArenaCounters.mIdleTrimmed;
                arenaDestroyBlock(b);
            }
        }
    }
}

void Context::arenaFreeNow(ArenaBlock* block, VmaVirtualAllocation alloc)
{
    flight(FL_ARENA_FREE_DONE, (unsigned long long)(uintptr_t)block, (unsigned long long)(uintptr_t)alloc, block ? (unsigned long long)block->mAddress : 0ull);
    std::lock_guard<std::mutex> lock(mArenaMutex);
    {
        VmaVirtualAllocationInfo info = {};
        vmaGetVirtualAllocationInfo(block->mVirtual, alloc, &info);
        block->mUsed = block->mUsed > info.size ? block->mUsed - info.size : 0;
    }
    vmaVirtualFree(block->mVirtual, alloc);
    if (--block->mAllocs != 0)
    {
        return;
    }
    block->mEmptyFrame = mFrameNumber;
    if (block->mDedicated && block->mAccel && as_pool_bytes() && mPoolBytes + block->mSize <= as_pool_bytes())
    {
        // <FS> kept for the next big range of its size class (the video memory manager is not asked for 1 GB a second and given it back)
        block->mPooled = true;
        block->mIdleSince = mFrameNumber;
        mPoolBytes += block->mSize;
        arenaTrimPool();
        return;
    }
    // an empty block goes when it is a block of its own, or when another empty one of its kind is already waiting for the next allocation
    bool release = block->mDedicated;
    for (ArenaBlock* b : mArenaBlocks)
    {
        if (b && b != block && !b->mDedicated && b->mShared == block->mShared && b->mHost == block->mHost && b->mAccel == block->mAccel && b->mAllocs == 0)
        {
            release = true;
            break;
        }
    }
    if (release)
    {
        arenaDestroyBlock(block);
    }
}

void Context::arenaFree(ArenaRef& ref)
{
    if (!ref.valid())
    {
        return;
    }
    flight(FL_ARENA_FREE_REQ, (unsigned long long)ref.mAddress, (unsigned long long)ref.mSize, (unsigned long long)ref.mBlock, (unsigned long long)ref.mOffset);
    ArenaBlock* block = nullptr;
    {
        std::lock_guard<std::mutex> lock(mArenaMutex);
        block = ref.mBlock < mArenaBlocks.size() ? mArenaBlocks[ref.mBlock] : nullptr;
    }
    const VmaVirtualAllocation alloc = ref.mAlloc;
    ref = ArenaRef();
    if (block)
    {
        destroyLater([this, block, alloc]() { arenaFreeNow(block, alloc); });
    }
}

ArenaStats Context::arenaStats()
{
    std::lock_guard<std::mutex> lock(mArenaMutex);
    ArenaStats s = mArenaCounters;
    s.mBlocks = s.mAllocs = s.mHostBlocks = s.mHostAllocs = 0;
    s.mHostBlockBytes = s.mHostUsedBytes = 0;
    s.mBlockBytes = s.mUsedBytes = s.mLargestFree = 0;
    for (ArenaBlock* b : mArenaBlocks)
    {
        if (b && b->mAccel)
        {
            VmaDetailedStatistics ast = {};
            vmaCalculateVirtualBlockStatistics(b->mVirtual, &ast);
            ++s.mAccelBlocks;
            s.mAccelBlockBytes += b->mSize;
            s.mAccelUsedBytes += ast.statistics.allocationBytes;
            s.mAccelLargestFree = std::max<U64>(s.mAccelLargestFree, ast.unusedRangeSizeMax);
            if (b->mDedicated)
            {
                ++s.mAccelDedicatedAlive;
                s.mAccelDedicatedBytes += b->mSize;
            }
            if (b->mPooled)
            {
                ++s.mAccelPooled;
                s.mAccelPooledBytes += b->mSize;
            }
            continue;       // (the rest of the arena report leaves these out: acceleration structure storage has its own report in Rt)
        }
        if (!b) continue;
        VmaDetailedStatistics st = {};
        vmaCalculateVirtualBlockStatistics(b->mVirtual, &st);
        if (b->mHost)
        {
            ++s.mHostBlocks;
            s.mHostAllocs += (U32)st.statistics.allocationCount;
            s.mHostBlockBytes += b->mSize;
            s.mHostUsedBytes += st.statistics.allocationBytes;
            continue;
        }
        ++s.mBlocks;
        s.mAllocs += (U32)st.statistics.allocationCount;
        s.mBlockBytes += b->mSize;
        s.mUsedBytes += st.statistics.allocationBytes;
        s.mLargestFree = std::max<U64>(s.mLargestFree, st.unusedRangeSizeMax);
    }
    return s;
}

// <FS> native-vk (S7 census, no behaviour change): the question is whether compacting the acceleration structure blocks (rebuilding the structures of the sparsest blocks into the free space of the others, so that those blocks can be
// given back) would return enough memory to be worth building. Regular (shared) blocks: fill histogram, and the sparsest-first drain: how many blocks could go, how many MB and how many structures would have to be rebuilt
// (an upper bound on the gain: it ignores that a moved structure must fit a free range, not just free bytes in total). Dedicated blocks hold one structure each: only their size-class slack is counted.
std::string Context::arenaAccelCensus()
{
    std::lock_guard<std::mutex> lock(mArenaMutex);
    struct Row { U64 mSize, mUsed; U32 mAllocs; };
    std::vector<Row> regular;
    U64 ded_size = 0, ded_used = 0, pooled_size = 0;
    U32 ded_n = 0, pooled_n = 0;
    for (ArenaBlock* b : mArenaBlocks)
    {
        if (!b || !b->mAccel)
        {
            continue;
        }
        if (b->mPooled)
        {
            ++pooled_n;
            pooled_size += b->mSize;
        }
        else if (b->mDedicated)
        {
            ++ded_n;
            ded_size += b->mSize;
            ded_used += b->mUsed;
        }
        else
        {
            regular.push_back({ (U64)b->mSize, b->mUsed, b->mAllocs });
        }
    }
    std::sort(regular.begin(), regular.end(), [](const Row& a, const Row& c) { return a.mUsed < c.mUsed; });
    U64 reg_size = 0, reg_used = 0, reg_allocs = 0;
    U32 hist[5] = {};       // empty, <25 %, <50 %, <75 %, 75 % and more
    for (const Row& r : regular)
    {
        reg_size += r.mSize;
        reg_used += r.mUsed;
        reg_allocs += r.mAllocs;
        const double f = r.mSize ? (double)r.mUsed / (double)r.mSize : 0.0;
        ++hist[r.mUsed == 0 ? 0 : f < 0.25 ? 1 : f < 0.5 ? 2 : f < 0.75 ? 3 : 4];
    }
    // the drain: take blocks sparsest first while what they hold fits in the free bytes of the blocks that stay
    U32 freed = 0, moved_allocs = 0;
    U64 moved = 0, freed_mb = 0;
    for (size_t k = 0; k < regular.size(); ++k)
    {
        U64 free_rest = 0;
        for (size_t j = k + 1; j < regular.size(); ++j)
        {
            free_rest += regular[j].mSize - regular[j].mUsed;
        }
        if (moved + regular[k].mUsed > free_rest * 9 / 10)      // (keep 10 % back for fragmentation and for new ranges)
        {
            break;
        }
        moved += regular[k].mUsed;
        moved_allocs += regular[k].mAllocs;
        freed_mb += regular[k].mSize;
        ++freed;
    }
    const U64 packed_blocks = regular.empty() ? 0 : (reg_used * 11 / 10 + (64ull << 20) - 1) / (64ull << 20);
    std::string out = "AS census: shared blocks " + std::to_string(regular.size()) + " (" + std::to_string(reg_size >> 20) + " MB, " + std::to_string(reg_used >> 20) + " MB in use, " + std::to_string(reg_allocs) + " structures)"
        + " fill: empty " + std::to_string(hist[0]) + ", <25% " + std::to_string(hist[1]) + ", <50% " + std::to_string(hist[2]) + ", <75% " + std::to_string(hist[3]) + ", more " + std::to_string(hist[4])
        + " | draining the sparsest: " + std::to_string(freed) + " blocks (" + std::to_string(freed_mb >> 20) + " MB) could go, moving " + std::to_string(moved >> 20) + " MB = " + std::to_string(moved_allocs) + " structures to rebuild"
        + "; fully packed with 10% slack it would be " + std::to_string(packed_blocks) + " blocks (" + std::to_string(packed_blocks * 64) + " MB)"
        + " | dedicated blocks " + std::to_string(ded_n) + " (" + std::to_string(ded_size >> 20) + " MB, " + std::to_string(ded_used >> 20) + " MB in use) | pooled empty " + std::to_string(pooled_n) + " (" + std::to_string(pooled_size >> 20) + " MB)";
    return out;
}

// once a frame on the render worker (LL_VK_HOST_BLOCK_GATE): finished fills make their blocks ready, and when fewer than LL_VK_HOST_BLOCK_SPARE host blocks have half their room free (ready or being filled) another is made and
// its fill submitted, so the next request finds a block the GPU has already written
void Context::arenaPrepareTick()
{
    if (!host_block_gate() || mAllocator == nullptr)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(mArenaMutex);
    U32 spare = 0;
    for (ArenaBlock* b : mArenaBlocks)
    {
        if (!b || !b->mHost || b->mDedicated)
        {
            continue;
        }
        if (!b->mReady)
        {
            arenaFinishReady(b, false);
        }
        if (b->mSize > b->mUsed && (b->mSize - b->mUsed) * 2 >= b->mSize)
        {
            ++spare;
        }
    }
    if (spare < host_block_spare())
    {
        arenaNewBlock(arena_block_bytes(), ArenaKind::Host, false);
    }
    static U32 ticks = 0;
    if (++ticks % 1800 == 0)
    {
        if (mGatePrepared || mGateLate)
        {
            LL_INFOS("Vulkan") << "buffer arena: host block gate, over the last 1800 frames: " << mGatePrepared << " blocks ready in time, " << mGateLate << " waited for (" << mGateLateMs << " ms in all)" << LL_ENDL;
        }
        mGatePrepared = mGateLate = 0;
        mGateLateMs = 0.0;
    }
}

void Context::arenaShutdown()
{
    std::lock_guard<std::mutex> lock(mArenaMutex);
    for (ArenaBlock* b : mArenaBlocks)
    {
        if (b)
        {
            arenaFinishReady(b, true);
            vmaDestroyVirtualBlock(b->mVirtual);
            vmaDestroyBuffer(mAllocator, b->mBuffer, b->mAllocation);
            delete b;
        }
    }
    mArenaBlocks.clear();
}

bool Context::createImage2D(U32 width, U32 height, U32 mips, U32 layers, VkFormat format, VkImageUsageFlags usage, Image& out)
{
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = { width, height, 1 };
    ii.mipLevels = mips;
    ii.arrayLayers = layers;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo ai = {};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    out = Image();
    if (!vk_ok(vmaCreateImage(mAllocator, &ii, &ai, &out.mImage, &out.mAllocation, nullptr), "vmaCreateImage"))
    {
        out = Image();
        return false;
    }
    {
        char tag[96];
        snprintf(tag, sizeof(tag), "target or image %ux%u fmt %u mips %u layers %u usage 0x%x", width, height, (unsigned)format, mips, layers, (unsigned)usage);
        noteImageTag((U64)out.mImage, tag);
    }
    out.mFormat = format;
    out.mAspect = aspectForFormat(format);
    out.mExtent = { width, height, 1 };
    out.mMipLevels = mips;
    out.mLayers = layers;
    out.mLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    const VkImageUsageFlags view_usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
        | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    if (usage & view_usage)
    {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = out.mImage;
        vi.viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = { out.mAspect, 0, mips, 0, layers };
        if (!vk_ok(vkCreateImageView(mDevice, &vi, nullptr, &out.mView), "vkCreateImageView (image)"))
        {
            vmaDestroyImage(mAllocator, out.mImage, out.mAllocation);
            out = Image();
            return false;
        }
    }
    return true;
}

void Context::noteImageTag(U64 image_handle, const char* text)
{
    std::lock_guard<std::mutex> lock(sImgTagMutex);
    if (sImgTags.empty())
    {
        sImgTags.resize(IMG_TAG_RING);
    }
    ImageTag& t = sImgTags[sImgTagCount++ % IMG_TAG_RING];
    t.mHandle = image_handle;
    t.mFrame = mFrameNumber;
    snprintf(t.mText, sizeof(t.mText), "%s", text);
}

void Context::noteDestroyedView(U64 view_id)
{
    if (view_id)
    {
        const U64 i = mDestroyedCount.fetch_add(1) % DESTROYED_RING;
        mDestroyedIds[i] = view_id;
        mDestroyedFrames[i] = mFrameNumber;
    }
}

bool Context::destroyedViewFrame(U64 view_id, U64& frame) const
{
    for (size_t i = 0; i < DESTROYED_RING; ++i)
    {
        if (mDestroyedIds[i] == view_id)
        {
            frame = mDestroyedFrames[i];
            return true;
        }
    }
    return false;
}

void Context::destroyImage(Image& image)
{
    if (!image.valid())
    {
        return;
    }
    if (mImageDestroyHook)
    {
        mImageDestroyHook(image.mImage);        // <FS> uploads the renderer still holds for this image are dropped
    }
    noteDestroyedView(image.mViewId);
    forgetView(image.mView);
    VkImage i = image.mImage;
    VkImageView v = image.mView;
    VmaAllocation a = image.mAllocation;
    VmaAllocator alloc = mAllocator;
    VkDevice dev = mDevice;
    const U64 view_id = image.mViewId;
    std::function<void(U64)> release_hook = mViewReleaseHook;
    std::function<void()> fn = [=]() {
        if (release_hook && view_id)
        {
            release_hook(view_id);          // a handle that still shows this view shows the placeholder from now on (never memory that is about to be freed)
        }
        if (v) vkDestroyImageView(dev, v, nullptr);
        if (a) vmaDestroyImage(alloc, i, a);
    };
    destroyLater(std::move(fn));
    image = Image();
}

// Exercises the allocator and the transfer path end to end: buffer -> device buffer -> readback, and
// buffer -> image -> readback, comparing the bytes that come back.
bool Context::immediateSubmit(const std::function<void(VkCommandBuffer)>& record)
{
    if (!mImmediatePool)
    {
        VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = mQueueFamily;
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (!vk_ok(vkCreateCommandPool(mDevice, &pi, nullptr, &mImmediatePool), "immediate pool")
            || !vk_ok(vkCreateFence(mDevice, &fi, nullptr, &mImmediateFence), "immediate fence"))
        {
            return false;
        }
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = mImmediatePool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (!vk_ok(vkAllocateCommandBuffers(mDevice, &ai, &mImmediateCmd), "immediate command buffer"))
        {
            return false;
        }
    }

    vkResetCommandBuffer(mImmediateCmd, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(mImmediateCmd, &bi);
    record(mImmediateCmd);
    vkEndCommandBuffer(mImmediateCmd);

    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &mImmediateCmd;
    vkResetFences(mDevice, 1, &mImmediateFence);
    return vk_ok(vkQueueSubmit(mQueue, 1, &si, mImmediateFence), "immediate submit")
        && vk_ok(vkWaitForFences(mDevice, 1, &mImmediateFence, VK_TRUE, 10ull * 1000 * 1000 * 1000), "immediate wait");
}

bool Context::selfTest()
{
    const U32 W = 64, H = 64;
    const VkDeviceSize image_bytes = W * H * 4;
    const VkDeviceSize buffer_bytes = 1024 * 1024;

    Buffer upload, device, readback;
    Image image;
    bool ok = createBuffer(buffer_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Memory::Upload, upload)
        && createBuffer(buffer_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, Memory::Device, device)
        && createBuffer(buffer_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, Memory::Readback, readback)
        && createImage2D(W, H, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, image);

    VkCommandPool pool = VK_NULL_HANDLE;
    if (ok)
    {
        U32* src = (U32*)upload.mMapped;
        for (U32 i = 0; i < buffer_bytes / 4; ++i)
        {
            src[i] = 0x9E3779B1u * (i + 1);
        }
        memset(readback.mMapped, 0, (size_t)buffer_bytes);

        VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pi.queueFamilyIndex = mQueueFamily;
        ok = vk_ok(vkCreateCommandPool(mDevice, &pi, nullptr, &pool), "self test pool");
    }

    if (ok)
    {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(mDevice, &ai, &cmd);

        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);

        auto memory_barrier = [&]() {
            VkMemoryBarrier2 mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
            mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
            VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers = &mb;
            vkCmdPipelineBarrier2(cmd, &dep);
        };

        // buffer round trip: upload [0, 512K) -> device -> readback [0, 512K)
        VkBufferCopy region = { 0, 0, buffer_bytes / 2 };
        vkCmdCopyBuffer(cmd, upload.mBuffer, device.mBuffer, 1, &region);
        memory_barrier();
        vkCmdCopyBuffer(cmd, device.mBuffer, readback.mBuffer, 1, &region);

        // image round trip: upload [512K, 512K + 16K) -> image -> readback [512K, ...)
        imageBarrier(cmd, image.mImage, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy up = {};
        up.bufferOffset = buffer_bytes / 2;
        up.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        up.imageExtent = { W, H, 1 };
        vkCmdCopyBufferToImage(cmd, upload.mBuffer, image.mImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &up);
        imageBarrier(cmd, image.mImage, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy down = up;
        vkCmdCopyImageToBuffer(cmd, image.mImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.mBuffer, 1, &down);

        vkEndCommandBuffer(cmd);

        VkFence fence = VK_NULL_HANDLE;
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        vkCreateFence(mDevice, &fi, nullptr, &fence);
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        ok = vk_ok(vkQueueSubmit(mQueue, 1, &si, fence), "self test submit")
            && vk_ok(vkWaitForFences(mDevice, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000), "self test wait");
        vkDestroyFence(mDevice, fence, nullptr);
    }

    if (ok)
    {
        const U8* sent = (const U8*)upload.mMapped;
        const U8* got = (const U8*)readback.mMapped;
        const bool buffer_ok = memcmp(sent, got, (size_t)(buffer_bytes / 2)) == 0;
        const bool image_ok = memcmp(sent + buffer_bytes / 2, got + buffer_bytes / 2, (size_t)image_bytes) == 0;
        LL_INFOS("Vulkan") << "self test: buffer round trip " << (buffer_ok ? "OK" : "FAILED") << ", image round trip "
                           << (image_ok ? "OK" : "FAILED") << LL_ENDL;
        ok = buffer_ok && image_ok;
    }

    if (pool) vkDestroyCommandPool(mDevice, pool, nullptr);
    if (upload.valid()) vmaDestroyBuffer(mAllocator, upload.mBuffer, upload.mAllocation);
    if (device.valid()) vmaDestroyBuffer(mAllocator, device.mBuffer, device.mAllocation);
    if (readback.valid()) vmaDestroyBuffer(mAllocator, readback.mBuffer, readback.mAllocation);
    if (image.valid()) vmaDestroyImage(mAllocator, image.mImage, image.mAllocation);
    return ok;
}

// C6: the viewer sorts GPUs into classes by memory bandwidth (LLFeatureManager::loadGPUClass; thresholds 48 / 96 / 192 / 384 GB/s times the CPU bias). The GL
// benchmark that used to feed it is gone, so this one measures what the device really does: large device-local copies, bytes read plus bytes written over the
// wall time of the submit. The first run wakes the memory clock up, the best of the following runs counts.
void Context::measureBandwidth()
{
    const U64 vram = deviceLocalBytes();
    VkDeviceSize chunk = 256ull * 1024 * 1024;
    while (chunk > 16ull * 1024 * 1024 && (vram == 0 || chunk * 8 > vram))
    {
        chunk /= 2;
    }
    Buffer a, b;
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (!createBuffer(chunk, usage, Memory::Device, a) || !createBuffer(chunk, usage, Memory::Device, b))
    {
        LL_WARNS("Vulkan") << "bandwidth measurement skipped: no memory for the test buffers" << LL_ENDL;
        if (a.valid()) vmaDestroyBuffer(mAllocator, a.mBuffer, a.mAllocation);
        return;
    }

    auto barrier = [](VkCommandBuffer cmd) {
        VkMemoryBarrier2 mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
        VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &dep);
    };
    const U32 copies_per_run = 8;
    auto run = [&]() -> double {
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        const bool ok = immediateSubmit([&](VkCommandBuffer cmd) {
            const VkBufferCopy region = { 0, 0, chunk };
            for (U32 i = 0; i < copies_per_run; ++i)
            {
                vkCmdCopyBuffer(cmd, (i & 1) ? b.mBuffer : a.mBuffer, (i & 1) ? a.mBuffer : b.mBuffer, 1, &region);
                barrier(cmd);
            }
        });
        QueryPerformanceCounter(&t1);
        const double seconds = (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart;
        return ok && seconds > 0.0 ? (double)copies_per_run * 2.0 * (double)chunk / seconds / 1.0e9 : 0.0;
    };

    // the buffers start out undefined; write them once so the copies read real data
    bool ok = immediateSubmit([&](VkCommandBuffer cmd) { vkCmdFillBuffer(cmd, a.mBuffer, 0, chunk, 0x3F800000u); vkCmdFillBuffer(cmd, b.mBuffer, 0, chunk, 0u); barrier(cmd); });
    double best = 0.0, first = 0.0;
    for (U32 i = 0; ok && i < 4; ++i)
    {
        const double gbps = run();
        if (i == 0) first = gbps;
        else best = std::max(best, gbps);
    }
    vmaDestroyBuffer(mAllocator, a.mBuffer, a.mAllocation);
    vmaDestroyBuffer(mAllocator, b.mBuffer, b.mAllocation);
    if (!ok || best <= 0.0)
    {
        LL_WARNS("Vulkan") << "bandwidth measurement failed" << LL_ENDL;
        return;
    }
    // A copy reaches about 85% of what the memory can do (RTX 5060 Ti: 379.7 measured, 448 in the data sheet), and the class thresholds are in peak terms
    // (384 GB/s is the Class 5 edge), so the raw figure would put a card that is Class 5 on paper into Class 4. Scaled to the estimated peak.
    const double COPY_EFFICIENCY = 0.85;
    mCaps.mMemoryBandwidthGBps = (F32)(best / COPY_EFFICIENCY);
    LL_INFOS("Vulkan") << "memory bandwidth (device-local copy, read + write): " << best << " GB/s measured (first run " << first << ", buffers " << (chunk >> 20) << " MB each), "
                       << mCaps.mMemoryBandwidthGBps << " GB/s estimated peak (copy efficiency " << COPY_EFFICIENCY << ")" << LL_ENDL;
}

//-----------------------------------------------------------------------------------------------------
// Swapchain
//-----------------------------------------------------------------------------------------------------

void Swapchain::destroy()
{
    Context& ctx = Context::instance();
    VkDevice dev = ctx.getDevice();
    if (dev == VK_NULL_HANDLE)
    {
        return;
    }
    vkDeviceWaitIdle(dev);
    for (VkSemaphore s : mRenderDone)
    {
        vkDestroySemaphore(dev, s, nullptr);
    }
    mRenderDone.clear();
    for (VkImageView v : mViews)
    {
        vkDestroyImageView(dev, v, nullptr);
    }
    mViews.clear();
    mImages.clear();
    if (mSwapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(dev, mSwapchain, nullptr);
        mSwapchain = VK_NULL_HANDLE;
    }
}

namespace
{
std::atomic<int> gPresentModePref{ 0 };
std::atomic<bool> gVsyncPref{ true };

VkPresentModeKHR wanted_present_mode()
{
    static const char* env = getenv("LL_VK_PRESENT");        // experiment: fifo, mailbox, immediate, relaxed; wins over the settings
    if (env)
    {
        return strcmp(env, "mailbox") == 0 ? VK_PRESENT_MODE_MAILBOX_KHR : strcmp(env, "immediate") == 0 ? VK_PRESENT_MODE_IMMEDIATE_KHR
             : strcmp(env, "relaxed") == 0 ? VK_PRESENT_MODE_FIFO_RELAXED_KHR : VK_PRESENT_MODE_FIFO_KHR;
    }
    switch (gPresentModePref.load(std::memory_order_relaxed))
    {
    case 1: return VK_PRESENT_MODE_FIFO_KHR;
    case 2: return VK_PRESENT_MODE_IMMEDIATE_KHR;
    case 3: return VK_PRESENT_MODE_MAILBOX_KHR;
    default: return gVsyncPref.load(std::memory_order_relaxed) ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
}
}

void setPresentPreference(int mode, bool vsync_on)
{
    gPresentModePref.store(mode, std::memory_order_relaxed);
    gVsyncPref.store(vsync_on, std::memory_order_relaxed);
}

bool Swapchain::needsRecreate() const
{
    return mNeedsRecreate || (mSwapchain != VK_NULL_HANDLE && mRequestedMode != wanted_present_mode());
}

bool Swapchain::create(VkSurfaceKHR surface, HWND window)
{
    Context& ctx = Context::instance();
    VkDevice dev = ctx.getDevice();
    VkPhysicalDevice phys = ctx.getPhysicalDevice();
    mSurface = surface;
    mWindow = window;
    mNeedsRecreate = false;

    VkSurfaceCapabilitiesKHR caps;
    if (!vk_ok(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps), "surface capabilities"))
    {
        return false;
    }
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu)
    {
        U32 w = 0, h = 0;
        windowClientSize(window, w, h);
        extent.width = w;
        extent.height = h;
    }
    // keep the old swapchain until the new one exists and hand it over (oldSwapchain): the compositor then switches over without showing a stale picture
    VkSwapchainKHR old_swapchain = mSwapchain;
    vkDeviceWaitIdle(dev);
    for (VkSemaphore s : mRenderDone) vkDestroySemaphore(dev, s, nullptr);
    mRenderDone.clear();
    for (VkImageView v : mViews) vkDestroyImageView(dev, v, nullptr);
    mViews.clear();
    mImages.clear();
    mSwapchain = VK_NULL_HANDLE;
    if (extent.width == 0 || extent.height == 0)
    {
        if (old_swapchain != VK_NULL_HANDLE)
        {
            vkDestroySwapchainKHR(dev, old_swapchain, nullptr);
        }
        return true;     // minimised: no swapchain until the window has an area again
    }

    U32 fcount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fcount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fcount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fcount, formats.data());
    VkSurfaceFormatKHR chosen = formats.empty() ? VkSurfaceFormatKHR{ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR } : formats[0];
    for (const auto& f : formats)
    {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            chosen = f;
        }
    }

    U32 image_count = caps.minImageCount + 1;
    if (const char* n = getenv("LL_VK_SC_IMAGES"))
    {
        image_count = std::max<U32>(caps.minImageCount, (U32)atoi(n));      // experiment: swapchain image count
    }
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount)
    {
        image_count = caps.maxImageCount;
    }
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    {
        const VkPresentModeKHR want = wanted_present_mode();
        mRequestedMode = want;
        U32 mcount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &mcount, nullptr);
        std::vector<VkPresentModeKHR> modes(mcount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &mcount, modes.data());
        for (VkPresentModeKHR pm : modes) if (pm == want) present_mode = want;
    }

    VkSwapchainCreateInfoKHR sc = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    sc.surface = surface;
    sc.minImageCount = image_count;
    sc.imageFormat = chosen.format;
    sc.imageColorSpace = chosen.colorSpace;
    sc.imageExtent = extent;
    sc.imageArrayLayers = 1;
    sc.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sc.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sc.preTransform = caps.currentTransform;
    sc.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sc.presentMode = present_mode;
    sc.clipped = VK_TRUE;
    sc.oldSwapchain = old_swapchain;
    const bool created = vk_ok(vkCreateSwapchainKHR(dev, &sc, nullptr, &mSwapchain), "vkCreateSwapchainKHR");
    if (old_swapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(dev, old_swapchain, nullptr);
    }
    if (!created)
    {
        mSwapchain = VK_NULL_HANDLE;
        return false;
    }
    mFormat = chosen.format;
    mExtent = extent;

    U32 icount = 0;
    vkGetSwapchainImagesKHR(dev, mSwapchain, &icount, nullptr);
    mImages.resize(icount);
    vkGetSwapchainImagesKHR(dev, mSwapchain, &icount, mImages.data());

    for (VkImage image : mImages)
    {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = chosen.format;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkImageView view = VK_NULL_HANDLE;
        if (!vk_ok(vkCreateImageView(dev, &vi, nullptr, &view), "vkCreateImageView"))
        {
            return false;
        }
        mViews.push_back(view);

        VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkSemaphore sem = VK_NULL_HANDLE;
        if (!vk_ok(vkCreateSemaphore(dev, &si, nullptr, &sem), "vkCreateSemaphore"))
        {
            return false;
        }
        mRenderDone.push_back(sem);
    }

    LL_INFOS("Vulkan") << "swapchain " << extent.width << "x" << extent.height << " format " << (S32)chosen.format << " images " << icount << LL_ENDL;
    return true;
}

bool Swapchain::acquire(VkSemaphore signal, U32& index, U64 timeout_ns)
{
    if (mSwapchain == VK_NULL_HANDLE)
    {
        return false;
    }
    const auto acquire_start = std::chrono::steady_clock::now();
    VkResult r = vkAcquireNextImageKHR(Context::instance().getDevice(), mSwapchain, timeout_ns, signal, VK_NULL_HANDLE, &index);
    gSyncStats.mAcquireWaitMicros.fetch_add((U64)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - acquire_start).count(), std::memory_order_relaxed);
    static U32 sLogged = 0;
    if (r != VK_SUCCESS && r != VK_NOT_READY && r != VK_TIMEOUT && sLogged++ < 20)
    {
        LL_WARNS("Vulkan") << "vkAcquireNextImageKHR returned " << (S32)r << LL_ENDL;
    }
    if (r == VK_ERROR_OUT_OF_DATE_KHR)
    {
        mNeedsRecreate = true;
        return false;
    }
    if (r == VK_SUBOPTIMAL_KHR)
    {
        mNeedsRecreate = true;
        return true;
    }
    return r == VK_SUCCESS;
}

bool Swapchain::present(U32 index, VkSemaphore wait)
{
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &wait;
    pi.swapchainCount = 1;
    pi.pSwapchains = &mSwapchain;
    pi.pImageIndices = &index;
    const auto present_start = std::chrono::steady_clock::now();
    VkResult r = vkQueuePresentKHR(Context::instance().getQueue(), &pi);
    gSyncStats.mPresentMicros.fetch_add((U64)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - present_start).count(), std::memory_order_relaxed);
    static U32 sLogged = 0;
    if (r != VK_SUCCESS && sLogged++ < 20)
    {
        LL_WARNS("Vulkan") << "vkQueuePresentKHR returned " << (S32)r << LL_ENDL;
    }
    mPresentOutOfDate = r == VK_ERROR_OUT_OF_DATE_KHR;
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
    {
        mNeedsRecreate = true;
        return false;
    }
    return r == VK_SUCCESS;
}

bool Swapchain::acquireBlocking(U32& index)
{
    if (mSwapchain == VK_NULL_HANDLE)
    {
        return false;
    }
    VkDevice dev = Context::instance().getDevice();
    VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = VK_NULL_HANDLE;
    if (vkCreateFence(dev, &fi, nullptr, &fence) != VK_SUCCESS)
    {
        return false;
    }
    VkResult r = vkAcquireNextImageKHR(dev, mSwapchain, 1000ull * 1000 * 1000, VK_NULL_HANDLE, fence, &index);
    bool ok = r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
    if (ok)
    {
        ok = vkWaitForFences(dev, 1, &fence, VK_TRUE, 1000ull * 1000 * 1000) == VK_SUCCESS;
    }
    vkDestroyFence(dev, fence, nullptr);
    return ok;
}

bool Swapchain::presentNoWait(U32 index)
{
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.swapchainCount = 1;
    pi.pSwapchains = &mSwapchain;
    pi.pImageIndices = &index;
    VkResult r = vkQueuePresentKHR(Context::instance().getQueue(), &pi);
    mPresentOutOfDate = r == VK_ERROR_OUT_OF_DATE_KHR;
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
    {
        mNeedsRecreate = true;
    }
    return r == VK_SUCCESS;
}

SyncStats gSyncStats;

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, U32 mips, U32 layers,
                  VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                  VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access)
{
    gSyncStats.mImageBarriers.fetch_add(1, std::memory_order_relaxed);
    VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = { aspect, 0, mips, 0, layers };
    VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace LLVk

#endif // LL_VULKAN
