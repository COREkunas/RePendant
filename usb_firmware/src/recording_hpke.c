#include "recording_hpke.h"

/* Only public protocol constants are static. Labels exclude their C NUL. */
static const uint8_t version[] = "HPKE-v1";
static const uint8_t kem_suite[] = { 'K', 'E', 'M', 0, 0x10 };
static const uint8_t hpke_suite[] = { 'H', 'P', 'K', 'E', 0, 0x10, 0, 1, 0, 2 };
static const uint8_t empty[1] = { 0 };

struct scratch {
    uint8_t data[320];
    uint8_t dh[32];
    uint8_t eae_prk[32];
    uint8_t shared[32];
    uint8_t secret[32];
    uint8_t schedule[65];
    uint8_t key[32];
    uint8_t nonce[32];
    uint8_t kem_context[130];
};

static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n != 0u) { *v++ = 0; --n; }
}

static void copy(uint8_t *out, const uint8_t *in, size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i) { out[i] = in[i]; }
}

struct span { uintptr_t first; size_t bytes; };

static int spans_disjoint(const struct recording_hpke_provider *p,
                          const uint8_t *recipient, size_t recipient_len,
                          const uint8_t *info, size_t info_len,
                          const uint8_t *aad, size_t aad_len,
                          const uint8_t *plaintext, size_t plaintext_len,
                          uint8_t *enc, size_t enc_capacity,
                          uint8_t *ciphertext, size_t ciphertext_capacity,
                          size_t *out_len)
{
    const struct span spans[8] = {
        { (uintptr_t)p, sizeof(*p) },
        { (uintptr_t)recipient, recipient_len },
        { (uintptr_t)info, info_len },
        { (uintptr_t)aad, aad_len },
        { (uintptr_t)plaintext, plaintext_len },
        { (uintptr_t)enc, enc_capacity },
        { (uintptr_t)ciphertext, ciphertext_capacity },
        { (uintptr_t)out_len, sizeof(*out_len) }
    };
    size_t i, j;
    for (i = 0; i < 8u; ++i) {
        uintptr_t last_i;
        if (spans[i].first == 0u || spans[i].bytes == 0u) { continue; }
        if (spans[i].bytes - 1u > UINTPTR_MAX - spans[i].first) { return 0; }
        last_i = spans[i].first + spans[i].bytes - 1u;
        for (j = 0; j < i; ++j) {
            uintptr_t last_j;
            if (spans[j].first == 0u || spans[j].bytes == 0u) { continue; }
            /* Earlier entries have already passed the overflow check. */
            last_j = spans[j].first + spans[j].bytes - 1u;
            if (spans[i].first <= last_j && spans[j].first <= last_i) { return 0; }
        }
    }
    return 1;
}

static int hmac(const struct recording_hpke_provider *p,
                const uint8_t *key, size_t key_len,
                const uint8_t *data, size_t data_len, uint8_t out[32])
{
    size_t n = 0;
    int result = p->hmac_sha256(p->user, key, key_len, data, data_len, out, 32u, &n);
    if (result != 0 || n != 32u) {
        wipe(out, 32u);
        return RECORDING_HPKE_PROVIDER_FAILED;
    }
    return RECORDING_HPKE_OK;
}

/* Extract implements HKDF-Extract(salt, "HPKE-v1"||suite||label||ikm).
 * Empty HMAC salt has the RFC5869 all-zero salt semantics. */
static int extract(const struct recording_hpke_provider *p, struct scratch *s,
                   const uint8_t *suite, size_t suite_len,
                   const uint8_t *salt, size_t salt_len,
                   const char *label, size_t label_len,
                   const uint8_t *ikm, size_t ikm_len, uint8_t out[32])
{
    size_t n = 0;
    if (suite_len > 10u || label_len > 13u || ikm_len > 256u) {
        return RECORDING_HPKE_INVALID;
    }
    copy(s->data + n, version, 7u); n += 7u;
    copy(s->data + n, suite, suite_len); n += suite_len;
    copy(s->data + n, (const uint8_t *)label, label_len); n += label_len;
    copy(s->data + n, ikm, ikm_len); n += ikm_len;
    return hmac(p, salt, salt_len, s->data, n, out);
}

