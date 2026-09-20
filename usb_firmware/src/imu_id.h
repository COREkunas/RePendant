/* Fixed, one-shot WHO_AM_I diagnostic. No sensor initialization or boot hook. */
#ifndef PENDANT_IMU_ID_H
#define PENDANT_IMU_ID_H

#include <stdbool.h>
#include <stdint.h>

#define IMU_ID_MAX_ATTEMPTS 3U

enum imu_id_outcome {
	IMU_ID_NOT_RUN = 0,
	IMU_ID_CONFIRMED,
	IMU_ID_INCONCLUSIVE,
	IMU_ID_UNEXPECTED,
	IMU_ID_TRANSFER_ERROR,
	IMU_ID_STOP_ERROR,
	IMU_ID_PRECONDITION_ERROR,
};

struct imu_id_attempt {
	uint8_t who_am_i;
	uint32_t tx_amount;
	uint32_t rx_amount;
	uint32_t errorsrc;
	int rc;
};

struct imu_id_result {
	int rc;
	enum imu_id_outcome outcome;
	uint8_t attempt_count;
	struct imu_id_attempt attempts[IMU_ID_MAX_ATTEMPTS];
	uint32_t elapsed_ms;
	bool clock_stopped;
	bool bus_released;
	bool fault_latched;
};

struct imu_id_state {
	bool attempted;
	bool busy;
	bool fault_latched;
	bool clock_stopped;
	bool bus_released;
};

/* Caller holds mic_commands_reserve_external(), excluding audio/retained PCM,
 * pairing, NAND and recovery, and verifies the physical button is released.
 * An admitted probe runs once per boot, logs nothing, and transmits only the
 * fixed register selector 0x0f to address 0x6a. No recovery clocks or retries.
 * Release admission only if stopped, released and not fault_latched. A failed
 * STOP permanently retains static DMA buffers and ownership until reset.
 * Finite poll budgets are not an RTOS/interrupt-stall wall-clock guarantee.
 */
int imu_id_probe(struct imu_id_result *result);
void imu_id_get_state(struct imu_id_state *state);

#endif
