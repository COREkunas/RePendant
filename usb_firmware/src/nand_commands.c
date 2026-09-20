/* Fixed local USB diagnostics. The separate qualifier can erase only its
 * preimage-bound block1024 and program one public test page; no generic access. */
#include <errno.h>
#include <string.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_uart.h>
#include <zephyr/sys/util.h>
#include "mic_commands.h"
#include "nand_id.h"
#include "nand_backup_commands.h"
#include "nand_qualification_commands.h"

int pendant_button_read(void);

/* One UART shell owns this result. The probe never logs; all output occurs
 * after its bounded cleanup attempt. Only ID bytes, not stored NAND data.
 */
static struct nand_id_result last_result = {
	.selected_index = NAND_ID_NO_SELECTION,
	.selected_pin = NAND_ID_NO_SELECTION,
};
static struct nand_feature_result last_features;
static struct nand_page0_result last_page0 = {.ram_scrubbed = true};
static struct nand_marker_result last_marker = {.ram_scrubbed = true};

static int selected_pin(void)
{
	return last_result.selected_pin == NAND_ID_NO_SELECTION ? -1 : last_result.selected_pin;
}

static int command_nand_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argv);
	if (sh != shell_backend_uart_get_ptr() || argc != 1U) { return -EINVAL; }
	struct nand_id_state state;
	nand_id_get_state(&state);
	shell_print(sh, "NAND_STATUS attempted=%u busy=%u fault=%u stopped=%u bus_released=%u cs_configured=%u cs_high=%u selected=%d rc=%d transfers=%u aux_configured=%u aux_high=%u; nand_writes=0",
		state.attempted, state.busy, state.fault_latched, state.clock_stopped,
		state.bus_released, state.cs_configured, state.cs_high,
		selected_pin(), last_result.rc, last_result.attempt_count,
		state.aux_configured, state.aux_high);
	return 0;
}

static int refuse(const struct shell *sh, int err)
{
	shell_print(sh, "NAND_REFUSED rc=%d", err);
	return err;
}

static int command_nand_id(const struct shell *sh, size_t argc, char **argv)
{
	if (sh != shell_backend_uart_get_ptr() || argc != 2U ||
	    strcmp(argv[1], "confirm") != 0) { return refuse(sh, -EINVAL); }
	int err = mic_commands_reserve_external();
	if (err != 0) { return refuse(sh, err); }
	struct nand_id_state state;
	nand_id_get_state(&state);
	if (state.attempted || state.busy || state.fault_latched ||
	    pendant_button_read() != 0) {
		mic_commands_release_external();
		return refuse(sh, -EBUSY);
	}
	/* No acceptance print while hardware ownership is reserved. The fixed
	 * module reproduces factory P1.07 HIGH, then performs no NAND command
	 * other than 9F + dummy + two ID bytes. It never lowers the auxiliary pin.
	 */
	err = nand_id_probe(&last_result);
	nand_id_get_state(&state);
	/* Unconfirmed STOP keeps static DMA and bus pins owned by the module.
	 * Keep the shared reservation too: only physical reset can release an
	 * uncertain peripheral. Status remains available; never auto-retry it.
	 */
	if (!state.fault_latched && state.clock_stopped && state.bus_released) {
		mic_commands_release_external();
	}
	for (unsigned int i = 0; i < last_result.attempt_count && i < NAND_ID_MAX_ATTEMPTS; ++i) {
		const struct nand_id_attempt *attempt = &last_result.attempts[i];
		shell_print(sh, "NAND_ID index=%u cs=%u rx=%02x%02x%02x%02x",
			i, attempt->cs_pin, attempt->rx[0], attempt->rx[1],
			attempt->rx[2], attempt->rx[3]);
	}
	shell_print(sh, "NAND_RESULT rc=%d selected=%d transfers=%u fault=%u stopped=%u bus_released=%u cs_configured=%u cs_high=%u aux_configured=%u aux_high=%u; nand_writes=0",
		err, selected_pin(), last_result.attempt_count, state.fault_latched,
		state.clock_stopped, state.bus_released, state.cs_configured, state.cs_high,
		state.aux_configured, state.aux_high);
	return err;
}

