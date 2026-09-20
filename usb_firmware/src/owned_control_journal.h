/* SPDX-License-Identifier: Apache-2.0
 * UNLINKED v2 persistent snapshot/range core. Physical callbacks are closed and
 * descriptor-bound, but real power/STOP/erase qualification is not supplied here.
 */
#ifndef OPENPENDANT_OWNED_CONTROL_JOURNAL_H
#define OPENPENDANT_OWNED_CONTROL_JOURNAL_H
#include "owned_dhara.h"
#define OCJ_PAGE_BYTES 4096U
#define OCJ_BODY_BYTES 1280U
#define OCJ_DIGEST_OFFSET 1280U
#define OCJ_PADDING_OFFSET 1312U
enum ocj_rc { OCJ_OK=0, OCJ_NO_ROOT=1, OCJ_ARGUMENT=-1, OCJ_REFUSED=-2,
    OCJ_FAULT=-3, OCJ_CONFLICT=-4, OCJ_BUSY=-5 };
enum ocj_admission { OCJ_ADMIT_BOOT=1, OCJ_ADMIT_PROVISION=2,
    OCJ_ADMIT_RECYCLE=3, OCJ_ADMIT_MAP=4, OCJ_ADMIT_ACTIVE=5 };
enum ocj_io_rc { OCJ_IO_OK=0, OCJ_IO_CHECKED_RAW=1, OCJ_IO_MEDIA=2,
    OCJ_IO_ERROR=-1, OCJ_IO_UNCERTAIN=-2 };
struct ocj_block { uint64_t base, valid, range_sequence; uint32_t consumed; };
struct ocj_snapshot {
    uint64_t epoch, previous_epoch, high_water;
    uint8_t previous_digest[32], digest[32];
    uint32_t state, bank, slot, retired, quarantine, open;
    uint32_t pending_kind, pending_slot;
    uint64_t pending_lease;
    struct ocj_block blocks[32];
};
struct ocj_io {
    void *user;
    uint64_t (*now_ms)(void *);
    /* Must validate exact trusted identity/ownership+power+exclusive HAL state.
     * RECYCLE explicitly admits the CONDITIONAL interrupted-disposable-bank
     * re-erase policy; an unimplemented/unknown policy returns failure.
     * ACTIVE validates the application's synced root, not just these bytes. */
    int (*admit)(void *,uint32_t reason,const uint8_t descriptor_digest[32],
                  uint32_t physical_block,uint64_t epoch,uint64_t deadline);
    /* Successful read fills4096 immutable bytes. CHECKED_RAW is explicitly a
     * safely completed raw fallback with restored configuration, never an
     * arbitrary IO-error conversion. Canonical full-record integrity still
     * decides validity. Read/transport uncertainty fences the whole boot. */
    int (*read)(void *,uint32_t row,uint8_t main[4096],uint64_t deadline);
    int (*raw)(void *,uint32_t row,uint8_t raw[4352],uint64_t deadline);
    int (*erase)(void *,uint32_t block,uint64_t deadline);
    int (*program)(void *,uint32_t row,const uint8_t main[4096],uint64_t deadline);
    /* A known control P/E failure cannot always be recorded on the remaining
     * bank without risking its sole root. This independent durable fail latch
     * is mandatory, checked at every boot; it must not borrow an unowned bank.
     * Hardware integration must implement it or refuse write admission.
     * It records only metadata and never grants recycle authority. */
    int (*fault_latched)(void *);
    int (*latch_fault)(void *,uint32_t control_bank,uint64_t deadline);
};
struct owned_control_journal {
    uint32_t initialized,busy,fault,retained,booted,reconciled,tail_fresh,next_slot;
    uint32_t fresh_map, map_inflight;
    uint64_t deadline,last_now;
    struct owned_volume_decoded volume;
    struct owned_page_hash hash;
    struct ocj_io io;
    struct ocj_snapshot current,candidate,first[2],last[2];
    struct ocj_block working[32];
    struct owned_dhara_lease inflight;
    _Alignas(4) uint8_t tx[4096],rx[4096],raw[4352];
};
int ocj_init(struct owned_control_journal *,const uint8_t descriptor[512],
    const struct owned_volume_spec *,const struct owned_page_hash *,const struct ocj_io *);
int ocj_boot(struct owned_control_journal *,uint64_t deadline); /* Read-only,128 fixed pages. */
int ocj_provision(struct owned_control_journal *,uint64_t deadline); /* Explicit two-bank format only. */
int ocj_reconcile(struct owned_control_journal *,uint64_t deadline); /* Publishes abandoned tails. */
int ocj_activate(struct owned_control_journal *,uint64_t deadline);
int ocj_capability(struct owned_control_journal *,struct owned_dhara_capability *);
/* These policy callbacks plug into OD_RESERVED_RANGE. They do not implement or
 * select physical map IO. Caller bridge supplies same journal, exclusive owner
 * and shared absolute deadline. Only history_sync makes verified page history
 * durable after real Dhara sync; per-page resolve is intentionally volatile. */
int ocj_validate_capability(void *,const struct owned_dhara_capability *,uint64_t);
int ocj_page_info(void *,uint32_t,struct owned_dhara_page_info *,uint64_t);
int ocj_acquire(void *,uint32_t,uint32_t,uint32_t,struct owned_dhara_lease *,uint64_t);
int ocj_resolve(void *,const struct owned_dhara_lease *,uint64_t);
int ocj_retire(void *,uint32_t,uint32_t,uint32_t,const struct owned_dhara_lease *,uint64_t);
int ocj_history_sync(void *,uint64_t);
/* Canonical codec exported for offline independent vectors; decoding is not
 * activation. Return0 valid,1 torn/non-v2,negative sealed contradiction/error. */
int ocj_encode(const struct owned_volume_decoded *,const struct ocj_snapshot *,
               const struct owned_page_hash *,uint8_t out[4096]);
int ocj_decode(const struct owned_volume_decoded *,uint32_t bank,uint32_t slot,
               const struct owned_page_hash *,const uint8_t page[4096],struct ocj_snapshot *);
#endif
