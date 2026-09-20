# SPDX-License-Identifier: Apache-2.0

if(CONFIG_BOARD_OPEN_PENDANT_NRF5340_CPUAPP)
  board_runner_args(jlink "--device=nrf5340_xxaa_app" "--speed=1000")
elseif(CONFIG_BOARD_OPEN_PENDANT_NRF5340_CPUNET)
  board_runner_args(jlink "--device=nrf5340_xxaa_net" "--speed=1000")
endif()

include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)

