/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OPENPENDANT_LONG_RECORDING_CONTROL_H
#define OPENPENDANT_LONG_RECORDING_CONTROL_H
#include <stddef.h>
#include <stdint.h>
/* Long-control protocol. Enabled only by the separately qualified build profile.
 * No authentication, I/O, microphone, queue, persistent intent, retry, or state
 * mutation occurs here. A valid packet is NOT authority to start capture.
 * The secure exact-connection broker must separately prove explicit consent,
 * current enrolled binding, unique consumed operation, USB engineering power,
 * exclusive resource ownership, original deadlines and capacity admission.
 * STATUS never starts/mounts storage. STOP acknowledgement is not finalization.
 * Existing short-clip and durable-library commands remain unchanged.
 */
#define LC_CAPABILITY (1UL << 6)
#define LC_STATUS 0x40U
#define LC_START 0x41U
#define LC_STOP 0x42U
#define LC_REQUEST_BYTES 80U
#define LC_RESPONSE_BYTES 81U
#define LC_MAX_FRAMES 2560000U /* full5120 segments *500 frames */
enum lc_result { LC_OK=0,LC_ARGUMENT=-1,LC_INVALID=-2 };
enum lc_phase { LC_IDLE=0,LC_STARTING=1,LC_RUNNING=2,LC_STOPPING=3,
 LC_DRAINING=4,LC_STOPPED=5,LC_NO_CAPACITY=6,LC_CANCELLED_BEFORE_START=7,LC_FAULT=8 };
/* Reason numbers deliberately match rw_reason; only NONE,USER,FULL may be
 * clean in this first continuous/manual-stop profile. Other reasons are faults. */
enum lc_flag { LC_USB_PRESENT=1,LC_MIC_ON=2,LC_PRODUCER_JOINED=4,
 LC_STORAGE_JOINED=8,LC_FINALIZED=16,LC_RELEASED=32,LC_SESSION_CREATED=64,
 LC_FAULT_LATCH=128,LC_CAPACITY_KNOWN=256,LC_STOP_REQUESTED=512,LC_BUTTON_ARMED=1024,
 LC_PORTABLE=2048 };
/* Capability, not battery readiness or a fabricated USB-present observation. */
#ifdef OPENPENDANT_PORTABLE_RECORDING
#define LC_PROFILE_FLAGS LC_PORTABLE
#define LC_FLAGS_MASK 4095U
#else
#define LC_PROFILE_FLAGS 0U
#define LC_FLAGS_MASK 2047U
#endif
struct lc_request {
 uint8_t boot[16],operation[16],binding[32];
 uint16_t sequence;
 uint8_t command,mode;
};
struct lc_state {
 uint8_t boot[16],operation[16],recording[16];
 uint8_t phase,reason;
 uint16_t flags;
 uint32_t epoch,accepted_frames,committed_frames,admitted_frames;
};
/* Valid disjoint spans required. Every failure leaves output unchanged.
 * Fresh STATUS bootstrap alone permits boot=operation=all00. After learning
 * boot, zero operation selects cached global state; exact operation selects
 * only that consumed start, never another current/last recording.
 * START mode1 begins now; mode2 arms one short button tap for120s this boot.
 * Neither pairing nor boot arms capture. Other requests mode0. Reserved zero.
 */
int lc_parse_request(const uint8_t *,size_t,const uint8_t binding[32],
 const uint8_t boot[16],struct lc_request *);
int lc_validate_state(const struct lc_state *);
int lc_encode_response(uint8_t *,size_t,const struct lc_request *,const struct lc_state *);
int lc_parse_response(const uint8_t *,size_t,const struct lc_request *,struct lc_state *);
/* Pure same-operation observation check, not a runtime state machine. It never
 * authorizes IDLE->START or another operation; terminal states cannot restart.
 * A new boot makes outcome UNKNOWN externally, never an automatic retry.
 */
int lc_same_operation_progress(const struct lc_state *,const struct lc_state *);
#endif
