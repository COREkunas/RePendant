/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OPENPENDANT_NAND_OWNED_PHY_H
#define OPENPENDANT_NAND_OWNED_PHY_H
#include <stdatomic.h>
#include "owned_volume_descriptor.h"

/* Internal closed physical service, UNLINKED. No USB/BLE arbitrary programmer.
 * Legacy descriptors fix all32map+2control members. The separate fixed large
 * extent constructor admits only blocks0..1535. Neither constructor opens the
 * bus or grants mutation authority. The trusted
 * authorization provider independently checks current ownership, exact consumed
 * mutation lease and power admission before WREN and EXECUTE/ERASE. A positive
 * voltage claim cannot be manufactured here. Reads require ownership too.
 * One permanent context per peripheral lifetime; never copy/reset after init or
 * fault. DMA is exclusively into its buffers, never the caller's spans. No heap.
 * A diagnosed P/E failure may return MEDIA after readiness/STOP/lock restoration;
 * all other failed physical work is sticky fault, never a retry authorization.
 */
enum nop_result { NOP_OK=0,NOP_MEDIA=1,NOP_ARGUMENT=-1,NOP_FAULT=-2,NOP_BUSY=-3,NOP_DENIED=-4 };
enum nop_access { NOP_READ=1,NOP_PROGRAM=2,NOP_ERASE=3 };
struct nop_transfer_result { uint32_t started,stopped,tx_amount,rx_amount; };
struct nop_port {
 void *user;
 uint64_t (*now_ms)(void*);
 void (*wait_us)(void*,uint32_t);
 int (*authorize)(void*,uint32_t access,uint32_t physical_row,uint64_t deadline);
 /* HAL owns shared resource reservation; open must reject unexpected pins or
  * peripheral ownership without repairing it. No active microphone admission
  * is implied: future composite owner must explicitly coordinate both services.
  * transfer0 proves exact amounts, CS allhigh, STOP, and default125k restored.
  * Nonzero with stopped0 MAY retain DMA; never touch/reuse its spans afterward.
  * These low-level hooks are private integration, never exposed to host input. */
 int (*open)(void*,uint64_t deadline);
 int (*transfer)(void*,const uint8_t*,uint8_t*,uint32_t length,uint32_t fast,
                 uint64_t deadline,struct nop_transfer_result*);
 int (*close)(void*,uint64_t deadline);
};
struct nand_owned_phy {
 atomic_uint gate;
 uint32_t magic,opened,fault,stopped,starts,a0,b0,status,ready_known,media_flags;
 uint32_t fault_line,last_opcode,last_row;
 uint32_t wel_observed,verified_programs,verify_mismatch;
 uint64_t last_now,deadline;
 uint32_t blocks[34];
 /* Zero = legacy exact table. Nonzero = explicit fixed large recording
  * constructor only; NOT inferred from a legacy descriptor or host address. */
 uint32_t recording_extent;
 struct nop_port port;
 _Alignas(4) uint8_t tx[4356],rx[4356];
};
int nand_owned_phy_init(struct nand_owned_phy*,const struct owned_volume_decoded*,const struct nop_port*);
int nand_owned_phy_init_recording_extent(struct nand_owned_phy*,const struct nop_port*);
/* Profile3: complete 2048-block recording volume. Separate trusted constructor;
 * no legacy descriptor or diagnostic request can enlarge its own scope. */
int nand_owned_phy_init_full_recording(struct nand_owned_phy*,const struct nop_port*);
/* Isolated engineering constructor: read markers throughout 2048 blocks,
 * mutate only the immutable capacity-probe sample. Never a recording profile. */
int nand_owned_phy_init_capacity_probe(struct nand_owned_phy*,const struct nop_port*);
int nand_capacity_probe_block(uint32_t block);
int nand_owned_phy_open(struct nand_owned_phy*,uint64_t deadline);
int nand_owned_phy_read(struct nand_owned_phy*,uint32_t row,uint8_t main[4096],uint32_t *ecc,uint64_t deadline);
/* Fresh array read/ECC, then only the bounded main-area column range.
 * No cached-array reuse; checked program verification remains full-page. */
int nand_owned_phy_read_range(struct nand_owned_phy*,uint32_t row,uint32_t column,uint32_t bytes,uint8_t *out,uint32_t *ecc,uint64_t deadline);
int nand_owned_phy_raw(struct nand_owned_phy*,uint32_t row,uint8_t raw[4352],uint64_t deadline);
int nand_owned_phy_program(struct nand_owned_phy*,uint32_t row,const uint8_t main[4096],uint64_t deadline);
/* Engineering trial only: fresh full-page array readback is mandatory before
 * success. Records unexpected WEL, clears it with WRDI only while ready, and
 * never retries PROGRAM EXECUTE. Existing strict program API is unchanged. */
int nand_owned_phy_program_checked(struct nand_owned_phy*,uint32_t row,const uint8_t main[4096],uint64_t deadline);
int nand_owned_phy_erase(struct nand_owned_phy*,uint32_t block,uint64_t deadline);
/* Separate engineering API: require fresh raw FF readback of every main+OOB
 * byte in all64 pages. One erase command, no retry or deadline renewal. */
int nand_owned_phy_erase_checked(struct nand_owned_phy*,uint32_t block,uint64_t deadline);
int nand_owned_phy_close(struct nand_owned_phy*,uint64_t deadline);
#endif
