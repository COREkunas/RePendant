# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
# Make the existing custom board available before sysbuild selects images.
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../independent_firmware)
find_package(Sysbuild REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sysbuild LANGUAGES)
