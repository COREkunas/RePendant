/* Fixed block1024 qualification adapter. No generic parameters or raw export. */
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
#include "nand_qualify.h"
#include "nand_qualification_wire.h"
#include "nand_qualification_commands.h"

int pendant_button_read(void);

BUILD_ASSERT(IS_ENABLED(CONFIG_SHELL_BACKEND_SERIAL_API_INTERRUPT_DRIVEN) &&
             !IS_ENABLED(CONFIG_SHELL_BACKEND_SERIAL_API_POLLING) &&
             !IS_ENABLED(CONFIG_SHELL_BACKEND_SERIAL_API_ASYNC) &&
             !IS_ENABLED(CONFIG_SHELL_BACKEND_SERIAL_FORCE_TX_BLOCKING_MODE),
             "Qualification requires the reviewed synchronous-copy nonblocking backend");
BUILD_ASSERT(!IS_ENABLED(CONFIG_LOG) && !IS_ENABLED(CONFIG_CONSOLE) &&
             !IS_ENABLED(CONFIG_PRINTK) && !IS_ENABLED(CONFIG_MCUMGR_TRANSPORT_SHELL),
             "No asynchronous or alternate binary/text producer is admitted");
BUILD_ASSERT(CONFIG_SHELL_BACKEND_SERIAL_RX_RING_BUFFER_SIZE == 64 &&
             CONFIG_SHELL_BACKEND_SERIAL_TX_RING_BUFFER_SIZE == 8 &&
             CONFIG_USB_CDC_ACM_RINGBUF_SIZE == 1024 && CONFIG_SHELL_STACK_SIZE == 4096,
             "Reviewed transport geometry/stack changed");
BUILD_ASSERT(NAND_QUALIFY_RESULT_WORDS == NAND_QUALIFICATION_WIRE_WORDS &&
             NQ_EVENT_COUNT == 97 && NAND_QUALIFY_DATA_MS == NAND_QUALIFICATION_WIRE_DATA_MS,
             "Qualification interface differs from fixed protocol1");
BUILD_ASSERT(IS_ENABLED(CONFIG_USB_WORKQUEUE) && CONFIG_USB_WORKQUEUE_PRIORITY < 0 &&
             !IS_ENABLED(CONFIG_SMP),
             "Pending-RX snapshot requires single-core cooperative USB callbacks");

/* Permanent metadata only. DMA buffers remain owned by nand_id.c, not the shell.
 * The shell is the only admitted caller, and external reservation excludes mic,
 * BLE capture, pairing, recovery, LED override and other local diagnostics.
 */
static struct nand_qualification_wire qualification_wire;
static struct nand_qualify_result qualification_result = {
    .words = {[NQ_RAM_SCRUBBED]=1, [NQ_STOPPED]=1, [NQ_BUS_RELEASED]=1,
              [NQ_ROW_INDEX]=UINT32_MAX, [NQ_LAST_OPERATION]=UINT32_MAX,
              [NQ_BLOCK_QUARANTINED]=1},
};
static atomic_t qualification_usb_epoch;
static atomic_t qualification_usb_active;
static atomic_t qualification_usb_fault;
static uint32_t qualification_usb_session_epoch;

