"""Patches the copy of the FidelityFX SDK 1.1.4 sources that the Linux build uses (the SDK itself is not changed).

Usage: python3 patch_sdk.py <sdk folder>   (the folder with include/ and src/)

1. src/backends/vk/ffx_vk.cpp, findMemoryTypeIndex: a resource that asks for plain device local memory skips every memory type that is also host visible ("make sure this is the invisible
   heap"). A device whose memory is all host visible (a software device, an integrated GPU of some drivers) then has no type that matches, the function returns UINT32_MAX and the backend
   reports FFX_ERROR_BACKEND_API_ERROR for the first resource of the first FSR context. With the patch a device local type that is host visible is taken when nothing else is found.
2. include/FidelityFX/host/ffx_types.h and ffx_denoiser.h: the opaque context blocks are made larger (wchar_t is 4 bytes on Linux and the SDK keeps names as wchar_t in its contexts).
"""
import os
import re
import sys

sdk = sys.argv[1]


def patch(path, old, new, count=1):
    with open(path, "r", newline="") as f:
        text = f.read()
    if new in text:
        print("already patched:", os.path.basename(path))
        return
    eol = "\r\n" if "\r\n" in text else "\n"
    old = old.replace("\n", eol)
    new = new.replace("\n", eol)
    found = text.count(old)
    assert found == count, "%s: expected %d match(es) of the anchor, found %d" % (path, count, found)
    with open(path, "w", newline="") as f:
        f.write(text.replace(old, new))
    print("patched:", os.path.basename(path))


vk = os.path.join(sdk, "src", "backends", "vk", "ffx_vk.cpp")
patch(
    vk,
    "    return bestCandidate;\n}\n\nVkBufferUsageFlags ffxGetVKBufferUsageFlagsFromResourceUsage",
    "    if (bestCandidate == UINT32_MAX && requestedProperties == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)\n"
    "    {\n"
    "        // (Linux build patch) all the memory of this device is host visible: a device local type that is host visible will do\n"
    "        for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {\n"
    "            if ((memRequirements.memoryTypeBits & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & requestedProperties)) {\n"
    "                outProperties = memProperties.memoryTypes[i].propertyFlags;\n"
    "                return i;\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "\n"
    "    return bestCandidate;\n}\n\nVkBufferUsageFlags ffxGetVKBufferUsageFlagsFromResourceUsage",
)

# 3. the arrays the backend carves out of its scratch buffer (the GPU jobs, resource views, staging ring, pipeline layouts, resources, effect contexts, extension properties) start wherever
#    the sum of the sizes before them leaves them: a multiple of 4 bytes. GCC at -O2 zeroes a struct with aligned 16 byte stores (movaps) because the type is aligned, and the effect context
#    of the FSR context creation sat at an address ending in 8: SIGSEGV in CreateBackendContextVK. Every array now starts at a multiple of 64 and the size of the scratch buffer has room for it.
with open(vk, "r", newline="") as f:
    text = f.read()
if "(Linux build patch) aligned array" not in text:
    names = "pGpuJobs|pResourceViews|pStagingRingBuffer|pPipelineLayouts|pResources|pEffectContexts|extensionProperties"
    pattern = re.compile(r"^([ \t]*)(backendContext->(?:%s) = [^;\n]*pMem\)?;)" % names, re.M)
    text, n = pattern.subn(lambda m: "%spMem = (uint8_t*)(((uintptr_t)pMem + 63) & ~(uintptr_t)63);  // (Linux build patch) aligned array\n%s%s" % (m.group(1), m.group(1), m.group(2)), text)
    assert n == 7, "the 7 array assignments of CreateBackendContextVK: found %d" % n
    old_size = "pipelineArraySize + resourceArraySize + contextArraySize,"
    assert text.count(old_size) == 1, "the scratch size sum: found %d" % text.count(old_size)
    text = text.replace(old_size, "pipelineArraySize + resourceArraySize + contextArraySize + 8 * 64,")
    with open(vk, "w", newline="") as f:
        f.write(text)
    print("patched: ffx_vk.cpp (aligned arrays, 7 places and the scratch size)")
else:
    print("already patched: ffx_vk.cpp (aligned arrays)")

