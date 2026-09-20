/* Fixed public-object read benchmark. No rate/address/data inputs or boot hook. */
#ifndef OPENPENDANT_NAND_READ_RATE_H
#define OPENPENDANT_NAND_READ_RATE_H
#include "nand_public_object.h"
#define NRR_WORDS 72U
#define NRR_RUNS 13U
#define NRR_MAX_STARTS 598U
#define NRR_DATA_MS 20000U
#define NRR_EVENT_MS 2000U
/* Words0..39 preserve enum npo_field exactly; no raw page bytes. */
enum nrr_field {
 NR_RUN_INDEX=40, NR_RATE_INDEX, NR_RATE_HZ, NR_RATE_REGISTER,
 NR_FINAL_REGISTER, NR_RUN_DEFAULT_VALID, NR_CACHE_STARTS, NR_CACHE_COMPLETED,
 NR_TIMING_VALID, NR_CYCLE_HZ, NR_CACHE0_CYCLES, NR_CACHE1_CYCLES,
 NR_CACHE2_CYCLES, NR_RUN_WALL_MS, NR_RATE_SETS, NR_RATE_RESTORES,
 NR_RC, NR_OUTCOME, NR_ATTEMPTED, NR_RUNS_STARTED, NR_RUNS_VERIFIED,
 NR_RUNS_ACKED, NR_STARTS, NR_ELAPSED_MS, NR_OBSERVER_RC, NR_FAULT,
 NR_STOPPED, NR_BUS_RELEASED, NR_RAM_SCRUBBED, NR_DEFAULT_VALID,
 NR_QUARANTINED, NR_EVENT_COUNT
};
enum nrr_outcome { NRR_NOT_RUN=0, NRR_VERIFIED, NRR_REFUSED, NRR_RUN_ERROR,
 NRR_DEADLINE_ERROR, NRR_OBSERVER_ERROR, NRR_RATE_ERROR, NRR_CLEANUP_ERROR };
struct nand_read_rate_result { uint32_t words[NRR_WORDS]; };
struct nand_read_rate_observer {
 void *user;
 /* Nonblocking lifecycle/cancellation check. Receives original absolute20s
  * data deadline; no observer calls during independent safe restoration. */
 int (*check)(void *user,int64_t absolute_data_deadline_ms);
 /* Metadata only, after one verified run and common safe cleanup. Deadline is
  * min(now+2000, data deadline). EVENT_COUNT increments BEFORE this call;
  * RUNS_VERIFIED includes this run, RUNS_ACKED excludes this pending ACK.
  * Return0 only after exact durable metadata ACK/no pending input. */
 int (*event)(void *user,const struct nand_read_rate_result *result,
              int64_t absolute_event_deadline_ms);
};
/* Caller holds shared mic/pairing/recovery/crypto admission across all runs.
 * One admitted invocation per boot, no automatic retry. Entry contention leaves
 * result untouched. Failed/uncertain session remains transport-quarantined.
 * Before any run RUN_INDEX/RATE_INDEX are UINT32_MAX. Progress OUTCOME=0/RC=0,
 * completed current NPO result is VERIFIED. Successful END OUTCOME=1 and all
 * three counts RUNS_VERIFIED/RUNS_ACKED/EVENT_COUNT=13. QUARANTINED remains1;
 * only the transport wrapper may release after clean END+ACK/no pending RX.
 * Rate-register fields are readbacks; RATE_SETS/RESTORES count successful
 * nondefault entry/return pairs,0 at125k,3/3 for a clean faster-rate run.
 * No mutation, payload export, arbitrary rate or row selection is possible. */
int nand_read_rate_run(struct nand_read_rate_result *result,
                      const struct nand_read_rate_observer *observer);
#endif
