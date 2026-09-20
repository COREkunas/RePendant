/* Fixed, shared one-shot external NAND diagnostics. No boot hook or driver. */
#ifndef PENDANT_NAND_ID_H
#define PENDANT_NAND_ID_H

#include <stdbool.h>
#include <stdint.h>
#include "nand_qualify.h"

#define NAND_ID_MAX_ATTEMPTS 6U
#define NAND_ID_NO_SELECTION UINT8_MAX
#define NAND_PAGE0_MAX_TRANSFERS 21U
#define NAND_PAGE0_MAIN_BYTES 4096U
#define NAND_PAGE0_CACHE_BYTES 4100U
#define NAND_MARKER_MAX_TRANSFERS 38U
#define NAND_MARKER_RAW_BYTES 4352U
#define NAND_MARKER_CACHE_BYTES 4356U
#define NAND_BLOCK1024_ROWS 64U
#define NAND_BLOCK1024_MAX_TRANSFERS 1487U
#define NAND_BLOCK1024_END_WORDS 44U

/* Closed, internal-program operation metadata; no caller-selected SPI access. */
enum nand_feature_operation {
	NAND_FEATURE_OP_ID = 0,
	NAND_FEATURE_OP_INITIAL_STATUS,
	NAND_FEATURE_OP_CONFIG,
	NAND_FEATURE_OP_FINAL_STATUS,
};

enum nand_feature_outcome {
	NAND_FEATURE_NOT_RUN = 0,
	NAND_FEATURE_SNAPSHOT,
	NAND_FEATURE_UNKNOWN_ID,
	NAND_FEATURE_INITIAL_BUSY,
	NAND_FEATURE_ANOMALOUS_FINAL,
	NAND_FEATURE_TRANSFER_ERROR,
	NAND_FEATURE_STOP_ERROR,
	NAND_FEATURE_PRECONDITION_ERROR,
	NAND_FEATURE_CLEANUP_ERROR,
};

enum nand_id_outcome {
	NAND_ID_NOT_RUN = 0,
	NAND_ID_CONFIRMED,
	NAND_ID_INCONCLUSIVE,
	NAND_ID_UNEXPECTED,
	NAND_ID_TRANSFER_ERROR,
	NAND_ID_STOP_ERROR,
	NAND_ID_PRECONDITION_ERROR,
};

struct nand_id_attempt {
	enum nand_feature_operation operation;
	uint8_t reg; /* Zero for READ ID, otherwise fixed feature address. */
	uint8_t expected_length;
	bool raw_valid; /* STOP confirmed and exact DMA amounts; inspect rc too. */
	uint8_t cs_index;
	uint8_t cs_pin; /* Port-1 pin number, not encoded GPIO number. */
	uint8_t rx[4];
	uint32_t tx_amount;
	uint32_t rx_amount;
	int rc;
};

struct nand_id_result {
	int rc;
	enum nand_id_outcome outcome;
	uint8_t attempt_count;
	uint8_t selected_index;
	uint8_t selected_pin;
	struct nand_id_attempt attempts[NAND_ID_MAX_ATTEMPTS];
	uint32_t elapsed_ms;
	bool clock_stopped;
	bool bus_released;
	bool cs_high;
	bool fault_latched;
	bool aux_configured;
	bool aux_high; /* Output configuration plus HIGH latch, not a voltmeter. */
};

struct nand_feature_result {
	struct nand_id_result common;
	enum nand_feature_outcome outcome;
	bool id_valid;
	bool initial_valid;
	bool config_valid;
	bool final_valid;
	uint8_t initial;
	uint8_t config;
	uint8_t final;
	bool changed;
	bool initial_busy;
	bool final_busy;
};

enum nand_page0_operation {
	NAND_PAGE0_ID = 0, NAND_PAGE0_INITIAL, NAND_PAGE0_CONFIG, NAND_PAGE0_PRE,
	NAND_PAGE0_LOAD, NAND_PAGE0_READY, NAND_PAGE0_CACHE_FIRST, NAND_PAGE0_MIDDLE,
	NAND_PAGE0_CACHE_SECOND, NAND_PAGE0_POST, NAND_PAGE0_CONFIG_AFTER, NAND_PAGE0_FINAL,
};

enum nand_page0_outcome {
	NAND_PAGE0_NOT_RUN = 0, NAND_PAGE0_VERIFIED, NAND_PAGE0_UNKNOWN_ID,
	NAND_PAGE0_PRECHECK_ERROR, NAND_PAGE0_READY_TIMEOUT, NAND_PAGE0_ECC_UNCORRECTABLE,
	NAND_PAGE0_ECC_UNSUPPORTED, NAND_PAGE0_POSTCHECK_ERROR, NAND_PAGE0_CACHE_MISMATCH,
	NAND_PAGE0_TRANSFER_ERROR, NAND_PAGE0_STOP_ERROR, NAND_PAGE0_PRECONDITION_ERROR,
	NAND_PAGE0_CLEANUP_ERROR,
};

