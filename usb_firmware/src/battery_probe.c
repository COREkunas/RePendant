/* SPDX-License-Identifier: Apache-2.0
 * TI BQ27427: SLUUCD5B 3.3/3.4/5.3/5.4. No configuration writes.
 */
#include "battery_probe.h"
#ifdef BATTERY_PROBE_NATIVE_TEST
#include "battery_probe_platform.h"
#else
#include <errno.h>
#include <string.h>
#include <soc.h>
#include <hal/nrf_gpio.h>
#include <hal/nrf_twim.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#endif

#define BP_SDA 38U
#define BP_SCL 46U
#define BP_SHORTS (NRF_TWIM_SHORT_LASTTX_STARTRX_MASK | NRF_TWIM_SHORT_LASTRX_STOP_MASK)
#define BP_PIN_CNF ((uint32_t)NRF_GPIO_PIN_S0D1 << GPIO_PIN_CNF_DRIVE_Pos)
BUILD_ASSERT(DT_REG_ADDR(DT_NODELABEL(i2c1)) == 0x50009000UL,"Factory TWIM1 only");
BUILD_ASSERT(!DT_NODE_HAS_STATUS(DT_NODELABEL(i2c1), okay) &&
 !DT_NODE_HAS_STATUS(DT_NODELABEL(spi1), okay) &&
 !DT_NODE_HAS_STATUS(DT_NODELABEL(uart1), okay),"Shared peripheral must be unused");
BUILD_ASSERT(!IS_ENABLED(CONFIG_I2C) && !IS_ENABLED(CONFIG_NRFX_TWIM1) &&
 !IS_ENABLED(CONFIG_SENSOR),"No automatic I2C/sensor configuration");
static NRF_TWIM_Type *const bp_twim=(NRF_TWIM_Type *)DT_REG_ADDR(DT_NODELABEL(i2c1));
static const uint8_t selectors[3]={0x06,0x04,0x06};
static __aligned(4) uint8_t tx[33],rx[32];
static atomic_t used;
static uint32_t stopped=1,released=1,fault;

int battery_probe_flags_trusted(uint16_t f)
{return (f&0x0008U)!=0 && (f&0x3c30U)==0;}

