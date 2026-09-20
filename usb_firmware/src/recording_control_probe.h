/* SPDX-License-Identifier: Apache-2.0
 * Read-only diagnostic; runtime admission required. Never a mount/adoption interface. */
#ifndef OPENPENDANT_CONTROL_PROBE_CANDIDATE_H
#define OPENPENDANT_CONTROL_PROBE_CANDIDATE_H
#include "owned_control_journal.h"
#include <stdatomic.h>

#define CP_RECORD_BYTES 200U
#define CP_ROWS 128U
#define CP_MAX_STARTS 2697U /* 6 preflight +128*(1+16+1+3)+3 final */
#define CP_BUDGET_MS 30000U
enum cp_rc { CP_OK=0, CP_ARGUMENT=-1, CP_BUSY=-2, CP_CONSUMED=-3,
 CP_ADMISSION=-4, CP_TIME=-5, CP_TRANSFER=-6, CP_STOP_UNKNOWN=-7,
 CP_BASELINE=-8, CP_ECC=-9, CP_HASH=-10, CP_OBSERVER=-11, CP_CLOSE=-12 };
enum cp_class { CP_NOT_VALID=1, CP_CANONICAL=2, CP_CONFLICT=3 };
struct cp_transfer { uint32_t started,stopped,tx_bytes,rx_bytes; };
struct cp_port {
 void *user;
 uint64_t (*now_ms)(void *);
 /* Must check actual device UUID, cached exact descriptor/phase2, exclusive
  * ownership and lifecycle. No settings/NAND writes, auto-repair or adoption. */
 int (*admit)(void *,const uint8_t device[16],const uint8_t descriptor[32],uint64_t);
 /* Open only configures the fixed controller/pins; no NAND command/reset.
  * Failure conservatively retains/quarantines this context (partial acquire
  * cannot be inferred undone). The adapter owns actual release knowledge. */
 int (*open)(void *,uint64_t);
 int (*transfer)(void *,const uint8_t *,uint8_t *,uint32_t,uint32_t fast,
                 uint64_t,struct cp_transfer *);
 /* Close only disables/disconnects STOP-proven controller and releases owner;
  * never change NAND registers. Deadline independent from exhausted data job. */
 int (*close)(void *,uint64_t);
 void (*pause_us)(void *,uint32_t);
 /* Immutable canonical metadata only; synchronous, no retained pointer.
  * Absolute <=2s callback deadline, clipped to original whole-job deadline. */
 int (*record)(void *,const uint8_t metadata[CP_RECORD_BYTES],uint64_t);
};
struct cp_highest {
 uint64_t epoch,previous_epoch,pending_lease;
 uint32_t bank,slot,state,pending_kind,pending_slot,pending_row;
 uint8_t record_digest[32];
};
struct cp_result {
 int32_t rc,close_rc,transport_rc;
 uint32_t attempted,opened,rows,records,starts,polls,stopped,ready_known;
 uint32_t released,scrubbed,retained,complete,a0,b0,c0,ecc;
 uint32_t canonical,not_valid,conflicts,predecessor_breaks;
 uint32_t last_row,last_stage; /* stages1ID,2baseline,3load,4poll,5cache,6hash,7record,8close */
 uint64_t elapsed_ms;
 struct cp_highest highest[2]; /* Observations, NEVER authoritative selected root. */
};
struct control_probe {
 atomic_uint gate;
 uint32_t used,open,stopped,ready,scrubbed,starts,backwards;
 uint64_t deadline,last_now,start;
 struct cp_port port;
 struct owned_page_hash hash;
 struct owned_volume_decoded volume;
 struct cp_result result;
 struct ocj_snapshot decoded;
 uint64_t previous_epoch[2];
 uint8_t previous_digest[2][32];
 uint32_t gap[2];
 uint8_t metadata[CP_RECORD_BYTES];
 _Alignas(4) uint8_t tx[4100],rx[4100],control_rx[4];
};
/* Zero-initialized permanent context required. One attempt per context; caller
 * cannot reset/reinitialize after failure. All DMA uses ONLY its owned arrays.
 * On unknown STOP no close, wipe, hash, callback, or reuse of those spans.
 * Expected is trusted cached config, never reconstructed from read NAND.
 * Exact fixed device/volume/generation/descriptor are additionally pinned.
 * Result is public metadata; partial results never confer recovery authority. */
int control_probe_run(struct control_probe *,const uint8_t descriptor[512],
 const struct owned_volume_spec *expected,const struct owned_page_hash *,
 const struct cp_port *,uint64_t absolute_deadline,struct cp_result *);
#endif
