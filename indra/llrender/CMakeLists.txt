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
    rkdevicecaps.cpp
    rkshaderprogram.cpp
    rkbasetexture.cpp
    rkimagetex.cpp
    llrender.cpp
    llrender2dutils.cpp
    llrendersphere.cpp
    llrendertarget.cpp
    llshadermgr.cpp
    lltexture.cpp
    lltexturemanagerbridge.cpp
    lluiimage.cpp
    llvertexbuffer.cpp
    rkvkcontext.cpp
    rkvkngx.cpp
    rkvkdlssnr.cpp
    rkvkdenoise.cpp
    rkvkfsr.cpp
    rkvkupscale.cpp
    rkvkasync.cpp
    rkvkpasscensus.cpp
    rkvkbridge.cpp
    rkvkworker.cpp
    rkvkformat.cpp
    rkvkrenderer.cpp
    rkvkrt.cpp
    rkvknative.cpp
    rkvkstate.cpp
    rkvkvma.cpp
    rkrendercommonfunc.cpp
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
    rkdevicecaps.h
    rkrenderheaders.h
    rkshaderprogram.h
    rkrenderpresets.h
    rkbasetexture.h
    llgltypes.h
    rkimagetex.h
    llrender.h
    rkrendermatrix.h
    rkrenderformat.h
    rkrenderpass.h
    rkrenderstate.h
    llrender2dutils.h
    llrendersphere.h
    llshadermgr.h
    lltexture.h
    lltexturemanagerbridge.h
    lluiimage.h
    lluiimage.inl
    llvertexbuffer.h
    rkvkcontext.h
    rkvkngx.h
    rkreadback.h
    rkvkdlssnr.h
    rkvkdenoise.h
    rkvkfsr.h
    rkvkupscale.h
    rkvkfunctions.inl
    rkvkasync.h
    rkvkpasscensus.h
    rkvkzones.h
    rkvksubstream.h
    rkvkbridge.h
    rkvkworker.h
    rkvkrenderer.h
    rkvkrt.h
    rkvkshader.h
    rkvknative.h
    rkvkstate.h
    rkvktex.h
    rkvkformat.h
    rkrendercommonfunc.h
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

# <FS> Experimental Vulkan backend (see rkvkprobe.h). Needs the Vulkan SDK headers; the loader is loaded at runtime.
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
    target_compile_definitions(llrender PRIVATE RK_VULKAN=1)
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
    set(rkrender_glslang_libs "")
    foreach (lib glslang-default-resource-limits glslang SPIRV SPIRV-Tools-opt SPIRV-Tools MachineIndependent GenericCodeGen OSDependent)
      find_library(RKRENDER_LIB_${lib} NAMES ${lib})
      if (NOT RKRENDER_LIB_${lib})
        message(FATAL_ERROR "USE_VULKAN on Linux needs lib${lib}.a (glslang-dev, spirv-tools-dev)")
      endif ()
      list(APPEND rkrender_glslang_libs "${RKRENDER_LIB_${lib}}")
    endforeach ()
    target_include_directories(llrender SYSTEM PRIVATE "${VULKAN_INCLUDE_DIR}")
    target_compile_definitions(llrender PRIVATE RK_VULKAN=1)
    target_link_libraries(llrender ${rkrender_glslang_libs} ${CMAKE_DL_LIBS})
  endif ()
  # NVIDIA, LOCAL builds only (doc/vulkan/PLAN-remove-nvidia.md): the viewer has no NVIDIA code in a release. With REKINDLED_LOCAL_NVIDIA=ON (never the default, never the package script) the
  # DLSS SDK's NGX is linked for DLSS-NR (the neural renderer, rkvkngx.cpp / rkvkdlssnr.cpp) when it is found at DLSS_SDK_DIR (a clone of github.com/NVIDIA/DLSS, its DLSS folder), and the
  # device asks the driver for VK_NV_device_diagnostics_config (resource tracking for the Aftermath decode of a device lost, rkvkcontext.cpp: RK_LOCAL_NV_DIAG).
  option(REKINDLED_LOCAL_NVIDIA "Local build: DLSS-NR through NGX and the NVIDIA device diagnostics (never for a release)" OFF)
  if (REKINDLED_LOCAL_NVIDIA)
    message(STATUS "REKINDLED_LOCAL_NVIDIA is ON: this build is for local use, not for a release")
    target_compile_definitions(llrender PRIVATE RK_LOCAL_NV_DIAG=1)
    set(DLSS_SDK_DIR "<work>/DLSS/DLSS" CACHE PATH "NVIDIA DLSS SDK root (include/nvsdk_ngx_vk.h, lib/Windows_x86_64/x64)")
    if (EXISTS "${DLSS_SDK_DIR}/include/nvsdk_ngx_vk.h" AND EXISTS "${DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
      message(STATUS "DLSS SDK found at ${DLSS_SDK_DIR} (DLSS-NR)")
      target_include_directories(llrender SYSTEM PRIVATE "${DLSS_SDK_DIR}/include")
      target_compile_definitions(llrender PRIVATE RK_NGX_NR=1)
      target_link_libraries(llrender "${DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
    endif ()
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
    target_compile_definitions(llrender PRIVATE RK_FSR=1)
    target_link_libraries(llrender "${FFX_LIB_DIR}/ffx_fsr3upscaler_x64.lib")
    set(FFX_ANY TRUE)
  endif ()
  if (EXISTS "${FFX_LIB_DIR}/ffx_denoiser_x64.lib" AND EXISTS "${FFX_LIB_DIR}/ffx_backend_vk_x64.lib" AND EXISTS "${FFX_SDK_DIR}/sdk/include/FidelityFX/host/ffx_denoiser.h")
    message(STATUS "FidelityFX Denoiser found at ${FFX_LIB_DIR}")
    target_compile_definitions(llrender PRIVATE RK_FFX_DENOISER=1)
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

