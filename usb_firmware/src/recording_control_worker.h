#ifndef OPENPENDANT_RECORDING_CONTROL_WORKER_H
#define OPENPENDANT_RECORDING_CONTROL_WORKER_H
#include "recording_control.h"
#include "recording_worker.h"
/* Codec-worker-only adapter. Runtime first takes existing command/catalog CAS,
 * mounts/grants/selects the bridge under its original preparation deadline, and
 * derives capacity under its existing exclusive bridge lease. These functions
 * do not mount or grant anything. Runtime must retain its actor/lease until all
 * stop/storage/release proofs return. No BT thread may call rw_get_status.
 * A STOP before final start check prevents capture; after that linearization it
 * is an in-flight start and drains, never a "microphone was never powered" claim.
 */
struct lcw_owner {struct recording_control *control;uint32_t ticket;struct es_binding session;int start_result;};
struct lcw_observation {
 uint32_t usb,mic_on,producer_joined,storage_joined,released;
 uint32_t accepted_frames; /* trusted capture accepted count, not returned DMIC blocks */
};
int lcw_start(struct lcw_owner*,struct recording_control*,uint32_t,const struct es_binding*,uint64_t first);
int lcw_step(struct lcw_owner*);
int lcw_publish(struct lcw_owner*,const struct lcw_observation*);
#endif
