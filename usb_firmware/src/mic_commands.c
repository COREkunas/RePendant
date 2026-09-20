/* Serialized USB diagnostics and physically confirmed encrypted BLE audio. */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>
#include "mic_commands.h"
#include "mic_diagnostics.h"
#include "mic_pcm_store.h"
#include "ble_audio.h"
#include "ble_security.h"

BUILD_ASSERT(IS_ENABLED(CONFIG_CBPRINTF_FULL_INTEGRAL),
	     "Exact microphone window sums require full 64-bit integer formatting");

static atomic_t busy;
static atomic_t initialized = ATOMIC_INIT(-EAGAIN);
static atomic_t rail_checked;
static atomic_t armed;
static const struct shell *request_shell;
enum request_mode { REQUEST_POWER, REQUEST_STATS, REQUEST_RECORD, REQUEST_BLE };
static enum request_mode requested_mode;
K_SEM_DEFINE(mic_request, 0, 1);

bool mic_commands_busy(void) { return atomic_get(&busy) != 0; }

int mic_commands_reserve_external(void)
{
	struct mic_diagnostics_state state;
	struct mic_pcm_store_info pcm;
	if (atomic_get(&initialized) != 0 || !atomic_cas(&busy, 0, 2)) {
		return -EBUSY;
	}
	/* The reservation is visible before checking pairing/recovery. Their
	 * admission checks observe the same busy flag, preventing both from
	 * committing across this check. Do not silently consume an audio arm.
	 */
	mic_diagnostics_get_state(&state);
	mic_pcm_store_get_state(&pcm);
	if (pendant_recovery_is_pending() || pendant_ble_pairing_busy() ||
	    atomic_get(&armed) != 0 || !state.initialized || state.busy ||
	    state.powered || state.fault_latched || !state.clock_stopped ||
	    !state.buffers_scrubbed || pcm.bytes_remaining != 0 || !pcm.scrubbed) {
		atomic_clear(&busy);
		return -EBUSY;
	}
	return 0;
}

void mic_commands_release_external(void)
{
	/* Never release a microphone worker's distinct busy=1 reservation. */
	(void)atomic_cas(&busy, 2, 0);
}

int mic_commands_init(void)
{
	mic_pcm_store_scrub();
	int err = mic_diagnostics_init();
	atomic_set(&initialized, err);
	return err;
}

int mic_commands_queue_ble(void)
{
	if (atomic_get(&initialized) != 0) { return -ENODEV; }
	if (pendant_recovery_is_pending() || !atomic_cas(&busy, 0, 1)) { return -EBUSY; }
	/* Pairing checks busy while holding its state lock. Claim first, then
	 * check that same lock, so pairing and sampling cannot both commit.
	 */
	if (pendant_recovery_is_pending() || pendant_ble_pairing_busy()) {
		atomic_clear(&busy); return -EBUSY;
	}
	/* A physical request cannot reuse or leave behind a USB one-shot arm. */
	atomic_clear(&armed);
	request_shell = NULL;
	requested_mode = REQUEST_BLE;
	k_sem_give(&mic_request);
	return 0;
}

int mic_commands_ble_capture(struct mic_pcm_store_info *info)
{
	/* Called only by the reserved microphone worker, never a BT callback.
	 * Keep this independent aggregate off the bounded worker stack.
	 */
	static struct mic_diagnostics_stats ble_stats;
	struct mic_diagnostics_state state;
	if (info == NULL || !mic_commands_busy()) { return -EINVAL; }
	memset(&ble_stats, 0, sizeof(ble_stats));
	mic_pcm_store_scrub();
	int err = mic_pcm_store_begin();
	if (err == 0) { err = pendant_audio_indicator(true); }
	if (err == 0) {
		err = mic_diagnostics_capture(MIC_DIAG_MAX_CAPTURE_MS, &ble_stats, true);
	}
	mic_diagnostics_get_state(&state);
	if (state.fault_latched || state.powered) {
		mic_pcm_store_scrub();
		sys_reboot(SYS_REBOOT_COLD);
	}
	int led_err = pendant_audio_indicator(false);
	if (err == 0 && led_err != 0) { err = led_err; }
	if (err == 0 && (!state.clock_stopped || !state.buffers_scrubbed ||
			state.powered || state.fault_latched)) { err = -EIO; }
	if (err == 0) { err = mic_pcm_store_seal(info); }
	if (err != 0) { mic_pcm_store_scrub(); }
	/* Deliberately no USB output, notify, persistent storage or pairing. */
	return err;
}

