/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OPENPENDANT_DHARA_TRIAL_H
#define OPENPENDANT_DHARA_TRIAL_H
#include "nand_owned_phy.h"
#include "dhara/map.h"
#define DT_FIRST_BLOCK 1025U
#define DT_BLOCKS 32U
#define DT_SECTORS 128U
#define DT_BUDGET_MS 120000U
struct dt_result {
 uint32_t attempted,complete,verify_only,stage,bad_mask,capacity,writes,reads,programs,erases;
 uint32_t checked_bytes,stopped,ready,released,last_row,ecc,library_error,a0,b0,c0,starts,phy_line,last_opcode;
 uint32_t wel_observed,verified_programs,verify_mismatch;
 int32_t rc,phy_rc;
 uint64_t elapsed_ms;
};
struct dhara_trial {
 struct dhara_nand nand;
 struct dhara_map map;
 struct nand_owned_phy phy;
 struct dt_result result;
 uint64_t deadline,start;
 uint32_t fault,writable;
 uint8_t metadata[4096],page[4096],scratch[4352];
};
/* Serialized once per boot. Explicit disposable region only; no control banks,
 * settings, keys or internal flash. Fail-stop on media/transport faults rather
 * than implementing a second persistent bad-block retirement system here. */
int dhara_trial_run(struct dhara_trial *,const struct owned_volume_decoded *,
                    const struct nop_port *,uint64_t deadline,int verify_only);
int dt_owns(const struct dhara_nand *);
int dt_is_bad(const struct dhara_nand *,dhara_block_t);
void dt_mark_bad(const struct dhara_nand *,dhara_block_t);
int dt_erase(const struct dhara_nand *,dhara_block_t,dhara_error_t *);
int dt_prog(const struct dhara_nand *,dhara_page_t,const uint8_t *,dhara_error_t *);
int dt_is_free(const struct dhara_nand *,dhara_page_t);
int dt_read(const struct dhara_nand *,dhara_page_t,size_t,size_t,uint8_t *,dhara_error_t *);
int dt_copy(const struct dhara_nand *,dhara_page_t,dhara_page_t,dhara_error_t *);
#endif
