/* SPDX-License-Identifier: Apache-2.0
 * Dedicated read-only scanner HAL. Never delegates to NAND mutation driver. */
#include "recording_control_probe_nrf.h"
#include <errno.h>
#include <string.h>
#include <soc.h>
#include <hal/nrf_gpio.h>
#include <hal/nrf_spim.h>
#include <zephyr/devicetree.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

BUILD_ASSERT(DT_REG_ADDR(DT_NODELABEL(spi4))==0x5000a000UL,"Fixed SPIM4");
BUILD_ASSERT(!DT_NODE_HAS_STATUS(DT_NODELABEL(spi4),okay)&&!IS_ENABLED(CONFIG_SPI)&&!IS_ENABLED(CONFIG_NRFX_SPIM4),"Exclusive controller");
BUILD_ASSERT(NRF_SPIM_FREQ_125K==0x02000000U&&NRF_SPIM_FREQ_2M==0x20000000U,"Reviewed clock encodings");
BUILD_ASSERT(CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC==32768,"Fixed diagnostic timing metadata");
BUILD_ASSERT((SPIM_TXD_MAXCNT_MAXCNT_Msk>>SPIM_TXD_MAXCNT_MAXCNT_Pos)>=4100U&&
 (SPIM_RXD_MAXCNT_MAXCNT_Msk>>SPIM_RXD_MAXCNT_MAXCNT_Pos)>=4100U,"Cache DMA representable");
static NRF_SPIM_Type *const cp_bus=(NRF_SPIM_Type*)DT_REG_ADDR(DT_NODELABEL(spi4));
static const uint32_t cp_selects[]={45,47,43,44};
static struct {
 struct control_probe *context;struct cp_nrf_owner owner;
 uint32_t bound,lease,opened,stopped,fault,ever_opened,next_row,loaded_row,cache_started;
 struct cp_nrf_fault first;
} probe_hw;
static uint64_t cp_now(void *u){(void)u;return (uint64_t)k_uptime_get();}
static int cp_high(void)
{int good=1;for(unsigned i=0;i<4;i++){nrf_gpio_pin_set(cp_selects[i]);good&=nrf_gpio_pin_out_read(cp_selects[i])==1U;}return good;}
static int cp_aux(void){return NRF_P1->PIN_CNF[7]==3U&&nrf_gpio_pin_out_read(39)==1U;}
static int cp_idle_bus(void)
{
 return cp_bus->ENABLE==0&&cp_bus->SHORTS==0&&cp_bus->INTENSET==0&&cp_bus->SUBSCRIBE_START==0&&cp_bus->SUBSCRIBE_STOP==0&&
 cp_bus->SUBSCRIBE_SUSPEND==0&&cp_bus->SUBSCRIBE_RESUME==0&&cp_bus->PUBLISH_STARTED==0&&cp_bus->PUBLISH_END==0&&
 cp_bus->PUBLISH_ENDRX==0&&cp_bus->PUBLISH_ENDTX==0&&cp_bus->PUBLISH_STOPPED==0&&
 (cp_bus->PSEL.SCK&BIT(31))&&(cp_bus->PSEL.MOSI&BIT(31))&&(cp_bus->PSEL.MISO&BIT(31))&&
 (cp_bus->PSEL.CSN&BIT(31))&&(cp_bus->PSELDCX&BIT(31))&&cp_bus->RXD.LIST==0&&cp_bus->TXD.LIST==0;
}
static int cp_idle_pins(int opened)
{
 for(unsigned pin=8;pin<=10;pin++)if(NRF_P1->PIN_CNF[pin]!=2U)return 0;
 if(opened){if(!cp_aux())return 0;}
 else if(NRF_P1->PIN_CNF[7]!=2U)return 0;
 for(unsigned i=0;i<4;i++)if(NRF_P1->PIN_CNF[cp_selects[i]-32]!=(opened?3U:2U)||
    (opened&&nrf_gpio_pin_out_read(cp_selects[i])!=1U))return 0;
 return 1;
}
static int cp_configured(void)
{
 if(!cp_aux()||!nrf_spim_enable_check(cp_bus)||cp_bus->FREQUENCY!=NRF_SPIM_FREQ_125K||cp_bus->CONFIG||
 cp_bus->SHORTS||cp_bus->INTENSET||cp_bus->SUBSCRIBE_START||cp_bus->SUBSCRIBE_STOP||cp_bus->SUBSCRIBE_SUSPEND||cp_bus->SUBSCRIBE_RESUME||
 cp_bus->PUBLISH_STARTED||cp_bus->PUBLISH_END||cp_bus->PUBLISH_ENDRX||cp_bus->PUBLISH_ENDTX||cp_bus->PUBLISH_STOPPED||
 cp_bus->RXD.LIST||cp_bus->TXD.LIST||cp_bus->ORC||cp_bus->IFTIMING.RXDELAY||
 !(cp_bus->PSEL.CSN&BIT(31))||!(cp_bus->PSELDCX&BIT(31))||cp_bus->PSEL.SCK!=40||cp_bus->PSEL.MOSI!=41||cp_bus->PSEL.MISO!=42||
 NRF_P1->PIN_CNF[8]!=3||NRF_P1->PIN_CNF[9]!=3||NRF_P1->PIN_CNF[10]!=0)return 0;
 for(unsigned i=0;i<4;i++)if(NRF_P1->PIN_CNF[cp_selects[i]-32]!=3U||nrf_gpio_pin_out_read(cp_selects[i])!=1U)return 0;
 return 1;
}
static void cp_erratum(int on)
{if(NRF_ERRATA_DYNAMIC_CHECK(53,135))*(volatile uint32_t*)((uintptr_t)cp_bus+0xc04U)=on?1U:0U;}
static int cp_failure(struct cp_nrf_fault *f,uint32_t stage,int rc,uint32_t started_at)
{
 f->valid=1;f->stage=stage;f->rc=rc;f->elapsed_cycles=(uint32_t)(k_cycle_get_32()-started_at);
 f->cycle_hz=CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC;f->frequency=cp_bus->FREQUENCY;
 unsigned key=irq_lock();if(!probe_hw.first.valid)probe_hw.first=*f;irq_unlock(key);
 probe_hw.fault=1;return rc;
}
int cp_nrf_get_fault(struct cp_nrf_fault *out)
{if(!out)return -EINVAL;unsigned key=irq_lock();*out=probe_hw.first;irq_unlock(key);return 0;}
static int cp_owner_check(uint64_t d)
{
 return cp_now(NULL)>=d||probe_hw.owner.check(probe_hw.owner.user,probe_hw.context->volume.spec.device_id,
  probe_hw.context->volume.digest,d)||cp_now(NULL)>=d?-EPERM:0;
}
static int cp_admit(void *u,const uint8_t device[16],const uint8_t digest[32],uint64_t d)
{(void)u;return !probe_hw.bound||probe_hw.fault||cp_now(NULL)>=d||
 probe_hw.owner.check(probe_hw.owner.user,device,digest,d)||cp_now(NULL)>=d?-EPERM:0;}
