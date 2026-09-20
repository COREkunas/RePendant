#ifndef OPENPENDANT_RECORDING_FRAME_QUEUE_H
#define OPENPENDANT_RECORDING_FRAME_QUEUE_H
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

/* OFFLINE / UNLINKED. Eight copied mono PCM320 frames = 160 ms at 16 kHz.
 * Not a microphone/DMA driver, scheduler, deadline, encoder or persistence API.
 * One producer and one consumer, each nonreentrant; all other entry lifetimes
 * are externally coordinated. No heap, spin loop, interrupt masking or reset.
 *
 * Declare fresh storage with RFQ_INITIALIZER, then call rfq_init exactly once
 * before publishing its address to any actor. Never memcpy/memset a live queue,
 * guess initialization from uninitialized memory, or implicitly reuse an instance.
 * Objects and all caller spans must be valid for their complete call lifetime.
 * ANY negative push/pop result requires integration to stop acquisition and
 * resolve the fault; never silently drop/retry that input. Invalid argument or
 * span admission cannot safely mutate a possibly invalid queue to latch stop.
 * The producer owns immutable input; successful push copies it. Successful pop
 * transfers a copy to the consumer, which must wipe it after encoding/use.
 *
 * Release/acquire head/tail handoff makes slot ownership valid on coherent SMP.
 * Producer publication follows its complete copy; consumer returns space only
 * after complete copy-out AND volatile slot wipe. UInt32 tickets deliberately
 * wrap modulo 2^32, with <=8 outstanding entries; no signed ordering is used.
 * Init requires lock-free 32-bit unsigned atomics for EVERY actual queue object
 * (including platforms whose capability macro conservatively says sometimes).
 * Unsupported atomics fail initialization before mutation. This does NOT prove
 * target ISR latency, native CAS instruction boundedness, DMA/cache coherency,
 * stack usage, real-time service or that interrupts are disabled. Target code
 * generation and ISR/DMA cache policy require a separate integration review.
 * Cortex-M33 strong CAS uses inline LL/SC and can retry after reservation loss;
 * a single C atomic operation is not a wait-free or hard ISR deadline claim.
 *
 * Stop reason is first successful CAS wins (USER/full/continuity/overflow/etc).
 * A push/pop admitted before a concurrent stop may finish; accepted frames are
 * still an exact contiguous prefix. A failed/reentrant call never wipes another
 * actor's slot. The integration must immediately stop acquisition on any stop.
 * After stopping DMA/IRQ callbacks and joining any in-flight producer, call
 * rfq_confirm_producer_stopped. Its nonblocking admission establishes the queue
 * fence, NOT hardware-stop proof. BUSY is not a fence; no automatic retry here.
 * Ordinary pop is allowed while running; after stop, new pops require the fence.
 * Drain under an external finite deadline, then finish; or explicitly discard.
 * No rejected frame is retried automatically and no resume/reset exists.
 */
#if ATOMIC_INT_LOCK_FREE == 0 || UINT_MAX != UINT32_MAX
#error "recording_frame_queue requires lock-free 32-bit unsigned atomics"
#endif
#define RFQ_FRAME_SAMPLES 320U
#define RFQ_CAPACITY 8U
enum rfq_state { RFQ_FRESH=0, RFQ_INITIALIZING=1, RFQ_RUNNING=2, RFQ_FENCED=3, RFQ_CLOSED=4 };
enum rfq_reason { RFQ_NONE=0, RFQ_USER=1, RFQ_FULL=2, RFQ_CONTINUITY=3,
 RFQ_SAMPLE_OVERFLOW=4, RFQ_REENTRY=5, RFQ_INVARIANT=6 };
enum rfq_rc { RFQ_OK=0, RFQ_EMPTY=1, RFQ_DRAINED=2, RFQ_ARGUMENT=-1,
 RFQ_STATE=-2, RFQ_BUSY=-3, RFQ_STOPPED=-4, RFQ_NEEDS_FENCE=-5, RFQ_NOT_EMPTY=-6, RFQ_UNSUPPORTED=-7 };
struct rfq_frame { uint64_t first_sample; int16_t pcm[RFQ_FRAME_SAMPLES]; };
struct rfq_summary {
 uint32_t state,reason,queued;
 uint64_t first_sample,next_sample,accepted,consumed,discarded;
};
/* Private implementation storage: caller must not read or mutate fields. */
struct rfq_queue {
 atomic_uint state,reason,head,tail,producer_busy,consumer_busy;
 uint64_t first_sample,next_sample,accepted,consumed,discarded;
 struct rfq_frame slots[RFQ_CAPACITY];
};
_Static_assert(sizeof(struct rfq_queue)==5248,"fixed queue instance storage");
#define RFQ_INITIALIZER { RFQ_FRESH,RFQ_NONE,0,0,0,0,0,0,0,0,0,{{0,{0}}} }

int rfq_init(struct rfq_queue *,uint64_t first_sample);
/* Explicit lifecycle-only reuse after a proven complete producer/consumer join.
 * CLOSED, empty and already wiped; takes both actor guards once, never waits.
 * Caller must also fence stale external callbacks with a non-reused epoch. This
 * does not reset hardware, authorize recording, clear an integration fault, or
 * weaken init's exactly-once rule. Failure leaves the closed queue unchanged. */
int rfq_rearm_closed(struct rfq_queue *,uint64_t first_sample);
int rfq_push(struct rfq_queue *,uint64_t first_sample,const int16_t pcm[RFQ_FRAME_SAMPLES]);
int rfq_pop(struct rfq_queue *,struct rfq_frame *out); /* no output writes unless OK */
int rfq_request_stop(struct rfq_queue *); /* USER, does not erase an earlier fault */
int rfq_stop_reason(const struct rfq_queue *); /* reason >=0 or negative ARG/STATE */
int rfq_confirm_producer_stopped(struct rfq_queue *); /* external producer stopped first */
int rfq_finish(struct rfq_queue *); /* fenced + empty only; wipe, close */
int rfq_discard(struct rfq_queue *); /* explicitly discard fenced accepted remainder, close */
int rfq_get_summary(struct rfq_queue *,struct rfq_summary *); /* fenced/closed only */
#endif
