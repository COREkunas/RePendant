/* Fixed destructive qualification of already-preserved block1024 only. */
#ifndef PENDANT_NAND_QUALIFY_H
#define PENDANT_NAND_QUALIFY_H
#include <stdint.h>
#define NAND_QUALIFY_MAX_STARTS 3229U
#define NAND_QUALIFY_DATA_MS 180000U
#define NAND_QUALIFY_PROGRAM_BYTES 4099U
#define NAND_QUALIFY_RESULT_WORDS 98U
enum nand_qualify_field {
	NQ_RC = 0,
	NQ_OUTCOME = 1,
	NQ_PRIMARY_RC = 2,
	NQ_PRIMARY_OUTCOME = 3,
	NQ_PHASE = 4,
	NQ_FAILED_PHASE = 5,
	NQ_ROW_INDEX = 6,
	NQ_TRANSFERS = 7,
	NQ_PREIMAGE_ROWS = 8,
	NQ_BLANK_ROWS = 9,
	NQ_VERIFY_READS = 10,
	NQ_PREIMAGE_POLLS = 11,
	NQ_BLANK_POLLS = 12,
	NQ_VERIFY_POLLS = 13,
	NQ_MARKER_POLLS = 14,
	NQ_ERASE_POLLS = 15,
	NQ_PROGRAM_POLLS = 16,
	NQ_ID_VALID = 17,
	NQ_INITIAL_VALID = 18,
	NQ_INITIAL_C0 = 19,
	NQ_INITIAL_A0_VALID = 20,
	NQ_INITIAL_A0 = 21,
	NQ_INITIAL_B0_VALID = 22,
	NQ_INITIAL_B0 = 23,
	NQ_PREIMAGE_HASH_VALID = 24,
	NQ_PREIMAGE_MATCH = 25,
	NQ_MARKER_BEFORE_VALID = 26,
	NQ_MARKER_BEFORE = 27,
	NQ_A0_CHANGE_ATTEMPTED = 28,
	NQ_A0_CHANGE_CONFIRMED = 29,
	NQ_A0_CURRENT_VALID = 30,
	NQ_A0_CURRENT = 31,
	NQ_A0_RESTORE_REQUIRED = 32,
	NQ_A0_RESTORE_ATTEMPTED = 33,
	NQ_A0_RESTORE_VALID = 34,
	NQ_A0_RESTORE_VALUE = 35,
	NQ_A0_RESTORE_STATE = 36,
	NQ_A0_RESTORE_RC = 37,
	NQ_B0_OFF_STARTS = 38,
	NQ_B0_ON_STARTS = 39,
	NQ_B0_CURRENT_VALID = 40,
	NQ_B0_CURRENT = 41,
	NQ_B0_RESTORE_REQUIRED = 42,
	NQ_B0_RESTORE_ATTEMPTED = 43,
	NQ_B0_RESTORE_VALID = 44,
	NQ_B0_RESTORE_VALUE = 45,
	NQ_B0_RESTORE_STATE = 46,
	NQ_B0_RESTORE_RC = 47,
	NQ_WREN_STARTS = 48,
	NQ_WEL_VALID = 49,
	NQ_WEL = 50,
	NQ_WRDI_ATTEMPTED = 51,
	NQ_WRDI_VERIFIED = 52,
	NQ_DISARM_RC = 53,
	NQ_ERASE_STARTED = 54,
	NQ_ERASE_TRANSFER_VALID = 55,
	NQ_ERASE_READY_VALID = 56,
	NQ_ERASE_STATUS = 57,
	NQ_ERASE_COMPLETED = 58,
	NQ_PROGRAM_LOAD_STARTED = 59,
	NQ_PROGRAM_LOAD_VALID = 60,
	NQ_PROGRAM_LOAD_TX = 61,
	NQ_PROGRAM_LOAD_RX = 62,
	NQ_PROGRAM_STARTED = 63,
	NQ_PROGRAM_TRANSFER_VALID = 64,
	NQ_PROGRAM_READY_VALID = 65,
	NQ_PROGRAM_STATUS = 66,
	NQ_PROGRAM_COMPLETED = 67,
	NQ_PATTERN_READS = 68,
	NQ_MARKER_AFTER_VALID = 69,
	NQ_MARKER_AFTER = 70,
	NQ_ARRAY_MAY_HAVE_CHANGED = 71,
	NQ_FINAL_VALID = 72,
	NQ_FINAL_C0 = 73,
	NQ_STOPPED = 74,
	NQ_BUS_RELEASED = 75,
	NQ_FAULT = 76,
	NQ_RAM_SCRUBBED = 77,
	NQ_READY_UNKNOWN = 78,
	NQ_CS_CONFIGURED = 79,
	NQ_CS_HIGH = 80,
	NQ_AUX_CONFIGURED = 81,
	NQ_AUX_HIGH = 82,
	NQ_ELAPSED_MS = 83,
	NQ_OBSERVER_RC = 84,
	NQ_BLOCK_QUARANTINED = 85,
	NQ_ATTEMPTED = 86,
	NQ_BUSY = 87,
	NQ_LAST_OPERATION = 88,
	NQ_LAST_STARTED = 89,
	NQ_LAST_EXPECTED_LENGTH = 90,
	NQ_LAST_TX_AMOUNT = 91,
	NQ_LAST_RX_AMOUNT = 92,
	NQ_LAST_RC = 93,
	NQ_LAST_CONTROL_VALID = 94,
	NQ_LAST_CONTROL = 95,
	NQ_EVENT = 96,
	NQ_EVENT_COUNT = 97,
};
enum nand_qualify_phase {
 NQ_PHASE_NOT_RUN=0, NQ_PHASE_PREFLIGHT, NQ_PHASE_PREIMAGE, NQ_PHASE_PROTECTION,
 NQ_PHASE_ERASE, NQ_PHASE_BLANK, NQ_PHASE_ECC_ON, NQ_PHASE_PROGRAM,
 NQ_PHASE_VERIFY, NQ_PHASE_MARKER, NQ_PHASE_RESTORE, NQ_PHASE_DONE,
};
enum nand_qualify_outcome {
 NQ_NOT_RUN=0, NQ_VERIFIED, NQ_REFUSED, NQ_ID_ERROR, NQ_PRECHECK_ERROR,
 NQ_HASH_ERROR, NQ_PREIMAGE_ERROR, NQ_MARKER_ERROR, NQ_PROTECTION_ERROR,
 NQ_WEL_ERROR, NQ_ERASE_ERROR, NQ_BLANK_ERROR, NQ_PROGRAM_ERROR,
 NQ_READBACK_ERROR, NQ_ECC_ERROR, NQ_CONFIG_ERROR, NQ_READY_TIMEOUT,
 NQ_TRANSFER_ERROR, NQ_STOP_ERROR, NQ_OBSERVER_ERROR, NQ_DEADLINE_ERROR,
 NQ_RESTORE_ERROR, NQ_CLEANUP_ERROR,
};
enum nand_qualify_restore_state {
 NQ_RESTORE_NOT_NEEDED=0, NQ_RESTORE_VERIFIED, NQ_RESTORE_SKIPPED_UNSAFE,
 NQ_RESTORE_FAILED,
};
enum nand_qualify_event {
 NQ_EVENT_PHASE=1, NQ_EVENT_PROGRESS, NQ_EVENT_BEFORE_ERASE_WREN,
 NQ_EVENT_BEFORE_ERASE_EXECUTE, NQ_EVENT_AFTER_ERASE,
 NQ_EVENT_BEFORE_PROGRAM_WREN, NQ_EVENT_BEFORE_PROGRAM_LOAD,
 NQ_EVENT_BEFORE_PROGRAM_EXECUTE, NQ_EVENT_AFTER_PROGRAM,
};
struct nand_qualify_result { uint32_t words[NAND_QUALIFY_RESULT_WORDS]; };
struct nand_qualify_observer {
 void *user;
 /* Mandatory, bounded/nonblocking. No bus actions or retained pointers.
  * start is called once before first START; check before every data START.
  * Neither is called during independent NAND restoration. */
 int (*start)(void *user, int64_t absolute_data_deadline_ms);
 int (*check)(void *user, int64_t absolute_data_deadline_ms);
 /* Synchronous metadata only; integration must bound write+ACK by2s and the
  * passed absolute deadline. Nonzero aborts further data work, never cleanup.
  * Exact automatic host journal ACK, not a new user permission request. */
 int (*event)(void *user, const struct nand_qualify_result *snapshot,
              int64_t absolute_data_deadline_ms);
};
/* No parameters select addresses/opcodes/pattern. One admitted attempt per boot.
 * Caller owns mic/pairing/recovery admission. Entry-busy leaves result untouched;
 * ordinary refused-before-START is not a fabricated hardware fault.
 * May ERASE exactly block1024, then PROGRAM exactly row65536 once; no rollback.
 * Exact pinned preimage is required before either A0 change or WREN.
 * STOP/unknown-NAND faults retain ownership and forbid blind cleanup.
 * Caller handles metadata transport/quarantine after return; no raw export. */
int nand_block1024_qualify(struct nand_qualify_result *result,
                         const struct nand_qualify_observer *observer);
#endif

