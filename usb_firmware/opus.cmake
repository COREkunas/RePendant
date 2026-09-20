# Pinned fixed-point Opus with upstream inline Thumb DSP only.
# Include after find_package(Zephyr)/project(). No external ARM assembly/RTCD.
# This fragment does not start a worker or register a command.
if(NOT TARGET app OR NOT TARGET zephyr_interface OR TARGET opus)
  message(FATAL_ERROR "Opus requires this Zephyr application and no existing opus target")
endif()
if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU" OR NOT CONFIG_CPU_CORTEX_M33)
  message(FATAL_ERROR "The reviewed Opus integration requires GNU and Cortex-M33")
endif()
if(CMAKE_BUILD_TYPE OR CMAKE_CONFIGURATION_TYPES)
  message(FATAL_ERROR "Zephyr controls optimization: clear any inherited CMAKE_BUILD_TYPE/configuration types before Opus integration")
endif()
foreach(_opus_required CONFIG_REBOOT CONFIG_RESET_ON_FATAL_ERROR CONFIG_ASSERT CONFIG_ARMV8_M_DSP
    CONFIG_STACK_CANARIES_STRONG CONFIG_INIT_STACKS CONFIG_THREAD_STACK_INFO)
  if(NOT ${_opus_required})
    message(FATAL_ERROR "Opus integration requires ${_opus_required}=y")
  endif()
endforeach()
foreach(_opus_forbidden CONFIG_LOG CONFIG_PRINTK CONFIG_COVERAGE
    CONFIG_COVERAGE_DUMP CONFIG_COVERAGE_DUMP_SEMIHOST CONFIG_SMP CONFIG_FPU)
  if(${_opus_forbidden})
    message(FATAL_ERROR "Unreviewed Opus configuration: ${_opus_forbidden}")
  endif()
endforeach()

get_filename_component(_opus_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_opus_verify "${_opus_root}/tools/verify_opus_integration.py")
set(_opus_source "${_opus_root}/third_party/opus-1.6.1")
if(NOT PYTHON_EXECUTABLE)
  message(FATAL_ERROR "Zephyr Python executable is required for pinned-source verification")
endif()
execute_process(COMMAND "${PYTHON_EXECUTABLE}" "${_opus_verify}" --source-only
  RESULT_VARIABLE _opus_source_result OUTPUT_VARIABLE _opus_source_proof
  ERROR_VARIABLE _opus_source_error)
if(NOT _opus_source_result EQUAL 0)
  message(FATAL_ERROR "Opus source verification failed: ${_opus_source_error}")
endif()
message(STATUS "${_opus_source_proof}")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Retain effective compilation evidence" FORCE)

# Compile actual upstream inline arithmetic and FFT macros for this exact core.
# A compile/assemble probe only: it is never linked into the firmware or run.
# Keep this function scoped so flags cannot leak into app/kernel compilation.
function(_openpendant_check_inline_dsp)
  include(CheckCSourceCompiles)
  set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
  set(CMAKE_REQUIRED_FLAGS "-O3 -mcpu=cortex-m33 -mthumb -mfloat-abi=soft -fno-fast-math")
  set(CMAKE_REQUIRED_INCLUDES "${_opus_source}/include" "${_opus_source}/celt" "${_opus_source}/silk")
  set(CMAKE_REQUIRED_DEFINITIONS -DFIXED_POINT=1 -DOPUS_BUILD=1
      -DOPUS_ARM_INLINE_ASM=1 -DOPUS_ARM_INLINE_EDSP=1 -DOPUS_ARM_INLINE_MEDIA=1)
  # Do not trust a stale success from another compiler or configuration.
  unset(OPENPENDANT_OPUS_M33_INLINE_DSP)
  unset(OPENPENDANT_OPUS_M33_INLINE_DSP CACHE)
  check_c_source_compiles([=[
/* OPENPENDANT_INLINE_DSP_PROBE_BEGIN */
#if !defined(__ARM_ARCH_8M_MAIN__) || !defined(__ARM_FEATURE_DSP) || __ARM_FEATURE_DSP != 1 || !defined(__thumb__)
#error Expected Cortex-M33 Thumb DSP compiler target
#endif
#include "arch.h"
#include "SigProc_FIX.h"
#include "_kiss_fft_guts.h"
int opus_dsp_probe(int a, int b, int c) {
  return silk_ADD_SAT32(silk_SMLAWB(a,b,c), silk_SUB_SAT32(silk_SMULWT(b,c),a))
       ^ silk_SMLATT(a,b,c) ^ SIG2WORD16(a) ^ silk_SMULWW(b,c);
}
void opus_fft_probe(kiss_fft_cpx *out, const kiss_fft_cpx *a,
                    const kiss_twiddle_cpx *b) {
  C_MUL(out[0],a[0],b[0]);
  C_MUL4(out[1],a[1],b[1]);
  C_MULC(out[2],a[2],b[2]);
}
/* OPENPENDANT_INLINE_DSP_PROBE_END */
]=] OPENPENDANT_OPUS_M33_INLINE_DSP)
  if(NOT OPENPENDANT_OPUS_M33_INLINE_DSP)
    message(FATAL_ERROR "Pinned upstream Thumb DSP/FFT inline assembly is not supported by this compiler")
  endif()
endfunction()
_openpendant_check_inline_dsp()

