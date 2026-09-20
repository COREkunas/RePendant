# SPDX-License-Identifier: Apache-2.0

# nRF5340 uses the UICR branch to open debug in SystemInit. The separate
# NRF_APPROTECT_DISABLE option only exists on newer nRF54 parts.
# Initial SWD provisioning must preserve/set both applicable UICR words to
# their documented Unprotected value; this build does not write UICR.
get_target_property(open_pendant_conf ${ZCMAKE_APPLICATION} CONFIG)
# This application requests reboot into MCUboot; it does not have an
# application-level image manager. Do not pass the SDK's image-manager-only
# Kconfig assignments, whose dependencies are deliberately disabled.
if(ZCMAKE_APPLICATION STREQUAL "usb_firmware")
  string(REGEX REPLACE "(^|\n)CONFIG_(UPDATEABLE_IMAGE_NUMBER|MCUBOOT_UPDATE_FOOTER_SIZE)=[^\n]*" "\\1"
    open_pendant_conf "${open_pendant_conf}")
endif()
string(REGEX REPLACE "(^|\n)CONFIG_NRF_APPROTECT_(USE_UICR|LOCK|USER_HANDLING)=[^\n]*" "\\1"
  open_pendant_conf "${open_pendant_conf}")
if(ZCMAKE_APPLICATION STREQUAL "usb_firmware" OR ZCMAKE_APPLICATION STREQUAL "mcuboot")
  string(REGEX REPLACE "(^|\n)CONFIG_NRF_SECURE_APPROTECT_(USE_UICR|LOCK|USER_HANDLING)=[^\n]*" "\\1"
    open_pendant_conf "${open_pendant_conf}")
endif()
set_property(TARGET ${ZCMAKE_APPLICATION} PROPERTY CONFIG "${open_pendant_conf}")
set_config_bool(${ZCMAKE_APPLICATION} CONFIG_NRF_APPROTECT_USE_UICR y)
set_config_bool(${ZCMAKE_APPLICATION} CONFIG_NRF_APPROTECT_LOCK n)
if(ZCMAKE_APPLICATION STREQUAL "usb_firmware" OR ZCMAKE_APPLICATION STREQUAL "mcuboot")
  set_config_bool(${ZCMAKE_APPLICATION} CONFIG_NRF_SECURE_APPROTECT_USE_UICR y)
  set_config_bool(${ZCMAKE_APPLICATION} CONFIG_NRF_SECURE_APPROTECT_LOCK n)
endif()
