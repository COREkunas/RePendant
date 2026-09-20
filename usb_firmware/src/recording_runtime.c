/* SPDX-License-Identifier: Apache-2.0 */
#include "recording_runtime.h"
#include "recording_public_pattern.h"
#include "recording_configuration.h"
#include "recording_volume.h"
#include "recording_worker.h"
#include "recording_store_bridge.h"
#include "recording_capture.h"
#include "recording_fault_trace.h"
#include "recording_ble.h"
#include <zephyr/sys/byteorder.h>
#include "recording_catalog.h"
#include "recording_control_probe_nrf.h"
#include "recording_phy_probe.h"
#include "recording_current_preimage.h"
#include "dhara_trial.h"
#include "nand_capacity_probe.h"
#include "mic_commands.h"
#include "ble_security.h"
#include <errno.h>
#include <string.h>
#include <psa/crypto.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/irq.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_uart.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>
#include "recording_cpu_clock.h"
#ifdef OPENPENDANT_PORTABLE_RECORDING
#include "battery_power.h"
static atomic_t portable_owned;
static struct bp_lease portable_lease;
static int portable_admit(void),portable_begin(void),portable_check(void),portable_capture_ready(void);
static void portable_end(void);
#ifdef OPENPENDANT_BATTERY_SYNC
static atomic_t sync_power_owned;
static struct bp_lease sync_power_lease;
static uint64_t sync_power_work_until;
static int sync_power_ready(void),sync_power_begin(void),sync_power_check(void);
static void sync_power_end(void);
#endif
#endif
#ifdef OPENPENDANT_LONG_CONTROL
#include "recording_control_ble.h"
#endif

enum task { TASK_INIT=1,TASK_ENROLL,TASK_PROVISION,TASK_MOUNT,TASK_START,TASK_SYNC,TASK_RETIRE,TASK_CONTROL_PROBE,TASK_PHY_PROBE,TASK_PREIMAGE,TASK_RECOVER,TASK_DHARA_TRIAL,TASK_METADATA_EXTEND,TASK_CAPACITY_PROBE,TASK_FULL_PREPARE,TASK_FULL_FORMAT,TASK_FULL_COMPLETE };
struct command { uint32_t task,mode,usb_epoch,public_test;const struct shell *shell;uint8_t confirmation[32];
#ifdef OPENPENDANT_PORTABLE_RECORDING
 uint32_t portable;
#endif
#ifdef OPENPENDANT_LONG_CONTROL
 uint32_t control_ticket;
#endif
 struct owned_volume_spec spec;uint8_t recipient[65];uint64_t deadline;
 uint32_t sync_epoch;struct db_request request; };
static struct recording_volume volume;
static struct rsb_context bridge;
static struct ros_context *store;
static struct recording_sync_metadata *sync_metadata;
static struct es_binding volume_binding;
static struct recording_catalog catalog;
static psa_hash_operation_t catalog_stream=PSA_HASH_OPERATION_INIT;
static atomic_t full_storage;
int recording_runtime_full_storage(void){return atomic_get(&full_storage)!=0;}
static atomic_t sync_configured,catalog_epoch,retire_pending,sync_actor;
static uint8_t sync_response[DB_MAX_RESPONSE];
static size_t sync_response_bytes;
static atomic_t initialized,ready,faulted,command_busy,usb_configured,usb_epoch;
static atomic_t leased,recording,epoch,storage_running;
static uint32_t lease_usb_epoch;
static atomic_t volume_initialized,volume_mounted,volume_suspended,codec_initialized;
static struct rw_status worker_status;
static struct recording_capture_status capture_status;
static uint64_t guards[2];
static struct command active;
static atomic_t runtime_standby;
static uint32_t standby_entries,standby_wakes;
static void runtime_resume(void);
#ifdef OPENPENDANT_LONG_CONTROL
static struct recording_control long_control;
static struct lcw_owner long_worker;
/* Publish immutable boot/binding callbacks to main/GATT actors with acquire /
 * release semantics; those actors must never see a half-initialized owner. */
static atomic_int long_bound;
static void *long_auth_user;
static int (*long_authorized)(void*,uint64_t);
static int long_ready(void*),long_queue(void*,uint32_t,uint64_t),long_publish(void),long_failure(uint32_t),long_initialize(void);
static void long_terminal_publish(void);
#endif
static struct recording_fault_trace check_failure;
static uint32_t trace_job,trace_sequence,trace_job_started,trace_stage_started,trace_stage;
static struct recording_timing timing;
static uint32_t timing_job_start;
static struct recording_cpu_clock cpu_clock;
/* One diagnostic attempt per boot; permanent buffers survive every returned
 * failure. Only immutable 200-byte metadata records may leave this context. */
/* All peers have permanent one-use admission flags, even after clean close.
 * Never overwrite an ambiguous DMA lifetime by selecting a different peer. */
static union {struct control_probe control;struct phy_read_probe phy;struct current_preimage preimage;struct dhara_trial trial;struct nand_capacity_probe capacity;} diagnostic_context;
#define capacity_context diagnostic_context.capacity
static atomic_t capacity_used,capacity_running,capacity_owner;
#define trial_context diagnostic_context.trial
static atomic_t trial_used,trial_running,trial_owner;
static struct dt_result trial_result;
#define control_probe_context diagnostic_context.control
#define phy_probe_context diagnostic_context.phy
#define preimage_context diagnostic_context.preimage
static uint8_t control_probe_records[CP_ROWS][CP_RECORD_BYTES];
static struct cp_result control_probe_result={.stopped=1,.scrubbed=1,.last_row=UINT32_MAX};
static struct cp_nrf_fault control_probe_hal_fault;
static int control_probe_hal_rc;
static uint32_t control_probe_stored;
static atomic_t control_probe_used,control_probe_running,control_probe_expired,control_probe_owner;
static uint64_t control_probe_deadline;
static const uint8_t control_probe_device[16]={0x6a,0x5f,0x3e,0xaf,0xbb,0x97,0x51,0xfb,0xa9,0x93,0xe2,0x8d,0xcf,0x2b,0x87,0x10};
static const uint8_t control_probe_digest[32]={0x11,0xc7,0xcf,0x70,0x13,0xbd,0x8e,0xfe,0x7d,0x17,0x63,0xad,0xef,0xa0,0x60,0x62,0xc5,0xc8,0x37,0x29,0xf3,0x15,0x80,0x5b,0xfc,0xca,0xda,0x49,0x4a,0x87,0xaf,0x8b};
// PHY_READ_PROBE_BEGIN globals
BUILD_ASSERT(CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC==32768,"PHY probe reports RTC ticks only");
static struct prp_result phy_probe_result={.stopped=1,.output_scrubbed=1,.last_row=UINT32_MAX};
static atomic_t phy_probe_used,phy_probe_running,phy_probe_expired,phy_probe_owner;
static uint64_t phy_probe_deadline;
// PHY_READ_PROBE_END globals
// CURRENT_PREIMAGE_BEGIN globals
static struct cpi_result preimage_result={.stopped=1,.index=UINT32_MAX,.row=UINT32_MAX};
static atomic_t preimage_used,preimage_running,preimage_expired,preimage_owner,preimage_initialized;
static uint32_t preimage_exported;
static uint8_t preimage_last_nonce[16];
static uint64_t preimage_deadline,preimage_export_last;
// CURRENT_PREIMAGE_END globals
#include "recovery_state.inc"
static int hp_runtime_bind(void);
K_MSGQ_DEFINE(runtime_commands,sizeof(struct command),1,4);
K_SEM_DEFINE(codec_wake,0,1);
K_SEM_DEFINE(storage_wake,0,1);
K_SEM_DEFINE(command_dispatch,0,1);
K_MUTEX_DEFINE(enrollment_lock);
static struct {uint32_t active,has_point,usb_epoch;uint64_t deadline;uint8_t nonce[16];
 struct owned_volume_spec spec;uint8_t point[65];} enrollment;

static uint64_t now(void *u){ARG_UNUSED(u);return (uint64_t)k_uptime_get();}
static _Noreturn void fatal_at(void *u,int reason,unsigned line)
{
 ARG_UNUSED(u);(void)irq_lock();atomic_set(&faulted,1);
 struct recording_fault_trace trace=check_failure;
 trace.line=line;trace.reason=(uint32_t)reason;trace.uptime=(uint32_t)now(NULL);trace.task=active.task;
 trace.bridge_state=bridge.state;trace.bridge_error=(uint32_t)bridge.error;
 trace.store_job=volume.store.status.job;trace.store_stage=volume.store.status.stage;
 trace.store_fault=volume.store.status.fault;trace.volume_state=volume.state;trace.volume_fault=volume.fault;
 trace.storage_running=(uint32_t)atomic_get(&storage_running);
 trace.recording_job=trace_job;trace.segment_sequence=trace_sequence;trace.job_started=trace_job_started;
 trace.stage_started=trace_stage_started;trace.observed_stage=trace_stage;
 trace.timing=timing; /* completed calls only; an in-flight call is not guessed */
#ifdef OPENPENDANT_NATIVE_STORAGE
 trace.native_line=volume.native.fault_line;
 struct nand_owned_phy *p=&volume.native.phy;struct nop_nrf_diagnostics h;
 nand_owned_phy_nrf_diagnostics(&h);
 trace.phy_line=p->fault_line;trace.phy_fault=p->fault;trace.opcode=p->last_opcode;
 trace.row=p->last_row;trace.status=p->status;trace.stopped=p->stopped;trace.ready=p->ready_known;
 trace.verify_mismatch=p->verify_mismatch;trace.verified=p->verified_programs;trace.wel=p->wel_observed;
 trace.native_deadline=(uint32_t)volume.native.deadline;trace.phy_deadline=(uint32_t)p->deadline;trace.phy_now=(uint32_t)p->last_now;
 trace.hal_valid=h.valid;trace.hal_rc=(uint32_t)h.rc;trace.hal_opcode=h.opcode;trace.hal_bytes=h.bytes;
 trace.hal_started=h.started;trace.hal_stopped=h.stopped;trace.hal_tx=h.tx_amount;trace.hal_rx=h.rx_amount;
 trace.hal_before=(uint32_t)h.before_ms;trace.hal_after=(uint32_t)h.after_ms;trace.hal_deadline=(uint32_t)h.deadline;
 trace.late_end=h.late_end;trace.late_stop=h.late_stop;
#endif
 recording_fault_save(&trace);sys_reboot(SYS_REBOOT_COLD);for(;;){}
}
static _Noreturn void fatal(void *u,int reason){fatal_at(u,reason,0);}
#define fatal(u,reason) fatal_at(u,reason,__LINE__)
static int guard_set(unsigned index,uint64_t deadline)
{
 unsigned key=irq_lock();uint64_t t=now(NULL);
 if(index>1||guards[index]||deadline<=t){irq_unlock(key);return -EPERM;}
 runtime_resume();
 guards[index]=deadline;irq_unlock(key);return 0;
}
static int guard_clear(unsigned index)
{
 unsigned key=irq_lock();uint64_t t=now(NULL);
 if(index>1||!guards[index]||t>=guards[index]){irq_unlock(key);return -EPERM;}
 guards[index]=0;irq_unlock(key);return 0;
}
static int guarded(uint64_t deadline)
{
 unsigned key=irq_lock();uint64_t t=now(NULL);
 int good=deadline>t&&((guards[0]>=deadline&&guards[0]>t)||(guards[1]>=deadline&&guards[1]>t));
 irq_unlock(key);return good;
}
static void supervise(struct k_timer *timer)
{
 ARG_UNUSED(timer);uint64_t t=now(NULL);
 /* Diagnostic timeout only latches a refusal of future STARTs. It cannot
  * preempt a hung HAL/PSA call, and never resets away its failure evidence. */
 if(atomic_get(&control_probe_running)&&t>=control_probe_deadline)atomic_set(&control_probe_expired,1);
 if(atomic_get(&phy_probe_running)&&t>=phy_probe_deadline)atomic_set(&phy_probe_expired,1);
 if(atomic_get(&preimage_running)&&t>=preimage_deadline)atomic_set(&preimage_expired,1);
 if(atomic_get(&recovery_running)&&t>=recovery_deadline)atomic_set(&recovery_expired,1);
 if((guards[0]&&t>=guards[0])||(guards[1]&&t>=guards[1]))fatal(NULL,-ETIMEDOUT);
}
K_TIMER_DEFINE(runtime_supervisor,supervise,NULL);
static void runtime_resume(void)
{
 /* Called under IRQ lock or by the sole codec actor before executing work. */
 if(atomic_cas(&runtime_standby,1,0)){
  ++standby_wakes;k_timer_start(&runtime_supervisor,K_MSEC(10),K_MSEC(10));
 }
}
int recording_runtime_standby(int requested)
{
 /* Scheduling only: battery monitor and Bluetooth retain their own timers.
  * Never suspend an active deadline or rely on an idle snapshot for ownership. */
 unsigned key=irq_lock();
 int idle=requested&&atomic_get(&initialized)&&atomic_get(&ready)&&!atomic_get(&faulted)&&
  !atomic_get(&command_busy)&&!atomic_get(&leased)&&!atomic_get(&recording)&&!atomic_get(&storage_running)&&
  !atomic_get(&catalog_epoch)&&!atomic_get(&retire_pending)&&!atomic_get(&sync_actor)&&
  !guards[0]&&!guards[1]&&!enrollment.active&&!mic_commands_busy()&&
  !pendant_ble_pairing_busy()&&!pendant_recovery_is_pending()&&
  !atomic_get(&control_probe_running)&&!atomic_get(&phy_probe_running)&&!atomic_get(&preimage_running)&&
  !atomic_get(&recovery_running)&&!atomic_get(&trial_running)&&!atomic_get(&capacity_running);
#ifdef OPENPENDANT_LONG_CONTROL
 int held=idle&&atomic_load(&long_bound)&&lrc_maintenance_claim(&long_control);
 idle=idle&&held;
#endif
 if(idle){if(atomic_cas(&runtime_standby,0,1)){++standby_entries;k_timer_stop(&runtime_supervisor);}}
 else runtime_resume();
#ifdef OPENPENDANT_LONG_CONTROL
 if(held)lrc_maintenance_release(&long_control);
#endif
 irq_unlock(key);return idle;
}
/* Explicit generated-data test: same real codec/storage actors, NEVER DMIC.
 * Timer submission is bounded, lock-free and single-core. Stop joins the ISR
 * before the codec can release ownership. No retry of a refused frame. */
