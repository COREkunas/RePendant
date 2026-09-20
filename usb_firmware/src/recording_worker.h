#ifndef OPENPENDANT_RECORDING_WORKER_H
#define OPENPENDANT_RECORDING_WORKER_H
#include "recording_frame_queue.h"
#include "recording_pipeline.h"

/* UNLINKED integration core, not a capture driver or persistence implementation.
 * Worker owns pipeline/codec/store; producer ONLY copies a PCM320 frame. No heap,
 * payload log, startup, IRQ, hardware, clock setting or automatic restart here.
 * All actors must use this API exclusively: do not call rp_* concurrently.
 * A qualified >=64KiB guarded worker is mandatory; combined target stack/RAM and
 * complete codec+crypto+storage throughput have NOT been measured by host tests.
 * Eight queued frames represent160ms, NOT a sufficient-storage-stall claim.
 *
 * Bind a higher-priority independent supervisor before integration. arm(deadline)
 * must nonblockingly arrange capture inhibition + cold reset if this worker
 * never returns by that exact monotonic deadline, without touching/wiping its
 * live buffers. Do not extend a deadline on progress. A kernel timer is not a
 * hard IRQ-hang bound; hardware-watchdog/power policy needs its own review.
 * Clock is monotonic uint64 milliseconds, never wraps for this lifecycle.
 * Every synchronous callback and Opus call is checked before/after; a late return
 * is still NOT preemption. On timeout/clock failure the supervisor stays armed,
 * ownership is retained and no new storage call is made. stop_and_join may be
 * attempted once with the original (possibly expired) deadline solely to fence
 * DMA. Only proven-stopped data is wiped; no live callback pointer is retained.
 * If no supervisor was confirmed armed (clock failure before arm, failed arm,
 * or failed/late disarm), do NOT attempt a blocking safety stop: retain ownership
 * and return FAULT. Integration MUST immediately inhibit acquisition/reset via
 * its independently reviewed nonblocking fatal path, never call this worker
 * again or release/reuse those buffers. supervisor_armed means an arm attempt
 * remains unretired; supervisor_confirmed separately proves its accepted guard.
 *
 * acquire=0 atomically reserves capture/codec/crypto/store and confirms all six
 * flags. No microphone starts until explicit rw_start; mode/init never starts it.
 * A nonzero acquire MUST prove that no lease was taken; ambiguous acquisition
 * is not this callback contract. release=0 proves every reserved service released.
 * capture_start(epoch,first) may call rw_submit synchronously. stop_and_join=0
 * is a TRUSTED proof that DMA/IRQs are stopped and every producer callback joined.
 * Failure retains ownership without queue wipe/retry. Callbacks never retain
 * supplied identity/PCM/plaintext/receipt pointers after returning.
 *
 * Producer must use its captured nonzero epoch and hardware first_sample counter.
 * On any non-stale negative submit it must request hardware capture inhibition
 * using the driver-specific ISR-safe mechanism and wake the worker. Accepted
 * submit and stop requests also wake the worker externally. There is no scheduling
 * callback here. An already admitted frame may finish when stop is requested.
 *
 * Clean, finalized sessions may explicitly start again after release. Epochs
 * never wrap; RW_EPOCH_MAX exhausts this boot. The high bit of the internal
 * atomic word is a tagged producer fault, never part of a public token.
 * Old-epoch callbacks are rejected
 * before queue access. Overflow/external/clock/timeout/core faults prohibit reuse.
 */
#define RW_CALL_BUDGET_MAX_MS 8000U
#define RW_CAPACITY_MAX_SLOTS 5120U
#define RW_DRAIN_BUDGET_JOBS 3U /* existing segment, tail, finalization; fixed once */
#define RW_EPOCH_MAX UINT32_C(0x7fffffff)
enum rw_state { RW_UNINITIALIZED=0,RW_IDLE=1,RW_STARTING=2,RW_RUNNING=3,
 RW_STOPPING=4,RW_STOPPED=5,RW_FAULT=6,RW_DRAINING=7 };
enum rw_reason { RW_NONE=0,RW_USER=1,RW_LOW_POWER=2,RW_OVERFLOW=3,
 RW_CONTINUITY=4,RW_EXTERNAL=5,RW_TIMEOUT=6,RW_CLOCK=7,RW_CORE=8,
 RW_STOP_FAILED=9,RW_ADMISSION=10,RW_FULL=11,RW_RELEASE_FAILED=12 };
enum rw_rc { RW_OK=0,RW_WAIT=1,RW_COMPLETE=2,RW_ARGUMENT=-1,RW_BUSY=-2,
 RW_STATE=-3,RW_STALE=-4,RW_STOPPED_INPUT=-5,RW_ERROR=-6,RW_NO_CAPACITY=-7,RW_CANCELLED=-8 };