static int command_nand_feature_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argv);
	if (sh != shell_backend_uart_get_ptr() || argc != 1U) { return -EINVAL; }
	struct nand_id_state s;
	nand_id_get_state(&s);
	const struct nand_feature_result *r = &last_features;
	shell_print(sh, "NAND_FEATURE_STATUS attempted=%u busy=%u rc=%d outcome=%u transfers=%u id_valid=%u initial_valid=%u initial=%u config_valid=%u config=%u final_valid=%u final=%u changed=%u initial_busy=%u final_busy=%u fault=%u stopped=%u bus_released=%u cs_configured=%u cs_high=%u aux_configured=%u aux_high=%u; array_reads=0 feature_writes=0 nand_writes=0",
		s.attempted, s.busy, r->common.rc, (unsigned int)r->outcome,
		r->common.attempt_count, r->id_valid, r->initial_valid, r->initial,
		r->config_valid, r->config, r->final_valid, r->final, r->changed,
		r->initial_busy, r->final_busy, s.fault_latched, s.clock_stopped,
		s.bus_released, s.cs_configured, s.cs_high, s.aux_configured, s.aux_high);
	return 0;
}

static int feature_refuse(const struct shell *sh, int err)
{
	shell_print(sh, "NAND_FEATURE_REFUSED rc=%d", err);
	return err;
}

static int command_nand_features(const struct shell *sh, size_t argc, char **argv)
{
	if (sh != shell_backend_uart_get_ptr() || argc != 2U ||
	    strcmp(argv[1], "confirm") != 0) { return feature_refuse(sh, -EINVAL); }
	int err = mic_commands_reserve_external();
	if (err != 0) { return feature_refuse(sh, err); }
	struct nand_id_state s;
	nand_id_get_state(&s);
	if (s.attempted || s.busy || s.fault_latched || pendant_button_read() != 0) {
		mic_commands_release_external();
		return feature_refuse(sh, -EBUSY);
	}
	err = nand_feature_probe(&last_features);
	nand_id_get_state(&s);
	if (!s.fault_latched && s.clock_stopped && s.bus_released) {
		mic_commands_release_external();
	}
	/* Hardware cleanup has finished or latched a non-reusable fault. Invalid
	 * RX stays zero; unresolved DMA storage must never be inspected/logged. */
	const struct nand_feature_result *r = &last_features;
	for (unsigned int i = 0; i < r->common.attempt_count && i < NAND_ID_MAX_ATTEMPTS; ++i) {
		const struct nand_id_attempt *a = &r->common.attempts[i];
		shell_print(sh, "NAND_FEATURE_XFER index=%u op=%u cs=%u reg=%u expected=%u tx=%u rx_count=%u raw_valid=%u raw=%02x%02x%02x%02x rc=%d",
			i, (unsigned int)a->operation, a->cs_pin, a->reg, a->expected_length,
			a->tx_amount, a->rx_amount, a->raw_valid, a->rx[0], a->rx[1],
			a->rx[2], a->rx[3], a->rc);
	}
	shell_print(sh, "NAND_FEATURE_RESULT rc=%d outcome=%u transfers=%u id_valid=%u initial_valid=%u initial=%u config_valid=%u config=%u final_valid=%u final=%u changed=%u initial_busy=%u final_busy=%u fault=%u stopped=%u bus_released=%u cs_configured=%u cs_high=%u aux_configured=%u aux_high=%u; array_reads=0 feature_writes=0 nand_writes=0",
		err, (unsigned int)r->outcome, r->common.attempt_count, r->id_valid,
		r->initial_valid, r->initial, r->config_valid, r->config, r->final_valid,
		r->final, r->changed, r->initial_busy, r->final_busy, s.fault_latched,
		s.clock_stopped, s.bus_released, s.cs_configured, s.cs_high,
		s.aux_configured, s.aux_high);
	return err;
}

