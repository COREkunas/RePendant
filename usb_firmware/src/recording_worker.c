#include "recording_worker.h"
#include <string.h>
_Static_assert(RW_CAPACITY_MAX_SLOTS<=UINT32_MAX/RP_SEGMENT_FRAMES,"Capacity frame credits fit uint32");

static struct {
 struct rw_hooks hooks;
 struct rw_status report;
 struct rfq_frame frame;
 uint32_t budget;
 uint64_t last_now,deadline,drain_deadline,pending_deadline;
 int clock_seen,queue_initialized,capture_possible,stop_attempted,unsafe;
 uint32_t unsafe_reason;
 int (*poll)(void*,const struct es_binding*,struct rp_segment_receipt*,uint64_t);
 rw_capacity_fn capacity;
 uint32_t remaining_frames; /* protected by producer gate; initialized before RUNNING */
} rw;
static struct rfq_queue rw_queue=RFQ_INITIALIZER;
static atomic_uint rw_entry,rw_producer,rw_initialized,rw_state_atomic,rw_epoch,rw_stop,rw_external;
#define RW_EPOCH_FAULT UINT32_C(0x80000000)
static int rw_fence(void);
static int rw_close_queue(void);
static int rw_begin_drain(void);

static void rw_wipe(void *p,size_t bytes) { volatile uint8_t *b=p;while(bytes--)*b++=0; }
static int rw_take(atomic_uint *p) { unsigned v=0;return atomic_compare_exchange_strong_explicit(p,&v,1,memory_order_acquire,memory_order_relaxed); }
static void rw_drop(atomic_uint *p) { atomic_store_explicit(p,0,memory_order_release); }
static void rw_state_set(uint32_t state) { rw.report.state=state;atomic_store_explicit(&rw_state_atomic,state,memory_order_release); }
static void rw_latch(uint32_t reason) {
 unsigned v=RW_NONE;(void)atomic_compare_exchange_strong_explicit(&rw_stop,&v,reason,memory_order_acq_rel,memory_order_acquire);
 if(reason==RW_EXTERNAL)atomic_store_explicit(&rw_external,1,memory_order_release);
}
static unsigned rw_token(void) { return atomic_load_explicit(&rw_epoch,memory_order_acquire)&RW_EPOCH_MAX; }
static void rw_producer_fault(unsigned epoch) {
 /* One tagged CAS, not check-then-latch: a caller that read an old epoch before
  * rearm cannot fault the new session if its gate admission resumes late. */
 unsigned expected=epoch;
 (void)atomic_compare_exchange_strong_explicit(&rw_epoch,&expected,epoch|RW_EPOCH_FAULT,memory_order_acq_rel,memory_order_acquire);
}
static void rw_collect_fault(void) {
 if(atomic_load_explicit(&rw_epoch,memory_order_acquire)&RW_EPOCH_FAULT)rw_latch(RW_EXTERNAL);
}
static int rw_clock(void) {
 uint64_t now=rw.hooks.now_ms(rw.hooks.user);
 if(rw.clock_seen && now<rw.last_now) { rw.unsafe=1;rw.unsafe_reason=RW_CLOCK;rw_latch(RW_CLOCK);return 0; }
 rw.clock_seen=1;rw.last_now=now;
 if((rw.report.supervisor_armed && now>=rw.deadline) ||
    (rw.pending_deadline && now>=rw.pending_deadline) ||
    (rw.drain_deadline && now>=rw.drain_deadline)) {
  rw.unsafe=1;if(!rw.unsafe_reason)rw.unsafe_reason=RW_TIMEOUT;rw_latch(RW_TIMEOUT);return 0;
 }
 return 1;
}
static int rw_guard(void) { rw_collect_fault();return rw_clock() && !rw.unsafe && !atomic_load_explicit(&rw_external,memory_order_acquire); }
static int rw_arm(void) {
 if(!rw_clock() || rw.last_now>UINT64_MAX-rw.budget) { rw.unsafe=1;if(!rw.unsafe_reason)rw.unsafe_reason=RW_CLOCK;rw_latch(RW_CLOCK);return 0; }
 rw.deadline=rw.last_now+rw.budget;
 if(rw.drain_deadline && rw.deadline>rw.drain_deadline)rw.deadline=rw.drain_deadline;
 rw.report.supervisor_armed=1;
 int rc=rw.hooks.supervisor_arm(rw.hooks.user,rw.deadline);rw.report.callback_error=rc;
 rw.report.supervisor_confirmed=rc==0;
 if(rc || !rw_clock()) { rw.unsafe=1;if(rc) { rw.unsafe_reason=RW_EXTERNAL;rw_latch(RW_EXTERNAL); }return 0; }
 return 1;
}
static int rw_disarm(void) {
 if(!rw_clock() || rw.unsafe)return 0;
 int rc=rw.hooks.supervisor_disarm(rw.hooks.user);rw.report.callback_error=rc;
 rw.report.supervisor_confirmed=0; /* no continued guard can be assumed */
 if(rc || !rw_clock()) { rw.unsafe=1;if(rc)rw.unsafe_reason=RW_EXTERNAL;rw_latch(rc?RW_EXTERNAL:RW_TIMEOUT);return 0; }
 rw.report.supervisor_armed=0;return 1;
}
/* Only after capture has joined: one finite envelope for an existing full
 * segment, a new tail and finalization. It is never moved on progress. */
