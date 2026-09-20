/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OPENPENDANT_RECORDING_MANIFEST_H
#define OPENPENDANT_RECORDING_MANIFEST_H
#include "recording_object_store.h"

/* Pure canonical metadata encoder. No storage, authentication, BLE, allocation,
 * startup, receipt or deletion. Caller supplies a single consistent snapshot
 * from validated committed ROS metadata, never pending/private context fields.
 * A SHA is content identity, NOT owner authentication or persistence proof.
 * Inputs remain immutable through the trusted synchronous SHA callback.
 * No argument/output may overlap. Invalid inputs leave outputs unchanged;
 * provider failure wipes both outputs. Exact output length is required.
 * All UUID bytes are network order; all integers are little endian.
 */
#define RM_HEADER_BYTES 128U
#define RM_ENTRY_BYTES 64U
#define RM_MAX_SEGMENTS 32U
#define RM_MAX_BYTES (RM_HEADER_BYTES+RM_ENTRY_BYTES*RM_MAX_SEGMENTS)
enum rm_result { RM_OK=0,RM_ARGUMENT=-1,RM_INVALID=-2,RM_HASH=-3 };
int recording_manifest_build(uint8_t *output,size_t bytes,uint8_t digest[32],
 const struct ros_record*,const struct ros_segment*,size_t count,
 const struct owned_page_hash*);

/* Full-chip v2 manifest: same 128/64 byte layout, distinct tag/version.
 * Read one verified segment at a time and hash incrementally. No full manifest
 * allocation: at 5120 segments the encoded manifest is 327808 bytes.
 * The caller owns an exclusive immutable ROS snapshot and checks all callback
 * deadlines/cancellation. Providers retain no spans, including on failure.
 * Hash abort is called on every exit after begin, including begin failure.
 * The header and digest are published together only after successful finish.
 */
#define RM_FULL_MAX_BYTES (RM_HEADER_BYTES+RM_ENTRY_BYTES*RLL_SLOTS)
struct rm_stream_port {
 void *user;
 int (*begin)(void *);
 int (*update)(void *,const uint8_t *,size_t);
 int (*finish)(void *,uint8_t digest[32]);
 void (*abort)(void *);
 int (*segment)(void *,uint32_t sequence,struct ros_segment *);
};
int recording_manifest_stream(uint8_t header[128],uint8_t digest[32],
 const struct ros_record *,const struct rm_stream_port *);
/* Encode one independently validated metadata entry for paged read responses.
 * The frozen stream index additionally binds recording/root/order/digest. */
int recording_manifest_entry(uint8_t out[64],const struct ros_segment *);

/* Incremental validation of the SAME v2 header/ordered entries. No hash owner,
 * storage, publication or callbacks. Start from an all-zero/cleared context;
 * push must receive logical sequence order. A content error fences the context
 * until clear. Header/entry output is staging only: it is not a complete
 * manifest/proof until finish AND the caller's hash finish both succeed.
 * Inputs/outputs must be separate from this privately owned fixed context.
 * The existing whole stream remains the reference/fallback implementation. */
struct rm_incremental {
 struct ros_record record;
 uint8_t slots[RLL_SLOTS/8U];
 uint64_t total,next;
 uint32_t count,gaps,root,state;
};
int recording_manifest_start(struct rm_incremental*,const struct ros_record*,uint8_t header[128]);
int recording_manifest_push(struct rm_incremental*,const struct ros_segment*,uint8_t entry[64]);
int recording_manifest_finish(struct rm_incremental*);
void recording_manifest_clear(struct rm_incremental*);
#endif
