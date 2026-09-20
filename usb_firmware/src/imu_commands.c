/* Fixed local USB identity read; no motion sampling or generic I2C commands. */
#include <errno.h>
#include <string.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_uart.h>
#include <zephyr/sys/util.h>
#include "mic_commands.h"
#include "imu_id.h"

int pendant_button_read(void);
static struct imu_id_result last_result;

static int command_imu_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argv);
	if (sh != shell_backend_uart_get_ptr() || argc != 1U) { return -EINVAL; }
	struct imu_id_state state;
	imu_id_get_state(&state);
	shell_print(sh, "IMU_STATUS attempted=%u busy=%u fault=%u stopped=%u bus_released=%u rc=%d reads=%u; address=0x6a reg=0x0f config_writes=0",
		state.attempted, state.busy, state.fault_latched, state.clock_stopped,
		state.bus_released, last_result.rc, last_result.attempt_count);
	return 0;
}

static int refuse(const struct shell *sh, int err)
{
	shell_print(sh, "IMU_REFUSED rc=%d", err);
	return err;
}

static int command_imu_id(const struct shell *sh, size_t argc, char **argv)
{
	if (sh != shell_backend_uart_get_ptr() || argc != 2U ||
	    strcmp(argv[1], "confirm") != 0) { return refuse(sh, -EINVAL); }
	int err = mic_commands_reserve_external();
	if (err != 0) { return refuse(sh, err); }
	struct imu_id_state state;
	imu_id_get_state(&state);
	if (state.attempted || state.busy || state.fault_latched ||
	    pendant_button_read() != 0) {
		mic_commands_release_external();
		return refuse(sh, -EBUSY);
	}
	/* The driver never logs. No shell output until its cleanup has returned. */
	err = imu_id_probe(&last_result);
	imu_id_get_state(&state);
	if (!state.fault_latched && state.clock_stopped && state.bus_released) {
		mic_commands_release_external();
	}
	/* On uncertain STOP/ownership the reservation stays held until reset.
	 * The diagnostic status remains available, but audio/recovery stay gated. */
	for (unsigned int i = 0; i < last_result.attempt_count && i < IMU_ID_MAX_ATTEMPTS; ++i) {
		const struct imu_id_attempt *a = &last_result.attempts[i];
		shell_print(sh, "IMU_ID index=%u who=%02x tx=%u rx=%u errorsrc=%u rc=%d",
			i, a->who_am_i, a->tx_amount, a->rx_amount, a->errorsrc, a->rc);
	}
	shell_print(sh, "IMU_RESULT rc=%d reads=%u fault=%u stopped=%u bus_released=%u; address=0x6a reg=0x0f config_writes=0",
		err, last_result.attempt_count, state.fault_latched,
		state.clock_stopped, state.bus_released);
	return err;
}

SHELL_STATIC_SUBCMD_SET_CREATE(imudiag_commands,
	SHELL_CMD_ARG(status, NULL, "Read IMU identity diagnostic state; no bus access.",
		command_imu_status, 1, 0),
	SHELL_CMD_ARG(id, NULL, "With confirm: fixed WHO_AM_I check once per boot.",
		command_imu_id, 2, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(imudiag, &imudiag_commands, "Local IMU identification only.", NULL);
