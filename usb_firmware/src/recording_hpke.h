#ifndef OPENPENDANT_RECORDING_HPKE_H
#define OPENPENDANT_RECORDING_HPKE_H

#include <stddef.h>
#include <stdint.h>

/* Dormant, unlinked RFC 9180 single-shot sender. This is not a complete
 * recording-storage service or a reviewed target cryptographic provider.
 * Suite: mode=0, DHKEM(P-256, HKDF-SHA256)=0x0010, HKDF-SHA256=0x0001,
 * AES-256-GCM=0x0002. Exactly one Seal, sequence zero, per invocation.
 */
#define RECORDING_HPKE_PUBLIC_BYTES 65u
#define RECORDING_HPKE_TAG_BYTES 16u
#define RECORDING_HPKE_CONTEXT_MAX 256u
#define RECORDING_HPKE_PLAINTEXT_MAX 65536u
#define RECORDING_HPKE_OK 0
#define RECORDING_HPKE_INVALID (-1)
#define RECORDING_HPKE_PROVIDER_FAILED (-2)

struct recording_hpke_provider {
    void *user;
    /* Trusted provider must validate the uncompressed P-256 recipient point,
     * generate a fresh ephemeral private key from a failure-reporting CSPRNG,
     * validate ECDH, emit SEC1 enc=04||X||Y and fixed-width big-endian DH X,
     * and destroy the private key/handle on EVERY return path. No deterministic
     * owner keys, cached ephemeral key, fallback RNG, retry, or persistence.
     * Test-only deterministic private keys belong exclusively to host fixtures.
     */
    int (*encap_dh)(void *user, const uint8_t *recipient, size_t recipient_len,
                    uint8_t *enc, size_t enc_capacity, size_t *enc_len,
                    uint8_t *dh, size_t dh_capacity, size_t *dh_len);
    /* Actual HMAC-SHA256, not plain SHA256. Inputs may have zero length but
     * are passed as non-NULL spans by the wrapper. Outputs must be exactly 32B.
     */
    int (*hmac_sha256)(void *user, const uint8_t *key, size_t key_len,
                       const uint8_t *data, size_t data_len,
                       uint8_t *out, size_t capacity, size_t *out_len);
    /* Actual AES-256-GCM with key32, nonce12, tag16 appended to ciphertext.
     * Provider must not export secret diagnostics or use output before success.
     */
    int (*aes256gcm_seal)(void *user, const uint8_t *key, size_t key_len,
                         const uint8_t *nonce, size_t nonce_len,
                         const uint8_t *aad, size_t aad_len,
                         const uint8_t *plaintext, size_t plaintext_len,
                         uint8_t *ciphertext, size_t capacity, size_t *out_len);
};

/* No I/O, heap, static mutable state, locks or target provider in this module.
 * Caller exclusively owns all valid spans for the entire synchronous call.
 * All spans (including provider, length result and outputs) must not overlap.
 * Overlap/address-wrap is rejected BEFORE any writes, with everything untouched.
 * NULL input is allowed only for zero-length info/aad/plaintext. enc capacity
 * MUST be65, ciphertext capacity MUST be plaintext_len+16 and <=65552.
 * On other failure: out_len=0, valid bounded output spans and all local secret
 * scratch are wiped. Invalid pointers or lying capacities are caller bugs.
 * Inputs are not wiped; caller must wipe encoded/plaintext input after use.
 * Never resume a sender context after reboot or re-encrypt under reused
 * ephemeral randomness. Re-send already sealed bytes instead of re-sealing.
 * One shot avoids persisted AEAD counters, but does not cure bad randomness.
 * HPKE base does NOT authenticate sender identity, stop replay, hide lengths,
 * or provide forward secrecy after recipient private-key compromise.
 */
int recording_hpke_seal(const struct recording_hpke_provider *provider,
                        const uint8_t *recipient, size_t recipient_len,
                        const uint8_t *info, size_t info_len,
                        const uint8_t *aad, size_t aad_len,
                        const uint8_t *plaintext, size_t plaintext_len,
                        uint8_t *enc, size_t enc_capacity,
                        uint8_t *ciphertext, size_t ciphertext_capacity,
                        size_t *out_len);

/* Explicit exact-alias variant for a provider qualified for in-place AEAD.
 * buffer holds plaintext on entry, ciphertext+tag only on success. It MUST have
 * exactly plaintext_len+16 bytes, all exclusively owned throughout the call.
 * All OTHER spans remain disjoint; original seal still rejects all overlap.
 * After an admitted failure buffer (including plaintext) is wiped. */
int recording_hpke_seal_in_place(const struct recording_hpke_provider*,
 const uint8_t*,size_t,const uint8_t*,size_t,const uint8_t*,size_t,
 uint8_t *buffer,size_t plaintext_len,size_t capacity,uint8_t *enc,size_t enc_capacity,size_t *out_len);

#endif
