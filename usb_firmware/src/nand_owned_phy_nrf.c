/* SPDX-License-Identifier: Apache-2.0
 * UNLINKED target-only HAL. Fixed SPIM4/P1.15, no host-command passthrough. */
#include "nand_owned_phy_nrf.h"
#include <errno.h>
#include <string.h>
#include <soc.h>
#include <hal/nrf_gpio.h>
#include <hal/nrf_spim.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

BUILD_ASSERT(DT_REG_ADDR(DT_NODELABEL(spi4))==0x5000a000UL,"Only confirmed SPIM4");
BUILD_ASSERT(!DT_NODE_HAS_STATUS(DT_NODELABEL(spi4),okay)&&!IS_ENABLED(CONFIG_SPI)&&!IS_ENABLED(CONFIG_NRFX_SPIM4),"No second SPI driver");
BUILD_ASSERT(NRF_SPIM_FREQ_8M==0x80000000U,"Factory SPI clock encoding");
static NRF_SPIM_Type *const bus=(NRF_SPIM_Type*)DT_REG_ADDR(DT_NODELABEL(spi4));
static const uint32_t selects[]={45,47,43,44};
static struct {
 struct nand_owned_phy *context;struct nop_nrf_owner owner;
 uint32_t bound,lease,opened,stopped,fault,ever_opened;
} hw;
static struct nop_nrf_diagnostics diagnostics;
static int all_high(void)
{int good=1;for(unsigned i=0;i<4;++i){nrf_gpio_pin_set(selects[i]);good&=nrf_gpio_pin_out_read(selects[i])==1U;}return good;}
static int aux(void){return NRF_P1->PIN_CNF[7]==3U&&nrf_gpio_pin_out_read(39)==1U;}
static int idle_bus(void)
{
 return bus->ENABLE==0&&bus->SHORTS==0&&bus->INTENSET==0&&bus->SUBSCRIBE_START==0&&bus->SUBSCRIBE_STOP==0&&
 bus->SUBSCRIBE_SUSPEND==0&&bus->SUBSCRIBE_RESUME==0&&bus->PUBLISH_STARTED==0&&bus->PUBLISH_END==0&&
 bus->PUBLISH_ENDRX==0&&bus->PUBLISH_ENDTX==0&&bus->PUBLISH_STOPPED==0&&
 (bus->PSEL.SCK&BIT(31))&&(bus->PSEL.MOSI&BIT(31))&&(bus->PSEL.MISO&BIT(31))&&
 (bus->PSEL.CSN&BIT(31))&&(bus->PSELDCX&BIT(31))&&bus->RXD.LIST==0&&bus->TXD.LIST==0;
}
static int idle_pins(void)
{
 for(unsigned i=8;i<=10;++i)if(NRF_P1->PIN_CNF[i]!=2U)return 0;
 if(!hw.ever_opened){if(NRF_P1->PIN_CNF[7]!=2U)return 0;
  for(unsigned i=0;i<4;++i)if(NRF_P1->PIN_CNF[selects[i]-32]!=2U)return 0;
 }else{
  if(!aux())return 0;
  for(unsigned i=0;i<4;++i)if(NRF_P1->PIN_CNF[selects[i]-32]!=3U||nrf_gpio_pin_out_read(selects[i])!=1U)return 0;
 }
 return 1;
}
static int configured(void)
{
 if(!aux()||!nrf_spim_enable_check(bus)||bus->FREQUENCY!=NRF_SPIM_FREQ_8M||bus->CONFIG||
    bus->SHORTS||bus->INTENSET||bus->SUBSCRIBE_START||bus->SUBSCRIBE_STOP||bus->SUBSCRIBE_SUSPEND||bus->SUBSCRIBE_RESUME||
    bus->PUBLISH_STARTED||bus->PUBLISH_END||bus->PUBLISH_ENDRX||bus->PUBLISH_ENDTX||bus->PUBLISH_STOPPED||
    bus->RXD.LIST||bus->TXD.LIST||bus->ORC||bus->IFTIMING.RXDELAY||
    !(bus->PSEL.CSN&BIT(31))||!(bus->PSELDCX&BIT(31))||
    bus->PSEL.SCK!=40||bus->PSEL.MOSI!=41||bus->PSEL.MISO!=42||
    NRF_P1->PIN_CNF[8]!=3||NRF_P1->PIN_CNF[9]!=3||NRF_P1->PIN_CNF[10]!=0)return 0;
 for(unsigned i=0;i<4;++i)if(NRF_P1->PIN_CNF[selects[i]-32]!=3U||nrf_gpio_pin_out_read(selects[i])!=1U)return 0;
 return 1;
}
static void erratum(int on)
{if(NRF_ERRATA_DYNAMIC_CHECK(53,135))*(volatile uint32_t*)((uintptr_t)bus+0xc04U)=on?1U:0U;}
static uint64_t now(void *user){(void)user;return (uint64_t)k_uptime_get();}
static void pause_us(void *user,uint32_t us){(void)user;k_busy_wait(us);}
static int authority(void *user,uint32_t access,uint32_t row,uint64_t deadline)
{(void)user;if(!hw.lease||!hw.opened||hw.fault||!hw.stopped||now(NULL)>=deadline)return -EPERM;
 return hw.owner.authorize(hw.owner.user,access,row,deadline);}