BUILD_ASSERT(!IS_ENABLED(CONFIG_SMP),"Generated producer requires single core");
static atomic_t generated_live,generated_frames,generated_error;
static int16_t generated_pcm[320];
static void generated_tick(struct k_timer *timer)
{
 if(!atomic_get(&generated_live))return;
 uint32_t frames=(uint32_t)atomic_get(&generated_frames);
 /* Nonzero public broadband/tonal load: silence exercises Opus's cheap path
  * and is not representative of concurrent voice encoding plus storage. */
 recording_public_pattern(frames,generated_pcm);
 int rc=rw_submit((uint32_t)atomic_get(&epoch),(uint64_t)frames*320U,generated_pcm);
 if(rc==RW_OK)atomic_inc(&generated_frames);else atomic_set(&generated_error,rc);
 if(rc!=RW_OK||frames+1U==active.public_test*50U){
  atomic_clear(&generated_live);k_timer_stop(timer);
  (void)rw_request_stop((uint32_t)atomic_get(&epoch),rc==RW_OK?RW_USER:RW_EXTERNAL);
 }
 k_sem_give(&codec_wake);
}
K_TIMER_DEFINE(generated_timer,generated_tick,NULL);
void recording_runtime_usb_status(enum usb_dc_status_code status,const uint8_t *param)
{
 ARG_UNUSED(param);
 if(status==USB_DC_CONFIGURED){atomic_inc(&usb_epoch);atomic_set(&usb_configured,1);}
 else if(status==USB_DC_DISCONNECTED||status==USB_DC_RESET||status==USB_DC_SUSPEND||status==USB_DC_ERROR){
  atomic_clear(&usb_configured);atomic_inc(&usb_epoch);
  if(atomic_get(&recording)
#ifdef OPENPENDANT_PORTABLE_RECORDING
     &&!atomic_get(&portable_owned)
#endif
  ){(void)rw_request_stop((uint32_t)atomic_get(&epoch),RW_EXTERNAL);k_sem_give(&codec_wake);}
 }
}
static int check_reject(uint32_t code,uint64_t deadline)
{
 unsigned key=irq_lock();
 check_failure.check_code=code;check_failure.check_deadline=(uint32_t)deadline;
 check_failure.check_now=(uint32_t)now(NULL);check_failure.guard_codec=(uint32_t)guards[0];
 check_failure.guard_storage=(uint32_t)guards[1];check_failure.usb_epoch=(uint32_t)atomic_get(&usb_epoch);
 check_failure.active_epoch=active.usb_epoch;check_failure.lease_epoch=lease_usb_epoch;
 check_failure.usb_configured=(uint32_t)atomic_get(&usb_configured);
 irq_unlock(key);return -EPERM;
}
static int check(void *u,int writing,uint64_t deadline)
{
 ARG_UNUSED(u);ARG_UNUSED(writing);
 if(atomic_get(&faulted))return check_reject(1,deadline);
 if(!guarded(deadline))return check_reject(2,deadline);
 if(pendant_recovery_is_pending())return check_reject(4,deadline);
#ifdef OPENPENDANT_PORTABLE_RECORDING
 if(atomic_get(&portable_owned)){
  if(active.task!=TASK_START||!active.portable||!atomic_get(&command_busy)||!portable_check())return check_reject(9,deadline);
 }else
#endif
 {
#ifdef OPENPENDANT_BATTERY_SYNC
 if(atomic_get(&sync_power_owned)){
  if((active.task!=TASK_SYNC&&active.task!=TASK_RETIRE)||!atomic_get(&command_busy)||
     active.sync_epoch!=(uint32_t)atomic_get(&catalog_epoch)||!sync_power_check())return check_reject(10,deadline);
 }else
#endif
 {
 if(!atomic_get(&usb_configured))return check_reject(3,deadline);
 /* A reconnect before acquisition cannot adopt an already queued request. */
 if(atomic_get(&command_busy)&&active.task!=TASK_INIT&&active.usb_epoch!=(uint32_t)atomic_get(&usb_epoch))return check_reject(5,deadline);
 if(atomic_get(&leased)&&lease_usb_epoch!=(uint32_t)atomic_get(&usb_epoch))return check_reject(6,deadline);
 }
 }
 if(active.task==TASK_SYNC&&(!atomic_get(&command_busy)||deadline>active.deadline||
    active.sync_epoch!=(uint32_t)atomic_get(&catalog_epoch)||
    !recording_ble_admitted(active.sync_epoch,&active.request,active.deadline)))return check_reject(7,deadline);
 if(active.task==TASK_RETIRE&&(!atomic_get(&command_busy)||atomic_get(&sync_actor)||
    active.sync_epoch!=(uint32_t)atomic_get(&catalog_epoch)||
    active.sync_epoch!=(uint32_t)atomic_get(&retire_pending)))return check_reject(8,deadline);
 return 0;
}
static int acquire(void *u,uint64_t deadline)
{
 if(check(u,0,deadline)||atomic_get(&leased)||mic_commands_reserve_external())return -EBUSY;
 lease_usb_epoch=(uint32_t)atomic_get(&usb_epoch);atomic_set(&leased,1);
 if(check(u,0,deadline)){atomic_clear(&leased);mic_commands_release_external();return -EPERM;}
 return 0;
}
static int release(void *u,uint64_t deadline)
{
 if(check(u,0,deadline)||!atomic_get(&leased)||atomic_get(&recording)||atomic_get(&storage_running))return -EPERM;
 atomic_clear(&leased);mic_commands_release_external();
 /* A new legitimate owner may immediately acquire after our release. Its busy
  * flag is not evidence that this exact tracked external reservation failed. */
 return 0;
}
static int power(void *u,uint32_t access,uint32_t row,uint64_t d)
{ARG_UNUSED(access);ARG_UNUSED(row);return check(u,1,d);}
static int yield_job(void *u,uint64_t d){if(check(u,0,d))return -EPERM;
#ifdef OPENPENDANT_NATIVE_STORAGE
 static uint32_t reported;
 if(active.task==TASK_FULL_FORMAT&&active.shell&&volume.native.format_next>=reported+64U){
  reported=volume.native.format_next;shell_print(active.shell,"FULL_FORMAT_PROGRESS checked_blocks=%u total=2048 microphone=0",reported);
 }
#endif
 k_yield();return check(u,0,d);}
static int confirmation(void *u,const struct recording_configuration *cfg,const uint8_t value[32],uint64_t d)
{
 ARG_UNUSED(u);
 return check(NULL,1,d)||(active.task!=TASK_PROVISION&&active.task!=TASK_FULL_FORMAT&&active.task!=TASK_FULL_COMPLETE)||!atomic_get(&command_busy)||
 active.usb_epoch!=(uint32_t)atomic_get(&usb_epoch)||memcmp(active.confirmation,value,32)||
#ifdef OPENPENDANT_NATIVE_STORAGE
 (active.task==TASK_FULL_COMPLETE?(!rcfg_is_full(cfg)||cfg->phase!=RCFG_PROVISIONING):active.task==TASK_FULL_FORMAT?(!rcfg_is_full(cfg)||cfg->phase!=RCFG_PREPARED):
  (rcfg_is_full(cfg)||cfg->phase!=RCFG_PROVISIONING||active.mode!=1))?-EPERM:0;
#else
 cfg->phase!=RCFG_PREPARED?-EPERM:0;
#endif
}
static int catalog_admit(void *u,uint64_t d)
{return check(u,0,d)||active.task!=TASK_SYNC||!atomic_get(&sync_actor)||
 !atomic_get(&leased)||atomic_get(&recording)||atomic_get(&storage_running)?-EPERM:0;}
static int catalog_yield(void *u,uint64_t d)
{if(catalog_admit(u,d))return -EPERM;k_yield();return catalog_admit(u,d);}
static int catalog_retire_admit(void *u,uint64_t d)
{return check(u,0,d)||active.task!=TASK_RETIRE||!atomic_get(&leased)||
 atomic_get(&recording)||atomic_get(&storage_running)||atomic_get(&sync_actor)?-EPERM:0;}
