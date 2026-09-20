/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OPENPENDANT_DURABLE_BLE_CODEC_H
#define OPENPENDANT_DURABLE_BLE_CODEC_H
#include <stddef.h>
#include <stdint.h>

/* Pure bounded codec, NOT linked/enabled by its existence. The backend must
 * authenticate the exact connection, reserve shared radio/storage ownership,
 * enforce deadlines, latch nonce/full volume/snapshot, verify object identity,
 * and durably commit receipts/tombstones BEFORE building successful replies.
 * No allocation/I/O/crypto/state, automatic retry or physical NAND authority.
 * Full layout: android_app/DURABLE_BLE_V1_20260916.md.
 * Inputs remain immutable; disjoint valid spans are required. Invalid argument
 * or semantic input leaves outputs unchanged. Capacity is EXACT encoded size.
 * UUID/digest arrays use network/digest byte order, integers encode LE.
 */
#define DB_CAPABILITY (1UL << 5)
#define DB_CATALOG 0x30U
#define DB_MANIFEST 0x31U
#define DB_SEGMENT 0x32U
#define DB_RECEIVE_ACK 0x33U
#define DB_DELETE 0x34U
#define DB_FULL_CAPABILITY (1UL << 7)
#define DB_WIDE_CAPABILITY (1UL << 9)
#define DB_STREAM_CAPABILITY (1UL << 10)
#define DB_RECEIPT_BATCH_CAPABILITY (1UL << 11)
#define DB_FULL_RECEIVE_RANGE 0x3eU
#define DB_RECEIPT_BATCH_MAX 32U
#define DB_FULL_STREAM 0x3dU
#define DB_STREAM_MAX_DATA 4096U
#define DB_FULL_CATALOG 0x38U
#define DB_FULL_MANIFEST 0x39U
#define DB_FULL_SEGMENT 0x3aU
#define DB_FULL_RECEIVE_ACK 0x3bU
#define DB_FULL_DELETE 0x3cU
#define DB_MAX_REQUEST 82U
#define DB_MAX_RESPONSE 225U
#define DB_MAX_DATA 52U
#define DB_CATALOG_HEADER 128U
#define DB_CATALOG_ENTRY 64U
#define DB_MAX_RECORDINGS 8U
#define DB_MAX_CATALOG 640U
#define DB_MAX_MANIFEST 2176U
#define DB_MAX_CONTAINER 34357U
#define DB_FULL_MAX_DATA 192U
#define DB_FULL_MAX_RECORDINGS 32U
#define DB_FULL_MAX_SEGMENTS 5120U
#define DB_FULL_MAX_CATALOG (128U+64U*DB_FULL_MAX_RECORDINGS)
#define DB_FULL_MAX_MANIFEST (128U+64U*DB_FULL_MAX_SEGMENTS)

enum db_result { DB_OK=0, DB_ARGUMENT=-1, DB_INVALID=-2 };
struct db_request {
 uint8_t nonce[16],recording[16],operation[16],sha256[32];
 uint64_t revision;
 uint32_t segment,container_bytes,offset;
 uint16_t sequence,maximum;
 uint8_t command;
};
struct db_volume {
 uint8_t device[16],volume[16],fingerprint[32];
 uint64_t generation;
};
struct db_catalog_entry {
 uint8_t recording[16],manifest_sha256[32];
 uint64_t revision;
 uint32_t segments,state;
};

/* Complete OP request frame, not an arbitrary payload. Output has all unused
 * fields cleared. Manifest revision is the bounded producer value, not a
 * truncated arbitrary u64. A parsed DELETE still needs persisted intent/full
 * manifest -> terminal recording resolution; absence is never success. */
int durable_ble_parse_request(const uint8_t *frame,size_t bytes,struct db_request *out);
/* Exact min(maximum,total-offset) data only; every byte is supplied by a backend
 * that verified this immutable selector. SEGMENT data remains ciphertext. */
int durable_ble_read_response(uint8_t *out,size_t capacity,const struct db_request*,
 uint32_t total,const uint8_t *data,size_t bytes);
/* Caller has already committed a durable exact receipt; this NEVER deletes. */
int durable_ble_receipt_response(uint8_t *out,size_t capacity,const struct db_request*);
/* Caller has committed/reopened an exact persisted tombstone. Original terminal
 * revision is independently retained metadata, not inferred from the request. */
int durable_ble_delete_response(uint8_t *out,size_t capacity,const struct db_request*,
 uint64_t original_manifest_revision,uint64_t tombstone_revision);
/* Entries strictly sorted by unsigned network recording UUID bytes. Count0
 * permits a NULL entries pointer; other pointers are mandatory. No footer. */
int durable_ble_catalog_build(uint8_t *out,size_t capacity,const struct db_volume*,
 const uint8_t nonce[16],uint64_t snapshot_revision,const struct db_catalog_entry*,size_t count);
int durable_ble_catalog_build_full(uint8_t *out,size_t capacity,const struct db_volume*,
 const uint8_t nonce[16],uint64_t snapshot_revision,const struct db_catalog_entry*,size_t count);
/* Explicit command family negotiation. The old wire format is unchanged;
 * full-profile read requests/responses use u32 byte offsets/totals. */
int durable_ble_is_full(uint8_t command);
#endif
