#ifndef OPENPENDANT_NRR_WIRE_H
#define OPENPENDANT_NRR_WIRE_H
/* Fixed public-object read-rate metadata transport. Pure C, no heap, OS, driver or logging.
 * All callbacks are synchronous and nonblocking; no retained caller pointers.
 * The caller installs terminal bypass and exclusive producer reservation first.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define NRR_WIRE_HEADER 48U
#define NRR_WIRE_WORDS 72U
#define NRR_WIRE_BODY 288U
#define NRR_WIRE_EVENTS 13U
#define NRR_WIRE_IO_MAX 64U
#define NRR_WIRE_ATTEMPTS 8192U
#define NRR_WIRE_FRAME_MS 2000U
#define NRR_WIRE_DATA_MS 20000U
enum nrr_wire_rc {
 NRR_WIRE_OK=0, NRR_WIRE_ARGUMENT=-1,
 NRR_WIRE_IO=-2, NRR_WIRE_TIMEOUT=-3,
 NRR_WIRE_PROTOCOL=-4, NRR_WIRE_HOST_ABORT=-5,
 NRR_WIRE_STATE=-6, NRR_WIRE_GLOBAL_TIMEOUT=-7
};
struct nrr_wire_io {
 int (*write)(void*,const uint8_t*,size_t,size_t*);
 int (*read)(void*,uint8_t*,size_t,size_t*);
 uint32_t (*now_ms)(void*);
 void (*yield_us)(void*,uint32_t);
 int (*pending_rx)(void*,bool*);
 void *user;
};
struct nrr_wire {
 struct nrr_wire_io io;
 uint8_t nonce[16],tx_header[48],tx_metadata[288],rx_control[56];
 uint32_t data_deadline_ms;
 /* Last fully sent verified-run metadata; no array data. Final success must
  * preserve it and exact aggregate counts, not substitute another run. */
 uint32_t last_run[56],last_starts,last_elapsed_ms,cycle_hz;
 uint16_t events_sent,events_acked;
 int last_rc;
 uint8_t host_abort_reason;
 bool initialized,handshake_started,hello_received,deadline_armed;
 bool output_frame_boundary,transport_usable,data_stopped,end_attempted,end_sent;
 bool clean_end_acknowledged,console_reuse_allowed,quarantined;
};
/* accepted/received counts must always be set, <=requested, even on error.
 * An accepted write is a copied PREFIX, not a durable host acknowledgement. */
uint32_t nrr_wire_crc32(const uint8_t*,size_t);
int nrr_wire_init(struct nrr_wire*,const struct nrr_wire_io*);
int nrr_wire_handshake(struct nrr_wire*);
/* Once before first NAND START, never renewal. Absolute <=now+20000ms.
 * Driver owns full int64 deadline; modulo32 comparison covers bounded horizon. */
int nrr_wire_arm_data_deadline(struct nrr_wire*,uint32_t);
/* Nonblocking check before every data START. Pending input outside an ACK
 * window is rejected without consumption/interpretation. Never gates restore. */
int nrr_wire_check(struct nrr_wire*);
/* absolute_event_deadline is the driver's already-clipped original deadline;
 * it must be in (now,min(now+2000,data_deadline)] and is never renewed. */
int nrr_wire_progress(struct nrr_wire*,const uint32_t[72],uint32_t absolute_event_deadline);
/* Failed END no ACK and no console return. Clean END requires exact final ACK
 * and no queued RX. Caller repeats pending/lifecycle test under IRQ lock before
 * releasing bypass/reservation. Serialized quarantine flags always stay1. */
int nrr_wire_finish(struct nrr_wire*,const uint32_t[72],bool);
/* Fixed metadata success predicate shared by adapter and synthetic tests. */
bool nrr_wire_clean_result(const uint32_t[72]);
#endif
