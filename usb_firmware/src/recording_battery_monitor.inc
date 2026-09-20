/* Single service actor. Verified gauge POR may restore factory configuration
 * with stable USB and exclusive idle admission. If USB is absent, release
 * external owners and defer BEFORE recovery I/O. Never start audio or retry
 * uncertain transfers/configuration writes. */
static struct bp_sample watch_sample;
static atomic_t watch_requested,watch_initialized,watch_done,watch_cancel,watch_error;
#ifdef OPENPENDANT_GAUGE_POR_RECOVERY
static atomic_t watch_deferred;
#endif
static uint32_t watch_samples,watch_usb_samples,watch_battery_samples;
static uint16_t watch_min_mv,watch_max_mv;
static int watch_init_allowed(void *);
#ifdef OPENPENDANT_PORTABLE_RECORDING
static int portable_admit(void)
{
 unsigned key=irq_lock();int ok=atomic_get(&watch_initialized)&&!atomic_get(&watch_done)&&
  !atomic_get(&watch_cancel)&&bp_start(&watch_sample,now(NULL));irq_unlock(key);return ok;
}
static int portable_begin(void)
{
 unsigned key=irq_lock();int ok=atomic_get(&watch_initialized)&&!atomic_get(&portable_owned)&&!atomic_get(&watch_done)&&
  !atomic_get(&watch_cancel)&&bp_begin(&portable_lease,&watch_sample,now(NULL));
 if(ok)atomic_set(&portable_owned,1);
 irq_unlock(key);return ok;
}
static int portable_check(void)
{
 unsigned key=irq_lock();struct bp_sample s=watch_sample;
 if(atomic_get(&watch_done)||atomic_get(&watch_cancel))s.valid=0;
 int ok=atomic_get(&portable_owned)&&bp_check(&portable_lease,&s,now(NULL));irq_unlock(key);return ok;
}
static int portable_capture_ready(void)
{
 unsigned key=irq_lock();int ok=atomic_get(&portable_owned)&&!portable_lease.draining&&
  !atomic_get(&watch_done)&&!atomic_get(&watch_cancel)&&bp_start(&watch_sample,now(NULL));
 irq_unlock(key);return ok;
}
static void portable_end(void)
{
 /* Called by the command owner only after capture/storage release. Never
  * clear power ownership over an uncertain peripheral/worker lifetime. */
 if(atomic_get(&recording)||atomic_get(&storage_running)||atomic_get(&leased))fatal(NULL,-EPERM);
 unsigned key=irq_lock();atomic_clear(&portable_owned);memset(&portable_lease,0,sizeof(portable_lease));irq_unlock(key);
}
#ifdef OPENPENDANT_BATTERY_SYNC
#include "recording_sync_power.inc"
#endif
void recording_runtime_battery_telemetry(struct battery_telemetry *out)
{
 if(!out)return;
 unsigned key=irq_lock();struct bp_sample s=watch_sample;uint64_t t=now(NULL);
 int stopped=atomic_get(&watch_done)||atomic_get(&watch_cancel);irq_unlock(key);
 *out=(struct battery_telemetry){.flags=(uint8_t)(4U|(stopped?8U:0U))};
 if(!stopped&&bp_fresh(&s,t)){
  out->flags|=(uint8_t)(1U|(bp_start(&s,t)?2U:0U));out->mv=s.mv;out->temp=s.temp;
  out->soc=s.soc;out->gauge_flags=s.flags;out->age_ms=(uint32_t)(t-s.at);out->sequence=s.sequence;
 }
}
static int watch_auto_start(void)
{
 if(!long_bound||!atomic_get(&ready)||atomic_get(&watch_requested)||
    battery_result.attempted||battery_restore.attempted||battery_service_claimed()||
    atomic_get(&capacity_used)||atomic_get(&trial_used)||atomic_get(&recovery_used)||
    !atomic_cas(&command_busy,0,1))return 0;
 if(!battery_control_lock()){atomic_clear(&command_busy);return 0;}
 if(!watch_init_allowed(NULL)||enrollment.active||pendant_button_read()!=0||mic_commands_reserve_external()){
  battery_control_unlock();atomic_clear(&command_busy);return 0;
 }
 atomic_set(&watch_requested,1);return 1;
}
#endif
static int watch_init_allowed(void *unused)
{
 ARG_UNUSED(unused);return atomic_get(&ready)&&atomic_get(&command_busy)&&
  !atomic_get(&watch_cancel)&&!atomic_get(&faulted)&&!atomic_get(&recording)&&
  !atomic_get(&leased)&&!atomic_get(&storage_running)&&!atomic_get(&catalog_epoch)&&
  !atomic_get(&sync_actor)&&!atomic_get(&retire_pending)&&
  !pendant_recovery_is_pending()&&!pendant_ble_pairing_busy();
}
static int watch_read_allowed(void *unused)
{ARG_UNUSED(unused);return !atomic_get(&watch_cancel)&&!atomic_get(&faulted)&&!pendant_recovery_is_pending();}
#ifdef OPENPENDANT_GAUGE_POR_RECOVERY
struct watch_repair_gate {uint32_t usb_epoch;int bound;};
static int watch_restore_allowed(void *user)
{
 struct watch_repair_gate *g=user;unsigned key=irq_lock();int ok=0;
 /* Read-only identity may begin before USB enumeration. Bind exactly once
  * at the recovery decision, before any recovery I/O, never at early boot and
  * never again after disconnect/reconnect during the repair. */
 if(g&&atomic_get(&usb_configured)&&watch_init_allowed(NULL)){
  if(!g->bound){g->usb_epoch=(uint32_t)atomic_get(&usb_epoch);g->bound=1;}
  ok=g->usb_epoch==(uint32_t)atomic_get(&usb_epoch);
 }
 irq_unlock(key);return ok;
}
static int watch_resume_claim(void)
{
 if(!atomic_get(&watch_deferred)||atomic_get(&watch_done)||atomic_get(&watch_cancel)||
    !atomic_get(&usb_configured)||battery_restore.attempted||
    atomic_get(&capacity_used)||atomic_get(&trial_used)||atomic_get(&recovery_used)||
    !atomic_cas(&command_busy,0,1))return 0;
 if(!battery_control_lock()){atomic_clear(&command_busy);return 0;}
 if(!watch_init_allowed(NULL)||enrollment.active||pendant_button_read()!=0||mic_commands_reserve_external()){
  battery_control_unlock();atomic_clear(&command_busy);return 0;
 }
 /* No reader may copy service_initial while resumed initialization writes it. */
 atomic_clear(&watch_initialized);return 1;
}
#endif
static int watch_initialize(void)
{
#ifdef OPENPENDANT_GAUGE_POR_RECOVERY
 struct watch_repair_gate g={0};
 int rc=atomic_get(&watch_deferred)?
  battery_service_resume_recover(watch_init_allowed,watch_restore_allowed,&g,&battery_restore):
  battery_service_init_recover(watch_init_allowed,watch_restore_allowed,&g,&battery_restore);
 atomic_set(&watch_deferred,rc==-EAGAIN);
#else
 int rc=battery_service_init(watch_init_allowed,NULL);
#endif
 /* A failed physical STOP is not permission to reuse ownership. */
 if(battery_service_released())mic_commands_release_external();
 else atomic_set(&faulted,1);
 battery_control_unlock();atomic_clear(&command_busy);
 atomic_set(&watch_error,rc);atomic_set(&watch_initialized,1);
 if(rc
#ifdef OPENPENDANT_GAUGE_POR_RECOVERY
    &&rc!=-EAGAIN
#endif
 )atomic_set(&watch_done,1);
 return rc;
}
static int watch_poll(void)
{
 struct bp_sample next={0};int rc=battery_service_read(&next,watch_read_allowed,NULL);
 if(!battery_service_released())atomic_set(&faulted,1);
 unsigned key=irq_lock();
 if(!rc){
  watch_sample=next;++watch_samples;
  if(atomic_get(&usb_configured))++watch_usb_samples;else ++watch_battery_samples;
  if(!watch_min_mv||next.mv<watch_min_mv)watch_min_mv=next.mv;
  if(next.mv>watch_max_mv)watch_max_mv=next.mv;
 }else{watch_sample.valid=0;watch_sample.flags=next.flags;atomic_set(&watch_done,1);}
 atomic_set(&watch_error,rc);irq_unlock(key);return rc;
}
static void battery_watch_thread(void *a,void *b,void *c)
{
 ARG_UNUSED(a);ARG_UNUSED(b);ARG_UNUSED(c);
 while(!atomic_get(&watch_requested)){
#ifdef OPENPENDANT_PORTABLE_RECORDING
  if(watch_auto_start())break;
#endif
  k_msleep(250);
 }
 int rc=watch_initialize();
#ifdef OPENPENDANT_GAUGE_POR_RECOVERY
 while(rc==-EAGAIN){
  /* Idle wait performs no gauge/NAND I/O and holds no command/mic owner. */
  while(!watch_resume_claim()){
   if(atomic_get(&watch_cancel)||atomic_get(&faulted)||pendant_recovery_is_pending()){
    atomic_set(&watch_error,-ECANCELED);atomic_set(&watch_done,1);return;
   }
   k_msleep(250);
  }
  rc=watch_initialize();
 }
#endif
 if(rc)return;
 while(!watch_poll())k_msleep(250);
}
/* Outrank codec/command10 and storage12 so a mount cannot starve the bounded
 * TWIM completion check. The actor sleeps between transactions; it never
 * holds IRQs across bus I/O. Worst error path is25ms plus10ms STOP, then quits.
 * Audio performance under this priority still needs target qualification. */
