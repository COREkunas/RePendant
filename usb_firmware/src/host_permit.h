/* Isolated trusted-host permit candidate, not production admission. */
#ifndef PHASE2_HOST_PERMIT_H
#define PHASE2_HOST_PERMIT_H
#include <stdint.h>
#include <stdatomic.h>
#define HP_OFFER_MS 30000U
struct hp_binding { uint8_t device[16],transaction[16],record_digest[32]; };
struct hp_view {
 uint64_t now;
 uint32_t epoch,allowed,command_busy,task,running,used;
};
struct hp_offer {
 struct hp_binding binding;
 uint64_t issued,expires;
 uint32_t epoch;
};
struct hp_port {
 void *user;
 /* Trusted cached-only snapshot: allowed==1 means exact independent config,
  * USB/local-owner, bond, no conflicting peripheral/volume/enrollment activity.
  * Must report real current epoch/task state and monotonic device milliseconds.
  * No settings/HAL/files/RNG. Synchronous; no retained output pointer. */
 int (*snapshot)(void *,struct hp_view *);
};
struct host_permit {
 atomic_uint gate;
 uint32_t magic,offered,consumed,fault;
 uint64_t last;
 struct hp_binding binding;
 struct hp_port port;
 struct hp_offer offer;
};
/* One externally zero-initialized, aligned, nonmoving owner per boot. No reset,
 * copying, fallback or reinitialization. Input/output spans disjoint from owner.
 * Snapshot callback and bindings immutable; one nonblocking guard, no spin. */
int hp_init(struct host_permit *,const struct hp_binding *,const struct hp_port *);
/* Caller owns command_busy for the local cached shell query. First valid query
 * freezes offer; repeats return same expiry and epoch, never renew it. */
int hp_get_offer(struct host_permit *,struct hp_offer *);
/* Exact rr_external_permit.consume signature. Consumes RAM once before any
 * callback; accepts only active Task11 on offered epoch within both deadlines.
 * Does NOT prove/inspect host fsync. Trusted closed host controller must already
 * have permanently consumed its per-device ledger. Epoch repeats after reboot;
 * replay safety across boots requires that ledger AND RA, never absent=>virgin.
 * RA settings save/readback remains the NEXT barrier, before NAND acquisition. */
int hp_consume(void *,const uint8_t transaction[16],const uint8_t record_digest[32],
 uint32_t epoch,uint64_t original_deadline);
#endif