struct rw_readiness {
 uint32_t owner_verified,recovery_verified,store_ready,capture_ready,power_safe,profile_qualified;
};
struct rw_hooks {
 void *user;
 uint64_t (*now_ms)(void*);
 int (*supervisor_arm)(void*,uint64_t absolute_deadline_ms);
 int (*supervisor_disarm)(void*); /* nonblocking;0 proves no pending expiry */
 int (*acquire)(void*,struct rw_readiness*,uint64_t deadline);
 int (*release)(void*,uint64_t deadline);
 int (*capture_start)(void*,uint32_t epoch,uint64_t first_sample,uint64_t deadline);
 int (*stop_and_join)(void*,uint32_t epoch,uint64_t deadline);
 int (*reserve)(void*,const struct es_binding*,uint64_t deadline);
 int (*seal_commit)(void*,const struct es_binding*,const uint8_t*,size_t,
                    struct rp_segment_receipt*,uint64_t deadline);
 int (*finalize)(void*,const struct rp_completion*,struct rp_final_receipt*,uint64_t deadline);
};
/* Optional deferred storage. submit.seal_commit makes one bounded immutable
 * submission; poll performs NO NAND/crypto/other blocking I/O. A separate
 * storage worker owns persistence and makes progress independently. Concurrent
 * storage BUSY maps to RP_IO_PENDING with a zero receipt, never OK.
 *
 * Stop closes/joins acquisition and drains accepted frames once. If rp_end is
 * pending, RW_DRAINING never renews an in-flight storage job. After acquisition
 * joins, a once-only total envelope of 3*call_budget_ms bounds draining. Only
 * a NEW tail/finalization job may disarm/rearm the worker supervisor with its
 * own call budget, clipped to that envelope; pending storage keeps its original
 * deadline. Merely polling never rearms. Each step queries at most one pending
 * receipt. Exact finalization + no pending ownership are
 * required before release. Failure with pending storage retains lease/deadline
 * and prohibits further store calls; caller's supervisor must independently
 * stop/join storage or reset, never wipe storage-owned buffers while active.
 * No asynchronous storage thread, queue resizing or new target hook is created.
 */
struct rw_async_hooks {
 struct rw_hooks submit;
 int (*poll)(void*,const struct es_binding*,struct rp_segment_receipt*,uint64_t deadline);
};
struct rw_status {
 uint32_t state,epoch,reason,stop_reason,fault,lease_held,producer_fenced,queue_closed;
 uint32_t supervisor_armed,supervisor_confirmed,queue_summary_valid,mode;
 int32_t error,callback_error;
 uint64_t worker_discarded_frames;
 struct rfq_summary queue;
 struct rp_status pipeline;
 uint32_t capacity_known,admitted_frames; /* candidate metadata, immutable per start */
};
int rw_init(const struct rw_hooks*,enum rp_mode,uint32_t stack_bytes,uint32_t call_budget_ms);
int rw_init_async(const struct rw_async_hooks*,enum rp_mode,uint32_t stack_bytes,uint32_t call_budget_ms);
/* Called once after exclusive acquire, before reserve or capture. A successful
 * query returns 0..RW_CAPACITY_MAX_SLOTS reservable whole-segment slots under
 * that same exclusive store lease. It is not physical write authority. No other
 * actor may consume those slots until release. Zero refuses without creating a
 * session: RW_NO_CAPACITY, IDLE/FULL, no final-manifest claim. Invalid/late query
 * is a fault. Default rw_init[_async] retains its previous behavior.
 * Producer admission consumes one of slots*500 frame credits per accepted push;
 * last accepted frame latches FULL. An in-flight physical frame may be rejected,
 * but no accepted frame is discarded on an otherwise clean capacity stop. */
typedef int (*rw_capacity_fn)(void*,const struct es_binding*,uint32_t *slots,uint64_t deadline);
int rw_init_capacity_async(const struct rw_async_hooks*,rw_capacity_fn,enum rp_mode,
                           uint32_t stack_bytes,uint32_t call_budget_ms);
int rw_select_mode(enum rp_mode); /* clean idle/completed only */
int rw_start(const struct es_binding*,uint64_t first_sample); /* explicit consent external */
/* Isolated long-control seam. Independent stop latch survives rw_start's own
 * state reset. Check is nonblocking:0 allow,1 normal USER cancellation,<0 fault.
 * Checks before capacity/reserve and immediately before capture. epoch0 means
 * no session exists; after reserve, cancellation finalizes a REAL empty session.
 * Callback may not enter worker or retain spans. A positive final check claims
 * no mic attempt; a STOP after final claim is in-flight start and must drain.
 * Only usable with reviewed capacity initialization, never generic rw_init. */
typedef int (*rw_start_check_fn)(void*,uint32_t epoch,int session_created,uint64_t deadline);
int rw_start_controlled(const struct es_binding*,uint64_t first_sample,rw_start_check_fn,void*);
int rw_submit(uint32_t epoch,uint64_t first_sample,const int16_t pcm[320]);
int rw_request_stop(uint32_t epoch,enum rw_reason); /* USER,LOW_POWER,EXTERNAL only */
int rw_step(void); /* at most one live frame, or <=8 fenced drain frames + final seal */
int rw_get_status(struct rw_status*); /* nonblocking, never a payload export */
#endif