/* Compile-time format/argument lists keep cached and live page metadata exact.
 * No cache header or payload ever enters a shell print. */
#define PAGE0_FIELDS "rc=%d outcome=%u transfers=%u polls=%u id_valid=%u initial_valid=%u initial=%u config_valid=%u config=%u pre_valid=%u pre=%u page_started=%u ready_valid=%u ready=%u ecc_valid=%u ecc_code=%u cache1_valid=%u cache2_valid=%u middle_valid=%u middle=%u post_valid=%u post=%u config_after_valid=%u config_after=%u final_valid=%u final=%u crc_valid=%u crc1=%u crc2=%u match=%u data_valid=%u ram_scrubbed=%u ready_unknown=%u fault=%u stopped=%u bus_released=%u cs_configured=%u cs_high=%u aux_configured=%u aux_high=%u; row=0 column=0 main_bytes=4096 oob_bytes=0 feature_writes=0 nand_programs=0 nand_erases=0"
#define PAGE0_VALUES r->common.rc, (unsigned int)r->outcome, r->common.attempt_count, r->polls, \
	r->id_valid, r->initial_valid, r->initial, r->config_valid, r->config, r->pre_valid, r->pre, \
	r->page_started, r->ready_valid, r->ready, r->ecc_valid, r->ecc_code, r->cache1_valid, r->cache2_valid, \
	r->middle_valid, r->middle, r->post_valid, r->post, r->config_after_valid, r->config_after, \
	r->final_valid, r->final, r->crc_valid, r->crc1, r->crc2, r->match, r->data_valid, r->ram_scrubbed, \
	r->ready_unknown, s.fault_latched, s.clock_stopped, s.bus_released, s.cs_configured, s.cs_high, \
	s.aux_configured, s.aux_high

static int command_nand_page0_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argv);
	if (sh != shell_backend_uart_get_ptr() || argc != 1U) { return -EINVAL; }
	struct nand_id_state s;
	nand_id_get_state(&s);
	const struct nand_page0_result *r = &last_page0;
	shell_print(sh, "NAND_PAGE0_STATUS attempted=%u busy=%u " PAGE0_FIELDS,
		    s.attempted, s.busy, PAGE0_VALUES);
	return 0;
}

static int page0_refuse(const struct shell *sh, int err)
{
	shell_print(sh, "NAND_PAGE0_REFUSED rc=%d", err);
	return err;
}

static int command_nand_page0(const struct shell *sh, size_t argc, char **argv)
{
	if (sh != shell_backend_uart_get_ptr() || argc != 2U || strcmp(argv[1], "confirm") != 0) {
		return page0_refuse(sh, -EINVAL);
	}
	int err = mic_commands_reserve_external();
	if (err != 0) { return page0_refuse(sh, err); }
	struct nand_id_state s;
	nand_id_get_state(&s);
	if (s.attempted || s.busy || s.fault_latched || pendant_button_read() != 0) {
		mic_commands_release_external();
		return page0_refuse(sh, -EBUSY);
	}
	err = nand_page0_probe(&last_page0);
	nand_id_get_state(&s);
	const struct nand_page0_result *r = &last_page0;
	if (!s.fault_latched && s.clock_stopped && s.bus_released &&
	    r->ram_scrubbed && !r->ready_unknown) { mic_commands_release_external(); }
	for (unsigned int i = 0; i < r->common.attempt_count && i < NAND_PAGE0_MAX_TRANSFERS; ++i) {
		const struct nand_page0_attempt *a = &r->attempts[i];
		shell_print(sh, "NAND_PAGE0_XFER index=%u op=%u cs=%u reg=%u expected=%u tx=%u rx_count=%u raw_valid=%u raw=%02x%02x%02x%02x rc=%d",
			i, (unsigned int)a->operation, a->cs_pin, a->reg, a->expected_length,
			a->tx_amount, a->rx_amount, a->raw_valid, a->rx[0], a->rx[1], a->rx[2], a->rx[3], a->rc);
	}
	shell_print(sh, "NAND_PAGE0_RESULT " PAGE0_FIELDS, PAGE0_VALUES);
	return err;
}
#undef PAGE0_FIELDS
#undef PAGE0_VALUES

