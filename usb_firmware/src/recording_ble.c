/* SPDX-License-Identifier: Apache-2.0 */
#include "recording_ble.h"
#include <limits.h>
#include <errno.h>
#include <string.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#if defined(CONFIG_SMP) && CONFIG_SMP
#error "Recording BLE broker requires the reviewed single-core IRQ exclusion"
#endif

static struct {
 struct recording_ble_hooks hooks;
 struct bt_conn *conn;
 uint32_t initialized,fault,active,revoked,pending,submitting,completing;
 uint32_t terminal,terminal_ok,delivering,delivery_queued,retire_queued,actors,ever_submitted;
 uint32_t epoch,last_epoch,prior_valid,full_profile;
 uint32_t stream_sent,stream_total,stream_more;
 uint16_t last_sequence;
 uint64_t last_now,expires,deadline;
 struct db_request request;
 uint8_t nonce[16],prior_nonce[16],response[DB_MAX_RESPONSE];
 size_t response_bytes;
} broker;
/* Scheduling hint only. A late callback may outlive its connection/epoch:
 * it never changes broker authority, response bytes, or a storage receipt.
 * Saturation bounds stale hints; the worker must recheck readiness/deadline. */
K_SEM_DEFINE(recording_ble_stream_wake,0,1);
void recording_ble_tx_progress(void)
{k_sem_give(&recording_ble_stream_wake);}
void recording_ble_stream_wait(void)
{(void)k_sem_take(&recording_ble_stream_wake,K_MSEC(1));}
static void wipe(void *p,size_t n){volatile uint8_t *b=p;while(n--)*b++=0;}
static uint16_t le16(const uint8_t *p){return (uint16_t)(p[0]|(uint16_t)p[1]<<8);}
static uint32_t le32(const uint8_t *p){return (uint32_t)le16(p)|(uint32_t)le16(p+2)<<16;}
static uint64_t le64(const uint8_t *p){uint64_t v=0;for(unsigned i=0;i<8;i++)v|=(uint64_t)p[i]<<(8*i);return v;}
/* Called only with IRQ exclusion. The actual kernel clock primitive is ISR
 * safe; arbitrary hooks never run here. No 64-bit atomic library is needed. */
static int clock_locked(uint64_t *out)
{
 int64_t t=k_uptime_get();
 if(t<0||(uint64_t)t<broker.last_now){broker.fault=broker.revoked=1;return 0;}
 broker.last_now=(uint64_t)t;*out=(uint64_t)t;return 1;
}
static void expire_locked(uint64_t t)
{if(broker.active&&t>=broker.expires)broker.revoked=1;}
/* Clean broker state only, not runtime/catalog state. Caller holds the IRQ
 * lock and will unref outside it. Never reset monotone epochs/prior nonce. */
