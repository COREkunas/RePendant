#ifndef OPENPENDANT_RECORDING_HPKE_PSA_H
#define OPENPENDANT_RECORDING_HPKE_PSA_H

#include "recording_hpke.h"

/* Dormant/unlinked adapter, not a recording service or enrollment mechanism.
 * The application must own exactly ONE long-lived context for all HPKE jobs.
 * Initialize once from INIT; NEVER copy, reset, zero, or replace it to bypass a
 * failure fence. Externally serialize the COMPLETE recording_hpke_seal call,
 * not just individual callbacks. This module has no locks and is not ISR-safe.
 * Use recording_hpke_psa_seal for complete jobs: it admits all spans against
 * the FULL context and reserves the owner across the complete generic sender.
 * Calling the generic sender directly with a borrowed provider bypasses this
 * whole-owner boundary; callback checks cannot repair an earlier aliased write.
 * No other user may use its temporary PSA handles. A failed key destruction
 * permanently fences this context: stop encryption until a reviewed reboot/
 * recovery boundary; allocating a fresh context is NOT recovery.
 *
 * Only volatile local PSA keys are used; no private export, persistent key IDs,
 * cached ephemeral key, logs, application heap, fallback RNG or retry exists.
 * PSA owns transient key storage and may allocate internally. PSA generation
 * uses the configured failure-reporting CSPRNG, which MUST be audited/pinned.
 * PSA's internal scalar rejection sampling and synchronous provider calls have
 * no caller cancellation hook; this is NOT a hard runtime/stack bound.
 * No deterministic key/RNG hook exists in production. Host shims are separate.
 */
struct recording_hpke_psa_context {
    uint32_t state; /* Private state; no key material. Do not modify directly. */
    struct recording_hpke_provider provider;
};

#define RECORDING_HPKE_PSA_CONTEXT_INIT { 0u, { 0 } }

/* Calls psa_crypto_init once for this owner, never frees/resets global PSA.
 * Invalid/repeated init is refused. Init failure is also permanently fenced.
 */
int recording_hpke_psa_init(struct recording_hpke_psa_context *context);

/* Preferred complete-job entry. No automatic init, retry or owner reset.
 * Null/shape-invalid, overlapping/address-wrapped arguments are rejected with
 * INVALID before ANY output/result/owner write. Not-ready/faulted/reentrant
 * owners are rejected with PROVIDER_FAILED, also entirely untouched. NULL
 * info/aad/plaintext is permitted only with zero length, as in the sender.
 * After admission, sender/provider failures wipe bounded outputs and out_len=0.
 * A cleanup fault stays fenced; ordinary cleanly-ended errors release the job.
 * Valid pointers and truthful capacities remain trusted caller obligations.
 */
int recording_hpke_psa_seal(struct recording_hpke_psa_context *context,
    const uint8_t *recipient, size_t recipient_len,
    const uint8_t *info, size_t info_len,
    const uint8_t *aad, size_t aad_len,
    const uint8_t *plaintext, size_t plaintext_len,
    uint8_t *enc, size_t enc_capacity,
    uint8_t *ciphertext, size_t ciphertext_capacity, size_t *out_len);

/* Borrowed view valid for the owner's entire lifetime; NULL unless ready.
 * A previously borrowed view still refuses every callback after a fence.
 */
const struct recording_hpke_provider *recording_hpke_psa_provider(
    const struct recording_hpke_psa_context *context);

/* Metadata only: 1 after init/cleanup failure, 0 otherwise. */
int recording_hpke_psa_faulted(const struct recording_hpke_psa_context *context);

/* Explicit exact in-place job, same persistent owner and failure fences.
 * PSA Crypto specifies input/output buffer overlap (library conventions5.4.4).
 * No other overlaps are accepted, and the original entry remains disjoint-only.
 * Validate the linked target provider with a public-vector test before use. */
int recording_hpke_psa_seal_in_place(struct recording_hpke_psa_context*,
 const uint8_t*,size_t,const uint8_t*,size_t,const uint8_t*,size_t,
 uint8_t*,size_t,size_t,uint8_t*,size_t,size_t*);

#endif