static int rw_begin_drain(void) {
 if(!rw.poll || rw.drain_deadline)return rw_guard();
 if(!rw_guard() || !rw.report.producer_fenced || rw.capture_possible)return 0;
 uint64_t span=(uint64_t)rw.budget*RW_DRAIN_BUDGET_JOBS;
 if(rw.last_now>UINT64_MAX-span) { rw.unsafe=1;rw.unsafe_reason=RW_CLOCK;rw_latch(RW_CLOCK);return 0; }
 rw.drain_deadline=rw.last_now+span;return 1;
}
/* Renew only for a NEW job after the previous exact receipt. An in-flight
 * storage deadline is immutable and is also independently supervised by the
 * runtime. The rearm is bounded by the once-only drain envelope. */
static int rw_new_job(void) {
 if(!rw_guard() || rw.pending_deadline)return 0;
 if(!rw.drain_deadline)return 1;
 return rw_disarm() && rw_arm();
}
/* Preserve a returned real durable receipt even if it returned late; after-call
 * guard prevents ANY subsequent store callback. The final bridge state is fault,
 * never a successful timeout. Unknown/failed receipts remain core-owned rules. */
static int rw_reserve(void *ignored,const struct es_binding *s) {
 (void)ignored;if(!rw_guard())return RP_IO_FAILED;
 int rc=rw.hooks.reserve(rw.hooks.user,s,rw.deadline);(void)rw_clock();return rc;
}
static int rw_commit(void *ignored,const struct es_binding *s,const uint8_t *p,size_t n,struct rp_segment_receipt *r) {
 (void)ignored;if(!rw_new_job())return RP_IO_FAILED;
 int rc=rw.hooks.seal_commit(rw.hooks.user,s,p,n,r,rw.deadline);
 if(rc==RP_IO_PENDING)rw.pending_deadline=rw.deadline;
 /* This also covers a partial final seal inside rp_end, not just a full-segment
  * seal inside push. An advertised slot that is unavailable is a broken budget
  * contract; any captured tail loss must remain a fault. */
 if(rc==RP_IO_FULL && rw.capacity)rw.report.fault=1;
 (void)rw_clock();return rc;
}
static int rw_poll(void *ignored,const struct es_binding *s,struct rp_segment_receipt *r) {
 (void)ignored;if(!rw.poll || !rw_guard())return RP_IO_FAILED;
 if(!rw.pending_deadline)return RP_IO_FAILED;
 int rc=rw.poll(rw.hooks.user,s,r,rw.deadline);(void)rw_clock();
 if(rc==RP_IO_OK)rw.pending_deadline=0;
 return rc;
}
static int rw_finalize(void *ignored,const struct rp_completion *s,struct rp_final_receipt *r) {
 (void)ignored;if(!rw_guard())return RP_IO_FAILED;
 /* The pipeline may discover FULL and finalize from inside push_frame(). Stop
  * before that catalog publication, without recursively entering the pipeline. */
 if(!rw.report.producer_fenced || !rw.report.queue_closed) {
  if(s->reason!=RP_REASON_FULL) { rw_latch(RW_CORE);return RP_IO_FAILED; }
  rw_latch(RW_FULL);
  if(!rw_fence() || !rw_close_queue() || !rw_guard())return RP_IO_FAILED;
 }
 if(!rw_begin_drain() || !rw_new_job())return RP_IO_FAILED;
 int rc=rw.hooks.finalize(rw.hooks.user,s,r,rw.deadline);(void)rw_clock();return rc;
}
static int rw_pipeline(void) {
 int rc=rp_get_status(&rw.report.pipeline);
 if(rc) { rw.report.error=rc;rw.report.fault=1;rw_latch(RW_CORE);return 0; }
 return 1;
}
static void rw_fault(void) {
 rw.report.fault=1;rw.report.stop_reason=atomic_load_explicit(&rw_stop,memory_order_acquire);
 if(rw.unsafe)rw.report.reason=rw.unsafe_reason;
 else if(atomic_load_explicit(&rw_external,memory_order_acquire))rw.report.reason=RW_EXTERNAL;
 else if(!rw.report.reason)rw.report.reason=rw.report.stop_reason;
 if(!rw.report.reason)rw.report.reason=RW_CORE;
 rw_state_set(RW_FAULT);
}
static int rw_release_lease(void) {
 if(!rw.report.lease_held)return 1;
 if(!rw_guard())return 0;
 int rc=rw.hooks.release(rw.hooks.user,rw.deadline);rw.report.callback_error=rc;
 if(rc || !rw_clock()) { rw.report.reason=RW_RELEASE_FAILED;rw_latch(RW_RELEASE_FAILED);return 0; }
 rw.report.lease_held=0;return 1;
}
static int rw_feed(void) {
 if(!rw_pipeline() || !rw_guard()) {
  ++rw.report.worker_discarded_frames;rw_wipe(&rw.frame,sizeof(rw.frame));return RW_ERROR;
 }
 uint64_t before=rw.report.pipeline.captured_samples;
 int rc=rp_push_frame(rw.frame.first_sample,rw.frame.pcm);
 rw_wipe(&rw.frame,sizeof(rw.frame));rw.report.error=rc;(void)rw_clock();
 if(!rw_pipeline())return RW_ERROR;
 if(rw.report.pipeline.captured_samples==before)++rw.report.worker_discarded_frames;
 else if(before>UINT64_MAX-320U || rw.report.pipeline.captured_samples!=before+320U) {
  rw.report.fault=1;rw_latch(RW_CORE);return RW_ERROR;
 }
 return rc;
}
static int rw_fence(void) {
 rw_state_set(RW_STOPPING);
 if(rw.queue_initialized && !rw.report.queue_closed)(void)rfq_request_stop(&rw_queue);
 /* Safety stop is the sole callback allowed after clock/timeout failure. It
  * receives the ORIGINAL deadline, not a renewed grace period. */
 if(rw.capture_possible) {
  if(rw.stop_attempted) { rw.report.reason=RW_STOP_FAILED;rw_fault();return 0; }
  rw.stop_attempted=1;
  int rc=rw.hooks.stop_and_join(rw.hooks.user,rw_token(),rw.deadline);
  rw.report.callback_error=rc;(void)rw_clock();
  if(rc) { rw.report.reason=RW_STOP_FAILED;rw_latch(RW_STOP_FAILED);rw_fault();return 0; }
  rw.capture_possible=0;
 }
 if(!rw_take(&rw_producer)) { rw.report.reason=RW_STOP_FAILED;rw_latch(RW_STOP_FAILED);rw_fault();return 0; }
 if(rw.queue_initialized && !rw.report.queue_closed && rfq_confirm_producer_stopped(&rw_queue)!=RFQ_OK) {
  rw_drop(&rw_producer);rw.report.reason=RW_STOP_FAILED;rw_latch(RW_STOP_FAILED);rw_fault();return 0;
 }
 rw.report.producer_fenced=1;rw_drop(&rw_producer);return 1;
}
static int rw_close_queue(void) {
 if(rw.queue_initialized && !rw.report.queue_closed) {
  int rc=rfq_discard(&rw_queue);if(rc || rfq_get_summary(&rw_queue,&rw.report.queue)!=RFQ_OK) {
   rw_latch(RW_CORE);rw_fault();return 0;
  }
  rw.report.queue_closed=1;rw.report.queue_summary_valid=1;
 }
 return 1;
}
static int rw_shutdown(void) {
 if(!rw.report.supervisor_confirmed) { rw_fault();return RW_ERROR; }
 if(!rw_fence())return RW_ERROR;
 unsigned reason=atomic_load_explicit(&rw_stop,memory_order_acquire);
 int can_drain=!rw.unsafe && !atomic_load_explicit(&rw_external,memory_order_acquire) &&
  (reason==RW_USER || reason==RW_LOW_POWER || reason==RW_OVERFLOW || (reason==RW_FULL && rw.capacity));
 if(!rw_pipeline())can_drain=0;
 if(rw.report.pipeline.state!=RP_RECORDING || rw.report.queue_closed)can_drain=0;
 if(can_drain && !rw_begin_drain())can_drain=0;
 if(can_drain)for(unsigned i=0;i<RFQ_CAPACITY;++i) {
  if(!rw_guard())break;
  int rc=rfq_pop(&rw_queue,&rw.frame);if(rc==RFQ_DRAINED)break;
  if(rc!=RFQ_OK) { rw_latch(RW_CORE);rw.report.fault=1;break; }
  rc=rw_feed();if(rc || rw.unsafe)break;
 }
 if(!rw_close_queue())return RW_ERROR;
 if(!rw_pipeline()) { rw_fault();return RW_ERROR; }
 reason=atomic_load_explicit(&rw_stop,memory_order_acquire);
 int finalizable=!rw.report.fault && rw_guard() &&
  (reason==RW_USER || reason==RW_LOW_POWER || reason==RW_OVERFLOW || (reason==RW_FULL && rw.capacity));
 if(rw.report.pipeline.state==RP_RECORDING || rw.report.pipeline.state==RP_PAUSED ||
    (rw.report.pipeline.state==RP_DRAINING && !finalizable)) {
  int rc;
  if(finalizable)rc=rp_end(reason==RW_FULL?RP_REASON_FULL:reason==RW_LOW_POWER?RP_REASON_LOW_POWER:reason==RW_OVERFLOW?RP_REASON_OVERFLOW:RP_REASON_USER);
  else rc=rp_external_fault();
  rw.report.error=rc;(void)rw_clock();
 }
 rw_wipe(&rw.frame,sizeof(rw.frame));
 if(!rw_pipeline()) { rw_fault();return RW_ERROR; }
 if(rw.report.pipeline.state==RP_DRAINING) {
  if(rw.poll && finalizable && rw_guard()) {
   rw.report.reason=reason;rw.report.stop_reason=reason;rw_state_set(RW_DRAINING);return RW_WAIT;
  }
  /* A submission may have returned PENDING while its callback became late or
   * requested external stop. This is local fault accounting, not more I/O. */
  (void)rp_external_fault();if(!rw_pipeline()) { rw_fault();return RW_ERROR; }
 }
 /* The storage worker still owns immutable ciphertext. Never release its
  * shared lease, disarm the original supervisor, retry or wipe its buffers. */
 if(rw.report.pipeline.pending_segments || (rw.poll && rw.report.pipeline.commit_uncertain)) { rw_fault();return RW_ERROR; }
 if(rw.report.pipeline.reason==RP_REASON_FULL && !rw.unsafe)rw_latch(RW_FULL);
 reason=atomic_load_explicit(&rw_stop,memory_order_acquire);rw.report.stop_reason=reason;rw.report.reason=reason;
 if(rw.report.pipeline.reason==RP_REASON_FULL && !rw.unsafe && !atomic_load(&rw_external))rw.report.reason=RW_FULL;
 if(rw.report.pipeline.state==RP_FAULT && !rw.unsafe && !atomic_load(&rw_external) &&
    (reason==RW_USER || reason==RW_LOW_POWER || reason==RW_FULL || reason==RW_NONE))rw.report.reason=RW_CORE;
 int clean=!rw.unsafe && !atomic_load_explicit(&rw_external,memory_order_acquire) && !rw.report.fault &&
  (reason==RW_USER || reason==RW_LOW_POWER || reason==RW_FULL) &&
  rw.report.pipeline.buffers_wiped==1 && rw.report.pipeline.commit_uncertain==0 &&
  rw.report.pipeline.manifest_finalized==1 && rw.report.pipeline.pending_segments==0;
 /* A completed overflow prefix can be valid, but overflow remains a fault and
  * this bridge cannot silently start another session after acquisition loss. */
 if(!rw.unsafe && rw.report.pipeline.buffers_wiped && !rw.report.pipeline.commit_uncertain) {
  if(!rw_release_lease())clean=0;
 } else clean=0;
 if(!rw_disarm())clean=0;
 if(clean) { rw_state_set(RW_STOPPED);return RW_COMPLETE; }
 rw_fault();return RW_ERROR;
}
static int rw_init_core(const struct rw_hooks *h,
 int (*poll)(void*,const struct es_binding*,struct rp_segment_receipt*,uint64_t),
 rw_capacity_fn capacity,enum rp_mode mode,uint32_t stack,uint32_t budget) {
 if(!h || !h->now_ms || !h->supervisor_arm || !h->supervisor_disarm || !h->acquire || !h->release ||
    !h->capture_start || !h->stop_and_join || !h->reserve || !h->seal_commit || !h->finalize ||
    (mode!=RP_MANUAL && mode!=RP_CONTINUOUS) || stack<RP_WORKER_STACK_MIN || !budget || budget>RW_CALL_BUDGET_MAX_MS)return RW_ARGUMENT;
 if(!atomic_is_lock_free(&rw_entry) || !atomic_is_lock_free(&rw_producer) || !atomic_is_lock_free(&rw_epoch) ||
    !atomic_is_lock_free(&rw_state_atomic) || !atomic_is_lock_free(&rw_initialized) ||
    !atomic_is_lock_free(&rw_stop) || !atomic_is_lock_free(&rw_external))return RW_ARGUMENT;
 if(!rw_take(&rw_entry))return RW_BUSY;
 unsigned initial=0;if(!atomic_compare_exchange_strong(&rw_initialized,&initial,1)) { rw_drop(&rw_entry);return RW_STATE; }
 rw.hooks=*h;rw.poll=poll;rw.capacity=capacity;rw.budget=budget;rw.report.mode=(uint32_t)mode;
 const struct rp_sink bridge={NULL,rw_reserve,rw_commit,rw_finalize};
 const struct rp_async_sink async_bridge={bridge,rw_poll};
 int rc=poll?rp_init_async_profile(&async_bridge,mode,stack,RP_PROFILE_CELT_LOW_DELAY):
  rp_init_profile(&bridge,mode,stack,RP_PROFILE_CELT_LOW_DELAY);
 if(rc) { rw_latch(RW_CORE);rw_fault(); }else rw_state_set(RW_IDLE);
 rw_drop(&rw_entry);return rc?RW_ERROR:RW_OK;
}
int rw_init(const struct rw_hooks *h,enum rp_mode mode,uint32_t stack,uint32_t budget)
{return rw_init_core(h,NULL,NULL,mode,stack,budget);}
int rw_init_async(const struct rw_async_hooks *h,enum rp_mode mode,uint32_t stack,uint32_t budget)
{if(!h||!h->poll)return RW_ARGUMENT;return rw_init_core(&h->submit,h->poll,NULL,mode,stack,budget);}
int rw_init_capacity_async(const struct rw_async_hooks *h,rw_capacity_fn capacity,
 enum rp_mode mode,uint32_t stack,uint32_t budget)
{if(!h||!h->poll||!capacity)return RW_ARGUMENT;return rw_init_core(&h->submit,h->poll,capacity,mode,stack,budget);}
int rw_select_mode(enum rp_mode mode) {
 if(mode!=RP_MANUAL && mode!=RP_CONTINUOUS)return RW_ARGUMENT;
 if(!rw_take(&rw_entry))return RW_BUSY;
 int rc=RW_STATE;
 if((rw.report.state==RW_IDLE || rw.report.state==RW_STOPPED) && !rw.report.fault) {
  rc=rp_select_mode(mode);if(!rc)rw.report.mode=(uint32_t)mode;
 }
 rw_drop(&rw_entry);return rc;
}
static int rw_start_inner(const struct es_binding *s,uint64_t first,rw_start_check_fn consent,void *consent_user) {
 if(!s || first>UINT64_MAX-320U || s->segment_sequence || s->plaintext_bytes)return RW_ARGUMENT;
 struct es_binding bound=*s;bound.plaintext_bytes=1;uint8_t header[ES_HEADER_BYTES];
 if(es_header_build(header,sizeof(header),&bound))return RW_ARGUMENT;
 if(!rw_take(&rw_entry))return RW_BUSY;
 if((rw.report.state!=RW_IDLE && rw.report.state!=RW_STOPPED) || rw.report.fault || rw.report.lease_held ||
    atomic_load(&rw_epoch)>=RW_EPOCH_MAX) { rw_drop(&rw_entry);return RW_STATE; }
 uint32_t mode=rw.report.mode;memset(&rw.report,0,sizeof(rw.report));rw.report.mode=mode;
 atomic_store(&rw_stop,RW_NONE);atomic_store(&rw_external,0);rw.unsafe=0;rw.unsafe_reason=0;rw.stop_attempted=0;
 rw.drain_deadline=0;rw.pending_deadline=0;rw_state_set(RW_STARTING);
 int rc=RW_ERROR;
 if(!rw_arm()) { rw_fault();goto done; }
 struct rw_readiness ready={0};
 int acquired=rw.hooks.acquire(rw.hooks.user,&ready,rw.deadline);rw.report.callback_error=acquired;
 if(acquired==0)rw.report.lease_held=1;
 if(!rw_clock() || acquired || ready.owner_verified!=1 || ready.recovery_verified!=1 || ready.store_ready!=1 ||
    ready.capture_ready!=1 || ready.power_safe!=1 || ready.profile_qualified!=1) {
  rw_latch(RW_ADMISSION);(void)rw_release_lease();(void)rw_disarm();rw_fault();goto done;
 }
 if(consent) {
  int admitted=consent(consent_user,0,0,rw.deadline);
  if(!rw_guard()||admitted<0||admitted>1){rw_latch(RW_ADMISSION);(void)rw_release_lease();(void)rw_disarm();rw_fault();goto done;}
  if(admitted){
   rw_latch(RW_USER);rw.report.reason=rw.report.stop_reason=RW_USER;
   if(!rw_release_lease()||!rw_disarm()){rw_fault();goto done;}
   rw_state_set(RW_IDLE);rc=RW_CANCELLED;goto done;
  }
 }
 if(rw.capacity) {
  uint32_t slots=0;
  int queried=rw.capacity(rw.hooks.user,s,&slots,rw.deadline);
  rw.report.callback_error=queried;
  if(!rw_guard() || queried || slots>RW_CAPACITY_MAX_SLOTS) {
   rw_latch(RW_ADMISSION);(void)rw_release_lease();(void)rw_disarm();rw_fault();goto done;
  }
  rw.report.capacity_known=1;rw.report.admitted_frames=slots*RP_SEGMENT_FRAMES;
  if(!slots) {
   /* No queue epoch, session reserve or microphone attempt has occurred. Do
    * not fabricate a finalized empty recording or erase an earlier receipt. */
   rw_latch(RW_FULL);rw.report.reason=rw.report.stop_reason=RW_FULL;
   if(!rw_release_lease() || !rw_disarm()) { rw_fault();goto done; }
   rw_state_set(RW_IDLE);rc=RW_NO_CAPACITY;goto done;
  }
  rw.remaining_frames=slots*RP_SEGMENT_FRAMES;
 }
 if(consent) {
  int admitted=consent(consent_user,0,0,rw.deadline);
  if(!rw_guard()||admitted<0||admitted>1){rw_latch(RW_ADMISSION);(void)rw_release_lease();(void)rw_disarm();rw_fault();goto done;}
  if(admitted){
   rw_latch(RW_USER);rw.report.reason=rw.report.stop_reason=RW_USER;
   if(!rw_release_lease()||!rw_disarm()){rw_fault();goto done;}
   rw_state_set(RW_IDLE);rc=RW_CANCELLED;goto done;
  }
 }
 if(!rw_take(&rw_producer)) { rw_latch(RW_STOP_FAILED);rw_fault();goto done; }
 int qrc=rw.queue_initialized?rfq_rearm_closed(&rw_queue,first):rfq_init(&rw_queue,first);
 if(qrc) { rw_drop(&rw_producer);rw_latch(RW_CORE);rw_fault();goto done; }
 rw.queue_initialized=1;unsigned previous=rw_token(),epoch=previous+1U;
 /* Never clear an unhandled producer fault that arrived during readiness/rearm. */
 if(!atomic_compare_exchange_strong_explicit(&rw_epoch,&previous,epoch,memory_order_acq_rel,memory_order_acquire)) {
  rw.report.epoch=rw_token();rw_drop(&rw_producer);rw_collect_fault();rc=rw_shutdown();goto done;
 }
 rw.report.epoch=epoch;rw_drop(&rw_producer);
 if(!rw_guard()) { rw_latch(RW_EXTERNAL);rc=rw_shutdown();goto done; }
 rc=rp_start(s,first);rw.report.error=rc;(void)rw_clock();
 if(rc || !rw_guard()) { rw_latch(rc==RP_STORAGE_FULL?RW_FULL:RW_CORE);rc=rw_shutdown();goto done; }
 if(consent) {
  int admitted=consent(consent_user,epoch,1,rw.deadline);
  if(!rw_guard()||admitted<0||admitted>1){rw_latch(RW_EXTERNAL);rc=rw_shutdown();goto done;}
  if(admitted){rw_latch(RW_USER);rc=rw_shutdown();goto done;}
 }
 rw.capture_possible=1;rw_state_set(RW_RUNNING);
 rc=rw.hooks.capture_start(rw.hooks.user,epoch,first,rw.deadline);rw.report.callback_error=rc;(void)rw_clock();
 if(rc || !rw_guard())rw_latch(RW_EXTERNAL);
 if(atomic_load_explicit(&rw_stop,memory_order_acquire)!=RW_NONE)rc=rw_shutdown();
 else if(!rw_disarm())rc=rw_shutdown();
 else rc=RW_OK;
done: rw_drop(&rw_entry);return rc;
}
int rw_start(const struct es_binding *s,uint64_t first)
{return rw_start_inner(s,first,NULL,NULL);}
int rw_start_controlled(const struct es_binding *s,uint64_t first,rw_start_check_fn check,void *user)
{if(!check||!rw.capacity)return RW_ARGUMENT;return rw_start_inner(s,first,check,user);}
int rw_submit(uint32_t epoch,uint64_t first,const int16_t pcm[320]) {
 if(!epoch || epoch>RW_EPOCH_MAX || !pcm)return RW_ARGUMENT;
 if(epoch!=rw_token())return RW_STALE;
 if(!rw_take(&rw_producer)) { rw_producer_fault(epoch);return RW_BUSY; }
 int rc;
 if(epoch!=rw_token())rc=RW_STALE;
 else if(atomic_load_explicit(&rw_epoch,memory_order_acquire)&RW_EPOCH_FAULT) { rw_latch(RW_EXTERNAL);rc=RW_STOPPED_INPUT; }
 else if(atomic_load_explicit(&rw_state_atomic,memory_order_acquire)!=RW_RUNNING ||
         atomic_load_explicit(&rw_stop,memory_order_acquire)!=RW_NONE)rc=RW_STOPPED_INPUT;
 else {
  if(rw.capacity && !rw.remaining_frames) {
   rw_latch(RW_FULL);rw_drop(&rw_producer);return RW_STOPPED_INPUT;
  }
  rc=rfq_push(&rw_queue,first,pcm);
  if(rc!=RFQ_OK) {
   int reason=rfq_stop_reason(&rw_queue);
   rw_latch(reason==RFQ_FULL?RW_OVERFLOW:reason==RFQ_CONTINUITY || reason==RFQ_SAMPLE_OVERFLOW?RW_CONTINUITY:RW_EXTERNAL);
   rc=RW_STOPPED_INPUT;
  }
  else if(rw.capacity && --rw.remaining_frames==0)rw_latch(RW_FULL);
 }
 rw_drop(&rw_producer);return rc;
}
int rw_request_stop(uint32_t epoch,enum rw_reason reason) {
 if(reason!=RW_USER && reason!=RW_LOW_POWER && reason!=RW_EXTERNAL)return RW_ARGUMENT;
 if(!epoch || epoch>RW_EPOCH_MAX || epoch!=rw_token())return RW_STALE;
 if(!rw_take(&rw_producer))return RW_BUSY;
 if(epoch!=rw_token()) { rw_drop(&rw_producer);return RW_STALE; }
 unsigned state=atomic_load_explicit(&rw_state_atomic,memory_order_acquire);
 if(state!=RW_RUNNING && state!=RW_STOPPING && state!=RW_DRAINING) { rw_drop(&rw_producer);return RW_STATE; }
 rw_latch((uint32_t)reason);rw_drop(&rw_producer);return RW_OK;
}
int rw_step(void) {
 if(!rw_take(&rw_entry))return RW_BUSY;
 int rc=RW_STATE;
 if(rw.report.state==RW_DRAINING) {
  /* Acquisition is joined and the queue closed. Merely waiting cannot rearm.
   * Only a NEW tail/finalization job can rearm within the fixed drain bound. */
  if(!rw.report.supervisor_confirmed || !rw.report.producer_fenced || !rw.report.queue_closed) {
   rw.report.fault=1;rw_latch(RW_CORE);rw_fault();rc=RW_ERROR;goto done;
  }
  if(!rw_guard()) { rc=rw_shutdown();goto done; }
  int polled=rp_poll();rw.report.error=polled;(void)rw_clock();rw_collect_fault();
  if(!rw_pipeline()) { rw_fault();rc=RW_ERROR;goto done; }
  if(polled<0 && polled!=RP_STORAGE_FULL) { rw.report.fault=1;rw_latch(RW_CORE); }
  if(!rw_guard() || polled<0 || rw.report.pipeline.state!=RP_DRAINING)rc=rw_shutdown();
  else rc=RW_WAIT;
  goto done;
 }
 if(rw.report.state!=RW_RUNNING)goto done;
 if(!rw_arm()) { rc=rw_shutdown();goto done; }
 rw_collect_fault();
 if(atomic_load_explicit(&rw_stop,memory_order_acquire)!=RW_NONE) { rc=rw_shutdown();goto done; }
 rc=rfq_pop(&rw_queue,&rw.frame);
 if(rc==RFQ_EMPTY) {
  if(rw.poll) {
   int polled=rp_poll();rw.report.error=polled;(void)rw_clock();rw_collect_fault();
   if(polled<0 || !rw_guard()) { rw_latch(RW_CORE);rc=rw_shutdown();goto done; }
  }
  rc=rw_disarm()?RW_WAIT:rw_shutdown();goto done;
 }
 if(rc!=RFQ_OK) { rw_latch(RW_CORE);rc=rw_shutdown();goto done; }
 rc=rw_feed();
 rw_collect_fault();
 if(rc || rw.unsafe)rw_latch(rc==RP_STORAGE_FULL?RW_FULL:RW_CORE);
 if(atomic_load_explicit(&rw_stop,memory_order_acquire)!=RW_NONE)rc=rw_shutdown();
 else if(!rw_disarm())rc=rw_shutdown();else rc=RW_OK;
done: rw_drop(&rw_entry);return rc;
}
int rw_get_status(struct rw_status *out) {
 if(!out)return RW_ARGUMENT;
 if(!rw_take(&rw_entry))return RW_BUSY;
 int rc=rp_get_status(&rw.report.pipeline);if(!rc)*out=rw.report;
 rw_drop(&rw_entry);return rc?RW_ERROR:RW_OK;
}
