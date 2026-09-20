#ifndef OPENPENDANT_RECORDING_CONTROL_H
#define OPENPENDANT_RECORDING_CONTROL_H
#include <stdatomic.h>
#include "long_recording_control_codec.h"
#include "button_gesture.h"
/* Additive coordinator. No pointers to conn,
 * PCM or caller frames are retained. Platform owns a non-recycled per-boot
 * connection epoch, exact bonded/encrypted/CCC/MTU authorization and a bounded
 * system-workqueue notification pump. Recheck reply_allowed immediately before
 * enqueue; encode success is NOT delivery. Root runtime owns actual resources.
 * Only explicit START or an enabled debounced physical tap queues work. STOP latches independently of the
 * busy recording worker. A first STOP can return PENDING (no success body), until
 * the worker publishes its actual first reason: never invent USER over FULL.
 * Status/duplicate START never mounts/reserves/captures. 32 consumed operation
 * remote identities are retained for this entire boot, including failed admission.
 * Physical recordings use a separate monotonic, non-recycled ticket namespace.
 */
#define LRC_OPERATIONS 32U
#define LRC_PREPARE_MS 120000U
/* DEFERRED is returned ONLY by command(), before claiming the metadata gate:
 * neither sequence nor operation was admitted. BUSY after admission must use
 * pending_reply(), never re-execute the command. Both keep the original budget. */
enum lrc_rc { LRC_OK=0,LRC_PENDING=1,LRC_DEFERRED=2,LRC_PAIRING_REQUEST=3,LRC_ARGUMENT=-1,LRC_BUSY=-2,LRC_REFUSED=-3,LRC_FAULT=-4 };
struct lrc_port {
 void *user;
 uint64_t (*now_ms)(void*);
 int (*authorized)(void*,uint64_t connection_epoch); /* exact secure peer/CCC/MTU, 1 only */
 int (*start_ready)(void*); /* cached admission only; runtime queue rechecks CAS */
 int (*queue_start)(void*,uint32_t ticket,uint64_t original_deadline); /* copy,0 admitted */
 void (*wake)(void*); /* nonblocking worker wake, no storage/capture work */
 int (*usb_present)(void*); /* primitive cached0/1, never a rail measurement */
};
struct recording_control {
 atomic_uint gate,stop[LRC_OPERATIONS],issued,fault,submitting;
 struct lrc_port port;
 struct lc_state states[LRC_OPERATIONS],idle;
 uint8_t binding[32];
 uint64_t last_now,connection,last_connection;
 uint16_t sequence;
 uint32_t initialized,used,current;
 uint32_t armed,button_ticket;
 uint64_t armed_deadline;
 struct button_gesture button;
 /* A single local slot is reusable only with a fresh monotonically tagged
  * ticket. Remote consumed IDs remain retained and cannot alias this slot. */
 struct lc_state local;
 atomic_uint local_ticket,local_stop;
 uint32_t local_count,standalone,button_local_ready;
};
int lrc_init(struct recording_control*,const struct lrc_port*,const uint8_t boot[16],const uint8_t binding[32]);
int lrc_command(struct recording_control*,uint64_t,const uint8_t*,size_t,uint8_t reply[LC_RESPONSE_BYTES]);
int lrc_reply_allowed(struct recording_control*,uint64_t,uint16_t);
/* Re-encode the SAME admitted sequence after a PENDING/BUSY result. Never
 * executes START/STOP, requeues work, or changes an operation. */
int lrc_pending_reply(struct recording_control*,uint64_t,const uint8_t*,size_t,uint8_t reply[LC_RESPONSE_BYTES]);
int lrc_disconnected(struct recording_control*,uint64_t);
/* Worker only; publish exact joined/accepted/durable counters, not UI estimates.
 * Invalid or regressing publication fences controller; never erases old proof. */
int lrc_publish(struct recording_control*,uint32_t ticket,const struct lc_state*);
int lrc_snapshot(struct recording_control*,uint32_t ticket,struct lc_state*);
/* Nonblocking maintenance claim. Caller holds runtime admission first;
 * successful return keeps metadata gate until release. No I/O in coordinator. */
int lrc_maintenance_claim(struct recording_control*);
void lrc_maintenance_release(struct recording_control*);
/* Lock-free read safe while rw_start owns worker gate. 1 means normal USER stop;
 * -1 invalid/fenced;0 no stop. Never clears/renews anything. */
int lrc_stop_requested(struct recording_control*,uint32_t ticket);
/* Trusted physical input, sampled25ms on the platform main actor. No I/O or
 * microphone work here: a debounced50..1000ms tap queues an enabled standalone
 * or pre-armed operation, or stops the exact operation present at press, after
 *600ms quiet. Five taps return LRC_PAIRING_REQUEST only if still idle; caller
 * must separately claim power/maintenance admission. Never opens pairing here. */
int lrc_button_sample(struct recording_control*,int pressed);
int lrc_set_standalone(struct recording_control*,int enabled);
#define LRC_LOCAL_TICKET 0x80000000U
#endif
