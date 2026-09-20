/* Fixed current-preimage capture. No recovery/provisioning authority. */
#ifndef CURRENT_PREIMAGE_CANDIDATE_H
#define CURRENT_PREIMAGE_CANDIDATE_H
#include "nand_owned_phy.h"
#define CPI_PAGES 2176U
#define CPI_BYTES 4352U
#define CPI_PAGE_MS 2000U
enum cpi_code { CPI_OK=0,CPI_ARGUMENT=-100,CPI_BUSY=-101,CPI_USED=-102,
 CPI_ADMISSION=-103,CPI_TIME=-104,CPI_SHAPE=-105,CPI_MISMATCH=-106,CPI_HASH=-107 };
enum cpi_stage { CPI_OPEN=1,CPI_FIRST=2,CPI_SECOND=3,CPI_CLOSE=4,CPI_HASHING=5 };
struct cpi_owner {
 void *user;
 /* Trusted cached phase2/revision2/descriptor and exclusive task/lifecycle check.
  * Phase-aware: called before physical acquire and after physical release too.
  * Original deadline only. No I/O, rebind, repair or settings operation. */
 int (*check)(void *,uint64_t);
 uint32_t (*cycles)(void *);
 uint32_t cycle_hz; /* RTC32768, not CPU cycles. */
 int (*sha256)(void *,const uint8_t *,size_t,uint8_t digest[32],size_t *actual);
};
struct cpi_fault {
 uint32_t valid,stage,opcode,row,bytes,fast,started,stopped,tx_amount,rx_amount;
 uint32_t elapsed_cycles,cycle_hz;
 int32_t rc;
 uint64_t before_ms,after_ms;
};
struct cpi_result {
 int32_t rc,open_rc,close_rc;
 uint32_t attempted,complete,index,row,next_index,reads,starts,off_starts,on_starts;
 uint32_t stopped,ready,released,retained,matched,hash_valid,a0,b0;
 uint64_t elapsed_ms;
 uint8_t sha256[32];
 struct cpi_fault first;
};
struct current_preimage {
 atomic_uint gate;
 uint32_t initialized,fault,next_index,available,stage,raw_phase;
 uint64_t deadline,start,last_now;
 struct nop_port actual,filtered;
 struct cpi_owner owner;
 struct cpi_result result;
 struct nand_owned_phy phy; /* Original HAL binds once to THIS permanent member. */
 uint8_t first[CPI_BYTES],second[CPI_BYTES]; /* CPU outputs only, never DMA. */
};
/* No I/O at init. Descriptor must already be independently validated from the
 * exact cached phase2/revision2 public configuration. Init cannot establish
 * phase, a fresh boot or permission to repeat after an uncertain old session.
 * Parent must retain one exclusive context/port lifetime and a durable host
 * intent ledger. Never zero/rebind this context on error or to restart.
 */
int current_preimage_init(struct current_preimage *,const struct owned_volume_decoded *,
 const struct nop_port *original_hal,const struct cpi_owner *);
/* Sole data operation: exact monotone index0..2175 maps to1025..1058, all64pages.
 * At most2s ORIGINAL caller deadline includes open, two independently loaded
 * raw reads, ECC restoration, close, exact comparison and SHA. Every admitted
 * page consumes its cursor BEFORE any I/O. Failure is permanently sticky.
 * Only original raw-read B0=0/10 changes are permitted; not persistent writes.
 * Original PHY failure may leave ECC off. No blind restore, close, retry,
 * reset, WRDI, WREN, SET A0, program or erase is supplied by this candidate.
 * Unknown STOP leaves original permanent DMA spans and ownership untouched.
 * Result is metadata only and valid after return; no concurrent snapshots.
 */
int current_preimage_capture(struct current_preimage *,uint32_t linear_index,
 uint64_t original_deadline,struct cpi_result *);
/* Exactly one cached CPU copy after complete+STOP+close+hash; no NAND callback,
 * no deadline renewal and no I/O. Parent owns bounded authenticated/private
 * export after capture. Previous output must be consumed before next capture.
 * Caller owns/wipes its external4352-byte buffer. No bytes logged here.
 */
int current_preimage_take(struct current_preimage *,uint32_t linear_index,uint8_t out[CPI_BYTES]);
#endif
