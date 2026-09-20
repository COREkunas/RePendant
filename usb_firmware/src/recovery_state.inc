/* Additive isolated Task11 state. Must never be reset after admission. */
#include "recovery_runtime.h"
#include "fixed_evidence.h"
#include "recovery_authorizer_settings.h"
#include <zephyr/bluetooth/bluetooth.h>
#ifndef OPENPENDANT_NATIVE_STORAGE
static struct recovery_authorizer recovery_authorizer;
static atomic_t recovery_permit_used;
#endif
static struct rr_external_permit recovery_external;
static atomic_t recovery_bound,recovery_used,recovery_running,recovery_expired;
static uint64_t recovery_deadline,recovery_last,recovery_started;
static uint32_t recovery_session;
static struct {
 uint32_t attempted,complete,stage,expired,permit_attempted,authorizer_saved;
 int32_t rc,cfg_rc,suspend_rc;
 uint32_t phase,fault_banks,leased,stopped,ready,state;
 uint64_t revision,elapsed_ms,parent_deadline,preimage_deadline,mutation_deadline;
 struct rv_recovery_first_fault first;
} recovery_result={.cfg_rc=-EAGAIN};
