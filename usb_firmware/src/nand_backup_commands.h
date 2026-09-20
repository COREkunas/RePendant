#ifndef OPENPENDANT_NAND_BACKUP_COMMANDS_H
#define OPENPENDANT_NAND_BACKUP_COMMANDS_H
#include <stddef.h>
#include <stdint.h>
#include <zephyr/drivers/usb/usb_dc.h>
struct shell;
/* Registered once with usb_enable. Metadata-only lifecycle latch; no I/O,
 * sleep, logging, callback replacement or USB teardown in this callback. */
void nand_backup_usb_status(enum usb_dc_status_code status, const uint8_t *param);
int command_nand_backup_status(const struct shell *sh, size_t argc, char **argv);
int command_nand_backup(const struct shell *sh, size_t argc, char **argv);
#endif
