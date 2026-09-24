/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OPENPENDANT_MIC_COMMANDS_H_
#define OPENPENDANT_MIC_COMMANDS_H_
#include <stdbool.h>
struct mic_pcm_store_info;
int mic_commands_init(void);
bool mic_commands_busy(void);
/* Reserve the same exclusion for a short, non-audio local hardware diagnostic.
 * Success owns busy=2, does not enqueue work or touch audio hardware. Only the
 * successful caller may release it. Pairing/recovery/RGB already honor busy.
 */
int mic_commands_reserve_external(void);
void mic_commands_release_external(void);
/* BLE session reserves the same worker/busy exclusion as USB diagnostics.
 * queue only enqueues; the caller must initialize its session under its lock.
 * capture is private to that worker and returns only after safe cleanup/seal.
 */
int mic_commands_queue_ble(void);
int mic_commands_ble_capture(struct mic_pcm_store_info *info);
/* Implemented by the main application; indicator cannot be overridden by BLE
 * while a diagnostic is active. No microphone starts if the indicator fails. */
int pendant_audio_indicator(bool enabled);
int pendant_recording_prepare(bool enabled);
void pendant_recording_confirm(bool started);
bool pendant_recovery_is_pending(void);
#endif
