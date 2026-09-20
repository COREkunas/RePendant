/* SPDX-License-Identifier: Apache-2.0
 * Dormant, one-shot WHO_AM_I reads only. Not a sensor or generic I2C driver.
 */
#include "imu_id.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <soc.h>
#include <hal/nrf_gpio.h>
#include <hal/nrf_twim.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#define IMU_ID_SDA 38U
#define IMU_ID_SCL 46U
#define IMU_ID_EVENT_US 2000U
#define IMU_ID_POLL_LIMIT 4096U
#define IMU_ID_TOTAL_MS 25U
#define IMU_ID_SHORTS (NRF_TWIM_SHORT_LASTTX_STARTRX_MASK | NRF_TWIM_SHORT_LASTRX_STOP_MASK)
#define IMU_ID_PIN_CNF ((uint32_t)NRF_GPIO_PIN_S0D1 << GPIO_PIN_CNF_DRIVE_Pos)

BUILD_ASSERT(DT_REG_ADDR(DT_NODELABEL(i2c1)) == 0x50009000UL,
	     "Only factory TWIM1 is permitted");
BUILD_ASSERT(!DT_NODE_HAS_STATUS(DT_NODELABEL(i2c1), okay) &&
	     !DT_NODE_HAS_STATUS(DT_NODELABEL(spi1), okay) &&
	     !DT_NODE_HAS_STATUS(DT_NODELABEL(uart1), okay),
	     "No driver may own the shared serial peripheral");
BUILD_ASSERT(!IS_ENABLED(CONFIG_I2C) && !IS_ENABLED(CONFIG_NRFX_TWIM1) &&
	     !IS_ENABLED(CONFIG_SENSOR), "No automatic I2C/sensor initialization");
BUILD_ASSERT(NRF_TWIM_FREQ_100K == 0x01980000UL && IMU_ID_MAX_ATTEMPTS == 3,
	     "Diagnostic frequency/transfer bounds changed");

static NRF_TWIM_Type *const imu_id_twim =
	(NRF_TWIM_Type *)DT_REG_ADDR(DT_NODELABEL(i2c1));
static const uint32_t imu_id_bus_pins[2] __attribute__((used, retain)) = {38U, 46U};
static const uint32_t imu_id_address __attribute__((used, retain)) = 0x6aU;
static const uint8_t imu_id_register __attribute__((used, retain)) = 0x0fU;
/* EasyDMA storage survives every failure path; never clear/reuse if STOP is
 * unconfirmed. The only writable bus byte is a register-address selector. */
static __aligned(4) uint8_t imu_id_tx[1] = {0x0f};
static __aligned(4) uint8_t imu_id_rx[1];
static atomic_t attempted;
static atomic_t busy;
static atomic_t fault;
static atomic_t stopped = ATOMIC_INIT(1);
static atomic_t bus_released = ATOMIC_INIT(1);

void imu_id_get_state(struct imu_id_state *state)
{
	if (state == NULL) { return; }
	state->attempted = atomic_get(&attempted) != 0;
	state->busy = atomic_get(&busy) != 0;
	state->fault_latched = atomic_get(&fault) != 0;
	state->clock_stopped = atomic_get(&stopped) != 0;
	state->bus_released = atomic_get(&bus_released) != 0;
}

static void result_state(struct imu_id_result *result)
{
	result->clock_stopped = atomic_get(&stopped) != 0;
	result->bus_released = atomic_get(&bus_released) != 0;
	result->fault_latched = atomic_get(&fault) != 0;
}