/* Every requested output here is <=SHA256 output length, so HKDF-Expand
 * needs exactly its first block: HMAC(PRK, labeled_info || 0x01). The two-byte
 * output L is RFC I2OSP big endian, not native/endian-dependent storage. */
static int expand(const struct recording_hpke_provider *p, struct scratch *s,
                  const uint8_t *suite, size_t suite_len,
                  const uint8_t prk[32], const char *label, size_t label_len,
                  const uint8_t *info, size_t info_len, size_t wanted,
                  uint8_t out[32])
{
    size_t n = 0;
    if (suite_len > 10u || label_len > 13u || info_len > 256u ||
        wanted == 0u || wanted > 32u) { return RECORDING_HPKE_INVALID; }
    s->data[n++] = 0;
    s->data[n++] = (uint8_t)wanted;
    copy(s->data + n, version, 7u); n += 7u;
    copy(s->data + n, suite, suite_len); n += suite_len;
    copy(s->data + n, (const uint8_t *)label, label_len); n += label_len;
    copy(s->data + n, info, info_len); n += info_len;
    s->data[n++] = 1;
    return hmac(p, prk, 32u, s->data, n, out);
}

static int seal_core(const struct recording_hpke_provider *p,
                        const uint8_t *recipient, size_t recipient_len,
                        const uint8_t *info, size_t info_len,
                        const uint8_t *aad, size_t aad_len,
                        const uint8_t *plaintext, size_t plaintext_len,
                        uint8_t *enc, size_t enc_capacity,
                        uint8_t *ciphertext, size_t ciphertext_capacity,
                        size_t *out_len, int in_place)
{
    struct scratch s;
    size_t enc_len = 0, dh_len = 0, sealed_len = 0, i;
    uint8_t dh_nonzero = 0;
    int rc = RECORDING_HPKE_INVALID;
    /* Reject aliasing before dereferencing provider or writing result/output.
     * Otherwise even a failure wipe can corrupt a recipient key/header/input. */
    if (!spans_disjoint(p, recipient, recipient_len, info, info_len, aad, aad_len,
                        in_place?NULL:plaintext, in_place?0:plaintext_len, enc, enc_capacity, ciphertext,
                        ciphertext_capacity, out_len)) { return RECORDING_HPKE_INVALID; }
    /* Volatile wipe also initializes all padding/scratch, without libc helpers. */
    wipe(&s, sizeof(s));
    if (out_len != NULL) { *out_len = 0; }
    if (p == NULL || p->encap_dh == NULL || p->hmac_sha256 == NULL ||
        p->aes256gcm_seal == NULL || recipient == NULL || recipient_len != 65u ||
        recipient[0] != 4u || info_len > 256u || aad_len > 256u ||
        plaintext_len > 65536u || (info == NULL && info_len != 0u) ||
        (aad == NULL && aad_len != 0u) || (plaintext == NULL && plaintext_len != 0u) ||
        enc == NULL || enc_capacity != 65u || ciphertext == NULL ||
        ciphertext_capacity != plaintext_len + 16u || out_len == NULL) { goto done; }
    if (info == NULL) { info = empty; }
    if (aad == NULL) { aad = empty; }
    if (plaintext == NULL) { plaintext = empty; }
    rc = RECORDING_HPKE_PROVIDER_FAILED;
    if (p->encap_dh(p->user, recipient, 65u, enc, 65u, &enc_len,
                    s.dh, 32u, &dh_len) != 0 || enc_len != 65u ||
        dh_len != 32u || enc[0] != 4u) { goto done; }
    for (i = 0; i < 32u; ++i) { dh_nonzero |= s.dh[i]; }
    if (dh_nonzero == 0u) { goto done; }
    /* Recipient point/ECDH validation is provider-owned; this wrapper does not
     * implement ECC and must never claim the prefix test validates the curve. */
    copy(s.kem_context, enc, 65u);
    copy(s.kem_context + 65u, recipient, 65u);
    if (extract(p, &s, kem_suite, sizeof(kem_suite), empty, 0u,
                "eae_prk", 7u, s.dh, 32u, s.eae_prk) != 0) { goto done; }
    wipe(s.dh, sizeof(s.dh));
    if (expand(p, &s, kem_suite, sizeof(kem_suite), s.eae_prk,
               "shared_secret", 13u, s.kem_context, 130u, 32u, s.shared) != 0) { goto done; }
    wipe(s.eae_prk, sizeof(s.eae_prk));
    s.schedule[0] = 0; /* Base mode. No PSK, psk_id or sender static secret. */
    if (extract(p, &s, hpke_suite, sizeof(hpke_suite), empty, 0u,
                "psk_id_hash", 11u, empty, 0u, s.schedule + 1u) != 0) { goto done; }
    if (extract(p, &s, hpke_suite, sizeof(hpke_suite), empty, 0u,
                "info_hash", 9u, info, info_len, s.schedule + 33u) != 0) { goto done; }
    if (extract(p, &s, hpke_suite, sizeof(hpke_suite), s.shared, 32u,
                "secret", 6u, empty, 0u, s.secret) != 0) { goto done; }
    wipe(s.shared, sizeof(s.shared));
    if (expand(p, &s, hpke_suite, sizeof(hpke_suite), s.secret,
               "key", 3u, s.schedule, 65u, 32u, s.key) != 0) { goto done; }
    if (expand(p, &s, hpke_suite, sizeof(hpke_suite), s.secret,
               "base_nonce", 10u, s.schedule, 65u, 12u, s.nonce) != 0) { goto done; }
    wipe(s.secret, sizeof(s.secret));
    /* Single shot: sequence0 XOR base_nonce equals base_nonce. No application
     * supplied nonce or persisted encryption context exists in this API. */
    if (p->aes256gcm_seal(p->user, s.key, 32u, s.nonce, 12u, aad, aad_len,
                         plaintext, plaintext_len, ciphertext, ciphertext_capacity,
                         &sealed_len) != 0 || sealed_len != plaintext_len + 16u) { goto done; }
    *out_len = sealed_len;
    rc = RECORDING_HPKE_OK;
done:
    wipe(&s, sizeof(s));
    wipe(&dh_nonzero, sizeof(dh_nonzero));
    if (rc != RECORDING_HPKE_OK) {
        if (enc != NULL && enc_capacity <= 65u) { wipe(enc, enc_capacity); }
        if (ciphertext != NULL && ciphertext_capacity <= 65552u) {
            wipe(ciphertext, ciphertext_capacity);
        }
    }
    return rc;
}

