/* Generated-public-PCM Opus1.6.1 fixed-point benchmark; explicit invocation only.
 * Opus remains unmodified; preserve third_party/opus-1.6.1/COPYING in releases.
 * Caller-supplied encoder state via official get_size/init, never create/free.
 */
#include "opus_benchmark.h"
#include "opus.h"
#include <string.h>
#include <zephyr/sys/atomic.h>

#if defined(__GNUC__)
#define BENCH_RETAIN __attribute__((used,retain))
#else
#define BENCH_RETAIN
#endif
static const uint32_t opus_benchmark_bounds[12] BENCH_RETAIN = {
 16000U,1U,320U,30U,3U,90U,24000U,32768U,1275U,1000U,30000U,65536U
};
static const uint32_t opus_benchmark_complexities[3] BENCH_RETAIN = {1U,3U,5U};
static const uint32_t opus_benchmark_application_id BENCH_RETAIN =
 OPUS_APPLICATION_RESTRICTED_LOWDELAY;
static const uint32_t opus_benchmark_counter_bounds[2] BENCH_RETAIN = {
 OPUS_BENCHMARK_COUNTER_MIN_HZ, OPUS_BENCHMARK_COUNTER_MAX_HZ
};
static union { uint64_t align; unsigned char bytes[OPUS_BENCHMARK_STATE_CAPACITY]; } benchmark_encoder;
static opus_int16 benchmark_pcm[OPUS_BENCHMARK_FRAME_SAMPLES];
static unsigned char benchmark_packet[1275];
static struct opus_benchmark_result benchmark_result;
static struct opus_benchmark_platform benchmark_platform;
static atomic_t benchmark_active,benchmark_attempted;
static uint32_t benchmark_start_ms,benchmark_last_elapsed;
_Static_assert(OPUS_BENCHMARK_TOTAL_FRAMES<=100U,"Synthetic frame allowance too large");
_Static_assert(OPUS_BENCHMARK_TOTAL_FRAMES==OPUS_BENCHMARK_FRAMES_PER_PROFILE*OPUS_BENCHMARK_PROFILE_COUNT,
               "Synthetic allowance differs");
_Static_assert(sizeof(opus_int16)==2U,"PCM must be signed16-bit");
_Static_assert(OPUS_APPLICATION_RESTRICTED_LOWDELAY==2051,"Pinned CELT application differs");

