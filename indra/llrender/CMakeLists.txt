# -*- cmake -*-

project(llrender)

include(00-Common)
include(OpenGL)
include(FreeType)
include(LLCommon)
include(LLImage)
include(LLWindow)

set(llrender_SOURCE_FILES
    llcubemap.cpp
    llcubemaparray.cpp
    llfontbitmapcache.cpp
    llfontfreetype.cpp
    llfontfreetypesvg.cpp
    llfontgl.cpp
    llfontvertexbuffer.cpp
    llfontregistry.cpp
    lldevicecaps.cpp
    llshaderprogram.cpp
    llbasetexture.cpp
    llimagetex.cpp
    llpostprocess.cpp
    llrender.cpp
    llrender2dutils.cpp
    llrendernavprim.cpp
    llrendersphere.cpp
    llrendertarget.cpp
    llshadermgr.cpp
    lltexture.cpp
    lltexturemanagerbridge.cpp
    lluiimage.cpp
    llvertexbuffer.cpp
    llvkcontext.cpp
    llvkdlss.cpp
    llvkdlssnr.cpp
    llvkdenoise.cpp
    llvkfsr.cpp
    llvkupscale.cpp
    llvkasync.cpp
    llvkpasscensus.cpp
    llvkbridge.cpp
    llvkworker.cpp
    llvkformat.cpp
    llvkrenderer.cpp
    llvkrt.cpp
    llvknative.cpp
    llvkstate.cpp
    llvkvma.cpp
    llrendercommonfunc.cpp
    )
    
set(llrender_HEADER_FILES
    CMakeLists.txt

    llcubemap.h
    llcubemaparray.h
    llfontgl.h
    llfontvertexbuffer.h
    llfontfreetype.h
    llfontfreetypesvg.h
    llfontbitmapcache.h
    llfontregistry.h
    lldevicecaps.h
    llrenderheaders.h
    llshaderprogram.h
    llrenderpresets.h
    llbasetexture.h
    llgltypes.h
    llimagetex.h
    llpostprocess.h
    llrender.h
    llrendermatrix.h
    llrenderformat.h
    llrenderpass.h
    llrenderstate.h
    llrender2dutils.h
    llrendernavprim.h
    llrendersphere.h
    llshadermgr.h
    lltexture.h
    lltexturemanagerbridge.h
    lluiimage.h
    lluiimage.inl
    llvertexbuffer.h
    llvkcontext.h
    llvkdlss.h
    llvkdlssnr.h
    llvkdenoise.h
    llvkfsr.h
    llvkupscale.h
    llvkfunctions.inl
    llvkasync.h
    llvkpasscensus.h
    llvkzones.h
    llvksubstream.h
    llvkbridge.h
    llvkworker.h
    llvkrenderer.h
    llvkrt.h
    llvkshader.h
    llvknative.h
    llvkstate.h
    llvktex.h
    llvkformat.h
    llrendercommonfunc.h
    )

list(APPEND llrender_SOURCE_FILES ${llrender_HEADER_FILES})

if (BUILD_HEADLESS)
  add_library (llrenderheadless
    ${llrender_SOURCE_FILES}
    )

  set_property(TARGET llrenderheadless
    PROPERTY COMPILE_DEFINITIONS LL_MESA=1 LL_MESA_HEADLESS=1
      )

  target_link_libraries(llrenderheadless
          llcommon
          llimage
          llmath
          llrender
          llxml
          llfilesystem
          )
endif (BUILD_HEADLESS)

add_library (llrender ${llrender_SOURCE_FILES})
target_include_directories( llrender  INTERFACE   ${CMAKE_CURRENT_SOURCE_DIR})

