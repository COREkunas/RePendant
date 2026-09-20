# SPDX-License-Identifier: Apache-2.0

# The parent application is added before SDK modules. All SDK images have
# been added before this per-application sysbuild file is processed.
add_overlay_dts(${DEFAULT_IMAGE}
  ${CMAKE_CURRENT_LIST_DIR}/sysbuild/app_partitions.overlay)

# These scripts execute after SDK pre-CMake configuration. They remove any
# previous assignment of the narrowly overridden settings before adding the
# project choice; duplicate Kconfig assignments are avoided.
foreach(open_pendant_image ${DEFAULT_IMAGE} mcuboot hci_ipc b0n)
  if(TARGET ${open_pendant_image})
    set_property(TARGET ${open_pendant_image} APPEND PROPERTY IMAGE_CONF_SCRIPT
      ${CMAKE_CURRENT_LIST_DIR}/sysbuild/development_access.cmake)
  endif()
endforeach()

if(TARGET mcuboot)
  set_property(TARGET mcuboot APPEND PROPERTY IMAGE_CONF_SCRIPT
    ${CMAKE_CURRENT_LIST_DIR}/sysbuild/single_slot_network.cmake)
endif()

if(TARGET b0n)
  set_property(TARGET b0n APPEND PROPERTY IMAGE_CONF_SCRIPT
    ${CMAKE_CURRENT_LIST_DIR}/sysbuild/network_validation.cmake)
endif()
