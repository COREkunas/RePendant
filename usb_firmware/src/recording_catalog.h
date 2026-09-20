/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OPENPENDANT_RECORDING_CATALOG_H
#define OPENPENDANT_RECORDING_CATALOG_H
#include "durable_ble_codec.h"
#include "recording_manifest.h"
#include "recording_sync_metadata.h"

/* One frozen, owner-bound sync snapshot. Caller exclusively reserves ROS/RSM,
 * inhibits recording, authenticates the exact BLE connection and supervises
 * the absolute deadline before EVERY call. No hardware ownership is acquired
 * here. Full manifests are cached; loaded segment bytes remain in ROS only.
 * Nonce must come from the phone's admitted logical session. Never reuse a
 * nonce. Clean context reuse requires explicit cancel, joined actors and
 * rcat_retire under the same parent's exclusive storage lease; failures and
 * mount changes may NOT be cleared/reinitialized here. The prior nonce is
 * retained to reject immediate reuse; lifetime nonce uniqueness is the parent
 * authenticated-session contract, not an unbounded in-RAM replay database.
 * Caller request/response/length and open-nonce spans must be disjoint from
 * context/ROS/RSM and each other; input spans stay immutable until return.
 * The init descriptor/ports are likewise external immutable inputs; pure RSM
 * resolver arguments may be borrowed from its exclusively owned context.
 * A response and its
 * length are published together only on success, after the last callback.
 * Responses are complete OP frames, including sequence/status. Receipt/delete
 * success follows RSM sync+readback, never a volatile catalog update.
 */
enum rcat_result { RCAT_OK=0,RCAT_ARGUMENT=-1,RCAT_REFUSED=-2,RCAT_FAULT=-3,RCAT_BUSY=-4 };
struct rcat_port {
 void *user;
 uint64_t (*now_ms)(void*);
 int (*admit)(void*,uint64_t deadline); /* Full owner/volume/session, nonblocking. */
 int (*yield)(void*,uint64_t deadline);
 /* Exact lease revoked, all catalog actors joined, exclusive same-volume
  * ROS/RSM ownership still held. No I/O/wait. Only cleanup authority, not a
  * new session or storage authority. Parent must reject stale actors. */
 int (*retire_admit)(void*,uint64_t cleanup_deadline);
};
struct rcat_item {
 struct db_catalog_entry entry;
 uint16_t bytes;
 uint8_t manifest[RM_MAX_BYTES];
};
struct recording_catalog {
 atomic_uint gate,cancelled;
 uint32_t initialized,opened,fault,count,loaded_slot,loaded,prior_valid;
 uint64_t last_now,expires,deadline;
 uint8_t nonce[16],prior_nonce[16],catalog[DB_FULL_MAX_CATALOG];
 uint16_t catalog_bytes;
 struct rcat_port port;
 struct owned_page_hash hash;
 struct db_volume volume;
 struct ros_context *store;
 struct recording_sync_metadata *metadata;
 uint32_t full_profile;
 struct rm_stream_port streaming; /* hash provider only; segment is internal */
 union {
  struct {
   struct rcat_item items[DB_MAX_RECORDINGS];
   struct ros_segment segments[RM_MAX_SEGMENTS];
   uint8_t slots[DB_MAX_RECORDINGS][RM_MAX_SEGMENTS];
  };
  struct {
   struct {
    struct db_catalog_entry entry;
    uint8_t header[RM_HEADER_BYTES];
    uint16_t root,base;
   } items[RLL_ROOTS];
   uint16_t slots[RLL_SLOTS];
   struct ros_segment segment;
   uint32_t selected,cache_valid,cache_item,cache_sequence;
   /* Borrowed only inside one synchronous, gated receive-range call. These
    * identities were resolved from the frozen catalog before RSM admission. */
   const struct rsm_receipt *batch_receipts;
   uint32_t batch_count;
   uint8_t entry[RM_ENTRY_BYTES];
   /* First-pass ordered-run validation uses the SAME single hash provider.
    * All state belongs only to this immutable lease; no cross-lease cache. */
   struct rm_incremental run;
   uint32_t run_item,hash_active;
   uint8_t ready[RLL_ROOTS];
  } full;
 };
};
int rcat_init(struct recording_catalog*,struct ros_context*,struct recording_sync_metadata*,
 const struct db_volume*,const struct owned_page_hash*,const struct rcat_port*);
/* Explicit profile selection from trusted configuration, never from a request.
 * Full catalog is streamed from an exclusively held immutable ROS snapshot.
 * The provider's segment callback is unused. Hash callbacks must not retain
 * input/output spans, and abort must release the hash operation on every exit. */
int rcat_init_full(struct recording_catalog*,struct ros_context*,struct recording_sync_metadata*,
 const struct db_volume*,const struct owned_page_hash*,const struct rcat_port*,const struct rm_stream_port*);
int rcat_open(struct recording_catalog*,const uint8_t nonce[16],uint64_t deadline);
int rcat_handle(struct recording_catalog*,const struct db_request*,uint8_t response[DB_MAX_RESPONSE],
 size_t *response_bytes,uint64_t deadline);
int rcat_cancel(struct recording_catalog*); /* Nonblocking latch, no buffer reuse. */
int rcat_retire(struct recording_catalog*,uint64_t cleanup_deadline);
/* Pure immutable resolver callbacks for RSM. Parent additionally checks exact
 * authenticated lease. They do not call ROS/RSM recursively or grant authority. */
int rcat_approve_receipt(const struct recording_catalog*,const struct rsm_volume*,const struct rsm_receipt*);
int rcat_approve_delete(const struct recording_catalog*,const struct rsm_volume*,const uint8_t operation[16],const struct rsm_terminal*);
#endif
