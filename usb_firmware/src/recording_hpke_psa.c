#include "recording_hpke_psa.h"
#include <psa/crypto.h>

#define OWNER_READY 0x48505231u
#define OWNER_BUSY 0x48504231u
#define OWNER_FAULT 0x48504631u
#define OWNER_JOB_READY 0x48504A31u
#define OWNER_JOB_BUSY 0x48504A32u
#define OWNER_INPLACE_READY 0x48504931u
#define OWNER_INPLACE_BUSY 0x48504932u

static void wipe(void *buffer, size_t bytes)
{
    volatile uint8_t *p = (volatile uint8_t *)buffer;
    while (bytes != 0u) { *p++ = 0; --bytes; }
}

struct span { const void *pointer; size_t bytes; };

/* Match the sender's no-write rejection for aliasing/address overflow. Include
 * the owner because provider.user is otherwise outside the sender span list.
 */
static int disjoint(const struct span *spans, size_t count)
{
    size_t i, j;
    for (i = 0; i < count; ++i) {
        uintptr_t first = (uintptr_t)spans[i].pointer, last;
        if (first == 0u || spans[i].bytes == 0u) { continue; }
        if (spans[i].bytes - 1u > UINTPTR_MAX - first) { return 0; }
        last = first + spans[i].bytes - 1u;
        for (j = 0; j < i; ++j) {
            uintptr_t other = (uintptr_t)spans[j].pointer;
            if (other != 0u && spans[j].bytes != 0u &&
                first <= other + spans[j].bytes - 1u && other <= last) { return 0; }
        }
    }
    return 1;
}

static int begin(struct recording_hpke_psa_context *context)
{
    if (context == NULL) { return 0; }
    if (context->state == OWNER_READY) { context->state = OWNER_BUSY; }
    else if (context->state == OWNER_JOB_READY) { context->state = OWNER_JOB_BUSY; }
    else if (context->state == OWNER_INPLACE_READY) { context->state = OWNER_INPLACE_BUSY; }
    else { return 0; }
    return 1;
}

static void finish(struct recording_hpke_psa_context *context)
{
    if (context != NULL && context->state == OWNER_BUSY) { context->state = OWNER_READY; }
    else if (context != NULL && context->state == OWNER_JOB_BUSY) { context->state = OWNER_JOB_READY; }
    else if (context != NULL && context->state == OWNER_INPLACE_BUSY) { context->state = OWNER_INPLACE_READY; }
}

/* Attempt each destruction exactly once. Clear our handle before the call so
 * a later cleanup path cannot retry an uncertain operation. A non-success is
 * never treated as successful erasure, even if the provider removed the key.
 */
static int destroy(struct recording_hpke_psa_context *context, mbedtls_svc_key_id_t *key)
{
    mbedtls_svc_key_id_t current = *key;
    *key = MBEDTLS_SVC_KEY_ID_INIT;
    if (mbedtls_svc_key_id_is_null(current)) { return 1; }
    if (psa_destroy_key(current) != PSA_SUCCESS) {
        context->state = OWNER_FAULT;
        return 0;
    }
    return 1;
}