# <FS> Experimental Vulkan backend (see llvkprobe.h). Needs the Vulkan SDK headers; the loader is loaded at runtime.
option(USE_VULKAN "Build the experimental Vulkan backend" OFF)
set(VULKAN_SDK_DIR "" CACHE PATH "Vulkan SDK root (headers are in Include/)")
if (USE_VULKAN)
  if (WINDOWS)
    if (NOT VULKAN_SDK_DIR AND DEFINED ENV{VULKAN_SDK})
      set(VULKAN_SDK_DIR "$ENV{VULKAN_SDK}")
    endif ()
    if (NOT EXISTS "${VULKAN_SDK_DIR}/Include/vulkan/vulkan.h")
      message(FATAL_ERROR "USE_VULKAN needs VULKAN_SDK_DIR to point at a Vulkan SDK (Include/vulkan/vulkan.h not found)")
    endif ()
    target_include_directories(llrender SYSTEM PRIVATE "${VULKAN_SDK_DIR}/Include")
    target_compile_definitions(llrender PRIVATE LL_VULKAN=1)
    # glslang (GLSL -> SPIR-V) is linked statically; the viewer builds with the release CRT
    target_link_libraries(llrender
            "${VULKAN_SDK_DIR}/Lib/glslang.lib"
            "${VULKAN_SDK_DIR}/Lib/glslang-default-resource-limits.lib"
            "${VULKAN_SDK_DIR}/Lib/SPIRV-Tools-opt.lib"
            "${VULKAN_SDK_DIR}/Lib/SPIRV-Tools.lib"
            )
  else ()
    # Linux (WSL build, doc/vulkan/PLAN-linux-build.md): the distribution's Vulkan headers (libvulkan-dev, the loader is loaded at run time)
    # and static glslang / SPIR-V (glslang-dev, spirv-tools-dev)
    find_path(VULKAN_INCLUDE_DIR vulkan/vulkan.h)
    if (NOT VULKAN_INCLUDE_DIR)
      message(FATAL_ERROR "USE_VULKAN on Linux needs the Vulkan headers (libvulkan-dev)")
    endif ()
    set(llrender_glslang_libs "")
    foreach (lib glslang-default-resource-limits glslang SPIRV SPIRV-Tools-opt SPIRV-Tools MachineIndependent GenericCodeGen OSDependent)
      find_library(LLRENDER_LIB_${lib} NAMES ${lib})
      if (NOT LLRENDER_LIB_${lib})
        message(FATAL_ERROR "USE_VULKAN on Linux needs lib${lib}.a (glslang-dev, spirv-tools-dev)")
      endif ()
      list(APPEND llrender_glslang_libs "${LLRENDER_LIB_${lib}}")
    endforeach ()
    target_include_directories(llrender SYSTEM PRIVATE "${VULKAN_INCLUDE_DIR}")
    target_compile_definitions(llrender PRIVATE LL_VULKAN=1)
    target_link_libraries(llrender ${llrender_glslang_libs} ${CMAKE_DL_LIBS})
  endif ()
  # NVIDIA DLSS (optional): the SDK is not part of the source tree; point DLSS_SDK_DIR at a clone of github.com/NVIDIA/DLSS (its DLSS folder)
  set(DLSS_SDK_DIR "<work>/DLSS/DLSS" CACHE PATH "NVIDIA DLSS SDK root (include/nvsdk_ngx_vk.h, lib/Windows_x86_64/x64)")
  if (EXISTS "${DLSS_SDK_DIR}/include/nvsdk_ngx_vk.h" AND EXISTS "${DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
    message(STATUS "DLSS SDK found at ${DLSS_SDK_DIR}")
    target_include_directories(llrender SYSTEM PRIVATE "${DLSS_SDK_DIR}/include")
    target_compile_definitions(llrender PRIVATE LL_DLSS=1)
    target_link_libraries(llrender "${DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
  endif ()
  # AMD FidelityFX SDK v1.1.4 (MIT, the last release with Vulkan), built to static libraries from the SDK's own source (no amd_fidelityfx_vk.dll any more): the FSR 3.1 upscaler, the denoiser
  # (1.3) and their Vulkan backend:
  #   cmake -S <FFX_SDK_DIR>/sdk -B <build dir> -G "Visual Studio 17 2022" -A x64 -DFFX_API_BACKEND=VK_X64 -DFFX_ALL=OFF -DFFX_SSSR=ON -DFFX_DENOISER=ON -DFFX_FSR3UPSCALER=ON -DFFX_AUTO_COMPILE_SHADERS=ON
  #   (with VULKAN_SDK set), then cmake --build <build dir> --config Release (<work>fx-build-fsr3.bat). The libraries land in <FFX_SDK_DIR>/sdk/bin/ffx_sdk. doc/vulkan/PLAN-amd-denoiser.md
  set(FFX_SDK_DIR "<work>/FidelityFX-SDK-1.1.4" CACHE PATH "AMD FidelityFX SDK v1.1.4 root (sdk/include/FidelityFX/host/ffx_fsr3upscaler.h)")
  set(FFX_LIB_DIR "${FFX_SDK_DIR}/sdk/bin/ffx_sdk")
  set(FFX_ANY FALSE)
  if (EXISTS "${FFX_LIB_DIR}/ffx_fsr3upscaler_x64.lib" AND EXISTS "${FFX_LIB_DIR}/ffx_backend_vk_x64.lib" AND EXISTS "${FFX_SDK_DIR}/sdk/include/FidelityFX/host/ffx_fsr3upscaler.h")
    message(STATUS "FidelityFX FSR 3 upscaler found at ${FFX_LIB_DIR}")
    target_compile_definitions(llrender PRIVATE LL_FSR=1)
    target_link_libraries(llrender "${FFX_LIB_DIR}/ffx_fsr3upscaler_x64.lib")
    set(FFX_ANY TRUE)
  endif ()
  if (EXISTS "${FFX_LIB_DIR}/ffx_denoiser_x64.lib" AND EXISTS "${FFX_LIB_DIR}/ffx_backend_vk_x64.lib" AND EXISTS "${FFX_SDK_DIR}/sdk/include/FidelityFX/host/ffx_denoiser.h")
    message(STATUS "FidelityFX Denoiser found at ${FFX_LIB_DIR}")
    target_compile_definitions(llrender PRIVATE LL_FFX_DENOISER=1)
    target_link_libraries(llrender "${FFX_LIB_DIR}/ffx_denoiser_x64.lib")
    set(FFX_ANY TRUE)
  endif ()
  if (FFX_ANY)
    target_include_directories(llrender SYSTEM PRIVATE "${FFX_SDK_DIR}/sdk/include")
    target_link_libraries(llrender "${FFX_LIB_DIR}/ffx_backend_vk_x64.lib")
    # (the SDK's Vulkan backend calls the loader's functions directly: the import library of the Vulkan SDK; the viewer itself still loads the loader at run time)
    if (EXISTS "${VULKAN_SDK_DIR}/Lib/vulkan-1.lib")
      target_link_libraries(llrender "${VULKAN_SDK_DIR}/Lib/vulkan-1.lib")
    endif ()
  endif ()
endif ()

set_property(TARGET llrender PROPERTY POSITION_INDEPENDENT_CODE ON)

# Libraries on which this library depends, needed for Linux builds
# Sort by high-level to low-level
target_link_libraries(llrender
        llcommon
        llimage
        llmath
        llfilesystem
        llxml
        llwindow
        ll::freetype
        )
if (NOT WINDOWS)
  # <FS> native-vk: on Windows the viewer is linked without the OpenGL import libraries and calls no GL function directly
  target_link_libraries(llrender
          OpenGL::GL
          OpenGL::GLU
          )
endif ()

