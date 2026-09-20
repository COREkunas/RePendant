#ifndef OPENPENDANT_NWR_COMMANDS_H
#define OPENPENDANT_NWR_COMMANDS_H
#include <stddef.h>
#include <stdint.h>
#include <zephyr/drivers/usb/usb_dc.h>
struct shell;
void nand_write_rate_usb_status(enum usb_dc_status_code,const uint8_t *);
int command_nand_write_rate(const struct shell *,size_t,char **);
int command_nand_write_rate_status(const struct shell *,size_t,char **);
#endif
