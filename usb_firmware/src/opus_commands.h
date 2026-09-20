#ifndef OPENPENDANT_OPUS_COMMANDS_H
#define OPENPENDANT_OPUS_COMMANDS_H

/* Local UART-shell, one public synthetic Opus attempt per boot. Init creates
 * only an idle supervisor; a 65536-byte priority-14 worker waits on a semaphore.
 * Priority-13 shell replies preempt the worker; the supervisor stays priority4.
 * No PCM input, packet export, mic/NAND/BLE/key operation or automatic run.
 * Caller integrates verified fixed Opus, fatal policy and stack guards first.
 *
 * Fixed decimal metadata protocol (shell_print has no delivery ACK):
 * OPUS_ACCEPTED public_only=1 timeout_ms=30000 counter_hz=32768
 * OPUS_REFUSED rc=<signed errno>
 * OPUS_STATUS state=<0..5> error=<signed errno> result_valid=<0|1>
 *   core_rc=<signed> opus_rc=<signed> stage=<0..6> frames_attempted=<0..90>
 *   frames_completed=<0..90> profiles_completed=<0..3> buffers_wiped=<0|1>
 *   elapsed_ms=<u32> stack_unused=<u32> stack_valid=<0|1>
 *   reservation_held=<0|1> timeout_ms=30000 counter_hz=32768 public_only=1
 * State: 0 IDLE, 1 RUNNING, 2 FINISHING, 3 DONE, 4 FAILED, 5 EXPIRED.
 * Nonterminal fields are zero except state/reservation; results are immutable.
 * `result` after DONE or FAILED with valid core metadata emits exactly 5 lines:
 * OPUS_RESULT core_rc=<signed> opus_rc=<signed> required_state_bytes=<signed>
 *   stage=<0..6> profile_index=<0..2> frame_index=<0..29>
 *   frames_attempted=<0..90> frames_completed=<0..90>
 *   profiles_completed=<0..3> buffers_wiped=1 core_elapsed_ms=<0..29999>
 *   counter_hz=32768 public_only=1
 * OPUS_PROFILE index=<0..2> complexity=<0|1|3|5> frames_attempted=<0..30>
 *   frames_completed=<0..30> lookahead_samples=<0..15999>
 *   min_encode_ticks=<u32> max_encode_ticks=<u32> max_encode_ms=<0..999>
 *   total_encode_ticks=<u64> (three lines in index order)
 * OPUS_RESULT_END profiles=3 public_only=1
 * The RTC counter measures elapsed ticks INCLUDING preemption, NOT CPU cycles.
 * No clock configuration; a different effective frequency fails compilation.
 * Core elapsed is its last accepted sample, not complete cleanup timing. Adapter
 * elapsed includes return, timeout cancellation and stack inspection.
 *
 * Priority-4 supervisor preempts worker at absolute 30 s, atomically marks
 * EXPIRED and cold-reboots with reservation held and no output. It must not
 * concurrently wipe a live codec. Late return/backward time also cold-reboots;
 * no late success/release. FINISHING joins the bounded nonblocking supervisor
 * before stack inspection/publication/release; sync relies on kernel progress.
 * Fatal/reset cannot prove codec RAM, registers or stack have been scrubbed.
 * Confirmed core wipe and valid metadata plus >=1024 unused worker bytes are
 * required for release even after ordinary failure. Unknown cleanup/stack keeps
 * exclusion until reboot. A 64 KiB floor is NOT proof of actual peak stack.
 * IRQ masking/kernel starvation can defeat this scheduler-based deadline.
 * No reset/retry API; initialize exactly once. No production recording enabled.
 */
int opus_commands_init(void);

#endif
