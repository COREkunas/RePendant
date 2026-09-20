/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OPENPENDANT_RECORDING_RUNTIME_H
#define OPENPENDANT_RECORDING_RUNTIME_H
/* Local USB `recorder diagnostics` is idle/cached-only: no NAND/settings write,
 * audio, recovery or reset. Reset-cause flags are not cleared; worker stack
 * watermarks are from the current boot only. It does not resume provisioning. */
/* `recorder controlprobe confirm` is an explicit one-shot fixed-bank read-only
 * diagnostic on the codec worker. The original30s data deadline is never
 * renewed. Its supervisor latches expiry rather than rebooting away evidence;
 * this cannot preempt a hung HAL/PSA call. Unknown STOP/readiness retains both
 * permanent DMA memory and the external reservation. No mount/adoption/retry.
 * `controlstatus` and `controlpage index` read joined cached metadata only. */
#include <stdint.h>
#include "device_telemetry.h"
/* `recorder phyprobe confirm` is a mutually exclusive one-shot diagnostic with
 * controlprobe: original PHY/HAL, fixed67652/67653 corrected reads,15s sampling
 * and original20s deadline. Transfer filter blocks all mutation opcodes even
 * WRDI. Expiry only latches refusal; no failure reboot/retry/forced release.
 * `phystatus` exports joined metadata only; output_scrubbed excludes DMA.
 * Current phase2/revision2 config is observed, never changed/adopted. */
#include "durable_ble_codec.h"
#ifdef OPENPENDANT_LONG_CONTROL
#include "recording_long_runtime.h"
#endif
/* `recorder preimage INDEX TOKEN` captures one strictly ascending page of the
 * fixed34-block phase2 volume. Mutually exclusive with both prior probes for
 * the whole boot; union DMA context never reused between peers. Original2s
 * physical deadline, twin raw reads with B0 off/restore only, then checked
 * cached private export by the codec worker. No array program/erase/settings
 * write/retry/reset. Exported count is local framing, not durable host proof.
 * `preimagestatus` reads joined metadata only. Host must persist one-use intent
 * before first command and must not repeat after a reset or ambiguous failure. */
#include <zephyr/usb/usb_device.h>
/* Target integration. Init prepares idle workers only; no automatic mount,
 * NAND write, enrollment, or capture. The portable profile admits explicit
 * recording with a fresh battery lease. Sync/provision/diagnostic writes still
 * require USB throughout. No boot/button auto-resume of an old recording. */
int recording_runtime_init(void);
void recording_runtime_usb_status(enum usb_dc_status_code,const uint8_t*);
int recording_runtime_microphone_power(void);
int recording_runtime_faulted(void);
/* System-ON idle scheduling only; no power-off, resource release or audio.
 * Returns 1 only after cached idle checks pass. Work/guards resume supervision. */
int recording_runtime_standby(int requested);
/* USB-idle exclusive settings transaction; no capture, mount or NAND I/O. */
int recording_runtime_preferences_claim(void);
int recording_runtime_preferences_ready(void);
void recording_runtime_preferences_release(void);
/* Security actor first publishes pairing-busy, then claims idle/power gates.
 * Does not hold controller metadata lock over Bluetooth callbacks or NVS I/O. */
int recording_runtime_pairing_claim(void);
int recording_runtime_pairing_ready(void);
void recording_runtime_pairing_release(void);
/* Bounded cached-only snapshot. Never mounts, initializes, reads NAND, starts
 * audio, changes settings, or waits for a lock. Safe from BLE receive context. */
void recording_runtime_telemetry(struct recorder_telemetry *);
#ifdef OPENPENDANT_PORTABLE_RECORDING
void recording_runtime_battery_telemetry(struct battery_telemetry *);
#endif
/* Nonblocking broker hooks. Submit copies the parsed request and its original
 * deadline. Retire only latches a joined-cleanup request; neither hook performs
 * NAND I/O. An admitted job may finish locally after BLE disconnect; USB power
 * policy and its original deadline remain mandatory. */
int recording_runtime_sync_ready(void *);
int recording_runtime_full_storage(void); /* validated configuration cached at init */
int recording_runtime_sync_submit(void *,uint32_t,const struct db_request *,uint64_t);
int recording_runtime_sync_retire(void *,uint32_t);
#endif