static bool peripheral_idle(void)
{
	return imu_id_twim->ENABLE == 0 && imu_id_twim->SHORTS == 0 &&
	       imu_id_twim->INTENSET == 0 &&
	       imu_id_twim->SUBSCRIBE_STARTRX == 0 &&
	       imu_id_twim->SUBSCRIBE_STARTTX == 0 &&
	       imu_id_twim->SUBSCRIBE_STOP == 0 &&
	       imu_id_twim->SUBSCRIBE_SUSPEND == 0 &&
	       imu_id_twim->SUBSCRIBE_RESUME == 0 &&
	       imu_id_twim->PUBLISH_STOPPED == 0 &&
	       imu_id_twim->PUBLISH_ERROR == 0 &&
	       imu_id_twim->PUBLISH_SUSPENDED == 0 &&
	       imu_id_twim->PUBLISH_RXSTARTED == 0 &&
	       imu_id_twim->PUBLISH_TXSTARTED == 0 &&
	       imu_id_twim->PUBLISH_LASTRX == 0 &&
	       imu_id_twim->PUBLISH_LASTTX == 0 &&
	       (imu_id_twim->PSEL.SDA & BIT(31)) != 0 &&
	       (imu_id_twim->PSEL.SCL & BIT(31)) != 0 &&
	       imu_id_twim->TXD.LIST == 0 && imu_id_twim->RXD.LIST == 0 &&
	       imu_id_twim->TXD.PTR == 0 && imu_id_twim->RXD.PTR == 0 &&
	       imu_id_twim->TXD.MAXCNT == 0 && imu_id_twim->RXD.MAXCNT == 0;
}

static bool pins_idle(void)
{
	return NRF_P1->PIN_CNF[6] == 2U && NRF_P1->PIN_CNF[14] == 2U;
}

static bool lines_high(void)
{
	return nrf_gpio_pin_read(IMU_ID_SDA) == 1U &&
	       nrf_gpio_pin_read(IMU_ID_SCL) == 1U;
}

/* ERROR breaks the transfer wait immediately; STOP cleanup ignores ERROR so
 * its presence cannot conceal a subsequently completed STOP. No recovery. */
static int wait_stopped(int64_t deadline, bool break_on_error)
{
	uint32_t begin = k_cycle_get_32();
	uint32_t limit = k_us_to_cyc_ceil32(IMU_ID_EVENT_US);
	for (uint32_t poll = 0; poll < IMU_ID_POLL_LIMIT; ++poll) {
		if ((uint32_t)(k_cycle_get_32() - begin) >= limit || k_uptime_get() >= deadline) {
			return -ETIMEDOUT;
		}
		if (break_on_error && nrf_twim_event_check(imu_id_twim, NRF_TWIM_EVENT_ERROR)) {
			return -EIO;
		}
		if (nrf_twim_event_check(imu_id_twim, NRF_TWIM_EVENT_STOPPED)) { return 0; }
		k_busy_wait(1);
	}
	return -ETIMEDOUT;
}

