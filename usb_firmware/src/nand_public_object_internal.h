/* Private closed backend contract for actual-C offline tests and NAND adapter.
 * Not a USB/API programmer: no caller-selected bytes, addresses or lengths. */
#ifndef OPENPENDANT_NPO_INTERNAL_H
#define OPENPENDANT_NPO_INTERNAL_H
#include "nand_public_object.h"
#include "owned_page.h"
#include <stdbool.h>
enum npo_op { PO_ID=0, PO_C0, PO_A0, PO_B0, PO_OFF, PO_ON, PO_UNLOCK,
 PO_LOCK, PO_LOAD, PO_RAW, PO_MAIN, PO_WREN, PO_WRDI,
 PO_DATA_LOAD, PO_COMMIT_LOAD, PO_DATA_EXEC, PO_COMMIT_EXEC, PO_OP_COUNT };
struct npo_reply {
 bool started, stopped;
 uint32_t tx_amount, rx_amount;
 const uint8_t *bytes; /* STOP + exact successful transfer only. */
};
struct npo_io {
 void *user;
 int64_t (*now)(void *user);
 void (*wait_us)(void *user, unsigned us);
 int (*transfer)(void *user, enum npo_op op, unsigned row_index,
                 int64_t deadline, struct npo_reply *reply);
 bool (*safe)(void *user);
 void (*wipe_rx)(void *user); /* Only when STOP proved. */
 const uint8_t *qualified_pattern; /* Exact frozen public row65536 main4096. */
 struct owned_page_hash hash;
};
struct npo_core {
 struct npo_result *result;
 const struct npo_io *io;
 const struct npo_observer *observer;
 uint8_t data[4096], commit[4096], payload[2048];
 int64_t begin, deadline;
 bool restoring;
};
/* Actual deterministic state machine; adapter supplies no user parameters. */
int npo_core_run(struct npo_core *c, struct npo_result *result, enum npo_mode mode,
                 const struct npo_io *io, const struct npo_observer *observer);
void npo_core_wipe(struct npo_core *c);
extern const struct owned_page_binding npo_data_binding, npo_commit_binding;
extern const uint8_t npo_object_id[16];
#endif
