/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OPENPENDANT_DEVICE_PREFERENCES_H
#define OPENPENDANT_DEVICE_PREFERENCES_H
#include <stddef.h>
#include <stdint.h>
#define DP_BYTES 16U
#define DP_GET 0x21U
#define DP_SET 0x22U
#define DP_CAPABILITY (1U << 12)
/* Byte grammar shared with Android: schema, profile, brightness, recording,
 * low battery, connected, USB-present, fault colors, low threshold, idle delay,
 * recording behavior (0 steady, 1 confirmations only), charging color,
 * revision LE32. Schema2; legacy schema1 reserved zeros migrate in RAM.
 * Profiles change disconnected discovery; standalone-capable builds also use
 * balanced/saver battery-idle System-ON standby. Never changes audio or safety.
 * Idle is not System OFF or a guaranteed mA value. */
enum dp_event { DP_OFF,DP_RECORDING,DP_LOW,DP_CONNECTED,DP_USB,DP_FAULT,DP_CHARGING };
void dp_defaults(uint8_t out[DP_BYTES]);
void dp_upgrade(uint8_t p[DP_BYTES]);
int dp_valid(const uint8_t *p,size_t n);
uint32_t dp_revision(const uint8_t p[DP_BYTES]);
int dp_next(const uint8_t current[DP_BYTES],const uint8_t request[DP_BYTES],uint8_t out[DP_BYTES]);
uint32_t dp_idle_ms(const uint8_t p[DP_BYTES]);
uint16_t dp_advertising_units(const uint8_t p[DP_BYTES],int idle);
void dp_color(const uint8_t p[DP_BYTES],enum dp_event event,uint64_t now,uint8_t rgb[3]);
/* 150ms on/150ms off, bounded with no sleeping in the recording worker. */
int dp_blink(unsigned count,uint64_t began,uint64_t now);
#endif
