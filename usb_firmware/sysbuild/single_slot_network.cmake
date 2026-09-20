# SPDX-License-Identifier: Apache-2.0

# NCS 3.4 assigns network image ID -1 when there is no secondary network
# slot. MCUboot nevertheless explicitly supports single-slot nRF5340 serial
# recovery. Give that recovery hook its virtual image ID (1), whose direct
# serial upload destination is 2*1+1 = 3. This does not add flash slots.
if(NOT SB_CONFIG_MCUBOOT_MODE_SINGLE_APP OR NOT SB_CONFIG_NETCORE_APP_UPDATE)
  message(FATAL_ERROR "OpenPendant recovery expects single-app-slot plus network recovery")
endif()
get_target_property(open_pendant_conf ${ZCMAKE_APPLICATION} CONFIG)
string(REGEX REPLACE "(^|\n)CONFIG_MCUBOOT_NETWORK_CORE_IMAGE_NUMBER=[^\n]*" "\\1"
  open_pendant_conf "${open_pendant_conf}")
set_property(TARGET ${ZCMAKE_APPLICATION} PROPERTY CONFIG "${open_pendant_conf}")
set_config_int(${ZCMAKE_APPLICATION} CONFIG_MCUBOOT_NETWORK_CORE_IMAGE_NUMBER 1)
