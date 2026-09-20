/* SPDX-License-Identifier: Apache-2.0 */
#include "recording_store_bridge.h"
#include <string.h>

#define RSB_MAGIC UINT32_C(0x52534231)
static int overlap(const void *a,size_t an,const void *b,size_t bn)
{uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return an>UINTPTR_MAX-x||bn>UINTPTR_MAX-y||(x<y+bn&&y<x+an);}
static int external(struct rsb_context *c,const void *p,size_t n)
{return p&&n&&!overlap(c,sizeof(*c),p,n)&&!overlap(c->store,sizeof(*c->store),p,n);}
static int same(const struct es_binding *a,const struct es_binding *b)
{return a->generation==b->generation&&a->segment_sequence==b->segment_sequence&&a->plaintext_bytes==b->plaintext_bytes&&
 !memcmp(a->key_fingerprint,b->key_fingerprint,32)&&!memcmp(a->device_id,b->device_id,16)&&
 !memcmp(a->volume_id,b->volume_id,16)&&!memcmp(a->recording_id,b->recording_id,16);}
static int take(struct rsb_context *c)
{unsigned zero=0;if(!c||c->initialized!=RSB_MAGIC)return RSB_ARGUMENT;
 return atomic_compare_exchange_strong(&c->gate,&zero,1)?RSB_OK:RSB_BUSY;}
static int leave(struct rsb_context *c,int rc){atomic_store(&c->gate,0);return rc;}
static int failed(struct rsb_context *c,int rc)
{c->state=RSB_FAULT;c->error=rc?rc:RSB_FAILED;return RSB_FAILED;}
static int guard(struct rsb_context *c,uint64_t deadline)
{uint64_t now=c->now_ms(c->clock_user);
 if(c->state==RSB_FAULT||atomic_load(&c->cancelled)||now<c->last_now||now>=deadline)return failed(c,RSB_FAILED);
 c->last_now=now;return RSB_OK;}
static int admission(struct rsb_context *c,uint64_t deadline)
{uint64_t now=c->now_ms(c->clock_user);
 if(now<c->last_now||deadline<=now||deadline-now>RSB_MAX_MS)return failed(c,RSB_ARGUMENT);
 c->last_now=now;return guard(c,deadline);}
static int clean_store(struct rsb_context *c)
{struct ros_status s;int rc=ros_get_status(c->store,&s);
 return rc||!s.mounted||s.fault||s.job!=ROS_NONE?failed(c,rc?rc:RSB_STATE):RSB_OK;}
static int root_steps(struct rsb_context *c,uint64_t deadline)
{for(unsigned i=0;i<3;++i){if(guard(c,deadline))return RSB_FAILED;
 int rc=ros_step(c->store);if(guard(c,deadline))return RSB_FAILED;
 if(rc!=(i==2?ROS_READY:ROS_PENDING))return failed(c,rc);}
 return RSB_OK;}
int rsb_init(struct rsb_context *c,struct ros_context *store,void *user,uint64_t (*now)(void*))
{if(!c||!store||!now||c->initialized||overlap(c,sizeof(*c),store,sizeof(*store)))return RSB_ARGUMENT;
 struct ros_status s;if(ros_get_status(store,&s)||!s.mounted||s.fault||s.job!=ROS_NONE)return RSB_STATE;
 memset(c,0,sizeof(*c));atomic_init(&c->gate,0);atomic_init(&c->cancelled,0);c->store=store;c->clock_user=user;
 c->now_ms=now;c->last_now=now(user);c->initialized=RSB_MAGIC;return RSB_OK;}
int rsb_init_deferred_seal(struct rsb_context *c,struct ros_context *store,void *user,uint64_t (*now)(void*))
{int rc=rsb_init(c,store,user,now);if(!rc)c->deferred_seal=1;return rc;}
int rsb_select(struct rsb_context *c,const struct es_binding *b,enum rp_mode mode,uint64_t first)
{int rc=take(c);if(rc)return rc;
 if(!external(c,b,sizeof(*b))||b->segment_sequence||b->plaintext_bytes||(mode!=RP_MANUAL&&mode!=RP_CONTINUOUS))return leave(c,RSB_ARGUMENT);
 if((c->state!=RSB_IDLE&&c->state!=RSB_COMPLETE)||atomic_load(&c->cancelled))return leave(c,RSB_STATE);
 if(clean_store(c))return leave(c,RSB_FAILED);
 c->session=*b;c->mode=(uint32_t)mode;c->first_sample=first;c->job_deadline=0;c->state=RSB_SELECTED;return leave(c,RSB_OK);}