int recording_hpke_seal(const struct recording_hpke_provider *p,
 const uint8_t *recipient,size_t recipient_len,const uint8_t *info,size_t info_len,
 const uint8_t *aad,size_t aad_len,const uint8_t *plain,size_t n,
 uint8_t *enc,size_t enc_cap,uint8_t *cipher,size_t cap,size_t *actual)
{return seal_core(p,recipient,recipient_len,info,info_len,aad,aad_len,plain,n,enc,enc_cap,cipher,cap,actual,0);}

int recording_hpke_seal_in_place(const struct recording_hpke_provider *p,
 const uint8_t *recipient,size_t recipient_len,const uint8_t *info,size_t info_len,
 const uint8_t *aad,size_t aad_len,uint8_t *buffer,size_t n,size_t cap,
 uint8_t *enc,size_t enc_cap,size_t *actual)
{
 /* Only this explicitly selected entry permits ONE exact input/output alias.
  * The whole payload+tag span still participates in every other alias check. */
 if(!buffer||n>RECORDING_HPKE_PLAINTEXT_MAX||cap!=n+16U)return RECORDING_HPKE_INVALID;
 return seal_core(p,recipient,recipient_len,info,info_len,aad,aad_len,buffer,n,enc,enc_cap,buffer,cap,actual,1);
}
