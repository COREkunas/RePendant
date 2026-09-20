/* Actual production security C with host-only deterministic native BT/RTOS
 * fakes. These tests do not simulate cryptography, NVS durability or radio. */
#include "test_stubs.h"
#include "../../../usb_firmware/src/ble_security.c"
#include "../../../usb_firmware/src/button_gesture.h"

static int64_t now;
static int lock_depth, violations, assertion_count;
static int fake_bonds, fake_bond_address;
static int register_error, info_register_error, settings_error, security_error;
static int scheduling_error, print_count, code_print_count, stored_code_print;
static bool mic_busy, recovery_busy, cancel_reentry;
static void (*unlock_hook)(void);
static void (*bond_hook)(void);
static int hook_result;
static int maintenance_error,maintenance_held,power_lost,unpair_error,unpair_calls,saved_bonds,readback_error;
static struct shell usb_shell={1}, other_shell={2};
static struct bt_conn peer, stranger;
static char *open_argv[]={"open","confirm"};
static char *status_argv[]={"status"};
static char *close_argv[]={"close"};
static struct bt_conn_pairing_feat valid_feature={4,0,9,16,0,0};

int test_strcmp(const char *a,const char *b)
{ while(*a && *a==*b){a++;b++;} return (unsigned char)*a-(unsigned char)*b; }
static bool prefix(const char *a,const char *b)
{ while(*b){if(*a++!=*b++)return false;}return true; }
k_spinlock_key_t k_spin_lock(struct k_spinlock *lock)
{ ARG_UNUSED(lock); if(lock_depth++)violations++; return 0; }
void k_spin_unlock(struct k_spinlock *lock,k_spinlock_key_t key)
{
	ARG_UNUSED(lock);ARG_UNUSED(key);if(--lock_depth!=0)violations++;
	if(unlock_hook){void(*hook)(void)=unlock_hook;unlock_hook=NULL;hook();}
}
int64_t k_uptime_get(void){return now;}
int k_work_reschedule(struct k_work_delayable *work,int64_t delay)
{if(scheduling_error)return scheduling_error;work->due=now+delay;work->scheduled=true;return 0;}
int k_work_cancel_delayable(struct k_work_delayable *work)
{if(lock_depth)violations++;work->scheduled=false;return 0;}
static void outside_lock(void){if(lock_depth)violations++;}
int bt_conn_get_info(struct bt_conn *conn,struct bt_conn_info *info)
{outside_lock();*info=conn->info;return 0;}
const bt_addr_le_t *bt_conn_get_dst(struct bt_conn *conn)
{outside_lock();return &conn->address;}
bool bt_le_bond_exists(int id,const bt_addr_le_t *address)
{outside_lock();return id==0 && fake_bonds==1 && address->value==fake_bond_address;}
void bt_foreach_bond(int id,void (*fn)(const struct bt_bond_info*,void*),void *data)
{
	ARG_UNUSED(id);outside_lock();struct bt_bond_info info={0};
	for(int i=0;i<fake_bonds;i++)fn(&info,data);
	if(bond_hook){void(*hook)(void)=bond_hook;bond_hook=NULL;hook();}
}
struct bt_conn *bt_conn_ref(struct bt_conn *conn)
{outside_lock();conn->refs++;return conn;}
void bt_conn_unref(struct bt_conn *conn)
{outside_lock();if(--conn->refs<1)violations++;}
int bt_conn_auth_cancel(struct bt_conn *conn)
{outside_lock();conn->cancelled++;if(cancel_reentry)authentication_cancelled(conn);return 0;}
int bt_conn_disconnect(struct bt_conn *conn,int reason)
{ARG_UNUSED(reason);outside_lock();conn->disconnected++;return 0;}
int bt_conn_set_security(struct bt_conn *conn,bt_security_t level)
{outside_lock();if(level!=4)violations++;conn->set_security++;return security_error;}
int bt_conn_auth_cb_register(const struct bt_conn_auth_cb *cb)
{outside_lock();if(cb!=&authentication_callbacks)violations++;return register_error;}
int bt_conn_auth_info_cb_register(struct bt_conn_auth_info_cb *cb)
{outside_lock();if(cb!=&authentication_info)violations++;return info_register_error;}
int settings_load(void){outside_lock();return settings_error;}
int recording_runtime_pairing_claim(void)
{outside_lock();if(!closing)violations++;if(maintenance_error)return maintenance_error;maintenance_held=1;return 0;}
void recording_runtime_pairing_release(void){outside_lock();maintenance_held=0;}
int recording_runtime_pairing_ready(void){return maintenance_held&&!maintenance_error&&!power_lost;}
int bt_unpair(uint8_t id,const bt_addr_le_t *address)
{outside_lock();unpair_calls++;if(id||address||!closing||!maintenance_held)violations++;
 if(unpair_error)return unpair_error;fake_bonds=0;disconnected(&peer,0);return 0;}