int rsb_cancel(struct rsb_context *c)
{if(!c||c->initialized!=RSB_MAGIC)return RSB_ARGUMENT;atomic_store(&c->cancelled,1);return RSB_OK;}
int rsb_capacity(void *user,const struct es_binding *b,uint32_t *segments,uint64_t deadline)
{struct rsb_context *c=user;int rc=take(c);if(rc)return rc;
 if(!external(c,b,sizeof(*b))||!external(c,segments,sizeof(*segments))||
    overlap(b,sizeof(*b),segments,sizeof(*segments)))return leave(c,RSB_ARGUMENT);
 *segments=0;
 if(c->state!=RSB_SELECTED||!same(b,&c->session)||admission(c,deadline))
  return leave(c,failed(c,RSB_STATE));
 struct ros_status s;struct ros_geometry geometry;
 if(ros_get_geometry(c->store,&geometry)||ros_get_status(c->store,&s)||!s.mounted||s.fault||s.job!=ROS_NONE||
    s.slot_count!=geometry.slots||s.consumed_slots>s.slot_count)
  return leave(c,failed(c,RSB_STATE));
 unsigned free_root=0;
 for(uint32_t i=0;i<geometry.roots;++i){struct ros_record r;
  rc=ros_get_record(c->store,i,&r);
  if(rc==ROS_NOT_FOUND)free_root=1;
  else if(rc||!memcmp(b->recording_id,r.identity.recording_id,16))
   return leave(c,failed(c,RSB_STATE));
 }
 if(guard(c,deadline))return leave(c,RSB_FAILED);
 if(free_root)*segments=s.slot_count-s.consumed_slots;
 return leave(c,RSB_OK);}
int rsb_reserve(void *user,const struct es_binding *b,uint64_t deadline)
{struct rsb_context *c=user;int rc=take(c);if(rc)return RP_IO_UNCERTAIN;
 if(!external(c,b,sizeof(*b))||c->state!=RSB_SELECTED||!same(b,&c->session)||admission(c,deadline))
  {failed(c,RSB_STATE);return leave(c,RP_IO_UNCERTAIN);}
 rc=ros_reserve(c->store,b,(enum rp_mode)c->mode,c->first_sample,deadline);
 if(guard(c,deadline))return leave(c,RP_IO_UNCERTAIN);
 if(rc==ROS_FULL){c->state=RSB_IDLE;return leave(c,RP_IO_FULL);}
 if(rc!=ROS_PENDING||root_steps(c,deadline)||ros_finish(c->store)||guard(c,deadline))
  {failed(c,rc);return leave(c,RP_IO_UNCERTAIN);}
 c->state=RSB_ACTIVE;return leave(c,RP_IO_OK);}
int rsb_submit(void *user,const struct es_binding *b,const uint8_t *plain,size_t n,struct rp_segment_receipt *out,uint64_t deadline)
{struct rsb_context *c=user;int rc=take(c);if(rc)return RP_IO_UNCERTAIN;
 if(!external(c,out,sizeof(*out))||!external(c,b,sizeof(*b))||!external(c,plain,n)||
 overlap(out,sizeof(*out),b,sizeof(*b))||overlap(out,sizeof(*out),plain,n))return leave(c,RP_IO_UNCERTAIN);
 memset(out,0,sizeof(*out));
 if(c->state!=RSB_ACTIVE||admission(c,deadline)){failed(c,RSB_STATE);return leave(c,RP_IO_UNCERTAIN);}
 /* Establish conservative ownership BEFORE the synchronous seal. Cancellation
  * or a late return may leave ciphertext/job state even without a receipt. */
 c->pending=*b;c->pending_valid=1;c->job_deadline=deadline;c->state=RSB_STORING;
 rc=c->deferred_seal?ros_prepare_deferred(c->store,b,plain,n,deadline):ros_prepare(c->store,b,plain,n,deadline);
 if(guard(c,deadline))return leave(c,RP_IO_UNCERTAIN);
 if(rc==ROS_FULL){memset(&c->pending,0,sizeof(c->pending));c->pending_valid=0;c->job_deadline=0;c->state=RSB_ACTIVE;return leave(c,RP_IO_FULL);}
 if(rc!=ROS_PENDING){failed(c,rc);return leave(c,RP_IO_UNCERTAIN);}
 return leave(c,RP_IO_PENDING);}