static int catalog_hash(void *u,const uint8_t *p,size_t n,uint8_t *out,size_t cap,size_t *actual)
{ARG_UNUSED(u);return psa_hash_compute(PSA_ALG_SHA_256,p,n,out,cap,actual)==PSA_SUCCESS?0:-EIO;}
static int probe_configuration(struct recording_configuration *cfg)
{
 return rcfg_get(cfg)||cfg->phase!=RCFG_PROVISIONING||cfg->revision!=2||cfg->fault_banks||
  memcmp(cfg->spec.device_id,control_probe_device,16)||memcmp(cfg->descriptor+352,control_probe_digest,32)?-EPERM:0;
}
static int probe_check(void *u,const uint8_t device[16],const uint8_t digest[32],uint64_t d)
{
 ARG_UNUSED(u);struct recording_configuration cfg;
 if(!atomic_get(&control_probe_running)||!atomic_get(&command_busy)||active.task!=TASK_CONTROL_PROBE||
  atomic_get(&control_probe_expired)||d>control_probe_deadline||now(NULL)>=d||
  !atomic_get(&usb_configured)||active.usb_epoch!=(uint32_t)atomic_get(&usb_epoch)||
  atomic_get(&faulted)||atomic_get(&recording)||atomic_get(&storage_running)||atomic_get(&catalog_epoch)||
  atomic_get(&volume_initialized)||pendant_recovery_is_pending()||pendant_ble_pairing_busy()||
  memcmp(device,control_probe_device,16)||memcmp(digest,control_probe_digest,32)||probe_configuration(&cfg))return -EPERM;
 return 0;
}
static int probe_acquire(void *u,uint64_t d)
{
 if(probe_check(u,control_probe_device,control_probe_digest,d)||atomic_get(&leased)||
  atomic_get(&control_probe_owner)||mic_commands_reserve_external())return -EBUSY;
 lease_usb_epoch=(uint32_t)atomic_get(&usb_epoch);atomic_set(&leased,1);atomic_set(&control_probe_owner,1);
 /* A late lifecycle failure retains the reservation. Only the dedicated HAL
  * can subsequently prove release; never infer a partially opened bus safe. */
 return probe_check(u,control_probe_device,control_probe_digest,d);
}
static int probe_release(void *u,uint64_t d)
{
 ARG_UNUSED(u);
 /* STOP/idle/controller cleanup is already proved by the dedicated HAL.
  * Cleanup does not renew/check expired data or USB-session authority. */
 if(!atomic_get(&control_probe_running)||active.task!=TASK_CONTROL_PROBE||!atomic_get(&command_busy)||
  !atomic_get(&leased)||atomic_get(&recording)||atomic_get(&storage_running)||now(NULL)>=d||
  !atomic_cas(&control_probe_owner,1,0))return -EPERM;
 atomic_clear(&leased);mic_commands_release_external();return 0;
}
static uint32_t probe_u32(const uint8_t *p)
{return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static int probe_record(void *u,const uint8_t p[CP_RECORD_BYTES],uint64_t d)
{
 if(!p||d>control_probe_deadline||now(NULL)>=d||probe_check(u,control_probe_device,control_probe_digest,control_probe_deadline)||
  !atomic_get(&control_probe_owner)||!atomic_get(&leased)||control_probe_stored>=CP_ROWS||
  memcmp(p,"OPNDCP1\0",8)||probe_u32(p+8)!=1||probe_u32(p+12)!=control_probe_stored||
  probe_u32(p+16)!=control_probe_stored/64U||probe_u32(p+20)!=control_probe_stored%64U||
  probe_u32(p+24)!=67648U+control_probe_stored)return -EPERM;
 memcpy(control_probe_records[control_probe_stored],p,CP_RECORD_BYTES);
 if(now(NULL)>=d||probe_check(u,control_probe_device,control_probe_digest,control_probe_deadline))return -EPERM;
 ++control_probe_stored;return 0;
}
static const struct cp_nrf_owner probe_owner_hooks={NULL,probe_acquire,probe_release,probe_check,probe_record};
static void execute_control_probe(void)
{
 struct recording_configuration cfg;uint8_t device[16];struct cp_port port;
 const struct owned_page_hash hash={NULL,catalog_hash};
 uint64_t started=now(NULL);int rc=CP_ADMISSION;
 unsigned key=irq_lock();control_probe_deadline=active.deadline;atomic_set(&control_probe_running,1);irq_unlock(key);
 control_probe_result.attempted=1;
 if(started>=active.deadline||atomic_get(&control_probe_expired)){rc=CP_TIME;goto finished;}
 if(probe_configuration(&cfg)||rcfg_device_id(device)||memcmp(device,control_probe_device,16)||
  probe_check(NULL,control_probe_device,control_probe_digest,active.deadline)||
  cp_nrf_bind(&control_probe_context,&probe_owner_hooks,&port))goto finished;
 rc=control_probe_run(&control_probe_context,cfg.descriptor,&cfg.spec,&hash,&port,active.deadline,&control_probe_result);
 control_probe_hal_rc=cp_nrf_get_fault(&control_probe_hal_fault);
 if(control_probe_hal_rc)memset(&control_probe_hal_fault,0,sizeof(control_probe_hal_fault));
finished:
 control_probe_result.rc=rc;
 if(rc){control_probe_result.complete=0;atomic_set(&faulted,1);}
 if(!control_probe_context.used)control_probe_result.elapsed_ms=now(NULL)>=started?now(NULL)-started:0;
 /* Unlike ordinary storage errors this diagnostic keeps all returned evidence
  * and any uncertain reservation. No fatal/reset, automatic retry, or reuse. */
 atomic_clear(&control_probe_running);
 if(active.shell)shell_print(active.shell,"RECORDER_DONE task=8 rc=%d microphone=0",rc);
 memset(&active,0,sizeof(active));atomic_clear(&command_busy);
}
// PHY_READ_PROBE_BEGIN helpers
static int phy_probe_check(void *u,uint64_t d)
{
 ARG_UNUSED(u);struct recording_configuration cfg;
 return !atomic_get(&phy_probe_running)||!atomic_get(&command_busy)||active.task!=TASK_PHY_PROBE||
  atomic_get(&phy_probe_expired)||d!=phy_probe_deadline||now(NULL)>=d||
  !atomic_get(&usb_configured)||active.usb_epoch!=(uint32_t)atomic_get(&usb_epoch)||
  atomic_get(&faulted)||atomic_get(&recording)||atomic_get(&storage_running)||atomic_get(&catalog_epoch)||
  atomic_get(&volume_initialized)||atomic_get(&control_probe_used)||pendant_recovery_is_pending()||
  pendant_ble_pairing_busy()||probe_configuration(&cfg)?-EPERM:0;
}
static uint32_t phy_probe_cycles(void *u){ARG_UNUSED(u);return k_cycle_get_32();}
static int phy_probe_acquire(void *u,uint64_t d)
{
 if(phy_probe_check(u,d)||atomic_get(&leased)||atomic_get(&phy_probe_owner)||mic_commands_reserve_external())return -EBUSY;
 lease_usb_epoch=(uint32_t)atomic_get(&usb_epoch);atomic_set(&leased,1);atomic_set(&phy_probe_owner,1);
 return phy_probe_check(u,d); /* Late refusal conservatively retains owner. */
}
static int phy_probe_release(void *u,uint64_t d)
{
 if(phy_probe_check(u,d)||!atomic_get(&leased)||!atomic_cas(&phy_probe_owner,1,0))return -EPERM;
 atomic_clear(&leased);mic_commands_release_external();return 0;
}
static int phy_probe_authorize(void *u,uint32_t access,uint32_t row,uint64_t d)
{return !atomic_get(&phy_probe_owner)||!atomic_get(&leased)||access!=NOP_READ||
 (row!=67652U&&row!=67653U)||phy_probe_check(u,d)?-EPERM:0;}
static const struct nop_nrf_owner phy_probe_hal_owner={NULL,phy_probe_acquire,phy_probe_release,phy_probe_authorize};
static const struct prp_owner phy_probe_hooks={NULL,phy_probe_check,phy_probe_cycles,32768};
static int trial_check(void *u,uint64_t d)
{
 ARG_UNUSED(u);struct recording_configuration cfg;
 return !atomic_get(&trial_running)||!atomic_get(&command_busy)||active.task!=TASK_DHARA_TRIAL||
  d!=active.deadline||now(NULL)>=d||!atomic_get(&usb_configured)||
  active.usb_epoch!=(uint32_t)atomic_get(&usb_epoch)||atomic_get(&faulted)||
  atomic_get(&recording)||atomic_get(&storage_running)||atomic_get(&catalog_epoch)||
  atomic_get(&volume_initialized)||pendant_recovery_is_pending()||pendant_ble_pairing_busy()||
  probe_configuration(&cfg)?-EPERM:0;
}
static int trial_acquire(void *u,uint64_t d)
{
 if(trial_check(u,d)||atomic_get(&leased)||atomic_get(&trial_owner)||mic_commands_reserve_external())return -EBUSY;
 lease_usb_epoch=(uint32_t)atomic_get(&usb_epoch);atomic_set(&leased,1);atomic_set(&trial_owner,1);
 return trial_check(u,d);
}
static int trial_release(void *u,uint64_t d)
{
 if(trial_check(u,d)||!atomic_get(&leased)||!atomic_cas(&trial_owner,1,0))return -EPERM;
 atomic_clear(&leased);mic_commands_release_external();return 0;
}
static int trial_authorize(void *u,uint32_t access,uint32_t row,uint64_t d)
{
 return trial_check(u,d)||!atomic_get(&trial_owner)||!atomic_get(&leased)||
  row<DT_FIRST_BLOCK*64U||row>=(DT_FIRST_BLOCK+DT_BLOCKS)*64U||
  access<NOP_READ||access>NOP_ERASE||(active.mode&&access!=NOP_READ)?-EPERM:0;
}
static const struct nop_nrf_owner trial_hal_owner={NULL,trial_acquire,trial_release,trial_authorize};
#include "capacity_probe_helpers.inc"
static void execute_trial(void)
{
 struct recording_configuration cfg;struct owned_volume_decoded decoded;struct nop_port port;
 const struct owned_page_hash hash={NULL,catalog_hash};int rc=-EPERM;
 atomic_set(&trial_running,1);
 if(!probe_configuration(&cfg)&&!trial_check(NULL,active.deadline)&&
    !owned_volume_descriptor_validate(cfg.descriptor,512,&cfg.spec,&hash,&decoded)&&
    !nand_owned_phy_nrf_bind(&port,&trial_context.phy,&trial_hal_owner))
  rc=dhara_trial_run(&trial_context,&decoded,&port,active.deadline,(int)active.mode);
 trial_result=trial_context.result;trial_result.rc=rc;
 if(rc)atomic_set(&faulted,1);
 atomic_clear(&trial_running);
 if(active.shell)shell_print(active.shell,"RECORDER_DONE task=12 rc=%d microphone=0",rc);
 memset(&active,0,sizeof(active));atomic_clear(&command_busy);
}
static void execute_phy_probe(void)
{
 struct recording_configuration cfg;struct owned_volume_decoded decoded;struct nop_port port;uint8_t device[16];
 const struct owned_page_hash hash={NULL,catalog_hash};uint64_t started=now(NULL);int rc=PRP_ADMISSION;
 unsigned key=irq_lock();phy_probe_deadline=active.deadline;atomic_set(&phy_probe_running,1);irq_unlock(key);
 phy_probe_result.attempted=1;
 if(started>=active.deadline||atomic_get(&phy_probe_expired)){rc=PRP_TIME;goto finished;}
 if(probe_configuration(&cfg)||rcfg_device_id(device)||memcmp(device,control_probe_device,16)||
  phy_probe_check(NULL,active.deadline)||owned_volume_descriptor_validate(cfg.descriptor,512,&cfg.spec,&hash,&decoded)||
  nand_owned_phy_nrf_bind(&port,&phy_probe_context.phy,&phy_probe_hal_owner))goto finished;
 rc=phy_read_probe_run(&phy_probe_context,&decoded,&port,&phy_probe_hooks,active.deadline,&phy_probe_result);
finished:
 phy_probe_result.rc=rc;
 if(rc){phy_probe_result.complete=0;atomic_set(&faulted,1);}
 if(!phy_probe_context.used)phy_probe_result.elapsed_ms=now(NULL)>=started?now(NULL)-started:0;
 atomic_clear(&phy_probe_running);
 if(active.shell)shell_print(active.shell,"RECORDER_DONE task=9 rc=%d microphone=0",rc);
 memset(&active,0,sizeof(active));atomic_clear(&command_busy);
}
// PHY_READ_PROBE_END helpers
// CURRENT_PREIMAGE_BEGIN helpers
static void hex(const uint8_t *p,size_t n,char *out);
static void preimage_wipe(void *p,size_t n){volatile uint8_t *b=p;while(n--)*b++=0;}
static int preimage_lifecycle(void)
{
 struct recording_configuration cfg;
 return !atomic_get(&command_busy)||active.task!=TASK_PREIMAGE||!atomic_get(&preimage_used)||
  !atomic_get(&usb_configured)||active.usb_epoch!=(uint32_t)atomic_get(&usb_epoch)||
  atomic_get(&faulted)||atomic_get(&recording)||atomic_get(&storage_running)||atomic_get(&catalog_epoch)||
  atomic_get(&volume_initialized)||atomic_get(&control_probe_used)||atomic_get(&phy_probe_used)||
  pendant_recovery_is_pending()||pendant_ble_pairing_busy()||probe_configuration(&cfg)?-EPERM:0;
}
static int preimage_check(void *u,uint64_t d)
{
 ARG_UNUSED(u);
 return !atomic_get(&preimage_running)||atomic_get(&preimage_expired)||d!=preimage_deadline||
  now(NULL)>=d||preimage_lifecycle()?-EPERM:0;
}
static int preimage_acquire(void *u,uint64_t d)
{
 if(preimage_check(u,d)||atomic_get(&leased)||atomic_get(&preimage_owner)||mic_commands_reserve_external())return -EBUSY;
 lease_usb_epoch=(uint32_t)atomic_get(&usb_epoch);atomic_set(&leased,1);atomic_set(&preimage_owner,1);
 return preimage_check(u,d);
}
static int preimage_release(void *u,uint64_t d)
{
 if(preimage_check(u,d)||!atomic_get(&leased)||!atomic_cas(&preimage_owner,1,0))return -EPERM;
 atomic_clear(&leased);mic_commands_release_external();return 0;
}
static int preimage_authorize(void *u,uint32_t access,uint32_t row,uint64_t d)
{
 return !atomic_get(&preimage_owner)||!atomic_get(&leased)||access!=NOP_READ||
  active.mode>=CPI_PAGES||row!=65600U+active.mode||preimage_check(u,d)?-EPERM:0;
}
static int preimage_hash(void *u,const uint8_t *p,size_t n,uint8_t digest[32],size_t *actual)
{ARG_UNUSED(u);if(n!=CPI_BYTES)return -EINVAL;return catalog_hash(NULL,p,n,digest,32,actual);}
static const struct nop_nrf_owner preimage_hal_owner={NULL,preimage_acquire,preimage_release,preimage_authorize};
static const struct cpi_owner preimage_hooks={NULL,preimage_check,phy_probe_cycles,32768,preimage_hash};
static int preimage_export_check(uint64_t d)
{
 /* Hardware is closed. Shell output is not preemptible/host-acknowledged. */
 uint64_t t=now(NULL);
 if(t<preimage_export_last||t>=d||preimage_lifecycle()||atomic_get(&leased)||
  atomic_get(&preimage_owner)||!preimage_result.complete||!preimage_result.released||
  !preimage_result.stopped||!preimage_result.ready||preimage_result.retained)return -EPERM;
 preimage_export_last=t;return 0;
}
static void execute_preimage(void)
{
 struct recording_configuration cfg;struct owned_volume_decoded decoded;struct nop_port port;uint8_t device[16];
 uint8_t bytes[CPI_BYTES];char token[33],digest[65],chunk[129];
 const struct owned_page_hash hash={NULL,catalog_hash};uint64_t started=now(NULL);int rc=CPI_ADMISSION;
 unsigned key=irq_lock();preimage_deadline=active.deadline;atomic_set(&preimage_running,1);irq_unlock(key);
 memset(&preimage_result,0,sizeof(preimage_result));preimage_result.attempted=1;
 preimage_result.index=active.mode;preimage_result.row=65600U+active.mode;
 if(started>=active.deadline||atomic_get(&preimage_expired)){rc=CPI_TIME;goto finished;}
 if(preimage_check(NULL,active.deadline))goto finished;
 if(!atomic_get(&preimage_initialized)){
  if(active.mode||probe_configuration(&cfg)||rcfg_device_id(device)||memcmp(device,control_probe_device,16)||
   owned_volume_descriptor_validate(cfg.descriptor,512,&cfg.spec,&hash,&decoded)||
   nand_owned_phy_nrf_bind(&port,&preimage_context.phy,&preimage_hal_owner)||
   current_preimage_init(&preimage_context,&decoded,&port,&preimage_hooks))goto finished;
  atomic_set(&preimage_initialized,1);
 }
 rc=current_preimage_capture(&preimage_context,active.mode,active.deadline,&preimage_result);
 if(rc)goto finished;
 if(preimage_result.index!=active.mode||preimage_result.row!=65600U+active.mode||
  preimage_result.reads!=2||preimage_result.off_starts!=2||preimage_result.on_starts!=2||
  preimage_result.starts<38||preimage_result.starts>803||!preimage_result.matched||!preimage_result.hash_valid||
  preimage_result.a0!=124||preimage_result.b0!=16){rc=CPI_SHAPE;goto finished;}
 rc=current_preimage_take(&preimage_context,active.mode,bytes);if(rc)goto finished;
 atomic_clear(&preimage_running); /* Original hardware work has joined. */
 uint64_t export_end=active.deadline+6000U; /* Original submission+8s, never renewed. */
 preimage_export_last=preimage_context.last_now;
 hex(active.confirmation,16,token);hex(preimage_result.sha256,32,digest);
 rc=preimage_export_check(export_end);if(rc)goto finished;
 shell_print(active.shell,"PIMG_BEGIN protocol=1 token=%s index=%u row=%u bytes=4352 copies=2 sha256=%s",
  token,active.mode,preimage_result.row,digest);
 rc=preimage_export_check(export_end);if(rc)goto finished;
 for(unsigned i=0;i<68;++i){
  rc=preimage_export_check(export_end);if(rc)goto finished;hex(bytes+64U*i,64,chunk);
  shell_print(active.shell,"PIMG_DATA token=%s chunk=%u hex=%s",token,i,chunk);
  rc=preimage_export_check(export_end);if(rc)goto finished;
 }
 shell_print(active.shell,"PIMG_END token=%s starts=%u stopped=1 released=1 matched=1 a0=124 b0=16 array_writes=0 sha256=%s",
  token,preimage_result.starts,digest);
 rc=preimage_export_check(export_end);if(rc)goto finished;++preimage_exported;
finished:
 preimage_wipe(bytes,sizeof(bytes));preimage_wipe(chunk,sizeof(chunk));
 preimage_result.rc=rc;if(rc){atomic_set(&faulted,1);preimage_context.fault=1;preimage_context.available=0;
  preimage_wipe(preimage_context.first,CPI_BYTES);preimage_wipe(preimage_context.second,CPI_BYTES);}
 if(!atomic_get(&preimage_initialized))preimage_result.elapsed_ms=now(NULL)>=started?now(NULL)-started:0;
 atomic_clear(&preimage_running);
 if(active.shell)shell_print(active.shell,"RECORDER_DONE task=10 rc=%d microphone=0",rc);
 memset(&active,0,sizeof(active));atomic_clear(&command_busy);
}
// CURRENT_PREIMAGE_END helpers
static int approve_receipt(void *u,const struct rsm_volume *v,const struct rsm_receipt *r,uint64_t d)
{
 if(active.request.command==DB_FULL_RECEIVE_RANGE){
  if(!recording_runtime_full_storage()||!r||catalog_admit(u,d)||
   memcmp(active.request.nonce,catalog.nonce,16)||memcmp(active.request.recording,r->recording,16)||
   !catalog.full.batch_count||catalog.full.batch_count!=active.request.container_bytes||
   r->sequence<active.request.segment||r->sequence-active.request.segment>=active.request.container_bytes)return -EPERM;
  return rcat_approve_receipt(&catalog,v,r)?-EPERM:0;
 }
 if(!r||catalog_admit(u,d)||active.request.command!=(recording_runtime_full_storage()?DB_FULL_RECEIVE_ACK:DB_RECEIVE_ACK)||
    memcmp(active.request.nonce,catalog.nonce,16)||memcmp(active.request.recording,r->recording,16)||
    active.request.segment!=r->sequence||active.request.container_bytes!=r->container_bytes||
    memcmp(active.request.sha256,r->digest,32))return -EPERM;
 return rcat_approve_receipt(&catalog,v,r)?-EPERM:0;
}
static int approve_delete(void *u,const struct rsm_volume *v,const uint8_t op[16],const struct rsm_terminal *t,uint64_t d)
{
 if(!op||!t||catalog_admit(u,d)||active.request.command!=(recording_runtime_full_storage()?DB_FULL_DELETE:DB_DELETE)||
    memcmp(active.request.nonce,catalog.nonce,16)||memcmp(active.request.operation,op,16)||
    memcmp(active.request.sha256,t->manifest,32))return -EPERM;
 return rcat_approve_delete(&catalog,v,op,t)?-EPERM:0;
}
static const struct rv_hooks volume_hooks={NULL,acquire,release,check,power,yield_job,confirmation,approve_receipt,approve_delete};
#include "recovery_helpers.inc"
static void wake(void *u){ARG_UNUSED(u);k_sem_give(&codec_wake);}
static int capture_owner(void *u,uint32_t value)
{ARG_UNUSED(u);return atomic_get(&leased)&&atomic_get(&recording)&&value==(uint32_t)atomic_get(&epoch)&&
 !atomic_get(&faulted)&&
#ifdef OPENPENDANT_PORTABLE_RECORDING
 (atomic_get(&portable_owned)?portable_check():
#endif
 (atomic_get(&usb_configured)&&lease_usb_epoch==(uint32_t)atomic_get(&usb_epoch))
#ifdef OPENPENDANT_PORTABLE_RECORDING
 )
#endif
 ;}
static int arm(void *u,uint64_t d){ARG_UNUSED(u);return guard_set(0,d);}
static int disarm(void *u){ARG_UNUSED(u);return guard_clear(0);}
static int recorder_acquire(void *u,struct rw_readiness *out,uint64_t d)
{
 if(check(u,1,d)||atomic_get(&catalog_epoch)||!atomic_get(&leased)||!atomic_get(&volume_mounted)||atomic_get(&volume_suspended)||atomic_get(&recording))return -EPERM;
 struct recording_configuration cfg;
 if(rcfg_get(&cfg)||cfg.phase!=RCFG_ACTIVE||cfg.fault_banks)return -EPERM;
 struct ros_status s;struct rsb_status b;struct recording_capture_status c;
 if(ros_get_status(store,&s)||!s.mounted||s.fault||s.job!=ROS_NONE||
    rsb_get_status(&bridge,&b)||b.state!=RSB_SELECTED||b.cancelled||b.pending||
    recording_capture_get_status(&c)||c.fault||c.power_on||!c.joined||!c.clock_stopped||!c.buffers_scrubbed||
    (c.state!=RC_IDLE&&c.state!=RC_JOINED))return -EPERM;
 /* profile_qualified means this fixed reviewed engineering CELT profile, not
  * a claim that whole-pipeline target latency/power has already been measured. */
 *out=(struct rw_readiness){1,1,1,1,1,1};return 0;
}
static int recorder_release(void *u,uint64_t d)
{
 if(check(u,1,d)||atomic_get(&storage_running))return -EPERM;
 /* A user stop may race a DMIC frame already returned to the producer. The
  * worker rejects that frame with STOPPED_INPUT; capture still joins and
  * proves STOP/power-off/scrubbing. Other submit failures remain failures. */
 if(recording_capture_get_status(&capture_status)||capture_status.fault||capture_status.capture_error||
 (capture_status.submit_rc<0&&capture_status.submit_rc!=RW_STOPPED_INPUT)||
 !capture_status.joined||capture_status.power_on||
 !capture_status.clock_stopped||!capture_status.buffers_scrubbed)return -EPERM;
 atomic_clear(&recording);
 if(pendant_audio_indicator(false)||rv_suspend(&volume,d))return -EPERM;
 if(rpc_leave(&cpu_clock))return -EPERM;
 atomic_set(&volume_suspended,1);return 0;
}
static int capture_start(void *u,uint32_t value,uint64_t first,uint64_t d)
{
 memset(&timing,0,sizeof(timing));
#ifdef OPENPENDANT_PORTABLE_RECORDING
 /* A grace window only finishes existing work: never starts a microphone. */
 if(active.portable&&!portable_capture_ready())return -EPERM;
#endif
 if(check(u,1,d)||rpc_enter(&cpu_clock)||now(NULL)>=d)return -EPERM;
 if(active.public_test){
  if(check(u,1,d)||first||atomic_get(&generated_live)||recording_runtime_microphone_power())return -EPERM;
  atomic_set(&epoch,(atomic_val_t)value);atomic_set(&recording,1);
  atomic_clear(&generated_frames);atomic_clear(&generated_error);atomic_set(&generated_live,1);
  k_timer_start(&generated_timer,K_MSEC(20),K_MSEC(20));return 0;
 }
 if(check(u,1,d)||pendant_audio_indicator(true))return -EPERM;
 atomic_set(&epoch,(atomic_val_t)value);atomic_set(&recording,1);
 return recording_capture_start(value,first,d);
}
static int stop_capture(void *u,uint32_t value,uint64_t d)
{
 ARG_UNUSED(u);
 if(active.public_test){
  unsigned key=irq_lock();atomic_clear(&generated_live);k_timer_stop(&generated_timer);irq_unlock(key);
  return value==(uint32_t)atomic_get(&epoch)&&now(NULL)<d&&!atomic_get(&generated_error)?0:-EIO;
 }
 return recording_capture_stop_and_join(value,d);
}
static int reserve(void *u,const struct es_binding *b,uint64_t d)
{ARG_UNUSED(u);return rsb_reserve(&bridge,b,d);}
static int recorder_capacity(void *u,const struct es_binding *b,uint32_t *slots,uint64_t d)
{
 struct rw_readiness r;
 if(!b||!slots||active.task!=TASK_START||!atomic_get(&command_busy)||
    atomic_get(&storage_running)||recorder_acquire(u,&r,d))return -EPERM;
 return rsb_capacity(&bridge,b,slots,d);
}
static int submit(void *u,const struct es_binding *b,const uint8_t *p,size_t n,struct rp_segment_receipt *r,uint64_t d)
{
 ARG_UNUSED(u);if(guard_set(1,d))return RP_IO_UNCERTAIN;
 trace_job=1;trace_sequence=b->segment_sequence;trace_job_started=(uint32_t)now(NULL);
 timing_job_start=k_cycle_get_32();
 trace_stage_started=trace_job_started;trace_stage=UINT32_MAX;
 int rc=rsb_submit(&bridge,b,p,n,r,d);
 if(rc==RP_IO_PENDING){atomic_set(&storage_running,1);k_sem_give(&storage_wake);}
 else if(rc==RP_IO_FULL){if(guard_clear(1))fatal(NULL,-ETIMEDOUT);}
 else fatal(NULL,-EIO);
 return rc;
}
static int poll_store(void *u,const struct es_binding *b,struct rp_segment_receipt *r,uint64_t d)
{
 ARG_UNUSED(u);
 /* The storage actor publishes its joined READY state before the codec is
  * allowed to consume the receipt; bridge-gate release alone is not a join. */
 if(atomic_get(&storage_running)){memset(r,0,sizeof(*r));return RP_IO_PENDING;}
 int rc=rsb_poll(&bridge,b,r,d);
 if(rc==RP_IO_OK){if(atomic_get(&storage_running)||guard_clear(1))fatal(NULL,-EIO);}
 return rc;
}
static int finalize(void *u,const struct rp_completion *c,struct rp_final_receipt *r,uint64_t d)
{ARG_UNUSED(u);trace_job=2;trace_job_started=(uint32_t)now(NULL);
 trace_stage_started=trace_job_started;trace_stage=UINT32_MAX;return rsb_finalize(&bridge,c,r,d);}
static const struct rw_async_hooks recorder_hooks={{NULL,now,arm,disarm,recorder_acquire,recorder_release,
 capture_start,stop_capture,reserve,submit,finalize},poll_store};
static int initialize_volume(void)
{
 if(atomic_get(&volume_initialized))return 0;
 int rc=rv_init(&volume,&volume_hooks);if(rc)return rc;atomic_set(&volume_initialized,1);return 0;
}
static int access_store(void)
{
 if(rv_access(&volume,&store,&sync_metadata,&volume_binding))return -EIO;
 if(!atomic_get(&codec_initialized)){
#ifdef OPENPENDANT_NATIVE_STORAGE
  /* rv_init qualified exact in-place AES on this target before volume access. */
  if(rsb_init_deferred_seal(&bridge,store,NULL,now))return -EIO;
#else
  if(rsb_init(&bridge,store,NULL,now))return -EIO;
#endif
  /* Measured native checked commits exceed 5 s. Keep one absolute 8 s
   * deadline, below the 10 s production interval; never renew on progress. */
  if(rw_init_capacity_async(&recorder_hooks,recorder_capacity,RP_MANUAL,65536,8000))return -EIO;
  atomic_set(&codec_initialized,1);
 }
 return 0;
}
static int mount_writable(uint64_t d)
{
 if(initialize_volume())return -EIO;
 if(!atomic_get(&volume_mounted)){if(rv_mount(&volume,d))return -EIO;atomic_set(&volume_mounted,1);}
 else if(atomic_get(&volume_suspended)){if(rv_reopen(&volume,d))return -EIO;atomic_clear(&volume_suspended);}
 if(rv_grant(&volume,d)||access_store())return -EIO;
 return 0;
}
static int stream_begin(void *u){ARG_UNUSED(u);return psa_hash_setup(&catalog_stream,PSA_ALG_SHA_256)==PSA_SUCCESS?0:-EIO;}
static int stream_update(void *u,const uint8_t *p,size_t n){ARG_UNUSED(u);return psa_hash_update(&catalog_stream,p,n)==PSA_SUCCESS?0:-EIO;}
static int stream_finish(void *u,uint8_t out[32]){ARG_UNUSED(u);size_t n=0;return psa_hash_finish(&catalog_stream,out,32,&n)==PSA_SUCCESS&&n==32?0:-EIO;}
static void stream_abort(void *u){ARG_UNUSED(u);if(psa_hash_abort(&catalog_stream)!=PSA_SUCCESS)atomic_set(&faulted,1);}
static int sync_perform(uint64_t d)
{
 if(check(NULL,0,d)||atomic_get(&recording)||atomic_get(&storage_running))return -EPERM;
 if(!catalog.opened){
  if(active.request.command!=(recording_runtime_full_storage()?DB_FULL_CATALOG:DB_CATALOG)||active.request.offset)return RCAT_REFUSED;
  if(mount_writable(d))return -EIO;
  if(!catalog.initialized){
   struct db_volume v={.generation=volume_binding.generation};
   memcpy(v.device,volume_binding.device_id,16);memcpy(v.volume,volume_binding.volume_id,16);
   memcpy(v.fingerprint,volume_binding.key_fingerprint,32);
   const struct owned_page_hash hash={NULL,catalog_hash};
   const struct rcat_port port={NULL,now,catalog_admit,catalog_yield,catalog_retire_admit};
   const struct rm_stream_port streaming={NULL,stream_begin,stream_update,stream_finish,stream_abort,NULL};
   if(recording_runtime_full_storage()?rcat_init_full(&catalog,store,sync_metadata,&v,&hash,&port,&streaming):
    rcat_init(&catalog,store,sync_metadata,&v,&hash,&port))return -EIO;
  }
  int rc=rcat_open(&catalog,active.request.nonce,d);if(rc)return rc;
 }
 if(active.request.command!=DB_FULL_STREAM)
  return rcat_handle(&catalog,&active.request,sync_response,&sync_response_bytes,d);
 /* Keep the admitted request immutable; only the catalog's local selector
  * advances. Its cached authenticated segment supplies bounded192B chunks.
  * One broker staging slot and the stack's finite ATT credits bound memory.
  * No next request, renewed deadline or durable receipt is implied by enqueue. */
 uint32_t used=0;
 for(;;){
  int ready_state;
  while((ready_state=recording_ble_stream_ready(active.sync_epoch,active.request.sequence))==0){
   if(check(NULL,0,d))return -EPERM;
   recording_ble_stream_wait();
  }
  if(ready_state<0)return RCAT_REFUSED;
  struct db_request part=active.request;part.command=DB_FULL_SEGMENT;part.offset+=used;
  uint32_t left=active.request.maximum-used;
  part.maximum=(uint16_t)(left<DB_FULL_MAX_DATA?left:DB_FULL_MAX_DATA);
  int rc=rcat_handle(&catalog,&part,sync_response,&sync_response_bytes,d);if(rc)return rc;
  sync_response[3]=(uint8_t)(DB_FULL_STREAM|0x80U);
  uint32_t total=sys_get_le32(sync_response+29);used+=(uint32_t)(sync_response_bytes-33U);
  /* Final delivery only after execute_active joins the worker/backend. */
  if(used==active.request.maximum||active.request.offset+used==total)return 0;
  if(recording_ble_stream_frame(active.sync_epoch,active.request.sequence,sync_response,sync_response_bytes))return -EIO;
  memset(sync_response,0,sizeof(sync_response));sync_response_bytes=0;
 }
}
static int retire_perform(uint64_t d)
{
 if(check(NULL,0,d)||atomic_get(&recording)||atomic_get(&storage_running)||atomic_get(&sync_actor))return -EPERM;
 if(catalog.initialized&&(rcat_cancel(&catalog)||rcat_retire(&catalog,d)))return -EIO;
 if(atomic_get(&leased)){
  if(rv_suspend(&volume,d))return -EIO;
  atomic_set(&volume_suspended,1);
 }
 return 0;
}
static void storage_thread(void *a,void *b,void *c)
{
 ARG_UNUSED(a);ARG_UNUSED(b);ARG_UNUSED(c);
 for(;;){k_sem_take(&storage_wake,K_FOREVER);
  int rc=RSB_PENDING;
  while(rc==RSB_PENDING||rc==RSB_BUSY){
   uint32_t stage=volume.store.status.stage;
   if(stage!=trace_stage){trace_stage=stage;trace_stage_started=(uint32_t)now(NULL);}
   uint32_t started=k_cycle_get_32(),codec_before=timing.codec_ticks;
   rc=rsb_advance(&bridge);
   rt_stage(&timing,stage,(uint32_t)(k_cycle_get_32()-started),timing.codec_ticks-codec_before);
   if(rc==RSB_BUSY)k_msleep(1);else k_yield();
  }
  if(rc!=RSB_READY)fatal(NULL,-EIO);
  rt_job(&timing,(uint32_t)(k_cycle_get_32()-timing_job_start));
  atomic_clear(&storage_running);k_sem_give(&codec_wake);
 }
}
K_THREAD_DEFINE(recording_storage_thread,16384,storage_thread,NULL,NULL,NULL,12,0,0);
static void record_loop(void)
{
 uint64_t status_busy_deadline=0;
 for(;;){
  (void)k_sem_take(&codec_wake,K_MSEC(10));
  recording_ble_poll();
#ifdef OPENPENDANT_PORTABLE_RECORDING
  if(atomic_get(&portable_owned)){
   if(!portable_check())fatal(NULL,-EPERM);
   unsigned key=irq_lock();int draining=portable_lease.draining;irq_unlock(key);
   if(draining)(void)rw_request_stop((uint32_t)atomic_get(&epoch),RW_LOW_POWER);
  }
#endif
#ifdef OPENPENDANT_LONG_CONTROL
  recording_control_ble_poll();
#endif
  if(recording_capture_get_status(&capture_status)==0&&capture_status.fault)
   (void)rw_request_stop((uint32_t)atomic_get(&epoch),RW_EXTERNAL);
  uint32_t started=k_cycle_get_32();
  int rc=
#ifdef OPENPENDANT_LONG_CONTROL
   active.control_ticket?lcw_step(&long_worker):
#endif
   rw_step();
  rt_codec(&timing,(uint32_t)(k_cycle_get_32()-started));
  /* A lower-priority USB/BLE status reader can hold rw_entry when this
   * codec actor preempts it. Yield so that bounded snapshot can finish.
   * Do not renew any storage/codec guard, retry a mutation or spin forever. */
  if(rc==RW_BUSY){
   uint64_t t=now(NULL);
   if(!status_busy_deadline){if(t>UINT64_MAX-100U)fatal(NULL,RW_BUSY);status_busy_deadline=t+100U;}
   if(t>=status_busy_deadline)fatal(NULL,RW_BUSY);
   k_msleep(1);continue;
  }
  status_busy_deadline=0;
  if(rc==RW_COMPLETE){
#ifdef OPENPENDANT_LONG_CONTROL
   if(active.control_ticket)long_terminal_publish();
#endif
   break;
  }
#ifdef OPENPENDANT_LONG_CONTROL
  if(active.control_ticket){int cached=long_publish();if(cached&&cached!=LRC_BUSY)fatal(NULL,-EIO);}
#endif
  if(rc!=RW_OK&&rc!=RW_WAIT)fatal(NULL,rc);
 }
}
static int perform(struct command *cmd)
{
 uint64_t d=cmd->deadline;
 if(cmd->task==TASK_INIT){
  int rc=rcfg_init();if(rc<0)return rc;
  struct recording_configuration cfg;
  atomic_set(&sync_configured,rcfg_get(&cfg)==0&&cfg.phase==RCFG_ACTIVE&&!cfg.fault_banks);
  atomic_set(&full_storage,rcfg_get(&cfg)==0&&rcfg_is_full(&cfg));
  const struct recording_capture_hooks hooks={NULL,capture_owner,wake,fatal};
  return recording_capture_init(&hooks);
 }
 if(now(NULL)>=d)return -EPERM;
#ifdef OPENPENDANT_PORTABLE_RECORDING
 if(cmd->portable){if(cmd->task!=TASK_START||!portable_begin())return -EPERM;}
 else
#endif
#ifdef OPENPENDANT_BATTERY_SYNC
 if(atomic_get(&sync_power_owned)&&(cmd->task==TASK_SYNC||cmd->task==TASK_RETIRE)){
  if(cmd->task==TASK_SYNC){
   /* Queue delay is not permission to start fresh I/O on stale/low power. */
   if(!sync_power_ready())return RCAT_REFUSED;
   unsigned key=irq_lock();sync_power_work_until=d;irq_unlock(key);
  }
 }else
#endif
 if(cmd->usb_epoch!=(uint32_t)atomic_get(&usb_epoch)||!atomic_get(&usb_configured))return -EPERM;
 if(cmd->task==TASK_SYNC)return sync_perform(d);
 if(cmd->task==TASK_RETIRE)return retire_perform(d);
 if(atomic_get(&catalog_epoch))return -EBUSY;
#ifdef OPENPENDANT_NATIVE_STORAGE
 if(cmd->task==TASK_FULL_PREPARE){
  if(atomic_get(&volume_initialized)||atomic_get(&leased)||mic_commands_busy()||pendant_ble_pairing_busy()||pendant_recovery_is_pending())return -EPERM;
  int rc=rcfg_prepare_full(cmd->confirmation,cmd->spec.volume_id);if(rc)return rc;
  atomic_clear(&sync_configured);atomic_clear(&ready); /* Reboot required; never bind old and new HAL owners. */
  return 0;
 }
 if(cmd->task==TASK_FULL_FORMAT){
  if(initialize_volume()||rv_provision_full(&volume,cmd->confirmation,d)||access_store()||rv_suspend(&volume,d))return -EIO;
  atomic_set(&volume_mounted,1);atomic_set(&volume_suspended,1);atomic_set(&sync_configured,1);return 0;
 }
 if(cmd->task==TASK_FULL_COMPLETE){
  if(initialize_volume()||rv_complete_full(&volume,cmd->confirmation,d)||access_store()||rv_suspend(&volume,d))return -EIO;
  atomic_set(&volume_mounted,1);atomic_set(&volume_suspended,1);atomic_set(&sync_configured,1);return 0;
 }
#endif
 if(cmd->task==TASK_ENROLL){
  if(atomic_get(&leased)||mic_commands_busy()||pendant_ble_pairing_busy()||pendant_recovery_is_pending())return -EBUSY;
  int rc=rcfg_prepare(&cmd->spec,cmd->recipient);
  if(rc||check(NULL,1,d))return rc?rc:-EPERM;
  return initialize_volume();
 }
 if(cmd->task==TASK_PROVISION){
  if(initialize_volume()||rv_provision(&volume,cmd->confirmation,d)||access_store())return -EIO;
  atomic_set(&volume_mounted,1);
  if(rv_suspend(&volume,d))return -EIO;
  atomic_set(&volume_suspended,1);atomic_set(&sync_configured,1);return 0;
 }
 if(cmd->task==TASK_MOUNT){
  if(mount_writable(d)||rv_suspend(&volume,d))return -EIO;
  atomic_set(&volume_suspended,1);return 0;
 }
#ifdef OPENPENDANT_NATIVE_STORAGE
 if(cmd->task==TASK_METADATA_EXTEND){
  if(mount_writable(d)||rv_extend_metadata(&volume,d)||rv_suspend(&volume,d))return -EIO;
  atomic_set(&volume_suspended,1);return 0;
 }
#endif
 if(cmd->task==TASK_START){
  if(mount_writable(d)||rw_select_mode((enum rp_mode)cmd->mode))return -EIO;
  struct es_binding session=volume_binding;
  if(psa_generate_random(session.recording_id,16)!=PSA_SUCCESS)return -EIO;
  session.recording_id[6]=(session.recording_id[6]&15U)|0x40U;
  session.recording_id[8]=(session.recording_id[8]&63U)|0x80U;
  if(rsb_select(&bridge,&session,(enum rp_mode)cmd->mode,0))return -EIO;
  /* The long-control guard ends before the worker establishes its own exact
   * per-call guards. Capture can only start inside rw_start after admission. */
  int started=RW_BUSY;
#ifdef OPENPENDANT_LONG_CONTROL
  for(unsigned tries=0;tries<(cmd->control_ticket?32U:1U);++tries){
   if(guard_clear(0))fatal(NULL,-EIO);
   started=cmd->control_ticket?lcw_start(&long_worker,&long_control,cmd->control_ticket,&session,0):rw_start(&session,0);
   if(started!=RW_BUSY||!cmd->control_ticket)break;
   if(guard_set(0,d))fatal(NULL,-ETIMEDOUT);
   if(tries<31U)k_msleep(1);
  }
  if(cmd->control_ticket&&started==RW_BUSY){
   if(rv_suspend(&volume,d))fatal(NULL,-EIO);
   atomic_set(&volume_suspended,1);return -EBUSY;
  }
#else
  if(guard_clear(0))fatal(NULL,-EIO);
  started=rw_start(&session,0);
#endif
  if(started==RW_NO_CAPACITY||started==RW_CANCELLED||started==RW_COMPLETE){
#ifdef OPENPENDANT_LONG_CONTROL
   if(cmd->control_ticket)long_terminal_publish();
#endif
   return 0;
  }
  if(started)fatal(NULL,-EIO);
#ifdef OPENPENDANT_LONG_CONTROL
  if(cmd->control_ticket){int cached=long_publish();if(cached&&cached!=LRC_BUSY)fatal(NULL,-EIO);}
#endif
  record_loop();return 0;
 }
 return -EINVAL;
}
#ifdef OPENPENDANT_LONG_CONTROL
#include "recording_long_runtime.inc"
#endif
static int sync_base_ready(void)
{return !atomic_get(&capacity_used)&&!atomic_get(&recovery_used)&&atomic_get(&ready)&&atomic_get(&sync_configured)&&!atomic_get(&faulted)&&
#ifdef OPENPENDANT_BATTERY_SYNC
 (atomic_get(&sync_power_owned)?sync_power_ready():(atomic_get(&usb_configured)||(!atomic_get(&catalog_epoch)&&portable_admit())))&&
#else
 atomic_get(&usb_configured)&&
#endif
 !atomic_get(&recording)&&!atomic_get(&storage_running)&&
 !atomic_get(&retire_pending)&&!pendant_recovery_is_pending();}
int recording_runtime_sync_ready(void *u)
{
 ARG_UNUSED(u);
 /* Callback-safe cached state only; no rcfg/settings locks or catalog I/O. */
 return sync_base_ready()&&((!atomic_get(&command_busy)&&!mic_commands_busy())||atomic_get(&catalog_epoch));
}
int recording_runtime_sync_submit(void *u,uint32_t value,const struct db_request *request,uint64_t deadline)
{
 ARG_UNUSED(u);uint64_t t=now(NULL);
 uint64_t budget=request&&request->command==DB_FULL_CATALOG&&!request->offset?RECORDING_BLE_FULL_OPEN_MS:RECORDING_BLE_REQUEST_MS;
 if(!value||!request||!recording_runtime_sync_ready(NULL)||deadline<=t||deadline-t>budget||
    recording_runtime_full_storage()!=durable_ble_is_full(request->command)||
    !recording_ble_admitted(value,request,deadline)||!atomic_cas(&command_busy,0,1))return -EBUSY;
 uint32_t prior=(uint32_t)atomic_get(&catalog_epoch);
 if(!sync_base_ready()||(prior&&prior!=value)||(!prior&&((request->command!=DB_CATALOG&&request->command!=DB_FULL_CATALOG)||request->offset))){
  atomic_clear(&command_busy);return -EPERM;
 }
 struct command cmd={.task=TASK_SYNC,.usb_epoch=(uint32_t)atomic_get(&usb_epoch),
  .deadline=deadline,.sync_epoch=value};
 memcpy(&cmd.request,request,sizeof(cmd.request));
#ifdef OPENPENDANT_BATTERY_SYNC
 /* Select once per catalog lifetime. Never promote an existing USB lease
  * after losing its epoch, nor renew a draining battery lease. */
 if(!prior&&!sync_power_begin()&&!atomic_get(&usb_configured)){
  atomic_clear(&command_busy);return -EPERM;
 }
#endif
 atomic_set(&catalog_epoch,(atomic_val_t)value);
 if(k_msgq_put(&runtime_commands,&cmd,K_NO_WAIT)){
#ifdef OPENPENDANT_BATTERY_SYNC
  if(!prior)sync_power_end();
#endif
  if(!prior)atomic_clear(&catalog_epoch);
  atomic_clear(&command_busy);return -EBUSY;
 }
 k_sem_give(&command_dispatch);return 0;
}
int recording_runtime_sync_retire(void *u,uint32_t value)
{
 ARG_UNUSED(u);
 if(!value||value!=(uint32_t)atomic_get(&catalog_epoch))return -EPERM;
 /* Completion can synchronously request retirement before command_busy is
  * cleared. Latch only; the single codec actor queues cleanup after joining. */
 if(!atomic_cas(&retire_pending,0,(atomic_val_t)value)&&
    (uint32_t)atomic_get(&retire_pending)!=value)return -EBUSY;
 return 0;
}
static void schedule_retirement(void)
{
 uint32_t value=(uint32_t)atomic_get(&retire_pending);
 if(!value||!atomic_cas(&command_busy,0,1))return;
 struct command cmd={.task=TASK_RETIRE,.usb_epoch=(uint32_t)atomic_get(&usb_epoch),
  .deadline=now(NULL)+RECORDING_BLE_REQUEST_MS,.sync_epoch=value};
 if(value!=(uint32_t)atomic_get(&catalog_epoch)||k_msgq_put(&runtime_commands,&cmd,K_NO_WAIT))fatal(NULL,-EIO);
 k_sem_give(&command_dispatch);
}
static void execute_active(void)
{
 runtime_resume(); /* Includes diagnostics which run before guard_set(). */
 if(active.task==TASK_CAPACITY_PROBE){execute_capacity_probe();return;}
 if(active.task==TASK_DHARA_TRIAL){execute_trial();return;}
 if(active.task==TASK_CONTROL_PROBE){execute_control_probe();return;}
 if(active.task==TASK_PHY_PROBE){execute_phy_probe();return;}
 if(active.task==TASK_PREIMAGE){execute_preimage();return;}
 if(active.task==TASK_RECOVER){execute_recovery();return;}
 if(guard_set(0,active.deadline))fatal(NULL,-ETIMEDOUT);
 if(active.task==TASK_SYNC){atomic_set(&sync_actor,1);sync_response_bytes=0;memset(sync_response,0,sizeof(sync_response));}
 int rc=perform(&active);
 if(active.task==TASK_SYNC){
  if(!rc&&check(NULL,0,active.deadline))rc=-EPERM;
  atomic_clear(&sync_actor); /* All catalog/ROS/RSM calls have returned. */
 }
 if(active.task!=TASK_START||rc){if(guard_clear(0))fatal(NULL,-ETIMEDOUT);}
 if(active.task==TASK_SYNC){
  int br=rc?recording_ble_failed(active.sync_epoch,active.request.sequence):
   recording_ble_complete(active.sync_epoch,active.request.sequence,sync_response,sync_response_bytes);
  memset(sync_response,0,sizeof(sync_response));sync_response_bytes=0;
  /* A refused selector has no uncertain I/O and can retire the clean catalog.
   * Backend/ownership faults never become clean reusable sessions. */
  if(rc==RCAT_REFUSED)rc=0;
  if(br)rc=-EIO;
 }else if(active.task==TASK_RETIRE){
  int br=recording_ble_retired(active.sync_epoch,rc);
  if(!rc&&!br){
#ifdef OPENPENDANT_BATTERY_SYNC
   sync_power_end();
#endif
   atomic_clear(&retire_pending);atomic_clear(&catalog_epoch);}
  else rc=-EIO;
 }
 if(rc){
#ifdef OPENPENDANT_LONG_CONTROL
  if(active.control_ticket&&long_failure(active.control_ticket))atomic_store(&long_control.fault,1);
#endif
  atomic_set(&faulted,1);if(atomic_get(&leased)||atomic_get(&recording))fatal(NULL,rc);}
 int initializing=active.task==TASK_INIT;
#ifdef OPENPENDANT_PORTABLE_RECORDING
 if(active.portable)portable_end();
#endif
 if(active.shell)shell_print(active.shell,"RECORDER_DONE task=%u rc=%d microphone=%u",active.task,rc,recording_runtime_microphone_power());
 memset(&active,0,sizeof(active));atomic_clear(&command_busy);
 /* TASK_INIT still has ready=0: publish no shell/radio admission until the
  * once-only RAM permit binding succeeds. Binding performs no storage I/O. */
 if(initializing){if(!rc)rc=hp_runtime_bind();if(rc)atomic_set(&faulted,1);atomic_set(&ready,rc==0);}
#ifdef OPENPENDANT_LONG_CONTROL
 if(initializing&&!rc&&long_initialize()){atomic_set(&faulted,1);atomic_clear(&ready);}
#endif
}
static void codec_thread(void *a,void *b,void *c)
{
 ARG_UNUSED(a);ARG_UNUSED(b);ARG_UNUSED(c);
 for(;;){
  recording_ble_poll();schedule_retirement();
#ifdef OPENPENDANT_LONG_CONTROL
  recording_control_ble_poll();
#endif
  if(k_msgq_get(&runtime_commands,&active,K_MSEC(atomic_get(&runtime_standby)?250:10)))continue;
  /* Publishing the message can preempt lower-priority shell code. Wait until
   * its QUEUED line has been issued before any task/DONE output or capture. */
  uint64_t t=now(NULL);
  if(t>=active.deadline||k_sem_take(&command_dispatch,K_MSEC(active.deadline-t))){
   if(active.task==TASK_CONTROL_PROBE)atomic_set(&control_probe_expired,1);
   else if(active.task==TASK_PHY_PROBE)atomic_set(&phy_probe_expired,1);
   else if(active.task==TASK_PREIMAGE)atomic_set(&preimage_expired,1);
   else if(active.task==TASK_RECOVER)atomic_set(&recovery_expired,1);
   else if(active.task==TASK_DHARA_TRIAL||active.task==TASK_CAPACITY_PROBE){/* Admission below observes original deadline. */}
   else fatal(NULL,-ETIMEDOUT);
  }
  execute_active();
 }
}
K_THREAD_DEFINE(recording_codec_thread,65536,codec_thread,NULL,NULL,NULL,10,0,0);
int recording_runtime_init(void)
{
 if(!atomic_cas(&initialized,0,1))return -EALREADY;
 k_timer_start(&runtime_supervisor,K_MSEC(10),K_MSEC(10));
 struct command cmd={.task=TASK_INIT,.deadline=now(NULL)+5000};
 atomic_set(&command_busy,1);int rc=k_msgq_put(&runtime_commands,&cmd,K_NO_WAIT);
 if(!rc)k_sem_give(&command_dispatch);
 return rc;
}
int recording_runtime_microphone_power(void)
{struct recording_capture_status s;return recording_capture_get_status(&s)==0?(int)s.power_on:atomic_get(&recording)!=0;}
int recording_runtime_faulted(void){return atomic_get(&faulted)!=0;}

static int unhex(const char *text,uint8_t *out,size_t n)
{
 if(!text||strlen(text)!=n*2)return -EINVAL;
 for(size_t i=0;i<n*2;++i)if(!((text[i]>='0'&&text[i]<='9')||(text[i]>='a'&&text[i]<='f')))return -EINVAL;
 for(size_t i=0;i<n;++i){unsigned a=(unsigned)text[2*i],b=(unsigned)text[2*i+1];
  out[i]=(uint8_t)(((a<='9'?a-'0':a-'a'+10)<<4)|(b<='9'?b-'0':b-'a'+10));}
 return 0;
}
static void hex(const uint8_t *p,size_t n,char *out)
{static const char digits[]="0123456789abcdef";for(size_t i=0;i<n;++i){out[2*i]=digits[p[i]>>4];out[2*i+1]=digits[p[i]&15];}out[n*2]=0;}
static int local(const struct shell *sh)
{return sh==shell_backend_uart_get_ptr()&&atomic_get(&ready)&&!atomic_get(&faulted)&&atomic_get(&usb_configured)&&!pendant_recovery_is_pending();}
static int queue(const struct shell *sh,struct command *cmd)
{
 if((atomic_get(&capacity_used)&&cmd->task!=TASK_CAPACITY_PROBE)||(atomic_get(&trial_used)&&cmd->task!=TASK_DHARA_TRIAL)||(atomic_get(&recovery_used)&&cmd->task!=TASK_RECOVER)||!local(sh)||atomic_get(&catalog_epoch)||!atomic_cas(&command_busy,0,1))return -EBUSY;
 if(atomic_get(&catalog_epoch)){atomic_clear(&command_busy);return -EBUSY;}
#ifdef OPENPENDANT_PORTABLE_RECORDING
 if(cmd->task==TASK_START){
  if(!portable_admit()){atomic_clear(&command_busy);return -EPERM;}
  cmd->portable=1;
 }
#endif
 cmd->shell=sh;cmd->usb_epoch=(uint32_t)atomic_get(&usb_epoch);
 if(k_msgq_put(&runtime_commands,cmd,K_NO_WAIT)){atomic_clear(&command_busy);return -EBUSY;}
 shell_print(sh,"RECORDER_QUEUED task=%u",cmd->task);k_sem_give(&command_dispatch);return 0;
}
static int status_command(const struct shell *sh,size_t argc,char **argv)
{
 ARG_UNUSED(argc);ARG_UNUSED(argv);if(sh!=shell_backend_uart_get_ptr())return -EPERM;
 struct recording_configuration cfg;uint8_t id[16];char text[65];int cr=rcfg_get(&cfg);
 shell_print(sh,"RECORDER_STATUS ready=%u fault=%u busy=%u usb=%u lease=%u recording=%u configured=%u phase=%u mounted=%u suspended=%u",
 (unsigned)atomic_get(&ready),(unsigned)atomic_get(&faulted),(unsigned)atomic_get(&command_busy),
 (unsigned)atomic_get(&usb_configured),(unsigned)atomic_get(&leased),(unsigned)atomic_get(&recording),
 cr==0,cr==0?cfg.phase:0,(unsigned)atomic_get(&volume_mounted),(unsigned)atomic_get(&volume_suspended));
 if(!rcfg_device_id(id)){hex(id,16,text);shell_print(sh,"RECORDER_DEVICE id=%s",text);}
 if(cr==0){hex(rcfg_descriptor_digest(&cfg),32,text);shell_print(sh,"RECORDER_DESCRIPTOR sha=%s",text);}
 if(atomic_get(&codec_initialized)&&!rw_get_status(&worker_status))shell_print(sh,"RECORDER_AUDIO state=%u reason=%u segments=%u captured=%llu committed=%llu",
 worker_status.state,worker_status.reason,worker_status.pipeline.segments,
 (unsigned long long)worker_status.pipeline.captured_samples,(unsigned long long)worker_status.pipeline.committed_samples);
 if(atomic_get(&volume_initialized)){uint8_t proof[32];if(!rv_confirmation(&volume,proof)){hex(proof,32,text);shell_print(sh,"RECORDER_PROVISION_CONFIRM sha=%s",text);}}
 return 0;
}
/* Explicit, idle, local-USB diagnostics only. rcfg_get copies its initialized
 * RAM cache; no loader/settings save, volume/PHY entry, capture or retry occurs.
 * Reset flags can accumulate across boots and are deliberately never cleared.
 * Stack watermarks describe this boot, not the boot which failed provisioning. */
/* Shared exclusion only. Reading cached capacity needs no USB or power lease. */
static int maintenance_idle(void)
{
 return !atomic_get(&capacity_used)&&!atomic_get(&trial_used)&&atomic_get(&ready)&&!pendant_recovery_is_pending()&&
  !atomic_get(&leased)&&!atomic_get(&recording)&&!atomic_get(&storage_running)&&
  !atomic_get(&catalog_epoch)&&!atomic_get(&retire_pending)&&!atomic_get(&sync_actor)&&
  !enrollment.active&&!mic_commands_busy()&&!pendant_ble_pairing_busy();
}
static int diagnostics_idle(void)
{return atomic_get(&usb_configured)&&maintenance_idle();}
static atomic_t preferences_owned;
static atomic_t pairing_owned;
static int preferences_power_ready(void)
{
#ifdef OPENPENDANT_BATTERY_SYNC
 /* Same fresh, conservative admission as a recording start; no microphone,
  * NAND or recording lease is granted by this read-only gauge check. */
 if(portable_admit())return 1;
#endif
 return atomic_get(&usb_configured)!=0;
}
int recording_runtime_preferences_ready(void)
{return atomic_get(&preferences_owned)&&preferences_power_ready()&&
 !atomic_get(&faulted)&&!pendant_recovery_is_pending()&&!pendant_ble_pairing_busy();}
int recording_runtime_pairing_claim(void)
{
 /* Pairing-busy is already published by the trusted security actor. Check
  * every other idle fence, including a not-yet-started local/remote command. */
 if(!pendant_ble_pairing_busy()||!atomic_cas(&command_busy,0,1))return -EBUSY;
 int ok=atomic_get(&ready)&&!atomic_get(&capacity_used)&&!atomic_get(&trial_used)&&
  !atomic_get(&recovery_used)&&!atomic_get(&faulted)&&!atomic_get(&leased)&&
  !atomic_get(&recording)&&!atomic_get(&storage_running)&&!atomic_get(&catalog_epoch)&&
  !atomic_get(&retire_pending)&&!atomic_get(&sync_actor)&&!guards[0]&&!guards[1]&&!enrollment.active&&
  !atomic_get(&control_probe_running)&&!atomic_get(&phy_probe_running)&&!atomic_get(&preimage_running)&&
  !atomic_get(&recovery_running)&&!atomic_get(&trial_running)&&!atomic_get(&capacity_running)&&
  !mic_commands_busy()&&!pendant_recovery_is_pending()&&preferences_power_ready();
#ifdef OPENPENDANT_LONG_CONTROL
 if(ok&&atomic_load(&long_bound)){
  ok=lrc_maintenance_claim(&long_control);
  if(ok)lrc_maintenance_release(&long_control);
 }
#endif
 if(!ok){atomic_clear(&command_busy);return -EBUSY;}
 atomic_set(&pairing_owned,1);return 0;
}
void recording_runtime_pairing_release(void)
{if(atomic_cas(&pairing_owned,1,0))atomic_clear(&command_busy);}
int recording_runtime_pairing_ready(void)
{return atomic_get(&pairing_owned)&&atomic_get(&command_busy)&&preferences_power_ready()&&
 !atomic_get(&faulted)&&!mic_commands_busy()&&!pendant_recovery_is_pending()&&pendant_ble_pairing_busy();}
int recording_runtime_preferences_claim(void)
{
 if(!atomic_cas(&command_busy,0,1))return -EBUSY;
 int ok=maintenance_idle()&&preferences_power_ready()&&!atomic_get(&faulted);
#ifdef OPENPENDANT_LONG_CONTROL
 int long_held=ok&&atomic_load(&long_bound)&&lrc_maintenance_claim(&long_control);
 ok=ok&&long_held;
#endif
 if(!ok||mic_commands_reserve_external()){
#ifdef OPENPENDANT_LONG_CONTROL
  if(long_held)lrc_maintenance_release(&long_control);
#endif
  atomic_clear(&command_busy);return -EBUSY;
 }
 /* Recheck power after reservation; recovery/pairing respect this reservation.
  * The caller rechecks again immediately before the single NVS transaction. */
 if(!preferences_power_ready()||pendant_recovery_is_pending()||pendant_ble_pairing_busy()){
  mic_commands_release_external();atomic_clear(&command_busy);
#ifdef OPENPENDANT_LONG_CONTROL
  lrc_maintenance_release(&long_control);
#endif
  return -EBUSY;
 }
 atomic_set(&preferences_owned,1);return 0;
}
void recording_runtime_preferences_release(void)
{if(atomic_cas(&preferences_owned,1,0)){
 mic_commands_release_external();atomic_clear(&command_busy);
#ifdef OPENPENDANT_LONG_CONTROL
 lrc_maintenance_release(&long_control);
#endif
}}
#ifdef OPENPENDANT_BATTERY_DIAGNOSTIC
#include "battery_probe.h"
#include "recording_battery_probe.inc"
#endif
#ifdef OPENPENDANT_BATTERY_MONITOR
#include "recording_battery_monitor.inc"
#endif
void recording_runtime_telemetry(struct recorder_telemetry *out)
{
 if(!out)return;
 memset(out,0,sizeof(*out));
 /* Reserve only the idle RAM command gate, never a microphone/NAND lease.
  * IRQ exclusion makes the initial flags coherent; no arbitrary hook runs in it. */
 unsigned key=irq_lock();int held=atomic_cas(&command_busy,0,1);
 out->flags=RT_ENGINEERING | (atomic_get(&usb_configured)?RT_USB:0) |
  (atomic_get(&ready)?RT_READY:0) | (atomic_get(&volume_mounted)?RT_MOUNTED:0) |
  (atomic_get(&volume_suspended)?RT_SUSPENDED:0) |
  (!held||atomic_get(&leased)||atomic_get(&storage_running)?RT_BUSY:0) |
  (atomic_get(&recording)?RT_RECORDING:0) | (atomic_get(&faulted)?RT_FAULT:0);
 irq_unlock(key);
 if(held && !(out->flags&(RT_BUSY|RT_RECORDING|RT_FAULT)) && maintenance_idle() &&
    atomic_get(&volume_mounted) && atomic_get(&volume_suspended) && store){
  struct ros_status s={0};struct ros_geometry geometry={0};uint32_t roots=0;
  int valid=ros_get_geometry(store,&geometry)==ROS_OK&&ros_get_status(store,&s)==ROS_OK &&
   s.mounted&&!s.fault&&s.job==ROS_NONE&&s.slot_count>0&&s.slot_count<=UINT16_MAX&&s.consumed_slots<=s.slot_count;
  for(unsigned i=0;valid&&i<geometry.roots;++i){struct ros_record record;int rc=ros_get_record(store,i,&record);
   if(rc==ROS_OK){if(record.present)++roots;}else if(rc!=ROS_NOT_FOUND)valid=0;
  }
  if(valid){out->flags|=RT_CAPACITY;out->roots_used=(uint8_t)roots;out->roots_total=(uint8_t)geometry.roots;
   out->slots_used=(uint16_t)s.consumed_slots;out->slots_total=(uint16_t)s.slot_count;}
 }
 if(atomic_get(&codec_initialized)){
  struct rw_status s;
  if(rw_get_status(&s)==RW_OK && s.state<=7 && s.reason<=12 && s.mode<=1 &&
     s.pipeline.captured_samples<=UINT32_MAX && s.pipeline.committed_samples<=s.pipeline.captured_samples &&
     s.pipeline.captured_samples%320==0 && s.pipeline.committed_samples%320==0){
   out->flags|=RT_WORKER;out->state=(uint8_t)s.state;out->reason=(uint8_t)s.reason;out->mode=(uint8_t)s.mode;
   out->captured_samples=(uint32_t)s.pipeline.captured_samples;out->committed_samples=(uint32_t)s.pipeline.committed_samples;
  }
 }
 if(held)atomic_clear(&command_busy);
}
static int diagnostics_command(const struct shell *sh,size_t argc,char **argv)
{
 ARG_UNUSED(argv);
 if(argc!=1||sh!=shell_backend_uart_get_ptr())return -EPERM;
 if(!diagnostics_idle()||!atomic_cas(&command_busy,0,1))return -EBUSY;
 uint32_t epoch_before=(uint32_t)atomic_get(&usb_epoch);
 if(!diagnostics_idle()){atomic_clear(&command_busy);return -EBUSY;}
 struct recording_configuration cfg;memset(&cfg,0,sizeof(cfg));
 int cr=rcfg_get(&cfg);if(cr)memset(&cfg,0,sizeof(cfg));
 uint32_t cause=0,supported=0;size_t codec_unused=0,storage_unused=0;
 int cause_rc=hwinfo_get_reset_cause(&cause);if(cause_rc)cause=0;
 int supported_rc=hwinfo_get_supported_reset_cause(&supported);if(supported_rc)supported=0;
 int codec_rc=k_thread_stack_space_get(recording_codec_thread,&codec_unused);
 int storage_rc=k_thread_stack_space_get(recording_storage_thread,&storage_unused);
 if(!codec_rc&&codec_unused>65536U)codec_rc=-EIO;
 if(!storage_rc&&storage_unused>16384U)storage_rc=-EIO;
 if(codec_rc)codec_unused=0;
 if(storage_rc)storage_unused=0;
 if(!diagnostics_idle()||epoch_before!=(uint32_t)atomic_get(&usb_epoch)){
  atomic_clear(&command_busy);return -EPERM;
 }
 char digest[65];hex(rcfg_descriptor_digest(&cfg),32,digest);
 shell_print(sh,"RECORDER_DIAGNOSTICS v=1 cfg_rc=%d phase=%u revision=%llu fault_banks=%u descriptor=%s",
  cr,cfg.phase,(unsigned long long)cfg.revision,cfg.fault_banks,digest);
 shell_print(sh,"RECORDER_RESET cause_rc=%d cause=%u supported_rc=%d supported=%u cleared=0",
  cause_rc,cause,supported_rc,supported);
 shell_print(sh,"RECORDER_STACK codec_rc=%d codec_unused=%u codec_size=65536 storage_rc=%d storage_unused=%u storage_size=16384",
  codec_rc,(unsigned)codec_unused,storage_rc,(unsigned)storage_unused);
 shell_print(sh,"RECORDER_DIAGNOSTICS_END nand_io=0 settings_writes=0 audio=0");
 atomic_clear(&command_busy);return 0;
}
static int control_probe_command(const struct shell *sh,size_t argc,char **argv)
{
 struct recording_configuration cfg;
 if(argc!=2||strcmp(argv[1],"confirm")||!local(sh)||!diagnostics_idle()||
  atomic_get(&volume_initialized)||atomic_get(&command_busy)||atomic_get(&phy_probe_used)||atomic_get(&preimage_used)||probe_configuration(&cfg))return -EPERM;
 if(!atomic_cas(&control_probe_used,0,1))return -EALREADY;
 struct command cmd={.task=TASK_CONTROL_PROBE,.deadline=now(NULL)+CP_BUDGET_MS};
 int rc=queue(sh,&cmd);
 if(rc){control_probe_result.attempted=1;control_probe_result.rc=CP_ADMISSION;}
 return rc; /* Even queue refusal retains the one-shot marker. */
}
static int control_snapshot_lock(const struct shell *sh,size_t argc)
{
 if(argc!=1||sh!=shell_backend_uart_get_ptr())return -EPERM;
 if(!atomic_cas(&command_busy,0,1))return -EBUSY;
 if(atomic_get(&control_probe_running)){atomic_clear(&command_busy);return -EBUSY;}
 return 0;
}
static int control_status_command(const struct shell *sh,size_t argc,char **argv)
{
 ARG_UNUSED(argv);int rc=control_snapshot_lock(sh,argc);if(rc)return rc;
 const struct cp_result *r=&control_probe_result;const struct cp_nrf_fault *h=&control_probe_hal_fault;
 shell_print(sh,"RECORDER_CONTROL v=1 attempted=%u busy=0 rc=%d close_rc=%d transport_rc=%d rows=%u records=%u starts=%u polls=%u",
  (unsigned)atomic_get(&control_probe_used),r->rc,r->close_rc,r->transport_rc,r->rows,r->records,r->starts,r->polls);
 shell_print(sh,"RECORDER_CONTROL_STATE opened=%u stopped=%u ready=%u released=%u scrubbed=%u retained=%u complete=%u lease=%u a0=%u b0=%u c0=%u ecc=%u",
  r->opened,r->stopped,r->ready_known,r->released,r->scrubbed,r->retained,r->complete,
  (unsigned)atomic_get(&control_probe_owner),r->a0,r->b0,r->c0,r->ecc);
 shell_print(sh,"RECORDER_CONTROL_SCAN canonical=%u invalid=%u conflicts=%u breaks=%u last_row=%u stage=%u elapsed_ms=%llu",
  r->canonical,r->not_valid,r->conflicts,r->predecessor_breaks,r->last_row,r->last_stage,(unsigned long long)r->elapsed_ms);
 shell_print(sh,"RECORDER_CONTROL_HAL rc=%d valid=%u stage=%u opcode=%u row=%u bytes=%u fast=%u started=%u ended=%u stopped=%u amounts_valid=%u tx=%u rx=%u frequency=%u cycles=%u cycle_hz=%u error=%d",
  control_probe_hal_rc,h->valid,h->stage,h->opcode,h->row,h->bytes,h->fast,h->started,h->ended,h->stopped,
  h->amounts_valid,h->tx_bytes,h->rx_bytes,h->frequency,h->elapsed_cycles,h->cycle_hz,h->rc);
 shell_print(sh,"RECORDER_CONTROL_END nand_writes=0 settings_writes=0 audio=0");
 atomic_clear(&command_busy);return 0;
}
static int control_page_command(const struct shell *sh,size_t argc,char **argv)
{
 if(argc!=2||!argv[1]||!argv[1][0]||(argv[1][0]=='0'&&argv[1][1]))return -EINVAL;
 uint32_t index=0;size_t digits=strlen(argv[1]);if(digits>3)return -EINVAL;
 for(size_t i=0;i<digits;++i){if(argv[1][i]<'0'||argv[1][i]>'9')return -EINVAL;index=index*10U+(uint32_t)(argv[1][i]-'0');}
 int rc=control_snapshot_lock(sh,1);if(rc)return rc;
 if(!atomic_get(&control_probe_used)||!control_probe_result.stopped||index>=CP_ROWS||
  index>=control_probe_result.records||index>=control_probe_stored){atomic_clear(&command_busy);return -EPERM;}
 char data[CP_RECORD_BYTES*2U+1U];hex(control_probe_records[index],CP_RECORD_BYTES,data);
 shell_print(sh,"RECORDER_CONTROL_PAGE index=%u bytes=200 data=%s",index,data);
 atomic_clear(&command_busy);return 0;
}
// PHY_READ_PROBE_BEGIN commands
static int phy_probe_command(const struct shell *sh,size_t argc,char **argv)
{
 struct recording_configuration cfg;
 if(argc!=2||strcmp(argv[1],"confirm")||!local(sh)||!diagnostics_idle()||atomic_get(&control_probe_used)||atomic_get(&preimage_used)||
  atomic_get(&volume_initialized)||atomic_get(&command_busy)||probe_configuration(&cfg))return -EPERM;
 if(!atomic_cas(&phy_probe_used,0,1))return -EALREADY;
 struct command cmd={.task=TASK_PHY_PROBE,.deadline=now(NULL)+PRP_MAX_MS};int rc=queue(sh,&cmd);
 if(rc){phy_probe_result.attempted=1;phy_probe_result.rc=PRP_ADMISSION;}return rc;
}
static int phy_status_command(const struct shell *sh,size_t argc,char **argv)
{
 ARG_UNUSED(argv);int rc=control_snapshot_lock(sh,argc);if(rc)return rc;
 if(atomic_get(&phy_probe_running)){atomic_clear(&command_busy);return -EBUSY;}
 const struct prp_result *r=&phy_probe_result;const struct prp_fault *f=&r->first;
 shell_print(sh,"RECORDER_PHY v=1 attempted=%u busy=0 rc=%d open_rc=%d close_rc=%d reads=%u starts=%u last_row=%u ecc=%u elapsed_ms=%llu",
  (unsigned)atomic_get(&phy_probe_used),r->rc,r->open_rc,r->close_rc,r->reads,r->starts,r->last_row,r->ecc,(unsigned long long)r->elapsed_ms);
 shell_print(sh,"RECORDER_PHY_STATE complete=%u stopped=%u ready=%u released=%u retained=%u output_scrubbed=%u lease=%u",
  r->complete,r->stopped,r->ready,r->released,r->retained,r->output_scrubbed,(unsigned)atomic_get(&phy_probe_owner));
 shell_print(sh,"RECORDER_PHY_FAULT valid=%u stage=%u opcode=%u row=%u bytes=%u fast=%u started=%u stopped=%u tx=%u rx=%u cycles=%u cycle_hz=%u error=%d before_ms=%llu after_ms=%llu",
  f->valid,f->stage,f->opcode,f->row,f->bytes,f->fast,f->started,f->stopped,f->tx_amount,f->rx_amount,
  f->elapsed_cycles,f->cycle_hz,f->rc,(unsigned long long)f->before_ms,(unsigned long long)f->after_ms);
 shell_print(sh,"RECORDER_PHY_END nand_writes=0 settings_writes=0 audio=0");atomic_clear(&command_busy);return 0;
}
// PHY_READ_PROBE_END commands
// CURRENT_PREIMAGE_BEGIN commands
static int preimage_command(const struct shell *sh,size_t argc,char **argv)
{
 struct recording_configuration cfg;struct command cmd={.task=TASK_PREIMAGE};
 if(argc!=3||!argv[1]||!argv[1][0]||(argv[1][0]=='0'&&argv[1][1])||strlen(argv[1])>4||
  unhex(argv[2],cmd.confirmation,16))return -EINVAL;
 uint32_t index=0,nonzero=0,not_ff=0;
 for(size_t i=0;argv[1][i];++i){if(argv[1][i]<'0'||argv[1][i]>'9')return -EINVAL;index=index*10U+(uint32_t)(argv[1][i]-'0');}
 for(unsigned i=0;i<16;++i){nonzero|=cmd.confirmation[i];not_ff|=cmd.confirmation[i]^255U;}
 if(!nonzero||!not_ff||index>=CPI_PAGES)return -EINVAL;
 if(!local(sh)||!diagnostics_idle()||atomic_get(&volume_initialized)||atomic_get(&command_busy)||
  atomic_get(&control_probe_used)||atomic_get(&phy_probe_used)||probe_configuration(&cfg))return -EPERM;
 if(atomic_get(&preimage_used)){
  if(!atomic_get(&preimage_initialized)||preimage_context.fault||preimage_context.available||
   index!=preimage_context.next_index||memcmp(cmd.confirmation,preimage_last_nonce,16)==0)return -EALREADY;
 }else if(index||!atomic_cas(&preimage_used,0,1))return -EALREADY;
 uint64_t t=now(NULL);if(t>UINT64_MAX-8000U){atomic_set(&faulted,1);return -EINVAL;}
 cmd.mode=index;cmd.deadline=t+CPI_PAGE_MS;memcpy(preimage_last_nonce,cmd.confirmation,16);
 int rc=queue(sh,&cmd);
 if(rc){preimage_result.attempted=1;preimage_result.index=index;preimage_result.row=65600U+index;
  preimage_result.rc=CPI_ADMISSION;preimage_context.fault=1;atomic_set(&faulted,1);}
 return rc;
}
static int preimage_status_command(const struct shell *sh,size_t argc,char **argv)
{
 ARG_UNUSED(argv);int rc=control_snapshot_lock(sh,argc);if(rc)return rc;
 if(atomic_get(&preimage_running)){atomic_clear(&command_busy);return -EBUSY;}
 const struct cpi_result *r=&preimage_result;const struct cpi_fault *f=&r->first;
 unsigned is_init=(unsigned)atomic_get(&preimage_initialized),used=(unsigned)atomic_get(&preimage_used);
 shell_print(sh,"RECORDER_PREIMAGE v=1 used=%u busy=0 initialized=%u fault=%u next=%u available=%u exported=%u rc=%d",
  used,is_init,used&&(unsigned)atomic_get(&faulted),is_init?preimage_context.next_index:0U,
  is_init?preimage_context.available:0U,preimage_exported,r->rc);
 shell_print(sh,"RECORDER_PREIMAGE_PAGE attempted=%u complete=%u index=%u row=%u reads=%u starts=%u off=%u on=%u stopped=%u ready=%u released=%u retained=%u matched=%u hash_valid=%u a0=%u b0=%u lease=%u elapsed_ms=%llu",
  r->attempted,r->complete,r->index,r->row,r->reads,r->starts,r->off_starts,r->on_starts,r->stopped,r->ready,r->released,
  r->retained,r->matched,r->hash_valid,r->a0,r->b0,(unsigned)atomic_get(&preimage_owner),(unsigned long long)r->elapsed_ms);
 shell_print(sh,"RECORDER_PREIMAGE_FAULT valid=%u stage=%u opcode=%u row=%u bytes=%u fast=%u started=%u stopped=%u tx=%u rx=%u cycles=%u cycle_hz=%u error=%d before_ms=%llu after_ms=%llu",
  f->valid,f->stage,f->opcode,f->row,f->bytes,f->fast,f->started,f->stopped,f->tx_amount,f->rx_amount,
  f->elapsed_cycles,f->cycle_hz,f->rc,(unsigned long long)f->before_ms,(unsigned long long)f->after_ms);
 shell_print(sh,"RECORDER_PREIMAGE_END nand_programs=0 nand_erases=0 settings_writes=0 audio=0");
 atomic_clear(&command_busy);return 0;
}
// CURRENT_PREIMAGE_END commands
#include "recovery_commands.inc"
#include "runtime_port.inc"
static int begin_enrollment(const struct shell *sh,size_t argc,char **argv)
{
 if(argc!=4||!local(sh)||atomic_get(&command_busy)||atomic_get(&leased)||atomic_get(&catalog_epoch))return -EPERM;
 if(k_mutex_lock(&enrollment_lock,K_NO_WAIT))return -EBUSY;
 int rc=-EINVAL;struct recording_configuration cfg;
 if(rcfg_get(&cfg)!=1||enrollment.active)goto done;
 if(unhex(argv[1],enrollment.nonce,16)||unhex(argv[2],enrollment.spec.volume_id,16)||
 unhex(argv[3],enrollment.spec.preservation_manifest_sha256,32)||rcfg_device_id(enrollment.spec.device_id))goto done;
 enrollment.spec.generation=1;for(unsigned i=0;i<32;++i)enrollment.spec.map_blocks[i]=1025+i;
 enrollment.spec.control_blocks[0]=1057;enrollment.spec.control_blocks[1]=1058;
 enrollment.usb_epoch=(uint32_t)atomic_get(&usb_epoch);enrollment.deadline=now(NULL)+60000;
 enrollment.active=1;rc=0;shell_print(sh,"RECORDER_ENROLL_STAGED public_only=1 seconds=60");
done:if(rc)memset(&enrollment,0,sizeof(enrollment));k_mutex_unlock(&enrollment_lock);return rc;
}
static int enrollment_check(const struct shell *sh,const char *nonce)
{uint8_t bytes[16];return !local(sh)||!enrollment.active||enrollment.usb_epoch!=(uint32_t)atomic_get(&usb_epoch)||
 now(NULL)>=enrollment.deadline||unhex(nonce,bytes,16)||memcmp(bytes,enrollment.nonce,16)?-EPERM:0;}
static int recipient_command(const struct shell *sh,size_t argc,char **argv)
{
 if(argc!=3||k_mutex_lock(&enrollment_lock,K_NO_WAIT))return -EINVAL;
 int rc=enrollment_check(sh,argv[1]);
 if(!rc&&(enrollment.has_point||unhex(argv[2],enrollment.point,65)))rc=-EINVAL;
 if(!rc){enrollment.has_point=1;shell_print(sh,"RECORDER_PUBLIC_RECIPIENT_STAGED");}
 else memset(&enrollment,0,sizeof(enrollment));
 k_mutex_unlock(&enrollment_lock);return rc;
}
static int commit_enrollment(const struct shell *sh,size_t argc,char **argv)
{
 if(argc!=4||strcmp(argv[3],"confirm")||k_mutex_lock(&enrollment_lock,K_NO_WAIT))return -EINVAL;
 int rc=enrollment_check(sh,argv[1]);struct command cmd={.task=TASK_ENROLL};
 if(!rc&&(!enrollment.has_point||unhex(argv[2],enrollment.spec.recipient_fingerprint,32)))rc=-EINVAL;
 if(!rc){cmd.spec=enrollment.spec;memcpy(cmd.recipient,enrollment.point,65);cmd.deadline=enrollment.deadline;
  if(cmd.deadline>now(NULL)+5000)cmd.deadline=now(NULL)+5000;
  rc=queue(sh,&cmd);}
 memset(&enrollment,0,sizeof(enrollment));k_mutex_unlock(&enrollment_lock);return rc;
}
static int provision_command(const struct shell *sh,size_t argc,char **argv)
{
 struct command cmd={.task=TASK_PROVISION,.deadline=now(NULL)+120000};
 if(argc!=3||strcmp(argv[2],"confirm")||unhex(argv[1],cmd.confirmation,32))return -EINVAL;
 return queue(sh,&cmd);
}
#ifdef OPENPENDANT_NATIVE_STORAGE
static int full_info_command(const struct shell *sh,size_t argc,char **argv)
{
 ARG_UNUSED(argv);struct recording_configuration cfg;
 if(argc!=1||!local(sh)||rcfg_get(&cfg)||!rcfg_is_full(&cfg))return -EPERM;
 static const uint8_t domain[]="OpenPendant native Dhara disposable format v1";
 uint8_t bytes[sizeof(domain)+512],sum[32];char text[131];size_t n=0;
 memcpy(bytes,domain,sizeof(domain));memcpy(bytes+sizeof(domain),cfg.descriptor,512);
 if(psa_hash_compute(PSA_ALG_SHA_256,bytes,sizeof(bytes),sum,32,&n)!=PSA_SUCCESS||n!=32)return -EIO;
 hex(sum,32,text);shell_print(sh,"RECORDER_FULL_CONFIRM sha=%s phase=%u generation=2 blocks=2048 logical_sectors=94208 slots=5120",text,cfg.phase);
 hex(cfg.spec.device_id,16,text);shell_print(sh,"RECORDER_FULL_DEVICE id=%s",text);
 hex(cfg.spec.volume_id,16,text);shell_print(sh,"RECORDER_FULL_VOLUME id=%s",text);
 hex(cfg.spec.recipient_fingerprint,32,text);shell_print(sh,"RECORDER_FULL_RECIPIENT fingerprint=%s",text);
 hex(cfg.recipient,65,text);shell_print(sh,"RECORDER_FULL_PUBLIC point=%s",text);return 0;
}
static int full_prepare_command(const struct shell *sh,size_t argc,char **argv)
{
 struct command cmd={.task=TASK_FULL_PREPARE,.deadline=now(NULL)+5000};
 if(argc!=4||strcmp(argv[3],"replace-disposable-storage")||!local(sh)||!diagnostics_idle()||
  atomic_get(&volume_initialized)||atomic_get(&control_probe_used)||atomic_get(&phy_probe_used)||
  atomic_get(&preimage_used)||atomic_get(&recovery_used)||unhex(argv[1],cmd.confirmation,32)||
  unhex(argv[2],cmd.spec.volume_id,16))return -EPERM;
 return queue(sh,&cmd);
}
static int full_format_command(const struct shell *sh,size_t argc,char **argv)
{
 struct recording_configuration cfg;struct command cmd={.task=TASK_FULL_FORMAT,.deadline=now(NULL)+3600000U};
 if(argc!=3||strcmp(argv[2],"erase-entire-external-nand")||!local(sh)||!diagnostics_idle()||
  rcfg_get(&cfg)||!rcfg_is_full(&cfg)||cfg.phase!=RCFG_PREPARED||unhex(argv[1],cmd.confirmation,32))return -EPERM;
 return queue(sh,&cmd);
}
static int full_complete_command(const struct shell *sh,size_t argc,char **argv)
{
 struct recording_configuration cfg;struct command cmd={.task=TASK_FULL_COMPLETE,.deadline=now(NULL)+300000U};
 if(argc!=3||strcmp(argv[2],"verify-root-complete-metadata")||!local(sh)||!diagnostics_idle()||
  atomic_get(&volume_initialized)||rcfg_get(&cfg)||!rcfg_is_full(&cfg)||cfg.phase!=RCFG_PROVISIONING||
  unhex(argv[1],cmd.confirmation,32))return -EPERM;
 return queue(sh,&cmd);
}
static int metadata_extend_command(const struct shell *sh,size_t argc,char **argv)
{
 struct command cmd={.task=TASK_METADATA_EXTEND,.deadline=now(NULL)+30000};
 if(argc!=2||strcmp(argv[1],"confirm-preserve-recordings")||!local(sh)||!diagnostics_idle())return -EPERM;
 return queue(sh,&cmd);
}
static int native_format_command(const struct shell *sh,size_t argc,char **argv)
{
 struct recording_configuration cfg;
 struct command cmd={.task=TASK_PROVISION,.mode=1,.deadline=now(NULL)+120000};
 if(argc!=3||strcmp(argv[2],"erase-1025-1056")||!local(sh)||!diagnostics_idle()||
    probe_configuration(&cfg)||unhex(argv[1],cmd.confirmation,32))return -EPERM;
 return queue(sh,&cmd);
}
#endif
static int trial_command(const struct shell *sh,size_t argc,char **argv)
{
 struct recording_configuration cfg;int verify=!strcmp(argv[0],"dharacheck");
 if(argc!=2||strcmp(argv[1],verify?"confirm":"erase-1025-1056")||!local(sh)||!diagnostics_idle()||
  atomic_get(&volume_initialized)||atomic_get(&control_probe_used)||atomic_get(&phy_probe_used)||
  atomic_get(&preimage_used)||atomic_get(&recovery_used)||probe_configuration(&cfg)||
  !atomic_cas(&trial_used,0,1))return -EPERM;
 struct command cmd={.task=TASK_DHARA_TRIAL,.mode=(uint32_t)verify,.deadline=now(NULL)+DT_BUDGET_MS};
 int rc=queue(sh,&cmd);if(rc){trial_result.attempted=1;trial_result.rc=rc;}return rc;
}
static int trial_status_command(const struct shell *sh,size_t argc,char **argv)
{
 ARG_UNUSED(argv);int rc=control_snapshot_lock(sh,argc);if(rc)return rc;
 const struct dt_result *r=&trial_result;
 shell_print(sh,"DHARA_TRIAL v=1 used=%u complete=%u verify_only=%u stage=%u rc=%d phy_rc=%d bad_mask=%u capacity=%u writes=%u reads=%u programs=%u erases=%u checked_bytes=%u stopped=%u ready=%u released=%u last_row=%u ecc=%u library_error=%u a0=%u b0=%u c0=%u starts=%u phy_line=%u last_opcode=%u wel_observed=%u verified_programs=%u verify_mismatch=%u elapsed_ms=%llu",
  (unsigned)atomic_get(&trial_used),r->complete,r->verify_only,r->stage,r->rc,r->phy_rc,r->bad_mask,r->capacity,
  r->writes,r->reads,r->programs,r->erases,r->checked_bytes,r->stopped,r->ready,r->released,
  r->last_row,r->ecc,r->library_error,r->a0,r->b0,r->c0,r->starts,r->phy_line,r->last_opcode,
  r->wel_observed,r->verified_programs,r->verify_mismatch,(unsigned long long)r->elapsed_ms);
 atomic_clear(&command_busy);return 0;
}
#include "capacity_probe_commands.inc"
static int mount_command(const struct shell *sh,size_t argc,char **argv)
{struct command cmd={.task=TASK_MOUNT,.deadline=now(NULL)+120000};
 if(argc!=2||strcmp(argv[1],"confirm"))return -EINVAL;
 return queue(sh,&cmd);}
static int start_command(const struct shell *sh,size_t argc,char **argv)
{
 struct command cmd={.task=TASK_START,.deadline=now(NULL)+120000};
 if(argc!=3||strcmp(argv[2],"confirm-audio")||(strcmp(argv[1],"manual")&&strcmp(argv[1],"continuous")))return -EINVAL;
 cmd.mode=!strcmp(argv[1],"continuous")?RP_CONTINUOUS:RP_MANUAL;return queue(sh,&cmd);
}
static int generated_command(const struct shell *sh,size_t argc,char **argv)
{
 if((argc!=2&&argc!=3)||strcmp(argv[1],"confirm-generated-only")||!diagnostics_idle())return -EPERM;
 uint32_t seconds=12;
 if(argc==3){if(!strcmp(argv[2],"30"))seconds=30;else if(!strcmp(argv[2],"60"))seconds=60;
  else if(!strcmp(argv[2],"300"))seconds=300;else if(!strcmp(argv[2],"3600"))seconds=3600;else return -EINVAL;}
 struct command cmd={.task=TASK_START,.mode=RP_MANUAL,.public_test=seconds,.deadline=now(NULL)+120000};
 return queue(sh,&cmd);
}
/* Cached metadata only. The shared command gate excludes recording/sync actors.
 * It does not mount, load a page, reclaim space or grant write authority. */
static int capacity_command(const struct shell *sh,size_t argc,char **argv)
{
 ARG_UNUSED(argv);
 if(argc!=1||!local(sh)||!atomic_cas(&command_busy,0,1))return -EPERM;
 int rc=-EPERM;struct ros_status s;struct ros_geometry geometry;uint32_t roots=0;
 if(!diagnostics_idle()||!atomic_get(&volume_mounted)||!atomic_get(&volume_suspended)||
    ros_get_geometry(&volume.store,&geometry)||ros_get_status(&volume.store,&s)||!s.mounted||s.fault||s.job!=ROS_NONE||s.consumed_slots>s.slot_count)goto done;
 for(unsigned i=0;i<geometry.roots;++i){struct ros_record r;
  int read=ros_get_record(&volume.store,i,&r);
  if(read==ROS_OK){if(r.present)++roots;}
  else if(read!=ROS_NOT_FOUND)goto done;
 }
 shell_print(sh,"RECORDER_CAPACITY v=1 roots_used=%u roots_total=%u slots_used=%u slots_total=%u free_slots=%u nand_io=0",
  roots,geometry.roots,s.consumed_slots,s.slot_count,s.slot_count-s.consumed_slots);rc=0;
 shell_print(sh,"RECORDER_METADATA format=%u tombstone_limit=%u receipt_limit=%u nand_io=0",
  volume.metadata.format,geometry.profile==2?RLL_TOMBSTONES:volume.metadata.format==2?14U:8U,geometry.profile==2?RLL_SLOTS:RSM_RECEIPTS);
done:atomic_clear(&command_busy);return rc;
}
static void print_timing(const struct shell *sh,const struct recording_timing *p,const char *source)
{
 shell_print(sh,"RECORDER_TIMING source=%s hz=32768 codec_calls=%u codec_ticks=%u codec_max=%u jobs=%u job_ticks=%u job_max=%u nand_io=0 completed_calls_only=1",
  source,p->codec_calls,p->codec_ticks,p->codec_max,p->job_calls,p->job_ticks,p->job_max);
 for(unsigned i=0;i<RT_STAGES;++i)shell_print(sh,"RECORDER_STAGE source=%s stage=%u calls=%u ticks=%u codec_ticks=%u",
  source,i,p->stages[i].calls,p->stages[i].ticks,p->stages[i].codec_ticks);
}
static int timing_command(const struct shell *sh,size_t argc,char **argv)
{
 ARG_UNUSED(argv);if(argc!=1||!local(sh)||!atomic_cas(&command_busy,0,1))return -EPERM;
 int rc=-EPERM;
 if(diagnostics_idle()){
  print_timing(sh,&timing,"current");
  shell_print(sh,"RECORDER_CPU before_hz=%u recording_hz=%u after_hz=%u active=%u configured_only=1",
   cpu_clock.before,cpu_clock.during,cpu_clock.after,cpu_clock.active);rc=0;
 }
 atomic_clear(&command_busy);return rc;
}
static int last_fault_command(const struct shell *sh,size_t argc,char **argv)
{
 ARG_UNUSED(argv);if(argc!=1||sh!=shell_backend_uart_get_ptr())return -EPERM;
 struct recording_fault_trace t={0};int valid=recording_fault_read(&t);
 if(!valid)memset(&t,0,sizeof(t));
 shell_print(sh,"RECORDER_LAST_FAULT v=2 valid=%u line=%u reason=%d uptime=%u task=%u bridge=%u bridge_error=%d job=%u stage=%u store_fault=%u volume=%u volume_fault=%u native_line=%u storing=%u check=%u deadline=%u now=%u guard_codec=%u guard_storage=%u usb_epoch=%u active_epoch=%u lease_epoch=%u configured=%u",
  valid,t.line,(int32_t)t.reason,t.uptime,t.task,t.bridge_state,(int32_t)t.bridge_error,
  t.store_job,t.store_stage,t.store_fault,t.volume_state,t.volume_fault,t.native_line,t.storage_running,
  t.check_code,t.check_deadline,t.check_now,t.guard_codec,t.guard_storage,t.usb_epoch,t.active_epoch,t.lease_epoch,t.usb_configured);
 shell_print(sh,"RECORDER_LAST_PHY line=%u fault=%u opcode=%u row=%u status=%u stopped=%u ready=%u mismatch=%u verified=%u wel=%u native_deadline=%u phy_deadline=%u phy_now=%u hal_valid=%u hal_rc=%d hal_opcode=%u hal_bytes=%u hal_started=%u hal_stopped=%u hal_tx=%u hal_rx=%u hal_before=%u hal_after=%u hal_deadline=%u late_end=%u late_stop=%u",
  t.phy_line,t.phy_fault,t.opcode,t.row,t.status,t.stopped,t.ready,t.verify_mismatch,t.verified,t.wel,
  t.native_deadline,t.phy_deadline,t.phy_now,t.hal_valid,(int32_t)t.hal_rc,t.hal_opcode,t.hal_bytes,
  t.hal_started,t.hal_stopped,t.hal_tx,t.hal_rx,t.hal_before,t.hal_after,t.hal_deadline,t.late_end,t.late_stop);
 shell_print(sh,"RECORDER_LAST_JOB kind=%u sequence=%u started=%u stage_started=%u observed_stage=%u",
  t.recording_job,t.segment_sequence,t.job_started,t.stage_started,t.observed_stage);
 if(valid)print_timing(sh,&t.timing,"fault");
 return 0;
}
static int stop_command(const struct shell *sh,size_t argc,char **argv)
{ARG_UNUSED(argv);if(argc!=1||!local(sh)||!atomic_get(&recording))return -EPERM;
 int rc=rw_request_stop((uint32_t)atomic_get(&epoch),RW_USER);k_sem_give(&codec_wake);return rc;}
static int standby_command(const struct shell *sh,size_t argc,char **argv)
{
 ARG_UNUSED(argv);if(argc!=1||sh!=shell_backend_uart_get_ptr())return -EPERM;
 unsigned key=irq_lock();uint32_t entries=standby_entries,wakes=standby_wakes;
 int sleeping=atomic_get(&runtime_standby);irq_unlock(key);
 shell_print(sh,"STANDBY_STATUS active=%u entries=%u wakes=%u system_off=0 microphone=%u",
  (unsigned)sleeping,entries,wakes,(unsigned)recording_runtime_microphone_power());return 0;
}
SHELL_STATIC_SUBCMD_SET_CREATE(recorder_commands,
 SHELL_CMD_ARG(standby,NULL,"Cached System-ON standby counters; no state changes.",standby_command,1,0),
#ifdef OPENPENDANT_BATTERY_MONITOR
 SHELL_CMD_ARG(batterywatchstart,NULL,"Explicit once-per-boot read-only monitor; no capture or power-policy change.",battery_watch_start,2,0),
 SHELL_CMD_ARG(batterywatch,NULL,"Cached battery monitor, no bus access.",battery_watch_status,1,0),
 SHELL_CMD_ARG(batteryinit,NULL,"Cached monitor initialization evidence, no bus access.",battery_watch_init_status,1,0),
 SHELL_CMD_ARG(batterywatchstop,NULL,"Stop read-only polling; no retry this boot.",battery_watch_stop,1,0),
#endif
#ifdef OPENPENDANT_BATTERY_DIAGNOSTIC
 SHELL_CMD_ARG(batteryprobe,NULL,"Once per boot: flags/voltage only, no gauge writes.",battery_probe_command,2,0),
 SHELL_CMD_ARG(batterystatus,NULL,"Cached battery diagnostic only; no bus access.",battery_status_command,1,0),
 SHELL_CMD_ARG(batteryrestore,NULL,"Explicit once-only factory gauge restoration; no charger changes.",battery_restore_command,2,0),
 SHELL_CMD_ARG(batteryrestored,NULL,"Cached factory gauge restoration evidence; no bus access.",battery_restore_status,1,0),
#endif
 SHELL_CMD_ARG(publictest,NULL,"Generated public noise/tone; default12s, optional30/60/300/3600s; no microphone.",generated_command,2,1),
 SHELL_CMD_ARG(lastfault,NULL,"Retained numeric runtime failure, if valid; no data access.",last_fault_command,1,0),
 SHELL_CMD_ARG(timing,NULL,"Joined cached recorder timing; no audio or NAND access.",timing_command,1,0),
 SHELL_CMD_ARG(capacity,NULL,"Cached mounted capacity only; no NAND access.",capacity_command,1,0),
#ifdef OPENPENDANT_NATIVE_STORAGE
 SHELL_CMD_ARG(fullprepare,NULL,"One-way new full-chip volume, same recipient; fresh boot required before/after.",full_prepare_command,4,0),
 SHELL_CMD_ARG(fullformat,NULL,"One-shot complete external NAND format from prepared full profile; no microphone.",full_format_command,3,0),
 SHELL_CMD_ARG(fullcomplete,NULL,"Explicit phase2 completion after native root verification; no reformat.",full_complete_command,3,0),
 SHELL_CMD_ARG(fullinfo,NULL,"Public full-profile identity and explicit format confirmation; no NAND access.",full_info_command,1,0),
 SHELL_CMD_ARG(metadataextend,NULL,"Explicit v1->v2 metadata copy; preserves all records and deletion history.",metadata_extend_command,2,0),
 SHELL_CMD_ARG(nativeformat,NULL,"Explicit native format of disposable external blocks1025..1056, no audio.",native_format_command,3,0),
#endif
 SHELL_CMD_ARG(dharatest,NULL,"Erase ONLY external blocks1025..1056; synthetic Dhara write/sync/reopen test.",trial_command,2,0),
 SHELL_CMD_ARG(dharacheck,NULL,"Read-only check of synthetic Dhara test after fresh boot.",trial_command,2,0),
 SHELL_CMD_ARG(dharastatus,NULL,"Cached direct Dhara test metadata.",trial_status_command,1,0),
 SHELL_CMD_ARG(capacitytest,NULL,"Erase16fixed sample blocks across512MiB; generated data only; fresh boot required.",capacity_probe_command,2,0),
 SHELL_CMD_ARG(capacitycheck,NULL,"Read-only verification of fixed512MiB address sample after restart.",capacity_probe_command,2,0),
 SHELL_CMD_ARG(capacitystatus,NULL,"Cached capacity-probe results; no NAND I/O.",capacity_probe_status,1,0),
 SHELL_CMD_ARG(recoverpermitstatus,NULL,"Cached one-use host offer; no recovery or disk-durability proof.",hp_runtime_status,1,0),
 SHELL_CMD_ARG(recover,NULL,"Isolated fixed recovery transaction; external durable intent required.",recovery_command,3,0),
 SHELL_CMD_ARG(recoverstatus,NULL,"Joined cached recovery metadata only.",recovery_status_command,1,0),
 SHELL_CMD_ARG(storagefault,NULL,"Cached SPI timing and first provider fault; no device I/O.",storage_fault_command,1,0),
 SHELL_CMD_ARG(status,NULL,"Public recorder status; no recording.",status_command,1,0),
 SHELL_CMD_ARG(diagnostics,NULL,"Cached state, reset flags and current-boot stack watermarks only.",diagnostics_command,1,0),
 SHELL_CMD_ARG(controlprobe,NULL,"One fixed read-only phase2 control-bank scan; no recovery/adoption.",control_probe_command,2,0),
 SHELL_CMD_ARG(controlstatus,NULL,"Cached joined control-probe metadata only.",control_status_command,1,0),
 SHELL_CMD_ARG(controlpage,NULL,"One cached 200-byte control metadata record, index0..127.",control_page_command,2,0),
 SHELL_CMD_ARG(phyprobe,NULL,"One original-PHY read-only timing probe, no writes or retry.",phy_probe_command,2,0),
 SHELL_CMD_ARG(phystatus,NULL,"Cached joined PHY-probe metadata only.",phy_status_command,1,0),
 SHELL_CMD_ARG(preimage,NULL,"Capture one fixed current raw page twice; private archive only, no retry.",preimage_command,3,0),
 SHELL_CMD_ARG(preimagestatus,NULL,"Cached joined current-preimage metadata only.",preimage_status_command,1,0),
 SHELL_CMD_ARG(enrollbegin,NULL,"Stage exact public volume/preservation identity.",begin_enrollment,4,0),
 SHELL_CMD_ARG(recipient,NULL,"Stage owner PUBLIC recipient point.",recipient_command,3,0),
 SHELL_CMD_ARG(enrollcommit,NULL,"Explicit verified-public-recipient enrollment.",commit_enrollment,4,0),
 SHELL_CMD_ARG(provision,NULL,"Explicit exact preserved pool provisioning.",provision_command,3,0),
 SHELL_CMD_ARG(mount,NULL,"Explicit owned volume mount; no audio.",mount_command,2,0),
 SHELL_CMD_ARG(start,NULL,"Explicit audio recording; red light; USB-only engineering.",start_command,3,0),
 SHELL_CMD_ARG(stop,NULL,"Stop and durably finalize recording.",stop_command,1,0),
 SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(recorder,&recorder_commands,"Owner-enrolled storage and recording.",NULL);