struct nand_page0_attempt {
	enum nand_page0_operation operation;
	uint8_t cs_pin;
	uint8_t reg;
	uint16_t expected_length;
	uint32_t tx_amount;
	uint32_t rx_amount;
	bool raw_valid; /* Control bytes only; cache payload/header never copied here. */
	uint8_t rx[4];
	int rc;
};

struct nand_page0_result {
	struct nand_id_result common;
	enum nand_page0_outcome outcome;
	struct nand_page0_attempt attempts[NAND_PAGE0_MAX_TRANSFERS];
	uint8_t polls;
	bool id_valid;
	bool initial_valid; uint8_t initial;
	bool config_valid; uint8_t config;
	bool pre_valid; uint8_t pre;
	bool page_started;
	bool ready_valid; uint8_t ready;
	bool ecc_valid; uint8_t ecc_code;
	bool cache1_valid;
	bool cache2_valid;
	bool middle_valid; uint8_t middle;
	bool post_valid; uint8_t post;
	bool config_after_valid; uint8_t config_after;
	bool final_valid; uint8_t final;
	bool crc_valid; uint32_t crc1; uint32_t crc2;
	bool match;
	bool data_valid;
	bool ram_scrubbed;
	bool ready_unknown;
};

struct nand_id_state {
	bool attempted;
	bool busy;
	bool fault_latched;
	bool clock_stopped;
	bool bus_released;
	bool cs_configured;
	bool cs_high;
	bool aux_configured;
	bool aux_high;
};

enum nand_marker_operation {
	NAND_MARKER_ID = 0, NAND_MARKER_INITIAL, NAND_MARKER_LOCK, NAND_MARKER_CONFIG,
	NAND_MARKER_PRE, NAND_MARKER_OFF, NAND_MARKER_RAW_CONFIG, NAND_MARKER_RAW_PRE,
	NAND_MARKER_LOAD1, NAND_MARKER_READY1, NAND_MARKER_CACHE1, NAND_MARKER_POST1,
	NAND_MARKER_LOAD2, NAND_MARKER_READY2, NAND_MARKER_CACHE2, NAND_MARKER_POST2,
	NAND_MARKER_RAW_FINAL, NAND_MARKER_RESTORE_GUARD, NAND_MARKER_RESTORE_WRITE,
	NAND_MARKER_RESTORE_CONFIG, NAND_MARKER_LOCK_AFTER, NAND_MARKER_FINAL,
};
enum nand_marker_outcome {
	NAND_MARKER_NOT_RUN = 0, NAND_MARKER_VERIFIED, NAND_MARKER_UNKNOWN_ID,
	NAND_MARKER_PRECHECK_ERROR, NAND_MARKER_CONFIG_ERROR, NAND_MARKER_READY_TIMEOUT,
	NAND_MARKER_POSTCHECK_ERROR, NAND_MARKER_COPY_MISMATCH, NAND_MARKER_TRANSFER_ERROR,
	NAND_MARKER_STOP_ERROR, NAND_MARKER_PRECONDITION_ERROR, NAND_MARKER_CLEANUP_ERROR,
	NAND_MARKER_RESTORE_ERROR,
};
enum nand_marker_restore_state {
	NAND_MARKER_RESTORE_NOT_NEEDED = 0, NAND_MARKER_RESTORE_VERIFIED,
	NAND_MARKER_RESTORE_SKIPPED_UNSAFE, NAND_MARKER_RESTORE_FAILED,
};
struct nand_marker_attempt {
	enum nand_marker_operation operation;
	uint8_t cs_pin; uint8_t reg; uint16_t expected_length;
	uint32_t tx_amount; uint32_t rx_amount;
	bool raw_valid; uint8_t rx[4]; int rc;
};
struct nand_marker_result {
	struct nand_id_result common;
	enum nand_marker_outcome outcome;
	int primary_rc; enum nand_marker_outcome primary_outcome;
	int restore_rc; enum nand_marker_restore_state restore_state;
	struct nand_marker_attempt attempts[NAND_MARKER_MAX_TRANSFERS];
	uint8_t polls1; uint8_t polls2;
	bool id_valid;
	bool initial_valid; uint8_t initial;
	bool lock_valid; uint8_t lock;
	bool config_valid; uint8_t config;
	bool pre_valid; uint8_t pre;
	bool off_attempted; bool off_confirmed;
	bool raw_config_valid; uint8_t raw_config;
	bool raw_pre_valid; uint8_t raw_pre;
	bool page1_started; bool ready1_valid; uint8_t ready1; bool cache1_valid;
	bool post1_valid; uint8_t post1;
	bool page2_started; bool ready2_valid; uint8_t ready2; bool cache2_valid;
	bool post2_valid; uint8_t post2;
	bool raw_final_valid; uint8_t raw_final;
	bool restore_attempted; bool restore_guard_valid; uint8_t restore_guard;
	bool restore_write_attempted;
	bool restore_config_valid; uint8_t restore_config;
	bool lock_after_valid; uint8_t lock_after;
	bool final_valid; uint8_t final;
	bool restored; bool restore_required;
	bool current_config_valid; uint8_t current_config; bool config_unknown;
	bool crc_valid; uint32_t crc1; uint32_t crc2; bool match;
	bool markers_valid; uint16_t marker1; uint16_t marker2; bool marker_ff;
	bool data_valid; bool ram_scrubbed; bool ready_unknown;
};

