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
    llvkfsr.cpp
    llvkupscale.cpp
    llvkasync.cpp
    llvkpasscensus.cpp
    llvkbridge.cpp
    llvkworker.cpp
    llvkformat.cpp
    llvkrenderer.cpp
    llvkrt.cpp
    llvkshader.cpp
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
  # NVIDIA DLSS (optional): the SDK is not part of the source tree; point DLSS_SDK_DIR at a clone of github.com/NVIDIA/DLSS (its DLSS folder)
  set(DLSS_SDK_DIR "<work>/DLSS/DLSS" CACHE PATH "NVIDIA DLSS SDK root (include/nvsdk_ngx_vk.h, lib/Windows_x86_64/x64)")
  if (EXISTS "${DLSS_SDK_DIR}/include/nvsdk_ngx_vk.h" AND EXISTS "${DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
    message(STATUS "DLSS SDK found at ${DLSS_SDK_DIR}")
    target_include_directories(llrender SYSTEM PRIVATE "${DLSS_SDK_DIR}/include")
    target_compile_definitions(llrender PRIVATE LL_DLSS=1)
    target_link_libraries(llrender "${DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
  endif ()
  # AMD FSR 3.1 upscaler (optional): headers of the FidelityFX SDK v1.1.4 (the last release with Vulkan), ffx-api. amd_fidelityfx_vk.dll is loaded at run time, nothing is linked.
  set(FFX_SDK_DIR "<work>/FidelityFX-SDK-1.1.4" CACHE PATH "AMD FidelityFX SDK v1.1.4 root (ffx-api/include/ffx_api/ffx_upscale.h)")
  if (EXISTS "${FFX_SDK_DIR}/ffx-api/include/ffx_api/ffx_upscale.h")
    message(STATUS "FidelityFX SDK found at ${FFX_SDK_DIR}")
    target_include_directories(llrender SYSTEM PRIVATE "${FFX_SDK_DIR}/ffx-api/include")
    target_compile_definitions(llrender PRIVATE LL_FSR=1)
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

