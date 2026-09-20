#ifndef OPENPENDANT_RECORDING_FAULT_TRACE_H
#define OPENPENDANT_RECORDING_FAULT_TRACE_H
#include <stdint.h>
#include "recording_timing.h"
/* Numeric diagnostics only. Never contains PCM, keys, addresses or identities.
 * Retention across software reset is best-effort; validity is checksum gated. */
struct recording_fault_trace {
 uint32_t line,reason,uptime,task,bridge_state,bridge_error,store_job,store_stage;
 uint32_t store_fault,volume_state,volume_fault,native_line,storage_running;
 uint32_t check_code,check_deadline,check_now,guard_codec,guard_storage;
 uint32_t usb_epoch,active_epoch,lease_epoch,usb_configured;
 uint32_t phy_line,phy_fault,opcode,row,status,stopped,ready,verify_mismatch,verified,wel;
 uint32_t native_deadline,phy_deadline,phy_now,hal_valid,hal_rc,hal_opcode,hal_bytes;
 uint32_t hal_started,hal_stopped,hal_tx,hal_rx,hal_before,hal_after,hal_deadline,late_end,late_stop;
 uint32_t recording_job,segment_sequence,job_started,stage_started,observed_stage;
 struct recording_timing timing;
};
void recording_fault_save(const struct recording_fault_trace *);
int recording_fault_read(struct recording_fault_trace *);
#endif
