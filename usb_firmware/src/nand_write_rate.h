/* Unlinked fixed PUBLIC append benchmark. Not a recorder or generic writer. */
#ifndef OPENPENDANT_NAND_WRITE_RATE_H
#define OPENPENDANT_NAND_WRITE_RATE_H
#include <stdint.h>
#define NWR_WORDS 96U
#define NWR_MAX_STARTS 1707U
#define NWR_RESTORE_STARTS 8U
#define NWR_DATA_MS 120000U
#define NWR_MAX_EVENTS 32U
enum nwr_outcome { NWR_NOT_RUN=0,NWR_VERIFIED,NWR_REFUSED,NWR_BINDING_ERROR,
 NWR_ID_ERROR,NWR_STATE_ERROR,NWR_PREIMAGE_ERROR,NWR_OLD_OBJECT_ERROR,
 NWR_NOT_BLANK,NWR_HASH_ERROR,NWR_READBACK_ERROR,NWR_PROGRAM_ERROR,
 NWR_TRANSFER_ERROR,NWR_STOP_ERROR,NWR_READY_TIMEOUT,NWR_OBSERVER_ERROR,
 NWR_DEADLINE_ERROR,NWR_RATE_ERROR,NWR_RESTORE_ERROR,NWR_CLEANUP_ERROR };
enum nwr_phase { NWR_IDLE=0,NWR_PREFLIGHT,NWR_PREIMAGE,NWR_OLD_PRE,
 NWR_PROGRAM0,NWR_PROGRAM1,NWR_OLD_POST,NWR_MARKER,NWR_RESTORE,NWR_DONE };
enum nwr_field {
 NW_RC,NW_OUTCOME,NW_PRIMARY_RC,NW_PRIMARY_OUTCOME,NW_PHASE,NW_EVENT,
 NW_ROW_INDEX,NW_STARTS,NW_PREIMAGE_ROWS,NW_BLANK_TAIL,NW_OLD_PRE_VALID,
 NW_OLD_POST_VALID,NW_MARKER_PRE,NW_MARKER_POST,NW_BINDING_VALID,
 NW_WHOLE_VALID,NW_WREN,NW_WRDI,NW_LOADS,NW_EXECUTES,NW_COMPLETED,
 NW_ARRAY_MAY_CHANGE,NW_A0_DIRTY,NW_B0_DIRTY,NW_A0_VALID,NW_A0,
 NW_B0_VALID,NW_B0,NW_C0_VALID,NW_C0,NW_READY_UNKNOWN,NW_RESTORE_RC,
 NW_RESTORED,NW_STOPPED,NW_BUS_RELEASED,NW_RAM_SCRUBBED,NW_FAULT,
 NW_ATTEMPTED,NW_QUARANTINED,NW_ELAPSED_MS,NW_OBSERVER_RC,NW_EVENT_COUNT,
 NW_DEFAULT_VALID,NW_RATE_REGISTER,NW_RATE_SETS,NW_RATE_RESTORES,
 NW_CYCLE_HZ,NW_ACK_MS,NW_HASH_RC,NW_LAST_OPERATION,NW_LAST_STARTED,
 NW_LAST_TX,NW_LAST_RX,NW_VERIFIED_MASK,NW_RESERVED0,NW_RESERVED1,
 NW_SAMPLE0=56,NW_SAMPLE1=76
};
enum nwr_sample_field { NS_ROW,NS_RATE_HZ,NS_RATE_REGISTER,NS_LOAD_STARTED,
 NS_LOAD_VALID,NS_LOAD_TX,NS_LOAD_RX,NS_LOAD_CYCLES,NS_EXEC_STARTED,
 NS_EXEC_VALID,NS_EXEC_CYCLES,NS_PROGRAM_POLLS,NS_READ0_CYCLES,
 NS_READ1_CYCLES,NS_READ0_VALID,NS_READ1_VALID,NS_SHA_VALID,NS_CRC32,
 NS_ACTIVE_CYCLES,NS_RESERVED };
enum nwr_event { NE_NONE=0,NE_PREFLIGHT,NE_PREIMAGE,NE_OLD_VALID,
 NE_BEFORE_WREN,NE_BEFORE_LOAD,NE_BEFORE_EXEC,NE_PROGRAM_VERIFIED,
 NE_PRESERVED };
struct nand_write_rate_result { uint32_t words[NWR_WORDS]; };
struct nand_write_rate_observer {
 void *user;
 int (*check)(void *user,int64_t absolute_data_deadline_ms);
 /* Metadata only after STOP/known idle. Count increments BEFORE invocation.
  * Deadline=min(now+2000, original120s deadline). Return0 only after durable
  * exact ACK. Host permanently consumes the fixed two-row lease BEFORE run. */
 int (*event)(void *user,const struct nand_write_rate_result *,int64_t deadline);
};
/* No startup hook. Caller holds shared admission. One invocation per boot and
 * one durable host lease for lifetime of these rows; failure NEVER grants a
 * retry, even when bytes look FF. A disabled build binding refuses before bus
 * access. The reviewed fresh preimage must be compiled and pinned for a run.
 * QUARANTINED stays1; only a reviewed transport may clear after clean END ACK.
 * RAM_SCRUBBED reports DMA scratch; all additional CPU scratch is wiped before
 * return (raw comparison scratch also after each completed preimage row).
 * Entry contention leaves active output untouched. No erase exists. */
int nand_write_rate_run(struct nand_write_rate_result *,
                       const struct nand_write_rate_observer *);
#endif
