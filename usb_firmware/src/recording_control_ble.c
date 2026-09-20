/* SPDX-License-Identifier: Apache-2.0 */
#include "recording_control_ble.h"
#include "recording_long_runtime.h"
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#if defined(CONFIG_SMP) && CONFIG_SMP
#error "Long-control broker requires single-core IRQ exclusion"
#endif
/* Reference is held across callbacks/work. Revoke is immediate; release waits
 * for the one worker and coordinator disconnect to join. No microphone/storage
 * work on the BT thread. A disconnect never synthesizes STOP. */
static struct {
 struct recording_control_ble_hooks hooks;
 struct bt_conn *conn;
 uint64_t epoch,next_epoch,last_now,deadline;
 unsigned initialized,fault,revoked,pending,executed,queued,working;
 uint8_t request[LC_REQUEST_BYTES];
} control_ble;
static int tick_locked(uint64_t *t)
{
 int64_t n=k_uptime_get();
 if(n<0||(uint64_t)n<control_ble.last_now){control_ble.fault=control_ble.revoked=1;return 0;}
 *t=control_ble.last_now=(uint64_t)n;return 1;
}
int recording_control_ble_authorized(void *u,uint64_t epoch)
{
 (void)u;unsigned key=irq_lock();uint64_t t=0;
 struct bt_conn *conn=control_ble.conn;
 int ok=tick_locked(&t)&&conn&&epoch==control_ble.epoch&&!control_ble.revoked&&!control_ble.fault;
 if(ok&&control_ble.pending&&t>=control_ble.deadline)ok=0;
 /* This is called only while the broker worker retains the reference. */
 ok=ok&&control_ble.working;
 irq_unlock(key);
 if(!ok||control_ble.hooks.authorized(control_ble.hooks.user,conn)!=1)return 0;
 key=irq_lock();ok=control_ble.conn==conn&&control_ble.epoch==epoch&&!control_ble.revoked&&!control_ble.fault;
 irq_unlock(key);return ok;
}
int recording_control_ble_reply_allowed(struct bt_conn *conn,uint16_t sequence)
{
 unsigned key=irq_lock();uint64_t t=0;
 int ok=tick_locked(&t)&&control_ble.working&&control_ble.pending&&control_ble.executed&&
  control_ble.conn==conn&&!control_ble.revoked&&!control_ble.fault&&t<control_ble.deadline&&
  sequence==(uint16_t)(control_ble.request[4]|(uint16_t)control_ble.request[5]<<8);
 uint64_t epoch=control_ble.epoch;irq_unlock(key);
 return ok&&recording_runtime_long_reply_allowed(epoch,sequence);
}
static void control_work(struct k_work *work)
{
 (void)work;unsigned key=irq_lock();uint64_t t=0;
 control_ble.queued=0;
 if(control_ble.working||!control_ble.conn){irq_unlock(key);return;}
 control_ble.working=1;
 if(!tick_locked(&t)||(control_ble.pending&&t>=control_ble.deadline))control_ble.revoked=1;
 struct bt_conn *conn=control_ble.conn;uint64_t epoch=control_ble.epoch;
 int revoked=control_ble.revoked,first=!control_ble.executed;
 int pending=control_ble.pending;irq_unlock(key);
 if(!revoked&&pending){
  uint8_t response[LC_RESPONSE_BYTES];int rc;
  if(first){
   /* Retry admission only when the core proves NOTHING was admitted. Once
    * admitted (including BUSY after queue_start), later passes only read the
    * accepted operation. The copied request and original deadline never change. */
   rc=recording_runtime_long_command(epoch,control_ble.request,LC_REQUEST_BYTES,response);
   key=irq_lock();control_ble.executed=rc!=LRC_DEFERRED;irq_unlock(key);
  }else rc=recording_runtime_long_pending(epoch,control_ble.request,LC_REQUEST_BYTES,response);
  if(rc==LRC_OK){
   uint16_t seq=(uint16_t)(response[4]|(uint16_t)response[5]<<8);
   /* A false check may be a short metadata-gate collision, not lost authority.
    * Do not enqueue anything; recheck within the SAME deadline on a later pass. */
   int sent=-EAGAIN;
   if(recording_control_ble_reply_allowed(conn,seq))sent=control_ble.hooks.notify(control_ble.hooks.user,conn,response,sizeof(response));
   key=irq_lock();
   if(!sent){control_ble.pending=0;memset(control_ble.request,0,sizeof(control_ble.request));}
   else if(sent!=-ENOMEM&&sent!=-EAGAIN)control_ble.revoked=1;
   irq_unlock(key);
  }else if(rc!=LRC_BUSY&&rc!=LRC_PENDING&&rc!=LRC_DEFERRED){key=irq_lock();control_ble.revoked=1;irq_unlock(key);}
  memset(response,0,sizeof(response));
 }
 key=irq_lock();revoked=control_ble.revoked;irq_unlock(key);
 if(revoked){
  int rc=recording_runtime_long_disconnected(epoch);
  key=irq_lock();
  if(rc!=LRC_BUSY){
   control_ble.conn=NULL;control_ble.pending=control_ble.executed=control_ble.revoked=0;
   memset(control_ble.request,0,sizeof(control_ble.request));
  }
  irq_unlock(key);
  if(rc!=LRC_BUSY)bt_conn_unref(conn);
 }
 key=irq_lock();control_ble.working=0;irq_unlock(key);
}
K_WORK_DEFINE(recording_control_work,control_work);
void recording_control_ble_poll(void)
{
 unsigned key=irq_lock();
 if(!control_ble.initialized||!control_ble.conn||control_ble.queued||control_ble.working||
    (!control_ble.pending&&!control_ble.revoked)){irq_unlock(key);return;}
 control_ble.queued=1;irq_unlock(key);
 if(k_work_submit(&recording_control_work)<0){
  key=irq_lock();control_ble.queued=0;control_ble.fault=control_ble.revoked=1;irq_unlock(key);
 }
}
int recording_control_ble_init(const struct recording_control_ble_hooks *h)
{
 if(!h||!h->authorized||!h->notify)return -EINVAL;
 unsigned key=irq_lock();if(control_ble.initialized){irq_unlock(key);return -EALREADY;}
 control_ble.hooks=*h;control_ble.initialized=1;irq_unlock(key);return 0;
}
int recording_control_ble_command(struct bt_conn *conn,const uint8_t *p,size_t n)
{
 if(!conn||!p||n!=LC_REQUEST_BYTES||!control_ble.initialized||!recording_runtime_long_available())return -EINVAL;
 struct bt_conn *hold=bt_conn_ref(conn);if(!hold)return -EPERM;
 if(control_ble.hooks.authorized(control_ble.hooks.user,conn)!=1){bt_conn_unref(hold);return -EPERM;}
 unsigned key=irq_lock();uint64_t t=0;int rc=-EBUSY;
 if(!control_ble.initialized||control_ble.fault||!tick_locked(&t)||control_ble.revoked||
    control_ble.pending||control_ble.queued||control_ble.working||t>UINT64_MAX-4000U)goto done;
 if(control_ble.conn&&control_ble.conn!=conn)goto done;
 if(!control_ble.conn){
  if(control_ble.next_epoch==UINT64_MAX)goto done;
  control_ble.conn=hold;hold=NULL;control_ble.epoch=++control_ble.next_epoch;
 }
 memcpy(control_ble.request,p,n);control_ble.deadline=t+4000U;
 control_ble.pending=1;control_ble.executed=0;rc=0;
done:irq_unlock(key);if(hold)bt_conn_unref(hold);
 if(!rc)recording_control_ble_poll();
 return rc;
}
void recording_control_ble_disconnected(struct bt_conn *conn)
{
 unsigned key=irq_lock();if(control_ble.conn==conn)control_ble.revoked=1;irq_unlock(key);
 recording_control_ble_poll();
}
