/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OPENPENDANT_RECORDING_CONFIGURATION_H
#define OPENPENDANT_RECORDING_CONFIGURATION_H
#include "owned_volume_descriptor.h"
#include "recording_extent.h"

/* Application-owned metadata in the EXISTING settings partition, never raw
 * flash addresses or a private recording key. Explicit init after settings/PSA
 * are ready. A missing configuration does not provision or start anything.
 * The preparer must independently verify the named preservation manifest and
 * the phone's backup-verified PUBLIC recipient. The device validates identity,
 * canonical descriptor, fixed pool and P-256 point, not host backup file bytes.
 * Explicit full-volume key reset is the sole recipient replacement API.
 * Failed saves/readbacks fence this boot.
 * Schema v1 revision is exactly phase + count(faulted control banks).
 * EMPTY means no leaf delivered by the settings API, not proof that storage
 * was never provisioned: the NVS backend may skip/clean damaged entries.
 * Init/load is not a physical read-only guarantee. EMPTY itself grants no
 * NAND write, replacement, erase, format or automatic enrollment authority.
 */
enum rcfg_phase { RCFG_EMPTY=0,RCFG_PREPARED=1,RCFG_PROVISIONING=2,RCFG_ACTIVE=3 };
struct recording_configuration {
    struct owned_volume_spec spec;
    uint8_t descriptor[512],recipient[65];
    uint64_t revision;
    uint32_t phase,fault_banks;
};
int rcfg_init(void); /* 0 present,1 empty,negative error */
int rcfg_get(struct recording_configuration*);
int rcfg_device_id(uint8_t out[16]);
int rcfg_prepare(const struct owned_volume_spec*,const uint8_t recipient[65]);
/* Explicit one-way replacement of ACTIVE legacy generation1 by a fresh full
 * volume generation2. Same device and public recipient; no key/bond changes.
 * Caller must have joined ALL storage actors and prevent future old-volume
 * access, then reboot before binding the peripheral to this new descriptor.
 * Persists PREPARED only; never erases NAND or automatically provisions.
 * Schema2 replaces the SAME settings leaf so old firmware fails closed. */
int rcfg_prepare_full(const uint8_t expected_legacy_digest[32],const uint8_t new_volume[16]);
/* Destructive NEW-key intent, not an erase. Requires exact ACTIVE full parent,
 * fresh volume UUID (also transaction ID), different verified public recipient.
 * One atomic settings leaf retains parent public identity and PREPARED child.
 * All old storage actors must be unused; reboot before binding the child.
 * Schema3 fails closed on older firmware. Phone backups/copies are not targets. */
int rcfg_prepare_key_reset(const uint8_t expected_digest[32],const uint8_t new_volume[16],
 const uint8_t fingerprint[32],const uint8_t recipient[65]);
int rcfg_is_key_reset(const struct recording_configuration*);
int rcfg_is_full(const struct recording_configuration*); /* trusted rcfg_get result only */
const uint8_t *rcfg_descriptor_digest(const struct recording_configuration*);
void rcfg_extent_identity(const struct recording_configuration*,struct recording_extent_identity*);
int rcfg_advance(uint32_t expected_phase,uint32_t next_phase);
int rcfg_latch_fault(const uint8_t descriptor_digest[32],uint32_t control_bank);
int rcfg_faulted(const uint8_t descriptor_digest[32]);
#endif