#define MARKER_FIELDS "rc=%d outcome=%u primary_rc=%d primary_outcome=%u restore_rc=%d restore_state=%u transfers=%u polls1=%u polls2=%u id_valid=%u initial_valid=%u initial=%u lock_valid=%u lock=%u config_valid=%u config=%u pre_valid=%u pre=%u off_attempted=%u off_confirmed=%u raw_config_valid=%u raw_config=%u raw_pre_valid=%u raw_pre=%u page1_started=%u ready1_valid=%u ready1=%u cache1_valid=%u post1_valid=%u post1=%u page2_started=%u ready2_valid=%u ready2=%u cache2_valid=%u post2_valid=%u post2=%u raw_final_valid=%u raw_final=%u restore_attempted=%u restore_guard_valid=%u restore_guard=%u restore_write_attempted=%u restore_config_valid=%u restore_config=%u lock_after_valid=%u lock_after=%u final_valid=%u final=%u restored=%u restore_required=%u current_config_valid=%u current_config=%u config_unknown=%u crc_valid=%u crc1=%u crc2=%u match=%u markers_valid=%u marker1=%u marker2=%u marker_ff=%u data_valid=%u ram_scrubbed=%u ready_unknown=%u fault=%u stopped=%u bus_released=%u cs_configured=%u cs_high=%u aux_configured=%u aux_high=%u; block=1024 row=65536 column=0 raw_bytes=4352 marker_bytes=2 data_ecc=0 a0_writes=0 nand_programs=0 nand_erases=0"
#define MARKER_VALUES r->common.rc, (unsigned int)r->outcome, r->primary_rc, (unsigned int)r->primary_outcome, \
	r->restore_rc, (unsigned int)r->restore_state, r->common.attempt_count, r->polls1, r->polls2, \
	r->id_valid, r->initial_valid, r->initial, r->lock_valid, r->lock, r->config_valid, r->config, r->pre_valid, r->pre, \
	r->off_attempted, r->off_confirmed, r->raw_config_valid, r->raw_config, r->raw_pre_valid, r->raw_pre, \
	r->page1_started, r->ready1_valid, r->ready1, r->cache1_valid, r->post1_valid, r->post1, \
	r->page2_started, r->ready2_valid, r->ready2, r->cache2_valid, r->post2_valid, r->post2, r->raw_final_valid, r->raw_final, \
	r->restore_attempted, r->restore_guard_valid, r->restore_guard, r->restore_write_attempted, \
	r->restore_config_valid, r->restore_config, r->lock_after_valid, r->lock_after, r->final_valid, r->final, \
	r->restored, r->restore_required, r->current_config_valid, r->current_config, r->config_unknown, \
	r->crc_valid, r->crc1, r->crc2, r->match, r->markers_valid, r->marker1, r->marker2, r->marker_ff, r->data_valid, \
	r->ram_scrubbed, r->ready_unknown, s.fault_latched, s.clock_stopped, s.bus_released, s.cs_configured, \
	s.cs_high, s.aux_configured, s.aux_high

static int command_nand_marker_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argv);
	if (sh != shell_backend_uart_get_ptr() || argc != 1U) { return -EINVAL; }
	struct nand_id_state s;
	nand_id_get_state(&s);
	const struct nand_marker_result *r = &last_marker;
	shell_print(sh, "NAND_MARKER_STATUS attempted=%u busy=%u " MARKER_FIELDS,
		    s.attempted, s.busy, MARKER_VALUES);
	return 0;
}

static int marker_refuse(const struct shell *sh, int err)
{
	shell_print(sh, "NAND_MARKER_REFUSED rc=%d", err);
	return err;
}

