#ifndef OPENPENDANT_RECORDING_PIPELINE_H
#define OPENPENDANT_RECORDING_PIPELINE_H
#include <stddef.h>
#include <stdint.h>
#include "encrypted_segment_header.h"

/* Unlinked worker core, NOT a microphone driver or durable store. No startup,
 * shell, ISR, BLE, NAND, key generation, heap allocation or automatic capture.
 * Integration must reserve mic/storage/crypto, enforce consent+visible LED,
 * supply a bounded producer queue, and STOP acquisition before pause/end.
 * Run ONLY on a guarded worker >=64KiB; actual peak stack/real-time/power remain
 * unmeasured. Calls into Opus/store are synchronous and not preemptible here;
 * callbacks must enforce their own deadlines and watchdog/lifecycle policy.
 * Preserve third_party/opus-1.6.1/COPYING. Codec source remains unmodified.
 */
#define RP_SAMPLE_RATE 16000U
#define RP_FRAME_SAMPLES 320U
#define RP_SEGMENT_FRAMES 500U
#define RP_PACKET_BYTES 60U
#define RP_HEADER_BYTES 80U
#define RP_RECORD_BYTES (8U+RP_PACKET_BYTES)
#define RP_PLAINTEXT_CAPACITY (RP_HEADER_BYTES+(RP_SEGMENT_FRAMES+1U)*RP_RECORD_BYTES)
#define RP_ENCODER_CAPACITY 32768U
#define RP_WORKER_STACK_MIN 65536U

enum rp_mode { RP_MANUAL=0, RP_CONTINUOUS=1 };
enum rp_profile { RP_PROFILE_LEGACY_VOIP=1, RP_PROFILE_CELT_LOW_DELAY=2 };
enum rp_state { RP_UNINITIALIZED=0, RP_IDLE=1, RP_RECORDING=2, RP_PAUSED=3,
 RP_FINALIZING=4, RP_STOPPED=5, RP_FULL=6, RP_OVERFLOW=7, RP_LOW_POWER=8, RP_FAULT=9,
 RP_DRAINING=10 };
enum rp_reason { RP_REASON_NONE=0, RP_REASON_USER=1, RP_REASON_FULL=2,
 RP_REASON_OVERFLOW=3, RP_REASON_LOW_POWER=4, RP_REASON_CODEC=5,
 RP_REASON_STORE=6, RP_REASON_RECEIPT=7, RP_REASON_CONTINUITY=8,
 RP_REASON_EXTERNAL=9, RP_REASON_LIMIT=10 };
enum rp_rc { RP_OK=0, RP_PENDING=1, RP_ARGUMENT=-1, RP_BUSY=-2, RP_STATE=-3,
 RP_CODEC_ERROR=-4, RP_STORE_ERROR=-5, RP_RECEIPT_ERROR=-6,
 RP_CONTINUITY_ERROR=-7, RP_STORAGE_FULL=-8, RP_LIMIT=-9, RP_EXTERNAL_ERROR=-10 };
enum rp_io { RP_IO_OK=0, RP_IO_FULL=1, RP_IO_FAILED=2, RP_IO_UNCERTAIN=3, RP_IO_PENDING=4 };

struct rp_segment_receipt { uint32_t durable; uint8_t es_header[ES_HEADER_BYTES]; };
struct rp_completion {
 struct es_binding session; /* sequence=0, plaintext_bytes=0: identity only */
 uint32_t mode,reason,segments,gaps;
 uint64_t captured_samples,committed_samples,next_sample;
};
struct rp_final_receipt {
 struct rp_completion completion; uint32_t durable;
};
struct rp_sink {
 void *user;
 /* Requires an enrolled owner public key + independently verified recovery,
  * reserved data/commit capacity, and a NEVER reused recording identity. OK
  * acknowledges durable IN_PROGRESS catalog creation, not captured audio.
  * FULL guarantees no publication. FAILED/UNCERTAIN fence this core.
  */
 int (*reserve)(void*,const struct es_binding*);
 /* Exact immutable input lifetime ends on return. Must HPKE-seal ONCE using
  * the canonical ES header/info and trusted recipient, then durable DATA and
  * catalog COMMIT in that order. No overwrite. OK requires exact header receipt
  * +durable=1; FULL guarantees NOT committed and receipt remains all zero.
  * Any ambiguous publication must return UNCERTAIN; caller NEVER retries.
  * This callback is the trusted durability boundary, not proved by this core.
  */
 int (*seal_commit)(void*,const struct es_binding*,const uint8_t*,size_t,struct rp_segment_receipt*);
 /* Finalize the exact committed prefix and termination reason, never invent
  * missing audio. OK must echo EVERY completion field +durable=1. This binds
  * device/volume/generation/key/recording identity AND mode/reason/timeline.
  * A failed/uncertain finalization leaves prior segments valid but recording
  * completion UNKNOWN; this core fences instead of retrying or replacing it.
  */
 int (*finalize)(void*,const struct rp_completion*,struct rp_final_receipt*);
};
struct rp_status {
 uint32_t state,mode,reason,segments,segment_frames,gaps,lookahead;
 uint32_t session_reserved,manifest_finalized,commit_uncertain,buffers_wiped;
 int32_t error,codec_error,store_error;
 uint64_t captured_samples,committed_samples,next_sample,discarded_samples;
 uint32_t codec_profile; /* Chosen once at init; immutable across sessions. */
 uint32_t pending_segments; /* 0/1: NOT durable or included in segments. */
 uint64_t pending_samples; /* Distinct from committed/discarded samples. */
};

