/* Actual production HAL implementation with register/time/IRQ effects modeled.
 * No hardware. Not proof of target interrupt latency or electrical behavior. */
#include <stdio.h>
#include <stdlib.h>
#define BATTERY_PROBE_NATIVE_TEST
#include "../../usb_firmware/src/battery_probe.c"
#include "../../usb_firmware/src/battery_power.c"
static uint64_t checks,groups,time_us,start_us,stop_us;
static unsigned starts,stops,sleeps,writes,output[64];
static unsigned transfer_delay,stop_delay,failure,fail_at,active,stop_active,cancel_at;
static unsigned preemption_us;
static uint16_t words[3];
static unsigned restore_mode,model_case,model_key,model_cfg,model_sealed,model_subclass;
static uint16_t model_cmd,model_flags,model_current;
static unsigned last_sleep_ms;
static unsigned model_cca_reads,model_status_skip;
static uint16_t model_status_extra;
static uint8_t model_state[32],model_gain[32],model_stage[32];
static void model_transfer(void);
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"battery:%d %s\n",__LINE__,#x);abort();}}while(0)
enum {OK,NACK,NEVER_STOP,SHORT_READ,TX_SHORT,NO_COMPLETE,LINES_LOW,DISABLE_FAIL,PSEL_FAIL,ENABLE_FAIL,PIN_FAIL,ERROR_AT_STOP,STUCK_CLOCK};
int64_t k_uptime_get(void){return (int64_t)(time_us/1000);}
uint32_t k_cycle_get_32(void){return failure==STUCK_CLOCK?0:(uint32_t)time_us;}
uint32_t k_us_to_cyc_ceil32(uint32_t u){return u;}
static int failing(void){return starts==fail_at;}
static void advance(void)
{
 if(active && time_us-start_us>=transfer_delay){
  if(failing() && (failure==NACK || failure==NEVER_STOP)){
   mock_twim.events[NRF_TWIM_EVENT_ERROR]=1;mock_twim.errors=2;
  }else if(!(failing() && (failure==NO_COMPLETE || failure==STUCK_CLOCK))){
   if(restore_mode)model_transfer();
   else {CHECK(starts>=1 && starts<=3);rx[0]=(uint8_t)words[starts-1];rx[1]=(uint8_t)(words[starts-1]>>8);}
   mock_twim.TXD.AMOUNT=failing()&&failure==TX_SHORT?0:mock_twim.TXD.MAXCNT;
   if(mock_twim.RXD.MAXCNT)mock_twim.RXD.AMOUNT=failing()&&failure==SHORT_READ?0:mock_twim.RXD.MAXCNT;
   mock_twim.events[NRF_TWIM_EVENT_STOPPED]=1;active=0;
   if(failing()&&failure==ERROR_AT_STOP)mock_twim.errors=4;
  }
 }
 if(stop_active && time_us-stop_us>=stop_delay && failure!=NEVER_STOP){
  mock_twim.events[NRF_TWIM_EVENT_STOPPED]=1;stop_active=active=0;
 }
}
void k_busy_wait(uint32_t u){CHECK(u==1||u==5);time_us+=u;
 if(active&&preemption_us){time_us+=preemption_us;preemption_us=0;}advance();}
