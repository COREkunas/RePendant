#ifndef OPENPENDANT_NAND_WRITE_RATE_BINDING_H
#define OPENPENDANT_NAND_WRITE_RATE_BINDING_H
#include <stdint.h>
/* Immutable metadata only: each packed row is LE row/length/CRC + SHA256.
 * No runtime setter, raw preimage payload, alternate rows or external input. */
struct nwr_binding {
 uint32_t enabled;
 uint8_t rows[64U*44U],table_sha[32],block_sha[32],manifest_sha[32],history_sha[32];
};
extern const struct nwr_binding nand_write_rate_binding;
#endif
