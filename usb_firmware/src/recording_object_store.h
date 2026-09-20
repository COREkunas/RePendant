/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OPENPENDANT_RECORDING_OBJECT_STORE_H
#define OPENPENDANT_RECORDING_OBJECT_STORE_H
#include <stdatomic.h>
#include "recording_pipeline.h"
#include "owned_page.h"
#include "recording_large_layout.h"

/* UNLINKED logical object layer. No physical address, format/erase/trim, thread,
 * microphone, allocator, SDK or automatic recovery/write authority. The backend
 * must be an exclusively owned, descriptor-bound, resumed logical map with
 * durable sync and checked reads; owned_page belongs BELOW that map. MISSING is
 * authoritative map-unmapped, NEVER an FF/ECC heuristic. All successful reads
 * fill exactly2048 bytes. Writes may replace a logical catalog sector through
 * the map, never reprogram a NAND page. Backend errors/ambiguity fence this
 * context. No retry or rollback-to-older-catalog fallback exists here.
 *
 * The caller owns one zero-initialized context for the mounted volume. Do not
 * copy/reset/reinitialize it after a fault; reboot/recovery is external. Public
 * API actors are serialized by a nonblocking atomic gate. No callback retains
 * pointers on ANY return, including error/timeout. The concrete NAND backend
 * must copy into its OWN permanent DMA buffers and never DMA into these spans;
 * unresolved backend DMA retains backend ownership, not caller spans. This
 * contract makes fault wiping safe; an adapter unable to prove it is forbidden.
 * Arguments/output buffers must not overlap context or each other.
 * Hash/seal providers are trusted; seal MUST use recording_hpke_psa_seal on the
 * single persistent provider owner in target integration, not borrowed callbacks.
 * No deterministic crypto provider is included in production.
 *
 * One absolute uint64 monotonic deadline per job, <=30s, checked before/after
 * every callback. Cancellation/deadline cannot preempt crypto/backend calls:
 * independent supervisor + capture inhibition remain required. Caller supplies
 * a qualified worker/stack, no ISR. Each step does at most one backend call,
 * plus bounded hash/metadata work. prepare performs one synchronous HPKE job.
 * PENDING is NOT a recording_pipeline durable receipt. The current synchronous
 * rp_sink requires a separately reviewed scheduling/buffering bridge; do not
 * return success merely because this core accepted a job.
 */
#define ROS_ROOTS 8U
#define ROS_RETIRED_MAX 14U
#define ROS_SLOTS 32U
#define ROS_EXTENT_ROOTS 32U
#define ROS_EXTENT_SLOTS 3600U
#define ROS_EXTENT_CAPACITY (ROS_EXTENT_ROOTS+ROS_EXTENT_SLOTS*ROS_SLOT_PAGES)
#define ROS_EXTENT_RECOVER_MS 120000U
#define ROS_DATA_PAGES 17U
#define ROS_SLOT_PAGES (1U+ROS_DATA_PAGES)
#define ROS_CONTAINER_BYTES (RP_PLAINTEXT_CAPACITY+ES_HEADER_BYTES+ES_ENCAP_BYTES+ES_TAG_BYTES)
#define ROS_JOB_MAX_MS 30000U
enum ros_rc { ROS_OK=0, ROS_PENDING=1, ROS_READY=2, ROS_ARGUMENT=-1,
 ROS_BUSY=-2, ROS_STATE=-3, ROS_FULL=-4, ROS_FAILED=-5, ROS_NOT_FOUND=-6 };
enum ros_read_rc { ROS_READ_OK=0, ROS_READ_MISSING=1 };
enum ros_job { ROS_NONE=0,ROS_RECOVER=1,ROS_RESERVE=2,ROS_SEGMENT=3,ROS_FINALIZE=4,ROS_LOAD=5,ROS_META=6 };
struct ros_port {
 void *user;
 uint64_t (*now_ms)(void*);
 int (*cancelled)(void*); /* nonblocking,0 only means continue */
 /* Checks complete owner/volume/power/backend admission. Never grants new
  * physical capabilities. Called before every read/write/sync/crypto job. */
 int (*admit)(void*,int writing,uint64_t deadline);
 int (*read)(void*,uint32_t sector,uint8_t page[2048],uint64_t deadline);
 int (*write)(void*,uint32_t sector,const uint8_t page[2048],uint64_t deadline);
 int (*sync)(void*,uint64_t deadline);
 int (*seal)(void*,const uint8_t recipient[65],const uint8_t info[161],
             const uint8_t *plain,size_t plain_bytes,uint8_t enc[65],
             uint8_t *cipher,size_t cipher_capacity,size_t *actual);
};
struct ros_record {
 struct es_binding identity; /* plaintext/sequence0; never raw struct on disk */
 uint64_t first_sample,next_sample,committed_samples;
 uint32_t present,finalized,interrupted,mode,segments,gaps,profile,blocked;
 struct rp_completion completion;
};
struct ros_segment {
 struct es_binding identity;
 uint64_t first_sample,next_sample,samples;
 uint32_t root,slot,profile,gap,container_bytes,pages;
 uint8_t digest[32];
};
struct ros_status {
 uint32_t mounted,fault,job,stage,ready,slot_count,consumed_slots;
 int32_t error;
};
/* Private fields are exposed solely for fixed static allocation. Do not modify. */
struct ros_context {
 atomic_uint gate;
 uint32_t initialized;
 struct ros_port port;
 struct owned_page_hash hash;
 struct es_binding volume;
 uint8_t recipient[65];
 struct ros_status status;
 uint64_t deadline,last_now;
 uint32_t root,slot,index,scan_root,scan_slot,kind,active_root;
 uint32_t root_count,segment_limit,extent_profile;
 /* One nibble per slot (free / intent / committed), never payloads. */
 uint8_t used[(RLL_SLOTS+1U)/2U];
 uint32_t retired_count;
 uint8_t retired[RLL_TOMBSTONES][16];
 struct ros_record records[ROS_EXTENT_ROOTS],pending_record;
 struct ros_segment segment;
 struct rp_segment_receipt receipt;
 struct rp_final_receipt final_receipt;
 uint8_t container[ROS_CONTAINER_BYTES],catalog[2048],page[2048];
};
int ros_init(struct ros_context*,const struct ros_port*,const struct owned_page_hash*,
 const struct es_binding *trusted_volume,const uint8_t trusted_recipient[65],uint32_t logical_capacity);
