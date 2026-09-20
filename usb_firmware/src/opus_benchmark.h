#ifndef OPENPENDANT_OPUS_BENCHMARK_H
#define OPENPENDANT_OPUS_BENCHMARK_H
/* Explicit synthetic-only benchmark. No microphone, DMA, NAND, BLE, USB,
 * startup, shell registration, audio input or encoded-output interface.
 * Uses pinned Opus1.6.1 RESTRICTED_LOWDELAY (2051), CELT-only:16kHz mono,
 * 20ms frames,24kbps CBR, queried40-sample lookahead, complexities1/3/5.
 * This does not change the production recording/segment codec profile.
 *
 * CALLER MUST use an exclusive, guarded worker stack >=64KiB, never shell/BLE
 * callback stack. This is an initial admission floor, NOT a proved peak bound.
 * Review/instrument full codec stack and watchdog behavior before on-target use.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define OPUS_BENCHMARK_WORKER_STACK_MIN 65536U
#define OPUS_BENCHMARK_STATE_CAPACITY 32768U
#define OPUS_BENCHMARK_FRAME_SAMPLES 320U
#define OPUS_BENCHMARK_FRAMES_PER_PROFILE 30U
#define OPUS_BENCHMARK_PROFILE_COUNT 3U
#define OPUS_BENCHMARK_TOTAL_FRAMES 90U
#define OPUS_BENCHMARK_FRAME_LIMIT_MS 1000U
#define OPUS_BENCHMARK_TOTAL_LIMIT_MS 30000U
#define OPUS_BENCHMARK_COUNTER_MIN_HZ 32768U
#define OPUS_BENCHMARK_COUNTER_MAX_HZ 200000000U
enum opus_benchmark_rc {
 OPUS_BENCHMARK_OK=0,OPUS_BENCHMARK_ARGUMENT=-1,OPUS_BENCHMARK_ALREADY=-2,
 OPUS_BENCHMARK_BUSY=-3,OPUS_BENCHMARK_CANCELLED=-4,OPUS_BENCHMARK_STATE_SIZE=-5,
 OPUS_BENCHMARK_VERSION=-6,OPUS_BENCHMARK_CODEC=-7,OPUS_BENCHMARK_PACKET=-8,
 OPUS_BENCHMARK_CLOCK=-9,OPUS_BENCHMARK_TIMEOUT=-10
};
enum opus_benchmark_stage {
 OPUS_BENCHMARK_NOT_RUN=0,OPUS_BENCHMARK_LIBRARY_CHECK=1,
 OPUS_BENCHMARK_INITIALIZE=2,OPUS_BENCHMARK_CONTROL=3,
 OPUS_BENCHMARK_ENCODE=4,OPUS_BENCHMARK_VALIDATE=5,OPUS_BENCHMARK_DONE=6
};
struct opus_benchmark_platform {
 /* Synchronous nonblocking callbacks, no reconfiguration/logging/audio.
  * wall_ms is monotonic modulo32 milliseconds. cycles is a free-running
  * fixed-frequency modulo32 counter, not changed/stopped during any encode.
  * Only a verified core counter may be described as CPU cycles. Counts include
  * preemption/interrupts, not exclusively codec execution. No clock setup here.
  */
 uint32_t (*wall_ms)(void*);
 uint32_t (*cycles)(void*);
 bool (*cancelled)(void*);
 void *user;
 uint32_t counter_hz; /*32768Hz..200MHz; timed call <1000ms => <=one wrap.
                       * RTC ticks are elapsed counter ticks, NOT CPU cycles.
                       * Zero-tick measurements are rejected, never guessed. */
 uint32_t worker_stack_bytes; /* Caller assertion, not measured by this module. */
};
struct opus_benchmark_profile {
 uint32_t complexity,frames_attempted,frames_completed,lookahead_samples;
 uint32_t min_encode_cycles,max_encode_cycles,max_encode_ms;
 uint64_t total_encode_cycles;
};
struct opus_benchmark_result {
 int32_t rc,opus_rc,required_state_bytes;
 uint32_t stage,profile_index,frame_index,counter_hz,elapsed_ms;
 uint32_t frames_attempted,frames_completed,profiles_completed,buffers_wiped;
 struct opus_benchmark_profile profiles[OPUS_BENCHMARK_PROFILE_COUNT];
};
/* One admitted run per boot, even failure/cancellation; no reset API.
 * Refused calls do not overwrite result. Valid admitted calls always return
 * metadata after wiping private state/PCM/packet scratch. A codec fatal
 * assertion/reset cannot return; integration must provide a reviewed policy.
 * The module does not itself reserve mic/NAND/recovery or create a worker.
 * Caller must hold that exclusive reservation throughout the call.
 *
 * Deadlines/cancel are checked between and after calls, not inside opus_encode:
 * a stuck codec cannot be preempted here. Reviewed stack protection plus an
 * outside timeout/fatal policy are prerequisites. The command adapter uses a
 * higher-priority scheduler-based supervisor, NOT a hardware watchdog; IRQ
 * masking, kernel starvation or fatal kernel faults can defeat that deadline.
 */
int opus_benchmark_run(const struct opus_benchmark_platform*,struct opus_benchmark_result*);
#endif