/* Only the microphone worker can export a sealed clip, and only after the
 * peripheral has stopped, DMA buffers have been scrubbed and rail is OFF.
 * Each transmitted chunk is removed from the store before USB output. The
 * independent expiry worker erases remaining PCM even if shell output stalls.
 * pcm_scrubbed covers the retained store only. If shell output stalls, up to
 * 128 encoded sample characters in this staging buffer and transport copies
 * may remain until output returns/reboot; capture is already OFF throughout.
 */
static int export_pcm(const struct shell *sh)
{
	struct mic_pcm_store_info info;
	uint8_t bytes[MIC_PCM_STORE_MAX_CHUNK];
	char hex[2U * MIC_PCM_STORE_MAX_CHUNK + 1U];
	static const char digits[] = "0123456789abcdef";
	size_t offset = 0;
	int err = mic_pcm_store_seal(&info);
	if (err == 0) {
		shell_print(sh, "MIC_PCM_BEGIN samples=%u bytes=%u rate=16000 channels=1 bits=16 skipped=16000 crc32=%08x",
			info.samples, info.total_bytes, info.crc32);
		while (offset < info.total_bytes) {
			int count = mic_pcm_store_take(offset, bytes, sizeof(bytes));
			if (count <= 0) { err = count < 0 ? count : -EIO; break; }
			for (int i = 0; i < count; ++i) {
				hex[2 * i] = digits[bytes[i] >> 4];
				hex[2 * i + 1] = digits[bytes[i] & 15U];
			}
			hex[2 * count] = '\0';
			for (size_t i = 0; i < sizeof(bytes); ++i) { ((volatile uint8_t *)bytes)[i] = 0; }
			shell_print(sh, "MIC_PCM_DATA offset=%u hex=%s", (unsigned int)offset, hex);
			/* Volatile erasure is deliberate: avoid leaving staging copies. */
			for (size_t i = 0; i < sizeof(bytes); ++i) { ((volatile uint8_t *)bytes)[i] = 0; }
			for (size_t i = 0; i < sizeof(hex); ++i) { ((volatile char *)hex)[i] = 0; }
			offset += (size_t)count;
		}
	}
	for (size_t i = 0; i < sizeof(bytes); ++i) { ((volatile uint8_t *)bytes)[i] = 0; }
	for (size_t i = 0; i < sizeof(hex); ++i) { ((volatile char *)hex)[i] = 0; }
	mic_pcm_store_scrub();
	mic_pcm_store_get_state(&info);
	if (!info.scrubbed && err == 0) { err = -EIO; }
	shell_print(sh, "MIC_PCM_END rc=%d bytes=%u scrubbed=%u", err,
		(unsigned int)offset, info.scrubbed);
	return err;
}

