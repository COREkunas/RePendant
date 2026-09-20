/*
 * Bounded microphone diagnostics; explicitly requested playback retains only
 * post-warm-up PCM in a separately bounded, expiring RAM store.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mic_diagnostics.h"
#include "mic_frame_math.h"
#include "mic_pcm_store.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>

#include <soc.h>

#include <zephyr/audio/dmic.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#define MIC_BLOCK_BYTES (MIC_DIAG_BLOCK_SAMPLES * sizeof(int16_t))
#define MIC_BLOCK_COUNT 6U
BUILD_ASSERT(MIC_DIAG_WARMUP_SAMPLES % MIC_DIAG_BLOCK_SAMPLES == 0 &&
	     MIC_PCM_STORE_MAX_SAMPLES + MIC_DIAG_WARMUP_SAMPLES ==
		MIC_DIAG_SAMPLE_RATE * MIC_DIAG_MAX_CAPTURE_MS / 1000U &&
	     MIC_PCM_STORE_FRAME_SAMPLES == MIC_DIAG_BLOCK_SAMPLES,
	     "Playback storage must cover only the post-startup part of capture");
BUILD_ASSERT(MIC_DIAG_WINDOW_SAMPLES % MIC_DIAG_BLOCK_SAMPLES == 0,
	     "A DMA block must not cross a statistics window boundary");
BUILD_ASSERT(MIC_DIAG_BLOCK_SAMPLES == MIC_FRAME_MATH_SAMPLES &&
	     MIC_DIAG_WINDOW_SAMPLES == 8000U,
	     "Frame-variance arithmetic and host protocol are bounded to 320/8000");
BUILD_ASSERT(MIC_DIAG_WINDOW_COUNT * MIC_DIAG_WINDOW_SAMPLES ==
	     MIC_DIAG_SAMPLE_RATE * MIC_DIAG_MAX_CAPTURE_MS / 1000U,
	     "Statistics windows must cover only the existing capture bound");
/* Diagnostic clock choice, NOT the factory ACLK/1.024 MHz configuration.
 * The SDK's nRF53 MDK fixups enable the documented general clock formula,
 * not the legacy enumerated table. PCLK32M_HFXO / 25 = 1.280 MHz;
 * / 80 = exactly 16 kHz PCM. The clock lies
 * between recovered factory PDM modes, but microphone part limits still need
 * physical validation. Never silently accept another clock or sample rate.
 */
#define MIC_PDM_CLOCK_HZ 1280000U
/* Nordic nRF5340 PS: 4096 * floor(f_pdm * 1048576 /
 * (f_source + f_pdm / 2)). Do not compare to deprecated FREQ_1280K:
 * 0x0A000000 and this SDK's 0x0A0A0000 both yield divider 25.
 */
#define MIC_CLOCK_FACTOR ((MIC_PDM_CLOCK_HZ * 1048576ULL) / \
			 (32000000ULL + MIC_PDM_CLOCK_HZ / 2U))
#define MIC_CLOCK_REGISTER (MIC_CLOCK_FACTOR * 4096ULL)
BUILD_ASSERT(MIC_CLOCK_REGISTER == 0x0A0A0000ULL, "Unexpected PDM clock encoding");
BUILD_ASSERT(32000000ULL / (1048576ULL / MIC_CLOCK_FACTOR) == MIC_PDM_CLOCK_HZ,
	     "PDM clock must be exactly 1.280 MHz");
#define MIC_POWER_SETTLE_MS 50U
#define MIC_READ_TIMEOUT_MS 100U
#define MIC_STOP_TIMEOUT_MS 150U

static const struct gpio_dt_spec mic_power =
	GPIO_DT_SPEC_GET(DT_NODELABEL(pendant_mic_power), gpios);
static const struct device *const pdm = DEVICE_DT_GET(DT_NODELABEL(pdm0));
static const NRF_PDM_Type *const pdm_registers =
	(const NRF_PDM_Type *)DT_REG_ADDR(DT_NODELABEL(pdm0));

/* Enum order is pinned by NCS 3.4.0 nordic,nrf-pdm.yaml: PCLK32M,
 * PCLK32M_HFXO, ACLK. This diagnostic deliberately uses HFXO, not ACLK.
 */
BUILD_ASSERT(DT_ENUM_IDX(DT_NODELABEL(pdm0), clock_source) == 1,
	     "PDM diagnostics require explicit PCLK32M_HFXO");