K_THREAD_DEFINE(recording_battery_watch,2048,battery_watch_thread,NULL,NULL,NULL,9,0,0);
static int battery_watch_start(const struct shell *sh,size_t argc,char **argv)
{
 if(argc!=2||strcmp(argv[1],"confirm-read-only")||!local(sh)||!diagnostics_idle()||
    atomic_get(&watch_requested)||battery_result.attempted||battery_restore.attempted||
    battery_service_claimed()||!atomic_cas(&command_busy,0,1))return -EPERM;
 if(!battery_control_lock()){atomic_clear(&command_busy);return -EBUSY;}
 if(!diagnostics_idle()||pendant_button_read()!=0||mic_commands_reserve_external()){
  battery_control_unlock();atomic_clear(&command_busy);return -EBUSY;
 }
 /* Publish only after all exclusions; the actor releases these after init. */
 atomic_set(&watch_requested,1);
 shell_print(sh,"BATTERY_WATCH_START accepted=1 microphone=0 config_writes=0");return 0;
}
static int battery_watch_stop(const struct shell *sh,size_t argc,char **argv)
{
 ARG_UNUSED(argv);if(argc!=1||sh!=shell_backend_uart_get_ptr()||!atomic_get(&watch_requested))return -EPERM;
 atomic_set(&watch_cancel,1);shell_print(sh,"BATTERY_WATCH_STOP requested=1");return 0;
}
static int battery_watch_status(const struct shell *sh,size_t argc,char **argv)
{
 ARG_UNUSED(argv);if(argc!=1||sh!=shell_backend_uart_get_ptr())return -EPERM;
 unsigned key=irq_lock();struct bp_sample s=watch_sample;uint64_t t=now(NULL);
 uint32_t count=watch_samples,on=watch_usb_samples,off=watch_battery_samples;
 uint16_t low=watch_min_mv,high=watch_max_mv;irq_unlock(key);
 unsigned config_writes=atomic_get(&watch_initialized)&&battery_restore.mutated?battery_restore.writes:0;
 /* Startup restoration writes are reported, not hidden as read-only polling.
  * Conservative count includes Control queries and block-selector writes. */
 shell_print(sh,"BATTERY_WATCH requested=%u initialized=%u done=%u rc=%d valid=%u sequence=%u age_ms=%llu mv=%u soc=%u temp=%u flags=%u samples=%u usb_samples=%u battery_samples=%u min_mv=%u max_mv=%u usb=%u config_writes=%u",
  (unsigned)atomic_get(&watch_requested),(unsigned)atomic_get(&watch_initialized),
  (unsigned)atomic_get(&watch_done),(int)atomic_get(&watch_error),(unsigned)bp_fresh(&s,t),s.sequence,
  (unsigned long long)(s.sequence&&t>=s.at?t-s.at:UINT32_MAX),s.mv,s.soc,s.temp,s.flags,
  count,on,off,low,high,(unsigned)atomic_get(&usb_configured),config_writes);return 0;
}
static int battery_watch_init_status(const struct shell *sh,size_t argc,char **argv)
{
 ARG_UNUSED(argv);if(argc!=1||sh!=shell_backend_uart_get_ptr())return -EPERM;
 struct battery_service_init_result d={0};
 /* Atomic publication after init returns; do not read the in-flight cache. */
 unsigned available=(unsigned)atomic_get(&watch_initialized);
 if(available)battery_service_init_status(&d);
 shell_print(sh,"BATTERY_INIT v=1 available=%u attempted=%u complete=%u stage=%u valid=%u rc=%d transfers=%u elapsed_ms=%u stopped=%u released=%u fault=%u config_writes=%u",
  available,d.attempted,d.complete,d.stage,d.valid,d.rc,d.transfers,d.elapsed_ms,d.stopped,d.released,d.fault,
  available&&battery_restore.mutated?battery_restore.writes:0);
 shell_print(sh,"BATTERY_INIT_VALUES type=%u fw=%u chem=%u status=%u flags=%u bus_io=0",
  d.type,d.fw,d.chem,d.status,d.flags);return 0;
}
