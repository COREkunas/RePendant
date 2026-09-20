/* SPDX-License-Identifier: Apache-2.0
 * Offline control metadata only. No NAND authority, persistent lease, activation,
 * bank erase/rotation or physical durability is implemented by this codec.
 */
#ifndef OPENPENDANT_OWNED_CONTROL_RECORD_H
#define OPENPENDANT_OWNED_CONTROL_RECORD_H
#include "owned_volume_descriptor.h"
#ifndef OWNED_CONTROL_API
#define OWNED_CONTROL_API
#endif
#define OWNED_CONTROL_PAGE_BYTES 4096U
#define OWNED_CONTROL_HEADER_BYTES 192U
#define OWNED_CONTROL_DIGEST_OFFSET 192U
#define OWNED_CONTROL_PADDING_OFFSET 224U
#define OWNED_CONTROL_BANKS 2U
#define OWNED_CONTROL_SLOTS 64U
#define OWNED_CONTROL_RECORDS 128U
#define OWNED_CONTROL_NO_TARGET UINT32_MAX

enum owned_control_state { OWNED_CONTROL_PROVISIONING=1, OWNED_CONTROL_ACTIVE=2,
    OWNED_CONTROL_READ_ONLY=3 };
enum owned_control_lease { OWNED_CONTROL_NO_LEASE=0, OWNED_CONTROL_ERASE_BLOCK=1,
    OWNED_CONTROL_PROGRAM_PAGE=2 };
enum owned_control_resolution { OWNED_CONTROL_UNRESOLVED=0,
    OWNED_CONTROL_VERIFIED_COMPLETE=1, OWNED_CONTROL_ABANDONED_RETIRED=2 };
enum owned_control_result { OWNED_CONTROL_VALID=0, OWNED_CONTROL_NOT_VALID=1,
    OWNED_CONTROL_ARGUMENT=-1, OWNED_CONTROL_HASH_ERROR=-2 };

struct owned_control_record {
    uint64_t sequence, previous_sequence;
    uint8_t previous_digest[32];
    uint32_t state, lease_kind;
    uint64_t lease_id;
    uint32_t target_map_slot, target_page, target_physical_row;
    uint32_t bank, slot, physical_row;
    uint32_t retired_map, retired_control; /* Bits0..31 and bits0..1 respectively. */
    uint32_t resolution;
    uint64_t resolved_lease_id, lease_high_water;
};
struct owned_control_decoded {
    struct owned_control_record record;
    uint8_t digest[32];
};

/* Inputs must remain immutable, valid spans; outputs cannot overlap any input.
 * Both APIs revalidate the descriptor against the full caller-trusted spec.
 * Build bad arguments leave output unchanged; SHA error wipes full4096 output.
 * Validate nonvalid clears decoded, except malformed/aliasing output arguments.
 * Bank/slot expected placement is separately supplied, never trusted from page.
 */
OWNED_CONTROL_API int owned_control_record_build(uint8_t *out,size_t out_size,
    const struct owned_control_record *record,const uint8_t *descriptor,size_t descriptor_size,
    const struct owned_volume_spec *expected,const struct owned_page_hash *hash);
OWNED_CONTROL_API int owned_control_record_validate(const uint8_t *page,size_t page_size,
    uint32_t expected_bank,uint32_t expected_slot,const uint8_t *descriptor,size_t descriptor_size,
    const struct owned_volume_spec *expected,const struct owned_page_hash *hash,
    struct owned_control_decoded *decoded);

enum owned_control_observation_kind {
    OWNED_CONTROL_UNKNOWN_OBSERVATION=0, /* Zero-initialized/missing evidence fails closed. */
    /* Only a trusted observer with independent program/lease history may assert
     * PROVEN_UNUSED. AllFF main/raw bytes after lost power are insufficient. */
    OWNED_CONTROL_PROVEN_UNUSED=1,
    OWNED_CONTROL_PAGE_PRESENT=2,
    OWNED_CONTROL_ERASED_LOOKING=3,
    OWNED_CONTROL_UNREADABLE=4
};
struct owned_control_observation {
    uint32_t kind;
    const uint8_t *page; /* Required only for PAGE_PRESENT; exactly4096 bytes. */
};
enum owned_control_selection_kind { OWNED_CONTROL_NO_CANDIDATE=0,
    OWNED_CONTROL_CLEAN_CANDIDATE=1, OWNED_CONTROL_RECOVER_READ_ONLY=2 };
enum owned_control_reason {
    OWNED_CONTROL_UNKNOWN=1U, OWNED_CONTROL_BAD_RECORD=2U,
    OWNED_CONTROL_DUPLICATE_SEQUENCE=4U, OWNED_CONTROL_CHAIN_GAP=8U,
    OWNED_CONTROL_BAD_TRANSITION=16U, OWNED_CONTROL_PENDING_LEASE=32U,
    OWNED_CONTROL_RETIRED_CONTROL=64U, OWNED_CONTROL_NOT_ACTIVE=128U,
    OWNED_CONTROL_INCOMPLETE_HISTORY=256U
};
struct owned_control_selection {
    uint32_t kind, reasons, latest_valid, latest_bank, latest_slot;
    uint64_t latest_sequence;
    uint8_t latest_digest[32];
    uint32_t retired_map, retired_control; /* Union of all valid snapshot evidence. */
    /* Unknown/torn/history failures quarantine ALL map tails. An otherwise
     * complete pending lease identifies its entire target block as unsafe.
     * These are recovery constraints, not a positive 'safe to write' result. */
    uint32_t quarantine_all_map_tails, pending_lease_valid;
    uint64_t pending_lease_id;
    uint32_t pending_map_slot, pending_physical_block;
};
/* Fixed caller-owned scratch, no allocation/static mutable storage. Contents
 * cleared after every admitted call, including hash failures. Not authority. */
struct owned_control_workspace {
    struct owned_control_decoded records[OWNED_CONTROL_RECORDS];
    uint8_t valid[OWNED_CONTROL_RECORDS];
};

/* Exactly128 observations in bank0 slots0..63 then bank1 slots0..63. This first
 * selector accepts only the complete visible initial append history from
 * sequence1/bank0/slot0, then contiguous positions through at most sequence128.
 * No missing genesis, compacted/rotated bank, reused slot or wrap is inferred
 * safe. Any unknown/invalid evidence yields read-only, never older-clean fallback.
 * VALID return means selector ran; inspect kind/reasons. Even CLEAN_CANDIDATE
 * is metadata only and cannot authorize NAND. No writes, retries or reset.
 */
OWNED_CONTROL_API int owned_control_select(const struct owned_control_observation *observations,
    size_t count,const uint8_t *descriptor,size_t descriptor_size,
    const struct owned_volume_spec *expected,const struct owned_page_hash *hash,
    struct owned_control_workspace *workspace,struct owned_control_selection *selection);
#endif
