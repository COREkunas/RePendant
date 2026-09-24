/* Actual production include with deterministic settings/radio/kernel seams.
 * This proves logic, not thread scheduling, flash timings or real radio behavior. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include "device_preferences.h"
#define OPENPENDANT_STANDALONE_CONTROL 1
static bool dp_standby_requested;
typedef intptr_t ssize_t;
typedef int atomic_t;
typedef int k_spinlock_key_t;
struct k_spinlock {int unused;};
#define K_NO_WAIT 0
#define K_MUTEX_DEFINE(name) static int name
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define OP_STATUS_OK 0
#define OP_STATUS_HARDWARE_ERROR 5
#define BT_GATT_CCC_NOTIFY 1
static int atomic_get(const atomic_t *v){return *v;}
static void atomic_set(atomic_t *v,int n){*v=n;}
static int atomic_cas(atomic_t *v,int before,int after){if(*v!=before)return 0;*v=after;return 1;}
static k_spinlock_key_t k_spin_lock(struct k_spinlock *s){(void)s;return 0;}
static void k_spin_unlock(struct k_spinlock *s,int key){(void)s;(void)key;}
static int k_mutex_lock(int *m,int t){(void)t;if(*m)return -1;*m=1;return 0;}
static void k_mutex_unlock(int *m){*m=0;}
static uint64_t clock_ms;
static int64_t k_uptime_get(void){return (int64_t)clock_ms;}
static uint32_t sys_get_le32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void sys_put_le32(uint32_t v,uint8_t *p){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
static uint32_t crc32_ieee(const uint8_t *p,size_t n){uint32_t c=UINT32_MAX;while(n--){c^=*p++;for(unsigned i=0;i<8;i++)c=(c>>1)^((c&1)?0xedb88320U:0);}return ~c;}
typedef ssize_t (*settings_read_cb)(void*,void*,size_t);
static uint8_t persisted[20];static int present,write_error,read_error,duplicate,short_read,writes,claims,held,admit=1,power_ready=1;
static ssize_t read_bytes(void *u,void *out,size_t n){(void)u;memcpy(out,persisted,n);return (ssize_t)n-(short_read?1:0);}
static int settings_load_subtree_direct(const char *key,int (*cb)(const char*,size_t,settings_read_cb,void*,void*),void *u)
{(void)key;if(read_error)return -EIO;if(present){cb(NULL,20,read_bytes,NULL,u);if(duplicate)cb(NULL,20,read_bytes,NULL,u);}return 0;}
static int settings_save_one(const char *key,const void *p,size_t n){(void)key;++writes;if(write_error)return -EIO;if(n!=20)abort();memcpy(persisted,p,n);present=1;return 0;}
struct bt_conn {int refs,authorized,subscribed;};
static struct {int attrs[5];} pendant_service;
static struct bt_conn *bt_conn_ref(struct bt_conn *c){++c->refs;return c;}
static void bt_conn_unref(struct bt_conn *c){--c->refs;}
static int pendant_ble_authorized(struct bt_conn *c){return c->authorized;}
static int bt_gatt_is_subscribed(struct bt_conn *c,void *a,int f){(void)a;(void)f;return c->subscribed;}
static int replies,last_status;static uint8_t last_body[16];
static void send_response(struct bt_conn *c,uint8_t cmd,uint16_t seq,uint8_t status,const uint8_t *p,uint16_t n)
{(void)c;(void)cmd;(void)seq;++replies;last_status=status;if(n)memcpy(last_body,p,n);}
static int recording_runtime_preferences_claim(void){++claims;if(!admit)return -EBUSY;held=1;return 0;}
static int recording_runtime_preferences_ready(void){return held&&power_ready;}
static void recording_runtime_preferences_release(void){held=0;}
static int busy,recovery,pairing,pressed,runtime_fault;
static unsigned fake_pairing_ms;
static unsigned int pendant_ble_pairing_remaining_ms(void){return fake_pairing_ms;}
static int mic_commands_busy(void){return busy;}
static int pendant_recovery_is_pending(void){return recovery;}
static int pendant_ble_pairing_busy(void){return pairing;}
static int pendant_button_read(void){return pressed;}
static int recording_runtime_faulted(void){return runtime_fault;}
static int charging;
static int recording_runtime_battery_charging(void){return charging;}
static int rgb_lock;static bool rgb_known;static uint8_t rgb_applied[3];static int rgb_writes;
static int set_rgb_guarded(uint8_t r,uint8_t g,uint8_t b,bool diagnostic){(void)diagnostic;rgb_applied[0]=r;rgb_applied[1]=g;rgb_applied[2]=b;rgb_known=true;++rgb_writes;return 0;}
static int set_rgb(uint8_t r,uint8_t g,uint8_t b){return set_rgb_guarded(r,g,b,false);}
struct bt_le_adv_param {uint16_t interval_min,interval_max;};
static const struct bt_le_adv_param fast={48,96};
#define BT_LE_ADV_CONN_FAST_1 (&fast)
static const int advertising_data[]={1},scan_response_data[]={2};
static int adv_stops,adv_starts;static uint16_t adv_min;
static int bt_le_adv_stop(void){++adv_stops;return 0;}
static int bt_le_adv_start(const struct bt_le_adv_param *p,const int *a,size_t n,const int *s,size_t m)
{(void)a;(void)n;(void)s;(void)m;++adv_starts;adv_min=p->interval_min;return 0;}
static atomic_t dp_ready,dp_failed,dp_recording,dp_connected,dp_usb,dp_manual_until,bluetooth_result;
#include "device_preferences_runtime.inc"
static unsigned checks;
#define CHECK(x) do{++checks;if(!(x))abort();}while(0)
static void reset(void)
{
 memset(&dp_pending,0,sizeof(dp_pending));dp_ready=dp_failed=dp_recording=dp_connected=dp_manual_until=bluetooth_result=0;
 dp_usb=1;dp_adv_units=48;dp_adv_lock=0;clock_ms=100;dp_idle_since=0;
 memset(persisted,0,20);present=write_error=read_error=duplicate=short_read=writes=claims=held=0;admit=power_ready=1;
 busy=recovery=pairing=pressed=runtime_fault=rgb_lock=rgb_writes=adv_starts=adv_stops=0;rgb_known=false;
 fake_pairing_ms=0;charging=0;dp_blink_count=0;dp_blink_began=0;
 replies=last_status=0;dp_initialize();
}
__declspec(dllexport) unsigned preferences_persistence_tests(void)
{
 checks=0;reset();pairing=1;fake_pairing_ms=60000;dp_manual_until=30000;dp_policy_tick();
 CHECK(rgb_applied[0]==0&&rgb_applied[1]==0&&rgb_applied[2]==32&&!dp_manual_until&&!writes&&!dp_standby_requested);
 int old_rgb=rgb_writes;dp_policy_tick();CHECK(rgb_writes==old_rgb);
 fake_pairing_ms=59750;dp_policy_tick();CHECK(rgb_applied[2]==0);
 fake_pairing_ms=59500;dp_policy_tick();CHECK(rgb_applied[2]==32);
 fake_pairing_ms=0;pairing=0;dp_policy_tick();CHECK(rgb_applied[2]!=32&&!writes);
 reset();uint8_t p[16];dp_copy(p);CHECK(dp_ready&&!dp_failed&&!writes&&dp_valid(p,16));
 struct bt_conn c={0,1,1};p[1]=2;
 CHECK(!dp_queue(&c,7,p,16)&&c.refs==1&&!writes);
 CHECK(dp_queue(&c,8,p,16)==-EBUSY);dp_process();
 CHECK(writes==1&&claims==1&&!held&&replies==1&&!last_status&&!c.refs&&dp_revision(last_body)==1);
 dp_copy(p);CHECK(p[1]==2&&dp_revision(p)==1);dp_ready=0;dp_initialize();dp_copy(p);CHECK(p[1]==2&&dp_revision(p)==1);
 p[12]=0;CHECK(dp_queue(&c,9,p,16)==-EBUSY&&writes==1);
 for(unsigned mode=0;mode<4;mode++){reset();dp_copy(p);CHECK(!dp_queue(&c,1,p,16));
  if(mode==0)c.authorized=0;if(mode==1)c.subscribed=0;if(mode==2)clock_ms+=3000;if(mode==3)admit=0;
  dp_process();CHECK(!writes&&!held&&!c.refs);c.authorized=c.subscribed=1;
 }
 reset();dp_copy(p);CHECK(!dp_queue(&c,1,p,16));write_error=1;dp_process();CHECK(writes==1&&dp_failed&&last_status==5&&!held&&!c.refs);
 CHECK(dp_queue(&c,2,p,16)<0&&writes==1);
 /* Unplugged success and power lost after admission but before NVS. */
 reset();dp_usb=0;dp_copy(p);p[1]=1;CHECK(!dp_queue(&c,1,p,16));dp_process();
 CHECK(writes==1&&!dp_failed&&!held&&!c.refs&&!last_status);
 reset();dp_copy(p);CHECK(!dp_queue(&c,1,p,16));power_ready=0;dp_process();
 CHECK(!writes&&!dp_failed&&!held&&!c.refs&&last_status==5);
 for(unsigned mode=0;mode<4;mode++){reset();dp_copy(p);CHECK(!dp_queue(&c,1,p,16));dp_process();
  if(mode==0)persisted[2]^=1;if(mode==1)duplicate=1;if(mode==2)short_read=1;if(mode==3)read_error=1;
  dp_ready=0;dp_initialize();CHECK(dp_failed);dp_copy(p);CHECK(dp_revision(p)==0&&p[3]==1);
 }
 reset();dp_applied[1]=2;clock_ms+=60000;dp_policy_tick();CHECK(adv_stops==1&&adv_starts==1&&adv_min==1600);
 dp_connected=1;clock_ms+=1;dp_policy_tick();CHECK(adv_starts==1);
 dp_connected=0;clock_ms+=1;dp_policy_tick();CHECK(adv_min==48&&adv_starts==2);
 CHECK(!dp_indicator(true));CHECK(rgb_applied[0]==32&&dp_recording);int count=rgb_writes;
 runtime_fault=1;dp_policy_tick();CHECK(rgb_writes==count);CHECK(!dp_indicator(false)&&!dp_recording);
 /* The real policy requests standby only on idle battery, with discovery kept. */
 reset();dp_applied[1]=2;dp_usb=0;clock_ms+=60000;dp_policy_tick();CHECK(dp_standby_requested);
 dp_connected=1;dp_policy_tick();CHECK(!dp_standby_requested);
 dp_connected=0;clock_ms+=60000;dp_policy_tick();CHECK(dp_standby_requested);
 pressed=1;dp_policy_tick();CHECK(!dp_standby_requested);
 pressed=0;clock_ms+=60000;dp_usb=1;dp_policy_tick();CHECK(!dp_standby_requested);
 dp_usb=0;dp_policy_tick();CHECK(dp_standby_requested);
 busy=1;dp_policy_tick();CHECK(!dp_standby_requested);busy=0;clock_ms+=60000;
 dp_applied[1]=0;dp_policy_tick();CHECK(!dp_standby_requested);
 dp_applied[1]=1;dp_policy_tick();CHECK(dp_standby_requested);
 dp_applied[9]=0;dp_policy_tick();CHECK(!dp_standby_requested);
 /* Schema1 boot migration is RAM-only, preserving colors/revision and CRC. */
 reset();dp_copy(p);p[0]=1;p[3]=3;p[12]=17;memcpy(persisted,p,16);
 sys_put_le32(crc32_ieee(p,16),persisted+16);present=1;dp_ready=0;dp_initialize();dp_copy(p);
 CHECK(p[0]==2&&p[3]==3&&p[12]==17&&!writes&&persisted[0]==1);
 CHECK(!dp_queue(&c,10,p,16));dp_process();CHECK(!last_status&&writes==1&&persisted[0]==2);
 /* Start is acknowledged only by confirm(), stop only after finalization.
  * Drive the actual runtime include at its25ms tick; exactly3/2 pulses. */
 reset();dp_applied[10]=1;dp_applied[3]=3;dp_applied[11]=2;charging=1;
 dp_policy_tick();CHECK(rgb_applied[1]==32);
 CHECK(!pendant_recording_prepare(true));dp_policy_tick();CHECK(!rgb_applied[0]&&!rgb_applied[1]&&!rgb_applied[2]);
 pendant_recording_confirm(true);uint64_t begin=clock_ms;
 unsigned rises=0,previous=0;
 for(unsigned t=0;t<=1200;t+=25){clock_ms=begin+t;dp_policy_tick();unsigned lit=rgb_applied[2]!=0;
  if(lit&&!previous)++rises;previous=lit;CHECK(rgb_applied[0]==0&&rgb_applied[1]==0);}
 CHECK(rises==3&&!previous&&dp_recording==3);
 CHECK(!pendant_recording_prepare(false));CHECK(!dp_blink_count); /* no success pulse before save */
 pendant_recording_confirm(false);begin=clock_ms;rises=previous=0;
 for(unsigned t=0;t<=600;t+=25){clock_ms=begin+t;dp_policy_tick();unsigned lit=rgb_applied[2]!=0;
  if(lit&&!previous)++rises;previous=lit;}
 CHECK(rises==2&&!previous);clock_ms+=25;dp_policy_tick();CHECK(rgb_applied[1]==32);
 CHECK(!pendant_recording_prepare(true));pendant_recording_confirm(true);runtime_fault=1;dp_policy_tick();
 CHECK(rgb_applied[0]==32&&!rgb_applied[2]); /* fault never hidden */
 return checks;
}
