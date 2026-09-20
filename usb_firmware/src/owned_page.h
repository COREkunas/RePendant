/* SPDX-License-Identifier: Apache-2.0
 * Offline-owned-volume page format. No NAND, GPIO, allocation or global state.
 */
#ifndef OPENPENDANT_OWNED_PAGE_H
#define OPENPENDANT_OWNED_PAGE_H
#include <stddef.h>
#include <stdint.h>
#ifndef OWNED_PAGE_API
#define OWNED_PAGE_API
#endif

#define OWNED_PAGE_BYTES 4096U
#define OWNED_PAGE_HEADER_BYTES 96U
#define OWNED_PAGE_PAYLOAD_BYTES 2048U
#define OWNED_PAGE_DIGEST_BYTES 32U
#define OWNED_PAGE_DIGEST_OFFSET 2144U
#define OWNED_PAGE_PADDING_OFFSET 2176U
#define OWNED_PAGE_ROW_LIMIT 131072U

enum owned_page_kind { OWNED_PAGE_DATA = 1, OWNED_PAGE_COMMIT = 2 };
enum owned_page_result {
    OWNED_PAGE_VALID = 0,
    /* Observed all-FF MAIN bytes only. NOT erased OOB, unprogrammed history,
     * ownership, erase/program eligibility, or power-cut-tail reuse proof. */
    OWNED_PAGE_ERASED_LOOKING = 1,
    OWNED_PAGE_NOT_VALID = 2,
    OWNED_PAGE_ARGUMENT = -1,
    OWNED_PAGE_HASH_ERROR = -2
};

/* This struct is NEVER serialized by copying its representation. The caller
 * supplies the expected trusted identity on every validation. Geometry checks
 * are format constraints, not a physical address allowlist or write authority. */
struct owned_page_binding {
    /* UUID bytes in canonical text/network order, NOT Windows GUID memory
     * order. Hardware serial-to-UUID assignment is outside this module. */
    uint8_t device_id[16];
    uint8_t volume_id[16];
    uint64_t generation; /* 1..INT64_MAX, matching Android's positive Long. */
    uint32_t logical_page;
    uint32_t physical_row;
    uint64_t lease_id;
    uint16_t kind;
};

struct owned_page_hash {
    void *user;
    /* Trusted synchronous SHA-256 provider: no retained pointers, no input
     * mutation, no reentrant page mutation. Return0 and exactly32 bytes;
     * failure/short/oversized outputs are rejected. out_capacity is32.
     * This module cannot prove a dishonest provider computed SHA-256. */
    int (*sha256)(void *user, const uint8_t *input, size_t length,
                  uint8_t *out, size_t out_capacity, size_t *out_length);
};

struct owned_page_view {
    /* Only non-NULL after VALID; borrowed while the immutable page survives. */
    const uint8_t *payload;
    uint8_t digest[OWNED_PAGE_DIGEST_BYTES];
};

/* Exact lengths only. Output may not overlap any input struct/buffer.
 * Bad arguments leave output untouched; provider failure wipes the full output.
 * Build DATA only; COMMIT must go through the checked relationship constructor. */
OWNED_PAGE_API int owned_page_build_data(uint8_t *out, size_t out_length,
    const struct owned_page_binding *binding, const uint8_t *payload,
    size_t payload_length, const struct owned_page_hash *hash);

/* Exact length, binding and canonical format checks precede payload exposure.
 * All non-VALID results leave view cleared (except malformed/overlapping view
 * arguments, which remain untouched). Integrity is NOT encryption/authentication.
 * Generic COMMIT validation proves its canonical payload syntax; validation of
 * its referenced DATA requires owned_page_match_commit. */
OWNED_PAGE_API int owned_page_validate(const uint8_t *page, size_t page_length,
    const struct owned_page_binding *expected, const struct owned_page_hash *hash,
    struct owned_page_view *view);

/* Fixed one2048-byte-object commit. DATA and COMMIT are adjacent logical and
 * physical pages in the same eraseblock, same device/volume/generation/lease.
 * object_id must be neither all00 nor allFF. This describes bytes only; it
 * never establishes that program/sync/STOP occurred, or authorizes either. */
/* Invalid source data leaves output untouched; a hash-provider error at either
 * source validation or commit sealing wipes the full output after argument
 * validation. The caller must use the result, never a previously valid buffer. */
OWNED_PAGE_API int owned_page_build_commit(uint8_t *out, size_t out_length,
    const struct owned_page_binding *commit_binding, const uint8_t object_id[16],
    const uint8_t *data_page, size_t data_length,
    const struct owned_page_binding *data_binding,
    const struct owned_page_hash *hash);

OWNED_PAGE_API int owned_page_match_commit(const uint8_t *commit_page,
    size_t commit_length, const struct owned_page_binding *commit_binding,
    const uint8_t object_id[16], const uint8_t *data_page, size_t data_length,
    const struct owned_page_binding *data_binding,
    const struct owned_page_hash *hash);
#endif