BUILD_ASSERT(DT_SAME_NODE(DT_GPIO_CTLR(DT_NODELABEL(pendant_mic_power), gpios),
			 DT_NODELABEL(gpio1)) &&
	     DT_GPIO_PIN(DT_NODELABEL(pendant_mic_power), gpios) == 1,
	     "Microphone power must be confirmed P1.01");
BUILD_ASSERT(DT_GPIO_FLAGS(DT_NODELABEL(pendant_mic_power), gpios) == GPIO_ACTIVE_HIGH,
	     "Microphone power must be active high without extra GPIO flags");

K_MEM_SLAB_DEFINE_STATIC(pcm_slab, MIC_BLOCK_BYTES, MIC_BLOCK_COUNT, 4);
K_MUTEX_DEFINE(diagnostic_lock);

static atomic_t initialized;
static atomic_t busy;
static atomic_t powered;
static atomic_t fault_latched;
static atomic_t clock_stopped = ATOMIC_INIT(1);
static atomic_t buffers_scrubbed;
static atomic_t last_cleanup_error;

/* Read-only guard of the SDK driver's actual configuration, BEFORE rail ON
 * or START. No direct register mutation bypasses the standard DMIC driver.
 */
static uint32_t inspect_pdm_configuration(struct mic_diagnostics_stats *stats)
{
	stats->pdm_clkctrl = pdm_registers->PDMCLKCTRL;
	stats->pdm_ratio = pdm_registers->RATIO;
	stats->pdm_mclk = pdm_registers->MCLKCONFIG;
	stats->pdm_mode = pdm_registers->MODE;
	stats->pdm_gainl = pdm_registers->GAINL;
	stats->pdm_gainr = pdm_registers->GAINR;
	stats->pdm_clkpin = pdm_registers->PSEL.CLK;
	stats->pdm_datapin = pdm_registers->PSEL.DIN;
	return (stats->pdm_clkctrl != MIC_CLOCK_REGISTER ? BIT(0) : 0) |
	       (stats->pdm_ratio != PDM_RATIO_RATIO_Ratio80 ? BIT(1) : 0) |
	       (stats->pdm_mclk != PDM_MCLKCONFIG_SRC_PCLK32M ? BIT(2) : 0) |
	       (stats->pdm_mode !=
		((PDM_MODE_OPERATION_Mono << PDM_MODE_OPERATION_Pos) |
		 (PDM_MODE_EDGE_LeftFalling << PDM_MODE_EDGE_Pos)) ? BIT(3) : 0) |
	       (stats->pdm_gainl != PDM_GAINL_GAINL_DefaultGain ? BIT(4) : 0) |
	       (stats->pdm_gainr != PDM_GAINR_GAINR_DefaultGain ? BIT(5) : 0) |
	       (stats->pdm_clkpin != 4 ? BIT(6) : 0) |
	       (stats->pdm_datapin != 5 ? BIT(7) : 0);
}

/* Volatile writes prevent removal of the diagnostic PCM erasure. Only owned
 * blocks may be scrubbed: never write a slab free-list or DMA-owned block.
 */
static void erase_owned_buffer(void *buffer)
{
	volatile uint8_t *p = buffer;

	for (size_t i = 0; i < MIC_BLOCK_BYTES; ++i) {
		p[i] = 0;
	}
}

static void release_owned_buffer(void *buffer)
{
	erase_owned_buffer(buffer);
	k_mem_slab_free(&pcm_slab, buffer);
}

/* Called after STOP; driver-owned blocks are returned asynchronously. */
static void drain_received_buffers(void)
{
	for (uint32_t i = 0; i < MIC_BLOCK_COUNT; ++i) {
		void *buffer = NULL;
		size_t size = 0;

		if (dmic_read(pdm, 0, &buffer, &size, 0) != 0) {
			break;
		}
		if (buffer != NULL) {
			release_owned_buffer(buffer);
		}
	}
}

static bool scrub_free_blocks(void)
{
	void *owned[MIC_BLOCK_COUNT] = {0};
	uint32_t count = 0;

	/* Hold each allocation so a newly freed block cannot be allocated twice. */
	while (count < MIC_BLOCK_COUNT &&
	       k_mem_slab_alloc(&pcm_slab, &owned[count], K_NO_WAIT) == 0) {
		erase_owned_buffer(owned[count]);
		++count;
	}
	for (uint32_t i = 0; i < count; ++i) {
		k_mem_slab_free(&pcm_slab, owned[i]);
	}
	return count == MIC_BLOCK_COUNT;
}

static int set_power(bool enable)
{
	int err = gpio_pin_set_dt(&mic_power, enable ? 1 : 0);

	if (err == 0) {
		atomic_set(&powered, enable ? 1 : 0);
	}
	return err;
}

