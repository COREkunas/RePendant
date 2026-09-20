#ifndef BP_TEST_PLATFORM_H
#define BP_TEST_PLATFORM_H
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
#define __aligned(n) __declspec(align(n))
#define BUILD_ASSERT(...)
#define BIT(n) (1UL<<(n))
#define __DMB() ((void)0)
typedef int atomic_t;
static int atomic_get(const atomic_t *p){return *p;}
static int atomic_cas(atomic_t *p,int a,int b){if(*p!=a)return 0;*p=b;return 1;}
typedef struct {uintptr_t PTR;uint32_t MAXCNT,LIST,AMOUNT;} dma_mock;
typedef struct {
 uint32_t ENABLE,SHORTS,INTENSET,FREQUENCY,ADDRESS;
 uint32_t SUBSCRIBE_STARTRX,SUBSCRIBE_STARTTX,SUBSCRIBE_STOP,SUBSCRIBE_SUSPEND,SUBSCRIBE_RESUME;
 uint32_t PUBLISH_STOPPED,PUBLISH_ERROR,PUBLISH_SUSPENDED,PUBLISH_RXSTARTED,PUBLISH_TXSTARTED,PUBLISH_LASTRX,PUBLISH_LASTTX;
 struct {uint32_t SDA,SCL;} PSEL;
 dma_mock TXD,RXD;
 uint32_t events[4],errors;
} NRF_TWIM_Type;
static NRF_TWIM_Type mock_twim;
static struct {uint32_t PIN_CNF[32];} mock_gpio;
#define NRF_P1 (&mock_gpio)
#define DT_REG_ADDR(x) ((uintptr_t)&mock_twim)
#define DT_NODELABEL(x) 0
#define NRF_TWIM_SHORT_LASTTX_STARTRX_MASK 1U
#define NRF_TWIM_SHORT_LASTRX_STOP_MASK 2U
#define NRF_TWIM_SHORT_LASTTX_STOP_MASK 4U
#define NRF_TWIM_FREQ_100K 0x01980000UL
#define TWIM_ENABLE_ENABLE_Enabled 6U
#define NRF_GPIO_PIN_S0D1 6U
#define GPIO_PIN_CNF_DRIVE_Pos 8U
#define NRF_GPIO_PIN_DIR_INPUT 0U
#define NRF_GPIO_PIN_INPUT_CONNECT 0U
#define NRF_GPIO_PIN_NOPULL 0U
#define NRF_GPIO_PIN_NOSENSE 0U
enum {NRF_TWIM_EVENT_STOPPED,NRF_TWIM_EVENT_ERROR,NRF_TWIM_EVENT_LASTTX,NRF_TWIM_EVENT_LASTRX};
enum {NRF_TWIM_TASK_STARTTX,NRF_TWIM_TASK_STOP};
int64_t k_uptime_get(void);
uint32_t k_cycle_get_32(void);
uint32_t k_us_to_cyc_ceil32(uint32_t);
void k_busy_wait(uint32_t);
void k_msleep(int32_t);
uint32_t nrf_gpio_pin_read(uint32_t);
uint32_t nrf_gpio_pin_out_read(uint32_t);
void nrf_gpio_pin_set(uint32_t);
void nrf_gpio_cfg_default(uint32_t);
void nrf_gpio_cfg(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
void nrf_twim_pins_set(NRF_TWIM_Type*,uint32_t,uint32_t);
void nrf_twim_tx_buffer_set(NRF_TWIM_Type*,const uint8_t*,size_t);
void nrf_twim_rx_buffer_set(NRF_TWIM_Type*,uint8_t*,size_t);
void nrf_twim_shorts_set(NRF_TWIM_Type*,uint32_t);
void nrf_twim_disable(NRF_TWIM_Type*);
void nrf_twim_enable(NRF_TWIM_Type*);
void nrf_twim_frequency_set(NRF_TWIM_Type*,uint32_t);
void nrf_twim_address_set(NRF_TWIM_Type*,uint8_t);
void nrf_twim_event_clear(NRF_TWIM_Type*,unsigned);
int nrf_twim_event_check(NRF_TWIM_Type*,unsigned);
uint32_t nrf_twim_errorsrc_get_and_clear(NRF_TWIM_Type*);
uint32_t nrf_twim_txd_amount_get(NRF_TWIM_Type*);
uint32_t nrf_twim_rxd_amount_get(NRF_TWIM_Type*);
void nrf_twim_task_trigger(NRF_TWIM_Type*,unsigned);
#endif