static void wipe_buffers(void)
{
 volatile unsigned char *state=benchmark_encoder.bytes,*packet=benchmark_packet;
 volatile opus_int16 *samples=benchmark_pcm;
 for(size_t i=0;i<sizeof(benchmark_encoder.bytes);++i)state[i]=0;
 for(size_t i=0;i<sizeof(benchmark_packet);++i)packet[i]=0;
 for(size_t i=0;i<OPUS_BENCHMARK_FRAME_SAMPLES;++i)samples[i]=0;
 benchmark_result.buffers_wiped=1;
}
static int sample_wall(uint32_t *value)
{
 uint32_t now=benchmark_platform.wall_ms(benchmark_platform.user);
 uint32_t elapsed=now-benchmark_start_ms;
 if(elapsed>=OPUS_BENCHMARK_TOTAL_LIMIT_MS)return OPUS_BENCHMARK_TIMEOUT;
 if(elapsed<benchmark_last_elapsed)return OPUS_BENCHMARK_CLOCK;
 benchmark_last_elapsed=elapsed;benchmark_result.elapsed_ms=elapsed;
 *value=now;return 0;
}
static int guard(void)
{
 uint32_t now;
 int rc=sample_wall(&now);if(rc)return rc;
 if(benchmark_platform.cancelled(benchmark_platform.user))return OPUS_BENCHMARK_CANCELLED;
 /* Do not accept a callback that crossed the deadline or moved time backwards. */
 return sample_wall(&now);
}
static void generate_frame(uint32_t frame)
{
 /* Identical public input for each complexity: five silence frames, ten
  * 200Hz triangle frames, fifteen amplitude-varied triangle frames.
  * No rand, float, external PCM pointer or hardware input. Range within int16. */
 for(uint32_t i=0;i<OPUS_BENCHMARK_FRAME_SAMPLES;++i) {
  uint32_t sample=frame*OPUS_BENCHMARK_FRAME_SAMPLES+i;
  int32_t triangle=(int32_t)(sample%80U);
  if(triangle>40)triangle=80-triangle;
  int32_t amplitude=frame<15U?350:50+(int32_t)(frame%23U)*15;
  benchmark_pcm[i]=frame<5U?0:(opus_int16)((triangle-20)*amplitude);
 }
}
static int controls(OpusEncoder *encoder,uint32_t complexity)
{
 int rc;
#define SET_CONTROL(expression) do { \
 rc=guard();if(rc)return rc; \
 benchmark_result.opus_rc=(expression); \
 rc=guard();if(rc)return rc; \
 if(benchmark_result.opus_rc!=OPUS_OK)return OPUS_BENCHMARK_CODEC; \
} while(0)
 SET_CONTROL(opus_encoder_ctl(encoder,OPUS_SET_BITRATE(24000)));
 SET_CONTROL(opus_encoder_ctl(encoder,OPUS_SET_COMPLEXITY((int)complexity)));
 SET_CONTROL(opus_encoder_ctl(encoder,OPUS_SET_VBR(0)));
 SET_CONTROL(opus_encoder_ctl(encoder,OPUS_SET_DTX(0)));
 SET_CONTROL(opus_encoder_ctl(encoder,OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE)));
 SET_CONTROL(opus_encoder_ctl(encoder,OPUS_SET_INBAND_FEC(0)));
 SET_CONTROL(opus_encoder_ctl(encoder,OPUS_SET_PACKET_LOSS_PERC(0)));
 opus_int32 lookahead=-1;
 SET_CONTROL(opus_encoder_ctl(encoder,OPUS_GET_LOOKAHEAD(&lookahead)));
 /* Pinned1.6.1 restricted-lowdelay at16kHz is CELT-only with2.5ms delay.
  * Query the real encoder; do not turn a different profile into valid metadata. */
 if(lookahead!=40)return OPUS_BENCHMARK_CODEC;
 benchmark_result.profiles[benchmark_result.profile_index].lookahead_samples=(uint32_t)lookahead;
