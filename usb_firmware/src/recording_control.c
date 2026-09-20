#include "recording_control.h"
#include <string.h>
static int zero(const uint8_t *p,size_t n){unsigned z=0;while(n--)z|=*p++;return !z;}
static int identity(const uint8_t *p,size_t n){unsigned z=0,f=0;while(n--){z|=*p;f|=*p++^255U;}return z&&f;}
static int separate(const void *a,size_t n,const void *b,size_t m)
{uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return a&&b&&n<=UINTPTR_MAX-x&&m<=UINTPTR_MAX-y&&(x+n<=y||y+m<=x);}
static int enter(struct recording_control *c){unsigned v=0;return atomic_compare_exchange_strong(&c->gate,&v,1);}
static int leave(struct recording_control *c,int rc){atomic_store(&c->gate,0);return rc;}
static int active(const struct lc_state *s){return s->phase>=LC_STARTING&&s->phase<=LC_DRAINING;}
static struct lc_state *state_at(struct recording_control *c,uint32_t ticket)
{if(ticket&LRC_LOCAL_TICKET)return ticket==atomic_load(&c->local_ticket)?&c->local:NULL;
 return ticket&&ticket<=c->used?&c->states[ticket-1U]:NULL;}
static void stop_ticket(struct recording_control *c,uint32_t ticket)
{if(ticket&LRC_LOCAL_TICKET){if(ticket==atomic_load(&c->local_ticket))atomic_store(&c->local_stop,1);}
 else if(ticket&&ticket<=c->used)atomic_store(&c->stop[ticket-1U],1);}
static uint32_t find_operation(struct recording_control *c,const uint8_t operation[16])
{if(atomic_load(&c->local_ticket)&&!memcmp(operation,c->local.operation,16))return atomic_load(&c->local_ticket);
 for(unsigned i=0;i<c->used;++i)if(!memcmp(operation,c->states[i].operation,16))return i+1U;return 0;}
static void local_operation(struct recording_control *c,uint32_t count,uint8_t operation[16])
{memcpy(operation,c->idle.boot,16);operation[0]^=0xa5;operation[1]^=0x73;
 operation[6]=(operation[6]&15U)|0x40U;operation[8]=(operation[8]&63U)|0x80U;
 for(unsigned i=0;i<4;i++)operation[12+i]=(uint8_t)(count>>(24U-i*8U));}
