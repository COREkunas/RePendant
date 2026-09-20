/* Isolated Task11 candidate. Generated fixture is NON-ADMITTED by default. */
#ifndef PHASE2_RECOVERY_RUNTIME_H
#define PHASE2_RECOVERY_RUNTIME_H
#include <stdint.h>
struct rr_external_permit {
 void *user;
 /* Mandatory trusted boundary: must verify/persist a new independent one-use
  * host intent BEFORE returning success. It binds exact fixed public evidence,
  * transaction, canonical1024 record digest and fresh authenticated session.
  * The device cannot prove host disk durability. Error/late/ambiguous results
  * remain consumed across resets; no automatic retries/reissuing. No spans
  * may be retained. Called synchronously before first settings access/HALopen. */
 int (*consume)(void *,const uint8_t transaction[16],const uint8_t record_digest[32],
                uint32_t usb_epoch,uint64_t original_deadline);
};
/* Exclusive boot setup, once only, no I/O. Missing port always refuses. This is
 * not a remote/token-only authorization API or a proof replacement endpoint. */
int recording_runtime_recovery_permit_bind(const struct rr_external_permit *);
#endif
