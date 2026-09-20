/* Fixed PUBLIC test object, not a recorder volume or generic NAND interface. */
#ifndef OPENPENDANT_NAND_PUBLIC_OBJECT_H
#define OPENPENDANT_NAND_PUBLIC_OBJECT_H
#include <stdint.h>
#define NPO_WORDS 40U
#define NPO_MAX_STARTS 1024U
#define NPO_DATA_MS 60000U
enum npo_mode { NPO_WRITE_ONCE=1, NPO_RECOVER_ONLY=2 };
enum npo_outcome { NPO_NOT_RUN=0, NPO_VERIFIED, NPO_REFUSED, NPO_ID_ERROR,
 NPO_STATE_ERROR, NPO_PATTERN_ERROR, NPO_TAIL_NOT_BLANK, NPO_HASH_ERROR,
 NPO_READBACK_ERROR, NPO_PROGRAM_ERROR, NPO_TRANSFER_ERROR, NPO_STOP_ERROR,
 NPO_READY_TIMEOUT, NPO_OBSERVER_ERROR, NPO_DEADLINE_ERROR, NPO_RESTORE_ERROR,
 NPO_CLEANUP_ERROR };
enum npo_phase { NPO_IDLE=0, NPO_PREFLIGHT, NPO_PATTERN, NPO_TAIL,
 NPO_DATA_PROGRAM, NPO_COMMIT_PROGRAM, NPO_RECOVERY, NPO_RESTORE, NPO_DONE };
enum npo_field { NP_RC, NP_OUTCOME, NP_PRIMARY_RC, NP_PRIMARY_OUTCOME,
 NP_MODE, NP_PHASE, NP_TRANSFERS, NP_BLANK_ROWS, NP_PATTERN_VALID,
 NP_DATA_VALID, NP_COMMIT_VALID, NP_COMMITTED, NP_PROGRAM_LOADS,
 NP_PROGRAM_EXECUTES, NP_PROGRAM_COMPLETED, NP_WREN_STARTS, NP_WRDI_STARTS,
 NP_ARRAY_MAY_CHANGE, NP_A0_DIRTY, NP_B0_DIRTY, NP_A0_VALID, NP_A0,
 NP_B0_VALID, NP_B0, NP_C0_VALID, NP_C0, NP_READY_UNKNOWN, NP_RESTORE_RC,
 NP_RESTORED, NP_FAULT, NP_STOPPED, NP_BUS_RELEASED, NP_RAM_SCRUBBED,
 NP_ELAPSED_MS, NP_OBSERVER_RC, NP_EVENT_COUNT, NP_WRITE_INTENT,
 NP_LAST_OPERATION, NP_LAST_STARTED, NP_BLOCK_QUARANTINED };
struct npo_result { uint32_t words[NPO_WORDS]; };
struct npo_observer {
 void *user;
 /* Nonblocking link/cancellation check. No callbacks during restoration. */
 int (*check)(void *user, int64_t deadline_ms);
 /* Fixed metadata checkpoint, <=2s and global deadline. Before the first
  * mutating START, host MUST durably consume the exact one-use WRITE lease.
  * FF-looking pages never grant retry authority across reset. No pointers kept. */
 int (*event)(void *user, const struct npo_result *result, int64_t deadline_ms);
};
/* Caller holds common mic/pairing/recovery/PSA admission. No boot hook.
 * WRITE: fixed public DATA row65537 + COMMIT65538, no erase, no retries.
 * RECOVER: reads fixed pattern/data/commit only, never changes configuration.
 * Any uncertain WRITE invocation consumes its host lease permanently.
 * A committed result is byte/readback evidence, not a power-cut durability test.
 * Common fault/unknown STOP retains caller reservation. Result is metadata only. */
int nand_public_object_run(struct npo_result *result, enum npo_mode mode,
                          const struct npo_observer *observer);
#endif
