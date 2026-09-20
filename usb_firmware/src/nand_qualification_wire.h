#ifndef OPENPENDANT_NAND_QUALIFICATION_WIRE_H
#define OPENPENDANT_NAND_QUALIFICATION_WIRE_H
/* Fixed metadata-only qualifier. Pure C, no heap, OS, driver or logging.
 * All callbacks are synchronous and nonblocking; no retained caller pointers.
 * The caller installs terminal bypass and exclusive producer reservation first.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define NAND_QUALIFICATION_WIRE_HEADER 48U
#define NAND_QUALIFICATION_WIRE_WORDS 98U
#define NAND_QUALIFICATION_WIRE_BODY 392U
#define NAND_QUALIFICATION_WIRE_EVENTS 256U
#define NAND_QUALIFICATION_WIRE_IO_MAX 64U
#define NAND_QUALIFICATION_WIRE_ATTEMPTS 8192U
#define NAND_QUALIFICATION_WIRE_FRAME_MS 2000U
#define NAND_QUALIFICATION_WIRE_DATA_MS 180000U
enum nand_qualification_wire_rc {
 NAND_QUALIFICATION_WIRE_OK=0, NAND_QUALIFICATION_WIRE_ARGUMENT=-1,
 NAND_QUALIFICATION_WIRE_IO=-2, NAND_QUALIFICATION_WIRE_TIMEOUT=-3,
 NAND_QUALIFICATION_WIRE_PROTOCOL=-4, NAND_QUALIFICATION_WIRE_HOST_ABORT=-5,
 NAND_QUALIFICATION_WIRE_STATE=-6, NAND_QUALIFICATION_WIRE_GLOBAL_TIMEOUT=-7
};
struct nand_qualification_wire_io {
 int (*write)(void*,const uint8_t*,size_t,size_t*);
 int (*read)(void*,uint8_t*,size_t,size_t*);
 uint32_t (*now_ms)(void*);
 void (*yield_us)(void*,uint32_t);
 int (*pending_rx)(void*,bool*);
 void *user;
};
struct nand_qualification_wire {
 struct nand_qualification_wire_io io;
 uint8_t nonce[16],tx_header[48],tx_metadata[392],rx_control[56];
 uint32_t data_deadline_ms;
 uint16_t events_sent,events_acked;
 int last_rc;
 uint8_t host_abort_reason;
 bool initialized,handshake_started,hello_received,deadline_armed;
 bool output_frame_boundary,transport_usable,data_stopped,end_attempted,end_sent;
 bool clean_end_acknowledged,console_reuse_allowed,quarantined;
};
/* accepted/received counts must always be set, <=requested, even on error.
 * An accepted write is a copied PREFIX, not a durable host acknowledgement. */
uint32_t nand_qualification_wire_crc32(const uint8_t*,size_t);
int nand_qualification_wire_init(struct nand_qualification_wire*,const struct nand_qualification_wire_io*);
int nand_qualification_wire_handshake(struct nand_qualification_wire*);
/* Once before first NAND START, never renewal. Absolute <=now+180000ms.
 * Driver owns full int64 deadline; modulo32 comparison covers bounded horizon. */
int nand_qualification_wire_arm_data_deadline(struct nand_qualification_wire*,uint32_t);
/* Nonblocking check before every data START. Pending input outside an ACK
 * window is rejected without consumption/interpretation. Never gates restore. */
int nand_qualification_wire_check(struct nand_qualification_wire*);
int nand_qualification_wire_progress(struct nand_qualification_wire*,const uint32_t[98]);
/* Failed END no ACK and no console return. Clean END requires exact final ACK
 * and no queued RX. Caller repeats pending/lifecycle test under IRQ lock before
 * releasing bypass/reservation. Logical BLOCK_QUARANTINED always stays1. */
int nand_qualification_wire_finish(struct nand_qualification_wire*,const uint32_t[98],bool);
/* Fixed metadata success predicate shared by adapter and synthetic tests. */
bool nand_qualification_wire_clean_result(const uint32_t[98]);
#endif

