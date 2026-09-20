/* Proposed encrypted-segment container. Pure bytes, no crypto or I/O. */
#ifndef OPENPENDANT_ENCRYPTED_SEGMENT_HEADER_H
#define OPENPENDANT_ENCRYPTED_SEGMENT_HEADER_H
#include <stddef.h>
#include <stdint.h>
#define ES_HEADER_BYTES 128U
#define ES_ENCAP_BYTES 65U
#define ES_TAG_BYTES 16U
#define ES_MAX_PLAINTEXT_BYTES 65536U
#define ES_MAX_CONTAINER_BYTES (ES_HEADER_BYTES+ES_ENCAP_BYTES+ES_MAX_PLAINTEXT_BYTES+ES_TAG_BYTES)
#define ES_INFO_BYTES 161U
enum es_result { ES_OK=0, ES_ARGUMENT=-1, ES_INVALID=-2 };
struct es_binding {
 uint8_t key_fingerprint[32],device_id[16],volume_id[16],recording_id[16];
 uint64_t generation;
 uint32_t segment_sequence,plaintext_bytes;
};
/* Binding comes from authenticated/pinned catalog state, not parsed page bytes.
 * No nonempty secret input is accepted here; headers contain visible metadata.
 * UUIDs are canonical/network byte order; numeric header fields are big-endian.
 * Caller exclusively owns immutable inputs, output must not alias binding. */
int es_header_build(uint8_t *out,size_t out_size,const struct es_binding *binding);
/* Successful header or frame validation is STRUCTURAL ONLY, not authentication
 * or permission to expose plaintext. HPKE Open/tag verification is still required.
 * Encoded EC point must be fully validated by the HPKE crypto provider. */
int es_header_validate(const uint8_t *header,size_t size,const struct es_binding *expected);
int es_container_validate(const uint8_t *container,size_t size,const struct es_binding *expected);
/* info = fixed domain (including its NUL) || validated header. Production AEAD
 * AAD is empty. No caller-supplied domain, algorithm negotiation or fallback. */
int es_info_build(uint8_t *out,size_t out_size,const uint8_t *header,size_t header_size,
 const struct es_binding *expected);
#endif
