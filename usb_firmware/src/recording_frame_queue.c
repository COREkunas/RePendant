#include "recording_frame_queue.h"
#include <string.h>

_Static_assert(sizeof(unsigned)==4,"ticket width");
_Static_assert((RFQ_CAPACITY&(RFQ_CAPACITY-1U))==0,"power-of-two capacity");
_Static_assert(sizeof(struct rfq_frame)==648,"exact frame storage");
static void wipe(void *value,size_t bytes) {
 volatile uint8_t *p=value; while(bytes--) *p++=0;
}
static int span(const void *p,size_t n,size_t alignment) {
 uintptr_t at=(uintptr_t)p;return p && at%alignment==0 && at<=UINTPTR_MAX-n;
}
static int queue_valid(const struct rfq_queue *q) { return span(q,sizeof(*q),_Alignof(struct rfq_queue)); }
static int apart(const struct rfq_queue *q,const void *p,size_t n,size_t align) {
 if(!queue_valid(q) || !span(p,n,align))return 0;
 uintptr_t a=(uintptr_t)q,b=(uintptr_t)p;
 return a+sizeof(*q)<=b || b+n<=a;
}
static int admitted(unsigned state) { return state==RFQ_RUNNING || state==RFQ_FENCED || state==RFQ_CLOSED; }
static int take(atomic_uint *guard) {
 unsigned expected=0;
 return atomic_compare_exchange_strong_explicit(guard,&expected,1,memory_order_acquire,memory_order_relaxed);
}
static void release(atomic_uint *guard) { atomic_store_explicit(guard,0,memory_order_release); }
static void stop(struct rfq_queue *q,unsigned reason) {
 unsigned expected=RFQ_NONE;
 (void)atomic_compare_exchange_strong_explicit(&q->reason,&expected,reason,memory_order_acq_rel,memory_order_acquire);
}
int rfq_init(struct rfq_queue *q,uint64_t first) {
 if(!queue_valid(q))return RFQ_ARGUMENT;
 if(!atomic_is_lock_free(&q->state) || !atomic_is_lock_free(&q->reason) ||
    !atomic_is_lock_free(&q->head) || !atomic_is_lock_free(&q->tail) ||
    !atomic_is_lock_free(&q->producer_busy) || !atomic_is_lock_free(&q->consumer_busy))return RFQ_UNSUPPORTED;
 unsigned expected=RFQ_FRESH;
 if(!atomic_compare_exchange_strong_explicit(&q->state,&expected,RFQ_INITIALIZING,memory_order_acq_rel,memory_order_acquire))return RFQ_STATE;
 atomic_init(&q->reason,RFQ_NONE);atomic_init(&q->head,0);atomic_init(&q->tail,0);
 atomic_init(&q->producer_busy,0);atomic_init(&q->consumer_busy,0);
 q->first_sample=first;q->next_sample=first;q->accepted=0;q->consumed=0;q->discarded=0;
 wipe(q->slots,sizeof(q->slots));
 atomic_store_explicit(&q->state,RFQ_RUNNING,memory_order_release);return RFQ_OK;
}
int rfq_push(struct rfq_queue *q,uint64_t first,const int16_t pcm[RFQ_FRAME_SAMPLES]) {
 if(!apart(q,pcm,RFQ_FRAME_SAMPLES*sizeof(*pcm),_Alignof(int16_t)))return RFQ_ARGUMENT;
 if(atomic_load_explicit(&q->state,memory_order_acquire)!=RFQ_RUNNING)return RFQ_STATE;
 if(!take(&q->producer_busy)) { stop(q,RFQ_REENTRY);return RFQ_BUSY; }
 int rc=RFQ_STOPPED;
 if(atomic_load_explicit(&q->state,memory_order_acquire)!=RFQ_RUNNING ||
    atomic_load_explicit(&q->reason,memory_order_acquire)!=RFQ_NONE)goto done;
 if(first!=q->next_sample) { stop(q,RFQ_CONTINUITY);goto done; }
 if(first>UINT64_MAX-RFQ_FRAME_SAMPLES) { stop(q,RFQ_SAMPLE_OVERFLOW);goto done; }
 unsigned head=atomic_load_explicit(&q->head,memory_order_relaxed);
 unsigned tail=atomic_load_explicit(&q->tail,memory_order_acquire);
 unsigned used=head-tail;
 if(used>=RFQ_CAPACITY) { stop(q,used==RFQ_CAPACITY?RFQ_FULL:RFQ_INVARIANT);goto done; }
 struct rfq_frame *slot=&q->slots[head&(RFQ_CAPACITY-1U)];
 slot->first_sample=first;memcpy(slot->pcm,pcm,sizeof(slot->pcm));
 q->next_sample=first+RFQ_FRAME_SAMPLES;++q->accepted;
 atomic_store_explicit(&q->head,head+1U,memory_order_release);rc=RFQ_OK;
done: release(&q->producer_busy);return rc;
}
int rfq_rearm_closed(struct rfq_queue *q,uint64_t first) {
 if(!queue_valid(q))return RFQ_ARGUMENT;
 if(atomic_load_explicit(&q->state,memory_order_acquire)!=RFQ_CLOSED)return RFQ_STATE;
 if(!take(&q->producer_busy))return RFQ_BUSY;
 if(!take(&q->consumer_busy)) { release(&q->producer_busy);return RFQ_BUSY; }
 int rc=RFQ_STATE;
 if(atomic_load_explicit(&q->state,memory_order_acquire)!=RFQ_CLOSED)goto done;
 if(atomic_load_explicit(&q->head,memory_order_acquire)!=atomic_load_explicit(&q->tail,memory_order_acquire))goto done;
 const uint8_t *bytes=(const uint8_t*)q->slots;uint8_t nonzero=0;
 for(size_t i=0;i<sizeof(q->slots);++i)nonzero|=bytes[i];
 if(nonzero)goto done;
 q->first_sample=first;q->next_sample=first;q->accepted=0;q->consumed=0;q->discarded=0;
 atomic_store_explicit(&q->reason,RFQ_NONE,memory_order_relaxed);
 atomic_store_explicit(&q->head,0,memory_order_relaxed);atomic_store_explicit(&q->tail,0,memory_order_relaxed);
 atomic_store_explicit(&q->state,RFQ_RUNNING,memory_order_release);rc=RFQ_OK;
done: release(&q->consumer_busy);release(&q->producer_busy);return rc;
}
int rfq_pop(struct rfq_queue *q,struct rfq_frame *out) {
 if(!apart(q,out,sizeof(*out),_Alignof(struct rfq_frame)))return RFQ_ARGUMENT;
 unsigned state=atomic_load_explicit(&q->state,memory_order_acquire);
 if(state!=RFQ_RUNNING && state!=RFQ_FENCED)return RFQ_STATE;
 if(!take(&q->consumer_busy)) { stop(q,RFQ_REENTRY);return RFQ_BUSY; }
 int rc=RFQ_STATE;
 state=atomic_load_explicit(&q->state,memory_order_acquire);
 if(state!=RFQ_RUNNING && state!=RFQ_FENCED)goto done;
 if(state==RFQ_RUNNING && atomic_load_explicit(&q->reason,memory_order_acquire)!=RFQ_NONE) { rc=RFQ_NEEDS_FENCE;goto done; }
 unsigned tail=atomic_load_explicit(&q->tail,memory_order_relaxed);
 unsigned head=atomic_load_explicit(&q->head,memory_order_acquire);
 unsigned used=head-tail;
 if(used>RFQ_CAPACITY) { stop(q,RFQ_INVARIANT);rc=RFQ_STOPPED;goto done; }
 if(!used) { rc=state==RFQ_FENCED?RFQ_DRAINED:RFQ_EMPTY;goto done; }
 struct rfq_frame *slot=&q->slots[tail&(RFQ_CAPACITY-1U)];
 memcpy(out,slot,sizeof(*out));wipe(slot,sizeof(*slot));++q->consumed;
 atomic_store_explicit(&q->tail,tail+1U,memory_order_release);rc=RFQ_OK;
done: release(&q->consumer_busy);return rc;
}
int rfq_request_stop(struct rfq_queue *q) {
 if(!queue_valid(q))return RFQ_ARGUMENT;
 unsigned state=atomic_load_explicit(&q->state,memory_order_acquire);
 if(state!=RFQ_RUNNING && state!=RFQ_FENCED)return RFQ_STATE;
 stop(q,RFQ_USER);return RFQ_OK;
}
int rfq_stop_reason(const struct rfq_queue *q) {
 if(!queue_valid(q))return RFQ_ARGUMENT;
 if(!admitted(atomic_load_explicit(&q->state,memory_order_acquire)))return RFQ_STATE;
 return (int)atomic_load_explicit(&q->reason,memory_order_acquire);
}
int rfq_confirm_producer_stopped(struct rfq_queue *q) {
 if(!queue_valid(q))return RFQ_ARGUMENT;
 if(atomic_load_explicit(&q->state,memory_order_acquire)==RFQ_FENCED)return RFQ_OK;
 if(atomic_load_explicit(&q->state,memory_order_acquire)!=RFQ_RUNNING ||
    atomic_load_explicit(&q->reason,memory_order_acquire)==RFQ_NONE)return RFQ_STATE;
 if(!take(&q->producer_busy))return RFQ_BUSY;
 int rc=RFQ_STATE;
 if(atomic_load_explicit(&q->state,memory_order_acquire)==RFQ_RUNNING) {
  atomic_store_explicit(&q->state,RFQ_FENCED,memory_order_release);rc=RFQ_OK;
 }
 release(&q->producer_busy);return rc;
}
static int close_queue(struct rfq_queue *q,int discard) {
 if(!queue_valid(q))return RFQ_ARGUMENT;
 if(atomic_load_explicit(&q->state,memory_order_acquire)!=RFQ_FENCED)return RFQ_NEEDS_FENCE;
 if(!take(&q->consumer_busy)) { stop(q,RFQ_REENTRY);return RFQ_BUSY; }
 int rc=RFQ_STATE;
 if(atomic_load_explicit(&q->state,memory_order_acquire)!=RFQ_FENCED)goto done;
 unsigned head=atomic_load_explicit(&q->head,memory_order_acquire);
 unsigned tail=atomic_load_explicit(&q->tail,memory_order_relaxed);
 unsigned used=head-tail;
 if(used>RFQ_CAPACITY) { stop(q,RFQ_INVARIANT);rc=RFQ_STOPPED;goto done; }
 if(used && !discard) { rc=RFQ_NOT_EMPTY;goto done; }
 wipe(q->slots,sizeof(q->slots));q->discarded+=used;
 atomic_store_explicit(&q->tail,head,memory_order_release);
 atomic_store_explicit(&q->state,RFQ_CLOSED,memory_order_release);rc=RFQ_OK;
done: release(&q->consumer_busy);return rc;
}
int rfq_finish(struct rfq_queue *q) { return close_queue(q,0); }
int rfq_discard(struct rfq_queue *q) { return close_queue(q,1); }
int rfq_get_summary(struct rfq_queue *q,struct rfq_summary *out) {
 if(!apart(q,out,sizeof(*out),_Alignof(struct rfq_summary)))return RFQ_ARGUMENT;
 unsigned state=atomic_load_explicit(&q->state,memory_order_acquire);
 if(state!=RFQ_FENCED && state!=RFQ_CLOSED)return RFQ_NEEDS_FENCE;
 if(!take(&q->consumer_busy))return RFQ_BUSY;
 struct rfq_summary s;memset(&s,0,sizeof(s));
 s.state=atomic_load_explicit(&q->state,memory_order_acquire);
 s.reason=atomic_load_explicit(&q->reason,memory_order_acquire);
 s.queued=atomic_load_explicit(&q->head,memory_order_acquire)-atomic_load_explicit(&q->tail,memory_order_relaxed);
 s.first_sample=q->first_sample;s.next_sample=q->next_sample;s.accepted=q->accepted;s.consumed=q->consumed;s.discarded=q->discarded;
 memcpy(out,&s,sizeof(s));release(&q->consumer_busy);return RFQ_OK;
}
