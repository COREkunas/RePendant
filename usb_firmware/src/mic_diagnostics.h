/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OPENPENDANT_MIC_DIAGNOSTICS_H_
#define OPENPENDANT_MIC_DIAGNOSTICS_H_

#include <stdbool.h>
#include <stdint.h>

#define MIC_DIAG_SAMPLE_RATE 16000U
#define MIC_DIAG_BLOCK_SAMPLES 320U
#define MIC_DIAG_MAX_CAPTURE_MS 2000U
#define MIC_DIAG_MAX_RAIL_CHECK_MS 5000U
#define MIC_DIAG_WINDOW_SAMPLES 8000U
#define MIC_DIAG_WINDOW_COUNT 4U
#define MIC_DIAG_WARMUP_SAMPLES 16000U

/* Four sample-indexed 500 ms windows at 16 kHz. Exact sums allow the host to
 * distinguish DC from varying signal. Empty/partial final windows are allowed;
 * collecting windows must never extend the capture deadline.
 */
struct mic_diagnostics_window {
	int64_t signed_sum;
	uint64_t sum_abs;
	uint64_t sum_square;
	/* Per-frame numerator: 320*sum(x*x) - sum(x)*sum(x). Sum/max are
	 * accumulated without rounding or modifying any captured sample.
	 */
	uint64_t frame_ac_sum;
	uint64_t frame_ac_max;
	uint32_t samples;
	uint32_t blocks;
	uint32_t nonzero;
	uint32_t saturated;
	int16_t minimum;
	int16_t maximum;
};

/* Aggregate measurements; optional playback PCM is owned by mic_pcm_store. */
struct mic_diagnostics_stats {
	/* Fixed pre-power configuration snapshot; no sample/address data. */
	uint32_t config_stage;
	uint32_t config_mismatch;
	uint32_t pdm_clkctrl;
	uint32_t pdm_ratio;
	uint32_t pdm_mclk;
	uint32_t pdm_mode;
	uint32_t pdm_gainl;
	uint32_t pdm_gainr;
	uint32_t pdm_clkpin;
	uint32_t pdm_datapin;
	uint32_t samples;
	uint32_t blocks;
	uint32_t nonzero;
	uint32_t saturated;
	uint32_t mean_abs;
	uint32_t mean_square;
	int16_t minimum;
	int16_t maximum;
	uint32_t elapsed_ms;
	int capture_error;
	int cleanup_error;
	bool clock_stopped;
	/* DMA slab only; retained playback PCM has separate store state. */
	bool buffers_scrubbed;
	bool power_off;
	struct mic_diagnostics_window windows[MIC_DIAG_WINDOW_COUNT];
};

struct mic_diagnostics_state {
	bool initialized;
	bool busy;
	bool powered;
	bool fault_latched;
	bool clock_stopped;
	/* DMA slab only; retained playback PCM has separate store state. */
	bool buffers_scrubbed;
	int last_cleanup_error;
};

/* Initializes only the power GPIO to OFF; never configures/starts capture. */
int mic_diagnostics_init(void);

/* Blocking manual operations. Invoke from the dedicated diagnostics worker,
 * never the Bluetooth callback/system workqueue. Concurrent calls fail -EBUSY.
 * Root command handler must gate capture on explicit rail-check confirmation.
 * Rail check enables only the shared microphone rail, not either audio clock.
 * Capture accepts multiples of 20 ms, at least 20 ms. Its sampling deadline
 * includes clock startup; a final incomplete block is discarded. Power settling
 * adds 50 ms and asynchronous STOP cleanup adds at most 150 ms to call duration.
 * If get_state().fault_latched becomes true, caller MUST immediately perform a
 * normal reboot after the call: late clock-start callbacks may otherwise exist.
 * Do not print a long result or return to command processing before that reset.
 * retain_audio requires a separately initialized mic_pcm_store and explicit
 * playback permission. Only post-warm-up complete frames are copied there;
 * caller must scrub it on every error and export only after verified cleanup.
 */
int mic_diagnostics_rail_check(uint32_t duration_ms);
int mic_diagnostics_capture(uint32_t duration_ms,
			    struct mic_diagnostics_stats *stats, bool retain_audio);

/* Snapshot for status output. GPIO state is software's last successful write,
 * not a voltmeter measurement of the rail.
 */
void mic_diagnostics_get_state(struct mic_diagnostics_state *state);
bool mic_diagnostics_power_is_on(void);

#endif /* OPENPENDANT_MIC_DIAGNOSTICS_H_ */
