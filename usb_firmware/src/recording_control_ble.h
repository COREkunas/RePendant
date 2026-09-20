/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OPENPENDANT_RECORDING_CONTROL_BLE_H
#define OPENPENDANT_RECORDING_CONTROL_BLE_H
#include <stddef.h>
#include <stdint.h>
struct bt_conn;
struct recording_control_ble_hooks {
 int (*authorized)(void*,struct bt_conn*);
 int (*notify)(void*,struct bt_conn*,const uint8_t*,size_t);
 void *user;
};
int recording_control_ble_init(const struct recording_control_ble_hooks*);
int recording_control_ble_command(struct bt_conn*,const uint8_t*,size_t);
void recording_control_ble_disconnected(struct bt_conn*);
void recording_control_ble_poll(void);
int recording_control_ble_authorized(void*,uint64_t);
int recording_control_ble_reply_allowed(struct bt_conn*,uint16_t);
#endif
