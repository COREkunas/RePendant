/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OPENPENDANT_RECORDING_STORE_BRIDGE_H
#define OPENPENDANT_RECORDING_STORE_BRIDGE_H
#include "recording_object_store.h"

/* UNLINKED scheduling adapter, no threads, capture, hardware or write authority.
 * Exclusively owns an already mounted ROS context. No other actor may call that
 * context until bridge ownership is explicitly retired after joined workers.
 * select precedes rw_start and supplies mode/first counter missing from rp_sink.
 * These deadline-taking functions match rw_async_hooks storage callbacks; a
 * platform hook wrapper forwards its bridge pointer. The worker supplies the
 * ORIGINAL confirmed absolute deadline, including all stop/drain polls.
 *
 * reserve/finalize perform exactly three logical I/O steps synchronously, ONLY
 * before capture / after stop-and-join respectively. submit seals once into ROS
 * owned ciphertext but performs no map I/O. poll performs metadata only; advance
 * on an independent storage worker performs at most one ROS logical I/O step.
 * A backend step can include Dhara GC, not just one physical NAND transaction.
 * On READY wake the codec worker and park storage until the next submission.
 * Repeated timely advance is metadata-only READY; the original deadline still
 * applies until poll takes the receipt, so READY never renews ownership.
 * All gates are try-only. Concurrent advance makes poll PENDING, never durable.
 *
 * cancel is a nonblocking sticky latch. It inhibits subsequent advance calls;
 * an already admitted synchronous callback cannot be preempted. Caller MUST
 * stop/join storage and acquisition, or reset, under an independent supervisor.
 * Fault/cancel/late return retains identity, lease and ROS context/buffers. No
 * retry, automatic cleanup, reinit, callback pointer retention or wipe is done.
 * A late verified commit remains recoverable but no late receipt is returned.
 * No target stack/throughput/admission guarantee follows from offline tests.
 */
#define RSB_MAX_MS 8000U
enum rsb_rc { RSB_OK=0,RSB_PENDING=1,RSB_READY=2,RSB_ARGUMENT=-1,RSB_BUSY=-2,RSB_STATE=-3,RSB_FAILED=-4 };
enum rsb_state { RSB_IDLE=0,RSB_SELECTED=1,RSB_ACTIVE=2,RSB_STORING=3,RSB_COMPLETE=4,RSB_FAULT=5 };
struct rsb_status { uint32_t state,cancelled,pending; int32_t error; uint64_t job_deadline; };
struct rsb_context {
 atomic_uint gate,cancelled;
 uint32_t initialized,state,mode,pending_valid,deferred_seal;
 int32_t error;
 struct ros_context *store;
 void *clock_user;
 uint64_t (*now_ms)(void*);
 uint64_t first_sample,last_now,job_deadline;
 struct es_binding session,pending;
};
int rsb_init(struct rsb_context*,struct ros_context*,void*,uint64_t (*now_ms)(void*));
/* Select only with a ROS seal provider qualified for exact in-place encryption.
 * Submit copies; the independent storage actor performs crypto as its first
 * advance. Original absolute deadline and pending-owner lifetime are unchanged. */
int rsb_init_deferred_seal(struct rsb_context*,struct ros_context*,void*,uint64_t (*now_ms)(void*));
int rsb_select(struct rsb_context*,const struct es_binding*,enum rp_mode,uint64_t first_sample);
int rsb_cancel(struct rsb_context*); /* sticky, no cleanup/backend call */
int rsb_advance(struct rsb_context*); /* independent storage worker, one step */
int rsb_get_status(struct rsb_context*,struct rsb_status*);
/* CANDIDATE metadata-only pre-capture query under the same exclusive store
 * ownership as reserve. Zero means no unused root or no unconsumed slot. This
 * does not reclaim tombstones or infer free capacity from NAND bytes. */
int rsb_capacity(void*,const struct es_binding*,uint32_t *segments,uint64_t deadline);
int rsb_reserve(void*,const struct es_binding*,uint64_t deadline);
int rsb_submit(void*,const struct es_binding*,const uint8_t*,size_t,struct rp_segment_receipt*,uint64_t deadline);
int rsb_poll(void*,const struct es_binding*,struct rp_segment_receipt*,uint64_t deadline);
int rsb_finalize(void*,const struct rp_completion*,struct rp_final_receipt*,uint64_t deadline);
#endif
