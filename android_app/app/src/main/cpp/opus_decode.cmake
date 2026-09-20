# Separate explicit decoder library. No network fetch, encoder JNI, playback,
# automatic load, model dependency or owner-state access. Generic fixed point
# provides a reproducible public PCM oracle; optimization can be reviewed later.
get_filename_component(_opd_root "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
  message(FATAL_ERROR "Android controls the explicit Release native configuration")
endif()
set(_opd_vendor "${_opd_root}/third_party/opus-1.6.1")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DOPUS_VENDOR=${_opd_vendor}"
  -P "${CMAKE_CURRENT_LIST_DIR}/verify_opus_sources.cmake" RESULT_VARIABLE _opd_result)
if(NOT _opd_result EQUAL 0)
  message(FATAL_ERROR "Opus source verification failed")
endif()
function(_opd_add_codec)
  # Function-local guard against upstream writing a global default build type.
  # Do not change whisper's flags/options or the parent's cache configuration.
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-fast-math")
  foreach(_on OPUS_FIXED_POINT OPUS_DISABLE_INTRINSICS OPUS_HARDENING OPUS_STACK_PROTECTOR OPUS_VAR_ARRAYS)
    set(${_on} ON CACHE BOOL "OpenPendant decoder" FORCE)
    set(${_on} ON)
  endforeach()
  foreach(_off BUILD_SHARED_LIBS BUILD_TESTING OPUS_BUILD_SHARED_LIBRARY OPUS_BUILD_TESTING
      OPUS_BUILD_PROGRAMS OPUS_BUILD_FRAMEWORK OPUS_ENABLE_FLOAT_API OPUS_FAST_MATH
      OPUS_FLOAT_APPROX OPUS_DRED OPUS_OSCE OPUS_DEEP_PLC OPUS_CUSTOM_MODES
      OPUS_USE_ALLOCA OPUS_NONTHREADSAFE_PSEUDOSTACK OPUS_FUZZING OPUS_CHECK_ASM
      OPUS_ASSERTIONS OPUS_FIXED_POINT_DEBUG OPUS_DNN_FLOAT_DEBUG
      OPUS_INSTALL_PKG_CONFIG_MODULE OPUS_INSTALL_CMAKE_CONFIG_MODULE)
    set(${_off} OFF CACHE BOOL "OpenPendant decoder" FORCE)
    set(${_off} OFF)
  endforeach()
  add_subdirectory("${_opd_vendor}" "${CMAKE_CURRENT_BINARY_DIR}/openpendant_opus" EXCLUDE_FROM_ALL)
endfunction()
_opd_add_codec()
get_directory_property(_opd_vla DIRECTORY "${_opd_vendor}" DEFINITION VLA_SUPPORTED)
get_directory_property(_opd_var_arrays DIRECTORY "${_opd_vendor}" DEFINITION OPUS_VAR_ARRAYS)
if(NOT _opd_vla OR NOT _opd_var_arrays OR NOT STACK_PROTECTOR_SUPPORTED)
  message(FATAL_ERROR "Actual compiler VLA and strong stack protection checks must pass")
endif()
set_target_properties(opus PROPERTIES C_VISIBILITY_PRESET hidden)
target_compile_options(opus PRIVATE -fno-fast-math -fno-strict-aliasing -ffunction-sections -fdata-sections)
add_custom_target(openpendant_opus_source_check
  COMMAND "${CMAKE_COMMAND}" "-DOPUS_VENDOR=${_opd_vendor}"
  -P "${CMAKE_CURRENT_LIST_DIR}/verify_opus_sources.cmake" VERBATIM)
add_dependencies(opus openpendant_opus_source_check)
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/../assets/OPUS_COPYING.txt" _opd_license)
if(NOT _opd_license STREQUAL "01e1167d54a096d123cf6dfbbeb19587278845c6481d2d66d545669846079551")
  message(FATAL_ERROR "Packaged Opus license differs")
endif()
add_library(openpendant_opus_decode SHARED opus_packet_decode.c opus_packet_decode_jni.c)
target_compile_options(openpendant_opus_decode PRIVATE -Wall -Wextra -Werror -fvisibility=hidden)
target_link_libraries(openpendant_opus_decode PRIVATE opus)
target_link_options(openpendant_opus_decode PRIVATE "-Wl,-z,max-page-size=16384" "-Wl,--gc-sections" "-Wl,--exclude-libs,ALL")
