/* SPDX-License-Identifier: Apache-2.0 -- explicit target coordinator, UNLINKED. */
#ifndef OPENPENDANT_RECORDING_VOLUME_H
#define OPENPENDANT_RECORDING_VOLUME_H
#include "recording_configuration.h"
#include "recording_storage_provider.h"
#include "recording_sync_metadata.h"
#include "recording_object_store.h"
#include "recording_hpke_psa.h"
#include "nand_owned_phy_nrf.h"
#ifdef OPENPENDANT_NATIVE_STORAGE
#include "recording_native_store.h"
#endif
#define RV_CAPACITY 418U
#define RV_ROS_CAPACITY 415U
#define RV_RECOVERY_PARENT_MS 240000U
#define RV_RECOVERY_PREIMAGE_MS 90000U
#define RV_RECOVERY_MUTATION_MS 120000U
struct rv_recovery_first_fault {
 uint32_t valid,stage,phase,leased,stopped,ready,provider_fault,journal_fault,map_fault,phy_fault;
 int32_t rc;
 uint64_t observed_ms,start_ms,parent_deadline,preimage_deadline,mutation_deadline,revision,epoch,high_water;
 uint32_t starts,a0,b0,c0,media_flags;
 struct rsp_first_fault provider;
};
enum rv_rc { RV_OK=0,RV_EMPTY=1,RV_INCOMPLETE=2,RV_ARGUMENT=-1,RV_BUSY=-2,RV_REFUSED=-3,RV_FAULT=-4 };
enum rv_state { RV_UNINITIALIZED=0,RV_PREPARED=1,RV_CONFIGURED=2,RV_PROVISIONING=3,
 RV_MOUNTING=4,RV_READ_ONLY=5,RV_WRITABLE=6,RV_FENCED=7,RV_CLOSED=8,RV_SUSPENDED=9,
 RV_RECOVERY_PREPARED=10 };
/* ISOLATED OFFLINE CANDIDATE. This is not an existing preservation manifest.
 * Config digest and current-pool commitment formats are specified in README.
 * Only the known phase2/revision2 epoch5/high-water130 shape is admitted.
 * Immutable expected values must come from independently verified evidence;
 * NEVER derive authorization by accepting freshly observed device bytes. */
struct rv_recovery_proof {
 uint8_t transaction[16],configuration_sha256[32],preimage_sha256[32],control_digest[32];
 uint8_t capture_firmware_digest[32];
 uint64_t control_epoch,lease_high_water;
};
struct rv_recovery_authorizer {
 void *user;
 /* Must atomically consume a NEW durable one-use transaction, verify both
  * original and current backup copies, exact scope, failure diagnosis and
  * explicit authority BEFORE returning0. Ambiguous result remains consumed.
  * Must refuse repeated calls across resets. This candidate supplies no
  * persistent implementation and cannot verify host private backup files. */
 int (*consume)(void *,const struct recording_configuration *,const struct rv_recovery_proof *,uint64_t);
};
struct rv_hooks {
 void *user;
 /* Root-owned composite storage/capture reservation. Called ONLY by nRF open/
  * close, not reacquired by individual ROS/RSM jobs. Failure must prove no
  * acquisition; an ambiguous/late return fences. All callbacks are synchronous. */
 int (*acquire)(void *,uint64_t);
 int (*release)(void *,uint64_t);
 /* Current serialized worker/session, lifecycle/cancel and independent fatal
  * supervisor admission; writing is0/1. No new capture or authority here. */
 int (*check)(void *,int writing,uint64_t);
 /* Explicit USB-only engineering policy, repeated for every PHY mutation.
  * This is NOT a calibrated live rail or arbitrary-brownout guarantee. */
 int (*usb_power)(void *,uint32_t access,uint32_t physical_row,uint64_t);
 int (*yield)(void *,uint64_t);
 /* Validates explicit host confirmation + fully preserved exact pool/history.
  * Digest equality alone is not confirmation or backup-file verification. */
 int (*confirm_provision)(void *,const struct recording_configuration *,const uint8_t confirmation[32],uint64_t);
 /* Trusted immutable resolvers; same volume/session as the mounted store.
  * They must not reenter RSM, or retain any caller span after return. */
 int (*approve_receipt)(void *,const struct rsm_volume *,const struct rsm_receipt *,uint64_t);
 int (*approve_delete)(void *,const struct rsm_volume *,const uint8_t operation[16],const struct rsm_terminal *,uint64_t);
};
/* Exactly one permanent zero-initialized target instance. No moving, resetting
 * or reinitializing after any attempt/fault. Root supplies serialized workers,
 * bounded original deadlines, independent fatal supervision and adequate RAM/
 * stacks. Context fields are allocation detail, NEVER direct access authority. */