static void microphone_worker(void *a, void *b, void *c)
{
	/* Only this serialized worker owns the aggregate result. Keep the larger
	 * statistics object off its bounded stack; it contains no PCM samples.
	 */
	static struct mic_diagnostics_stats stats;
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	for (;;) {
		struct mic_diagnostics_state state;
		int err;
		(void)k_sem_take(&mic_request, K_FOREVER);
		memset(&stats, 0, sizeof(stats));
		const struct shell *sh = request_shell;
		enum request_mode mode = requested_mode;
		if (mode == REQUEST_BLE) {
			ble_audio_worker();
			/* BLE worker scrubbed before returning; it alone used this job.
			 * Do not scrub after busy is released or touch its terminal data.
			 */
			atomic_clear(&busy);
			continue;
		}
		bool capture = mode != REQUEST_POWER;
		bool record = mode == REQUEST_RECORD;
		mic_pcm_store_scrub();
		err = record ? mic_pcm_store_begin() : 0;
		if (err == 0) { err = pendant_audio_indicator(true); }
		if (err == 0) {
			if (capture) {
				err = mic_diagnostics_capture(MIC_DIAG_MAX_CAPTURE_MS, &stats, record);
			} else {
				err = mic_diagnostics_rail_check(MIC_DIAG_MAX_RAIL_CHECK_MS);
			}
		}
		mic_diagnostics_get_state(&state);
		if (state.fault_latched || state.powered) {
			/* Module attempted power-off. Reset immediately on uncertain stop;
			 * do not wait on USB output or allow another capture. */
			mic_pcm_store_scrub();
			sys_reboot(SYS_REBOOT_COLD);
		}
		int led_err = pendant_audio_indicator(false);
		if (err == 0 && led_err != 0) { err = led_err; }
		if (err != 0) { mic_pcm_store_scrub(); }
		if (!capture && err == 0) { atomic_set(&rail_checked, 1); }
		if (capture) {
			shell_print(sh, "MIC_CONFIG stage=%u mismatch=%u clkctrl=%u ratio=%u mclk=%u mode=%u gainl=%u gainr=%u clkpin=%u datapin=%u",
				stats.config_stage, stats.config_mismatch, stats.pdm_clkctrl,
				stats.pdm_ratio, stats.pdm_mclk, stats.pdm_mode,
				stats.pdm_gainl, stats.pdm_gainr, stats.pdm_clkpin, stats.pdm_datapin);
			/* Emit only after STOP, scrubbing, rail OFF and the fault-reset
			 * guard above. No USB output occurs in the sample-processing loop.
			 */
			for (uint32_t i = 0; i < MIC_DIAG_WINDOW_COUNT; ++i) {
				const struct mic_diagnostics_window *window = &stats.windows[i];
				shell_print(sh, "MIC_WINDOW index=%u samples=%u blocks=%u nonzero=%u saturated=%u min=%d max=%d signed_sum=%lld sum_abs=%llu sum_square=%llu frame_ac_sum=%llu frame_ac_max=%llu",
					i, window->samples, window->blocks, window->nonzero,
					window->saturated, window->minimum, window->maximum,
					(long long)window->signed_sum,
					(unsigned long long)window->sum_abs,
					(unsigned long long)window->sum_square,
					(unsigned long long)window->frame_ac_sum,
					(unsigned long long)window->frame_ac_max);
			}
			if (record) {
				if (err == 0 && state.clock_stopped && state.buffers_scrubbed &&
				    !state.powered && !state.fault_latched) {
					err = export_pcm(sh);
				} else {
					mic_pcm_store_scrub();
					if (err == 0) { err = -EIO; }
					shell_print(sh, "MIC_PCM_END rc=%d bytes=0 scrubbed=1", err);
				}
			}
			shell_print(sh, "MIC_RESULT rc=%d samples=%u blocks=%u nonzero=%u saturated=%u min=%d max=%d mean_abs=%u mean_square=%u elapsed_ms=%u capture_error=%d cleanup_error=%d clock_stopped=%u buffers_scrubbed=%u power_off=%u",
				err, stats.samples, stats.blocks, stats.nonzero, stats.saturated,
				stats.minimum, stats.maximum, stats.mean_abs, stats.mean_square,
				stats.elapsed_ms, stats.capture_error, stats.cleanup_error,
				stats.clock_stopped, stats.buffers_scrubbed, stats.power_off);
		} else {
			shell_print(sh, "MIC_POWER_RESULT rc=%d power_off=%u clock_stopped=%u; no audio captured; supply voltage not measured",
				err, !state.powered, state.clock_stopped);
		}
		mic_pcm_store_scrub();
		atomic_clear(&busy);
	}
}
K_THREAD_DEFINE(microphone_thread, 2048, microphone_worker, NULL, NULL, NULL, 5, 0, 0);

static int command_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	struct mic_diagnostics_state state;
	struct mic_pcm_store_info pcm;
	mic_diagnostics_get_state(&state);
	mic_pcm_store_get_state(&pcm);
	shell_print(sh, "MIC_STATUS init=%d busy=%u power=%u fault=%u clock_stopped=%u buffers_scrubbed=%u rail_checked=%u armed=%u pcm_bytes=%u pcm_scrubbed=%u; USB/BLE bounded audio; buffers_scrubbed=DMA only",
		(int)atomic_get(&initialized), mic_commands_busy(), state.powered,
		state.fault_latched, state.clock_stopped, state.buffers_scrubbed,
		atomic_get(&rail_checked) != 0, (unsigned int)atomic_get(&armed),
		pcm.bytes_remaining, pcm.scrubbed);
	return 0;
}

