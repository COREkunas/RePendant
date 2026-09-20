/* Fixed binary block1024 backup adapter; no array writes or generic command. */
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_uart.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/util.h>
#include "mic_commands.h"
#include "nand_id.h"
#include "nand_backup_wire.h"
#include "nand_backup_commands.h"

int pendant_button_read(void);

BUILD_ASSERT(IS_ENABLED(CONFIG_SHELL_BACKEND_SERIAL_API_INTERRUPT_DRIVEN) &&
             !IS_ENABLED(CONFIG_SHELL_BACKEND_SERIAL_API_POLLING) &&
             !IS_ENABLED(CONFIG_SHELL_BACKEND_SERIAL_API_ASYNC) &&
             !IS_ENABLED(CONFIG_SHELL_BACKEND_SERIAL_FORCE_TX_BLOCKING_MODE),
             "Backup requires the reviewed synchronous-copy nonblocking backend");
BUILD_ASSERT(!IS_ENABLED(CONFIG_LOG) && !IS_ENABLED(CONFIG_CONSOLE) &&
             !IS_ENABLED(CONFIG_PRINTK) && !IS_ENABLED(CONFIG_MCUMGR_TRANSPORT_SHELL),
             "No asynchronous or alternate binary/text producer is admitted");
BUILD_ASSERT(CONFIG_SHELL_BACKEND_SERIAL_RX_RING_BUFFER_SIZE == 64 &&
             CONFIG_SHELL_BACKEND_SERIAL_TX_RING_BUFFER_SIZE == 8 &&
             CONFIG_USB_CDC_ACM_RINGBUF_SIZE == 1024 && CONFIG_SHELL_STACK_SIZE == 4096,
             "Reviewed transport geometry/stack changed");
BUILD_ASSERT(NAND_BLOCK1024_END_WORDS == NAND_BACKUP_WIRE_END_WORDS &&
             NB_TRANSPORT_QUARANTINED == 43 && NAND_MARKER_RAW_BYTES == NAND_BACKUP_WIRE_RAW,
             "Backup interface differs from fixed protocol1");
BUILD_ASSERT(IS_ENABLED(CONFIG_USB_WORKQUEUE) && CONFIG_USB_WORKQUEUE_PRIORITY < 0 &&
             !IS_ENABLED(CONFIG_SMP),
             "Pending-RX snapshot requires single-core cooperative USB callbacks");

/* Permanent metadata only. DMA buffers remain owned by nand_id.c, not the shell.
 * The shell is the only admitted caller, and external reservation excludes mic,
 * BLE capture, pairing, recovery, LED override and other local diagnostics.
 */
static struct nand_backup_wire backup_wire;
static struct nand_block1024_result backup_result = {
    .end = {[NB_RAM_SCRUBBED] = 1, [NB_STOPPED] = 1, [NB_BUS_RELEASED] = 1},
};
static atomic_t backup_usb_epoch;
static atomic_t backup_usb_active;
static atomic_t backup_usb_fault;
static uint32_t backup_usb_session_epoch;

void nand_backup_usb_status(enum usb_dc_status_code status, const uint8_t *param)
{
    ARG_UNUSED(param);
    /* SOF is ordinary traffic. Every other status denotes a lifecycle,
     * configuration, endpoint-halt or unknown transition; fail closed if it
     * occurs during binary mode. CDC reset clears rx_ready without clearing
     * its RX ring, so that cached flag alone cannot prove an empty queue.
     * Normal CDC line-control requests do not emit these lifecycle events.
     */
    if (status != USB_DC_SOF) {
        atomic_inc(&backup_usb_epoch);
        if (atomic_get(&backup_usb_active)) { atomic_set(&backup_usb_fault, 1); }
    }
}
static bool link_valid(void)
{
    return atomic_get(&backup_usb_active) && !atomic_get(&backup_usb_fault) &&
           (uint32_t)atomic_get(&backup_usb_epoch) == backup_usb_session_epoch;
}