/* Optional deferred persistence. reserve/finalize remain synchronous and are
 * called before capture / after capture is stopped by the outer worker.
 * submit has the seal_commit signature but may return PENDING with an all-zero
 * receipt only after taking its OWN immutable copy or sealed ciphertext.
 * It MUST NOT retain the supplied plaintext/binding pointers. poll is a bounded
 * nonblocking ownership query for that exact binding: PENDING+zero receipt, or
 * OK+exact durable receipt. No second submission until the first is durable.
 * A separate storage worker advances persistence; poll never performs NAND I/O.
 * Concurrent storage BUSY maps to PENDING, not success. All callbacks are trusted
 * boundaries and must enforce deadlines externally; no thread is created here.
 *
 * Captured frames continue while one segment persists. If it remains unresolved
 * at the next seal, this core faults rather than overwriting/queueing another.
 * pause/end may return RP_PENDING and enter DRAINING; caller first stops capture,
 * then calls rp_poll under its original finite drain deadline. No finalized or
 * durable claim is made until the exact receipt and final catalog are verified.
 * Faults with pending ownership retain its identity/accounting and prohibit
 * further callbacks; integration must separately stop/join storage before reuse.
 */
struct rp_async_sink {
 struct rp_sink submit; /* .seal_commit means bounded immutable submission. */
 int (*poll)(void*,const struct es_binding*,struct rp_segment_receipt*);
};

/* Exactly one init per boot. No reset API. All entries serialized with atomic
 * admission, including callbacks; recursive/concurrent calls return BUSY.
 * Caller exclusively owns truthful input/output spans; callbacks may not mutate
 * inputs or retain pointers. No output struct may alias another API argument.
 */
int rp_init(const struct rp_sink*,enum rp_mode selected_mode,uint32_t worker_stack_bytes);
/* Explicit opt-in: legacy rp_init above remains profile1. Profile selection is
 * immutable for this boot; neither mode changes nor later sessions change it. */
int rp_init_profile(const struct rp_sink*,enum rp_mode,uint32_t worker_stack_bytes,enum rp_profile);
int rp_init_async_profile(const struct rp_async_sink*,enum rp_mode,uint32_t worker_stack_bytes,enum rp_profile);
int rp_poll(void); /* Deferred mode only; no capture, blocking I/O or implicit restart. */
int rp_get_status(struct rp_status*);
int rp_select_mode(enum rp_mode); /* Idle/completed only; never starts capture. */
int rp_start(const struct es_binding *new_session,uint64_t first_sample);
int rp_push_frame(uint64_t first_sample,const int16_t pcm[RP_FRAME_SAMPLES]);
int rp_pause(void); /* seals pending prefix; no manifest-complete claim */
int rp_resume(uint64_t next_sample); /* explicit only; gap allowed, never backward */
int rp_end(enum rp_reason); /* USER/FULL/OVERFLOW/LOW_POWER; seals prefix if possible */
int rp_external_fault(void); /* discard uncommitted RAM; no new store operations */

/* Internal plaintext segment format v1, all integers LITTLE-ENDIAN:
 * 0 magic[8]="OPNDOP1\0"; 8 version:u16=1; 10 header:u16=80;
 * 12 rate:u32=16000; 16 channels:u16=1; 18 frame:u16=320;
 * 20 bitrate:u32=24000; 24 complexity:u32=3; 28 flags:u32 (RESET=1,GAP=2);
 * 32 first_sample:u64; 40 next_sample:u64 (captured samples only);
 * 48 captured_frames:u32; 52 packet_count:u32; 56 preskip:u32 (16k samples);
 * 60 end_trim:u32; 64 payload_bytes:u32; 68 valid_samples:u32;
 * 72 codec_profile:u32:1=Opus1.6.1-fixed VOIP;2=RESTRICTED_LOWDELAY CELT-only;
 * 76 reserved:u32=0. Both profiles use the fixed controls above. The producer
 * queries and requires preskip104 for profile1 or40 for profile2. For legacy
 * parser compatibility ONLY, profile1 validation still accepts preskip0..320
 * and any otherwise valid Opus mode; framing cannot prove encoder application.
 * Profile2 validation requires preskip40 and CELT TOC top bit in every packet.
 * Header/profile authenticity must come from the enclosing authenticated
 * container; this structural validator does not authenticate relabeling.
 * Each record: packet_bytes:u16=60, flags:u16 (flush=1 otherwise0),
 * packet_index:u32 starting0, followed by exactly60 Opus bytes.
 * Decoder RESET each segment. Decode every packet at16k mono, discard preskip
 * at head and end_trim at tail: exactly captured_frames*320 valid samples.
 * A lookahead<=320 is REQUIRED; a positive lookahead causes one zero-padding
 * flush packet. The final audio tail is retained; flush zeros are not capture.
 * Independent reset boundaries may affect quality; this is not Ogg/.opus.
 * first/next_sample preserve supplied timeline; gaps only on explicit resume.
 * No PCM/packets leave except the single encryption callback. Plaintext/codec
 * scratch are wiped after every seal/pause/end/fault; caller owns input PCM.
 */
int rp_segment_validate(const uint8_t*,size_t); /* structure + Opus framing only, not authentication */
#endif
