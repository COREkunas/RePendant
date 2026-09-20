/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OPENPENDANT_NAND_CAPACITY_PROBE_H
#define OPENPENDANT_NAND_CAPACITY_PROBE_H
#include "nand_owned_phy.h"
#define NCP_BUDGET_MS 120000U
struct ncp_result {
 uint32_t attempted,complete,verify_only,stage,markers,bad_blocks,selected_bad;
 uint32_t blocks,programs,reads,bytes,highest_row,stopped,ready,released;
 uint32_t a0,b0,c0,phy_line,phy_fault,opcode,mismatch,verified,wel;
 int32_t rc;
 uint64_t elapsed_ms;
};
struct nand_capacity_probe {
 struct nand_owned_phy phy;
 struct ncp_result result;
 uint8_t bad[256],page[4096],scratch[4352];
};
/* One context/attempt per boot; permanent on uncertainty. No heap, host address
 * or raw data export. All bad-marker reads precede the first erase. */
int nand_capacity_probe_run(struct nand_capacity_probe*,const struct nop_port*,uint64_t,int verify_only);
void nand_capacity_probe_pattern(uint32_t row,uint8_t out[4096]);
#endif