int settings_load_subtree_direct(const char *name,int (*cb)(const char*,size_t,settings_read_cb,void*,void*),void *user)
{outside_lock();if(test_strcmp(name,"bt/keys")||!maintenance_held)violations++;
 if(readback_error)return readback_error;if(saved_bonds)cb("fake-address",16,NULL,NULL,user);return 0;}
bool mic_commands_busy(void){return mic_busy;}
bool pendant_recovery_is_pending(void){return recovery_busy;}
const struct shell *shell_backend_uart_get_ptr(void){return &usb_shell;}
void shell_print(const struct shell *sh,const char *format,...)
{
	outside_lock();if(sh!=&usb_shell)violations++;print_count++;
	if(prefix(format,"PAIRING_PASSKEY ")){
		va_list args;va_start(args,format);stored_code_print=(int)va_arg(args,unsigned int);
		va_end(args);code_print_count++;
	}
}
static void reset_peer(struct bt_conn *conn,int address)
{
	conn->info.type=BT_CONN_TYPE_LE;conn->info.role=BT_CONN_ROLE_PERIPHERAL;
	conn->info.id=0;conn->info.state=BT_CONN_STATE_CONNECTED;
	conn->info.security.level=BT_SECURITY_L1;conn->info.security.enc_key_size=0;
	conn->info.security.flags=0;conn->address.value=address;conn->refs=1;
	conn->disconnected=conn->cancelled=conn->set_security=0;
}
static void reset(void)
{
	now=1000;lock_depth=0;violations=0;unlock_hook=NULL;bond_hook=NULL;
	register_error=info_register_error=settings_error=security_error=scheduling_error=0;
	print_count=code_print_count=stored_code_print=0;mic_busy=recovery_busy=false;
	cancel_reentry=true;fake_bonds=0;fake_bond_address=11;hook_result=0;
	init_result=-EAGAIN;init_called=0;owners=0;window_open=closing=false;
	window_deadline=0;pending=NULL;code_valid=false;displayed_code=0;
	replacement_result=maintenance_error=maintenance_held=power_lost=unpair_error=unpair_calls=saved_bonds=readback_error=0;
	expiry_work.due=0;expiry_work.scheduled=false;
	reset_peer(&peer,11);reset_peer(&stranger,12);
}
static int open_local(void){return command_open(&usb_shell,2,open_argv);}
static void secure_peer(void)
{peer.info.security.level=4;peer.info.security.enc_key_size=16;peer.info.security.flags=1;fake_bonds=1;}
static int setup_pair(void)
{
	reset();if(pendant_ble_security_init()!=0 || open_local()!=0)return -1;
	connected(&peer,0);if(accept_pairing(&peer,&valid_feature)!=0)return -2;
	show_passkey(&peer,37);return 0;
}
static void close_and_reopen(void)
{
	command_close(&usb_shell,1,close_argv);now++;
	hook_result=open_local();
}
static void expire_before_complete_commit(void)
{now=window_deadline;expire_window(NULL);}
#define CHECK(test) do {assertion_count++;if(!(test))return __LINE__;} while(0)

