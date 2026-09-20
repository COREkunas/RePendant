/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OPENPENDANT_DEVICE_TELEMETRY_H_
#define OPENPENDANT_DEVICE_TELEMETRY_H_
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define DEVICE_TELEMETRY_COMMAND 0x20U
#define DEVICE_TELEMETRY_SIZE 48U
#define DEVICE_TELEMETRY_RECORDER_SIZE 72U
enum recorder_telemetry_flag {
 RT_USB=1U, RT_READY=2U, RT_MOUNTED=4U, RT_SUSPENDED=8U,
 RT_CAPACITY=16U, RT_WORKER=32U, RT_BUSY=64U, RT_RECORDING=128U,
 RT_FAULT=256U, RT_ENGINEERING=512U
};
/* Optional read-only RAM observations, not storage admission or a start API.
 * Capacity is valid only under the idle runtime command gate. No NAND access.
 * Worker fields may be unavailable if its nonblocking status lock is busy. */
struct recorder_telemetry {
    uint16_t flags, slots_used, slots_total;
    uint8_t state, reason, mode, roots_used, roots_total;
    uint32_t captured_samples, committed_samples;
};
/* This schema reports software observations, never an optical LED measurement.
 * Battery, usable storage and owner provisioning remain explicitly unavailable. */
struct device_telemetry {
    uint64_t uptime_ms;
    uint16_t major, minor, patch;
    bool led_known, microphone_power, resource_busy;
    uint8_t red, green, blue;
    uint32_t faults;
};
/* v3 reuses reserved bytes26..41; still72 bytes. Cached gauge observations.
 * flags:1 fresh valid,2 start power threshold passed,4 portable capability,
 * 8 monitor stopped. Not charging status, calibrated SOC or a power guarantee. */
struct battery_telemetry {
    uint16_t mv, temp, soc, gauge_flags;
    uint32_t age_ms, sequence;
    uint8_t flags;
};
int device_telemetry_encode(const struct device_telemetry *state,
                            uint8_t *out, size_t capacity);
int device_telemetry_encode_recorder(const struct device_telemetry *,
    const struct recorder_telemetry *, uint8_t *, size_t);
int device_telemetry_encode_battery(const struct device_telemetry *,
    const struct recorder_telemetry *, const struct battery_telemetry *, uint8_t *, size_t);
#endif
