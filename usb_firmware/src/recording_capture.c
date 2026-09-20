/* SPDX-License-Identifier: Apache-2.0 */
#include "recording_capture.h"
#include "recording_worker.h"
#include "mic_diagnostics.h"
#include "mic_commands.h"
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>
#include <soc.h>
#include <zephyr/audio/dmic.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

#define RC_BYTES 640U
#define RC_BLOCKS 6U
#define RC_CLOCK 0x0a0a0000U
BUILD_ASSERT(!IS_ENABLED(CONFIG_SMP),"Capture metadata publication requires the reviewed single-core app");
BUILD_ASSERT(DT_ENUM_IDX(DT_NODELABEL(pdm0),clock_source)==1,"PCLK32M_HFXO required");
BUILD_ASSERT(DT_SAME_NODE(DT_GPIO_CTLR(DT_NODELABEL(pendant_mic_power),gpios),DT_NODELABEL(gpio1)) &&
 DT_GPIO_PIN(DT_NODELABEL(pendant_mic_power),gpios)==1 &&
 DT_GPIO_FLAGS(DT_NODELABEL(pendant_mic_power),gpios)==GPIO_ACTIVE_HIGH,"Confirmed active-high P1.01 required");
static const struct gpio_dt_spec rc_power=GPIO_DT_SPEC_GET(DT_NODELABEL(pendant_mic_power),gpios);
static const struct device *const rc_pdm=DEVICE_DT_GET(DT_NODELABEL(pdm0));
static const NRF_PDM_Type *const rc_regs=(const NRF_PDM_Type*)DT_REG_ADDR(DT_NODELABEL(pdm0));
K_MEM_SLAB_DEFINE_STATIC(rc_pcm_slab,RC_BYTES,RC_BLOCKS,4);
K_MUTEX_DEFINE(rc_control);
K_SEM_DEFINE(rc_request,0,1);
K_SEM_DEFINE(rc_done,0,1);
static struct recording_capture_hooks rc_hooks;
static atomic_t rc_state,rc_epoch,rc_stop,rc_joined,rc_fault,rc_powered;
static atomic_t rc_stopped=ATOMIC_INIT(1),rc_scrubbed,rc_error,rc_submit_result;
static atomic_t rc_returned,rc_submitted,rc_warmed;
/* first/last are immutable while the acquisition actor is published. deadline
 * is one write under irq_lock and read under irq_lock; never a torn ARM u64. */
static uint64_t rc_first,rc_stop_deadline,rc_last_time;
static struct pcm_stream_cfg rc_stream;
static struct dmic_cfg rc_cfg;
static bool rc_configured;

static void rc_wipe(void *span)
{volatile uint8_t *p=span;for(unsigned i=0;i<RC_BYTES;++i)p[i]=0;}
static bool rc_owned(void *span)
{uintptr_t base=(uintptr_t)rc_pcm_slab.buffer,p=(uintptr_t)span;
 return p>=base && p-base<RC_BYTES*RC_BLOCKS && (p-base)%RC_BYTES==0;}
static _Noreturn void rc_die(int reason)
{
 atomic_set(&rc_fault,1);atomic_set(&rc_state,RC_FAULT);atomic_set(&rc_error,reason);
 /* Power-off does not assert the clock/DMA is stopped. Never wipe an
  * outstanding block on this path, even if reset unexpectedly returns. */
 if(gpio_pin_set_dt(&rc_power,0)==0)atomic_clear(&rc_powered);
 rc_hooks.fatal(rc_hooks.user,reason);sys_reboot(SYS_REBOOT_COLD);for(;;){}
}
static uint64_t rc_now(void)
{int64_t value=k_uptime_get();if(value<0 || (uint64_t)value<rc_last_time)rc_die(-ETIMEDOUT);
 unsigned key=irq_lock();rc_last_time=(uint64_t)value;irq_unlock(key);return (uint64_t)value;}
