/* Fixed read-only diagnostic. Runtime admission required; never recovery. */
#ifndef PHY_READ_PROBE_CANDIDATE_H
#define PHY_READ_PROBE_CANDIDATE_H
#include "nand_owned_phy.h"
#define PRP_MIN_MS 15000U
#define PRP_MAX_MS 20000U
#define PRP_MAX_READS 1024U
enum prp_code { PRP_OK=0,PRP_ARGUMENT=-100,PRP_BUSY=-101,PRP_USED=-102,
 PRP_ADMISSION=-103,PRP_TIME=-104,PRP_SHAPE=-105,PRP_ECC=-106,PRP_LIMIT=-107 };
enum prp_stage { PRP_OPEN=1,PRP_READ=2,PRP_CLOSE=3 };
struct prp_owner {
 void *user;
 /* Trusted nonblocking lifecycle/phase2/exclusive-owner check, never I/O.
  * Same absolute deadline throughout. Refusal is permanent for this attempt. */
 int (*check)(void *,uint64_t);
 uint32_t (*cycles)(void *);
 uint32_t cycle_hz; /* Fixed32768 for this candidate; not CPU-cycle timing. */
};
struct prp_fault {
 uint32_t valid,stage,opcode,row,bytes,fast,started,stopped,tx_amount,rx_amount;
 uint32_t elapsed_cycles,cycle_hz;
 int32_t rc;
 uint64_t before_ms,after_ms;
};
struct prp_result {
 int32_t rc,open_rc,close_rc;
 uint32_t attempted,complete,reads,starts,last_row,ecc,stopped,ready,released,retained,output_scrubbed;
 uint64_t elapsed_ms;
 struct prp_fault first;
};
struct phy_read_probe {
 atomic_uint gate;
 uint32_t used,stage,expected_row,loaded,cache_permitted;
 uint64_t deadline,start,last_now;
 struct nop_port actual,filtered;
 struct prp_owner owner;
 struct prp_result result;
 struct nand_owned_phy phy; /* Bind original HAL to THIS permanent context. */
 uint8_t main[4096]; /* CPU output only, never DMA; wiped after every read. */
};
/* Caller owns an exclusive permanent zero-initialized context, validated cached
 * descriptor and original-HAL port bound to &context->phy. No auto bind/lease,
 * cfg loading, shell, worker, reboot, mutation or recovery is supplied here.
 * Fixed rows67652/67653 only. Original PHY open may ATTEMPT WRDI if WEL1; the
 * filter refuses it BEFORE forwarding. No raw/ECC-off reads are called.
 * Failure retains original PHY/HAL state/owner; no forced close or wipe of
 * unresolved DMA. Successful close uses the SAME original20s deadline.
 * output_scrubbed refers ONLY to the CPU main[4096] output, not retained DMA.
 * retained means no proved clean release, including pre-open refusal; it is
 * deliberately conservative, not a claim that acquisition occurred.
 * Only metadata copied to result after the sole run returns; no concurrent
 * snapshot access. Callback/HAL hangs are NOT preempted by this candidate.
 */
int phy_read_probe_run(struct phy_read_probe *,const struct owned_volume_decoded *,
 const struct nop_port *original_hal,const struct prp_owner *,uint64_t absolute_deadline,
 struct prp_result *);
#endif