static int command_nand_marker(const struct shell *sh, size_t argc, char **argv)
{
	if (sh != shell_backend_uart_get_ptr() || argc != 2U || strcmp(argv[1], "confirm") != 0) {
		return marker_refuse(sh, -EINVAL);
	}
	int err = mic_commands_reserve_external();
	if (err != 0) { return marker_refuse(sh, err); }
	struct nand_id_state s;
	nand_id_get_state(&s);
	if (s.attempted || s.busy || s.fault_latched || pendant_button_read() != 0) {
		mic_commands_release_external();
		return marker_refuse(sh, -EBUSY);
	}
	err = nand_marker_probe(&last_marker);
	nand_id_get_state(&s);
	const struct nand_marker_result *r = &last_marker;
	if (!s.fault_latched && s.clock_stopped && s.bus_released && r->ram_scrubbed &&
	    !r->ready_unknown && !r->restore_required && (!r->off_attempted || r->restored)) {
		mic_commands_release_external();
	}
	for (unsigned int i = 0; i < r->common.attempt_count && i < NAND_MARKER_MAX_TRANSFERS; ++i) {
		const struct nand_marker_attempt *a = &r->attempts[i];
		shell_print(sh, "NAND_MARKER_XFER index=%u op=%u cs=%u reg=%u expected=%u tx=%u rx_count=%u raw_valid=%u raw=%02x%02x%02x%02x rc=%d",
			i, (unsigned int)a->operation, a->cs_pin, a->reg, a->expected_length,
			a->tx_amount, a->rx_amount, a->raw_valid, a->rx[0], a->rx[1], a->rx[2], a->rx[3], a->rc);
	}
	shell_print(sh, "NAND_MARKER_RESULT " MARKER_FIELDS, MARKER_VALUES);
	return err;
}
#undef MARKER_FIELDS
#undef MARKER_VALUES

SHELL_STATIC_SUBCMD_SET_CREATE(nanddiag_commands,
	SHELL_CMD_ARG(block1024qualifystatus, NULL, "Cached fixed qualification metadata; no bus access.",
		command_nand_qualification_status, 1, 0),
	SHELL_CMD_ARG(block1024qualify, NULL, "With confirm and bound host: ERASE preserved block1024 once, program one test page. No rollback.",
		command_nand_qualification, 2, 0),
	SHELL_CMD_ARG(backupstatus, NULL, "Cached block1024 backup metadata; no bus access.",
		command_nand_backup_status, 1, 0),
	SHELL_CMD_ARG(block1024backup, NULL, "With confirm: fixed64-row raw USB backup and ECC restore; no array writes.",
		command_nand_backup, 2, 0),
	SHELL_CMD_ARG(status, NULL, "Read NAND ID diagnostic state; no bus access.",
		command_nand_status, 1, 0),
	SHELL_CMD_ARG(id, NULL, "With confirm: factory P1.07 HIGH and one ID-only probe per boot.",
		command_nand_id, 2, 0),
	SHELL_CMD_ARG(featurestatus, NULL, "Read cached feature diagnostic state; no bus access.",
		command_nand_feature_status, 1, 0),
	SHELL_CMD_ARG(features, NULL, "With confirm: fixed CS15 ID/C0/B0/C0 snapshot; no array access or writes.",
		command_nand_features, 2, 0),
	SHELL_CMD_ARG(page0status, NULL, "Cached page0 metadata only; no bus or content access.",
		command_nand_page0_status, 1, 0),
	SHELL_CMD_ARG(page0, NULL, "With confirm: row0 main4096 ECC read, two cache copies; CRC/match only, no writes.",
		command_nand_page0, 2, 0),
	SHELL_CMD_ARG(markerstatus, NULL, "Cached raw-marker/configuration metadata only; no bus access.",
		command_nand_marker_status, 1, 0),
	SHELL_CMD_ARG(marker, NULL, "With confirm: fixed block1024 raw reads; temporary B0=00 then restore10, no erase/program.",
		command_nand_marker, 2, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(nanddiag, &nanddiag_commands, "Local fixed NAND diagnostics only.", NULL);
