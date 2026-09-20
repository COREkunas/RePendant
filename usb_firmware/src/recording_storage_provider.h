/* SPDX-License-Identifier: Apache-2.0
 * UNLINKED integration. No physical pool/authority, keys or settings are chosen.
 */
#ifndef OPENPENDANT_RECORDING_STORAGE_PROVIDER_H
#define OPENPENDANT_RECORDING_STORAGE_PROVIDER_H
#include "owned_control_journal.h"
#include "nand_owned_phy.h"
enum rsp_result { RSP_OK=0,RSP_MISSING=1,RSP_NO_ROOT=2,RSP_ARGUMENT=-1,
    RSP_REFUSED=-2,RSP_FAULT=-3,RSP_BUSY=-4 };
struct rsp_hal {
    void *user;
    uint64_t (*now_ms)(void *);
    void (*wait_us)(void *,uint32_t);
    int (*open)(void *,uint64_t);
    int (*transfer)(void *,const uint8_t *,uint8_t *,uint32_t,uint32_t,
                    uint64_t,struct nop_transfer_result *);
    int (*close)(void *,uint64_t);
};
struct rsp_policy {
    void *user;
    /* Full trusted ownership/history validator, not a caller boolean. Reasons
     * are OCJ_ADMIT_*. PROVISION must prove all34 members fully preserved and
     * deliberately disposable; ACTIVE must prove the synced application root.
     * RECYCLE admits the documented target-only conditional re-erase policy.
     * READ/BOOT must reconcile historical PHY P/E flags without clearing them.
     * UINT32_MAX means no single target. Descriptor/history remain immutable. */
    int (*admit)(void *,uint32_t,const struct owned_volume_decoded *,uint64_t,
                 uint32_t,uint32_t historical_media_flags,uint64_t);
    /* Fresh trusted power-policy decision at EACH PHY authorization, including
     * before WREN and again before PROGRAM EXECUTE/D8. Unknown denies. An initial
     * USB-powered-only engineering policy is permissible if clearly labelled;
     * it is not a fabricated calibrated rail/brownout measurement. */
    int (*power)(void *,uint32_t access,uint32_t row,uint64_t);
    /* Independent durable control-media failure metadata. Must reconcile the
     * failure-to-latch cut window in the real implementation; not provided here.
     * Any absent/unknown implementation must refuse via admit/fault_latched. */
    int (*fault_latched)(void *,const uint8_t descriptor_digest[32],uint64_t);
    int (*latch_fault)(void *,const uint8_t descriptor_digest[32],uint32_t bank,uint64_t);
    /* Bounded cooperative scheduling point. Must return within original job
     * deadline and must never reenter this provider. Target can yield to PDM. */
    int (*yield)(void *,uint64_t);
};
/* Private live coordinator proof, not a host boolean. PREIMAGE=1, MUTATION=2;
 * proves exclusive owner, consumed authorizer, immutable cfg/proof and phase. */
struct rsp_recovery_owner { void *user; int (*admit)(void *,uint32_t,uint64_t); };
struct rsp_first_fault {
 uint32_t valid,stage,route,row,opcode,bytes,fast;
 int32_t transfer_rc;
 uint64_t observed_ms,deadline,epoch,high_water,programs,erases,transfer_before_ms,transfer_after_ms;
 uint32_t phy_fault,stopped,ready,a0,b0,c0,media_flags,journal_fault,map_fault;
 struct nop_transfer_result transfer;
};
struct recording_storage_provider {
    atomic_uint gate;
    uint32_t magic,fault,attempted,ready,route,route_row,route_control,suspended;
    uint32_t erase_scan_row,erase_scan_remaining;
    uint64_t deadline,last_now,verified_programs,verified_erases;
    struct rsp_hal hal;
    struct rsp_policy policy;
    struct owned_control_journal journal;
    struct owned_dhara map;
    struct nand_owned_phy phy;
    _Alignas(4) uint8_t verify[4096],raw[4352];
    /* Isolated recovery-only CPU scratch, never a DMA target. */
    uint8_t recovery_hash_input[68];
    uint32_t recovery_validated,recovery_commit_attempted,recovery_stage;
    uint64_t observed_ms,recovery_preimage_deadline,last_transfer_before,last_transfer_after;
    uint8_t recovery_control[32];
    struct rsp_recovery_owner recovery_owner;
    uint32_t last_opcode,last_bytes,last_fast;
    int32_t last_transfer_rc;
    struct nop_transfer_result last_transfer;
    struct rsp_first_fault first_fault;
};
/* Caller initializes storage to zero once. Never move/reinitialize after use or
 * fault: unresolved DMA is retained only in embedded PHY buffers. No heap. */
int rsp_init(struct recording_storage_provider *,const uint8_t descriptor[512],
    const struct owned_volume_spec *,const struct owned_page_hash *,
    const struct rsp_hal *,const struct rsp_policy *);
int rsp_resume(struct recording_storage_provider *,uint64_t); /* Read-only, never format. */
int rsp_provision(struct recording_storage_provider *,uint64_t); /* Explicit first call only. */
/* Candidate only: full read-only root/preimage validation precedes reconcile
 * and fresh-lease formatting. NEVER calls ocj_provision or resets history. */
int rsp_recovery_prepare(struct recording_storage_provider *,const struct rsp_recovery_owner *,const uint8_t preimage[32],
 const uint8_t control_digest[32],const uint8_t capture_firmware_digest[32],
 uint64_t epoch,uint64_t high_water,uint64_t deadline);
/* Completed prepare + same owner's MUTATION proof required. One attempt only;
 * exact canonical state/STOP/quiescence checked again, never resume/retry. */
int rsp_recovery_commit(struct recording_storage_provider *,uint64_t deadline);
int rsp_recovery_quiescent(const struct recording_storage_provider *);
int rsp_grant(struct recording_storage_provider *,uint64_t); /* Reconcile then validated ACTIVE. */
int rsp_activate(struct recording_storage_provider *,uint64_t); /* Synced application proof. */
int rsp_read(struct recording_storage_provider *,uint32_t sector,uint8_t out[2048],uint64_t);
int rsp_write(struct recording_storage_provider *,uint32_t sector,const uint8_t in[2048],uint64_t);
int rsp_sync(struct recording_storage_provider *,uint64_t);
int rsp_capacity(struct recording_storage_provider *,uint32_t *);
int rsp_close(struct recording_storage_provider *,uint64_t);
/* Same-boot session pause only: sync all writable map/history first, revoke,
 * close verified PHY and release owner. Reopen retains mounted metadata but
 * gives READ-ONLY access; a separate rsp_grant must validate write authority.
 * Neither function reinitializes state, rescans, formats or repairs faults. */
int rsp_suspend(struct recording_storage_provider *,uint64_t);
int rsp_reopen(struct recording_storage_provider *,uint64_t);
#endif
