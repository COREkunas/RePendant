/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OPENPENDANT_RECORDING_NATIVE_STORE_H
#define OPENPENDANT_RECORDING_NATIVE_STORE_H
#include "nand_owned_phy.h"
#include "recording_extent.h"
#include <dhara/map.h>

/* Native 4 KiB Dhara geometry over one explicitly selected recording pool.
 * The existing application keeps its 2 KiB sector protocol. Each such sector
 * occupies one bound, hashed 4 KiB map entry; no read/modify/write is needed.
 * The sector at capacity is a private format root, never visible to the phone.
 * Legacy constructor remains the qualified 32-block engineering pool. The new
 * extent constructor is separately identity-bound and requires new integration
 * and hardware qualification; it does NOT automatically enlarge old volumes.
 * No automatic formatting/retry, new bad-block retirement or internal-flash I/O. */
#define RNS_FIRST_BLOCK 1025U
#define RNS_BLOCKS 32U
#define RNS_CAPACITY 418U
#define RNS_ROOT RNS_CAPACITY
#define RNS_META_CACHE_ENTRIES 32U
enum rns_rc { RNS_OK=0,RNS_MISSING=1,RNS_ARGUMENT=-1,RNS_REFUSED=-2,
 RNS_FAULT=-3,RNS_BUSY=-4,RNS_MORE=2 };
enum rns_state { RNS_NEW=0,RNS_OPENING,RNS_FORMATTING,RNS_READING,
 RNS_WRITING,RNS_SUSPENDED,RNS_CLOSED,RNS_FENCED };
struct rns_hooks {
 void *user;
 /* Repeated at every physical operation, including fresh program readback.
  * The integration owns USB/power/session admission. No callback may reenter. */
 int (*admit)(void *,uint32_t access,uint32_t row,uint64_t deadline);
 int (*yield)(void *,uint64_t deadline);
};
struct recording_native_store {
 struct dhara_nand nand;
 atomic_uint gate;
 uint32_t magic,state,fault,fault_line,attempted,bad_mask,programs,erases,reads;
 uint64_t deadline,last_now;
 uint32_t first_block,block_count,capacity,bad_count,format_next,format_pending;
 uint64_t format_deadline;
 uint8_t bad_blocks[RLL_BLOCKS/8U];
 struct dhara_map map;
 struct nand_owned_phy phy;
 struct nop_port target;
 struct rns_hooks hooks;
 struct owned_page_hash hash;
 uint8_t descriptor_digest[32];
 /* Immutable, ECC-checked Dhara tree records only. Never cache payload,
  * raw reads, program verification or erase verification. Volatile per lease. */
 struct {uint32_t valid,page,offset;uint8_t bytes[DHARA_META_SIZE];}
  meta_cache[RNS_META_CACHE_ENTRIES];
 uint32_t meta_next,meta_hits,meta_misses;
 _Alignas(4) uint8_t metadata[4096],page[4096],scratch[4352];
};
/* Permanent zero-initialized context; never reset/move following a fault.
 * Hardware binding supplies a port; init itself does not open the peripheral. */
int rns_init(struct recording_native_store *,const struct owned_volume_decoded *,
 const struct nop_port *,const struct owned_page_hash *,const struct rns_hooks *);
/* New explicit profile, canonical identity-bound descriptor checked before
 * peripheral ownership. Legacy init always remains 32 blocks / 418 sectors. */
int rns_init_extent(struct recording_native_store *,const uint8_t[REX_DESCRIPTOR_BYTES],
 const struct recording_extent_identity *,const struct nop_port *,
 const struct owned_page_hash *,const struct rns_hooks *);
int rns_init_full(struct recording_native_store *,const uint8_t[REX_DESCRIPTOR_BYTES],
 const struct recording_extent_identity *,const struct nop_port *,
 const struct owned_page_hash *,const struct rns_hooks *);
int rns_format(struct recording_native_store *,uint64_t); /* explicit destructive call */
/* Large format is bounded/incremental: begin scans markers but never erases;
 * each step erases + verifies at most one block, except the last step which
 * publishes/syncs/verifies the private format root. RNS_MORE until complete.
 * Overall <= 15 min (v1 extent) / 60 min (full-chip v2), individual operations
 * <= 120s. The absolute overall deadline is never renewed.
 * No mount, writes or suspend while pending; no implicit resume after a crash.
 */
int rns_format_begin(struct recording_native_store *,uint64_t overall_deadline);
int rns_format_step(struct recording_native_store *,uint64_t step_deadline);
int rns_mount(struct recording_native_store *,uint64_t); /* ALWAYS read-only */
int rns_grant(struct recording_native_store *,uint64_t);
int rns_read(struct recording_native_store *,uint32_t,uint8_t[2048],uint64_t);
int rns_write(struct recording_native_store *,uint32_t,const uint8_t[2048],uint64_t);
int rns_sync(struct recording_native_store *,uint64_t);
int rns_suspend(struct recording_native_store *,uint64_t);
int rns_reopen(struct recording_native_store *,uint64_t); /* retained map, read-only */
int rns_close(struct recording_native_store *,uint64_t);

/* Dhara callback dispatcher. Exactly one native store per peripheral lifetime. */
int rns_owns(const struct dhara_nand *);
int rns_is_bad(const struct dhara_nand *,dhara_block_t);
void rns_mark_bad(const struct dhara_nand *,dhara_block_t);
int rns_erase(const struct dhara_nand *,dhara_block_t,dhara_error_t *);
int rns_prog(const struct dhara_nand *,dhara_page_t,const uint8_t *,dhara_error_t *);
int rns_is_free(const struct dhara_nand *,dhara_page_t);
int rns_nand_read(const struct dhara_nand *,dhara_page_t,size_t,size_t,uint8_t *,dhara_error_t *);
int rns_copy(const struct dhara_nand *,dhara_page_t,dhara_page_t,dhara_error_t *);
#endif
