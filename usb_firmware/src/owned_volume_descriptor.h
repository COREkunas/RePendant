/* SPDX-License-Identifier: Apache-2.0
 * OFFLINE metadata codec. A valid descriptor is NOT ownership, activation,
 * preservation proof, a persistent lease, or permission to issue NAND I/O.
 */
#ifndef OPENPENDANT_OWNED_VOLUME_DESCRIPTOR_H
#define OPENPENDANT_OWNED_VOLUME_DESCRIPTOR_H
#include <stddef.h>
#include <stdint.h>
#include "owned_page.h" /* Reuse only the synchronous SHA-256 provider interface. */
#ifndef OWNED_VOLUME_API
#define OWNED_VOLUME_API
#endif

#define OWNED_VOLUME_DESCRIPTOR_BYTES 512U
#define OWNED_VOLUME_HEADER_BYTES 352U
#define OWNED_VOLUME_DIGEST_OFFSET 352U
#define OWNED_VOLUME_PADDING_OFFSET 384U
#define OWNED_VOLUME_MAP_BLOCKS 32U
#define OWNED_VOLUME_CONTROL_BLOCKS 2U
#define OWNED_VOLUME_PAGES_PER_BLOCK 64U
#define OWNED_VOLUME_LOGICAL_PAGES 2048U
#define OWNED_VOLUME_BLOCK_LIMIT 1536U

enum owned_volume_result {
    OWNED_VOLUME_VALID = 0,
    OWNED_VOLUME_NOT_VALID = 1, /* Includes all-FF bytes; no erased/free inference. */
    OWNED_VOLUME_ARGUMENT = -1,
    OWNED_VOLUME_HASH_ERROR = -2
};

/* Never serialize this compiler-dependent struct representation.
 * UUIDs use canonical text/network byte order, matching owned_page/Android.
 * Identity/hash arrays must be neither all00 nor allFF. Map slots stay ordered;
 * no sorting/compaction is allowed when a block is subsequently retired.
 * These are candidate block numbers only: no IDs are selected by this module.
 */
struct owned_volume_spec {
    uint8_t device_id[16], volume_id[16];
    uint64_t generation; /* 1..INT64_MAX */
    /* SHA-256(ASCII "OpenPendant recipient public key v1\0" || KEM 0x0010 BE ||
     * canonical 65-byte uncompressed SEC1 P-256 point), matching Android. */
    uint8_t recipient_fingerprint[32];
    uint8_t preservation_manifest_sha256[32];
    uint32_t map_blocks[OWNED_VOLUME_MAP_BLOCKS];
    uint32_t control_blocks[OWNED_VOLUME_CONTROL_BLOCKS];
};

struct owned_volume_decoded {
    struct owned_volume_spec spec;
    uint8_t digest[32];
};

/* SHA-256(concatenation of eight raw pinned Dhara file hashes in the documented
 * lexical relative-path order). This pins bytes/policy, not deployed behavior. */
extern const uint8_t owned_volume_dhara_sha256[32];

/* Exact512-byte destination, no overlap with spec/provider. Invalid arguments
 * leave output untouched; provider failure wipes all512 bytes. Provider contract
 * is owned_page_hash: trusted, synchronous, input immutable, exactly32 output.
 * No pointers are retained, no allocation/globals with mutable state or I/O.
 */
OWNED_VOLUME_API int owned_volume_descriptor_build(uint8_t *out, size_t out_size,
    const struct owned_volume_spec *spec, const struct owned_page_hash *hash);

/* Trusted expected spec is REQUIRED and must not be derived from these untrusted
 * bytes as an authority shortcut. Every identity, hash and ordered block entry
 * must match it, in addition to canonical fields/CRC/SHA/padding validation.
 * Non-VALID clears decoded; malformed/overlapping decoded arguments instead
 * leave it untouched to avoid corrupting inputs. Result remains metadata only.
 */
OWNED_VOLUME_API int owned_volume_descriptor_validate(const uint8_t *bytes,
    size_t size, const struct owned_volume_spec *expected,
    const struct owned_page_hash *hash, struct owned_volume_decoded *decoded);

/* Checks logical_page<2048 BEFORE multiplication/indexing, then revalidates the
 * whole immutable descriptor and full trusted binding before mapping. Physical
 * row output is unchanged on any error. Only map slots, never control banks.
 * A returned row is arithmetic metadata, NOT a capability or NAND write API.
 */
OWNED_VOLUME_API int owned_volume_descriptor_map(const uint8_t *bytes, size_t size,
    const struct owned_volume_spec *expected, const struct owned_page_hash *hash,
    uint32_t logical_page, uint32_t *physical_row);
#endif