__declspec(dllexport) int security_tests(void)
{
	assertion_count=0;reset();
	CHECK(!pendant_ble_authorized(&peer));
	register_error=-17;CHECK(pendant_ble_security_init()==-17);
	CHECK(!pendant_ble_authorized(&peer));
	reset();info_register_error=-18;CHECK(pendant_ble_security_init()==-18);
	reset();settings_error=-19;CHECK(pendant_ble_security_init()==-19);
	CHECK(open_local()==-EACCES);
	reset();fake_bonds=2;CHECK(pendant_ble_security_init()==-EOVERFLOW);
	reset();CHECK(pendant_ble_security_init()==0);
	CHECK(pendant_ble_security_init()==-EALREADY);
	CHECK(!pendant_ble_authorized(NULL));
	connected(&peer,0);CHECK(peer.disconnected==1 && peer.set_security==0);
	CHECK(!pendant_ble_pairing_busy());
	CHECK(command_open(&other_shell,2,open_argv)==-EINVAL);
	CHECK(command_open(&usb_shell,1,open_argv)==-EINVAL);
	char *bad_argv[]={"open","not-confirm"};
	CHECK(command_open(&usb_shell,2,bad_argv)==-EINVAL);
	mic_busy=true;CHECK(open_local()==-EBUSY);mic_busy=false;
	recovery_busy=true;CHECK(open_local()==-EBUSY);recovery_busy=false;
	scheduling_error=-3;CHECK(open_local()==-3 && !pendant_ble_pairing_busy());
	scheduling_error=0;CHECK(open_local()==0 && pendant_ble_pairing_busy());
	CHECK(window_deadline==61000 && expiry_work.due==61000);
	CHECK(open_local()==-EBUSY);
	CHECK(command_status(&usb_shell,1,status_argv)==0 && code_print_count==0);
	connected(&peer,0);CHECK(pending==&peer && peer.refs==2 && peer.set_security==1);
	connected(&stranger,0);CHECK(stranger.disconnected==1 && stranger.refs==1);
	CHECK(accept_pairing(&peer,&valid_feature)==BT_SECURITY_ERR_SUCCESS);
	CHECK(accept_pairing(&stranger,&valid_feature)==BT_SECURITY_ERR_PAIR_NOT_ALLOWED);
	CHECK(accept_pairing(&peer,NULL)==BT_SECURITY_ERR_AUTH_REQUIREMENT);
	struct bt_conn_pairing_feat feat=valid_feature;
	feat.auth_req=1;CHECK(accept_pairing(&peer,&feat)==BT_SECURITY_ERR_AUTH_REQUIREMENT);
	feat=valid_feature;feat.auth_req=8;CHECK(accept_pairing(&peer,&feat)==BT_SECURITY_ERR_AUTH_REQUIREMENT);
	feat=valid_feature;feat.max_enc_key_size=15;CHECK(accept_pairing(&peer,&feat)==BT_SECURITY_ERR_AUTH_REQUIREMENT);
	feat=valid_feature;feat.io_capability=1;CHECK(accept_pairing(&peer,&feat)==BT_SECURITY_ERR_AUTH_REQUIREMENT);
	mic_busy=true;CHECK(accept_pairing(&peer,&valid_feature)==BT_SECURITY_ERR_PAIR_NOT_ALLOWED);mic_busy=false;
	recovery_busy=true;CHECK(accept_pairing(&peer,&valid_feature)==BT_SECURITY_ERR_PAIR_NOT_ALLOWED);recovery_busy=false;
	int before=print_count;show_passkey(&peer,37);
	CHECK(code_valid && displayed_code==37 && print_count==before);
	CHECK(!pendant_ble_authorized(&peer));
	CHECK(command_status(&usb_shell,1,status_argv)==0 && code_print_count==1 && stored_code_print==37);
	CHECK(command_status(&other_shell,1,status_argv)==-EINVAL);
	CHECK(command_close(&other_shell,1,close_argv)==-EINVAL);
	CHECK(command_replacement(&other_shell,1,status_argv)==-EINVAL);
	before=print_count;CHECK(!command_replacement(&usb_shell,1,status_argv)&&print_count==before+1);
	CHECK(command_close(&usb_shell,1,close_argv)==0);
	CHECK(!pendant_ble_pairing_busy() && pending==NULL && peer.refs==1 && peer.cancelled==1);
	CHECK(!code_valid && displayed_code==0 && !expiry_work.scheduled && violations==0);

	CHECK(setup_pair()==0);now=window_deadline-1;expire_window(NULL);
	CHECK(window_open && code_valid && peer.cancelled==0);
	now++;expire_window(NULL);
	CHECK(!window_open && !code_valid && displayed_code==0 && peer.cancelled==1 && peer.refs==1);
	CHECK(accept_pairing(&peer,&valid_feature)==BT_SECURITY_ERR_PAIR_NOT_ALLOWED);
	CHECK(command_status(&usb_shell,1,status_argv)==0 && code_print_count==0);
	CHECK(violations==0);

	/* Exact stale-expiry interleaving: expire snapshots old deadline, unlock
	 * hook closes/reopens, then old finish must preserve the new window. */
	CHECK(setup_pair()==0);now=window_deadline;unlock_hook=close_and_reopen;
	expire_window(NULL);
	CHECK(hook_result==0 && window_open && window_deadline==now+60000);
	CHECK(expiry_work.scheduled && expiry_work.due==window_deadline && pending==NULL);
	CHECK(peer.refs==1 && violations==0);
	/* Same interleave in the early-reschedule branch must preserve new timer. */
	CHECK(setup_pair()==0);now+=7;unlock_hook=close_and_reopen;expire_window(NULL);
	CHECK(hook_result==0 && window_open && expiry_work.due==window_deadline);
	CHECK(violations==0);

	CHECK(setup_pair()==0);secure_peer();pairing_complete(&peer,true);
	CHECK(owners==1 && !window_open && !code_valid && displayed_code==0 && peer.refs==1);
	CHECK(pendant_ble_authorized(&peer) && peer.disconnected==0);
	CHECK(open_local()==-EALREADY && accept_pairing(&peer,&valid_feature)==BT_SECURITY_ERR_PAIR_NOT_ALLOWED);
	CHECK(!pendant_ble_authorized(&stranger));
	peer.info.security.level=3;CHECK(!pendant_ble_authorized(&peer));peer.info.security.level=4;
	peer.info.security.enc_key_size=15;CHECK(!pendant_ble_authorized(&peer));peer.info.security.enc_key_size=16;
	peer.info.security.flags=0;CHECK(!pendant_ble_authorized(&peer));peer.info.security.flags=1;
	peer.info.state=0;CHECK(!pendant_ble_authorized(&peer));peer.info.state=BT_CONN_STATE_CONNECTED;
	peer.info.id=1;CHECK(!pendant_ble_authorized(&peer));peer.info.id=0;
	peer.info.role=0;CHECK(!pendant_ble_authorized(&peer));peer.info.role=1;
	CHECK(pendant_ble_authorized(&peer) && violations==0);
	connected(&stranger,0);CHECK(stranger.disconnected==1 && stranger.set_security==0);
	CHECK(command_close(&usb_shell,1,close_argv)==0 && owners==1 && fake_bonds==1);

	/* Persisted owner reconnect uses native L4 request without opening USB. */
	reset();fake_bonds=1;CHECK(pendant_ble_security_init()==0 && owners==1);
	connected(&peer,0);CHECK(peer.set_security==1 && pending==NULL && peer.refs==1);
	secure_peer();CHECK(pendant_ble_authorized(&peer));
	CHECK(accept_pairing(&peer,&valid_feature)==BT_SECURITY_ERR_PAIR_NOT_ALLOWED);
	security_changed(&peer,4,BT_SECURITY_ERR_SUCCESS);CHECK(peer.disconnected==0);
	security_changed(&peer,2,BT_SECURITY_ERR_SUCCESS);CHECK(peer.disconnected==1);
	CHECK(violations==0);

	/* Completion arriving after timeout or local cancel never authorizes. */
	CHECK(setup_pair()==0);secure_peer();now=window_deadline;pairing_complete(&peer,true);
	CHECK(init_result==-EACCES && !pendant_ble_authorized(&peer) && owners==0);
	CHECK(peer.refs==1 && displayed_code==0 && fake_bonds==1 && violations==0);
	CHECK(setup_pair()==0);secure_peer();bond_hook=expire_before_complete_commit;
	pairing_complete(&peer,true);
	CHECK(init_result==-EACCES && owners==0 && !pendant_ble_authorized(&peer));
	CHECK(peer.cancelled==1 && peer.refs==1 && !window_open && violations==0);
	CHECK(setup_pair()==0);secure_peer();command_close(&usb_shell,1,close_argv);
	pairing_complete(&peer,true);CHECK(init_result==-EACCES && !pendant_ble_authorized(&peer));
	CHECK(setup_pair()==0);secure_peer();pairing_complete(&peer,false);
	CHECK(init_result==-EACCES && !pendant_ble_authorized(&peer));
	CHECK(setup_pair()==0);secure_peer();code_valid=false;pairing_complete(&peer,true);
	CHECK(init_result==-EACCES && !pendant_ble_authorized(&peer));

	CHECK(setup_pair()==0);before=print_count;
	pairing_failed(&peer,BT_SECURITY_ERR_AUTH_FAIL);
	CHECK(!window_open && !code_valid && peer.refs==1 && print_count==before && violations==0);
	CHECK(setup_pair()==0);disconnected(&peer,0);
	CHECK(!window_open && !code_valid && peer.refs==1 && violations==0);
	CHECK(setup_pair()==0);before=print_count;show_passkey(&peer,1000000);
	CHECK(!window_open && !code_valid && displayed_code==0 && print_count==before && violations==0);
	CHECK(setup_pair()==0);show_passkey(&stranger,123456);
	CHECK(window_open && pending==&peer && displayed_code==37 && stranger.disconnected>0);
	CHECK(violations==0);
	/* Physical replacement, unlike USB open, can remove the old BT owner. */
	reset();fake_bonds=1;CHECK(!pendant_ble_security_init());
	CHECK(open_local()==-EALREADY&&unpair_calls==0);
	CHECK(!pendant_ble_pairing_replace_local()&&unpair_calls==1&&fake_bonds==0&&owners==0);
	CHECK(window_open&&pendant_ble_pairing_remaining_ms()==60000&&!maintenance_held&&violations==0);
	CHECK(pendant_ble_pairing_replace_local()==-EBUSY&&unpair_calls==1);
	/* The old peer's late disconnect cannot close a new enrollment. */
	connected(&stranger,0);CHECK(pending==&stranger&&accept_pairing(&stranger,&valid_feature)==0);
	disconnected(&peer,0);CHECK(window_open&&pending==&stranger);
	show_passkey(&stranger,123456);stranger.info.security.level=4;
	stranger.info.security.enc_key_size=16;stranger.info.security.flags=1;
	fake_bond_address=stranger.address.value;fake_bonds=1;pairing_complete(&stranger,true);
	CHECK(pendant_ble_authorized(&stranger)&&!pendant_ble_authorized(&peer)&&!window_open&&violations==0);
	CHECK(!pendant_ble_pairing_replace_local()&&unpair_calls==2);
	now+=59999;CHECK(pendant_ble_pairing_remaining_ms()==1);now++;
	CHECK(!pendant_ble_pairing_remaining_ms());expire_window(NULL);CHECK(!pendant_ble_pairing_busy());
	/* Idle/power refusal never deletes. Uncertain deletion/readback never opens. */
	reset();fake_bonds=1;CHECK(!pendant_ble_security_init());maintenance_error=-EBUSY;
	CHECK(pendant_ble_pairing_replace_local()==-EBUSY&&unpair_calls==0&&owners==1&&!closing);
	maintenance_error=0;power_lost=1;
	CHECK(pendant_ble_pairing_replace_local()==-EPERM&&!unpair_calls&&owners==1&&!closing&&!maintenance_held);
	CHECK(init_result==0&&!window_open&&violations==0);
	for(int mode=0;mode<4;mode++){
		reset();fake_bonds=1;CHECK(!pendant_ble_security_init());
		if(mode==0)unpair_error=-EIO;if(mode==1)saved_bonds=1;
		if(mode==2)readback_error=-EIO;if(mode==3)scheduling_error=-EIO;
		CHECK(pendant_ble_pairing_replace_local()==-EIO&&unpair_calls==1&&!window_open&&!maintenance_held);
		CHECK(pendant_ble_pairing_replace_local()==-EACCES&&unpair_calls==1&&violations==0);
	}
	reset();CHECK(!pendant_ble_security_init());mic_busy=true;
	CHECK(pendant_ble_pairing_replace_local()==-EBUSY&&!unpair_calls);mic_busy=false;recovery_busy=true;
	CHECK(pendant_ble_pairing_replace_local()==-EBUSY&&!unpair_calls);
	/* Actual pure decoder: no single action leaks out of2..6-tap sequences. */
	for(unsigned taps=1;taps<=6;taps++){
		struct button_gesture g={0};uint64_t t=100;unsigned pairs=0,singles=0;
		CHECK(bg_sample(&g,0,t)==BG_NONE);
		for(unsigned i=0;i<taps;i++){
			CHECK(bg_sample(&g,1,t+=25)==BG_NONE);
			enum bg_event e=bg_sample(&g,1,t+=50);CHECK(e==BG_PRESS||e==BG_NONE);
			CHECK(bg_sample(&g,0,t+=100)==BG_NONE);
			e=bg_sample(&g,0,t+=50);CHECK(e==BG_FIVE||e==BG_NONE);pairs+=e==BG_FIVE;
		}
		singles+=bg_sample(&g,0,t+=600)==BG_SINGLE;
		CHECK(pairs==(taps>=5?1U:0U)&&singles==(taps==1?1U:0U));
		CHECK(bg_sample(&g,0,t+600)==BG_NONE);
	}
	struct button_gesture g={0};CHECK(bg_sample(&g,1,0)==BG_NONE);
	CHECK(bg_sample(&g,0,200)==BG_NONE&&bg_sample(&g,0,250)==BG_NONE);
	CHECK(bg_sample(&g,0,900)==BG_NONE); /* held at boot */
	CHECK(bg_sample(&g,1,925)==BG_NONE&&bg_sample(&g,0,950)==BG_NONE);
	CHECK(bg_sample(&g,0,1600)==BG_NONE); /* bounce */
	CHECK(bg_sample(&g,1,1625)==BG_NONE&&bg_sample(&g,1,1675)==BG_PRESS);
	CHECK(bg_sample(&g,0,3000)==BG_NONE&&bg_sample(&g,0,3050)==BG_NONE);
	CHECK(bg_sample(&g,0,4000)==BG_NONE); /* long hold */
	CHECK(bg_sample(&g,-1,4001)==BG_NONE&&!g.seen);
	CHECK(bg_sample(&g,0,5000)==BG_NONE&&bg_sample(&g,0,4999)==BG_NONE&&!g.seen);
	CHECK(bg_sample(&g,0,UINT64_MAX)==BG_NONE&&!g.seen);
	return 0;
}
__declspec(dllexport) int security_assertion_count(void){return assertion_count;}
