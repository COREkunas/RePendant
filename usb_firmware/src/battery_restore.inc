/* Included in the fixed TWIM1 owner. TI SLUUCD5B + verified factory1.1.20.
 * All addresses and commands below are internal constants. No shell-supplied
 * bus/register/value, scan, charger access, chemistry change or full RESET.
 * Permanent DMA remains owned on unproven STOP; no bus recovery or I/O retries.
 */
struct restore_actor {
 struct battery_restore_result *out;
 int (*allow)(void*);
 void *user;
 int64_t begin;
};
static int restore_transfer(struct restore_actor *a,const uint8_t *data,unsigned nt,
                            uint8_t *dest,unsigned nr)
{
 if(!nt || nt>sizeof(tx) || nr>sizeof(rx) || !data || (nr && !dest) || fault ||
    !stopped || !released)return -EPERM;
 /* Two seconds before EVERY transfer satisfies standard-command pacing,
  * Control result settling and the 1100ms SET_CFGUPDATE minimum. */
 k_msleep(2000);
 if(!a->allow(a->user) || k_uptime_get()-a->begin>=170000)return -ECANCELED;
 if(!idle()){stopped=released=0;fault=1;return -EBUSY;}
 released=0;
 nrf_gpio_pin_set(BP_SDA);nrf_gpio_pin_set(BP_SCL);
 nrf_gpio_cfg(BP_SDA,NRF_GPIO_PIN_DIR_INPUT,NRF_GPIO_PIN_INPUT_CONNECT,
  NRF_GPIO_PIN_NOPULL,NRF_GPIO_PIN_S0D1,NRF_GPIO_PIN_NOSENSE);
 nrf_gpio_cfg(BP_SCL,NRF_GPIO_PIN_DIR_INPUT,NRF_GPIO_PIN_INPUT_CONNECT,
  NRF_GPIO_PIN_NOPULL,NRF_GPIO_PIN_S0D1,NRF_GPIO_PIN_NOSENSE);
 int rc=-EIO;
 if(NRF_P1->PIN_CNF[6]!=BP_PIN_CNF || NRF_P1->PIN_CNF[14]!=BP_PIN_CNF ||
    nrf_gpio_pin_out_read(BP_SDA)!=1 || nrf_gpio_pin_out_read(BP_SCL)!=1){fault=1;goto done;}
 k_busy_wait(5);if(!lines())goto done;
 nrf_twim_frequency_set(bp_twim,NRF_TWIM_FREQ_100K);nrf_twim_address_set(bp_twim,0x55);
 nrf_twim_pins_set(bp_twim,BP_SCL,BP_SDA);
 if(bp_twim->FREQUENCY!=(uint32_t)NRF_TWIM_FREQ_100K || bp_twim->ADDRESS!=0x55 ||
    bp_twim->PSEL.SDA!=BP_SDA || bp_twim->PSEL.SCL!=BP_SCL){fault=1;goto done;}
 nrf_twim_enable(bp_twim);
 if(bp_twim->ENABLE!=TWIM_ENABLE_ENABLE_Enabled){fault=1;goto done;}
 memcpy(tx,data,nt);memset(rx,0,sizeof(rx));
 nrf_twim_event_clear(bp_twim,NRF_TWIM_EVENT_STOPPED);
 nrf_twim_event_clear(bp_twim,NRF_TWIM_EVENT_ERROR);
 nrf_twim_event_clear(bp_twim,NRF_TWIM_EVENT_LASTTX);
 nrf_twim_event_clear(bp_twim,NRF_TWIM_EVENT_LASTRX);
 (void)nrf_twim_errorsrc_get_and_clear(bp_twim);
 nrf_twim_tx_buffer_set(bp_twim,tx,nt);nrf_twim_rx_buffer_set(bp_twim,rx,nr);
 uint32_t shortcuts=nr?BP_SHORTS:NRF_TWIM_SHORT_LASTTX_STOP_MASK;
 nrf_twim_shorts_set(bp_twim,shortcuts);
 if(bp_twim->TXD.MAXCNT!=nt || bp_twim->RXD.MAXCNT!=nr ||
    bp_twim->TXD.PTR!=(uintptr_t)tx || bp_twim->RXD.PTR!=(uintptr_t)rx ||
    bp_twim->SHORTS!=shortcuts)goto done;
 ++a->out->transfers;if(nt>1)++a->out->writes;
 stopped=0;__DMB();nrf_twim_task_trigger(bp_twim,NRF_TWIM_TASK_STARTTX);
 rc=wait_stop(25000,1);nrf_twim_shorts_set(bp_twim,0);
 if(!nrf_twim_event_check(bp_twim,NRF_TWIM_EVENT_STOPPED)){
  nrf_twim_task_trigger(bp_twim,NRF_TWIM_TASK_STOP);
  if(wait_stop(10000,0)){fault=1;return -EBUSY;}
 }
 __DMB();stopped=1;
 uint32_t errors=nrf_twim_errorsrc_get_and_clear(bp_twim);
 /* RXD.AMOUNT is the last RX transfer, not reset by a TX-only STARTTX. */
 if(!rc && (errors || nrf_twim_txd_amount_get(bp_twim)!=nt ||
    (nr && nrf_twim_rxd_amount_get(bp_twim)!=nr) || !lines()))rc=-EIO;
 if(!rc && nr)memcpy(dest,rx,nr);
done:
 cleanup();return fault?-EIO:rc;
}
static int restore_read(struct restore_actor *a,uint8_t reg,uint16_t *v)
{
 uint8_t b[2];int rc=restore_transfer(a,&reg,1,b,2);
 if(!rc)*v=(uint16_t)(b[0]|((uint16_t)b[1]<<8));
 return rc;
}
static int restore_control(struct restore_actor *a,uint16_t cmd)
{uint8_t b[3]={0,(uint8_t)cmd,(uint8_t)(cmd>>8)};return restore_transfer(a,b,3,NULL,0);}
static int restore_query(struct restore_actor *a,uint16_t cmd,uint16_t *v)
{int rc=restore_control(a,cmd);return rc?rc:restore_read(a,0,v);}
static int restore_byte(struct restore_actor *a,uint8_t reg,uint8_t v)
{uint8_t b[2]={reg,v};return restore_transfer(a,b,2,NULL,0);}
static uint8_t restore_checksum(const uint8_t *b)
{unsigned sum=0;for(unsigned i=0;i<32;++i)sum+=b[i];return (uint8_t)(255U-(sum&255U));}
static int restore_block(struct restore_actor *a,uint8_t *b)
{
 uint8_t reg=0x40,cs=0;int rc=restore_transfer(a,&reg,1,b,32);if(rc)return rc;
 reg=0x60;rc=restore_transfer(a,&reg,1,&cs,1);
 return rc?rc:(restore_checksum(b)==cs?0:-EIO);
}
static int restore_select(struct restore_actor *a,uint8_t subclass)
{
 int rc=restore_byte(a,0x3e,subclass);return rc?rc:restore_byte(a,0x3f,0);
}
static void restore_word(uint8_t *b,unsigned off,uint16_t value)
{b[off]=(uint8_t)(value>>8);b[off+1]=(uint8_t)value;}
static int restore_status_valid(uint16_t s)
{/* INITCOMP, no shutdown/watchdog/calibration/reserved/chemistry transition. */
 return (s&0x0080U)!=0 && (s&0xdc61U)==0;
}
static int restore_settled_status(struct restore_actor *a,uint16_t *s)
{
 /* TI SLUUCD5B table5-3: CCA is automatic offset calibration, not a latched
  * fault. Observe it finishing before admission/configuration. ONLY CCA may
  * defer this read; BCA/CALMODE/reset/shutdown/identity/transport failures
  * remain terminal. Up to16 successful observations (64s at fixed pacing),
  * still within the actor's170s total budget. No write/failed-I/O replay. */
 for(unsigned n=0;n<16;++n){
  int rc=restore_query(a,0,s);if(rc)return rc;
  if(restore_status_valid(*s))return 0;
  if(!(*s&0x0800U)||!restore_status_valid((uint16_t)(*s&~0x0800U)))return -EPERM;
 }
 return -ETIMEDOUT;
}
static int restore_flags_valid(uint16_t f,int initializing)
{return (f&8U)!=0 && !(f&0xfc10U) && (initializing || !(f&0x20U));}
static int restore_sequence(struct restore_actor *a)
{
 struct battery_restore_result *r=a->out;uint16_t value;uint8_t expect[33];int rc;
#define BR_DO(x) do {rc=(x);if(rc)return rc;}while(0)
#define BR_NEED(x) do {if(!(x))return -EPERM;}while(0)
 r->phase=1;
 BR_DO(restore_query(a,1,&r->device_type));BR_NEED(r->device_type==0x0427);
 BR_DO(restore_query(a,2,&r->fw_version));BR_NEED(r->fw_version==0x0202);
 BR_DO(restore_query(a,8,&r->chem_before));BR_NEED(r->chem_before==0x3230);
 BR_DO(restore_settled_status(a,&r->status_before));
 BR_DO(restore_read(a,6,&r->flags_before));BR_NEED(restore_flags_valid(r->flags_before,1));
 BR_DO(restore_read(a,4,&r->voltage_before));
 BR_NEED(r->voltage_before>=3500 && r->voltage_before<=4450);
 /* No gauge configuration until identity, settled status and voltage qualify.
  * From here, any failure is unresolved and recording remains fenced. */
 r->phase=2;r->mutated=1;
 if(r->status_before&0x2000U){
  BR_DO(restore_control(a,0x8000));BR_DO(restore_control(a,0x8000));
 }
 BR_DO(restore_settled_status(a,&value));BR_NEED(!(value&0x2000U));
 r->phase=3;
 BR_DO(restore_control(a,0x13));BR_DO(restore_read(a,6,&value));
 BR_NEED((value&0x18U)==0x18U && !(value&0xfc00U));
 BR_DO(restore_byte(a,0x61,0));BR_DO(restore_select(a,82));
 r->phase=4;BR_DO(restore_block(a,r->state_before));
 expect[0]=0x40;memcpy(expect+1,r->state_before,32);
 restore_word(expect+1,6,245);restore_word(expect+1,8,906);
 restore_word(expect+1,10,3000);restore_word(expect+1,21,54);
 r->state_changed=memcmp(expect+1,r->state_before,32)!=0;
 if(r->state_changed){
  BR_DO(restore_transfer(a,expect,33,NULL,0));
  BR_DO(restore_byte(a,0x60,restore_checksum(expect+1)));
 }
 /* Re-select to read committed data rather than trusting only the staging RAM. */
 BR_DO(restore_select(a,82));BR_DO(restore_block(a,r->state_after));
 BR_NEED(!memcmp(expect+1,r->state_after,32));r->verified|=1;
 r->phase=5;BR_DO(restore_select(a,105));BR_DO(restore_block(a,r->gain_before));
 memcpy(expect+1,r->gain_before,32);expect[6]&=0x7f;
 r->sign_changed=memcmp(expect+1,r->gain_before,32)!=0;
 if(r->sign_changed){
  /* Factory conditional CC gain sign correction, TI E2E1215460. Preserve all
   * gain magnitude/offset/calibration bytes; never perform board calibration. */
  BR_DO(restore_byte(a,0x45,expect[6]));
  BR_DO(restore_byte(a,0x60,restore_checksum(expect+1)));
 }
 BR_DO(restore_select(a,105));BR_DO(restore_block(a,r->gain_after));
 BR_NEED(!memcmp(expect+1,r->gain_after,32));r->verified|=2;
 r->phase=6;BR_DO(restore_control(a,0x42));BR_DO(restore_read(a,6,&value));
 BR_NEED(restore_flags_valid(value,0));
 /* BAT_DET was already set and must remain set. Do not replay BAT_INSERT on
  * an already inserted battery and needlessly restart learning. */
 r->phase=7;BR_DO(restore_control(a,0x20));
 BR_DO(restore_settled_status(a,&r->status_after));
 BR_NEED((r->status_after&0x2000U) && restore_status_valid(r->status_after));
 r->phase=8;BR_DO(restore_query(a,8,&r->chem_after));BR_NEED(r->chem_after==r->chem_before);
 BR_DO(restore_read(a,6,&r->flags_after));BR_NEED(restore_flags_valid(r->flags_after,0));
 BR_DO(restore_read(a,4,&r->voltage_after));
 BR_NEED(r->voltage_after>=3500 && r->voltage_after<=4450);
 BR_DO(restore_read(a,0x1c,&r->soc));BR_NEED(r->soc<=100);
 BR_DO(restore_read(a,0x10,&r->current));
 BR_DO(restore_read(a,0x1e,&r->temp));BR_NEED(r->temp>=2731 && r->temp<=3231);
 BR_DO(restore_read(a,6,&value));BR_NEED(restore_flags_valid(value,0));
 BR_NEED(a->allow(a->user));r->phase=9;r->complete=1;return 0;
#undef BR_DO
#undef BR_NEED
}
int battery_restore_run(struct battery_restore_result *out,int (*allow)(void*),void *user)
{
 if(!out || !allow)return -EINVAL;
 if(!atomic_cas(&used,0,1))return -EALREADY;
 memset(out,0,sizeof(*out));out->attempted=1;
 struct restore_actor a={out,allow,user,k_uptime_get()};
 int rc=-EPERM;
 if(!allow(user))goto finish;
 if(!idle()){stopped=released=0;fault=1;goto finish;}
 rc=restore_sequence(&a);
finish:
 /* Never hide partial configuration behind a successful hardware release. */
 if(rc && out->mutated)fault=1;
 out->rc=rc;out->fault=fault;out->stopped=stopped;out->released=released;
 out->elapsed_ms=(uint32_t)(k_uptime_get()-a.begin);return rc;
}
