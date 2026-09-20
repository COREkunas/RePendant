/* Actual broker/coordinator/codec, deterministic fake radio/workqueue. No audio. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../usb_firmware/src/recording_control_ble.c"
static uint64_t tests,groups;
#define CHECK(x) do{++tests;if(!(x)){fprintf(stderr,"long-control:%d: %s\n",__LINE__,#x);abort();}}while(0)
static struct recording_control coordinator;
static struct bt_conn phone;
static struct k_work *scheduled;
static int64_t clock_ms;
static unsigned irq_depth,starts,ticket,notifications,drop_queue,drop_auth,notify_busy;
static unsigned gate_after_queue,gate_on_reply_check,command_calls,pending_calls;
static int usb_connected=1;
static uint8_t boot[16],binding[32],operation[16],reply[LC_RESPONSE_BYTES];
static uint16_t sequence;
int64_t k_uptime_get(void){return clock_ms;}
unsigned irq_lock(void){return irq_depth++;}
void irq_unlock(unsigned key){CHECK(irq_depth==key+1);irq_depth=key;}
struct bt_conn *bt_conn_ref(struct bt_conn *p){CHECK(!irq_depth);++p->refs;return p;}
void bt_conn_unref(struct bt_conn *p){CHECK(!irq_depth&&p->refs);--p->refs;}
int k_work_submit(struct k_work *w){CHECK(!irq_depth&&!scheduled);scheduled=w;return 1;}
static uint64_t now_ms(void *u){(void)u;return (uint64_t)clock_ms;}
static int usb(void *u){(void)u;return usb_connected;}
static int ready(void *u){(void)u;return 1;}
static void wake(void *u){(void)u;}
static int queued(void *u,uint32_t value,uint64_t deadline)
{
 (void)u;CHECK(!irq_depth&&deadline==(uint64_t)clock_ms+LRC_PREPARE_MS);++starts;ticket=value;
 if(drop_queue){drop_queue=0;recording_control_ble_disconnected(&phone);}
 if(gate_after_queue){gate_after_queue=0;atomic_store(&coordinator.gate,1);}
 return 0;
}
int recording_runtime_long_available(void){return 1;}
int recording_runtime_long_command(uint64_t e,const uint8_t *p,size_t n,uint8_t *out){++command_calls;return lrc_command(&coordinator,e,p,n,out);}
int recording_runtime_long_pending(uint64_t e,const uint8_t *p,size_t n,uint8_t *out){++pending_calls;return lrc_pending_reply(&coordinator,e,p,n,out);}
int recording_runtime_long_disconnected(uint64_t e){return lrc_disconnected(&coordinator,e);}
int recording_runtime_long_reply_allowed(uint64_t e,uint16_t seq){
 if(gate_on_reply_check&&!--gate_on_reply_check)atomic_store(&coordinator.gate,1);
 return lrc_reply_allowed(&coordinator,e,seq);
}
static int authorized(void *u,struct bt_conn *p)
{
 (void)u;CHECK(!irq_depth);int ok=p==&phone&&p->connected&&p->authorized&&p->subscribed&&p->mtu>=96;
 if(drop_auth&&control_ble.working){drop_auth=0;recording_control_ble_disconnected(p);}
 return ok;
}
static int notify(void *u,struct bt_conn *p,const uint8_t *frame,size_t n)
{
 (void)u;CHECK(!irq_depth&&n==LC_RESPONSE_BYTES&&p==&phone);
 if(!recording_control_ble_reply_allowed(p,(uint16_t)(frame[4]|(uint16_t)frame[5]<<8)))return -EAGAIN;
 if(notify_busy){--notify_busy;return -ENOMEM;}
 ++notifications;memcpy(reply,frame,n);return 0;
}
static void pump(void)
{struct k_work *w=scheduled;scheduled=NULL;CHECK(w!=NULL);w->handler(w);}
static void fresh(void)
{
 CHECK(!phone.refs&&!scheduled);memset(&control_ble,0,sizeof(control_ble));memset(&coordinator,0,sizeof(coordinator));
 memset(&phone,0,sizeof(phone));phone.connected=phone.authorized=phone.subscribed=1;phone.mtu=247;
 for(unsigned i=0;i<16;++i){boot[i]=(uint8_t)(i+1);operation[i]=(uint8_t)(i+17);}
 for(unsigned i=0;i<32;++i)binding[i]=(uint8_t)(i+1);
 clock_ms=100;sequence=0;starts=ticket=notifications=drop_queue=drop_auth=notify_busy=0;
 gate_after_queue=gate_on_reply_check=command_calls=pending_calls=0;
 usb_connected=1;
 struct recording_control_ble_hooks h={authorized,notify,NULL};CHECK(!recording_control_ble_init(&h));
 struct lrc_port port={NULL,now_ms,recording_control_ble_authorized,ready,queued,wake,usb};
 CHECK(!lrc_init(&coordinator,&port,boot,binding));++groups;
}
static void frame(uint8_t out[80],unsigned command,int global)
{
 memset(out,0,80);out[0]='O';out[1]='P';out[2]=1;out[3]=(uint8_t)command;
 ++sequence;out[4]=(uint8_t)sequence;out[5]=(uint8_t)(sequence>>8);out[6]=72;
 memcpy(out+8,boot,16);if(!global)memcpy(out+24,operation,16);memcpy(out+40,binding,32);out[72]=(uint8_t)(command==LC_START);
}
static void command(unsigned cmd,int global)
{uint8_t out[80];frame(out,cmd,global);CHECK(!recording_control_ble_command(&phone,out,80));pump();}
static void close_link(void)
{recording_control_ble_disconnected(&phone);if(scheduled)pump();CHECK(!phone.refs&&!control_ble.conn&&!scheduled);}
static void publish(unsigned phase,unsigned reason,unsigned flags,unsigned count,unsigned admitted)
{
 struct lc_state s;CHECK(!lrc_snapshot(&coordinator,ticket,&s));
 s.phase=(uint8_t)phase;s.reason=(uint8_t)reason;s.flags=(uint16_t)(flags|LC_PROFILE_FLAGS);
 if(flags&LC_SESSION_CREATED){memcpy(s.recording,operation,16);s.recording[0]^=64;s.epoch=ticket;}
 s.accepted_frames=count;s.committed_frames=count;s.admitted_frames=admitted;
 CHECK(!lrc_publish(&coordinator,ticket,&s));
}
__declspec(dllexport) int long_control_ble_tests(uint64_t out[3])
{
 /* Busy BEFORE admission cannot be mislabeled as an executed command. The
  * same copied request may be admitted once after the metadata gate releases. */
 fresh();atomic_store(&coordinator.gate,1);command(LC_START,0);
 CHECK(!starts&&!notifications&&control_ble.pending&&!control_ble.executed&&!control_ble.revoked);
 uint64_t original_deadline=control_ble.deadline;clock_ms+=20;atomic_store(&coordinator.gate,0);
 recording_control_ble_poll();pump();
 CHECK(starts==1&&notifications==1&&command_calls==2&&!pending_calls&&control_ble.deadline==original_deadline);close_link();

 /* Once START was admitted, a busy response path must NEVER queue it again. */
 fresh();gate_after_queue=1;command(LC_START,0);
 CHECK(starts==1&&!notifications&&control_ble.pending&&control_ble.executed&&!control_ble.revoked);
 original_deadline=control_ble.deadline;atomic_store(&coordinator.gate,0);clock_ms+=20;
 recording_control_ble_poll();pump();
 CHECK(starts==1&&notifications==1&&command_calls==1&&pending_calls==1&&control_ble.deadline==original_deadline);close_link();

 /* Both permission rechecks can encounter metadata publication. No response
  * escapes during the collision, no command replay and no renewed deadline. */
 for(unsigned collision=1;collision<=2;++collision){
  fresh();gate_on_reply_check=collision;command(LC_START,0);
  CHECK(starts==1&&!notifications&&control_ble.pending&&control_ble.executed&&!control_ble.revoked);
  original_deadline=control_ble.deadline;clock_ms+=20;atomic_store(&coordinator.gate,0);
  recording_control_ble_poll();pump();
  CHECK(starts==1&&notifications==1&&command_calls==1&&pending_calls==1&&control_ble.deadline==original_deadline);close_link();
 }

 /* A permanently busy pre-admission gate expires WITHOUT any START. */
 fresh();atomic_store(&coordinator.gate,1);command(LC_START,0);clock_ms+=4000;
 recording_control_ble_poll();pump();CHECK(!starts&&!notifications&&control_ble.revoked);
 atomic_store(&coordinator.gate,0);recording_control_ble_poll();pump();
 CHECK(!starts&&!notifications&&!phone.refs&&command_calls==1&&!pending_calls);

 /* Repeated running STATUS collisions must not truncate a long recording.
  * START occurs once; status attempts never latch STOP or reset the coordinator. */
 fresh();command(LC_START,0);
 publish(LC_RUNNING,0,LC_PROFILE_FLAGS|LC_USB_PRESENT|LC_MIC_ON|LC_SESSION_CREATED|LC_CAPACITY_KNOWN,500,2560000);
 for(unsigned i=0;i<600;++i){
  unsigned before=notifications;clock_ms+=1000;
#ifdef OPENPENDANT_PORTABLE_RECORDING
  usb_connected=!(i>=200&&i<400);
#endif
  if(i%3==0)atomic_store(&coordinator.gate,1);else gate_on_reply_check=i%3;
  command(LC_STATUS,0);CHECK(notifications==before&&control_ble.pending&&!control_ble.revoked);
  original_deadline=control_ble.deadline;atomic_store(&coordinator.gate,0);clock_ms+=25;
  recording_control_ble_poll();pump();
  CHECK(notifications==before+1&&reply[57]==LC_RUNNING&&starts==1&&!coordinator.fault&&
   !lrc_stop_requested(&coordinator,ticket)&&control_ble.deadline==original_deadline);
 }
 command(LC_STOP,0);CHECK(starts==1&&lrc_stop_requested(&coordinator,ticket)==1);close_link();

 /* Revocation while waiting is not mistaken for benign contention. */
 fresh();atomic_store(&coordinator.gate,1);command(LC_START,0);
 recording_control_ble_disconnected(&phone);atomic_store(&coordinator.gate,0);pump();
 CHECK(!starts&&!notifications&&!phone.refs&&command_calls==1);
 fresh();gate_on_reply_check=1;command(LC_START,0);phone.authorized=0;atomic_store(&coordinator.gate,0);
 recording_control_ble_poll();pump();CHECK(starts==1&&!notifications&&!phone.refs);
 CHECK(!lrc_stop_requested(&coordinator,ticket));

 /* A reply-gate collision cannot renew or outlive the original four seconds. */
 fresh();gate_on_reply_check=1;command(LC_START,0);clock_ms+=4000;atomic_store(&coordinator.gate,0);
 recording_control_ble_poll();pump();CHECK(starts==1&&!notifications&&!phone.refs&&!lrc_stop_requested(&coordinator,ticket));

 fresh();command(LC_STATUS,1);CHECK(!starts&&reply[57]==LC_IDLE);command(LC_START,0);
 CHECK(starts==1&&reply[57]==LC_STARTING);command(LC_START,0);CHECK(starts==1);
 close_link();CHECK(!lrc_stop_requested(&coordinator,ticket));
 publish(LC_RUNNING,0,LC_USB_PRESENT|LC_MIC_ON|LC_SESSION_CREATED|LC_CAPACITY_KNOWN,180000,2560000);
 sequence=0;command(LC_STATUS,0);CHECK(reply[57]==LC_RUNNING&&starts==1);
 command(LC_STOP,0);CHECK(lrc_stop_requested(&coordinator,ticket)==1&&reply[57]==LC_RUNNING&&(reply[60]&2));
 publish(LC_STOPPED,1,LC_USB_PRESENT|LC_SESSION_CREATED|LC_CAPACITY_KNOWN|60,180000,2560000);
 command(LC_STATUS,0);CHECK(reply[57]==LC_STOPPED);close_link();

 fresh();notify_busy=2;command(LC_START,0);CHECK(starts==1&&!notifications&&control_ble.pending);
 recording_control_ble_poll();pump();CHECK(starts==1&&!notifications);
 recording_control_ble_poll();pump();CHECK(starts==1&&notifications==1);close_link();

 fresh();drop_queue=1;command(LC_START,0);CHECK(starts==1&&!phone.refs&&!notifications);
 CHECK(!lrc_stop_requested(&coordinator,ticket));sequence=0;command(LC_START,0);CHECK(starts==1);close_link();

 fresh();drop_auth=1;command(LC_START,0);CHECK(!starts&&!phone.refs&&!notifications);

 fresh();notify_busy=1;command(LC_START,0);clock_ms+=4000;recording_control_ble_poll();pump();
 CHECK(starts==1&&!phone.refs&&!notifications&&!lrc_stop_requested(&coordinator,ticket));

 fresh();phone.authorized=0;uint8_t p[80];frame(p,LC_START,0);
 CHECK(recording_control_ble_command(&phone,p,80)!=0&&!starts&&!phone.refs);phone.authorized=1;
 p[40]^=1;CHECK(!recording_control_ble_command(&phone,p,80));pump();CHECK(!starts&&!phone.refs&&!notifications);

 fresh();command(LC_START,0);atomic_store(&coordinator.gate,1);recording_control_ble_disconnected(&phone);pump();
 CHECK(phone.refs==1&&control_ble.revoked);atomic_store(&coordinator.gate,0);
 recording_control_ble_poll();pump();CHECK(!phone.refs&&!lrc_stop_requested(&coordinator,ticket));

 fresh();for(unsigned i=0;i<LRC_OPERATIONS;++i){operation[0]=(uint8_t)(i+1);command(LC_START,0);
  CHECK(starts==i+1);publish(LC_CANCELLED_BEFORE_START,1,LC_USB_PRESENT|44,0,0);}
 operation[0]=200;command(LC_START,0);CHECK(starts==LRC_OPERATIONS&&!phone.refs);

 fresh();for(unsigned n=1;n<=5120;++n){struct lc_state s={0};memcpy(s.boot,boot,16);memcpy(s.operation,operation,16);
  memcpy(s.recording,operation,16);s.epoch=1;s.phase=LC_STOPPED;s.reason=11;s.flags=LC_USB_PRESENT|LC_SESSION_CREATED|LC_CAPACITY_KNOWN|60;
  s.accepted_frames=s.committed_frames=s.admitted_frames=n*500U;CHECK(!lc_validate_state(&s));
  s.accepted_frames++;CHECK(lc_validate_state(&s)!=0);}
 /* Button is inert without explicit arming; held-at-boot does not become tap. */
 fresh();CHECK(!lrc_button_sample(&coordinator,1));clock_ms+=60;CHECK(!lrc_button_sample(&coordinator,0));
 clock_ms+=60;CHECK(!lrc_button_sample(&coordinator,0));CHECK(!starts);
 frame(p,LC_START,0);p[72]=2;CHECK(!recording_control_ble_command(&phone,p,80));pump();
 CHECK(!starts&&reply[57]==LC_STARTING&&(reply[60]&4));
 command(LC_STATUS,0);CHECK(!starts&&(reply[60]&4));close_link();
 CHECK(coordinator.armed==1&&!lrc_stop_requested(&coordinator,1));
 /* A bounced press/release cannot start. */
 clock_ms+=25;CHECK(!lrc_button_sample(&coordinator,1));clock_ms+=25;CHECK(!lrc_button_sample(&coordinator,0));
 clock_ms+=60;CHECK(!lrc_button_sample(&coordinator,0));CHECK(!starts);
 clock_ms+=25;CHECK(!lrc_button_sample(&coordinator,1));clock_ms+=50;CHECK(!lrc_button_sample(&coordinator,1));
 clock_ms+=200;CHECK(!lrc_button_sample(&coordinator,0));clock_ms+=50;CHECK(!lrc_button_sample(&coordinator,0));
 CHECK(!starts);clock_ms+=600;CHECK(!lrc_button_sample(&coordinator,0));
 CHECK(starts==1&&!coordinator.armed&&!lrc_stop_requested(&coordinator,1));
 publish(LC_RUNNING,0,LC_USB_PRESENT|LC_MIC_ON|LC_SESSION_CREATED|LC_CAPACITY_KNOWN,500,2560000);
 clock_ms+=25;CHECK(!lrc_button_sample(&coordinator,1));clock_ms+=50;CHECK(!lrc_button_sample(&coordinator,1));
 clock_ms+=200;CHECK(!lrc_button_sample(&coordinator,0));clock_ms+=50;CHECK(!lrc_button_sample(&coordinator,0));
 CHECK(!lrc_stop_requested(&coordinator,1));clock_ms+=600;CHECK(!lrc_button_sample(&coordinator,0));
 CHECK(starts==1&&lrc_stop_requested(&coordinator,1)==1);

 fresh();CHECK(!lrc_button_sample(&coordinator,0));frame(p,LC_START,0);p[72]=2;
 CHECK(!recording_control_ble_command(&phone,p,80));pump();clock_ms+=LRC_PREPARE_MS;
 CHECK(!lrc_button_sample(&coordinator,0));command(LC_STATUS,0);
 CHECK(reply[57]==LC_CANCELLED_BEFORE_START&&!starts&&!coordinator.armed);close_link();

 fresh();CHECK(!lrc_button_sample(&coordinator,0));frame(p,LC_START,0);p[72]=2;
 CHECK(!recording_control_ble_command(&phone,p,80));pump();command(LC_STOP,0);
 CHECK(reply[57]==LC_CANCELLED_BEFORE_START&&!starts&&!coordinator.armed);close_link();

 fresh();CHECK(!lrc_button_sample(&coordinator,0));frame(p,LC_START,0);p[72]=2;
 CHECK(!recording_control_ble_command(&phone,p,80));pump();
 clock_ms+=25;CHECK(!lrc_button_sample(&coordinator,1));clock_ms+=50;CHECK(!lrc_button_sample(&coordinator,1));
 clock_ms+=1500;CHECK(!lrc_button_sample(&coordinator,0));clock_ms+=50;CHECK(!lrc_button_sample(&coordinator,0));
 CHECK(!starts&&coordinator.armed);CHECK(lrc_button_sample(&coordinator,-1)==LRC_REFUSED);
 command(LC_STATUS,0);CHECK(reply[57]==LC_CANCELLED_BEFORE_START&&!starts);close_link();
 CHECK(!phone.refs);out[0]=tests;out[1]=groups;out[2]=sizeof(control_ble)+sizeof(coordinator);return 0;
}