# Force both normal and cache values: a parent normal variable must not silently
# override this closed configuration. VLA support is still compiler-detected.
foreach(_opus_on OPUS_FIXED_POINT OPUS_DISABLE_INTRINSICS OPUS_ASSERTIONS
    OPUS_HARDENING OPUS_STACK_PROTECTOR OPUS_VAR_ARRAYS)
  set(${_opus_on} ON)
  set(${_opus_on} ON CACHE BOOL "Reviewed OpenPendant codec configuration" FORCE)
endforeach()
foreach(_opus_off BUILD_SHARED_LIBS BUILD_TESTING OPUS_BUILD_SHARED_LIBRARY
    OPUS_BUILD_TESTING OPUS_BUILD_PROGRAMS OPUS_BUILD_FRAMEWORK
    OPUS_ENABLE_FLOAT_API OPUS_FORTIFY_SOURCE OPUS_USE_ALLOCA
    OPUS_NONTHREADSAFE_PSEUDOSTACK OPUS_DRED OPUS_OSCE OPUS_DEEP_PLC
    OPUS_FAST_MATH OPUS_FLOAT_APPROX OPUS_FUZZING OPUS_CUSTOM_MODES
    OPUS_FIXED_POINT_DEBUG OPUS_CHECK_ASM OPUS_DNN_FLOAT_DEBUG
    OPUS_INSTALL_PKG_CONFIG_MODULE OPUS_INSTALL_CMAKE_CONFIG_MODULE)
  set(${_opus_off} OFF)
  set(${_opus_off} OFF CACHE BOOL "Reviewed OpenPendant codec configuration" FORCE)
endforeach()
# OpusBuildtype otherwise writes Release to the *global cache* when C flags
# and build type are empty, leaking -DNDEBUG into unrelated Zephyr targets.
# A function-local explicit C flag suppresses that default without changing the
# parent's cache/flags. Child-directory C flags apply only to the codec.
function(_openpendant_add_opus)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-fast-math")
  add_subdirectory("${_opus_source}" "${CMAKE_CURRENT_BINARY_DIR}/openpendant_opus")
endfunction()
_openpendant_add_opus()
get_target_property(_opus_type opus TYPE)
get_directory_property(_opus_vla DIRECTORY "${_opus_source}" DEFINITION VLA_SUPPORTED)
if(NOT _opus_type STREQUAL "STATIC_LIBRARY" OR NOT _opus_vla
    OR NOT STACK_PROTECTOR_SUPPORTED)
  message(FATAL_ERROR "Opus must be static, with detected VLA and strong stack-protector support")
endif()
target_link_libraries(opus PRIVATE zephyr_interface)
target_compile_definitions(opus PRIVATE OVERRIDE_celt_fatal=1 OPENPENDANT_SILK_FATAL_REDIRECT=1
  OPUS_ARM_INLINE_ASM=1 OPUS_ARM_INLINE_EDSP=1 OPUS_ARM_INLINE_MEDIA=1)
target_compile_options(opus PRIVATE -UNDEBUG -fstack-usage -mfloat-abi=soft
  -fno-fast-math -fno-strict-aliasing -ffunction-sections -fdata-sections)
# Interface options follow target options; Zephyr's inherited -Os would win
# over a target-private -O3. Source options are emitted after both. Restrict
# this final override to the exact pinned codec C source set/directory only.
get_target_property(_opus_source_dir opus SOURCE_DIR)
get_target_property(_opus_sources opus SOURCES)
set(_opus_optimized_units 0)
foreach(_opus_unit IN LISTS _opus_sources)
  if(_opus_unit MATCHES "\\.c$")
    get_filename_component(_opus_unit_abs "${_opus_unit}" ABSOLUTE BASE_DIR "${_opus_source_dir}")
    cmake_path(IS_PREFIX _opus_source "${_opus_unit_abs}" NORMALIZE _opus_is_vendor)
    if(NOT _opus_is_vendor OR NOT EXISTS "${_opus_unit_abs}")
      message(FATAL_ERROR "Opus optimization source is outside the pinned vendor tree")
    endif()
    set_property(SOURCE "${_opus_unit_abs}" TARGET_DIRECTORY opus APPEND PROPERTY COMPILE_OPTIONS -O3)
    math(EXPR _opus_optimized_units "${_opus_optimized_units} + 1")
  endif()
endforeach()
if(NOT _opus_optimized_units EQUAL 132)
  message(FATAL_ERROR "Expected exactly 132 pinned codec C optimization overrides")
endif()
target_compile_options(opus PRIVATE
  "SHELL:-include \"${CMAKE_CURRENT_LIST_DIR}/src/opus_silk_fatal_override.h\"")
target_sources(app PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src/opus_fatal.c")
target_link_libraries(app PRIVATE opus)

# Always recheck before compilation, including an incremental build where CMake
# did not rerun. Never extract or patch the vendor tree.
add_custom_target(openpendant_opus_source_check
  COMMAND "${PYTHON_EXECUTABLE}" "${_opus_verify}" --source-only VERBATIM)
add_dependencies(opus openpendant_opus_source_check)
# This runs before app compilation/link and also on an incremental no-op build.
# It copies the exact upstream license into the build proof directory; a release
# must include that directory's OPUS_COPYING.txt alongside the firmware.
add_custom_target(openpendant_opus_audit
  COMMAND "${PYTHON_EXECUTABLE}" "${_opus_verify}"
    --build-dir "${CMAKE_CURRENT_BINARY_DIR}" --library "$<TARGET_FILE:opus>"
    --nm "${CMAKE_NM}" --output-dir "${CMAKE_CURRENT_BINARY_DIR}/opus-audit"
  DEPENDS opus VERBATIM)
add_dependencies(app openpendant_opus_audit)