/* Separate large layout: 32 roots, 3600 ten-second slots (64832 sectors).
 * Legacy init, on-disk tag, 8 roots / <=32 slots and recovery behavior remain
 * unchanged. Integration must bind this profile to a NEW extent descriptor.
 * Mount rebuilds the integrity-checked COMMIT metadata index, not all audio.
 * Every LOAD still verifies the complete encrypted container before export;
 * commits still sync and read back ALL payload bytes before success.
 * No receipt/delete/enrollment migration is implied by this lower layer. */
int ros_init_extent(struct ros_context*,const struct ros_port*,const struct owned_page_hash*,
 const struct es_binding *trusted_volume,const uint8_t trusted_recipient[65]);
/* Full-chip object format3,32 roots /5120 slots; requires the separately bound
 * full-chip map and complete retained deletion history on every mount. */
int ros_init_full(struct ros_context*,const struct ros_port*,const struct owned_page_hash*,
 const struct es_binding *trusted_volume,const uint8_t trusted_recipient[65]);
int ros_recover(struct ros_context*,uint64_t deadline); /* reads only; mandatory */
/* Trusted coordinator only: complete, durable, volume-bound tombstone snapshot.
 * No remote request or inferred receipt may supply this list. It is monotonic:
 * no previously retired UUID can disappear or ever be reserved again.
 * Exclusively idle, with no active recording/catalog consumer. Returns OK if
 * mounted and unchanged; otherwise PENDING for a READ-ONLY rebuild. Canonical
 * retired roots/slots are excluded before live ownership/sequence accounting.
 * Logical addresses may then be replaced via normal journaled writes. Old
 * ciphertext is NOT physically erased, and tombstones must NEVER be pruned.
 * Reuse requires this recovery rule on every subsequent boot (no downgrade). */
int ros_recover_retired(struct ros_context*,const struct es_binding *volume,
 const uint8_t *recording_ids,uint32_t count,uint64_t deadline);
int ros_reserve(struct ros_context*,const struct es_binding*,enum rp_mode,uint64_t first_sample,uint64_t deadline);
int ros_prepare(struct ros_context*,const struct es_binding*,const uint8_t*,size_t,uint64_t deadline);
/* Copies into the private container; no seal or backend I/O on submission.
 * First ros_step encrypts exactly in place, then the unchanged intent/data/
 * verification/commit sequence runs. port.seal MUST support plain==cipher,
 * with cipher_capacity==plain_bytes+16. No pointers retained from callers.
 * Fault wipes staged plaintext; no plaintext is ever written to the backend. */
int ros_prepare_deferred(struct ros_context*,const struct es_binding*,const uint8_t*,size_t,uint64_t deadline);
int ros_finalize(struct ros_context*,const struct rp_completion*,uint64_t deadline);
int ros_load(struct ros_context*,uint32_t committed_slot,uint64_t deadline);
/* One immutable COMMIT catalog read/hash, admitted only from the committed-slot
 * index established by full commit validation or profile-specific recovery.
 * Large-profile metadata recovery is NOT payload validation. No payload I/O.
 * Metadata is integrity-checked, NOT authenticated phone protocol authority. */
int ros_describe_committed(struct ros_context*,uint32_t committed_slot,uint64_t deadline);
int ros_step(struct ros_context*);
/* READY receipt is available once and only for the exact current job. take also
 * wipes job buffers. reserve/recovery READY must be acknowledged with finish.
 * Finalized/completed ciphertext is not a deletion or phone-transfer receipt. */
int ros_take_segment_receipt(struct ros_context*,struct rp_segment_receipt*);
int ros_take_final_receipt(struct ros_context*,struct rp_final_receipt*);
int ros_finish(struct ros_context*);
int ros_copy_loaded(struct ros_context*,uint32_t offset,uint8_t*,size_t capacity,size_t *actual);
/* Validated metadata only, exclusively after complete LOADREADY/METAREADY. Never a raw
 * private-context view, pending catalog entry, authentication or phone receipt. */
int ros_get_loaded_segment(struct ros_context*,struct ros_segment*);
int ros_get_status(struct ros_context*,struct ros_status*);
struct ros_geometry {uint32_t roots,slots,profile;};
int ros_get_geometry(struct ros_context*,struct ros_geometry*); /* trusted initialized layout */
int ros_get_record(struct ros_context*,uint32_t root,struct ros_record*);
#endif
