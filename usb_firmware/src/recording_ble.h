/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OPENPENDANT_RECORDING_BLE_H
#define OPENPENDANT_RECORDING_BLE_H
#include "durable_ble_codec.h"
struct bt_conn;

/* Target broker, initially unlinked. Init performs no Bluetooth/storage work.
 * All hooks are bounded/nonblocking, retain no spans, and execute outside the
 * short IRQ-locked broker state. No storage/microphone/key API is called here.
 * The runtime exclusively serializes the copied requests with recording and
 * other volume operations and supplies an independent deadline supervisor.
 * Single-core !SMP is required; k_uptime_get/irq_lock protect 64-bit clocks.
 * Terminal delivery runs on k_sys_work_q: the pinned Zephyr ATT notification
 * allocator uses K_NO_WAIT there, but K_FOREVER from an ordinary thread.
 */
#define RECORDING_BLE_SESSION_MS 900000U
#define RECORDING_BLE_REQUEST_MS 30000U
#define RECORDING_BLE_FULL_OPEN_MS 300000U
#define RECORDING_BLE_MIN_MTU 96U
enum recording_ble_result { RB_OK=0,RB_ARGUMENT=-1,RB_BUSY=-2,RB_REFUSED=-3,RB_FAULT=-4 };
struct recording_ble_hooks {
 void *user;
 int (*ready)(void *); /* 1 iff enrolled volume/runtime explicitly ready. */
 /* 1 iff this exact bonded/encrypted conn is currently authorized, subscribed
  * to the response characteristic and has negotiated actual ATT_MTU>=96. */
 int (*authorized)(void *,struct bt_conn *);
 /* Queue ONE immutable copied request;0 proves admission,nonzero proves NONE.
  * It may finish on another thread before submit returns. Deadline is original
  * min(first-nonce session expiry, now+30s), not renewable across fragments. */
 int (*submit)(void *,uint32_t epoch,const struct db_request *,uint64_t deadline);
 /* Recheck exact conn authorization/subscription/MTU at enqueue. Must copy the
  * complete OP frame synchronously, return actual enqueue status, no logging.
  * 0 is NOT phone delivery or a new storage receipt. A revoked connection must
  * not be sent data even if revocation races the broker's last check. */
 int (*notify)(void *,struct bt_conn *,const uint8_t *,size_t);
 /* Queue retirement ONLY;0 means accepted, not finished. Parent has joined the
  * old job, retains volume ownership, cancels/retires its exact catalog, then
  * calls recording_ble_retired. No new session before that acknowledgement. */
 int (*retire)(void *,uint32_t epoch);
};
int recording_ble_init(const struct recording_ble_hooks *);
int recording_ble_command(struct bt_conn *,const uint8_t complete_op_request[],size_t);
void recording_ble_disconnected(struct bt_conn *);
/* Worker proof query. Disconnect/session expiry does not abort an admitted
 * storage job: it remains permitted until its ORIGINAL request deadline.
 * No query grants or renews storage/power/volume authority. */
int recording_ble_admitted(uint32_t epoch,const struct db_request *,uint64_t deadline);
/* Call exactly once only AFTER worker/backend joined and no request work can
 * resume. Success frame must echo exact request and frozen codec shape.
 * Completion after disconnect/expiry resolves local ownership but never sends.
 * A response is copied; no caller pointer is retained. Return only means staged
 * terminal ownership, NOT successful notify/phone delivery/retirement. The
 * system-workqueue pump retains pending/ref ownership until actual enqueue
 * returns. Missed/delayed work remains fenced; no automatic retry or timeout
 * clear. Parent idle poll revokes expiry; eventual late pump sends nothing. */
int recording_ble_complete(uint32_t epoch,uint16_t sequence,const uint8_t *,size_t);
int recording_ble_failed(uint32_t epoch,uint16_t sequence);
/* Intermediate stream chunks only: backend call has joined, but the parent
 * worker still owns the request. One copied225B staging slot; actual ATT queue
 * credits bound further enqueues. Final chunk MUST use complete after join.
 * ready:1=slot clear/live,0=staged,-1=revoked (after staged work drains).
 * The worker waits on a progress hint with bounded1ms fallback, retaining the
 * original deadline. Every wake is followed by the same readiness checks. */
int recording_ble_stream_frame(uint32_t epoch,uint16_t sequence,const uint8_t *,size_t);
int recording_ble_stream_ready(uint32_t epoch,uint16_t sequence);
/* Hint-only wake from local slot release or GATT notification completion.
 * No connection/epoch/data pointer is retained. Duplicate/stale hints are safe:
 * they grant no authority, acknowledge nothing, and never enqueue a packet.
 * wait is worker-only, outside IRQ locks. Missing callbacks retain the old
 * bounded1ms polling fallback; no deadline is created or renewed here. */
void recording_ble_tx_progress(void);
void recording_ble_stream_wait(void);
int recording_ble_retired(uint32_t epoch,int result);
/* Notify hook's last broker check immediately before actual GATT enqueue;
 * still additionally recheck platform authorization/MTU/subscription there. */
int recording_ble_reply_allowed(struct bt_conn *,uint16_t sequence);
/* Optional bounded idle tick from parent worker: expires an idle session even
 * when no new BLE request arrives. No timer/thread or automatic reconnect. */
void recording_ble_poll(void);
int recording_ble_available(void); /* capability: runtime ready, not faulted */
#endif
