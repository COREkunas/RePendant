cmake_minimum_required(VERSION 3.22)
project(openpendant_whisper LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_OBJECT_PATH_MAX 180)

# The APK contains only the CPU backend. Model downloading and dynamic-backend
# builds are disabled; the JNI wrapper never requests backend discovery.
# All upstream sources are pinned locally.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_IS_DEV OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(WHISPER_CURL OFF CACHE BOOL "" FORCE)
set(WHISPER_SDL2 OFF CACHE BOOL "" FORCE)
set(WHISPER_COREML OFF CACHE BOOL "" FORCE)
set(WHISPER_OPENVINO OFF CACHE BOOL "" FORCE)
set(WHISPER_VITISAI OFF CACHE BOOL "" FORCE)
set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
set(GGML_CPU ON CACHE BOOL "" FORCE)
set(GGML_CPU_ARM_ARCH "armv8-a" CACHE STRING "" FORCE)
set(GGML_CPU_ALL_VARIANTS OFF CACHE BOOL "" FORCE)
set(GGML_CPU_KLEIDIAI OFF CACHE BOOL "" FORCE)
set(GGML_BACKEND_DL OFF CACHE BOOL "" FORCE)
set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
set(GGML_OPENMP_FETCH OFF CACHE BOOL "" FORCE)
set(GGML_CUDA OFF CACHE BOOL "" FORCE)
set(GGML_VULKAN OFF CACHE BOOL "" FORCE)
set(GGML_OPENCL OFF CACHE BOOL "" FORCE)
set(GGML_METAL OFF CACHE BOOL "" FORCE)
set(GGML_BLAS OFF CACHE BOOL "" FORCE)
set(GGML_SYCL OFF CACHE BOOL "" FORCE)
set(GGML_CANN OFF CACHE BOOL "" FORCE)
set(GGML_HEXAGON OFF CACHE BOOL "" FORCE)
set(GGML_RPC OFF CACHE BOOL "" FORCE)
set(GGML_CCACHE OFF CACHE BOOL "" FORCE)
add_subdirectory(vendor/whisper.cpp EXCLUDE_FROM_ALL)

add_library(openpendant_whisper SHARED whisper_jni.cpp)
target_compile_options(openpendant_whisper PRIVATE -Wall -Wextra -Werror)
target_link_libraries(openpendant_whisper PRIVATE whisper android log)
# NDK r30 already defaults to flexible page sizes. Keep the JNI boundary explicit
# for Android devices configured with 16 KiB memory pages.
target_link_options(openpendant_whisper PRIVATE "-Wl,-z,max-page-size=16384")

# Independent tiny storage boundary: no model, inference, audio or network code.
# Android SELinux forbids app hard links; API30+ no-replace rename is fail-closed.
add_library(openpendant_storage SHARED segment_publication.c segment_publication_jni.c)
target_compile_options(openpendant_storage PRIVATE -Wall -Wextra -Werror -fvisibility=hidden)
target_link_libraries(openpendant_storage PRIVATE android dl)
target_link_options(openpendant_storage PRIVATE "-Wl,-z,max-page-size=16384")

include(opus_decode.cmake)