static int begin_operation(void)
{
	int err = k_mutex_lock(&diagnostic_lock, K_NO_WAIT);

	if (err != 0) {
		return -EBUSY;
	}
	if (!atomic_get(&initialized) || atomic_get(&fault_latched)) {
		k_mutex_unlock(&diagnostic_lock);
		return -EIO;
	}
	atomic_set(&busy, 1);
	atomic_set(&last_cleanup_error, 0);
	return 0;
}

static void end_operation(void)
{
	atomic_clear(&busy);
	k_mutex_unlock(&diagnostic_lock);
}

int mic_diagnostics_init(void)
{
	int err = k_mutex_lock(&diagnostic_lock, K_NO_WAIT);

	if (err != 0) {
		return -EBUSY;
	}
	if (atomic_get(&initialized)) {
		err = atomic_get(&fault_latched) ? -EIO : 0;
		goto out;
	}
	if (!gpio_is_ready_dt(&mic_power)) {
		err = -ENODEV;
		goto out;
	}

	/* Confirmed active-high P1.01. Neither microphone clock is started. */
	err = gpio_pin_configure_dt(&mic_power, GPIO_OUTPUT_INACTIVE);
	if (err != 0) {
		goto out;
	}
	atomic_clear(&powered);
	if (!device_is_ready(pdm)) {
		err = -ENODEV;
		goto out;
	}
	if (!scrub_free_blocks()) {
		err = -EIO;
		goto out;
	}
	atomic_set(&buffers_scrubbed, 1);
	atomic_set(&initialized, 1);
out:
	k_mutex_unlock(&diagnostic_lock);
	return err;
}

int mic_diagnostics_rail_check(uint32_t duration_ms)
{
	int err;
	int off_err;

	if (duration_ms == 0 || duration_ms > MIC_DIAG_MAX_RAIL_CHECK_MS) {
		return -EINVAL;
	}
	err = begin_operation();
	if (err != 0) {
		return err;
	}
	err = set_power(true);
	if (err == 0) {
		k_msleep(duration_ms);
	}
	off_err = set_power(false);
	if (off_err != 0) {
		atomic_set(&fault_latched, 1);
		atomic_set(&last_cleanup_error, off_err);
	}
	end_operation();
	return err != 0 ? err : off_err;
}

/* Nordic's DMIC STOP is asynchronous and does not drain the RX queue. A
 * successful zero-rate configure, after all slab blocks return, uninitializes
 * nrfx PDM and disables its peripheral/interrupts. A stuck clock-start/STOP
 * handshake must not be spun on forever or be called successfully stopped.
 */
static int stop_and_scrub(struct dmic_cfg *cfg, bool configured)
{
	int err = 0;
	bool stopped = !configured;
	bool scrubbed;
	int off_err;

	if (configured) {
		int stop_err = dmic_trigger(pdm, DMIC_TRIGGER_STOP);
		int64_t deadline = k_uptime_get() + MIC_STOP_TIMEOUT_MS;

		cfg->streams[0].pcm_rate = 0;
		if (stop_err != 0) {
			err = stop_err;
		} else {
			for (;;) {
				drain_received_buffers();
				/* Avoid uninitializing while the nrfx STOP handler still
				 * owns its final DMA buffer. Disable only when none remain.
				 */
				if (k_mem_slab_num_used_get(&pcm_slab) == 0) {
					int disable_err = dmic_configure(pdm, cfg);

					if (disable_err == 0) {
						stopped = true;
						break;
					}
					if (disable_err != -EBUSY) {
						err = disable_err;
						break;
					}
				}
				if (k_uptime_get() >= deadline) {
					err = -ETIMEDOUT;
					break;
				}
				k_msleep(1);
			}
		}
	}
	atomic_set(&clock_stopped, stopped ? 1 : 0);
	scrubbed = scrub_free_blocks();
	atomic_set(&buffers_scrubbed, (stopped && scrubbed) ? 1 : 0);
	if (!scrubbed && err == 0) {
		err = -EIO;
	}

	/* Normal path: clocks stopped and PCM erased before rail OFF. Exceptional
	 * path still powers OFF, but latches a fault: caller MUST immediately reboot
	 * normally (not recovery), because a late driver clock callback may exist.
	 * No assertions are made about erasing outstanding DMA-owned memory.
	 */
	off_err = set_power(false);
	if (err == 0) {
		err = off_err;
	}
	atomic_set(&last_cleanup_error, err);
	if (err != 0 || !stopped) {
		atomic_set(&fault_latched, 1);
	}
	return err;
}