static struct bt_conn *clear_locked(void)
{
 struct bt_conn *conn=broker.conn;
 memcpy(broker.prior_nonce,broker.nonce,16);broker.prior_valid=1;
 broker.conn=NULL;broker.active=broker.revoked=broker.pending=broker.submitting=broker.completing=0;
 broker.terminal=broker.terminal_ok=broker.delivering=broker.delivery_queued=broker.retire_queued=broker.ever_submitted=0;
 broker.last_sequence=0;broker.expires=broker.deadline=0;broker.response_bytes=0;
 broker.stream_sent=broker.stream_total=broker.stream_more=0;
 wipe(broker.nonce,16);wipe(&broker.request,sizeof(broker.request));wipe(broker.response,sizeof(broker.response));
 return conn;
}
static void kick(void)
{
 uint32_t epoch=0;struct bt_conn *drop=NULL;
 unsigned key=irq_lock();
 if(broker.active&&broker.revoked&&!broker.pending&&!broker.actors&&!broker.retire_queued){
  if(!broker.ever_submitted)drop=clear_locked();
  else {broker.retire_queued=1;epoch=broker.epoch;}
 }
 irq_unlock(key);
 if(drop)bt_conn_unref(drop);
 if(epoch){
  int rc=broker.hooks.retire(broker.hooks.user,epoch);
  if(rc){key=irq_lock();broker.fault=1;if(broker.active&&broker.epoch==epoch)broker.revoked=1;irq_unlock(key);}
 }
}
static void leave(void)
{unsigned key=irq_lock();if(!broker.actors)broker.fault=broker.revoked=1;else --broker.actors;irq_unlock(key);kick();}
static int live(uint32_t epoch,struct bt_conn *conn)
{
 unsigned key=irq_lock();uint64_t t=0;int good=clock_locked(&t);expire_locked(t);
 good=good&&!broker.fault&&broker.active&&broker.epoch==epoch&&broker.conn==conn&&!broker.revoked&&t<broker.deadline;
 irq_unlock(key);return good;
}
static int response_valid(const struct db_request *r,const uint8_t *p,size_t n)
{
 if(!p||n<9||n>DB_MAX_RESPONSE||p[0]!='O'||p[1]!='P'||p[2]!=1||p[3]!=(uint8_t)(r->command|0x80U)||
 le16(p+4)!=r->sequence||le16(p+6)!=n-8||p[8]!=0)return 0;
 uint8_t expected[DB_MAX_RESPONSE];int rc;
 if(r->command>=DB_CATALOG&&r->command<=DB_SEGMENT){
  if(n<30)return 0;
  rc=durable_ble_read_response(expected,n,r,le16(p+27),p+29,n-29);
 }else if((r->command>=DB_FULL_CATALOG&&r->command<=DB_FULL_SEGMENT)||r->command==DB_FULL_STREAM){
  if(n<34)return 0;
  rc=durable_ble_read_response(expected,n,r,le32(p+29),p+33,n-33);
 }else if(r->command==DB_RECEIVE_ACK||r->command==DB_FULL_RECEIVE_ACK||r->command==DB_FULL_RECEIVE_RANGE){rc=durable_ble_receipt_response(expected,n,r);}
 else if(r->command==DB_DELETE||r->command==DB_FULL_DELETE){
  if(n!=81)return 0;
  /* Only shape/minimum bound here. The runtime/RSM must independently prove
   * the actual terminal manifest revision; it is not present in this request. */
  rc=durable_ble_delete_response(expected,n,r,1,le64(p+73));
 }else return 0;
 return !rc&&!memcmp(expected,p,n);
}
static void deliver(void)
{
 uint32_t epoch;struct bt_conn *conn;uint8_t frame[DB_MAX_RESPONSE];size_t n;int ok;
 unsigned key=irq_lock();
 if(!broker.pending||broker.submitting||broker.completing||!broker.terminal||broker.delivering){irq_unlock(key);return;}
 broker.delivering=1;epoch=broker.epoch;conn=broker.conn;n=broker.response_bytes;ok=(int)broker.terminal_ok;
 if(ok)memcpy(frame,broker.response,n);
 irq_unlock(key);
 int retry=0;
 if(ok&&live(epoch,conn)){
  ok=broker.hooks.ready(broker.hooks.user)==1&&live(epoch,conn);
  if(ok)ok=broker.hooks.authorized(broker.hooks.user,conn)==1&&live(epoch,conn);
  if(ok){int rc=broker.hooks.notify(broker.hooks.user,conn,frame,n);
   retry=broker.request.command==DB_FULL_STREAM&&(rc==-ENOMEM||rc==-EAGAIN)&&live(epoch,conn);
   ok=rc==0&&live(epoch,conn);
  }
 }else ok=0;
 wipe(frame,sizeof(frame));
 key=irq_lock();
 if(broker.active&&broker.epoch==epoch&&broker.delivering){
  if(retry){broker.delivering=0;irq_unlock(key);return;}
  if(!ok)broker.revoked=1;
  if(broker.stream_more){
   if(ok)broker.stream_sent+=(uint32_t)(n-33U);
   broker.terminal=broker.terminal_ok=broker.delivering=broker.stream_more=0;broker.response_bytes=0;
   wipe(broker.response,sizeof(broker.response));irq_unlock(key);
   recording_ble_tx_progress();return;
  }
  broker.pending=broker.terminal=broker.terminal_ok=broker.delivering=0;broker.response_bytes=0;
  wipe(broker.response,sizeof(broker.response));wipe(&broker.request,sizeof(broker.request));
 }
 irq_unlock(key);
}
static void delivery_work_handler(struct k_work *work)
{
 (void)work;unsigned key=irq_lock();
 if(!broker.delivery_queued){irq_unlock(key);return;}
 broker.delivery_queued=0;
 if(!broker.pending||broker.submitting||broker.completing||!broker.terminal||broker.delivering){
  broker.fault=broker.revoked=1;irq_unlock(key);return;
 }
 broker.actors++;irq_unlock(key);deliver();leave();
}
K_WORK_DEFINE(recording_ble_delivery_work,delivery_work_handler);
static void schedule_delivery(void)
{
 unsigned key=irq_lock();
 if(!broker.pending||broker.submitting||broker.completing||!broker.terminal||broker.delivering||broker.delivery_queued){irq_unlock(key);return;}
 broker.delivery_queued=1;broker.actors++;irq_unlock(key);
 int rc=k_work_submit(&recording_ble_delivery_work);
 if(rc<0){
  key=irq_lock();broker.fault=broker.revoked=1;
  broker.delivery_queued=broker.pending=broker.terminal=broker.terminal_ok=0;broker.response_bytes=0;
  wipe(broker.response,sizeof(broker.response));wipe(&broker.request,sizeof(broker.request));irq_unlock(key);
 }
 leave();
}
int recording_ble_init(const struct recording_ble_hooks *h)
{
 if(!h||!h->ready||!h->authorized||!h->submit||!h->notify||!h->retire)return RB_ARGUMENT;
 unsigned key=irq_lock();if(broker.initialized){irq_unlock(key);return RB_REFUSED;}
 broker.hooks=*h;broker.initialized=1;irq_unlock(key);return RB_OK;
}
int recording_ble_command(struct bt_conn *conn,const uint8_t *frame,size_t n)
{
 struct db_request request;
 if(!conn||durable_ble_parse_request(frame,n,&request))return RB_ARGUMENT;
 struct bt_conn *hold=bt_conn_ref(conn);if(!hold)return RB_REFUSED;
 unsigned key=irq_lock();uint64_t t=0;int rc=RB_REFUSED;uint32_t epoch=0;
 if(!broker.initialized||broker.fault||!clock_locked(&t))goto refused;
 expire_locked(t);
 if(broker.active&&(broker.revoked||broker.pending||broker.actors||broker.retire_queued)){rc=RB_BUSY;goto refused;}
 if(!broker.active){
  if((request.command!=DB_CATALOG&&request.command!=DB_FULL_CATALOG)||request.offset||broker.last_epoch==UINT32_MAX||
   (broker.prior_valid&&!memcmp(request.nonce,broker.prior_nonce,16))||t>UINT64_MAX-RECORDING_BLE_SESSION_MS)goto refused;
  broker.active=1;broker.conn=hold;hold=NULL;broker.epoch=++broker.last_epoch;
  broker.full_profile=(uint32_t)durable_ble_is_full(request.command);
  broker.expires=t+RECORDING_BLE_SESSION_MS;memcpy(broker.nonce,request.nonce,16);
 }else if(broker.conn!=conn||memcmp(request.nonce,broker.nonce,16)||request.sequence<=broker.last_sequence||
  broker.full_profile!=(uint32_t)durable_ble_is_full(request.command))goto refused;
 epoch=broker.epoch;broker.actors++;
 uint64_t budget=broker.full_profile&&!broker.ever_submitted?RECORDING_BLE_FULL_OPEN_MS:RECORDING_BLE_REQUEST_MS;
 broker.deadline=broker.expires-t>budget?t+budget:broker.expires;
 uint64_t deadline=broker.deadline;
 irq_unlock(key);
 int permitted=broker.hooks.ready(broker.hooks.user)==1&&live(epoch,conn);
 if(permitted)permitted=broker.hooks.authorized(broker.hooks.user,conn)==1&&live(epoch,conn);
 key=irq_lock();
 if(!permitted||!clock_locked(&t)||broker.fault||!broker.active||broker.epoch!=epoch||broker.revoked||t>=deadline){broker.revoked=1;irq_unlock(key);rc=RB_REFUSED;goto done;}
 broker.request=request;broker.pending=broker.submitting=1;broker.last_sequence=request.sequence;
 broker.stream_sent=broker.stream_total=broker.stream_more=0;
 irq_unlock(key);
 rc=broker.hooks.submit(broker.hooks.user,epoch,&request,deadline);
 key=irq_lock();broker.submitting=0;
 if(rc){
  /* A nonzero enqueue result promises no worker admission. Contradictory
   * completion is a provider contract fault, never a success response. */
  if(broker.terminal||broker.completing){broker.fault=1;broker.ever_submitted=1;}
  broker.revoked=1;broker.pending=broker.terminal=broker.terminal_ok=0;wipe(broker.response,sizeof(broker.response));
 }else broker.ever_submitted=1;
 irq_unlock(key);
 if(!rc)schedule_delivery();
 rc=rc?RB_REFUSED:RB_OK;
done:if(hold)bt_conn_unref(hold);leave();return rc;
refused:irq_unlock(key);bt_conn_unref(hold);kick();return rc;
}
void recording_ble_disconnected(struct bt_conn *conn)
{unsigned key=irq_lock();if(broker.active&&broker.conn==conn)broker.revoked=1;irq_unlock(key);kick();}
int recording_ble_admitted(uint32_t epoch,const struct db_request *r,uint64_t deadline)
{
 if(!r)return 0;
 unsigned key=irq_lock();uint64_t t=0;int good=clock_locked(&t);
 good=good&&broker.initialized&&!broker.fault&&broker.active&&broker.epoch==epoch&&broker.pending&&!broker.completing&&(!broker.terminal||broker.stream_more)&&
  deadline==broker.deadline&&t<deadline&&!memcmp(r,&broker.request,sizeof(*r));
 irq_unlock(key);return good;
}
static int complete(uint32_t epoch,uint16_t sequence,const uint8_t *p,size_t n,int success,int more)
{
 unsigned key=irq_lock();
 if(!broker.initialized||!broker.active||broker.epoch!=epoch||!broker.pending||broker.completing||broker.terminal||
 broker.request.sequence!=sequence){irq_unlock(key);return RB_REFUSED;}
 broker.completing=1;broker.actors++;struct db_request r=broker.request;irq_unlock(key);
 uint32_t sent=broker.stream_sent,total=broker.stream_total;
 if(r.command==DB_FULL_STREAM){r.offset+=sent;r.maximum=(uint16_t)(r.maximum-sent);}
 int valid=success&&response_valid(&r,p,n);
 if(valid&&r.command==DB_FULL_STREAM){
  uint32_t incoming=le32(p+29),left=incoming-r.offset;
  if(left>r.maximum)left=r.maximum;
  valid=(!total||total==incoming)&&((n-33U<left)==(more!=0));
 }else if(more)valid=0;
 key=irq_lock();
 if(success&&!valid)broker.fault=broker.revoked=1;
 if(valid){memcpy(broker.response,p,n);broker.response_bytes=n;
  if(r.command==DB_FULL_STREAM)broker.stream_total=le32(p+29);
 }
 broker.stream_more=(uint32_t)(more!=0);
 broker.terminal=1;broker.terminal_ok=(uint32_t)valid;broker.completing=0;
 irq_unlock(key);schedule_delivery();leave();return success&&!valid?RB_FAULT:RB_OK;
}
int recording_ble_complete(uint32_t epoch,uint16_t sequence,const uint8_t *p,size_t n)
{return complete(epoch,sequence,p,n,1,0);}
int recording_ble_failed(uint32_t epoch,uint16_t sequence)
{return complete(epoch,sequence,NULL,0,0,0);}
int recording_ble_stream_frame(uint32_t epoch,uint16_t sequence,const uint8_t *p,size_t n)
{return complete(epoch,sequence,p,n,1,1);}
int recording_ble_stream_ready(uint32_t epoch,uint16_t sequence)
{
 unsigned key=irq_lock();uint64_t t=0;int valid=clock_locked(&t);expire_locked(t);
 valid=valid&&broker.active&&broker.pending&&broker.epoch==epoch&&broker.request.sequence==sequence&&broker.request.command==DB_FULL_STREAM;
 int staged=valid&&(broker.terminal||broker.completing||broker.delivering);
 int rc=staged?0:valid&&!broker.fault&&!broker.revoked&&t<broker.deadline?1:-1;
 irq_unlock(key);if(staged)schedule_delivery();return rc;
}
int recording_ble_retired(uint32_t epoch,int result)
{
 struct bt_conn *drop=NULL;unsigned key=irq_lock();
 if(!broker.active||broker.epoch!=epoch||!broker.retire_queued||broker.pending||broker.actors){irq_unlock(key);return RB_REFUSED;}
 if(result){broker.fault=1;irq_unlock(key);return RB_FAULT;}
 drop=clear_locked();irq_unlock(key);bt_conn_unref(drop);return RB_OK;
}
int recording_ble_reply_allowed(struct bt_conn *conn,uint16_t sequence)
{
 unsigned key=irq_lock();uint64_t t=0;int good=clock_locked(&t);expire_locked(t);
 good=good&&broker.initialized&&!broker.fault&&broker.active&&broker.conn==conn&&!broker.revoked&&
  broker.pending&&broker.delivering&&broker.terminal_ok&&broker.request.sequence==sequence&&t<broker.deadline;
 irq_unlock(key);return good;
}
void recording_ble_poll(void)
{unsigned key=irq_lock();int initialized=broker.initialized;irq_unlock(key);
 /* Revoke an idle session when its cached power/ownership admission is lost.
  * Do not cancel a NAND call: admitted work joins before existing retirement. */
 int permitted=!initialized||broker.hooks.ready(broker.hooks.user)==1;
 key=irq_lock();uint64_t t=0;if(broker.initialized&&clock_locked(&t))expire_locked(t);
 if(broker.active&&!permitted)broker.revoked=1;
 int stream=broker.request.command==DB_FULL_STREAM;irq_unlock(key);if(stream)schedule_delivery();kick();}
int recording_ble_available(void)
{
 unsigned key=irq_lock();int good=broker.initialized&&!broker.fault;irq_unlock(key);
 if(!good)return 0;
 good=broker.hooks.ready(broker.hooks.user)==1;
 key=irq_lock();good=good&&!broker.fault;irq_unlock(key);return good;
}