static int cp_open(void *u,uint64_t d)
{
 (void)u;struct cp_nrf_fault f={.row=UINT32_MAX};uint32_t t=k_cycle_get_32();
 if(!probe_hw.bound||probe_hw.fault||probe_hw.opened||probe_hw.ever_opened||probe_hw.lease||cp_owner_check(d))
  return cp_failure(&f,CP_NRF_OPEN,-EPERM,t);
 int rc=probe_hw.owner.acquire(probe_hw.owner.user,d);
 if(rc)return cp_failure(&f,CP_NRF_OPEN,rc,t);
 probe_hw.lease=1;
 if(cp_owner_check(d)||!cp_idle_bus()||!cp_idle_pins(0)||!cp_high())return cp_failure(&f,CP_NRF_OPEN,-EPERM,t);
 for(unsigned i=0;i<4;i++)nrf_gpio_cfg_output(cp_selects[i]);
 nrf_gpio_pin_set(39);nrf_gpio_cfg_output(39);if(!cp_aux())return cp_failure(&f,CP_NRF_OPEN,-EIO,t);
 k_busy_wait(5000); /* Supply-settle margin, not a voltage measurement. */
 if(cp_owner_check(d))return cp_failure(&f,CP_NRF_OPEN,-ETIMEDOUT,t);
 nrf_gpio_pin_clear(40);nrf_gpio_pin_clear(41);nrf_gpio_cfg_output(40);nrf_gpio_cfg_output(41);
 nrf_gpio_cfg_input(42,NRF_GPIO_PIN_NOPULL);nrf_spim_frequency_set(cp_bus,NRF_SPIM_FREQ_125K);
 nrf_spim_configure(cp_bus,NRF_SPIM_MODE_0,NRF_SPIM_BIT_ORDER_MSB_FIRST);nrf_spim_orc_set(cp_bus,0);nrf_spim_iftiming_set(cp_bus,0);
 nrf_spim_pins_set(cp_bus,40,41,42);cp_erratum(1);nrf_spim_enable(cp_bus);
 if(!cp_configured())return cp_failure(&f,CP_NRF_OPEN,-EIO,t);
 probe_hw.opened=probe_hw.stopped=probe_hw.ever_opened=1;probe_hw.next_row=1057U*64U;probe_hw.loaded_row=UINT32_MAX;return 0;
}
static int cp_wait(nrf_spim_event_t event,uint64_t d,int cache)
{
 uint32_t start=k_cycle_get_32(),limit=k_us_to_cyc_ceil32(cache?80000U:2000U);
 for(unsigned i=0;i<(cache?256U:4096U);i++){
  if((uint32_t)(k_cycle_get_32()-start)>=limit||cp_now(NULL)>=d)return 0;
  if(nrf_spim_event_check(cp_bus,event))return 1;
  if(cache)k_usleep(100);else k_busy_wait(1);
 }
 return 0;
}
static int cp_shape(const uint8_t *tx,uint8_t *rx,uint32_t n,uint32_t fast,uint32_t *row)
{
 if(tx!=probe_hw.context->tx||fast>1)return 0;
 if(fast){
  if(rx!=probe_hw.context->rx||n!=4100||tx[0]!=0x03||probe_hw.loaded_row==UINT32_MAX||probe_hw.cache_started)return 0;
  for(unsigned i=1;i<4100;i++)if(tx[i])return 0;
  *row=probe_hw.loaded_row;return 1;
 }
 if(rx!=probe_hw.context->control_rx||n<3||n>4)return 0;
 if(tx[0]==0x9f)return n==4&&!tx[1]&&!tx[2]&&!tx[3]&&probe_hw.next_row==1057U*64U;
 if(tx[0]==0x0f)return n==3&&!tx[2]&&(tx[1]==0xa0||tx[1]==0xb0||tx[1]==0xc0);
 if(tx[0]!=0x13||n!=4)return 0;
 *row=((uint32_t)tx[1]<<16)|((uint32_t)tx[2]<<8)|tx[3];
 return *row==probe_hw.next_row&&*row<1059U*64U&&
   (probe_hw.loaded_row==UINT32_MAX||probe_hw.cache_started);
}
static int cp_exchange(void *u,const uint8_t *tx,uint8_t *rx,uint32_t n,uint32_t fast,uint64_t d,struct cp_transfer *r)
{
 (void)u;if(!r)return -EINVAL;memset(r,0,sizeof(*r));
 struct cp_nrf_fault f={.row=probe_hw.loaded_row,.bytes=n,.fast=fast};uint32_t t=k_cycle_get_32();
 if(probe_hw.context&&tx==probe_hw.context->tx)f.opcode=tx[0];
 if(probe_hw.fault||!probe_hw.lease||!probe_hw.opened||!probe_hw.stopped||!cp_configured())
  return cp_failure(&f,CP_NRF_ADMISSION,-EPERM,t);
 r->stopped=f.stopped=1;
 if(!cp_shape(tx,rx,n,fast,&f.row))return cp_failure(&f,CP_NRF_SHAPE,-EINVAL,t);
 if(cp_owner_check(d))return cp_failure(&f,CP_NRF_ADMISSION,-EPERM,t);
 if(fast)nrf_spim_frequency_set(cp_bus,NRF_SPIM_FREQ_2M);
 if(cp_bus->FREQUENCY!=(fast?NRF_SPIM_FREQ_2M:NRF_SPIM_FREQ_125K))return cp_failure(&f,CP_NRF_START,-EIO,t);
 nrf_spim_event_clear(cp_bus,NRF_SPIM_EVENT_END);nrf_spim_event_clear(cp_bus,NRF_SPIM_EVENT_STOPPED);
 nrf_spim_tx_buffer_set(cp_bus,tx,n);nrf_spim_rx_buffer_set(cp_bus,rx,n);
 if(!cp_high())return cp_failure(&f,CP_NRF_START,-EIO,t);
 nrf_gpio_pin_clear(47);k_busy_wait(2);
 if(cp_owner_check(d)){
  (void)cp_high();nrf_spim_frequency_set(cp_bus,NRF_SPIM_FREQ_125K);return cp_failure(&f,CP_NRF_START,-EPERM,t);
 }
 probe_hw.stopped=r->stopped=f.stopped=0;__DMB();
 if(f.opcode==0x13){probe_hw.loaded_row=f.row;++probe_hw.next_row;probe_hw.cache_started=0;}
 if(f.opcode==0x03)probe_hw.cache_started=1;
 nrf_spim_task_trigger(cp_bus,NRF_SPIM_TASK_START);r->started=f.started=1;
 f.ended=(uint32_t)cp_wait(NRF_SPIM_EVENT_END,d,fast!=0);int high=cp_high();
 nrf_spim_event_clear(cp_bus,NRF_SPIM_EVENT_STOPPED);nrf_spim_task_trigger(cp_bus,NRF_SPIM_TASK_STOP);
 /* Original short2ms cycle bound retained; only outer cleanup window is3ms. */
 if(!cp_wait(NRF_SPIM_EVENT_STOPPED,cp_now(NULL)+3U,0))return cp_failure(&f,CP_NRF_STOP,-EBUSY,t);
 __DMB();probe_hw.stopped=r->stopped=f.stopped=1;f.amounts_valid=1;
 r->tx_bytes=f.tx_bytes=nrf_spim_tx_amount_get(cp_bus);r->rx_bytes=f.rx_bytes=nrf_spim_rx_amount_get(cp_bus);
 nrf_spim_frequency_set(cp_bus,NRF_SPIM_FREQ_125K);
 if(!f.ended)return cp_failure(&f,CP_NRF_END,-ETIMEDOUT,t);
 if(!high||r->tx_bytes!=n||r->rx_bytes!=n||!cp_configured())return cp_failure(&f,CP_NRF_COUNTS,-EIO,t);
 if(cp_owner_check(d))return cp_failure(&f,CP_NRF_ADMISSION,-EPERM,t);
 return 0;
}
static int cp_close(void *u,uint64_t d)
{
 (void)u;struct cp_nrf_fault f={.row=probe_hw.loaded_row,.stopped=probe_hw.stopped};uint32_t t=k_cycle_get_32();
 /* Even a failed read may close its electrically proven controller. No data
  * admission is renewed here; release callback has separate cleanup authority. */
 if(!probe_hw.opened||!probe_hw.lease||!probe_hw.stopped||!cp_configured()||cp_now(NULL)>=d||!cp_high())
  return cp_failure(&f,CP_NRF_CLOSE,-EPERM,t);
 nrf_spim_disable(cp_bus);if(cp_bus->ENABLE)return cp_failure(&f,CP_NRF_CLOSE,-EIO,t);
 cp_erratum(0);nrf_spim_pins_set(cp_bus,NRF_SPIM_PIN_NOT_CONNECTED,NRF_SPIM_PIN_NOT_CONNECTED,NRF_SPIM_PIN_NOT_CONNECTED);
 nrf_spim_tx_buffer_set(cp_bus,NULL,0);nrf_spim_rx_buffer_set(cp_bus,NULL,0);
 for(unsigned pin=40;pin<=42;pin++)nrf_gpio_cfg_default(pin);
 if(!cp_idle_bus()||!cp_idle_pins(1)||!cp_aux())return cp_failure(&f,CP_NRF_CLOSE,-EIO,t);
 int rc=probe_hw.owner.release(probe_hw.owner.user,d);
 if(rc||cp_now(NULL)>=d)return cp_failure(&f,CP_NRF_CLOSE,rc?rc:-ETIMEDOUT,t);
 probe_hw.lease=probe_hw.opened=0;return 0;
}
static void cp_pause(void *u,uint32_t us){(void)u;if(us==100)k_busy_wait(us);}
static int cp_record(void *u,const uint8_t metadata[CP_RECORD_BYTES],uint64_t d)
{(void)u;if(probe_hw.fault||!probe_hw.stopped||cp_owner_check(d))return -EPERM;
 return probe_hw.owner.record(probe_hw.owner.user,metadata,d);}
int cp_nrf_bind(struct control_probe *context,const struct cp_nrf_owner *owner,struct cp_port *out)
{
 if(!context||!owner||!out||!owner->acquire||!owner->release||!owner->check||!owner->record||probe_hw.bound)return -EINVAL;
 probe_hw.context=context;probe_hw.owner=*owner;probe_hw.bound=1;probe_hw.loaded_row=UINT32_MAX;
 *out=(struct cp_port){NULL,cp_now,cp_admit,cp_open,cp_exchange,cp_close,cp_pause,cp_record};return 0;
}