#undef SET_CONTROL
 return 0;
}
static int run_profiles(void)
{
 int rc=guard();if(rc)return rc;
 benchmark_result.stage=OPUS_BENCHMARK_LIBRARY_CHECK;
 const char *version=opus_get_version_string();
 if(version==NULL || strcmp(version,"libopus 1.6.1-fixed")!=0)return OPUS_BENCHMARK_VERSION;
 rc=guard();if(rc)return rc;
 int required=opus_encoder_get_size(1);
 benchmark_result.required_state_bytes=required;
 rc=guard();if(rc)return rc;
 if(required<=0 || (uint32_t)required>sizeof(benchmark_encoder.bytes))return OPUS_BENCHMARK_STATE_SIZE;
 OpusEncoder *encoder=(OpusEncoder*)(void*)benchmark_encoder.bytes;
 for(uint32_t p=0;p<OPUS_BENCHMARK_PROFILE_COUNT;++p) {
  benchmark_result.profile_index=p;benchmark_result.frame_index=0;
  struct opus_benchmark_profile *profile=&benchmark_result.profiles[p];
  profile->complexity=opus_benchmark_complexities[p];
  benchmark_result.stage=OPUS_BENCHMARK_INITIALIZE;
  rc=guard();if(rc)return rc;
  benchmark_result.opus_rc=opus_encoder_init(encoder,16000,1,(int)opus_benchmark_application_id);
  rc=guard();if(rc)return rc;
  if(benchmark_result.opus_rc!=OPUS_OK)return OPUS_BENCHMARK_CODEC;
  benchmark_result.stage=OPUS_BENCHMARK_CONTROL;
  rc=controls(encoder,profile->complexity);if(rc)return rc;
  for(uint32_t frame=0;frame<OPUS_BENCHMARK_FRAMES_PER_PROFILE;++frame) {
   benchmark_result.frame_index=frame;benchmark_result.stage=OPUS_BENCHMARK_ENCODE;
   generate_frame(frame);rc=guard();if(rc)return rc;
   uint32_t ms0;
   rc=sample_wall(&ms0);if(rc)return rc;
   uint32_t cycles0=benchmark_platform.cycles(benchmark_platform.user);
   /* Recheck cancellation/deadline after clock callback, before codec entry. */
   rc=guard();if(rc)return rc;
   ++profile->frames_attempted;++benchmark_result.frames_attempted;
   int bytes=opus_encode(encoder,benchmark_pcm,OPUS_BENCHMARK_FRAME_SAMPLES,
                         benchmark_packet,(opus_int32)sizeof(benchmark_packet));
   benchmark_result.opus_rc=bytes<0?bytes:0;
   uint32_t cycles1=benchmark_platform.cycles(benchmark_platform.user);
   uint32_t ms1;
   rc=sample_wall(&ms1);if(rc)return rc;
   uint32_t elapsed=ms1-ms0,cycles=cycles1-cycles0;
   /* Even the final codec call cannot turn a late result into success. */
   rc=guard();if(rc)return rc;
   if(elapsed>=OPUS_BENCHMARK_FRAME_LIMIT_MS)return OPUS_BENCHMARK_TIMEOUT;
   /* The cycle interval is enclosed by wall samples. Permit one millisecond
    * quantization uncertainty, but reject a forward jump/inconsistent rate. */
   uint64_t maximum_cycles=((uint64_t)(elapsed+1U)*benchmark_platform.counter_hz+999U)/1000U;
   if(cycles==0U || cycles>=benchmark_platform.counter_hz || cycles>maximum_cycles)
       return OPUS_BENCHMARK_CLOCK;
   if(bytes<0)return OPUS_BENCHMARK_CODEC;
   if(bytes!=60 || bytes>(int)sizeof(benchmark_packet))return OPUS_BENCHMARK_PACKET;
   benchmark_result.stage=OPUS_BENCHMARK_VALIDATE;
   int samples=opus_packet_get_nb_samples(benchmark_packet,bytes,16000);
   int channels=opus_packet_get_nb_channels(benchmark_packet);
   rc=guard();if(rc)return rc;
   /* RFC6716 TOC configurations16..31 (top bit set) are CELT-only. */
   if(samples!=320 || channels!=1 || (benchmark_packet[0]&0x80U)==0U)
       return OPUS_BENCHMARK_PACKET;
   if(profile->frames_completed==0U || cycles<profile->min_encode_cycles)profile->min_encode_cycles=cycles;
   if(cycles>profile->max_encode_cycles)profile->max_encode_cycles=cycles;
   if(elapsed>profile->max_encode_ms)profile->max_encode_ms=elapsed;
   profile->total_encode_cycles+=cycles;++profile->frames_completed;++benchmark_result.frames_completed;
  }
  ++benchmark_result.profiles_completed;
  wipe_buffers();
 }
 benchmark_result.stage=OPUS_BENCHMARK_DONE;
 return guard();
}
int opus_benchmark_run(const struct opus_benchmark_platform *platform,struct opus_benchmark_result *result)
{
 if(platform==NULL || result==NULL || platform->wall_ms==NULL || platform->cycles==NULL ||
    platform->cancelled==NULL || platform->counter_hz<OPUS_BENCHMARK_COUNTER_MIN_HZ ||
    platform->counter_hz>OPUS_BENCHMARK_COUNTER_MAX_HZ ||
    platform->worker_stack_bytes<OPUS_BENCHMARK_WORKER_STACK_MIN)return OPUS_BENCHMARK_ARGUMENT;
 if(!atomic_cas(&benchmark_active,0,1))return OPUS_BENCHMARK_BUSY;
 if(!atomic_cas(&benchmark_attempted,0,1)) {
  atomic_clear(&benchmark_active);return OPUS_BENCHMARK_ALREADY;
 }
 benchmark_platform=*platform;
 memset(&benchmark_result,0,sizeof(benchmark_result));
 benchmark_result.counter_hz=platform->counter_hz;
 benchmark_start_ms=benchmark_platform.wall_ms(benchmark_platform.user);
 benchmark_last_elapsed=0;
 benchmark_result.rc=run_profiles();
 wipe_buffers();
 /* Only bounded metadata leaves this module; never PCM/state/packet bytes. */
 *result=benchmark_result;
 memset(&benchmark_platform,0,sizeof(benchmark_platform));
 atomic_clear(&benchmark_active);
 return result->rc;
}
