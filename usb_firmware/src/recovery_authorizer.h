/* Isolated candidate; no production registration or physical NAND access. */
#ifndef PHASE2_RECOVERY_AUTHORIZER_H
#define PHASE2_RECOVERY_AUTHORIZER_H
#include "recording_volume.h"
#define RA_RECORD_BYTES 1024U
#define RA_DIGEST_OFFSET 992U
#define RA_MAX_MS 5000U
enum ra_rc { RA_OK=0,RA_ABSENT=1,RA_ARGUMENT=-1,RA_REFUSED=-2,RA_FAULT=-3,RA_BUSY=-4 };
struct ra_evidence {
 uint8_t expected_device[16],current_manifest[32],original_manifest[32],control_scan[32];
};
struct ra_port {
 void *user;
 uint64_t (*now_ms)(void *);
 /* Shared across ALL instances/writers of this namespace. Nonblocking acquire,
  * synchronous release; no recursive calls or borrowed spans after return. */
 int (*acquire)(void *);
 int (*release)(void *);
 /* Independent durable one-use permit, NOT settings absence. Bind transaction
  * + canonical record digest + exact fresh authenticated session/host ledger.
  * Must remain consumed after errors/reset, never automatically reissued.
  * Must verify original/current backups and all external authority/evidence.
  * Missing implementation refuses. No private bytes are sent to this port. */
 int (*consume_permit)(void *,const uint8_t transaction[16],const uint8_t record_digest[32],uint64_t);
 /* Fixed namespace only. Read returns RA_ABSENT only when no leaf was delivered,
  * not proof of virgin flash; malformed/duplicate/short/unknown => negative.
  * Write is one attempt, may have committed on ANY return. Readback is mandatory. */
 int (*read)(void *,uint8_t out[RA_RECORD_BYTES],size_t *actual,uint64_t);
 int (*write)(void *,const uint8_t record[RA_RECORD_BYTES],uint64_t);
};
struct recovery_authorizer {
 atomic_uint gate;
 uint32_t magic,attempted,fault,held;
 uint64_t deadline,last_now;
 struct ra_port port;
 struct owned_page_hash hash;
 struct ra_evidence evidence;
 uint8_t expected[RA_RECORD_BYTES],work[RA_RECORD_BYTES];
};
/* External once-only initialization of fresh zero storage; no copying/resetting.
 * Trusted cfg is independently validated settings state, not adopted NAND data.
 * Point curve validation remains rcfg's job; this validates its exact canonical
 * descriptor, SEC1 shape/fingerprint and recovery config/proof binding.
 * Caller keeps cfg/proof and port/provider code immutable for the whole call;
 * callbacks are trusted synchronous boundaries, not memory-safety sandboxes. */
int ra_init(struct recovery_authorizer *,const struct ra_port *,const struct owned_page_hash *,
 const struct recording_configuration *,const struct rv_recovery_proof *,const struct ra_evidence *);
/* Fits rv_recovery_authorizer.consume exactly. No NAND open can follow until0.
 * Every admitted invocation is single-use even on failure; a NEW instance still
 * needs the independent permit and refuses every existing settings record. */
int ra_consume(void *,const struct recording_configuration *,const struct rv_recovery_proof *,uint64_t);
#endif