struct recording_volume {
 atomic_uint gate;
 uint32_t magic,state,fault,attempted,leased,confirmed,root_valid,ros_valid;
 uint64_t deadline,last_now;
 struct rv_hooks hooks;
 struct recording_configuration configuration;
 struct owned_page_hash hash;
 struct nop_port target;
 struct recording_hpke_psa_context crypto;
#ifdef OPENPENDANT_NATIVE_STORAGE
 /* The native profile must not reserve RAM for the retired legacy provider.
  * Its NAND-recovery command is disabled; signed USB boot recovery is separate
  * and unaffected. Larger indexes need this reclaimed permanent allocation. */
 struct recording_native_store native;
#else
 struct recording_storage_provider storage;
#endif
 struct recording_sync_metadata metadata;
 struct ros_context store;
 struct rsm_root_proof root;
 struct es_binding binding;
 uint8_t confirmation[32];
    struct rv_recovery_proof recovery;
    struct rv_recovery_proof recovery_authorized;
    uint32_t recovery_authorized_valid;
    struct rv_recovery_authorizer recovery_authorizer;
    uint32_t recovery_active,recovery_phase,recovery_stage,recovery_transitions;
    int32_t recovery_rc;
    uint64_t observed_ms,recovery_start,recovery_parent,recovery_preimage,recovery_mutation,recovery_transition_ms;
    struct rv_recovery_first_fault recovery_first_fault;
};
/* Call only after settings, rcfg_init and PSA subsystem readiness. Binds the
 * actual singleton nRF HAL but does not open hardware. Partial provisioning
 * returns INCOMPLETE without binding, formatting or recovery repair. */
int rv_init(struct recording_volume *,const struct rv_hooks *);
int rv_recovery_init(struct recording_volume *,const struct rv_hooks *,
 const struct rv_recovery_authorizer *,const struct rv_recovery_proof *);
int rv_recovery_run(struct recording_volume *,uint64_t deadline);
int rv_confirmation(struct recording_volume *,uint8_t out[32]); /* public bytes, not authority */
int rv_provision(struct recording_volume *,const uint8_t confirmation[32],uint64_t deadline);
int rv_mount(struct recording_volume *,uint64_t deadline); /* ACTIVE config, read-only */
int rv_grant(struct recording_volume *,uint64_t deadline); /* explicit writable reconcile */
#ifdef OPENPENDANT_NATIVE_STORAGE
int rv_extend_metadata(struct recording_volume *,uint64_t deadline);
/* Explicit one-shot full-chip format from PREPARED schema2 only. Fixed <=1h
 * parent, checked incremental erase steps, root and ACTIVE published last.
 * Phase2 after reset is intentionally not retried by this API. */
int rv_provision_full(struct recording_volume *,const uint8_t confirmation[32],uint64_t deadline);
/* Explicit phase2 completion; never reformats. Requires valid native root,
 * and either complete RSM or wholly absent RSM. Partial metadata refuses. */
int rv_complete_full(struct recording_volume *,const uint8_t confirmation[32],uint64_t deadline);
#endif
int rv_close(struct recording_volume *,uint64_t deadline);
int rv_suspend(struct recording_volume *,uint64_t deadline); /* sync, revoke, release */
int rv_reopen(struct recording_volume *,uint64_t deadline); /* same-boot read-only, no rescan */
/* Returns only validated mounted contexts and trusted volume binding. The root
 * owner must serialize ALL subsequent ROS/RSM/bridge actors; no borrowed DMA or
 * callback pointer retention. Read-only mount never authorizes writing. Faulted
 * contexts are retained, not reset/released/reused. */
int rv_access(struct recording_volume *,struct ros_context **,struct recording_sync_metadata **,struct es_binding *);
#endif
