# The AMD FidelityFX SDK 1.1.4 static libraries (FSR 3.1 upscaler, the denoiser, the Vulkan backend) for Linux, with GCC.
#
# The SDK's own CMake files build for Windows (Visual Studio, its shader compiler is a Windows program). This file compiles the same sources with the C++ compiler of the
# machine and takes the compiled shaders (the *_permutations.h and the hashed .h files, SPIR-V byte arrays, the same for every platform) from a build of the SDK that was
# made on Windows: FFX_GEN_DIR is its folder src/backends/shaders/vk.
#
#   cmake -S <this folder> -B <build> -DFFX_SDK_DIR=<FidelityFX-SDK-1.1.4>/sdk -DFFX_GEN_DIR=<windows build>/src/backends/shaders/vk
#   cmake --build <build> -j
#
# The libraries are named like the Windows ones (ffx_fsr3upscaler_x64, ffx_denoiser_x64, ffx_backend_vk_x64) and land in <build>; llrender/CMakeLists.txt looks for them in FFX_LIB_DIR.
cmake_minimum_required(VERSION 3.16)
project(ffx_linux CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif ()

set(FFX_SDK_DIR "" CACHE PATH "the sdk folder of the FidelityFX SDK 1.1.4 (it holds include/ and src/)")
set(FFX_GEN_DIR "" CACHE PATH "the folder with the compiled shader headers (from a Windows build of the SDK: src/backends/shaders/vk)")
if (NOT EXISTS "${FFX_SDK_DIR}/include/FidelityFX/host/ffx_fsr3upscaler.h")
  message(FATAL_ERROR "FFX_SDK_DIR must name the sdk folder of the FidelityFX SDK (include/FidelityFX/host/ffx_fsr3upscaler.h)")
endif ()
if (NOT EXISTS "${FFX_GEN_DIR}/ffx_fsr3upscaler_accumulate_pass_permutations.h")
  message(FATAL_ERROR "FFX_GEN_DIR must name the folder with the compiled shader headers (ffx_fsr3upscaler_accumulate_pass_permutations.h)")
endif ()

find_package(Vulkan REQUIRED)

file(GLOB FFX_SHARED_SOURCES "${FFX_SDK_DIR}/src/shared/*.cpp")
set(FFX_INCLUDES
  "${FFX_SDK_DIR}/include"
  "${FFX_SDK_DIR}/src/shared"
  "${FFX_SDK_DIR}/src/backends/shared"
  "${FFX_SDK_DIR}/src/backends/shared/blob_accessors"
  "${FFX_SDK_DIR}/src/components"
  "${FFX_GEN_DIR}"
  ${Vulkan_INCLUDE_DIRS})

function (ffx_lib name)
  add_library(${name} STATIC ${ARGN})
  target_include_directories(${name} PRIVATE ${FFX_INCLUDES})
  target_compile_options(${name} PRIVATE -w -g1 -fno-strict-aliasing -include "${CMAKE_CURRENT_LIST_DIR}/ffx_linux_compat.h")
endfunction ()

# the Vulkan backend with the shader blobs of the two components used
ffx_lib(ffx_backend_vk_x64
  ${FFX_SHARED_SOURCES}
  "${FFX_SDK_DIR}/src/backends/shared/ffx_shader_blobs.cpp"
  "${FFX_SDK_DIR}/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp"
  "${FFX_SDK_DIR}/src/backends/shared/blob_accessors/ffx_denoiser_shaderblobs.cpp"
  "${FFX_SDK_DIR}/src/backends/vk/ffx_vk.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/ffx_linux_stubs.cpp")
# ffx_shader_blobs.cpp has one case per effect behind these macros; without them every effect gets an empty shader blob (NVIDIA refuses the empty module: FSR does nothing; RADV crashes on it)
target_compile_definitions(ffx_backend_vk_x64 PRIVATE FFX_FSR3UPSCALER FFX_DENOISER)

ffx_lib(ffx_fsr3upscaler_x64
  ${FFX_SHARED_SOURCES}
  "${FFX_SDK_DIR}/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp")

file(GLOB FFX_DENOISER_SOURCES "${FFX_SDK_DIR}/src/components/denoiser/*.cpp")
ffx_lib(ffx_denoiser_x64 ${FFX_SHARED_SOURCES} ${FFX_DENOISER_SOURCES})