int rsb_advance(struct rsb_context *c)
{int rc=take(c);if(rc)return rc;if(c->state!=RSB_STORING)return leave(c,RSB_STATE);
 if(guard(c,c->job_deadline))return leave(c,RSB_FAILED);
 rc=ros_step(c->store);
 if(guard(c,c->job_deadline))return leave(c,RSB_FAILED);
 if(rc!=ROS_PENDING&&rc!=ROS_READY)return leave(c,failed(c,rc));
 return leave(c,rc==ROS_READY?RSB_READY:RSB_PENDING);}
int rsb_poll(void *user,const struct es_binding *b,struct rp_segment_receipt *out,uint64_t deadline)
{struct rsb_context *c=user;
 if(!c||c->initialized!=RSB_MAGIC||!external(c,out,sizeof(*out))||!external(c,b,sizeof(*b))||overlap(out,sizeof(*out),b,sizeof(*b)))return RP_IO_UNCERTAIN;
 memset(out,0,sizeof(*out));int rc=take(c);if(rc)return rc==RSB_BUSY?RP_IO_PENDING:RP_IO_UNCERTAIN;
 if(c->state!=RSB_STORING||!same(b,&c->pending)||admission(c,deadline)||guard(c,c->job_deadline))
  {failed(c,RSB_STATE);return leave(c,RP_IO_UNCERTAIN);}
 struct ros_status s;rc=ros_get_status(c->store,&s);
 if(guard(c,deadline)||guard(c,c->job_deadline))return leave(c,RP_IO_UNCERTAIN);
 if(rc||s.fault||s.job!=ROS_SEGMENT){failed(c,rc);return leave(c,RP_IO_UNCERTAIN);}
 if(!s.ready)return leave(c,RP_IO_PENDING);
 struct rp_segment_receipt receipt;rc=ros_take_segment_receipt(c->store,&receipt);
 if(rc||guard(c,deadline)||guard(c,c->job_deadline)){failed(c,rc);return leave(c,RP_IO_UNCERTAIN);}
 *out=receipt;memset(&c->pending,0,sizeof(c->pending));c->pending_valid=0;c->job_deadline=0;c->state=RSB_ACTIVE;return leave(c,RP_IO_OK);}
int rsb_finalize(void *user,const struct rp_completion *f,struct rp_final_receipt *out,uint64_t deadline)
{struct rsb_context *c=user;int rc=take(c);if(rc)return RP_IO_UNCERTAIN;
 if(!external(c,f,sizeof(*f))||!external(c,out,sizeof(*out))||overlap(f,sizeof(*f),out,sizeof(*out)))return leave(c,RP_IO_UNCERTAIN);
 memset(out,0,sizeof(*out));
 if(c->state!=RSB_ACTIVE||!same(&f->session,&c->session)||f->mode!=c->mode||admission(c,deadline))
  {failed(c,RSB_STATE);return leave(c,RP_IO_UNCERTAIN);}
 rc=ros_finalize(c->store,f,deadline);
 if(guard(c,deadline)||rc!=ROS_PENDING||root_steps(c,deadline)){failed(c,rc);return leave(c,RP_IO_UNCERTAIN);}
 struct rp_final_receipt receipt;rc=ros_take_final_receipt(c->store,&receipt);
 if(rc||guard(c,deadline)){failed(c,rc);return leave(c,RP_IO_UNCERTAIN);}
 *out=receipt;c->state=RSB_COMPLETE;return leave(c,RP_IO_OK);}
int rsb_get_status(struct rsb_context *c,struct rsb_status *out)
{int rc=take(c);if(rc)return rc;if(!external(c,out,sizeof(*out)))return leave(c,RSB_ARGUMENT);
 out->state=c->state;out->cancelled=atomic_load(&c->cancelled);out->pending=c->pending_valid;
 out->error=c->error;out->job_deadline=c->job_deadline;return leave(c,RSB_OK);}