static int transfer(struct imu_id_attempt *attempt, int64_t deadline, bool *started)
{
	*started = false;
	if (k_uptime_get() >= deadline) { return -ETIMEDOUT; }
	if (!lines_high()) { return -EIO; }
	/* Prior transfer must already have proved STOPPED before reuse. */
	if (!atomic_get(&stopped) || imu_id_tx[0] != imu_id_register) { return -EPERM; }
	imu_id_rx[0] = 0;
	nrf_twim_event_clear(imu_id_twim, NRF_TWIM_EVENT_STOPPED);
	nrf_twim_event_clear(imu_id_twim, NRF_TWIM_EVENT_ERROR);
	nrf_twim_event_clear(imu_id_twim, NRF_TWIM_EVENT_LASTTX);
	nrf_twim_event_clear(imu_id_twim, NRF_TWIM_EVENT_LASTRX);
	(void)nrf_twim_errorsrc_get_and_clear(imu_id_twim);
	nrf_twim_tx_buffer_set(imu_id_twim, imu_id_tx, sizeof(imu_id_tx));
	nrf_twim_rx_buffer_set(imu_id_twim, imu_id_rx, sizeof(imu_id_rx));
	nrf_twim_shorts_set(imu_id_twim, IMU_ID_SHORTS);
	if (imu_id_twim->TXD.MAXCNT != 1U || imu_id_twim->RXD.MAXCNT != 1U ||
	    imu_id_twim->TXD.PTR != (uintptr_t)imu_id_tx ||
	    imu_id_twim->RXD.PTR != (uintptr_t)imu_id_rx ||
	    imu_id_twim->SHORTS != IMU_ID_SHORTS) { return -EIO; }
	atomic_clear(&stopped);
	__DMB();
	nrf_twim_task_trigger(imu_id_twim, NRF_TWIM_TASK_STARTTX);
	*started = true;
	int err = wait_stopped(deadline, true);
	/* Prevent an error path from starting the RX shortcut. Never issue a new
 * START after error, and never pulse GPIOs to recover another bus device. */
	nrf_twim_shorts_set(imu_id_twim, 0);
	if (err != 0 && !nrf_twim_event_check(imu_id_twim, NRF_TWIM_EVENT_STOPPED)) {
		nrf_twim_task_trigger(imu_id_twim, NRF_TWIM_TASK_STOP);
		/* Independent cleanup budget even when the operation deadline expired. */
		if (wait_stopped(k_uptime_get() + 3, false) != 0) {
			atomic_set(&fault, 1);
			return -EBUSY;
		}
	}
	__DMB();
	atomic_set(&stopped, 1);
	attempt->tx_amount = (uint32_t)nrf_twim_txd_amount_get(imu_id_twim);
	attempt->rx_amount = (uint32_t)nrf_twim_rxd_amount_get(imu_id_twim);
	attempt->errorsrc = nrf_twim_errorsrc_get_and_clear(imu_id_twim);
	attempt->who_am_i = imu_id_rx[0];
	if (err != 0) { return err; }
	if (attempt->errorsrc != 0 || attempt->tx_amount != 1U ||
	    attempt->rx_amount != 1U || !lines_high()) { return -EIO; }
	return 0;
}

static void cleanup(void)
{
	if (!atomic_get(&stopped)) {
		/* Permanent buffers, DMA routing and ENABLE are retained until reset. */
		atomic_set(&fault, 1);
		return;
	}
	nrf_twim_shorts_set(imu_id_twim, 0);
	nrf_twim_disable(imu_id_twim);
	if (imu_id_twim->ENABLE != 0 || imu_id_twim->SHORTS != 0) {
		atomic_set(&fault, 1);
		return;
	}
	nrf_twim_pins_set(imu_id_twim, UINT32_MAX, UINT32_MAX);
	nrf_twim_tx_buffer_set(imu_id_twim, NULL, 0);
	nrf_twim_rx_buffer_set(imu_id_twim, NULL, 0);
	for (size_t i = 0; i < ARRAY_SIZE(imu_id_bus_pins); ++i) {
		nrf_gpio_cfg_default(imu_id_bus_pins[i]);
	}
	bool released = peripheral_idle() && pins_idle();
	atomic_set(&bus_released, released);
	if (!released) { atomic_set(&fault, 1); }
	/* RX is an ID byte, but scrub only after hardware relinquished DMA. */
	volatile uint8_t *wipe = imu_id_rx;
	*wipe = 0;
}