static int local_namespace(struct recording_control *c,const uint8_t operation[16])
{uint8_t prefix[16];local_operation(c,0,prefix);return !memcmp(prefix,operation,12);}
static int tick(struct recording_control *c,uint64_t *t)
{*t=c->port.now_ms(c->port.user);if(*t<c->last_now){c->fault=1;return -1;}c->last_now=*t;return 0;}
static void cancel_arm(struct recording_control *c,int usb)
{
 struct lc_state *s=&c->states[c->armed-1U];atomic_store(&c->stop[c->armed-1U],1);
 s->phase=LC_CANCELLED_BEFORE_START;s->reason=1;
 s->flags=(uint16_t)(LC_PROFILE_FLAGS|LC_PRODUCER_JOINED|LC_STORAGE_JOINED|LC_RELEASED|(usb?LC_USB_PRESENT:0));
 c->armed=0;c->armed_deadline=0;
}
int lrc_init(struct recording_control *c,const struct lrc_port *p,const uint8_t boot[16],const uint8_t binding[32])
{
 if(!separate(c,sizeof(*c),p,sizeof(*p))||!separate(c,sizeof(*c),boot,16)||!separate(c,sizeof(*c),binding,32)||
    !p->now_ms||!p->authorized||!p->start_ready||!p->queue_start||!p->wake||!p->usb_present||
    c->initialized||!identity(boot,16)||!identity(binding,32))return LRC_ARGUMENT;
 memset(c,0,sizeof(*c));atomic_init(&c->gate,0);atomic_init(&c->issued,0);atomic_init(&c->fault,0);atomic_init(&c->submitting,0);
 atomic_init(&c->local_ticket,0);atomic_init(&c->local_stop,0);
 for(unsigned i=0;i<LRC_OPERATIONS;++i)atomic_init(&c->stop[i],0);
 if(!atomic_is_lock_free(&c->gate)||!atomic_is_lock_free(&c->stop[0])||
    !atomic_is_lock_free(&c->issued)||!atomic_is_lock_free(&c->fault)||!atomic_is_lock_free(&c->submitting)||
    !atomic_is_lock_free(&c->local_ticket)||!atomic_is_lock_free(&c->local_stop))return LRC_REFUSED;
 c->port=*p;memcpy(c->binding,binding,32);memcpy(c->idle.boot,boot,16);
 c->idle.flags=LC_PROFILE_FLAGS|LC_PRODUCER_JOINED|LC_STORAGE_JOINED|LC_RELEASED;c->initialized=1;
 return 0;
}
int lrc_command(struct recording_control *c,uint64_t conn,const uint8_t *frame,size_t n,uint8_t reply[LC_RESPONSE_BYTES])
{
 struct lc_request request;uint64_t now;uint32_t ticket=0;int start=0;
 if(!separate(c,sizeof(*c),frame,n)||!separate(c,sizeof(*c),reply,LC_RESPONSE_BYTES)||
    !c->initialized||!conn||lc_parse_request(frame,n,c->binding,c->idle.boot,&request))return LRC_ARGUMENT;
 /* Authorization may call platform primitives; do not hold the metadata gate. */
 if(c->port.authorized(c->port.user,conn)!=1)return LRC_REFUSED;
 int usb=c->port.usb_present(c->port.user);if(usb!=0&&usb!=1)return LRC_REFUSED;
 int ready=request.command==LC_START?c->port.start_ready(c->port.user):0;
 if(!enter(c))return LRC_DEFERRED; /* No sequence/operation was admitted. */
 if(c->fault||tick(c,&now)||c->submitting)return leave(c,LRC_REFUSED);
 if(c->connection&&c->connection!=conn)return leave(c,LRC_REFUSED);
 if(!c->connection){
  if(conn<=c->last_connection)return leave(c,LRC_REFUSED);
  c->connection=c->last_connection=conn;c->sequence=0;
 }
 if(request.sequence<=c->sequence)return leave(c,LRC_REFUSED);
 c->sequence=request.sequence; /* every admitted parser request, even refused selector */
 if(zero(request.operation,16))ticket=c->current;
 else ticket=find_operation(c,request.operation);
 /* Reserved local IDs can be queried/stopped, never replayed as remote START. */
 if(request.command==LC_START&&local_namespace(c,request.operation))return leave(c,LRC_REFUSED);
 if(request.command==LC_START&&!ticket){
  if(ready!=1||(!LC_PROFILE_FLAGS&&usb!=1)||c->used==LRC_OPERATIONS||(c->current&&active(state_at(c,c->current)))||
     now>UINT64_MAX-LRC_PREPARE_MS)return leave(c,LRC_REFUSED);
  if(request.mode==2U&&(!c->button.seen||c->button.candidate||c->button.stable||c->button.count))return leave(c,LRC_REFUSED);
  ticket=++c->used;c->current=ticket;struct lc_state *s=&c->states[ticket-1U];
  memcpy(s->boot,c->idle.boot,16);memcpy(s->operation,request.operation,16);
  s->phase=LC_STARTING;s->flags=(uint16_t)(LC_PROFILE_FLAGS|(usb?LC_USB_PRESENT:0)|LC_PRODUCER_JOINED|LC_STORAGE_JOINED);
  atomic_store(&c->stop[ticket-1U],0);atomic_store(&c->issued,ticket);c->submitting=1;start=1;
  if(request.mode==2U){s->flags|=LC_BUTTON_ARMED;c->armed=ticket;c->armed_deadline=now+LRC_PREPARE_MS;c->submitting=0;start=0;}
 }else if(!ticket&&!zero(request.operation,16))return leave(c,LRC_REFUSED);
 if(request.command==LC_STOP&&ticket&&active(state_at(c,ticket)))stop_ticket(c,ticket);
 if(request.command==LC_STOP&&ticket&&c->armed==ticket)cancel_arm(c,usb);
 leave(c,0);
 if(request.command==LC_STOP)c->port.wake(c->port.user);
 if(start){
  int queued=c->port.queue_start(c->port.user,ticket,now+LRC_PREPARE_MS);
  if(!enter(c)){
   /* A successful callback admitted the exact immutable ticket. Publication
    * may own the gate now; do not permanently strand STATUS/STOP admission. */
   if(queued)atomic_store(&c->fault,1);
   atomic_store(&c->submitting,0);return queued?LRC_FAULT:LRC_BUSY;
  }
  c->submitting=0;
  if(queued){
   struct lc_state *s=&c->states[ticket-1U];
   if(s->phase!=LC_STARTING||s->epoch){c->fault=1;return leave(c,LRC_FAULT);}
   s->phase=LC_FAULT;s->reason=10;s->flags=(uint16_t)(LC_PROFILE_FLAGS|LC_FAULT_LATCH|(usb?LC_USB_PRESENT:0));
  }
  leave(c,0);
 }
 if(!enter(c))return LRC_BUSY;
 if(c->fault||tick(c,&now)||c->connection!=conn||c->sequence!=request.sequence)return leave(c,LRC_REFUSED);
 struct lc_state *target=state_at(c,ticket);
 /* A local terminal slot may have been replaced while the gate was released.
  * Never answer an old selector using the replacement recording. */
 if(ticket&&!target)return leave(c,LRC_REFUSED);
 struct lc_state snapshot=ticket?*target:c->idle;
 if(ticket&&lrc_stop_requested(c,ticket)==1)snapshot.flags|=LC_STOP_REQUESTED;
 snapshot.flags=(uint16_t)((snapshot.flags&~LC_USB_PRESENT)|(usb?LC_USB_PRESENT:0));
 int result=lc_encode_response(reply,LC_RESPONSE_BYTES,&request,&snapshot);
 return leave(c,result?LRC_PENDING:LRC_OK);
}
int lrc_reply_allowed(struct recording_control *c,uint64_t conn,uint16_t seq)
{
 if(!c||!c->initialized||!conn||!seq||c->port.authorized(c->port.user,conn)!=1||!enter(c))return 0;
 uint64_t t;int ok=!c->fault&&!tick(c,&t)&&c->connection==conn&&c->sequence==seq;
 leave(c,0);return ok;
}
int lrc_disconnected(struct recording_control *c,uint64_t conn)
{
 if(!c||!c->initialized||!conn)return LRC_ARGUMENT;
 if(!enter(c))return LRC_BUSY;
 if(c->connection!=conn){
  /* A disconnect may arrive after authorization but before a first command
   * claims its epoch. Preserve that revocation; do not later adopt a dead peer.
   * A different already-active peer is not disconnected or stopped here. */
  if(conn<=c->last_connection)return leave(c,LRC_REFUSED);
  c->last_connection=conn;return leave(c,0);
 }
 c->connection=0;c->sequence=0;
 /* An explicit recording survives radio loss. Only STOP or local button stops it. */
 leave(c,0);c->port.wake(c->port.user);return 0;
}
int lrc_pending_reply(struct recording_control *c,uint64_t conn,const uint8_t *frame,size_t n,uint8_t out[LC_RESPONSE_BYTES])
{
 struct lc_request r;struct lc_state s;uint64_t t;
 if(!separate(c,sizeof(*c),frame,n)||!separate(c,sizeof(*c),out,LC_RESPONSE_BYTES)||
    !c->initialized||lc_parse_request(frame,n,c->binding,c->idle.boot,&r))return LRC_ARGUMENT;
 if(c->port.authorized(c->port.user,conn)!=1)return LRC_REFUSED;
 int usb=c->port.usb_present(c->port.user);if(usb!=0&&usb!=1)return LRC_REFUSED;
 if(!enter(c))return LRC_BUSY;
 if(c->fault||tick(c,&t)||c->connection!=conn||c->sequence!=r.sequence)return leave(c,LRC_REFUSED);
 if(c->submitting)return leave(c,LRC_BUSY);
 uint32_t ticket=0;
 if(zero(r.operation,16))ticket=c->current;
 else ticket=find_operation(c,r.operation);
 if(!ticket&&!zero(r.operation,16))return leave(c,LRC_REFUSED);
 s=ticket?*state_at(c,ticket):c->idle;
 if(ticket&&lrc_stop_requested(c,ticket)==1)s.flags|=LC_STOP_REQUESTED;
 s.flags=(uint16_t)((s.flags&~LC_USB_PRESENT)|(usb?LC_USB_PRESENT:0));
 return leave(c,lc_encode_response(out,LC_RESPONSE_BYTES,&r,&s)?LRC_PENDING:LRC_OK);
}
int lrc_publish(struct recording_control *c,uint32_t ticket,const struct lc_state *s)
{
 if(!separate(c,sizeof(*c),s,sizeof(*s))||!c->initialized)return LRC_ARGUMENT;
 if(!enter(c))return LRC_BUSY;
 struct lc_state *target=state_at(c,ticket);
 if(c->fault||!target||ticket!=c->current)return leave(c,LRC_REFUSED);
 if(lc_same_operation_progress(target,s)){c->fault=1;return leave(c,LRC_FAULT);}
 *target=*s;return leave(c,0);
}
int lrc_snapshot(struct recording_control *c,uint32_t ticket,struct lc_state *out)
{
 if(!separate(c,sizeof(*c),out,sizeof(*out))||!c->initialized)return LRC_ARGUMENT;
 if(!enter(c))return LRC_BUSY;
 struct lc_state *target=state_at(c,ticket);
 if(c->fault||(ticket&&!target))return leave(c,LRC_REFUSED);
 *out=ticket?*target:c->idle;return leave(c,0);
}
int lrc_stop_requested(struct recording_control *c,uint32_t ticket)
{if(!c||!c->initialized||!ticket||atomic_load(&c->fault))return -1;
 if(ticket&LRC_LOCAL_TICKET)return ticket==atomic_load(&c->local_ticket)?(int)atomic_load(&c->local_stop):-1;
 if(ticket>atomic_load(&c->issued))return -1;
 return atomic_load(&c->stop[ticket-1U])?1:0;}