static void rc_release(void *buffer)
{if(!rc_owned(buffer))rc_die(-EFAULT);rc_wipe(buffer);k_mem_slab_free(&rc_pcm_slab,buffer);}
static bool rc_scrub_free(void)
{
 void *owned[RC_BLOCKS]={0};unsigned count=0;
 while(count<RC_BLOCKS && k_mem_slab_alloc(&rc_pcm_slab,&owned[count],K_NO_WAIT)==0){
  if(!rc_owned(owned[count]))rc_die(-EFAULT);
  rc_wipe(owned[count]);++count;
 }
 for(unsigned i=0;i<count;++i)k_mem_slab_free(&rc_pcm_slab,owned[i]);
 return count==RC_BLOCKS;
}
static uint64_t rc_cleanup_deadline(uint64_t fixed)
{unsigned key=irq_lock();uint64_t requested=rc_stop_deadline;irq_unlock(key);
 return requested && requested<fixed?requested:fixed;}
static void rc_finish_capture(void)
{
 atomic_set(&rc_state,RC_STOPPING);uint64_t until=rc_now()+RECORDING_CAPTURE_STOP_MS;
 int stop_error=rc_configured?dmic_trigger(rc_pdm,DMIC_TRIGGER_STOP):0;
 if(stop_error)rc_die(stop_error);
 if(rc_configured){
  rc_stream.pcm_rate=0;
  for(unsigned attempts=0;attempts<=RECORDING_CAPTURE_STOP_MS;++attempts){
   if(rc_now()>=rc_cleanup_deadline(until))rc_die(-ETIMEDOUT);
   for(unsigned i=0;i<RC_BLOCKS;++i){void *buffer=NULL;size_t size=0;
    int got=dmic_read(rc_pdm,0,&buffer,&size,0);
    if(got){if(buffer)rc_die(-EFAULT);break;}
    if(!buffer)rc_die(-EFAULT);
    rc_release(buffer);
   }
   if(k_mem_slab_num_used_get(&rc_pcm_slab)==0){
    int disabled=dmic_configure(rc_pdm,&rc_cfg);
    if(rc_now()>=rc_cleanup_deadline(until))rc_die(-ETIMEDOUT);
    if(!disabled){rc_configured=false;atomic_set(&rc_stopped,1);break;}
    if(disabled!=-EBUSY)rc_die(disabled);
   }
   if(attempts==RECORDING_CAPTURE_STOP_MS)rc_die(-ETIMEDOUT);
   k_msleep(1);
  }
 }
 if(!atomic_get(&rc_stopped) || !rc_scrub_free())rc_die(-EIO);
 atomic_set(&rc_scrubbed,1);
 int off=gpio_pin_set_dt(&rc_power,0);if(off)rc_die(off);atomic_clear(&rc_powered);
 if(rc_now()>=rc_cleanup_deadline(until))rc_die(-ETIMEDOUT);
 rc_hooks.wake_worker(rc_hooks.user); /* all producer callbacks precede join */
 if(rc_now()>=rc_cleanup_deadline(until))rc_die(-ETIMEDOUT);
 atomic_set(&rc_state,atomic_get(&rc_fault)?RC_FAULT:RC_JOINED);
 atomic_set(&rc_joined,1);k_sem_give(&rc_done);
}
static void rc_capture_failure(int error)
{
 atomic_set(&rc_error,error);atomic_set(&rc_fault,1);atomic_set(&rc_stop,1);
 (void)rw_request_stop((uint32_t)atomic_get(&rc_epoch),RW_EXTERNAL);
}
static bool rc_owner(void)
{return mic_commands_busy() && rc_hooks.owner_valid(rc_hooks.user,(uint32_t)atomic_get(&rc_epoch))==1;}
static bool rc_configuration(void)
{
 return rc_regs->PDMCLKCTRL==RC_CLOCK && rc_regs->RATIO==PDM_RATIO_RATIO_Ratio80 &&
 rc_regs->MCLKCONFIG==PDM_MCLKCONFIG_SRC_PCLK32M && rc_regs->MODE==
 ((PDM_MODE_OPERATION_Mono<<PDM_MODE_OPERATION_Pos)|(PDM_MODE_EDGE_LeftFalling<<PDM_MODE_EDGE_Pos)) &&
 rc_regs->GAINL==PDM_GAINL_GAINL_DefaultGain && rc_regs->GAINR==PDM_GAINR_GAINR_DefaultGain &&
 rc_regs->PSEL.CLK==4 && rc_regs->PSEL.DIN==5 && rc_cfg.channel.act_num_streams==1 &&
 rc_cfg.channel.act_num_chan==1 && rc_cfg.channel.act_chan_map_lo==rc_cfg.channel.req_chan_map_lo &&
 !rc_cfg.channel.act_chan_map_hi && rc_stream.pcm_rate==16000 && rc_stream.pcm_width==16 && rc_stream.block_size==RC_BYTES;
}
static void rc_capture_run(void)
{
 uint64_t last_frame=rc_now();
 while(!atomic_get(&rc_stop)){
  if(!rc_owner()){rc_capture_failure(-EPERM);break;}
  void *buffer=NULL;size_t size=0;
  int got=dmic_read(rc_pdm,0,&buffer,&size,RECORDING_CAPTURE_READ_MS);
  uint64_t now=rc_now();
  if(got){
   if(buffer)rc_die(-EFAULT);
   if(atomic_get(&rc_stop))break;
   if((got==-EAGAIN || got==-ENOMSG) && now-last_frame<RECORDING_CAPTURE_STALL_MS)continue;
   rc_capture_failure(got);break;
  }
  if(!buffer || !rc_owned(buffer))rc_die(-EFAULT);
  if(size!=RC_BYTES || now-last_frame>=RECORDING_CAPTURE_STALL_MS){
   rc_release(buffer);rc_capture_failure(size!=RC_BYTES?-EIO:-ETIMEDOUT);break;
  }
  if((uint32_t)atomic_get(&rc_returned)==INT32_MAX){rc_release(buffer);rc_capture_failure(-EOVERFLOW);break;}
  last_frame=now;atomic_inc(&rc_returned);
  if(atomic_get(&rc_stop)){rc_release(buffer);break;}
  if(!rc_owner()){rc_release(buffer);rc_capture_failure(-EPERM);break;}
  if((uint32_t)atomic_get(&rc_warmed)<RECORDING_CAPTURE_WARMUP_FRAMES){
   atomic_inc(&rc_warmed);rc_release(buffer);
   if((uint32_t)atomic_get(&rc_warmed)==RECORDING_CAPTURE_WARMUP_FRAMES)atomic_set(&rc_state,RC_RUNNING);
   continue;
  }
  uint32_t frames=(uint32_t)atomic_get(&rc_submitted);
  if(frames==INT32_MAX || rc_first>UINT64_MAX-(uint64_t)(frames+1U)*320U){
   rc_release(buffer);rc_capture_failure(-EOVERFLOW);break;
  }
  int submitted=rw_submit((uint32_t)atomic_get(&rc_epoch),rc_first+(uint64_t)frames*320U,buffer);
  rc_release(buffer);atomic_set(&rc_submit_result,submitted);
  if(submitted==RW_OK)atomic_inc(&rc_submitted);
  else {
   /* USER stop can race a frame already returned by DMIC. The recorder owns
    * its first reason; no rejected frame is retried or counted as captured. */
   atomic_set(&rc_stop,1);
   if(submitted!=RW_STOPPED_INPUT && submitted!=RW_BUSY)rc_capture_failure(-EIO);
  }
  rc_hooks.wake_worker(rc_hooks.user);
 }
 rc_finish_capture();
}
static void rc_acquisition_thread(void *a,void *b,void *c)
{ARG_UNUSED(a);ARG_UNUSED(b);ARG_UNUSED(c);for(;;){if(k_sem_take(&rc_request,K_FOREVER)==0)rc_capture_run();}}
K_THREAD_DEFINE(recording_capture_thread,RECORDING_CAPTURE_STACK_BYTES,rc_acquisition_thread,
 NULL,NULL,NULL,RECORDING_CAPTURE_PRIORITY,0,0);

