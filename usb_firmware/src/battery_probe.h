/* Fixed read-only bring-up, not a battery admission/charging driver. */
#ifndef OPENPENDANT_BATTERY_PROBE_H
#define OPENPENDANT_BATTERY_PROBE_H
#include <stdint.h>
struct battery_probe_sample {
 uint32_t tx,rx,errors,started_ms,finished_ms;
 uint16_t raw;
 int rc;
};
struct battery_probe_result {
 struct battery_probe_sample sample[3];
 uint32_t attempted,reads,stopped,released,fault,coherent,trusted,elapsed_ms;
 int rc;
};
/* Caller exclusively owns recorder command gate + mic external reservation.
 * Fixed 06,04,06 selectors only, no call on boot. Once per boot, including
 * cancellation. Unknown STOP/ownership retains buffers AND caller reservation.
 * Voltage/flags are raw diagnostic evidence, never power admission.
 * allow() must be a bounded cached-only USB epoch/owner/recovery check.
 */
int battery_probe_run(struct battery_probe_result*,int (*allow)(void*),void*);
int battery_probe_flags_trusted(uint16_t);
/* Explicit factory restoration only. Shared once-per-boot marker with probe.
 * No charge controller, chemistry override, full reset or automatic startup.
 * phase:1 identity,2 unseal,3 config,4 state,5 sign,6 exit,7 seal,8 final,9 done.
 * A failure after the first mutating command fences recording until reviewed.
 */
struct battery_restore_result {
 uint32_t attempted,phase,transfers,writes,stopped,released,fault,complete,elapsed_ms;
 uint32_t state_changed,sign_changed,verified,mutated;
 uint16_t device_type,fw_version,chem_before,chem_after,status_before,status_after;
 uint16_t flags_before,flags_after,voltage_before,voltage_after,soc,current,temp;
 uint8_t state_before[32],state_after[32],gain_before[32],gain_after[32];
 int rc;
};
int battery_restore_run(struct battery_restore_result*,int (*allow)(void*),void*);
#include "battery_power.h"
/* Dedicated service actor: claims TWIM for this boot, identity-only startup,
 * selector-only repeated sampling. Any bus/config error stops polling. No
 * configuration writes or automatic retries in the service. */
int battery_service_init(int (*allow)(void*),void*);
/* Cached initialization evidence, not new gauge I/O. Caller must wait until
 * init() returned (monitor publishes watch_initialized atomically). Immutable
 * afterward, including rejected repeat init calls. valid bits1/2/4/8/16 mean
 * type/fw/chem/status/flags were actually read; absent values stay zero.
 * stages:1 claim,2 admission/idle,3 type,4 firmware,5 chemistry,6 control status,
 * 7 flags,8 success. Rejection retains its exact stage and transport result. */
struct battery_service_init_result {
 uint32_t attempted,complete,stage,valid,transfers,elapsed_ms,stopped,released,fault;
 int rc;
 uint16_t type,fw,chem,status,flags;
};
void battery_service_init_status(struct battery_service_init_result*);
/* Optional, separately gated factory reinitialization ONLY with proven ITPOR.
 * Uses the reviewed restoration sequence and verifies complete readback/seal.
 * restore_allow must bind stable USB + exclusive ownership for EVERY transfer.
 * No retry after any failure; unknown or partially mutated state stays fenced.
 * Initial evidence keeps pre-recovery values; stage9 identifies recovery. */
int battery_service_init_recover(int (*allow)(void*),int (*restore_allow)(void*),
                                void*,struct battery_restore_result*);
int battery_service_read(struct bp_sample*,int (*allow)(void*),void*);
/* Only the service actor calls released(), after a completed API call. */
int battery_service_released(void);
int battery_service_claimed(void);
#endif