/* Fixed block export only. Values 0..12 retain marker outcome meanings. */
enum nand_block1024_sink_status {
	NAND_BLOCK1024_SINK_OK = 0, NAND_BLOCK1024_USB_ERROR = 13,
	NAND_BLOCK1024_HOST_ABORT = 14, NAND_BLOCK1024_OVERALL_TIMEOUT = 15,
};
struct nand_block1024_ack {
	enum nand_block1024_sink_status status;
	int rc; /* Original transport return code, not an errno-based classification. */
	bool frame_sent; /* Complete frame accepted before ACK failure, if any. */
};
struct nand_block1024_sink {
	void *user;
	/* Metadata only, bounded and nonblocking. Failure aborts before first START. */
	int (*start)(void *user, int64_t absolute_data_deadline_ms);
	struct nand_block1024_ack (*begin)(void *user, const uint32_t fields[16],
					 int64_t absolute_data_deadline_ms);
	struct nand_block1024_ack (*page)(void *user, uint16_t sequence,
		const uint32_t fields[16], const uint8_t raw[NAND_MARKER_RAW_BYTES],
		int64_t absolute_data_deadline_ms);
};
/* Exact END word order. Signed rc values are their 32-bit representation.
 * This result contains metadata only; callbacks must never retain raw pointers.
 * OK means complete frame + exact durable ACK + no pending RX. The application
 * supplies a bounded callback and retains its external admission reservation.
 */
enum nand_block1024_field {
	NB_RC, NB_OUTCOME, NB_PRIMARY_RC, NB_PRIMARY_OUTCOME, NB_RESTORE_RC,
	NB_RESTORE_STATE, NB_ROWS_COMPARED, NB_ROWS_SENT, NB_ROWS_ACKED,
	NB_RAW_BYTES_ACKED, NB_TRANSFERS, NB_POLLS1, NB_POLLS2, NB_OFF_ATTEMPTED,
	NB_OFF_CONFIRMED, NB_RESTORE_ATTEMPTED, NB_RESTORE_WRITE_ATTEMPTED,
	NB_RESTORE_CONFIG_VALID, NB_RESTORE_CONFIG, NB_LOCK_BEFORE_VALID,
	NB_LOCK_BEFORE, NB_LOCK_AFTER_VALID, NB_LOCK_AFTER, NB_FINAL_VALID, NB_FINAL,
	NB_RESTORED, NB_RESTORE_REQUIRED, NB_CURRENT_CONFIG_VALID, NB_CURRENT_CONFIG,
	NB_CONFIG_UNKNOWN, NB_BLOCK_CRC_VALID, NB_BLOCK_CRC32, NB_RAM_SCRUBBED,
	NB_READY_UNKNOWN, NB_FAULT, NB_STOPPED, NB_BUS_RELEASED, NB_CS_CONFIGURED,
	NB_CS_HIGH, NB_AUX_CONFIGURED, NB_AUX_HIGH, NB_ELAPSED_MS, NB_USB_RC,
	NB_TRANSPORT_QUARANTINED,
};
struct nand_block1024_result {
	uint32_t end[NAND_BLOCK1024_END_WORDS];
	struct nand_id_state state;
};

/* Caller must hold the application's shared admission reservation, excluding
 * microphone/retained PCM, enrollment and recovery. Internal admission also
 * rejects concurrent calls and permits only one admitted probe per boot.
 * Pins remain untouched until preconditions pass. The function never logs.
 * Poll budgets are finite; scheduler/interrupt stalls are not a wall-time SLA.
 */
int nand_id_probe(struct nand_id_result *result);
/* Fixed CS P1.15, three IDs then C0/B0/C0; consumes the same allowance as ID. */
int nand_feature_probe(struct nand_feature_result *result);
/* Fixed row0/main4096 only, two cache copies; CRC/exact-match metadata, no bytes. */
int nand_page0_probe(struct nand_page0_result *result);
/* Fixed block1024 raw marker snapshot; B0 temporarily00, restored10 or fault-held. */
int nand_marker_probe(struct nand_marker_result *result);
/* Fixed CS15, rows65536..65599, two fresh raw copies per row. Temporarily B0=00;
 * restores B0=10 independently of callback errors, or retains a fault. No A0
 * writes, array program/erase, reset or generic SPI. Caller sends END afterward.
 * Ordinary shared busy/already-used refusal is internal outcome10 without a
 * fabricated fault; caller must not send END for that admission refusal. Entry
 * contention returns -EBUSY without touching result (it may be the active one).
 */
int nand_block1024_probe(struct nand_block1024_result *result,
			 const struct nand_block1024_sink *sink);
void nand_id_get_state(struct nand_id_state *state);

#endif
