#ifndef OPENPENDANT_NAND_QUALIFICATION_COMMANDS_H
#define OPENPENDANT_NAND_QUALIFICATION_COMMANDS_H
#include <stddef.h>
#include <stdint.h>
#include <zephyr/drivers/usb/usb_dc.h>
struct shell;
/* Root dispatches from the one registered usb_enable callback. Metadata-only
 * lifecycle latch; no I/O, sleep, logging, teardown or callback replacement. */
void nand_qualification_usb_status(enum usb_dc_status_code,const uint8_t*);
int command_nand_qualification_status(const struct shell*,size_t,char**);
int command_nand_qualification(const struct shell*,size_t,char**);
#endif