static int encap(void *user, const uint8_t *recipient, size_t recipient_len,
                 uint8_t *enc, size_t enc_capacity, size_t *enc_len,
                 uint8_t *dh, size_t dh_capacity, size_t *dh_len)
{
    struct recording_hpke_psa_context *context = user;
    const struct span spans[] = {
        { context, sizeof(*context) }, { recipient, recipient_len },
        { enc, enc_capacity }, { enc_len, sizeof(*enc_len) },
        { dh, dh_capacity }, { dh_len, sizeof(*dh_len) }
    };
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    mbedtls_svc_key_id_t public_key = MBEDTLS_SVC_KEY_ID_INIT;
    mbedtls_svc_key_id_t ephemeral = MBEDTLS_SVC_KEY_ID_INIT;
    size_t actual_enc = 0, actual_dh = 0, i;
    uint8_t nonzero = 0;
    int rc = RECORDING_HPKE_INVALID, entered = 0;
    if (!disjoint(spans, sizeof(spans) / sizeof(spans[0]))) { return rc; }
    if (enc_len != NULL) { *enc_len = 0; }
    if (dh_len != NULL) { *dh_len = 0; }
    if (recipient == NULL || recipient_len != 65u || recipient[0] != 4u ||
        enc == NULL || enc_capacity != 65u || enc_len == NULL ||
        dh == NULL || dh_capacity != 32u || dh_len == NULL) { goto done; }
    rc = RECORDING_HPKE_PROVIDER_FAILED;
    if (!begin(context)) { goto done; }
    entered = 1;

    /* The import validates the full point/field/curve, not just SEC1 prefix.
     * Public object has no operation/export policy and is discarded first.
     */
    psa_set_key_lifetime(&attributes, PSA_KEY_LIFETIME_VOLATILE);
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, 256u);
    if (psa_import_key(&attributes, recipient, recipient_len, &public_key) != PSA_SUCCESS) { goto done; }
    if (!destroy(context, &public_key)) { goto done; }
    psa_reset_key_attributes(&attributes);

    psa_set_key_lifetime(&attributes, PSA_KEY_LIFETIME_VOLATILE);
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, 256u);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DERIVE);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDH);
    if (psa_generate_key(&attributes, &ephemeral) != PSA_SUCCESS) { goto done; }
    /* Public export needs no PSA_KEY_USAGE_EXPORT; private export is forbidden. */
    if (psa_export_public_key(ephemeral, enc, enc_capacity, &actual_enc) != PSA_SUCCESS ||
        actual_enc != 65u || enc[0] != 4u) { goto done; }
    if (psa_raw_key_agreement(PSA_ALG_ECDH, ephemeral, recipient, recipient_len,
                              dh, dh_capacity, &actual_dh) != PSA_SUCCESS || actual_dh != 32u) { goto done; }
    for (i = 0; i < 32u; ++i) { nonzero |= dh[i]; }
    if (nonzero == 0u) { goto done; }
    rc = RECORDING_HPKE_OK;
done:
    if (entered) {
        if (!destroy(context, &public_key)) { rc = RECORDING_HPKE_PROVIDER_FAILED; }
        if (!destroy(context, &ephemeral)) { rc = RECORDING_HPKE_PROVIDER_FAILED; }
        finish(context);
    }
    psa_reset_key_attributes(&attributes);
    wipe(&nonzero, sizeof(nonzero));
    if (rc == RECORDING_HPKE_OK) { *enc_len = actual_enc; *dh_len = actual_dh; }
    else {
        if (enc != NULL && enc_capacity <= 65u) { wipe(enc, enc_capacity); }
        if (dh != NULL && dh_capacity <= 32u) { wipe(dh, dh_capacity); }
    }
    return rc;
}

static int mac(void *user, const uint8_t *key, size_t key_len,
               const uint8_t *data, size_t data_len,
               uint8_t *out, size_t capacity, size_t *out_len)
{
    struct recording_hpke_psa_context *context = user;
    const struct span spans[] = {
        { context, sizeof(*context) }, { key, key_len }, { data, data_len },
        { out, capacity }, { out_len, sizeof(*out_len) }
    };
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    mbedtls_svc_key_id_t imported = MBEDTLS_SVC_KEY_ID_INIT;
    uint8_t zero_salt[32] = { 0 };
    size_t actual = 0;
    int rc = RECORDING_HPKE_INVALID, entered = 0;
    if (!disjoint(spans, sizeof(spans) / sizeof(spans[0]))) { return rc; }
    if (out_len != NULL) { *out_len = 0; }
    if (key == NULL || (key_len != 0u && key_len != 32u) || data == NULL || data_len > 320u ||
        out == NULL || capacity != 32u || out_len == NULL) { goto done; }
    rc = RECORDING_HPKE_PROVIDER_FAILED;
    if (!begin(context)) { goto done; }
    entered = 1;
    psa_set_key_lifetime(&attributes, PSA_KEY_LIFETIME_VOLATILE);
    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attributes, 256u);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    /* PSA rejects empty imported keys. Both empty HMAC key and 32 zero bytes
     * produce the same SHA256 block-padded key, as required by HKDF-Extract.
     */
    if (psa_import_key(&attributes, key_len == 0u ? zero_salt : key, 32u, &imported) != PSA_SUCCESS) { goto done; }
    if (psa_mac_compute(imported, PSA_ALG_HMAC(PSA_ALG_SHA_256), data, data_len,
                        out, capacity, &actual) != PSA_SUCCESS || actual != 32u) { goto done; }
    rc = RECORDING_HPKE_OK;