int mic_diagnostics_capture(uint32_t duration_ms,
			    struct mic_diagnostics_stats *stats, bool retain_audio)
{
	struct pcm_stream_cfg stream = {
		.pcm_rate = MIC_DIAG_SAMPLE_RATE,
		.pcm_width = 16,
		.block_size = MIC_BLOCK_BYTES,
		.mem_slab = &pcm_slab,
	};
	struct dmic_cfg cfg = {
		.io = {
			.min_pdm_clk_freq = MIC_PDM_CLOCK_HZ,
			.max_pdm_clk_freq = MIC_PDM_CLOCK_HZ,
			.min_pdm_clk_dc = 50,
			.max_pdm_clk_dc = 50,
		},
		.streams = &stream,
		.channel = {
			.req_num_streams = 1,
			.req_num_chan = 1,
		},
	};
	uint64_t sum_abs = 0;
	uint64_t sum_square = 0;
	int64_t started_at;
	int64_t deadline;
	bool configured = false;
	int err;

	if (stats == NULL) {
		return -EINVAL;
	}
	memset(stats, 0, sizeof(*stats));
	if (duration_ms < 20 || duration_ms > MIC_DIAG_MAX_CAPTURE_MS ||
	    duration_ms % 20 != 0) {
		stats->capture_error = -EINVAL;
		return -EINVAL;
	}
	err = begin_operation();
	if (err != 0) {
		stats->capture_error = err;
		return err;
	}
	started_at = k_uptime_get();
	stats->minimum = INT16_MAX;
	stats->maximum = INT16_MIN;
	cfg.channel.req_chan_map_lo = dmic_build_channel_map(0, 0, PDM_CHAN_LEFT);
	stats->config_stage = 1; /* Configure, with microphone power still OFF. */
	err = dmic_configure(pdm, &cfg);
	stats->config_mismatch = inspect_pdm_configuration(stats);
	if (err != 0) {
		goto cleanup;
	}
	configured = true;
	stats->config_stage = 2; /* Validate exact configuration before power. */
	if (cfg.channel.act_num_streams != 1 || cfg.channel.act_num_chan != 1 ||
	    cfg.channel.act_chan_map_lo != cfg.channel.req_chan_map_lo ||
	    cfg.channel.act_chan_map_hi != 0 || stream.pcm_rate != MIC_DIAG_SAMPLE_RATE ||
	    stream.pcm_width != 16 || stream.block_size != MIC_BLOCK_BYTES) {
		stats->config_mismatch |= BIT(8);
	}
	if (stats->config_mismatch != 0) {
		err = -EINVAL;
		goto cleanup;
	}
	stats->config_stage = 3;
	err = set_power(true);
	if (err != 0) {
		goto cleanup;
	}
	k_msleep(MIC_POWER_SETTLE_MS);

