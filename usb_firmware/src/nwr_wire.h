#ifndef OPENPENDANT_NWR_WIRE_H
#define OPENPENDANT_NWR_WIRE_H
/* Fixed metadata-only qualifier. Pure C, no heap, OS, driver or logging.
 * All callbacks are synchronous and nonblocking; no retained caller pointers.
 * The caller installs terminal bypass and exclusive producer reservation first.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define NWR_WIRE_HEADER 48U
#define NWR_WIRE_WORDS 96U
#define NWR_WIRE_BODY 384U
#define NWR_WIRE_EVENTS 32U
#define NWR_WIRE_IO_MAX 64U
#define NWR_WIRE_ATTEMPTS 8192U
#define NWR_WIRE_FRAME_MS 2000U
#define NWR_WIRE_DATA_MS 120000U
enum nwr_wire_rc {
 NWR_WIRE_OK=0, NWR_WIRE_ARGUMENT=-1,
 NWR_WIRE_IO=-2, NWR_WIRE_TIMEOUT=-3,
 NWR_WIRE_PROTOCOL=-4, NWR_WIRE_HOST_ABORT=-5,
 NWR_WIRE_STATE=-6, NWR_WIRE_GLOBAL_TIMEOUT=-7
};
struct nwr_wire_io {
 int (*write)(void*,const uint8_t*,size_t,size_t*);
 int (*read)(void*,uint8_t*,size_t,size_t*);
 uint32_t (*now_ms)(void*);
 void (*yield_us)(void*,uint32_t);
 int (*pending_rx)(void*,bool*);
 void *user;
};
struct nwr_wire {
 struct nwr_wire_io io;
 uint8_t nonce[16],tx_header[48],tx_metadata[384],rx_control[56];
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
uint32_t nwr_wire_crc32(const uint8_t*,size_t);
int nwr_wire_init(struct nwr_wire*,const struct nwr_wire_io*);
int nwr_wire_handshake(struct nwr_wire*);
/* Once before first NAND START, never renewal. Absolute <=now+120000ms.
 * Driver owns full int64 deadline; modulo32 comparison covers bounded horizon. */
int nwr_wire_arm_data_deadline(struct nwr_wire*,uint32_t);
/* Nonblocking check before every data START. Pending input outside an ACK
 * window is rejected without consumption/interpretation. Never gates restore. */
int nwr_wire_check(struct nwr_wire*);
int nwr_wire_progress(struct nwr_wire*,const uint32_t[96]);
/* Failed END no ACK and no console return. Clean END requires exact final ACK
 * and no queued RX. Caller repeats pending/lifecycle test under IRQ lock before
 * releasing bypass/reservation. Logical BLOCK_QUARANTINED always stays1. */
int nwr_wire_finish(struct nwr_wire*,const uint32_t[96],bool);
/* Fixed metadata success predicate shared by adapter and synthetic tests. */
bool nwr_wire_clean_result(const uint32_t[96]);
#endif
