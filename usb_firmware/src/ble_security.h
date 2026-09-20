/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OPENPENDANT_BLE_SECURITY_H_
#define OPENPENDANT_BLE_SECURITY_H_

#include <stdbool.h>
struct bt_conn;

/* Call once after bt_enable(), before advertising. Loads the existing settings
 * partition through Zephyr's settings API; errors leave authorization closed. */
int pendant_ble_security_init(void);

/* Authentication identifies a bonded phone, not an individual Android app.
 * Call on every request and before every connection-specific audio send. */
bool pendant_ble_authorized(struct bt_conn *conn);

/* Capture/recovery must first acquire their own busy/pending flag, then check
 * this function and release that flag on rejection. Pairing open checks those
 * flags under this function's lock, closing the two-way admission race. */
bool pendant_ble_pairing_busy(void);

/* Trusted physical-gesture path only. Replaces Bluetooth bonds, never recording
 * keys or NAND. Native L4/passkey policy stays unchanged. Not a remote command. */
int pendant_ble_pairing_replace_local(void);
/* Cached window time only; no passkey, settings or radio operations. */
unsigned int pendant_ble_pairing_remaining_ms(void);

#endif
