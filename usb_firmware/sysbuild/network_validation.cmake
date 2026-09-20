# SPDX-License-Identifier: Apache-2.0

# KCONFIG_ROOT is not imported from the per-image sysbuild cache in Zephyr
# 4.4 (unlike CONF_FILE). ExternalZephyrProject_Cmake() reads CMAKE_ARGS,
# includes IMAGE_CONF_SCRIPT in the same scope, then invokes child CMake.
# Pass the root explicitly at that point; no SDK source changes are needed.
list(APPEND CMAKE_ARGS
  "-DKCONFIG_ROOT:FILEPATH=${CMAKE_CURRENT_LIST_DIR}/b0n/Kconfig")
