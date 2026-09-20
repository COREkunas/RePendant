/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OPENPENDANT_RECORDING_CONTROL_PROBE_NRF_H
#define OPENPENDANT_RECORDING_CONTROL_PROBE_NRF_H
#include "recording_control_probe.h"
struct cp_nrf_owner {
 void *user;
 int (*acquire)(void *,uint64_t deadline);
 /* Release/cleanup authorization must work under independent close deadline,
  * not renew NAND data admission or require an expired host session. */
 int (*release)(void *,uint64_t deadline);
 int (*check)(void *,const uint8_t device[16],const uint8_t descriptor[32],uint64_t deadline);
 int (*record)(void *,const uint8_t metadata[CP_RECORD_BYTES],uint64_t deadline);
};
/* First failure only, public metadata. No payload/header bytes or SPI response
 * dump. Amounts valid only after STOP. UINT32_MAX row means no loaded page. */
struct cp_nrf_fault {
 uint32_t valid,stage,opcode,row,bytes,fast,started,ended,stopped,amounts_valid;
 uint32_t tx_bytes,rx_bytes,frequency,elapsed_cycles,cycle_hz;
 int32_t rc;
};
enum cp_nrf_stage { CP_NRF_OPEN=1,CP_NRF_ADMISSION=2,CP_NRF_SHAPE=3,
 CP_NRF_START=4,CP_NRF_END=5,CP_NRF_STOP=6,CP_NRF_COUNTS=7,CP_NRF_CLOSE=8 };
/* Bind once, no hardware access. Context/outport/owner permanent or copied as
 * documented; no rebind/reset API. Only the fixed four read opcodes can START. */
int cp_nrf_bind(struct control_probe *,const struct cp_nrf_owner *,struct cp_port *);
int cp_nrf_get_fault(struct cp_nrf_fault *); /* Cached only; never touches MMIO. */
#endif