void nand_qualification_usb_status(enum usb_dc_status_code status, const uint8_t *param)
{
    ARG_UNUSED(param);
    /* SOF is ordinary traffic. Every other status denotes a lifecycle,
     * configuration, endpoint-halt or unknown transition; fail closed if it
     * occurs during binary mode. CDC reset clears rx_ready without clearing
     * its RX ring, so that cached flag alone cannot prove an empty queue.
     * Normal CDC line-control requests do not emit these lifecycle events.
     */
    if (status != USB_DC_SOF) {
        atomic_inc(&qualification_usb_epoch);
        if (atomic_get(&qualification_usb_active)) { atomic_set(&qualification_usb_fault, 1); }
    }
}
static bool link_valid(void)
{
    return atomic_get(&qualification_usb_active) && !atomic_get(&qualification_usb_fault) &&
           (uint32_t)atomic_get(&qualification_usb_epoch) == qualification_usb_session_epoch;
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
    if (!backend_valid(sh) || size > NAND_QUALIFICATION_WIRE_IO_MAX) { return -EINVAL; }
    if (!link_valid()) { return -EIO; }
    int rc = sh->iface->api->write(sh->iface, bytes, size, accepted);
    return link_valid() ? rc : -EIO; /* Preserve accepted prefix on late fault. */
}
static int raw_read(void *user, uint8_t *bytes, size_t size, size_t *received)
{
    const struct shell *sh = user;
    *received = 0;
    if (!backend_valid(sh) || size > NAND_QUALIFICATION_WIRE_IO_MAX) { return -EINVAL; }
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

static int observer_start(void *user,int64_t deadline)
{
    return nand_qualification_wire_arm_data_deadline(user,(uint32_t)deadline);
}
static int observer_check(void *user,int64_t deadline)
{
    struct nand_qualification_wire *w=user;
    if(!w->deadline_armed || w->data_deadline_ms!=(uint32_t)deadline)
        return NAND_QUALIFICATION_WIRE_STATE;
    return nand_qualification_wire_check(w);
}
static int observer_event(void *user,const struct nand_qualify_result *snapshot,int64_t deadline)
{
    struct nand_qualification_wire *w=user;
    if(snapshot==NULL || !w->deadline_armed || w->data_deadline_ms!=(uint32_t)deadline)
        return NAND_QUALIFICATION_WIRE_STATE;
    return nand_qualification_wire_progress(w,snapshot->words);
}
static bool clean_result(const struct nand_id_state *s)
{
    return nand_qualification_wire_clean_result(qualification_result.words) &&
        !s->busy && !s->fault_latched && s->clock_stopped && s->bus_released &&
        s->cs_configured && s->cs_high && s->aux_configured && s->aux_high;
}
/* Cached finite metadata only; no active read/hash/array command. Failure
 * sessions intentionally cannot reach this through the quarantined terminal.
 * The full98 words are preserved by binary END, not repeated as shell text. */
int command_nand_qualification_status(const struct shell *sh,size_t argc,char **argv)
{
    ARG_UNUSED(argv);
    if(sh!=shell_backend_uart_get_ptr() || argc!=1U) return -EINVAL;
    const uint32_t *f=qualification_result.words;
    shell_print(sh,"NAND_QUALIFY_STATUS attempted=%u busy=%u rc=%d outcome=%u "
        "phase=%u events=%u preimage_rows=%u blank_rows=%u verify_reads=%u "
        "erase_started=%u program_started=%u array_may_have_changed=%u "
        "block_quarantined=%u transport_quarantined=%u transport_rc=%d",
        f[NQ_ATTEMPTED],f[NQ_BUSY],(int32_t)f[NQ_RC],f[NQ_OUTCOME],f[NQ_PHASE],
        f[NQ_EVENT_COUNT],f[NQ_PREIMAGE_ROWS],f[NQ_BLANK_ROWS],f[NQ_VERIFY_READS],
        f[NQ_ERASE_STARTED],f[NQ_PROGRAM_STARTED],f[NQ_ARRAY_MAY_HAVE_CHANGED],
        f[NQ_BLOCK_QUARANTINED],qualification_wire.quarantined,qualification_wire.last_rc);
    return 0;
}
int command_nand_qualification(const struct shell *sh,size_t argc,char **argv)
{
    if(sh!=shell_backend_uart_get_ptr()) return -EINVAL;
    int rc=-EINVAL;
    if(argc!=2U || strcmp(argv[1],"confirm")!=0 || !backend_valid(sh)) goto refused;
    rc=mic_commands_reserve_external();
    if(rc!=0) goto refused;
    struct nand_id_state state;
    nand_id_get_state(&state);
    if(state.attempted || state.busy || state.fault_latched || pendant_button_read()!=0 ||
       qualification_wire.handshake_started) {
        mic_commands_release_external(); rc=-EBUSY; goto refused;
    }
    const struct nand_qualification_wire_io io={.write=raw_write,.read=raw_read,
        .now_ms=raw_now,.yield_us=raw_yield,.pending_rx=raw_pending,.user=(void*)sh};
    rc=nand_qualification_wire_init(&qualification_wire,&io);
    if(rc!=0) { mic_commands_release_external(); goto refused; }
    unsigned int key=irq_lock();
    atomic_clear(&qualification_usb_fault);
    qualification_usb_session_epoch=(uint32_t)atomic_get(&qualification_usb_epoch);
    atomic_set(&qualification_usb_active,1);
    shell_set_bypass(sh,discard_input,NULL);
    irq_unlock(key);
    rc=nand_qualification_wire_handshake(&qualification_wire);
    if(rc!=0) return 0; /* No NAND; keep terminal and reservation quarantined. */
    const struct nand_qualify_observer observer={.user=&qualification_wire,
        .start=observer_start,.check=observer_check,.event=observer_event};
    rc=nand_block1024_qualify(&qualification_result,&observer);
    if((rc==-EALREADY || rc==-EBUSY) && !qualification_wire.deadline_armed) {
        /* Entry contention can leave the result untouched. Never infer success
         * or reset driver fault/ownership from stale metadata. Owned post-START
         * restore failures can legitimately return -EBUSY and report END. */
        qualification_wire.last_rc=NAND_QUALIFICATION_WIRE_STATE;
        qualification_wire.transport_usable=false; qualification_wire.data_stopped=true;
        qualification_wire.quarantined=true; qualification_wire.console_reuse_allowed=false;
        return 0;
    }
    nand_id_get_state(&state);
    bool clean=clean_result(&state);
    rc=nand_qualification_wire_finish(&qualification_wire,qualification_result.words,clean);
    if(clean && rc==0 && qualification_wire.clean_end_acknowledged &&
       qualification_wire.console_reuse_allowed) {
        bool pending=true;
        key=irq_lock();
        int pending_rc=pending_locked(sh,&pending);
        if(pending_rc==0 && !pending) {
            /* This only releases the terminal. The tested block remains
             * quarantined; original content was not automatically restored. */
            shell_set_bypass(sh,NULL,NULL);
            mic_commands_release_external();
            atomic_clear(&qualification_usb_active);
        } else {
            qualification_wire.quarantined=true;
            qualification_wire.console_reuse_allowed=false;
            qualification_wire.last_rc=pending_rc!=0?NAND_QUALIFICATION_WIRE_IO:NAND_QUALIFICATION_WIRE_PROTOCOL;
        }
        irq_unlock(key);
    }
    return 0; /* No shell text until automatic prompt on exact clean release. */
refused:
    shell_print(sh,"NAND_QUALIFY_REFUSED rc=%d",rc);
    return rc;
}