static int queue_test(const struct shell *sh, size_t argc, char **argv, enum request_mode mode)
{
	bool capture = mode != REQUEST_POWER;
	bool record = mode == REQUEST_RECORD;
	if (argc != 2 || strcmp(argv[1], "confirm") != 0) {
		shell_error(sh, "Requires 'confirm'; power check is 5 seconds, capture is 2 seconds maximum.");
		return -EINVAL;
	}
	if (atomic_get(&initialized) != 0 || pendant_recovery_is_pending()) { return -EBUSY; }
	if (!atomic_cas(&busy, 0, 1)) { return -EBUSY; }
	if (pendant_recovery_is_pending() || pendant_ble_pairing_busy()) {
		atomic_clear(&busy); return -EBUSY;
	}
	if (capture && !atomic_cas(&armed, record ? 2 : 1, 0)) {
		atomic_clear(&busy);
		shell_error(sh, "Wrong or missing one-shot permission. Complete power check, then arm factory-confirm for statistics or playback-confirm for local PCM export.");
		return -EACCES;
	}
	if (!capture) { atomic_clear(&armed); atomic_clear(&rail_checked); }
	request_shell = sh;
	requested_mode = mode;
	if (record) {
		shell_print(sh, "MIC_REQUEST accepted mode=record; red indicator; automatic stop; local PCM export after power off");
	} else {
		shell_print(sh, "MIC_REQUEST accepted mode=%s; red indicator; automatic stop; no saved recording",
			capture ? "capture" : "power-only");
	}
	k_sem_give(&mic_request);
	return 0;
}

static int command_power(const struct shell *sh, size_t argc, char **argv)
{ return queue_test(sh, argc, argv, REQUEST_POWER); }
static int command_capture(const struct shell *sh, size_t argc, char **argv)
{ return queue_test(sh, argc, argv, REQUEST_STATS); }
static int command_record(const struct shell *sh, size_t argc, char **argv)
{ return queue_test(sh, argc, argv, REQUEST_RECORD); }

static int command_arm(const struct shell *sh, size_t argc, char **argv)
{
	/* This records the actual evidence basis, not a fictitious meter reading.
	 * Only for original microphones on unchanged factory connectors/power path.
	 * The power-check flag means a software ON/OFF cycle, not measured voltage.
	 */
	if (argc != 2) { return -EINVAL; }
	bool playback = strcmp(argv[1], "playback-confirm") == 0;
	if (!playback && strcmp(argv[1], "factory-confirm") != 0) { return -EINVAL; }
	if (!atomic_cas(&busy, 0, 1)) { return -EBUSY; }
	if (pendant_recovery_is_pending() || pendant_ble_pairing_busy() ||
	    atomic_get(&initialized) != 0 || !atomic_get(&rail_checked)) {
		atomic_clear(&busy); return -EBUSY;
	}
	atomic_set(&armed, playback ? 2 : 1);
	atomic_clear(&busy);
	if (playback) {
		shell_print(sh, "MIC_ARMED once; basis=factory-wiring; supply_unmeasured=1; local_PCM_export=1; no capture started");
	} else {
		shell_print(sh, "MIC_ARMED once; basis=factory-wiring; supply_unmeasured=1; no capture started");
	}
	return 0;
}

static int command_disarm(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	atomic_clear(&armed);
	shell_print(sh, "MIC_DISARMED; an already active bounded test still stops automatically");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(micdiag_commands,
	SHELL_CMD_ARG(status, NULL, "Read microphone diagnostic state.", command_status, 1, 0),
	SHELL_CMD_ARG(power, NULL, "With confirm: rail only for 5 seconds; no audio clocks.", command_power, 1, 1),
	SHELL_CMD_ARG(arm, NULL, "factory-confirm: statistics; playback-confirm: local PCM. Original wiring, supply unmeasured; once.", command_arm, 1, 1),
	SHELL_CMD_ARG(capture, NULL, "With confirm: at most 2 seconds RAM-only signal statistics.", command_capture, 1, 1),
	SHELL_CMD_ARG(record, NULL, "With confirm and playback arm: 2 seconds max; discard 1 second; local USB PCM.", command_record, 1, 1),
	SHELL_CMD_ARG(disarm, NULL, "Cancel permission for the next capture.", command_disarm, 1, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(micdiag, &micdiag_commands, "Explicit USB microphone diagnostics, default off.", NULL);