void k_msleep(int32_t m){CHECK(m==((restore_mode&&!service_ready)?2000:1000)&&!active&&!stop_active);last_sleep_ms=(unsigned)m;++sleeps;time_us+=(uint64_t)m*1000;}
uint32_t nrf_gpio_pin_read(uint32_t p){CHECK(p==38||p==46);return failure!=LINES_LOW;}
uint32_t nrf_gpio_pin_out_read(uint32_t p){return output[p];}
void nrf_gpio_pin_set(uint32_t p){CHECK(p==38||p==46);++writes;output[p]=1;}
void nrf_gpio_cfg_default(uint32_t p){CHECK(!active&&!stop_active);++writes;mock_gpio.PIN_CNF[p-32]=2;}
void nrf_gpio_cfg(uint32_t p,uint32_t d,uint32_t i,uint32_t pull,uint32_t drive,uint32_t sense)
{CHECK((p==38||p==46)&&!d&&!i&&!pull&&!sense&&drive==NRF_GPIO_PIN_S0D1);++writes;mock_gpio.PIN_CNF[p-32]=failure==PIN_FAIL?999:BP_PIN_CNF;}
void nrf_twim_pins_set(NRF_TWIM_Type *p,uint32_t c,uint32_t d)
{++writes;p->PSEL.SCL=failure==PSEL_FAIL&&c!=UINT32_MAX?999:c;p->PSEL.SDA=d;}
void nrf_twim_tx_buffer_set(NRF_TWIM_Type*p,const uint8_t*b,size_t n)
{CHECK(!active&&!stop_active&&n<=33&&(restore_mode||n<=1));++writes;p->TXD.PTR=(uintptr_t)b;p->TXD.MAXCNT=(uint32_t)n;}
void nrf_twim_rx_buffer_set(NRF_TWIM_Type*p,uint8_t*b,size_t n)
{CHECK(!active&&!stop_active&&n<=32&&(restore_mode||n==0||n==2));++writes;p->RXD.PTR=(uintptr_t)b;p->RXD.MAXCNT=(uint32_t)n;}
void nrf_twim_shorts_set(NRF_TWIM_Type*p,uint32_t v){CHECK(v==0||v==BP_SHORTS||(restore_mode&&v==4));++writes;p->SHORTS=v;}
void nrf_twim_disable(NRF_TWIM_Type*p){CHECK(!active&&!stop_active);++writes;if(failure!=DISABLE_FAIL)p->ENABLE=0;}
void nrf_twim_enable(NRF_TWIM_Type*p){++writes;p->ENABLE=failure==ENABLE_FAIL?0:6;}
void nrf_twim_frequency_set(NRF_TWIM_Type*p,uint32_t v){CHECK(v==NRF_TWIM_FREQ_100K);++writes;p->FREQUENCY=v;}
void nrf_twim_address_set(NRF_TWIM_Type*p,uint8_t a){CHECK(a==0x55);++writes;p->ADDRESS=a;}
void nrf_twim_event_clear(NRF_TWIM_Type*p,unsigned e){++writes;p->events[e]=0;}
int nrf_twim_event_check(NRF_TWIM_Type*p,unsigned e){advance();return p->events[e]!=0;}
uint32_t nrf_twim_errorsrc_get_and_clear(NRF_TWIM_Type*p){uint32_t v=p->errors;p->errors=0;return v;}
uint32_t nrf_twim_txd_amount_get(NRF_TWIM_Type*p){return p->TXD.AMOUNT;}
uint32_t nrf_twim_rxd_amount_get(NRF_TWIM_Type*p){return p->RXD.AMOUNT;}
void nrf_twim_task_trigger(NRF_TWIM_Type*p,unsigned task)
{
 CHECK(p==&mock_twim);
 if(task==NRF_TWIM_TASK_STARTTX){
  CHECK((restore_mode||starts<3)&&!active&&!stop_active&&p->ENABLE==6&&p->ADDRESS==0x55);
  if(!restore_mode)CHECK(p->TXD.MAXCNT==1&&p->RXD.MAXCNT==2&&tx[0]==selectors[starts]);
  CHECK(sleeps==starts+1&&p->SHORTS==(p->RXD.MAXCNT?BP_SHORTS:4));
  if(starts)CHECK(time_us-start_us>=(uint64_t)last_sleep_ms*1000U);
  ++starts;start_us=time_us;active=1;
 }else{CHECK(task==NRF_TWIM_TASK_STOP&&p->SHORTS==0);++stops;stop_us=time_us;stop_active=1;}
}
static int allowed(void*u){CHECK(u==&mock_twim);return !cancel_at||sleeps<cancel_at;}
static void reset(void)
{
 ++groups;memset(&mock_twim,0,sizeof(mock_twim));memset(&mock_gpio,0,sizeof(mock_gpio));
 memset(output,0,sizeof(output));memset(tx,0,sizeof(tx));memset(rx,0,sizeof(rx));
 used=0;stopped=released=1;fault=0;starts=stops=sleeps=writes=active=stop_active=cancel_at=0;
 service_ready=service_sequence=service_deferred=0;
 model_cca_reads=model_status_skip=model_status_extra=0;
 memset(&service_initial,0,sizeof(service_initial));
 preemption_us=0;
 time_us=0;transfer_delay=4400;stop_delay=4000;failure=OK;fail_at=1;restore_mode=0;
 mock_twim.PSEL.SDA=mock_twim.PSEL.SCL=UINT32_MAX;
 mock_gpio.PIN_CNF[6]=mock_gpio.PIN_CNF[14]=2;words[0]=words[2]=0x0188;words[1]=3987;
}
#include "battery_restore_model.inc"
#include "battery_service_model.inc"
__declspec(dllexport) int battery_tests(uint64_t*out)
{
 struct battery_probe_result r,old;
 reset();CHECK(!battery_probe_run(&r,allowed,&mock_twim));
 CHECK(r.trusted&&r.coherent&&r.reads==3&&r.stopped&&r.released&&!r.fault&&starts==3&&!stops);
 CHECK(r.sample[1].raw==3987&&r.elapsed_ms>=3000&&r.elapsed_ms<3100&&idle());
 for(unsigned i=1;i<3;++i)CHECK(r.sample[i].started_ms-r.sample[i-1].finished_ms>=1000);
 old=r;CHECK(battery_probe_run(&r,allowed,&mock_twim)==-EALREADY&&!memcmp(&old,&r,sizeof(r))&&starts==3);
 for(unsigned f=0;f<=65535;++f){
  int expected=(f&8)!=0 && !(f&(0x3c00|0x30));
  CHECK(battery_probe_flags_trusted((uint16_t)f)==expected);
 }
 reset();words[0]=words[2]=0x0120;CHECK(!battery_probe_run(&r,allowed,&mock_twim)&&!r.trusted&&r.coherent&&r.reads==3);
 reset();words[2]^=1;CHECK(!battery_probe_run(&r,allowed,&mock_twim)&&!r.trusted&&!r.coherent);
 for(unsigned v=0;v<2;++v){reset();words[1]=v?6001:0;CHECK(!battery_probe_run(&r,allowed,&mock_twim)&&!r.trusted);}
 for(unsigned err=NACK;err<=STUCK_CLOCK;++err){
  reset();failure=err;CHECK(battery_probe_run(&r,allowed,&mock_twim)!=0&&!r.trusted);
  CHECK(starts<=1&&r.elapsed_ms<1200);
  if(err==NEVER_STOP){CHECK(r.fault&&!r.stopped&&!r.released&&mock_twim.TXD.PTR==(uintptr_t)tx);}
  else if(err==DISABLE_FAIL){CHECK(r.fault&&r.stopped&&!r.released);}
  else CHECK(r.stopped&&r.released&&idle());
 }
 for(unsigned i=1;i<=4;++i){
  reset();cancel_at=i;int result=battery_probe_run(&r,allowed,&mock_twim);
  CHECK(i<=3?(result!=0&&starts==i-1):(result==0&&starts==3));CHECK(r.stopped&&r.released);
 }
 reset();mock_twim.SUBSCRIBE_STOP=1;CHECK(battery_probe_run(&r,allowed,&mock_twim)!=0&&writes==0&&starts==0&&r.fault&&!r.released);
 reset();mock_gpio.PIN_CNF[6]=0;CHECK(battery_probe_run(&r,allowed,&mock_twim)!=0&&writes==0&&r.fault);
 for(unsigned i=2;i<=3;++i){reset();failure=NACK;fail_at=i;CHECK(battery_probe_run(&r,allowed,&mock_twim)!=0&&starts==i&&r.reads==i&&r.released);}
 reset();time_us=UINT32_MAX-1500000ULL;CHECK(!battery_probe_run(&r,allowed,&mock_twim)&&r.trusted&&r.elapsed_ms<3100);
 restore_tests();service_tests();out[0]=checks;out[1]=groups;return 0;
}