# 3b. a pipeline whose shader blob is empty is refused with an error instead of being handed to the driver (RADV crashes on a zero size shader module; the FSR fallback only works on an error).
patch(
    vk,
    "    backendInterface->fpGetPermutationBlobByIndex(effect, pass, FFX_BIND_COMPUTE_SHADER_STAGE, permutationOptions, &shaderBlob);\n"
    "    FFX_ASSERT(shaderBlob.data && shaderBlob.size);\n",
    "    backendInterface->fpGetPermutationBlobByIndex(effect, pass, FFX_BIND_COMPUTE_SHADER_STAGE, permutationOptions, &shaderBlob);\n"
    "    FFX_ASSERT(shaderBlob.data && shaderBlob.size);\n"
    "    if (!shaderBlob.data || !shaderBlob.size)  // (Linux build patch) empty blob\n"
    "        return FFX_ERROR_BACKEND_API_ERROR;\n",
)

# 4. (only with the argument "trace") every "return FFX_ERROR_BACKEND_API_ERROR;" of the Vulkan backend prints its line to stderr: a diagnostic for a device on which an FSR context cannot be made.
if len(sys.argv) > 2 and sys.argv[2] == "trace":
    with open(vk, "r", newline="") as f:
        text = f.read()
    if "RK_FFX_TRACE_ERR" not in text:
        # the Vulkan result, the effect, the pass and the permutation of a shader module or compute pipeline the driver refuses
        shader_old = "if (backendContext->vkFunctionTable.vkCreateShaderModule(backendContext->device, &shaderModuleCreateInfo, nullptr, &shaderModule) != VK_SUCCESS) {"
        shader_new = ("VkResult rkShaderRes = backendContext->vkFunctionTable.vkCreateShaderModule(backendContext->device, &shaderModuleCreateInfo, nullptr, &shaderModule);\n"
                      "    if (rkShaderRes != VK_SUCCESS) {\n"
                      "        fprintf(stderr, \"FFX_VK_SHADER_MODULE_RESULT %d size %u effect %d pass %d permutation 0x%x\\n\", (int)rkShaderRes, (unsigned)shaderBlob.size, (int)effect, (int)pass, (unsigned)permutationOptions);")
        pipe_old = "if (backendContext->vkFunctionTable.vkCreateComputePipelines(backendContext->device, nullptr, 1, &pipelineCreateInfo, nullptr, &computePipeline) != VK_SUCCESS) {"
        pipe_new = ("fprintf(stderr, \"FFX_VK_CREATE_PIPELINE effect %d pass %d permutation 0x%x wave64 %d shader bytes %u\\n\", (int)effect, (int)pass, (unsigned)permutationOptions, (int)isWave64, (unsigned)shaderBlob.size); fflush(stderr);\n"
                    "    VkResult rkPipeRes = backendContext->vkFunctionTable.vkCreateComputePipelines(backendContext->device, nullptr, 1, &pipelineCreateInfo, nullptr, &computePipeline);\n"
                    "    if (rkPipeRes != VK_SUCCESS) {\n"
                    "        fprintf(stderr, \"FFX_VK_PIPELINE_RESULT %d effect %d pass %d permutation 0x%x\\n\", (int)rkPipeRes, (int)effect, (int)pass, (unsigned)permutationOptions);")
        assert text.count(shader_old) == 1 and text.count(pipe_old) == 1, "the shader module / compute pipeline calls: %d / %d" % (text.count(shader_old), text.count(pipe_old))
        text = text.replace(shader_old, shader_new).replace(pipe_old, pipe_new)
        text, n = re.subn(r"return FFX_ERROR_BACKEND_API_ERROR;", "return RK_FFX_TRACE_ERR;", text)
        text = "#include <cstdio>\n#define RK_FFX_TRACE_ERR (fprintf(stderr, \"FFX_VK_ERROR line %d\\n\", __LINE__), FFX_ERROR_BACKEND_API_ERROR)\n" + text
        with open(vk, "w", newline="") as f:
            f.write(text)
        print("patched: ffx_vk.cpp (error trace, %d returns)" % n)
    else:
        print("already patched: ffx_vk.cpp (error trace)")

patch(
    os.path.join(sdk, "include", "FidelityFX", "host", "ffx_types.h"),
    "#define FFX_SDK_DEFAULT_CONTEXT_SIZE (1024 * 128)",
    "#define FFX_SDK_DEFAULT_CONTEXT_SIZE (1024 * 512)",
)
patch(
    os.path.join(sdk, "include", "FidelityFX", "host", "ffx_denoiser.h"),
    "#define FFX_DENOISER_CONTEXT_SIZE (73098)",
    "#define FFX_DENOISER_CONTEXT_SIZE (73098 * 4)",
)