static struct shell_uart_int_driven *backend(const struct shell *sh)
{
    return (struct shell_uart_int_driven *)sh->iface->ctx;
}
static bool backend_valid(const struct shell *sh)
{
    return sh != NULL && sh == shell_backend_uart_get_ptr() && sh->iface != NULL &&
           sh->iface->api == &shell_uart_transport_api && sh->iface->ctx != NULL &&
           !backend(sh)->common.blocking_tx;
}
static int raw_write(void *user, const uint8_t *bytes, size_t size, size_t *accepted)
{
    const struct shell *sh = user;
    *accepted = 0;
    if (!backend_valid(sh) || size > NAND_BACKUP_WIRE_IO_MAX) { return -EINVAL; }
    if (!link_valid()) { return -EIO; }
    int rc = sh->iface->api->write(sh->iface, bytes, size, accepted);
    return link_valid() ? rc : -EIO; /* Preserve accepted prefix on late fault. */
}
static int raw_read(void *user, uint8_t *bytes, size_t size, size_t *received)
{
    const struct shell *sh = user;
    *received = 0;
    if (!backend_valid(sh) || size > NAND_BACKUP_WIRE_IO_MAX) { return -EINVAL; }
    if (!link_valid()) { return -EIO; }
    int rc = sh->iface->api->read(sh->iface, bytes, size, received);
    return link_valid() ? rc : -EIO;
}
static uint32_t raw_now(void *user)
{
    ARG_UNUSED(user);
    return (uint32_t)k_uptime_get();
}
static void raw_yield(void *user, uint32_t usec)
{
    ARG_UNUSED(user);
    /* Allow the same USB work queue to consume shell/CDC rings. No busy wait
     * spanning the USB frame; no sleep while an interrupt lock is held.
     */
    k_sleep(K_USEC(usec));
}
/* Must execute under irq_lock on this single-core application CPU. It checks
 * currently queued shell and CDC input without consuming any byte. It does
 * not claim a host cannot send future input or that consumed memory is erased.
 * Pinned CDC irq_update clears stale rx_ready after its ring becomes empty.
 * A separate lifecycle latch rejects the reset/disconnect case that can clear
 * rx_ready while its ring still retains bytes. The cooperative USB producer
 * cannot be preempted between ring publication and setting rx_ready.
 */
static int pending_locked(const struct shell *sh, bool *pending)
{
    *pending = true;
    if (!backend_valid(sh)) { return -EINVAL; }
    if (!link_valid()) { return -EIO; }
    struct shell_uart_int_driven *uart = backend(sh);
    if (uart_irq_update(uart->common.dev) != 1) { return -EIO; }
    int ready = uart_irq_rx_ready(uart->common.dev);
    if (ready < 0) { return -EIO; }
    *pending = !ring_buf_is_empty(&uart->rx_ringbuf) || ready != 0;
    return 0;
}
static int raw_pending(void *user, bool *pending)
{
    unsigned int key = irq_lock();
    int rc = pending_locked(user, pending);
    irq_unlock(key);
    return rc;
}
static void discard_input(const struct shell *sh, uint8_t *data, size_t size, void *user)
{
    ARG_UNUSED(sh); ARG_UNUSED(user);
    /* Shell supplies this bounded bypass copy after a terminal failure. Do not
     * decode, print, invoke a command, or claim SDK/OS buffers are wiped.
     */
    volatile uint8_t *owned = data;
    for (size_t i = 0; i < size; ++i) { owned[i] = 0; }
}
static struct nand_block1024_ack delivery(int rc, bool sent)
{
    enum nand_block1024_sink_status status = rc == 0 ? NAND_BLOCK1024_SINK_OK :
        rc == NAND_BACKUP_WIRE_HOST_ABORT ? NAND_BLOCK1024_HOST_ABORT :
        rc == NAND_BACKUP_WIRE_GLOBAL_TIMEOUT ? NAND_BLOCK1024_OVERALL_TIMEOUT : NAND_BLOCK1024_USB_ERROR;
    return (struct nand_block1024_ack){.status = status, .rc = rc, .frame_sent = sent};
}
static int sink_start(void *user, int64_t deadline)
{
    return nand_backup_wire_arm_data_deadline(user, (uint32_t)deadline);
}
static struct nand_block1024_ack sink_begin(void *user, const uint32_t fields[16], int64_t deadline)
{
    struct nand_backup_wire *wire = user;
    if (!wire->deadline_armed || wire->data_deadline_ms != (uint32_t)deadline) {
        return delivery(NAND_BACKUP_WIRE_STATE, false);
    }
    int rc = nand_backup_wire_send_begin(wire, fields);
    return delivery(rc, wire->begin_sent);
}
static struct nand_block1024_ack sink_page(void *user, uint16_t sequence,
    const uint32_t fields[16], const uint8_t raw[NAND_MARKER_RAW_BYTES], int64_t deadline)
{
    struct nand_backup_wire *wire = user;
    uint16_t before = wire->rows_sent;
    if (!wire->deadline_armed || wire->data_deadline_ms != (uint32_t)deadline ||
        sequence != before + 1U) { return delivery(NAND_BACKUP_WIRE_STATE, false); }
    int rc = nand_backup_wire_send_page(wire, fields, raw);
    return delivery(rc, wire->rows_sent == before + 1U);
}
static bool clean_result(const struct nand_id_state *s)
{
    const uint32_t *e = backup_result.end;
    return e[NB_RC] == 0 && e[NB_OUTCOME] == 1 && e[NB_ROWS_ACKED] == 64 &&
           e[NB_RESTORED] == 1 && e[NB_RESTORE_REQUIRED] == 0 &&
           e[NB_CURRENT_CONFIG_VALID] == 1 && e[NB_CURRENT_CONFIG] == 16 &&
           e[NB_CONFIG_UNKNOWN] == 0 && e[NB_READY_UNKNOWN] == 0 && e[NB_RAM_SCRUBBED] == 1 &&
           !s->busy && !s->fault_latched && s->clock_stopped && s->bus_released &&
           s->cs_configured && s->cs_high && s->aux_configured && s->aux_high;
}