int recording_capture_init(const struct recording_capture_hooks *hooks)
{
 if(!hooks || !hooks->owner_valid || !hooks->wake_worker || !hooks->fatal)return -EINVAL;
 if(k_mutex_lock(&rc_control,K_NO_WAIT))return -EBUSY;
 int result=-EBUSY;struct mic_diagnostics_state state={0};mic_diagnostics_get_state(&state);
 if(atomic_get(&rc_state)!=RC_UNINITIALIZED || mic_commands_busy())goto done;
 if(!state.initialized || state.busy || state.powered || state.fault_latched || !state.clock_stopped ||
    !state.buffers_scrubbed || !gpio_is_ready_dt(&rc_power) || !device_is_ready(rc_pdm)){result=-ENODEV;goto done;}
 rc_hooks=*hooks;rc_last_time=0;
 if(!rc_scrub_free()){atomic_set(&rc_fault,1);atomic_set(&rc_state,RC_FAULT);result=-EIO;goto done;}
 atomic_set(&rc_scrubbed,1);atomic_set(&rc_joined,1);atomic_set(&rc_state,RC_IDLE);result=0;
done:k_mutex_unlock(&rc_control);return result;
}
int recording_capture_start(uint32_t epoch,uint64_t first,uint64_t deadline)
{
 if(!epoch || epoch>RW_EPOCH_MAX || first>UINT64_MAX-320U)return -EINVAL;
 if(k_mutex_lock(&rc_control,K_NO_WAIT))return -EBUSY;
 int result=-EBUSY;uint32_t state=(uint32_t)atomic_get(&rc_state);
 if((state!=RC_IDLE && state!=RC_JOINED) || atomic_get(&rc_fault) || !atomic_get(&rc_joined) ||
    !atomic_get(&rc_stopped) || !atomic_get(&rc_scrubbed) || atomic_get(&rc_powered) ||
    epoch<=(uint32_t)atomic_get(&rc_epoch))goto done;
 uint64_t now=rc_now();if(deadline<=now || deadline-now>RW_CALL_BUDGET_MAX_MS){result=-ETIMEDOUT;goto done;}
 if(!mic_commands_busy() || rc_hooks.owner_valid(rc_hooks.user,epoch)!=1){result=-EPERM;goto done;}
 if(rc_now()>=deadline){result=-ETIMEDOUT;goto done;}
 atomic_set(&rc_epoch,epoch);rc_first=first;rc_stop_deadline=0;
 atomic_clear(&rc_stop);atomic_clear(&rc_joined);atomic_clear(&rc_error);atomic_clear(&rc_submit_result);
 atomic_clear(&rc_returned);atomic_clear(&rc_submitted);atomic_clear(&rc_warmed);
 k_sem_reset(&rc_done);atomic_set(&rc_state,RC_STARTING);
 rc_stream=(struct pcm_stream_cfg){.pcm_rate=16000,.pcm_width=16,.block_size=RC_BYTES,.mem_slab=&rc_pcm_slab};
 rc_cfg=(struct dmic_cfg){.io={.min_pdm_clk_freq=1280000,.max_pdm_clk_freq=1280000,.min_pdm_clk_dc=50,.max_pdm_clk_dc=50},
  .streams=&rc_stream,.channel={.req_num_streams=1,.req_num_chan=1}};
 rc_cfg.channel.req_chan_map_lo=dmic_build_channel_map(0,0,PDM_CHAN_LEFT);
 result=dmic_configure(rc_pdm,&rc_cfg);
 if(!result)rc_configured=true;
 if(!result && !rc_configuration())result=-EINVAL;
 if(!result && (!rc_owner() || rc_now()>=deadline))result=-ETIMEDOUT;
 if(!result){result=gpio_pin_set_dt(&rc_power,1);if(!result)atomic_set(&rc_powered,1);}
 if(!result && rc_now()>=deadline)result=-ETIMEDOUT;
 if(!result){k_msleep(50);if(!rc_owner() || rc_now()>=deadline)result=-ETIMEDOUT;}
 if(!result){atomic_clear(&rc_stopped);atomic_clear(&rc_scrubbed);result=dmic_trigger(rc_pdm,DMIC_TRIGGER_START);
  if(!result && rc_now()>=deadline)result=-ETIMEDOUT;}
 if(result){
  atomic_set(&rc_error,result);atomic_set(&rc_fault,1);rc_stop_deadline=deadline;rc_finish_capture();
 }else{atomic_set(&rc_state,RC_WARMING);k_sem_give(&rc_request);}
done:k_mutex_unlock(&rc_control);return result;
}
int recording_capture_stop_and_join(uint32_t epoch,uint64_t deadline)
{
 if(!epoch || epoch>RW_EPOCH_MAX)return -EINVAL;
 if(k_mutex_lock(&rc_control,K_NO_WAIT))return -EBUSY;
 int result=-EINVAL;
 if(epoch!=(uint32_t)atomic_get(&rc_epoch))goto done;
 if(atomic_get(&rc_joined)){
  result=atomic_get(&rc_stopped) && atomic_get(&rc_scrubbed) && !atomic_get(&rc_powered)?0:-EIO;goto done;
 }
 int64_t observed=k_uptime_get();unsigned time_key=irq_lock();uint64_t previous=rc_last_time;irq_unlock(time_key);
 if(observed<0 || (uint64_t)observed<previous)rc_die(-ETIMEDOUT);
 uint64_t now=(uint64_t)observed;
 if(deadline<=now || deadline-now>RW_CALL_BUDGET_MAX_MS)rc_die(-ETIMEDOUT);
 unsigned key=irq_lock();if(!rc_stop_deadline || deadline<rc_stop_deadline)rc_stop_deadline=deadline;
 atomic_set(&rc_stop,1);irq_unlock(key);
 result=k_sem_take(&rc_done,K_MSEC(deadline-now));
 if(result || (uint64_t)k_uptime_get()>=deadline || !atomic_get(&rc_joined) || !atomic_get(&rc_stopped) ||
    !atomic_get(&rc_scrubbed) || atomic_get(&rc_powered))rc_die(-ETIMEDOUT);
 result=0;
done:k_mutex_unlock(&rc_control);return result;
}
int recording_capture_get_status(struct recording_capture_status *out)
{
 if(!out)return -EINVAL;
 if(k_mutex_lock(&rc_control,K_NO_WAIT))return -EBUSY;
 unsigned key=irq_lock();uint32_t frames=(uint32_t)atomic_get(&rc_submitted);
 *out=(struct recording_capture_status){.state=(uint32_t)atomic_get(&rc_state),.epoch=(uint32_t)atomic_get(&rc_epoch),
 .warmup_frames=(uint32_t)atomic_get(&rc_warmed),.submitted_frames=frames,.returned_frames=(uint32_t)atomic_get(&rc_returned),
 .power_on=(uint32_t)atomic_get(&rc_powered),.clock_stopped=(uint32_t)atomic_get(&rc_stopped),
 .buffers_scrubbed=(uint32_t)atomic_get(&rc_scrubbed),.joined=(uint32_t)atomic_get(&rc_joined),
 .fault=(uint32_t)atomic_get(&rc_fault),.capture_error=(int32_t)atomic_get(&rc_error),
 .submit_rc=(int32_t)atomic_get(&rc_submit_result),.first_sample=rc_first,.next_sample=rc_first+(uint64_t)frames*320U};
 irq_unlock(key);k_mutex_unlock(&rc_control);return 0;
}