static int idle(void)
{
 return bp_twim->ENABLE==0 && bp_twim->SHORTS==0 && bp_twim->INTENSET==0 &&
  bp_twim->SUBSCRIBE_STARTRX==0 && bp_twim->SUBSCRIBE_STARTTX==0 &&
  bp_twim->SUBSCRIBE_STOP==0 && bp_twim->SUBSCRIBE_SUSPEND==0 && bp_twim->SUBSCRIBE_RESUME==0 &&
  bp_twim->PUBLISH_STOPPED==0 && bp_twim->PUBLISH_ERROR==0 && bp_twim->PUBLISH_SUSPENDED==0 &&
  bp_twim->PUBLISH_RXSTARTED==0 && bp_twim->PUBLISH_TXSTARTED==0 &&
  bp_twim->PUBLISH_LASTRX==0 && bp_twim->PUBLISH_LASTTX==0 &&
  (bp_twim->PSEL.SDA&BIT(31)) && (bp_twim->PSEL.SCL&BIT(31)) &&
  bp_twim->TXD.LIST==0 && bp_twim->RXD.LIST==0 && bp_twim->TXD.PTR==0 && bp_twim->RXD.PTR==0 &&
  bp_twim->TXD.MAXCNT==0 && bp_twim->RXD.MAXCNT==0 &&
  NRF_P1->PIN_CNF[6]==2U && NRF_P1->PIN_CNF[14]==2U;
}
static int lines(void)
{return nrf_gpio_pin_read(BP_SDA)==1 && nrf_gpio_pin_read(BP_SCL)==1;}
static int wait_stop(uint32_t us,int errors)
{
 uint32_t first=k_cycle_get_32(),limit=k_us_to_cyc_ceil32(us);
 int64_t deadline=k_uptime_get()+(int64_t)(us/1000U)+1;
 for(uint32_t i=0;i<us+1;++i){
  if((uint32_t)(k_cycle_get_32()-first)>=limit || k_uptime_get()>=deadline)return -ETIMEDOUT;
  if(errors && nrf_twim_event_check(bp_twim,NRF_TWIM_EVENT_ERROR))return -EIO;
  if(nrf_twim_event_check(bp_twim,NRF_TWIM_EVENT_STOPPED))return 0;
  k_busy_wait(1);
 }
 return -ETIMEDOUT;
}
static void cleanup(void)
{
 if(!stopped){fault=1;return;}
 nrf_twim_shorts_set(bp_twim,0);nrf_twim_disable(bp_twim);
 if(bp_twim->ENABLE || bp_twim->SHORTS){fault=1;return;}
 nrf_twim_pins_set(bp_twim,UINT32_MAX,UINT32_MAX);
 nrf_twim_tx_buffer_set(bp_twim,NULL,0);nrf_twim_rx_buffer_set(bp_twim,NULL,0);
 nrf_gpio_cfg_default(BP_SDA);nrf_gpio_cfg_default(BP_SCL);
 released=(uint32_t)(idle()!=0);if(!released){fault=1;return;}
 memset(rx,0,sizeof(rx));memset(tx,0,sizeof(tx));
}
static int read_fixed(unsigned index,struct battery_probe_sample *s)
{
 if(index>=3 || !stopped || !lines())return -EPERM;
 tx[0]=selectors[index];memset(rx,0,sizeof(rx));
 nrf_twim_event_clear(bp_twim,NRF_TWIM_EVENT_STOPPED);
 nrf_twim_event_clear(bp_twim,NRF_TWIM_EVENT_ERROR);
 nrf_twim_event_clear(bp_twim,NRF_TWIM_EVENT_LASTTX);
 nrf_twim_event_clear(bp_twim,NRF_TWIM_EVENT_LASTRX);
 (void)nrf_twim_errorsrc_get_and_clear(bp_twim);
 nrf_twim_tx_buffer_set(bp_twim,tx,1);nrf_twim_rx_buffer_set(bp_twim,rx,2);
 nrf_twim_shorts_set(bp_twim,BP_SHORTS);
 if(bp_twim->TXD.MAXCNT!=1 || bp_twim->RXD.MAXCNT!=2 ||
    bp_twim->TXD.PTR!=(uintptr_t)tx || bp_twim->RXD.PTR!=(uintptr_t)rx || bp_twim->SHORTS!=BP_SHORTS)return -EIO;
 stopped=0;__DMB();s->started_ms=(uint32_t)k_uptime_get();
 nrf_twim_task_trigger(bp_twim,NRF_TWIM_TASK_STARTTX);
 int rc=wait_stop(25000,1);
 nrf_twim_shorts_set(bp_twim,0);
 if(!nrf_twim_event_check(bp_twim,NRF_TWIM_EVENT_STOPPED)){
  nrf_twim_task_trigger(bp_twim,NRF_TWIM_TASK_STOP);
  if(wait_stop(10000,0)){fault=1;return -EBUSY;}
 }
 __DMB();stopped=1;s->finished_ms=(uint32_t)k_uptime_get();
 s->tx=nrf_twim_txd_amount_get(bp_twim);s->rx=nrf_twim_rxd_amount_get(bp_twim);
 s->errors=nrf_twim_errorsrc_get_and_clear(bp_twim);
 if(rc)return rc;
 if(s->errors || s->tx!=1 || s->rx!=2 || !lines())return -EIO;
 s->raw=(uint16_t)(rx[0]|((uint16_t)rx[1]<<8));return 0;
}
int battery_probe_run(struct battery_probe_result *out,int (*allow)(void*),void *user)
{
 if(!out || !allow)return -EINVAL;
 if(!atomic_cas(&used,0,1))return -EALREADY; /* Never overwrite previous proof. */
 memset(out,0,sizeof(*out));out->attempted=1;
 int64_t begin=k_uptime_get();int rc=-EPERM;
 if(!allow(user))goto done;
 if(!idle()){fault=1;stopped=0;released=0;goto done;} /* Not ours to repair. */
 for(unsigned i=0;i<3;++i){
  /* A sleeping shell actor, not a one-second CPU spin. No other gauge caller
   * exists; one-shot marker includes cancellation and all failed transfers. */
  k_msleep(1000);
  if(!allow(user) || k_uptime_get()-begin>=5000){rc=-ECANCELED;break;}
  released=0;
  nrf_gpio_pin_set(BP_SDA);nrf_gpio_pin_set(BP_SCL);
  nrf_gpio_cfg(BP_SDA,NRF_GPIO_PIN_DIR_INPUT,NRF_GPIO_PIN_INPUT_CONNECT,
   NRF_GPIO_PIN_NOPULL,NRF_GPIO_PIN_S0D1,NRF_GPIO_PIN_NOSENSE);
  nrf_gpio_cfg(BP_SCL,NRF_GPIO_PIN_DIR_INPUT,NRF_GPIO_PIN_INPUT_CONNECT,
   NRF_GPIO_PIN_NOPULL,NRF_GPIO_PIN_S0D1,NRF_GPIO_PIN_NOSENSE);
  if(NRF_P1->PIN_CNF[6]!=BP_PIN_CNF || NRF_P1->PIN_CNF[14]!=BP_PIN_CNF ||
     nrf_gpio_pin_out_read(BP_SDA)!=1 || nrf_gpio_pin_out_read(BP_SCL)!=1){rc=-EIO;fault=1;break;}
  k_busy_wait(5);
  if(!lines()){rc=-EIO;break;}
  nrf_twim_frequency_set(bp_twim,NRF_TWIM_FREQ_100K);nrf_twim_address_set(bp_twim,0x55);
  nrf_twim_pins_set(bp_twim,BP_SCL,BP_SDA);
  if(bp_twim->FREQUENCY!=(uint32_t)NRF_TWIM_FREQ_100K || bp_twim->ADDRESS!=0x55 ||
     bp_twim->PSEL.SDA!=BP_SDA || bp_twim->PSEL.SCL!=BP_SCL){rc=-EIO;fault=1;break;}
  nrf_twim_enable(bp_twim);
  if(bp_twim->ENABLE!=TWIM_ENABLE_ENABLE_Enabled){rc=-EIO;fault=1;break;}
  ++out->reads;rc=read_fixed(i,&out->sample[i]);out->sample[i].rc=rc;
  cleanup();if(rc || fault)break;
 }
 cleanup();
 if(!rc && !fault && allow(user) && k_uptime_get()-begin<5000){
  out->coherent=out->reads==3 && out->sample[0].raw==out->sample[2].raw;
  out->trusted=out->coherent && battery_probe_flags_trusted(out->sample[0].raw) &&
   out->sample[1].raw>0 && out->sample[1].raw<=6000;
 }else if(!rc)rc=fault?-EIO:-ECANCELED;
done:
 out->rc=rc;out->stopped=stopped;out->released=released;out->fault=fault;
 out->elapsed_ms=(uint32_t)(k_uptime_get()-begin);return rc;
}

#include "battery_restore.inc"
#include "battery_service.inc"
