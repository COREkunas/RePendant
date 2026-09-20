/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OPENPENDANT_RECORDING_EXTENT_H
#define OPENPENDANT_RECORDING_EXTENT_H
#include "owned_page.h"
#include "recording_large_layout.h"

/* A NEW format, not an enlargement of the old 32-block ownership table.
 * This first large profile deliberately leaves blocks 1536..2047 untouched.
 * A0=0x48 temporarily permits blocks 0..1535; every operation still requires
 * independent live power/session authority. A descriptor is metadata, not
 * permission to format. No wire-controlled geometry or arbitrary chip select.
 * 128 MiB logical space leaves Dhara GC / metadata / bad-block headroom inside
 * 384 MiB physical space. Each 2 KiB logical sector uses a 4 KiB physical page.
 */
#define REX_FIRST_BLOCK 0U
#define REX_BLOCKS 1536U
#define REX_CAPACITY 65536U
#define REX_MAX_BAD_BLOCKS 40U
#define REX_DESCRIPTOR_BYTES 256U
#define REX_DIGEST_OFFSET 224U
struct recording_extent_identity {
 uint8_t device_id[16],volume_id[16];
 uint64_t generation;
 uint8_t recipient_fingerprint[32];
};
/* Canonical little-endian descriptor. Trusted expected identity must come
 * from owner configuration, NEVER from the NAND bytes under validation.
 * No I/O, allocation, keys, old preservation-manifest authority or migration.
 * Returns 0 valid, -1 argument/content, -2 hash provider failure. */
int rex_build(uint8_t out[REX_DESCRIPTOR_BYTES],
 const struct recording_extent_identity *,const struct owned_page_hash *);
int rex_validate(const uint8_t bytes[REX_DESCRIPTOR_BYTES],
 const struct recording_extent_identity *,const struct owned_page_hash *);
/* Distinct descriptor v2 for the complete 512 MiB chip; old v1 is unchanged.
 * Requires explicit destructive migration, never an in-place expansion. */
int rex_build_full(uint8_t out[REX_DESCRIPTOR_BYTES],
 const struct recording_extent_identity *,const struct owned_page_hash *);
int rex_validate_full(const uint8_t bytes[REX_DESCRIPTOR_BYTES],
 const struct recording_extent_identity *,const struct owned_page_hash *);
#endif
