#ifndef OPENPENDANT_NPO_WIRE_H
#define OPENPENDANT_NPO_WIRE_H
/* Fixed metadata-only qualifier. Pure C, no heap, OS, driver or logging.
 * All callbacks are synchronous and nonblocking; no retained caller pointers.
 * The caller installs terminal bypass and exclusive producer reservation first.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define NPO_WIRE_HEADER 48U
#define NPO_WIRE_WORDS 40U
#define NPO_WIRE_BODY 160U
#define NPO_WIRE_EVENTS 32U
#define NPO_WIRE_IO_MAX 64U
#define NPO_WIRE_ATTEMPTS 8192U
#define NPO_WIRE_FRAME_MS 2000U
#define NPO_WIRE_DATA_MS 60000U
enum npo_wire_rc {
 NPO_WIRE_OK=0, NPO_WIRE_ARGUMENT=-1,
 NPO_WIRE_IO=-2, NPO_WIRE_TIMEOUT=-3,
 NPO_WIRE_PROTOCOL=-4, NPO_WIRE_HOST_ABORT=-5,
 NPO_WIRE_STATE=-6, NPO_WIRE_GLOBAL_TIMEOUT=-7
};
struct npo_wire_io {
 int (*write)(void*,const uint8_t*,size_t,size_t*);
 int (*read)(void*,uint8_t*,size_t,size_t*);
 uint32_t (*now_ms)(void*);
 void (*yield_us)(void*,uint32_t);
 int (*pending_rx)(void*,bool*);
 void *user;
};
struct npo_wire {
 struct npo_wire_io io;
 uint8_t nonce[16],tx_header[48],tx_metadata[160],rx_control[56];
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
uint32_t npo_wire_crc32(const uint8_t*,size_t);
int npo_wire_init(struct npo_wire*,const struct npo_wire_io*);
int npo_wire_handshake(struct npo_wire*);
/* Once before first NAND START, never renewal. Absolute <=now+60000ms.
 * Driver owns full int64 deadline; modulo32 comparison covers bounded horizon. */
int npo_wire_arm_data_deadline(struct npo_wire*,uint32_t);
/* Nonblocking check before every data START. Pending input outside an ACK
 * window is rejected without consumption/interpretation. Never gates restore. */
int npo_wire_check(struct npo_wire*);
int npo_wire_progress(struct npo_wire*,const uint32_t[40]);
/* Failed END no ACK and no console return. Clean END requires exact final ACK
 * and no queued RX. Caller repeats pending/lifecycle test under IRQ lock before
 * releasing bypass/reservation. Logical BLOCK_QUARANTINED always stays1. */
int npo_wire_finish(struct npo_wire*,const uint32_t[40],bool);
/* Fixed metadata success predicate shared by adapter and synthetic tests. */
bool npo_wire_clean_result(const uint32_t[40]);
#endif