int imu_id_probe(struct imu_id_result *result)
{
	if (result == NULL) { return -EINVAL; }
	memset(result, 0, sizeof(*result));
	if (!atomic_cas(&busy, 0, 1)) {
		result->rc = -EBUSY;
		result_state(result);
		return result->rc;
	}
	int64_t begin = k_uptime_get();
	if (atomic_get(&attempted) || atomic_get(&fault)) {
		result->rc = -EALREADY;
		goto done;
	}
	if (!peripheral_idle() || !pins_idle()) {
		result->rc = -EPERM;
		result->outcome = IMU_ID_PRECONDITION_ERROR;
		/* Unknown ownership is not ours to repair or claim safe. No writes. */
		atomic_set(&fault, 1);
		atomic_clear(&stopped);
		atomic_clear(&bus_released);
		goto done;
	}
	atomic_set(&attempted, 1);
	atomic_clear(&bus_released);
	/* Input-connected S0D1 matches Nordic TWIM GPIO mode. Preload HIGH even
 * though GPIO direction stays input; never drive high against a low device.
 * External board pull-ups only: no internal pull or power-rail changes. */
	for (size_t i = 0; i < ARRAY_SIZE(imu_id_bus_pins); ++i) {
		nrf_gpio_pin_set(imu_id_bus_pins[i]);
	}
	for (size_t i = 0; i < ARRAY_SIZE(imu_id_bus_pins); ++i) {
		nrf_gpio_cfg(imu_id_bus_pins[i], NRF_GPIO_PIN_DIR_INPUT,
			NRF_GPIO_PIN_INPUT_CONNECT, NRF_GPIO_PIN_NOPULL,
			NRF_GPIO_PIN_S0D1, NRF_GPIO_PIN_NOSENSE);
	}
	if (NRF_P1->PIN_CNF[6] != IMU_ID_PIN_CNF ||
	    NRF_P1->PIN_CNF[14] != IMU_ID_PIN_CNF ||
	    nrf_gpio_pin_out_read(IMU_ID_SDA) != 1U ||
	    nrf_gpio_pin_out_read(IMU_ID_SCL) != 1U) {
		result->rc = -EIO;
		result->outcome = IMU_ID_PRECONDITION_ERROR;
		atomic_set(&fault, 1);
		goto finish;
	}
	k_busy_wait(5);
	if (!lines_high()) {
		result->rc = -EIO;
		result->outcome = IMU_ID_INCONCLUSIVE;
		goto finish;
	}
	nrf_twim_frequency_set(imu_id_twim, NRF_TWIM_FREQ_100K);
	nrf_twim_address_set(imu_id_twim, (uint8_t)imu_id_address);
	nrf_twim_pins_set(imu_id_twim, IMU_ID_SCL, IMU_ID_SDA);
	if (imu_id_twim->FREQUENCY != (uint32_t)NRF_TWIM_FREQ_100K ||
	    imu_id_twim->ADDRESS != imu_id_address ||
	    imu_id_twim->PSEL.SDA != IMU_ID_SDA || imu_id_twim->PSEL.SCL != IMU_ID_SCL) {
		result->rc = -EIO;
		result->outcome = IMU_ID_PRECONDITION_ERROR;
		atomic_set(&fault, 1);
		goto finish;
	}
	nrf_twim_enable(imu_id_twim);
	if (imu_id_twim->ENABLE != TWIM_ENABLE_ENABLE_Enabled) {
		result->rc = -EIO;
		result->outcome = IMU_ID_PRECONDITION_ERROR;
		atomic_set(&fault, 1);
		goto finish;
	}
	for (uint8_t i = 0; i < IMU_ID_MAX_ATTEMPTS; ++i) {
		struct imu_id_attempt *a = &result->attempts[i];
		bool started;
		a->rc = transfer(a, begin + IMU_ID_TOTAL_MS, &started);
		if (started) { ++result->attempt_count; }
		if (a->rc != 0) {
			result->rc = a->rc;
			result->outcome = atomic_get(&stopped) ? IMU_ID_TRANSFER_ERROR : IMU_ID_STOP_ERROR;
			goto finish;
		}
		if (a->who_am_i != 0x6aU) {
			result->rc = -EPROTO;
			result->outcome = IMU_ID_UNEXPECTED;
			goto finish;
		}
	}
	result->rc = 0;
	result->outcome = IMU_ID_CONFIRMED;
finish:
	cleanup();
	if (atomic_get(&fault) && result->rc == 0) {
		result->rc = -EIO;
		result->outcome = IMU_ID_TRANSFER_ERROR;
	}
done:
	result->elapsed_ms = (uint32_t)(k_uptime_get() - begin);
	result_state(result);
	atomic_clear(&busy);
	return result->rc;
}
