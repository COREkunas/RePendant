/* SPDX-License-Identifier: Apache-2.0 -- UNLINKED logical metadata only. */
#ifndef OPENPENDANT_RECORDING_SYNC_METADATA_H
#define OPENPENDANT_RECORDING_SYNC_METADATA_H
#include <stdatomic.h>
#include "owned_page.h"
#include "recording_large_layout.h"
#define RSM_PAGE_BYTES 2048U
#define RSM_RECEIPTS 32U
#define RSM_TOMBSTONES 14U
#define RSM_RECEIPT_BATCH_MAX 32U
enum rsm_result { RSM_OK=0,RSM_NOT_FOUND=1,RSM_NOT_CREATED=2,RSM_ARGUMENT=-1,
 RSM_BUSY=-2,RSM_REFUSED=-3,RSM_CONFLICT=-4,RSM_FULL=-5,RSM_FAULT=-6 };
struct rsm_volume {
 uint8_t device[16],volume[16],recipient[32],descriptor_digest[32];
 uint64_t generation;
 uint32_t capacity; /* Immutable ORIGINAL logical ID space, not free capacity. */
};
struct rsm_receipt { uint8_t recording[16];uint32_t sequence,container_bytes;uint8_t digest[32]; };
struct rsm_terminal {
 uint8_t recording[16],manifest[32];uint64_t revision;uint32_t segments,state;
};
struct rsm_tombstone { uint8_t operation[16];struct rsm_terminal terminal;uint64_t revision; };
struct rsm_root_proof {
 uint8_t descriptor_digest[32],root_digest[32];
 uint32_t capacity,ros_capacity,slots;
};
struct rsm_port {
 void *user;
 uint64_t (*now_ms)(void *);
 int (*admit)(void *,int writing,uint64_t);
 /* Logical checked2048B only. Read1 means authoritative map-unmapped, never FF.
  * Callbacks retain NO pointers on any return. Backend owns permanent DMA. */
 int (*read)(void *,uint32_t,uint8_t page[2048],uint64_t);
 int (*write)(void *,uint32_t,const uint8_t page[2048],uint64_t);
 int (*sync)(void *,uint64_t);
 /* These resolve full current owner/volume/session and immutable committed
  * segment/terminal-manifest bindings, not untrusted request assertions.
  * Creation must be explicitly authorized inside PROVISIONING. */
 int (*approve_create)(void *,const struct rsm_volume *,uint64_t);
 int (*approve_receipt)(void *,const struct rsm_volume *,const struct rsm_receipt *,uint64_t);
 int (*approve_delete)(void *,const struct rsm_volume *,const uint8_t operation[16],const struct rsm_terminal *,uint64_t);
};
struct recording_sync_metadata {
 atomic_uint gate;
 uint32_t magic,mounted,fault,attempted,format;
 uint64_t deadline,last_now;
 struct rsm_volume volume;
 struct rsm_port port;
 struct owned_page_hash hash;
 uint8_t root_digest[32];
 /* Mutually exclusive mounted formats. The paged profile never needs the
  * legacy two-page cache, so that allocation holds its UUID revocations. */
 union {
  struct {uint8_t receipts[2048],tombstones[2048];};
  uint8_t deleted_ids[RLL_TOMBSTONES][16];
 };
 uint8_t work[2048],verify[2048];
 /* Large profile caches UUID revocations only, not full receipts/manifests.
  * Replay details are read on demand from independently checked pages. */
 uint32_t extent_profile,deleted_count;
 uint64_t delete_revision;
};
int rsm_init(struct recording_sync_metadata *,const struct rsm_volume *,const struct owned_page_hash *,const struct rsm_port *);
int rsm_init_extent(struct recording_sync_metadata *,const struct rsm_volume *,const struct owned_page_hash *,const struct rsm_port *);
/* Exact physical object SLOT supplied by the trusted catalog, not phone input.
 * One independently synced receipt per slot, safely replaceable only after
 * the former recording has a durable retained tombstone. */
int rsm_receive_slot(struct recording_sync_metadata *,uint32_t,const struct rsm_receipt *,uint64_t);
/* Same format3 pages, at most32 distinct trusted slots. All bindings/conflicts
 * are checked before the first write; each affected page syncs/readbacks once.
 * Success means ALL entries durable. Failure can leave a durable subset and
 * fences the context on I/O ambiguity; reopen and exact retry are idempotent.
 * External disjoint immutable arrays are borrowed only until return. */
int rsm_receive_slots(struct recording_sync_metadata *,const uint32_t *slots,
 const struct rsm_receipt *receipts,uint32_t count,uint64_t);
int rsm_has_receipt_slot(struct recording_sync_metadata *,uint32_t,const struct rsm_receipt *,uint64_t);
int rsm_replay_delete_paged(struct recording_sync_metadata *,const uint8_t operation[16],const uint8_t manifest[32],struct rsm_tombstone *,uint64_t);
int rsm_extent_deleted_snapshot(struct recording_sync_metadata *,uint8_t ids[RLL_TOMBSTONES][16],uint32_t *count);
/* Synchronous trusted coordinator callback under the metadata query gate.
 * Copies/submits the complete monotonic list without an 8 KiB stack copy.
 * Callback MUST NOT reenter RSM, retain pointers, do storage I/O or mutate IDs.
 * Full volume identity is supplied for independent coordinator binding. */
int rsm_visit_extent_deleted(struct recording_sync_metadata *,void *user,
 int (*visit)(void *,const struct rsm_volume *,const uint8_t *ids,uint32_t count));
/* Create only when ALL3 pages are MISSING and flag1+trusted admission explicitly
 * allow it. Empty companions are durable first; immutable root is published last.
 * Partial initialization/corruption never auto-repairs. One open attempt/context. */
int rsm_open(struct recording_sync_metadata *,uint32_t create_if_all_missing,uint64_t);
/* Explicit idle/writable-owner operation, never automatic on mount. v1 -> v2
 * only for the exact 418-sector layout. Copies companions into unused tail
 * sectors, verifies each checkpoint, then atomically replaces the root LAST.
 * Existing receipts/tombstones and their revisions are retained. */
int rsm_extend(struct recording_sync_metadata *,uint64_t);
int rsm_receive(struct recording_sync_metadata *,const struct rsm_receipt *,uint64_t);
int rsm_delete(struct recording_sync_metadata *,const uint8_t operation[16],const struct rsm_terminal *,uint64_t *revision,uint64_t);
/* Pure mounted-state queries, no physical/crypto callbacks or new authority.
 * Replay works after ROS/catalog omission. Exact operation/hash only. */
int rsm_has_receipt(struct recording_sync_metadata *,const struct rsm_receipt *);
int rsm_replay_delete(struct recording_sync_metadata *,const uint8_t operation[16],const uint8_t manifest[32],struct rsm_tombstone *);
int rsm_record_deleted(struct recording_sync_metadata *,const uint8_t recording[16]);
/* Cached complete durable tombstone UUID snapshot for the trusted volume
 * coordinator's read-only ROS rebuild. Does not prune/alter deletion replay. */
int rsm_deleted_snapshot(struct recording_sync_metadata *,uint8_t ids[RSM_TOMBSTONES][16],uint32_t *count);
int rsm_get_root_proof(struct recording_sync_metadata *,struct rsm_root_proof *);
/* Tombstones mean logical deletion; ciphertext stays retained. No free/reclaim,
 * trim, eviction or receipt-implied deletion. No physical address API. */
#endif
