#include "recording_control_worker.h"
#include <string.h>
static int consent(void *u,uint32_t epoch,int created,uint64_t deadline)
{
 struct lcw_owner *o=u;(void)epoch;(void)created;(void)deadline;
 return lrc_stop_requested(o->control,o->ticket);
}
int lcw_start(struct lcw_owner *o,struct recording_control *c,uint32_t ticket,const struct es_binding *s,uint64_t first)
{
 if(!o||!c||!s)return RW_ARGUMENT;
 struct lc_state state;
 int rc=lrc_snapshot(c,ticket,&state);if(rc==LRC_BUSY)return RW_BUSY;
 if(rc||state.phase!=LC_STARTING)return RW_STATE;
 *o=(struct lcw_owner){.control=c,.ticket=ticket,.session=*s};
 o->start_result=rw_start_controlled(s,first,consent,o);return o->start_result;
}
int lcw_step(struct lcw_owner *o)
{
 if(!o||!o->control)return RW_ARGUMENT;
 int stop=lrc_stop_requested(o->control,o->ticket);struct rw_status w;
 int status=rw_get_status(&w);
 if(status==RW_BUSY)return RW_BUSY;
 if(stop<0||status)return RW_ERROR;
 if(stop&&w.epoch&&(w.state==RW_RUNNING||w.state==RW_STOPPING||w.state==RW_DRAINING)){
  int rc=rw_request_stop(w.epoch,RW_USER);
  if(rc==RW_BUSY)return RW_WAIT; /* latch remains; no stale clear */
  if(rc)return RW_ERROR;
 }
 return rw_step();
}
int lcw_publish(struct lcw_owner *o,const struct lcw_observation *v)
{
 if(!o||!o->control||!v||v->usb>1||v->mic_on>1||v->producer_joined>1||v->storage_joined>1||
    v->released>1||v->accepted_frames>LC_MAX_FRAMES)return LRC_ARGUMENT;
 struct rw_status w;struct lc_state s;
 int status=rw_get_status(&w);if(status==RW_BUSY)return LRC_BUSY;if(status)return LRC_FAULT;
 status=lrc_snapshot(o->control,o->ticket,&s);if(status==LRC_BUSY)return LRC_BUSY;if(status)return LRC_FAULT;
 s.flags=(uint16_t)(LC_PROFILE_FLAGS|(v->usb?LC_USB_PRESENT:0)|(v->mic_on?LC_MIC_ON:0)|
   (v->producer_joined?LC_PRODUCER_JOINED:0)|(v->storage_joined?LC_STORAGE_JOINED:0)|(v->released?LC_RELEASED:0));
 s.reason=0;s.epoch=w.epoch;s.accepted_frames=v->accepted_frames;s.committed_frames=0;s.admitted_frames=w.admitted_frames;
 if(w.capacity_known)s.flags|=LC_CAPACITY_KNOWN;
 if(o->start_result==RW_NO_CAPACITY||o->start_result==RW_CANCELLED){
  if(w.state!=RW_IDLE||w.fault||w.lease_held||w.supervisor_armed||w.supervisor_confirmed||v->accepted_frames||v->mic_on||
     !v->producer_joined||!v->storage_joined||!v->released)return LRC_FAULT;
  s.phase=(uint8_t)(o->start_result==RW_NO_CAPACITY?LC_NO_CAPACITY:LC_CANCELLED_BEFORE_START);
  s.reason=(uint8_t)(o->start_result==RW_NO_CAPACITY?RW_FULL:RW_USER);s.epoch=0;memset(s.recording,0,16);
 }else{
  if(w.pipeline.committed_samples%320U||w.pipeline.committed_samples/320U>UINT32_MAX)return LRC_FAULT;
  s.committed_frames=(uint32_t)(w.pipeline.committed_samples/320U);
  if(w.epoch&&w.pipeline.session_reserved){s.flags|=LC_SESSION_CREATED;memcpy(s.recording,o->session.recording_id,16);}
  if(w.state==RW_RUNNING){s.phase=LC_RUNNING;}
  else if(w.state==RW_STOPPING||w.state==RW_DRAINING){
   s.phase=(uint8_t)(w.state==RW_DRAINING?LC_DRAINING:LC_STOPPING);s.reason=(uint8_t)w.reason;
  }else if(w.state==RW_STOPPED){
   if(w.fault||w.lease_held||w.supervisor_armed||w.supervisor_confirmed||!w.producer_fenced||!w.queue_closed||
      !w.queue_summary_valid||w.queue.accepted!=v->accepted_frames||w.queue.discarded||w.worker_discarded_frames||
      w.pipeline.discarded_samples||w.pipeline.commit_uncertain||w.pipeline.pending_segments||
      !w.pipeline.buffers_wiped||!w.pipeline.manifest_finalized||
      w.pipeline.captured_samples!=(uint64_t)v->accepted_frames*320U)return LRC_FAULT;
   s.phase=LC_STOPPED;s.reason=(uint8_t)w.reason;s.flags|=LC_FINALIZED;
  }else{
   s.phase=LC_FAULT;s.reason=(uint8_t)(w.reason?w.reason:RW_CORE);s.flags|=LC_FAULT_LATCH;
  }
 }
 return lrc_publish(o->control,o->ticket,&s);
}
