#ifndef PHASE2_RECOVERY_AUTHORIZER_SETTINGS_H
#define PHASE2_RECOVERY_AUTHORIZER_SETTINGS_H
#include "recovery_authorizer.h"
#define RA_SETTINGS_KEY "oprec/recovery-phase2-v1"
struct ra_settings_permit {
 void *user;
 int (*consume)(void *,const uint8_t transaction[16],const uint8_t record_digest[32],uint64_t deadline);
};
/* Once-only process binding. Shared Zephyr mutex covers the fixed namespace.
 * Settings subsystem readiness + external durable permit implementation are
 * REQUIRED. No auto provision/reset/delete/default authorization is supplied. */
int ra_settings_bind(struct ra_port *,const struct ra_settings_permit *);
#endif
