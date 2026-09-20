/* Use the SoC's supported 128 MHz CPU mode only while recording/draining.
 * No peripheral/source change: PDM, SPI, USB and the RTC keep their clocks.
 * nrfx updates SystemCoreClock, which the SDK busy-wait calibration consumes.
 * 20 us settling exceeds Nordic's 9 us typical transition; not a hard bound.
 * Readback proves the configured divider, not an external frequency measure. */
#ifndef OPENPENDANT_RECORDING_CPU_CLOCK_H
#define OPENPENDANT_RECORDING_CPU_CLOCK_H
#include <nrfx_clock.h>
struct recording_cpu_clock { uint32_t active,saved,before,during,after; };
static inline int rpc_enter(struct recording_cpu_clock *p)
{
 unsigned key=irq_lock();int rc=-EPERM;
 uint32_t divider=(uint32_t)nrfx_clock_hfclk_divider_get();
 /* Early engineering silicon needs an additional workaround not qualified
  * for this device. Refuse instead of guessing its clock configuration. */
 if(p->active||NRF_ERRATA_DYNAMIC_CHECK(53,4)||divider>NRF_CLOCK_HFCLK_DIV_2)goto done;
 p->saved=divider;p->before=128000000U>>divider;p->during=p->after=0;p->active=1;
 nrfx_clock_hfclk_divider_set(NRF_CLOCK_HFCLK_DIV_1);k_busy_wait(20);
 if(nrfx_clock_hfclk_divider_get()!=NRF_CLOCK_HFCLK_DIV_1||SystemCoreClock!=128000000U)goto done;
 p->during=SystemCoreClock;rc=0;
done:irq_unlock(key);return rc;
}
static inline int rpc_leave(struct recording_cpu_clock *p)
{
 unsigned key=irq_lock();int rc=-EPERM;
 if(!p->active){rc=0;goto done;} /* capacity/start refusal never acquired it */
 if(nrfx_clock_hfclk_divider_get()!=NRF_CLOCK_HFCLK_DIV_1||SystemCoreClock!=128000000U)goto done;
 nrfx_clock_hfclk_divider_set((nrf_clock_hfclk_div_t)p->saved);k_busy_wait(20);
 if((uint32_t)nrfx_clock_hfclk_divider_get()!=p->saved||SystemCoreClock!=p->before)goto done;
 p->after=SystemCoreClock;p->active=0;rc=0;
done:irq_unlock(key);return rc;
}
#endif
