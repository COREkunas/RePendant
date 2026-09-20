/* SPDX-License-Identifier: Apache-2.0
 * Unlinked, single-worker adapter. No physical volume is selected by this code.
 * All policy/history callbacks below are trusted durability boundaries, NOT
 * implemented by the descriptor/control codecs or by this adapter.
 */
#ifndef OPENPENDANT_OWNED_DHARA_H
#define OPENPENDANT_OWNED_DHARA_H
#include "owned_control_record.h"
#include "dhara/map.h"

enum owned_dhara_result { OD_OK=0, OD_MISSING=1, OD_NO_JOURNAL=2,
    OD_ARGUMENT=-1, OD_READ_ONLY=-2, OD_FATAL=-3, OD_LIBRARY=-4, OD_BUSY=-5 };
enum owned_dhara_fault { OD_FAULT_NONE=0, OD_FAULT_ADDRESS, OD_FAULT_HISTORY,
    OD_FAULT_TRANSPORT, OD_FAULT_ECC, OD_FAULT_ENVELOPE, OD_FAULT_HASH,
    OD_FAULT_LEASE, OD_FAULT_RESOLVE, OD_FAULT_RETIRE, OD_FAULT_CAPACITY, OD_FAULT_DEADLINE };
enum owned_dhara_io_result { OD_IO_OK=0, OD_IO_MEDIA=1,
    OD_IO_ERROR=-1, OD_IO_UNCERTAIN=-2 };
enum owned_dhara_page_state { OD_PAGE_UNKNOWN=0, OD_PAGE_UNUSED=1,
    OD_PAGE_COMMITTED=2, OD_PAGE_ABANDONED=3 };
enum owned_dhara_lease_mode { OD_LEASE_PER_OPERATION=0, OD_LEASE_RESERVED_RANGE=1 };
struct owned_dhara_page_info { uint32_t state; uint64_t lease_id; };
struct owned_dhara_capability {
    uint8_t descriptor_digest[32], control_digest[32];
    uint64_t sequence, lease_high_water;
    uint32_t state, retired_map;
};
struct owned_dhara_lease {
    uint8_t descriptor_digest[32];
    uint64_t sequence, lease_id, range_base;
    uint32_t kind, map_slot, physical_row;
};
struct owned_dhara_io {
    void *user;
    uint64_t (*now_ms)(void *);
    uint32_t operation_budget_ms; /* 1..120000, common to entire map call/GC. */
    /* Must independently prove persisted control/history, ownership, active
     * application root (ACTIVE), and power-loss reconciliation. A codec's
     * CLEAN_CANDIDATE alone is insufficient. No callback may reenter adapter. */
    int (*validate_capability)(void *,const struct owned_dhara_capability *,uint64_t deadline);
    /* UNUSED requires independent non-consumption history, not FF bytes. A
     * COMMITTED lease is exact producing history; reserved-range mode may expose
     * same-boot verified pages before history_sync, never after cold recovery. */
    int (*page_info)(void *,uint32_t row,struct owned_dhara_page_info *,uint64_t deadline);
    /* Physical callbacks use only backend-owned permanent buffers. OK/MEDIA
     * mean DMA STOP, NAND readiness and configuration/ownership are safe.
     * MEDIA is legal ONLY for a diagnosed program/erase media failure.
     * UNCERTAIN may retain the supplied buffer; adapter then never touches its
     * scratch again. Context must survive until separate safe reconciliation.
     * ERROR must not retain buffers. No caller logical payload is retained. */
    int (*read_corrected)(void *,uint32_t row,uint8_t main[4096],uint32_t *ecc,uint64_t deadline);
    int (*read_raw)(void *,uint32_t row,uint8_t raw[4352],uint64_t deadline);
    int (*program)(void *,uint32_t row,const uint8_t main[4096],uint64_t deadline);
    int (*erase)(void *,uint32_t physical_block,uint64_t deadline);
    /* Default mode: acquire must persist exact intent BEFORE returning. Every attempt uses
     * a fresh nonzero lease and increasing sequence, including failed START.
     * It proves ascending/not-previously-consumed PROGRAM and qualified ERASE.
     * Lease acquisition failure may be ambiguous and therefore fences. */
    int (*acquire)(void *,uint32_t kind,uint32_t slot,uint32_t row,
                   struct owned_dhara_lease *,uint64_t deadline);
    /* Default mode: persist verified completion/history; failure fences even after safe IO.
     * Failed media is resolved by retire(), never by a successful resolve. */
    int (*resolve)(void *,const struct owned_dhara_lease *,uint64_t deadline);
    int (*retire)(void *,uint32_t slot,uint32_t block,uint32_t new_bitmap,
                  const struct owned_dhara_lease *failed_lease,uint64_t deadline);
    /* Explicit opt-in only: persisted65-ID block reservation (erase+64pages),
     * volatile per-page consumption/resolution, then durable history publication
     * AFTER Dhara sync. ABANDONED reads are logically hidden uncommitted tails,
     * never free; only this mode may supply that classification. This is NOT
     * per-page persistent completion. The old mode and its guarantees stay0. */
    uint32_t lease_mode;
    int (*history_sync)(void *,uint64_t deadline);
};

/* Caller zero-initializes once; init refuses an already initialized object.
 * Never move/copy/reinitialize an active/faulted object. Serialized worker only.
 * Members are public for allocation, not permission to alter internal state.
 * No allocation, hardware API, global mutable registry or DMA caller alias. */
struct owned_dhara {
    struct dhara_nand nand; /* Required first member for Dhara callbacks. */
    struct dhara_map map;
    struct owned_volume_decoded volume;
    struct owned_page_hash hash;
    struct owned_dhara_io io;
    struct owned_dhara_capability capability;
    struct owned_dhara_lease pending;
    uint64_t sequence, lease_high_water, deadline, last_now;
    uint64_t range_base[32], range_consumed[32];
    uint32_t magic, fault, retired_map, media_pending, retained_dma;
    uint32_t ready, writable, busy, provisioning;
    dhara_error_t library_error;
    _Alignas(4) uint8_t metadata[2048], payload[2048], main[4096], raw[4352];
};
int owned_dhara_init(struct owned_dhara *,const uint8_t descriptor[512],
    const struct owned_volume_spec *,const struct owned_page_hash *,
    const struct owned_dhara_io *,uint32_t persisted_retired_map);
/* Every operation uses min(caller absolute deadline, now+configured budget),
 * never renewed inside GC. Providers must bound themselves by that deadline. */
int owned_dhara_resume(struct owned_dhara *,uint64_t deadline); /* Never formats. */
int owned_dhara_grant(struct owned_dhara *,const struct owned_dhara_capability *,uint64_t deadline);
/* Explicit deliberate destructive provisioning, never called by resume.
 * Erases only descriptor map members, requires trusted PROVISIONING capability.
 * A failure cannot be retried on this instance. No ACTIVE capability accepted. */
int owned_dhara_format(struct owned_dhara *,const struct owned_dhara_capability *,uint64_t deadline);
int owned_dhara_read(struct owned_dhara *,uint32_t sector,uint8_t out[2048],uint64_t deadline);
int owned_dhara_write(struct owned_dhara *,uint32_t sector,const uint8_t data[2048],uint64_t deadline);
int owned_dhara_sync(struct owned_dhara *,uint64_t deadline);
/* Conservative total allocation capacity, not free sectors. Sector IDs0..417
 * stay stable after retirement; reads of existing high IDs remain permitted. */
int owned_dhara_capacity(const struct owned_dhara *,uint32_t *sectors);
void owned_dhara_revoke(struct owned_dhara *); /* Never clears a fault. */
#endif