int command_nand_backup_status(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argv);
    if (sh != shell_backend_uart_get_ptr() || argc != 1U) { return -EINVAL; }
    struct nand_id_state state;
    nand_id_get_state(&state);
    const uint32_t *e = backup_result.end;
    shell_print(sh, "NAND_BACKUP_STATUS attempted=%u busy=%u "
        "rc=%d outcome=%u primary_rc=%d primary_outcome=%u restore_rc=%d restore_state=%u "
        "rows_compared=%u rows_sent=%u rows_acked=%u raw_bytes_acked=%u transfers=%u polls1=%u polls2=%u "
        "off_attempted=%u off_confirmed=%u restore_attempted=%u restore_write_attempted=%u "
        "restore_config_valid=%u restore_config=%u lock_before_valid=%u lock_before=%u lock_after_valid=%u lock_after=%u "
        "final_valid=%u final=%u restored=%u restore_required=%u current_config_valid=%u current_config=%u "
        "config_unknown=%u block_crc_valid=%u block_crc32=%u ram_scrubbed=%u ready_unknown=%u fault=%u "
        "stopped=%u bus_released=%u cs_configured=%u cs_high=%u aux_configured=%u aux_high=%u elapsed_ms=%u "
        "usb_rc=%d transport_quarantined=%u",
        state.attempted, state.busy, (int32_t)e[NB_RC], e[NB_OUTCOME], (int32_t)e[NB_PRIMARY_RC],
        e[NB_PRIMARY_OUTCOME], (int32_t)e[NB_RESTORE_RC], e[NB_RESTORE_STATE],
        e[NB_ROWS_COMPARED], e[NB_ROWS_SENT], e[NB_ROWS_ACKED], e[NB_RAW_BYTES_ACKED], e[NB_TRANSFERS],
        e[NB_POLLS1], e[NB_POLLS2], e[NB_OFF_ATTEMPTED], e[NB_OFF_CONFIRMED], e[NB_RESTORE_ATTEMPTED],
        e[NB_RESTORE_WRITE_ATTEMPTED], e[NB_RESTORE_CONFIG_VALID], e[NB_RESTORE_CONFIG],
        e[NB_LOCK_BEFORE_VALID], e[NB_LOCK_BEFORE], e[NB_LOCK_AFTER_VALID], e[NB_LOCK_AFTER], e[NB_FINAL_VALID],
        e[NB_FINAL], e[NB_RESTORED], e[NB_RESTORE_REQUIRED], e[NB_CURRENT_CONFIG_VALID], e[NB_CURRENT_CONFIG],
        e[NB_CONFIG_UNKNOWN], e[NB_BLOCK_CRC_VALID], e[NB_BLOCK_CRC32], e[NB_RAM_SCRUBBED], e[NB_READY_UNKNOWN],
        state.fault_latched, state.clock_stopped, state.bus_released, state.cs_configured,
        state.cs_high, state.aux_configured, state.aux_high, e[NB_ELAPSED_MS],
        (int32_t)e[NB_USB_RC], e[NB_TRANSPORT_QUARANTINED]);
    return 0;
}
int command_nand_backup(const struct shell *sh, size_t argc, char **argv)
{
    if (sh != shell_backend_uart_get_ptr()) { return -EINVAL; }
    int rc = -EINVAL;
    if (argc != 2U || strcmp(argv[1], "confirm") != 0 || !backend_valid(sh)) { goto refused; }
    rc = mic_commands_reserve_external();
    if (rc != 0) { goto refused; }
    struct nand_id_state state;
    nand_id_get_state(&state);
    if (state.attempted || state.busy || state.fault_latched || pendant_button_read() != 0 ||
        backup_wire.handshake_started) {
        mic_commands_release_external();
        rc = -EBUSY;
        goto refused;
    }
    const struct nand_backup_wire_io io = {.write = raw_write, .read = raw_read,
        .now_ms = raw_now, .yield_us = raw_yield, .pending_rx = raw_pending, .user = (void *)sh};
    rc = nand_backup_wire_init(&backup_wire, &io);
    if (rc != 0) { mic_commands_release_external(); goto refused; }
    unsigned int key = irq_lock();
    atomic_clear(&backup_usb_fault);
    backup_usb_session_epoch = (uint32_t)atomic_get(&backup_usb_epoch);
    atomic_set(&backup_usb_active, 1);
    shell_set_bypass(sh, discard_input, NULL);
    irq_unlock(key);
    backup_result.end[NB_TRANSPORT_QUARANTINED] = 1;
    rc = nand_backup_wire_handshake(&backup_wire);
    if (rc != 0) {
        backup_result.end[NB_RC] = backup_result.end[NB_PRIMARY_RC] = backup_result.end[NB_USB_RC] = (uint32_t)rc;
        backup_result.end[NB_OUTCOME] = backup_result.end[NB_PRIMARY_OUTCOME] = NAND_BLOCK1024_USB_ERROR;
        return 0; /* No NAND START, but retain quarantine and shared reservation. */
    }
    const struct nand_block1024_sink sink = {.user = &backup_wire,
        .start = sink_start, .begin = sink_begin, .page = sink_page};
    rc = nand_block1024_probe(&backup_result, &sink);
    if ((rc == -EALREADY || rc == -EBUSY) && !backup_wire.deadline_armed) {
        /* No driver ownership was acquired. In particular block_entry
         * contention can leave the result untouched (possibly belonging to
         * an active caller). Do not read it, invent a hardware fault, emit END
         * or release our terminal/reservation. Separate reconciliation only.
         * A post-arm -EBUSY can instead be an unsafe restoration result; that
         * genuine failed result follows the ordinary bounded END path. */
        backup_wire.last_rc = NAND_BACKUP_WIRE_STATE;
        backup_wire.transport_usable = false;
        backup_wire.data_stopped = true;
        backup_wire.quarantined = true;
        backup_wire.console_reuse_allowed = false;
        return 0;
    }
    nand_id_get_state(&state);
    bool clean = clean_result(&state);
    backup_result.end[NB_TRANSPORT_QUARANTINED] = 1;
    rc = nand_backup_wire_finish(&backup_wire, backup_result.end, clean);
    if (rc != 0 && backup_result.end[NB_USB_RC] == 0) {
        backup_result.end[NB_USB_RC] = (uint32_t)rc; /* Cached only; END already frozen. */
    }
    if (clean && rc == 0 && backup_wire.clean_end_acknowledged && backup_wire.console_reuse_allowed) {
        bool pending = true;
        key = irq_lock();
        int pending_rc = pending_locked(sh, &pending);
        if (pending_rc == 0 && !pending) {
            backup_result.end[NB_TRANSPORT_QUARANTINED] = 0;
            shell_set_bypass(sh, NULL, NULL);
            mic_commands_release_external();
            atomic_clear(&backup_usb_active);
        } else {
            backup_wire.quarantined = true;
            backup_wire.console_reuse_allowed = false;
            backup_wire.last_rc = pending_rc != 0 ? NAND_BACKUP_WIRE_IO : NAND_BACKUP_WIRE_PROTOCOL;
            if (backup_result.end[NB_USB_RC] == 0) {
                backup_result.end[NB_USB_RC] = (uint32_t)backup_wire.last_rc;
            }
        }
        irq_unlock(key);
    }
    return 0; /* No printf after binary. Shell emits a prompt only after clean release. */
refused:
    shell_print(sh, "NAND_BACKUP_REFUSED rc=%d", rc);
    return rc;
}
