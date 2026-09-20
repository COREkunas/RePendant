#include "recording_aead_selftest.h"
#include <psa/crypto.h>
#include <string.h>

int recording_aead_selftest(uint8_t *scratch,size_t capacity)
{
 /* PUBLIC test vector only: AES256 key=0, nonce=0, plaintext[i]=i mod256.
  * Never used for recording encryption. Independent host AESGCM oracle:
  * SHA256(AESGCM(bytes(32)).encrypt(bytes(12),pattern34148,b'')) below. */
 static const uint8_t key_bytes[32]={0},nonce[12]={0};
 static const uint8_t expected[32]={0xcc,0xa6,0x49,0x4d,0xe5,0x1b,0xcd,0x4a,
  0x0e,0x88,0x90,0x1a,0xce,0xb5,0xe0,0xe7,0x96,0x94,0x70,0x1e,0x11,0x2d,0xaa,0x8f,
  0x80,0x3e,0x43,0xad,0x12,0x57,0x9c,0x24};
 if(!scratch||capacity<34164||capacity>65552)return -1;
 uint8_t digest[32]={0};size_t actual=0,hashed=0;int rc=-1;
 psa_key_attributes_t attr=PSA_KEY_ATTRIBUTES_INIT;
 mbedtls_svc_key_id_t key=MBEDTLS_SVC_KEY_ID_INIT;
 psa_set_key_lifetime(&attr,PSA_KEY_LIFETIME_VOLATILE);
 psa_set_key_type(&attr,PSA_KEY_TYPE_AES);psa_set_key_bits(&attr,256);
 psa_set_key_usage_flags(&attr,PSA_KEY_USAGE_ENCRYPT);psa_set_key_algorithm(&attr,PSA_ALG_GCM);
 for(size_t i=0;i<34148;++i)scratch[i]=(uint8_t)i;
 if(psa_import_key(&attr,key_bytes,sizeof(key_bytes),&key)==PSA_SUCCESS&&
    psa_aead_encrypt(key,PSA_ALG_GCM,nonce,sizeof(nonce),NULL,0,scratch,34148,
                     scratch,34164,&actual)==PSA_SUCCESS&&actual==34164&&
    psa_hash_compute(PSA_ALG_SHA_256,scratch,actual,digest,sizeof(digest),&hashed)==PSA_SUCCESS&&
    hashed==32&&!memcmp(digest,expected,32))rc=0;
 if(!mbedtls_svc_key_id_is_null(key)&&psa_destroy_key(key)!=PSA_SUCCESS)rc=-1;
 psa_reset_key_attributes(&attr);
 volatile uint8_t *p=scratch;for(size_t i=0;i<capacity;++i)p[i]=0;
 p=digest;for(size_t i=0;i<sizeof(digest);++i)p[i]=0;
 return rc;
}