done:
    if (entered) {
        if (!destroy(context, &imported)) { rc = RECORDING_HPKE_PROVIDER_FAILED; }
        finish(context);
    }
    psa_reset_key_attributes(&attributes);
    wipe(zero_salt, sizeof(zero_salt));
    if (rc == RECORDING_HPKE_OK) { *out_len = actual; }
    else if (out != NULL && capacity <= 32u) { wipe(out, capacity); }
    return rc;
}

static int seal(void *user, const uint8_t *key, size_t key_len,
                const uint8_t *nonce, size_t nonce_len,
                const uint8_t *aad, size_t aad_len,
                const uint8_t *plaintext, size_t plaintext_len,
                uint8_t *out, size_t capacity, size_t *out_len)
{
    struct recording_hpke_psa_context *context = user;
    int in_place = context != NULL && context->state == OWNER_INPLACE_READY &&
                   plaintext == out && plaintext_len <= 65536u && capacity == plaintext_len+16u;
    const struct span spans[] = {
        { context, sizeof(*context) }, { key, key_len }, { nonce, nonce_len },
        { aad, aad_len }, { in_place ? NULL : plaintext, in_place ? 0u : plaintext_len },
        { out, capacity }, { out_len, sizeof(*out_len) }
    };
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    mbedtls_svc_key_id_t imported = MBEDTLS_SVC_KEY_ID_INIT;
    size_t actual = 0;
    int rc = RECORDING_HPKE_INVALID, entered = 0;
    if (!disjoint(spans, sizeof(spans) / sizeof(spans[0]))) { return rc; }
    if (out_len != NULL) { *out_len = 0; }
    if (key == NULL || key_len != 32u || nonce == NULL || nonce_len != 12u ||
        aad == NULL || aad_len > 256u || plaintext == NULL || plaintext_len > 65536u ||
        out == NULL || capacity != plaintext_len + 16u || out_len == NULL) { goto done; }
    rc = RECORDING_HPKE_PROVIDER_FAILED;
    if (!begin(context)) { goto done; }
    entered = 1;
    psa_set_key_lifetime(&attributes, PSA_KEY_LIFETIME_VOLATILE);
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, 256u);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_GCM); /* Full 16-byte tag. */
    if (psa_import_key(&attributes, key, key_len, &imported) != PSA_SUCCESS) { goto done; }
    if (psa_aead_encrypt(imported, PSA_ALG_GCM, nonce, nonce_len, aad, aad_len,
                         plaintext, plaintext_len, out, capacity, &actual) != PSA_SUCCESS ||
        actual != plaintext_len + 16u) { goto done; }
    rc = RECORDING_HPKE_OK;
done:
    if (entered) {
        if (!destroy(context, &imported)) { rc = RECORDING_HPKE_PROVIDER_FAILED; }
        finish(context);
    }
    psa_reset_key_attributes(&attributes);
    if (rc == RECORDING_HPKE_OK) { *out_len = actual; }
    else if (out != NULL && capacity <= 65552u) { wipe(out, capacity); }
    return rc;
}

int recording_hpke_psa_init(struct recording_hpke_psa_context *context)
{
    if (context == NULL || context->state != 0u) { return RECORDING_HPKE_INVALID; }
    context->state = OWNER_BUSY;
    if (psa_crypto_init() != PSA_SUCCESS) {
        context->state = OWNER_FAULT;
        return RECORDING_HPKE_PROVIDER_FAILED;
    }
    context->provider.user = context;
    context->provider.encap_dh = encap;
    context->provider.hmac_sha256 = mac;
    context->provider.aes256gcm_seal = seal;
    context->state = OWNER_READY;
    return RECORDING_HPKE_OK;
}

