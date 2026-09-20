# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
# This Zephyr application selects optimization/assertions through Kconfig.
# Do not inherit a third-party CMake Release profile into unrelated modules.
set(CMAKE_BUILD_TYPE "" CACHE STRING "Zephyr configuration owns optimization" FORCE)
set(CMAKE_BUILD_TYPE "")
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../independent_firmware)
list(APPEND EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_SOURCE_DIR}/audio.overlay)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(usb_firmware)
include(${CMAKE_CURRENT_SOURCE_DIR}/opus.cmake)
target_compile_options(app PRIVATE -fstack-usage)
option(OPENPENDANT_RECORDING_PROFILE "Build explicitly enrolled recorder instead of retired raw diagnostics" OFF)
zephyr_get(OPENPENDANT_RECORDING_PROFILE SYSBUILD LOCAL)
option(OPENPENDANT_NATIVE_STORAGE "Use tested native-geometry Dhara recording backend" OFF)
zephyr_get(OPENPENDANT_NATIVE_STORAGE SYSBUILD LOCAL)
option(OPENPENDANT_LONG_CONTROL "Explicit USB-powered long recording controls" OFF)
zephyr_get(OPENPENDANT_LONG_CONTROL SYSBUILD LOCAL)
message(STATUS "OpenPendant recording profile: ${OPENPENDANT_RECORDING_PROFILE}")
if(OPENPENDANT_RECORDING_PROFILE)
if(OPENPENDANT_LONG_CONTROL)
if(NOT OPENPENDANT_NATIVE_STORAGE)
message(FATAL_ERROR "Long controls require the native storage profile")
endif()
target_compile_definitions(app PRIVATE OPENPENDANT_LONG_CONTROL=1)
target_compile_definitions(app PRIVATE OPENPENDANT_BATTERY_DIAGNOSTIC=1)
target_compile_definitions(app PRIVATE OPENPENDANT_BATTERY_MONITOR=1)
target_compile_definitions(app PRIVATE OPENPENDANT_PORTABLE_RECORDING=1)
target_compile_definitions(app PRIVATE OPENPENDANT_STANDALONE_CONTROL=1)
target_compile_definitions(app PRIVATE OPENPENDANT_BATTERY_SYNC=1)
target_compile_definitions(app PRIVATE OPENPENDANT_GAUGE_POR_RECOVERY=1)
target_sources(app PRIVATE src/battery_probe.c src/battery_power.c)
target_sources(app PRIVATE src/long_recording_control_codec.c src/recording_control.c
    src/recording_control_worker.c src/recording_control_ble.c)
endif()
target_compile_definitions(app PRIVATE OPENPENDANT_RECORDING_PROFILE=1 OPENPENDANT_DHARA_TRIAL=1)
target_sources(app PRIVATE src/dhara_trial.c src/nand_capacity_probe.c)
if(OPENPENDANT_NATIVE_STORAGE)
target_compile_definitions(app PRIVATE OPENPENDANT_NATIVE_STORAGE=1)
target_sources(app PRIVATE src/recording_native_store.c src/recording_extent.c src/recording_native_volume.c src/recording_aead_selftest.c)
else()
target_sources(app PRIVATE src/recording_volume.c)
endif()
target_sources(app PRIVATE src/main.c src/mic_commands.c src/mic_diagnostics.c
    src/mic_pcm_store.c src/ble_audio.c src/ble_security.c src/device_telemetry.c src/device_preferences.c
    src/encrypted_segment_header.c src/recording_hpke.c src/recording_hpke_psa.c
    src/owned_page.c src/owned_volume_descriptor.c src/owned_control_record.c
    src/owned_control_journal.c src/owned_dhara.c src/nand_owned_phy.c src/nand_owned_phy_nrf.c
    src/recording_configuration.c src/recording_storage_provider.c src/recording_sync_metadata.c
    src/recording_object_store.c src/recording_pipeline.c
    src/recording_frame_queue.c src/recording_worker.c src/recording_store_bridge.c
    src/recording_capture.c src/recording_runtime.c src/recording_fault_trace.c src/recording_manifest.c
    src/recording_catalog.c src/recording_ble.c src/durable_ble_codec.c
    src/recording_control_probe.c src/recording_control_probe_nrf.c
    src/recording_phy_probe.c src/recording_current_preimage.c
    src/recovery_authorizer.c src/recovery_authorizer_settings.c src/host_permit.c)
set(_dhara "${ZEPHYR_BASE}/../modules/lib/dhara")
target_include_directories(app PRIVATE "${_dhara}/include")
target_sources(app PRIVATE "${_dhara}/dhara/map.c" "${_dhara}/dhara/journal.c" "${_dhara}/dhara/error.c")
add_custom_target(openpendant_dhara_source_check
    COMMAND "${PYTHON_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/verify_dhara_source.py" --source "${_dhara}" VERBATIM)
add_dependencies(app openpendant_dhara_source_check)
else()
target_compile_definitions(app PRIVATE OPENPENDANT_PUBLIC_OBJECT=1 OPENPENDANT_WRITE_RATE=1)
target_sources(app PRIVATE src/main.c src/mic_commands.c src/mic_diagnostics.c
    src/mic_pcm_store.c src/ble_audio.c src/ble_security.c
    src/nand_id.c src/nand_commands.c src/nand_backup_wire.c src/nand_backup_commands.c
    src/nand_qualification_wire.c src/nand_qualification_commands.c
    src/imu_id.c src/imu_commands.c
    src/encrypted_segment_header.c src/recording_hpke.c src/recording_hpke_psa.c
    src/crypto_public_test.c src/crypto_commands.c src/device_telemetry.c
    src/owned_page.c src/nand_public_object.c src/npo_wire.c src/nand_public_object_commands.c
    src/nrr_wire.c src/nand_read_rate_commands.c
    src/nand_write_rate.c src/nand_write_rate_binding.c src/nwr_wire.c src/nand_write_rate_commands.c
    src/opus_benchmark.c src/opus_commands.c
    src/nand_owned_phy.c src/nand_owned_phy_nrf.c src/nand_pool_preserve.c)
endif()