	/* The capture deadline includes asynchronous HFXO startup, not just the
	 * received blocks. STOP/cleanup has its own bounded 150 ms deadline.
	 */
	deadline = k_uptime_get() + duration_ms;
	atomic_clear(&clock_stopped);
	atomic_clear(&buffers_scrubbed);
	stats->config_stage = 4;
	err = dmic_trigger(pdm, DMIC_TRIGGER_START);
	if (err != 0) {
		goto cleanup;
	}
	stats->config_stage = 5;
	while (stats->blocks < duration_ms / 20U) {
		void *buffer = NULL;
		size_t size = 0;
		int64_t remaining = deadline - k_uptime_get();

		if (remaining <= 0) {
			break;
		}
		err = dmic_read(pdm, 0, &buffer, &size,
				(int32_t)MIN(remaining, MIC_READ_TIMEOUT_MS));
		if (err != 0) {
			/* A final partial 20 ms block need not complete by deadline. */
			if ((err == -EAGAIN || err == -ENOMSG) &&
			    k_uptime_get() >= deadline && stats->samples != 0) {
				err = 0;
			}
			break;
		}
		if (buffer == NULL || size != MIC_BLOCK_BYTES) {
			if (buffer != NULL) {
				release_owned_buffer(buffer);
			}
			err = -EIO;
			break;
		}
		const int16_t *samples = buffer;
		uint32_t window_index = stats->samples / MIC_DIAG_WINDOW_SAMPLES;
		if (window_index >= MIC_DIAG_WINDOW_COUNT) {
			release_owned_buffer(buffer);
			err = -EOVERFLOW;
			break;
		}
		struct mic_diagnostics_window *window = &stats->windows[window_index];
		int64_t frame_signed_sum = 0;
		uint64_t frame_sum_square = 0;
		uint64_t frame_ac;
		/* Validate exact per-frame arithmetic before committing this frame
		 * to the legacy or window aggregates. Buffer is application-owned.
		 */
		for (uint32_t i = 0; i < MIC_DIAG_BLOCK_SAMPLES; ++i) {
			int64_t value = samples[i];
			frame_signed_sum += value;
			frame_sum_square += (uint64_t)(value * value);
		}
		if (!mic_frame_ac_numerator(frame_signed_sum, frame_sum_square, &frame_ac)) {
			release_owned_buffer(buffer);
			err = -EIO;
			break;
		}
		/* Discard the first 50 complete frames (one second). The original
		 * two-second deadline is unchanged; never retain DMA ownership.
		 * No PCM reaches USB until the caller verifies STOP and rail OFF.
		 */
		if (retain_audio && stats->samples >= MIC_DIAG_WARMUP_SAMPLES) {
			err = mic_pcm_store_append(samples, MIC_DIAG_BLOCK_SAMPLES);
			if (err != 0) {
				release_owned_buffer(buffer);
				break;
			}
		}
		if (window->samples == 0) {
			window->minimum = INT16_MAX;
			window->maximum = INT16_MIN;
		}

		for (uint32_t i = 0; i < MIC_DIAG_BLOCK_SAMPLES; ++i) {
			int32_t value = samples[i];
			uint32_t magnitude = value < 0 ? (uint32_t)(-value) :
							(uint32_t)value;

			stats->minimum = MIN(stats->minimum, value);
			stats->maximum = MAX(stats->maximum, value);
			stats->nonzero += value != 0;
			stats->saturated += value == INT16_MIN || value == INT16_MAX;
			sum_abs += magnitude;
			sum_square += (uint64_t)((int64_t)value * value);
			window->minimum = MIN(window->minimum, value);
			window->maximum = MAX(window->maximum, value);
			window->nonzero += value != 0;
			window->saturated += value == INT16_MIN || value == INT16_MAX;
			window->signed_sum += value;
			window->sum_abs += magnitude;
			window->sum_square += (uint64_t)((int64_t)value * value);
		}
		/* Exact frame-centred energy numerator, not a PCM filter. For 320
		 * int16 samples each product is <= 109951162777600 (< 2^47).
		 * Cauchy's inequality guarantees nonnegative subtraction. Across 25
		 * frames the accumulated numerator remains below 2^52.
		 */
		window->frame_ac_sum += frame_ac;
		window->frame_ac_max = MAX(window->frame_ac_max, frame_ac);
		window->samples += MIC_DIAG_BLOCK_SAMPLES;
		++window->blocks;
		stats->samples += MIC_DIAG_BLOCK_SAMPLES;
		++stats->blocks;
		release_owned_buffer(buffer);
	}
	if (stats->samples == 0 && err == 0) {
		err = -ETIMEDOUT;
	}
	if (err == 0) { stats->config_stage = 6; }
cleanup:
	stats->capture_error = err;
	stats->cleanup_error = stop_and_scrub(&cfg, configured);
	stats->elapsed_ms = (uint32_t)(k_uptime_get() - started_at);
	stats->clock_stopped = atomic_get(&clock_stopped) != 0;
	stats->buffers_scrubbed = atomic_get(&buffers_scrubbed) != 0;
	stats->power_off = !atomic_get(&powered);
	if (stats->samples != 0) {
		stats->mean_abs = (uint32_t)(sum_abs / stats->samples);
		stats->mean_square = (uint32_t)(sum_square / stats->samples);
	} else {
		stats->minimum = 0;
		stats->maximum = 0;
	}
	end_operation();
	return err != 0 ? err : stats->cleanup_error;
}

void mic_diagnostics_get_state(struct mic_diagnostics_state *state)
{
	if (state == NULL) {
		return;
	}
	state->initialized = atomic_get(&initialized) != 0;
	state->busy = atomic_get(&busy) != 0;
	state->powered = atomic_get(&powered) != 0;
	state->fault_latched = atomic_get(&fault_latched) != 0;
	state->clock_stopped = atomic_get(&clock_stopped) != 0;
	state->buffers_scrubbed = atomic_get(&buffers_scrubbed) != 0;
	state->last_cleanup_error = (int)atomic_get(&last_cleanup_error);
}

bool mic_diagnostics_power_is_on(void)
{
	return atomic_get(&powered) != 0;
}