static int open_bus(void *user,uint64_t deadline)
{
 (void)user;if(hw.fault||hw.opened||hw.lease||now(NULL)>=deadline)return -EPERM;
 int rc=hw.owner.acquire(hw.owner.user,deadline);if(rc)return rc;hw.lease=1;
 if(now(NULL)>=deadline||!idle_bus()||!idle_pins()){hw.fault=1;return -EPERM;}
 if(!all_high()){hw.fault=1;return -EIO;}
 for(unsigned i=0;i<4;++i)nrf_gpio_cfg_output(selects[i]);
 nrf_gpio_pin_set(39);nrf_gpio_cfg_output(39);if(!aux()){hw.fault=1;return -EIO;}
 k_busy_wait(5000); /* Delay alone is not proof of actual supply ramp/voltage. */
 if(now(NULL)>=deadline){hw.fault=1;return -ETIMEDOUT;}
 nrf_gpio_pin_clear(40);nrf_gpio_pin_clear(41);nrf_gpio_cfg_output(40);nrf_gpio_cfg_output(41);
 nrf_gpio_cfg_input(42,NRF_GPIO_PIN_NOPULL);nrf_spim_frequency_set(bus,NRF_SPIM_FREQ_8M);
 nrf_spim_configure(bus,NRF_SPIM_MODE_0,NRF_SPIM_BIT_ORDER_MSB_FIRST);nrf_spim_orc_set(bus,0);nrf_spim_iftiming_set(bus,0);
 nrf_spim_pins_set(bus,40,41,42);erratum(1);nrf_spim_enable(bus);
 if(!configured()){hw.fault=1;return -EIO;}hw.opened=hw.stopped=hw.ever_opened=1;return 0;
}
static int wait_event(nrf_spim_event_t event,uint64_t deadline,int long_dma)
{
 uint32_t start=k_cycle_get_32(),cycles=k_us_to_cyc_ceil32(long_dma?80000U:2000U);
 for(unsigned i=0;i<(long_dma?256U:4096U);++i){
  /* END/STOPPED is latched by hardware while this thread may be preempted.
   * Observe completion BEFORE declaring the local polling budget exhausted.
   * The caller separately enforces the original operation deadline. A late
   * STOP observation still proves DMA quiescence; it does not renew a job. */
  uint32_t elapsed=(uint32_t)(k_cycle_get_32()-start);
  if(nrf_spim_event_check(bus,event)){
   if(elapsed>=cycles){if(event==NRF_SPIM_EVENT_END)++diagnostics.late_end;else ++diagnostics.late_stop;}
   return 1;
  }
  if(elapsed>=cycles||now(NULL)>=deadline)return 0;
  if(long_dma)k_usleep(100);else k_busy_wait(1);
 }
 return 0;
}
static int exchange_inner(void *user,const uint8_t *tx,uint8_t *rx,uint32_t bytes,uint32_t fast,
                    uint64_t deadline,struct nop_transfer_result *result)
{
 (void)user;memset(result,0,sizeof(*result));
 if(hw.fault||!hw.lease||!hw.opened||!hw.stopped||!configured()){hw.fault=1;return -EPERM;}
 result->stopped=1;
 if(tx!=hw.context->tx||rx!=hw.context->rx||bytes<1||bytes>4356||fast>1||
    (!fast&&bytes>4)||now(NULL)>=deadline)return -EINVAL;
 if(fast){
  uint32_t column=((uint32_t)tx[1]<<8)|tx[2];
  int main_read=tx[0]==0x03&&bytes>=5&&bytes<=4100&&tx[3]==0&&column<4096&&bytes-4<=4096-column;
  int raw_read=tx[0]==0x03&&bytes==4356&&column==0&&tx[3]==0;
  int page_load=tx[0]==0x02&&bytes==4099&&column==0;
  if(!main_read&&!raw_read&&!page_load)return -EINVAL;
 }
 /* The factory uses 8MHz for commands AND payload. 'fast' still identifies
  * a validated long-DMA tuple/poll policy, not a different electrical mode. */
 if(bus->FREQUENCY!=NRF_SPIM_FREQ_8M){hw.fault=1;return -EIO;}
 nrf_spim_event_clear(bus,NRF_SPIM_EVENT_END);nrf_spim_event_clear(bus,NRF_SPIM_EVENT_STOPPED);
 nrf_spim_tx_buffer_set(bus,tx,bytes);nrf_spim_rx_buffer_set(bus,rx,bytes);
 if(!all_high()){hw.fault=1;return -EIO;}nrf_gpio_pin_clear(47);k_busy_wait(2);
 if(now(NULL)>=deadline){(void)all_high();return -ETIMEDOUT;}
 hw.stopped=result->stopped=0;__DMB();nrf_spim_task_trigger(bus,NRF_SPIM_TASK_START);result->started=1;
 int ended=wait_event(NRF_SPIM_EVENT_END,deadline,fast!=0),high=all_high();
 nrf_spim_event_clear(bus,NRF_SPIM_EVENT_STOPPED);nrf_spim_task_trigger(bus,NRF_SPIM_TASK_STOP);
 /* Cleanup STOP owns a separate3ms safety budget, never a renewed array job. */
 if(!wait_event(NRF_SPIM_EVENT_STOPPED,now(NULL)+3U,0)){hw.fault=1;return -EBUSY;}
 __DMB();hw.stopped=result->stopped=1;result->tx_amount=nrf_spim_tx_amount_get(bus);result->rx_amount=nrf_spim_rx_amount_get(bus);
 if(!ended||!high||result->tx_amount!=bytes||result->rx_amount!=bytes||!configured()||now(NULL)>=deadline){hw.fault=1;return -EIO;}
 return 0;
}
static int exchange(void *user,const uint8_t *tx,uint8_t *rx,uint32_t bytes,uint32_t fast,
                    uint64_t deadline,struct nop_transfer_result *result)
{
 uint64_t before=now(NULL);uint32_t opcode=hw.context&&tx==hw.context->tx&&bytes?tx[0]:UINT32_MAX;
 int rc=exchange_inner(user,tx,rx,bytes,fast,deadline,result);++diagnostics.transfers;
 if(rc&&!diagnostics.valid){diagnostics.valid=1;diagnostics.opcode=opcode;diagnostics.bytes=bytes;
  diagnostics.fast=fast;diagnostics.rc=rc;diagnostics.before_ms=before;diagnostics.after_ms=now(NULL);
  diagnostics.deadline=deadline;diagnostics.started=result->started;diagnostics.stopped=result->stopped;
  diagnostics.tx_amount=result->tx_amount;diagnostics.rx_amount=result->rx_amount;}
 return rc;
}
void nand_owned_phy_nrf_diagnostics(struct nop_nrf_diagnostics *out)
{if(out)*out=diagnostics;}
static int close_bus(void *user,uint64_t deadline)
{
 (void)user;if(hw.fault||!hw.opened||!hw.lease||!hw.stopped||!configured()||now(NULL)>=deadline)return -EPERM;
 if(!all_high()){hw.fault=1;return -EIO;}nrf_spim_disable(bus);if(bus->ENABLE){hw.fault=1;return -EIO;}
 erratum(0);nrf_spim_pins_set(bus,NRF_SPIM_PIN_NOT_CONNECTED,NRF_SPIM_PIN_NOT_CONNECTED,NRF_SPIM_PIN_NOT_CONNECTED);
 nrf_spim_tx_buffer_set(bus,NULL,0);nrf_spim_rx_buffer_set(bus,NULL,0);
 for(unsigned pin=40;pin<=42;++pin)nrf_gpio_cfg_default(pin);
 if(!idle_bus()||!idle_pins()||!aux()){hw.fault=1;return -EIO;}
 int rc=hw.owner.release(hw.owner.user,deadline);if(rc||now(NULL)>=deadline){hw.fault=1;return -EIO;}
 hw.lease=hw.opened=0;return 0;
}
int nand_owned_phy_nrf_bind(struct nop_port *port,struct nand_owned_phy *context,const struct nop_nrf_owner *owner)
{
 if(!port||!context||!owner||!owner->acquire||!owner->release||!owner->authorize||hw.bound)return -EINVAL;
 hw.context=context;hw.owner=*owner;hw.bound=1;
 *port=(struct nop_port){NULL,now,pause_us,authority,open_bus,exchange,close_bus};return 0;
}
