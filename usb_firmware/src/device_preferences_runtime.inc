/* Main actor owns persistence; Bluetooth callbacks only copy bounded requests.
 * Independent NVS key: never modifies enrollment, bonds, keys or NAND. */
#include <zephyr/settings/settings.h>
#include <zephyr/sys/crc.h>
static const char dp_key[]="opdevice/preferences1";
static struct k_spinlock dp_lock;
static uint8_t dp_applied[DP_BYTES];
static struct {
 struct bt_conn *conn; uint64_t deadline; uint16_t sequence;
 uint8_t bytes[DP_BYTES];
} dp_pending;
static uint64_t dp_idle_since;
static uint16_t dp_adv_units=48;
K_MUTEX_DEFINE(dp_adv_lock);

static void dp_copy(uint8_t out[DP_BYTES])
{
 k_spinlock_key_t key=k_spin_lock(&dp_lock);
 if(atomic_get(&dp_ready))memcpy(out,dp_applied,DP_BYTES);else dp_defaults(out);
 k_spin_unlock(&dp_lock,key);
}
struct dp_load {uint8_t *out;int seen,error;};
static int dp_loaded(const char *key,size_t n,settings_read_cb read_cb,void *arg,void *user)
{
 struct dp_load *l=user;
 if(l->error)return l->error;
 if((key&&*key)||l->seen||n!=DP_BYTES+4||!read_cb)return l->error=-EINVAL;
 l->seen=1;
 if(read_cb(arg,l->out,n)!=(ssize_t)n)return l->error=-EIO;
 if(!dp_valid(l->out,DP_BYTES)||sys_get_le32(l->out+DP_BYTES)!=crc32_ieee(l->out,DP_BYTES))return l->error=-EINVAL;
 return 0;
}
static int dp_read_saved(uint8_t out[DP_BYTES+4])
{
 struct dp_load l={out,0,0};int rc=settings_load_subtree_direct(dp_key,dp_loaded,&l);
 /* The pinned backend can swallow callback errors. Check retained error. */
 return rc?rc:l.error?l.error:l.seen?0:1;
}
static void dp_initialize(void)
{
 uint8_t saved[DP_BYTES+4];dp_defaults(saved);
 int rc=dp_read_saved(saved);
 k_spinlock_key_t key=k_spin_lock(&dp_lock);
 if(rc<0){dp_defaults(dp_applied);atomic_set(&dp_failed,1);}
 else memcpy(dp_applied,saved,DP_BYTES);
 atomic_set(&dp_ready,1);k_spin_unlock(&dp_lock,key);
 dp_idle_since=(uint64_t)k_uptime_get();
}
static int dp_queue(struct bt_conn *conn,uint16_t seq,const uint8_t *bytes,size_t n)
{
 if(!atomic_get(&dp_ready)||atomic_get(&dp_failed)||!dp_valid(bytes,n))return -EINVAL;
 k_spinlock_key_t key=k_spin_lock(&dp_lock);
 int rc=-EBUSY;
 if(!dp_pending.conn&&dp_revision(bytes)==dp_revision(dp_applied)){
  dp_pending.conn=bt_conn_ref(conn);dp_pending.sequence=seq;
  dp_pending.deadline=(uint64_t)k_uptime_get()+3000;
  memcpy(dp_pending.bytes,bytes,DP_BYTES);rc=0;
 }
 k_spin_unlock(&dp_lock,key);return rc;
}
static void dp_process(void)
{
 /* Leave the slot occupied until completion so no second write can queue. */
 k_spinlock_key_t key=k_spin_lock(&dp_lock);
 struct bt_conn *conn=dp_pending.conn;
 uint64_t deadline=dp_pending.deadline;uint16_t seq=dp_pending.sequence;
 uint8_t request[DP_BYTES];memcpy(request,dp_pending.bytes,DP_BYTES);
 k_spin_unlock(&dp_lock,key);
 if(!conn)return;
 uint8_t current[DP_BYTES],next[DP_BYTES+4],check[DP_BYTES+4];dp_copy(current);
 int claimed=0,rc=-EPERM;
 if(!atomic_get(&dp_failed)&&pendant_ble_authorized(conn)&&
    bt_gatt_is_subscribed(conn,&pendant_service.attrs[4],BT_GATT_CCC_NOTIFY)&&
    (uint64_t)k_uptime_get()<deadline&&!dp_next(current,request,next)){
  rc=recording_runtime_preferences_claim();claimed=rc==0;
  if(claimed){
   rc=-EPERM;
   if(recording_runtime_preferences_ready()&&pendant_ble_authorized(conn)&&(uint64_t)k_uptime_get()<deadline){
    /* Finish the admitted atomic NVS save/readback even if USB disappears.
     * Do not cancel mid-write, retry an ambiguous result, or touch NAND. */
    sys_put_le32(crc32_ieee(next,DP_BYTES),next+DP_BYTES);
    rc=settings_save_one(dp_key,next,sizeof(next));
    if(!rc)rc=dp_read_saved(check);
    if(!rc&&memcmp(check,next,sizeof(next)))rc=-EIO;
    if(rc){atomic_set(&dp_failed,1);} /* Ambiguous write: no retry this boot. */
    else {
     key=k_spin_lock(&dp_lock);memcpy(dp_applied,next,DP_BYTES);k_spin_unlock(&dp_lock,key);
     dp_idle_since=(uint64_t)k_uptime_get();
    }
   }
  }
 }
 if(claimed)recording_runtime_preferences_release();
 if((uint64_t)k_uptime_get()<deadline)
  send_response(conn,DP_SET,seq,rc?OP_STATUS_HARDWARE_ERROR:OP_STATUS_OK,rc?NULL:next,rc?0:DP_BYTES);
 key=k_spin_lock(&dp_lock);memset(&dp_pending,0,sizeof(dp_pending));k_spin_unlock(&dp_lock,key);
 bt_conn_unref(conn);
}
static int dp_indicator(bool enabled)
{
 uint8_t p[DP_BYTES],rgb[3];dp_copy(p);
 atomic_set(&dp_recording,enabled?1:0);
 dp_color(p,enabled?DP_RECORDING:DP_OFF,(uint64_t)k_uptime_get(),rgb);
 return set_rgb_guarded(rgb[0],rgb[1],rgb[2],true);
}
static void dp_policy_tick(void)
{
 uint64_t now=(uint64_t)k_uptime_get();uint8_t p[DP_BYTES],rgb[3];dp_copy(p);
#ifdef OPENPENDANT_STANDALONE_CONTROL
 dp_standby_requested=false;
#endif
 unsigned int pairing_ms=pendant_ble_pairing_remaining_ms();
 if(pairing_ms&&!atomic_get(&dp_recording)&&!mic_commands_busy()&&!pendant_recovery_is_pending()){
  /* Explicit pairing indication cannot be disabled by cosmetic preferences.
   * Temporary blue blink; never rewrite stored colors or hide active audio. */
  uint8_t blue=((60000U-pairing_ms)%500U)<250U?32U:0U;
  dp_idle_since=now;atomic_set(&dp_manual_until,0);
  if(k_mutex_lock(&rgb_lock,K_NO_WAIT)==0){
   bool changed=!rgb_known||rgb_applied[0]||rgb_applied[1]||rgb_applied[2]!=blue;
   k_mutex_unlock(&rgb_lock);if(changed)(void)set_rgb(0,0,blue);
  }
  return;
 }
 if(atomic_get(&dp_recording)||mic_commands_busy()||pendant_recovery_is_pending()||pendant_ble_pairing_busy()){
  dp_idle_since=now;return;
 }
 if(atomic_get(&dp_connected)||pendant_button_read()!=0)dp_idle_since=now;
 uint32_t manual_until=(uint32_t)atomic_get(&dp_manual_until);
 if(!manual_until||(int32_t)((uint32_t)now-manual_until)>=0){
  if(manual_until)(void)atomic_cas(&dp_manual_until,(int)manual_until,0);
  enum dp_event event=DP_OFF;
  if(atomic_get(&dp_failed)||recording_runtime_faulted())event=DP_FAULT;
#ifdef OPENPENDANT_PORTABLE_RECORDING
  else {struct battery_telemetry b;recording_runtime_battery_telemetry(&b);
   if((b.flags&1)&&b.soc<=p[8])event=DP_LOW;}
#endif
  if(event==DP_OFF&&atomic_get(&dp_connected))event=DP_CONNECTED;
  if(event==DP_OFF&&atomic_get(&dp_usb))event=DP_USB;
  dp_color(p,event,now,rgb);
  if(k_mutex_lock(&rgb_lock,K_NO_WAIT)==0){
   bool changed=!rgb_known||memcmp(rgb_applied,rgb,3);k_mutex_unlock(&rgb_lock);
   if(changed)(void)set_rgb(rgb[0],rgb[1],rgb[2]);
  }
 }
 bool idle=dp_idle_ms(p)&&now-dp_idle_since>=dp_idle_ms(p);
#ifdef OPENPENDANT_STANDALONE_CONTROL
 /* Keep radio discovery and the fresh battery watch. This is System-ON
  * standby, not power-off. Runtime independently checks all live owners. */
 dp_standby_requested=idle&&p[1]!=0&&!atomic_get(&dp_usb)&&!atomic_get(&dp_connected)&&
  !atomic_get(&dp_failed)&&!recording_runtime_faulted()&&!atomic_get(&dp_manual_until);
#endif
 uint16_t units=dp_advertising_units(p,idle);
 if(units==dp_adv_units||atomic_get(&dp_connected)||atomic_get(&bluetooth_result)||
    k_mutex_lock(&dp_adv_lock,K_NO_WAIT))return;
 /* Only disconnected advertisements change. Never disconnect a live peer. */
 if(!atomic_get(&dp_connected)&&!pendant_recovery_is_pending()){
  int rc=bt_le_adv_stop();
  if(!rc){dp_adv_units=units;struct bt_le_adv_param param=*BT_LE_ADV_CONN_FAST_1;
   param.interval_min=units;param.interval_max=units==48?96:units+16;
   rc=bt_le_adv_start(&param,advertising_data,ARRAY_SIZE(advertising_data),scan_response_data,ARRAY_SIZE(scan_response_data));
   atomic_set(&bluetooth_result,rc== -EALREADY?0:rc);
  }
 }
 k_mutex_unlock(&dp_adv_lock);
}
