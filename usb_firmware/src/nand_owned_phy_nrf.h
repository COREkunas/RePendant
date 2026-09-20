/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OPENPENDANT_NAND_OWNED_PHY_NRF_H
#define OPENPENDANT_NAND_OWNED_PHY_NRF_H
#include "nand_owned_phy.h"
struct nop_nrf_owner {
 void *user;
 /* Qualified storage/composite reservation + independent supervisor. Failure
  * must prove no lease acquired; timeout/ambiguous callback fences integration.
  * These callbacks never start capture or grant array authority implicitly. */
 int (*acquire)(void*,uint64_t deadline);
 int (*release)(void*,uint64_t deadline);
 int (*authorize)(void*,uint32_t access,uint32_t row,uint64_t deadline);
};
/* Bind once at explicit integration startup, no hardware access. Context has
 * permanent lifetime and must subsequently be initialized with returned port.
 * No diagnostics may share SPIM4 except through this exact common owner. */
int nand_owned_phy_nrf_bind(struct nop_port*,struct nand_owned_phy*,const struct nop_nrf_owner*);
/* Cached transport metadata only. Read after the serialized worker has joined;
 * never reads registers, resets a fault, or exposes page/recording contents. */
struct nop_nrf_diagnostics {
 uint32_t transfers,late_end,late_stop,valid,opcode,bytes,fast,started,stopped,tx_amount,rx_amount;
 int32_t rc;
 uint64_t before_ms,after_ms,deadline;
};
void nand_owned_phy_nrf_diagnostics(struct nop_nrf_diagnostics *out);
#endif