int lrc_maintenance_claim(struct recording_control *c)
{
 if(!c||!c->initialized||!enter(c))return 0;
 int idle=!c->fault&&!c->submitting&&!c->armed;
 if(c->current){unsigned p=state_at(c,c->current)->phase;
  if(p>=LC_STARTING&&p<=LC_DRAINING)idle=0;}
 if(!idle)leave(c,0);return idle;
}
void lrc_maintenance_release(struct recording_control *c){(void)leave(c,0);}
int lrc_set_standalone(struct recording_control *c,int enabled)
{if(!c||!c->initialized||(enabled!=0&&enabled!=1)||!enter(c))return LRC_ARGUMENT;
 if(c->fault||c->submitting||c->armed||(c->current&&active(state_at(c,c->current))))return leave(c,LRC_REFUSED);
 c->standalone=(uint32_t)enabled;c->button=(struct button_gesture){0};c->button_local_ready=0;return leave(c,0);}
int lrc_button_sample(struct recording_control *c,int pressed)
{
 if(!c||!c->initialized)return LRC_ARGUMENT;
 int usb=c->port.usb_present(c->port.user),ready=c->port.start_ready(c->port.user);
 if(!enter(c))return LRC_BUSY;
 uint64_t t;uint32_t start=0;int stopped=0;
 if(c->fault||tick(c,&t)||c->submitting)return leave(c,LRC_REFUSED);
 if(c->armed&&((!LC_PROFILE_FLAGS&&usb!=1)||pressed<0||t>=c->armed_deadline))cancel_arm(c,usb==1);
 enum bg_event event=bg_sample(&c->button,pressed,t);
 if(pressed!=0&&pressed!=1){c->button_local_ready=0;return leave(c,LRC_REFUSED);}
 if(event==BG_PRESS){
  c->button_ticket=c->current;
  c->button_local_ready=c->standalone&&(!c->current||!active(state_at(c,c->current)));
 }
 if(event==BG_FIVE){
  int idle=c->button_local_ready&&c->button_ticket==c->current&&!c->armed&&(!c->current||!active(state_at(c,c->current)));
  c->button_local_ready=0;
  return leave(c,idle?LRC_PAIRING_REQUEST:LRC_REFUSED);
 }
 if(event==BG_SINGLE){
   uint32_t selected=c->button_ticket;c->button_ticket=0;
   if(selected&&selected==c->current&&active(state_at(c,selected))){
    if(c->armed==selected){
     if(ready!=1||(!LC_PROFILE_FLAGS&&usb!=1)||t>UINT64_MAX-LRC_PREPARE_MS)cancel_arm(c,usb==1);
     else{start=selected;c->armed=0;c->armed_deadline=0;c->states[selected-1U].flags&=(uint16_t)~LC_BUTTON_ARMED;c->submitting=1;}
    }else{stop_ticket(c,selected);stopped=1;}
   }else if(selected==c->current&&c->button_local_ready&&c->standalone&&
       ready==1&&(LC_PROFILE_FLAGS||usb==1)&&c->local_count<LRC_LOCAL_TICKET-1U&&t<=UINT64_MAX-LRC_PREPARE_MS){
    /* No app permit: the debounced physical action is the start authority. */
    start=LRC_LOCAL_TICKET|++c->local_count;memset(&c->local,0,sizeof(c->local));
    memcpy(c->local.boot,c->idle.boot,16);local_operation(c,c->local_count,c->local.operation);
    c->local.phase=LC_STARTING;c->local.flags=(uint16_t)(LC_PROFILE_FLAGS|(usb?LC_USB_PRESENT:0)|LC_PRODUCER_JOINED|LC_STORAGE_JOINED);
    atomic_store(&c->local_stop,0);atomic_store(&c->local_ticket,start);c->current=start;c->submitting=1;
   }
   c->button_local_ready=0;
 }
 leave(c,0);
 if(stopped)c->port.wake(c->port.user);
 if(!start)return 0;
 int queued=c->port.queue_start(c->port.user,start,t+LRC_PREPARE_MS);
 if(!enter(c)){if(queued)atomic_store(&c->fault,1);atomic_store(&c->submitting,0);return queued?LRC_FAULT:LRC_BUSY;}
 c->submitting=0;
 if(queued){struct lc_state *s=state_at(c,start);s->phase=LC_FAULT;s->reason=10;s->flags=(uint16_t)(LC_PROFILE_FLAGS|LC_FAULT_LATCH|(usb==1?LC_USB_PRESENT:0));}
 return leave(c,queued?LRC_REFUSED:0);
}
