/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OPENPENDANT_RECORDING_CAPTURE_H
#define OPENPENDANT_RECORDING_CAPTURE_H
#include <stdint.h>

/* Actual Zephyr DMIC acquisition adapter; initially UNLINKED. No shell/startup
 * recording hook, heap, storage, PCM export or microphone lease acquisition.
 * Root must reserve mic_commands external ownership for the ENTIRE recorder
 * session (including storage drain), then expose that exact epoch in owner_valid.
 * All previous mic diagnostics and other capture paths must remain excluded.
 * mic_diagnostics_init must already have configured confirmed P1.01 rail OFF.
 * This module owns its own permanent six640-byte DMA slab and priority6 thread.
 * Combined target RAM/stack, independent recorder progress supervisor and driver
 * IRQ latency remain integration gates; local host tests cannot prove them.
 */
#define RECORDING_CAPTURE_STACK_BYTES 4096U
#define RECORDING_CAPTURE_PRIORITY 6
#define RECORDING_CAPTURE_WARMUP_FRAMES 50U
#define RECORDING_CAPTURE_READ_MS 25U
#define RECORDING_CAPTURE_STALL_MS 100U
#define RECORDING_CAPTURE_STOP_MS 150U

enum recording_capture_state { RC_UNINITIALIZED=0,RC_IDLE=1,RC_STARTING=2,
 RC_WARMING=3,RC_RUNNING=4,RC_STOPPING=5,RC_JOINED=6,RC_FAULT=7 };
struct recording_capture_hooks {
 void *user;
 /* Nonblocking, exact epoch,1 iff composite recorder lease still held. Does
  * not reacquire/release a lease. Invoked before hardware start and each frame. */
 int (*owner_valid)(void*,uint32_t epoch);
 /* Nonblocking semaphore/event notification only; never call rw_step inline. */
 void (*wake_worker)(void*);
 /* Mandatory independently reviewed normal cold-reset path. MUST NOT RETURN,
  * log PCM, acquire locks or wait for this acquisition/codec/storage worker.
  * A return falls back to sys_reboot(COLD) then a nonreturning spin. */
 void (*fatal)(void*,int reason);
};
struct recording_capture_status {
 uint32_t state,epoch,warmup_frames,submitted_frames,returned_frames;
 uint32_t power_on,clock_stopped,buffers_scrubbed,joined,fault;
 int32_t capture_error,submit_rc;
 uint64_t first_sample,next_sample;
};

/* One init, readiness/scrub only: no DMIC configure/START or rail ON. */
int recording_capture_init(const struct recording_capture_hooks*);
/* rw_hooks adapters: caller must hold the independently supervised worker call
 * deadline. start does50ms settling but does NOT wait for1s warmup. Epochs are
 * nonzero, monotonically increasing, <=RW_EPOCH_MAX and never reused. Warmup
 * frames are scrubbed/discarded; first_sample is the FIRST KEPT sample, not the
 * physical startup sample. Every admitted DMA frame is copied into rw_submit;
 * the driver block is wiped/freed before the worker notification returns.
 */
int recording_capture_start(uint32_t epoch,uint64_t first_sample,uint64_t deadline_ms);
/* Stop producer, await asynchronous STOP, drain returned blocks, zero-rate
 * uninitialize driver, scrub all free slab blocks, rail OFF and join. No lease
 * release or queue wipe.0 proves capture ownership resolved, NOT recording
 * success: inspect capture_error/fault and recorder result separately.
 * No new stop deadline extends the caller's original deadline. Internal error
 * cleanup is separately bounded150ms; a caller can only shorten it. Failed or
 * late STOP/disable/rail-off invokes fatal, retaining unreturned DMA memory.
 */
int recording_capture_stop_and_join(uint32_t epoch,uint64_t deadline_ms);
int recording_capture_get_status(struct recording_capture_status*);
#endif