static int seal_job(struct recording_hpke_psa_context *context,
    const uint8_t *recipient, size_t recipient_len,
    const uint8_t *info, size_t info_len,
    const uint8_t *aad, size_t aad_len,
    const uint8_t *plaintext, size_t plaintext_len,
    uint8_t *enc, size_t enc_capacity,
    uint8_t *ciphertext, size_t ciphertext_capacity, size_t *out_len, int in_place)
{
    const struct span spans[] = {
        { context, sizeof(*context) }, { recipient, recipient_len },
        { info, info_len }, { aad, aad_len }, { in_place ? NULL : plaintext, in_place ? 0u : plaintext_len },
        { enc, enc_capacity }, { ciphertext, ciphertext_capacity },
        { out_len, sizeof(*out_len) }
    };
    int rc;
    /* In particular out_len must not alias context.state: the generic sender
     * writes it before callbacks and only knows the smaller provider subobject.
     */
    if (!disjoint(spans, sizeof(spans) / sizeof(spans[0]))) { return RECORDING_HPKE_INVALID; }
    if (context == NULL || recipient == NULL || recipient_len != 65u ||
        recipient[0] != 4u || info_len > 256u || aad_len > 256u ||
        plaintext_len > 65536u || (info == NULL && info_len != 0u) ||
        (aad == NULL && aad_len != 0u) || (plaintext == NULL && plaintext_len != 0u) ||
        enc == NULL || enc_capacity != 65u || ciphertext == NULL ||
        ciphertext_capacity != plaintext_len + 16u || out_len == NULL) { return RECORDING_HPKE_INVALID; }
    if (context->state != OWNER_READY || context->provider.user != context) {
        return RECORDING_HPKE_PROVIDER_FAILED;
    }
    uint32_t expected = in_place ? OWNER_INPLACE_READY : OWNER_JOB_READY;
    context->state = expected;
    if(in_place) rc = recording_hpke_seal_in_place(&context->provider,recipient,recipient_len,
        info,info_len,aad,aad_len,ciphertext,plaintext_len,ciphertext_capacity,enc,enc_capacity,out_len);
    else rc = recording_hpke_seal(&context->provider, recipient, recipient_len, info, info_len,
                            aad, aad_len, plaintext, plaintext_len, enc, enc_capacity,
                            ciphertext, ciphertext_capacity, out_len);
    if (context->state == expected) { context->state = OWNER_READY; }
    else {
        /* Unexpected owner state is never silently repaired. Also do not let a
         * future provider accidentally turn a latched cleanup fault into success.
         */
        context->state = OWNER_FAULT;
        *out_len = 0;
        wipe(enc, enc_capacity); wipe(ciphertext, ciphertext_capacity);
        rc = RECORDING_HPKE_PROVIDER_FAILED;
    }
    return rc;
}

int recording_hpke_psa_seal(struct recording_hpke_psa_context *context,
 const uint8_t *recipient,size_t recipient_len,const uint8_t *info,size_t info_len,
 const uint8_t *aad,size_t aad_len,const uint8_t *plain,size_t n,
 uint8_t *enc,size_t enc_cap,uint8_t *cipher,size_t cap,size_t *actual)
{return seal_job(context,recipient,recipient_len,info,info_len,aad,aad_len,plain,n,enc,enc_cap,cipher,cap,actual,0);}

int recording_hpke_psa_seal_in_place(struct recording_hpke_psa_context *context,
 const uint8_t *recipient,size_t recipient_len,const uint8_t *info,size_t info_len,
 const uint8_t *aad,size_t aad_len,uint8_t *buffer,size_t n,size_t cap,
 uint8_t *enc,size_t enc_cap,size_t *actual)
{
 if(!buffer||n>65536u||cap!=n+16u)return RECORDING_HPKE_INVALID;
 return seal_job(context,recipient,recipient_len,info,info_len,aad,aad_len,buffer,n,enc,enc_cap,buffer,cap,actual,1);
}

const struct recording_hpke_provider *recording_hpke_psa_provider(
    const struct recording_hpke_psa_context *context)
{
    return context != NULL && context->state == OWNER_READY && context->provider.user == context ?
        &context->provider : NULL;
}

int recording_hpke_psa_faulted(const struct recording_hpke_psa_context *context)
{
    return context != NULL && context->state == OWNER_FAULT;
}
