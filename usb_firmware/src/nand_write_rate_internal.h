/* Private closed HAL for actual-C tests and a future guarded NAND include. */
#ifndef OPENPENDANT_NAND_WRITE_RATE_INTERNAL_H
#define OPENPENDANT_NAND_WRITE_RATE_INTERNAL_H
#include "nand_write_rate.h"
#include "nand_write_rate_binding.h"
#include "owned_page.h"
#include <stdbool.h>
enum nwr_op { WO_ID=0,WO_C0,WO_A0,WO_B0,WO_OFF,WO_ON,WO_UNLOCK,
 WO_LOCK,WO_READ_LOAD,WO_RAW,WO_MAIN,WO_WREN,WO_WRDI,WO_LOAD0,
 WO_LOAD1,WO_EXEC0,WO_EXEC1,WO_COUNT };
struct nwr_reply {
 bool started,stopped,default_valid;
 uint32_t tx,rx,cycles,rate_register,rate_sets,rate_restores;
 const uint8_t *bytes; /* Only after successful END/STOP/exact counts. */
};
struct nwr_io {
 void *user;
 int64_t (*now)(void *);
 uint32_t (*cycles)(void *);
 uint32_t cycle_hz;
 void (*wait_us)(void *,unsigned);
 int (*transfer)(void *,enum nwr_op,unsigned row,int64_t,struct nwr_reply *);
 bool (*safe)(void *);
 void (*wipe_rx)(void *); /* Must not touch any unresolved DMA. */
 const uint8_t *qualified_pattern;
 struct owned_page_hash hash;
 int (*whole_begin)(void *);
 int (*whole_update)(void *,const uint8_t *,size_t);
 int (*whole_finish)(void *,uint8_t out[32]);
 int (*whole_abort)(void *);
};
struct nwr_core {
 struct nand_write_rate_result *result;
 const struct nwr_io *io;
 const struct nand_write_rate_observer *observer;
 uint8_t first[4352],data[4096],commit[4096],pattern[4096],payload[2048];
 int64_t begin,deadline;
 uint32_t exec_begin;
 bool restoring,whole_active;
};
int nwr_core_run(struct nwr_core *,struct nand_write_rate_result *,
                 const struct nwr_io *,const struct nand_write_rate_observer *);
void nwr_core_wipe(struct nwr_core *);
/* Deterministic public diagnostic patterns, row3 then row4. No caller bytes. */
void nwr_pattern(uint8_t out[4096],unsigned sample);
extern const uint32_t nand_write_rate_rates[2][2];
extern const uint32_t nand_write_rate_bounds[16];
extern const uint8_t nand_write_rate_ops[WO_COUNT][4];
#endif
