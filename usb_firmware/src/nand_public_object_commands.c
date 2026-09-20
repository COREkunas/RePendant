/* Fixed public object adapter. No generic parameters or raw export. */
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
#include "nand_public_object.h"
#include "npo_wire.h"
#include "nand_public_object_commands.h"

int pendant_button_read(void);

BUILD_ASSERT(IS_ENABLED(CONFIG_SHELL_BACKEND_SERIAL_API_INTERRUPT_DRIVEN) &&
             !IS_ENABLED(CONFIG_SHELL_BACKEND_SERIAL_API_POLLING) &&
             !IS_ENABLED(CONFIG_SHELL_BACKEND_SERIAL_API_ASYNC) &&
             !IS_ENABLED(CONFIG_SHELL_BACKEND_SERIAL_FORCE_TX_BLOCKING_MODE),
             "Public object requires the reviewed synchronous-copy nonblocking backend");
BUILD_ASSERT(!IS_ENABLED(CONFIG_LOG) && !IS_ENABLED(CONFIG_CONSOLE) &&
             !IS_ENABLED(CONFIG_PRINTK) && !IS_ENABLED(CONFIG_MCUMGR_TRANSPORT_SHELL),
             "No asynchronous or alternate binary/text producer is admitted");
BUILD_ASSERT(CONFIG_SHELL_BACKEND_SERIAL_RX_RING_BUFFER_SIZE == 64 &&
             CONFIG_SHELL_BACKEND_SERIAL_TX_RING_BUFFER_SIZE == 8 &&
             CONFIG_USB_CDC_ACM_RINGBUF_SIZE == 1024 && CONFIG_SHELL_STACK_SIZE == 4096,
             "Reviewed transport geometry/stack changed");
BUILD_ASSERT(NPO_WORDS == NPO_WIRE_WORDS && NPO_DATA_MS == NPO_WIRE_DATA_MS,
             "Public object protocol differs");
BUILD_ASSERT(IS_ENABLED(CONFIG_USB_WORKQUEUE) && CONFIG_USB_WORKQUEUE_PRIORITY < 0 &&
             !IS_ENABLED(CONFIG_SMP),
             "Pending-RX snapshot requires single-core cooperative USB callbacks");

/* Permanent metadata only. DMA buffers remain owned by nand_id.c, not the shell.
 * The shell is the only admitted caller, and external reservation excludes mic,
 * BLE capture, pairing, recovery, LED override and other local diagnostics.
 */
static struct npo_wire public_object_wire;
static struct npo_result public_object_result = {
 .words = {[NP_RAM_SCRUBBED]=1,[NP_STOPPED]=1,[NP_BUS_RELEASED]=1,[NP_BLOCK_QUARANTINED]=1}
};
static atomic_t public_object_usb_epoch;
static atomic_t public_object_usb_active;
static atomic_t public_object_usb_fault;
static uint32_t public_object_usb_session_epoch;

void nand_public_object_usb_status(enum usb_dc_status_code status, const uint8_t *param)
{
    ARG_UNUSED(param);
    /* SOF is ordinary traffic. Every other status denotes a lifecycle,
     * configuration, endpoint-halt or unknown transition; fail closed if it
     * occurs during binary mode. CDC reset clears rx_ready without clearing
     * its RX ring, so that cached flag alone cannot prove an empty queue.
     * Normal CDC line-control requests do not emit these lifecycle events.
     */
    if (status != USB_DC_SOF) {
        atomic_inc(&public_object_usb_epoch);
        if (atomic_get(&public_object_usb_active)) { atomic_set(&public_object_usb_fault, 1); }
    }
}
static bool link_valid(void)
{
    return atomic_get(&public_object_usb_active) && !atomic_get(&public_object_usb_fault) &&
           (uint32_t)atomic_get(&public_object_usb_epoch) == public_object_usb_session_epoch;
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
    if (!backend_valid(sh) || size > NPO_WIRE_IO_MAX) { return -EINVAL; }
    if (!link_valid()) { return -EIO; }
    int rc = sh->iface->api->write(sh->iface, bytes, size, accepted);
    return link_valid() ? rc : -EIO; /* Preserve accepted prefix on late fault. */
}
static int raw_read(void *user, uint8_t *bytes, size_t size, size_t *received)
{
    const struct shell *sh = user;
    *received = 0;
    if (!backend_valid(sh) || size > NPO_WIRE_IO_MAX) { return -EINVAL; }
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


/* First check arms the immutable global deadline. Event receives its shorter
 * 2-second bound; wire progress independently clips to the same global bound. */
static int observer_check(void *user,int64_t deadline)
{
 struct npo_wire *w=user;
 if(!w->deadline_armed){
  int rc=npo_wire_arm_data_deadline(w,(uint32_t)deadline);if(rc)return rc;
 }
 if(w->data_deadline_ms!=(uint32_t)deadline)return NPO_WIRE_STATE;
 return npo_wire_check(w);
}
static int observer_event(void *user,const struct npo_result *r,int64_t deadline)
{
 struct npo_wire *w=user;
 if(!r||!w->deadline_armed||k_uptime_get()>=deadline||
    deadline>k_uptime_get()+NPO_WIRE_FRAME_MS)return NPO_WIRE_STATE;
 return npo_wire_progress(w,r->words);
}
static bool clean_result(const struct nand_id_state *s)
{
 return npo_wire_clean_result(public_object_result.words)&&!s->busy&&!s->fault_latched&&
  s->clock_stopped&&s->bus_released&&s->cs_configured&&s->cs_high&&s->aux_configured&&s->aux_high;
}
int command_nand_public_object_status(const struct shell *sh,size_t argc,char **argv)
{
 ARG_UNUSED(argv);if(sh!=shell_backend_uart_get_ptr()||argc!=1)return -EINVAL;
 const uint32_t *w=public_object_result.words;
 shell_print(sh,"NAND_PUBLIC_STATUS rc=%d outcome=%u mode=%u committed=%u programs=%u array_may_change=%u fault=%u restored=%u block_quarantined=1 transport_quarantined=%u",
  (int32_t)w[NP_RC],w[NP_OUTCOME],w[NP_MODE],w[NP_COMMITTED],w[NP_PROGRAM_EXECUTES],
  w[NP_ARRAY_MAY_CHANGE],w[NP_FAULT],w[NP_RESTORED],public_object_wire.quarantined);
 return 0;
}
int command_nand_public_object(const struct shell *sh,size_t argc,char **argv)
{
 if(sh!=shell_backend_uart_get_ptr())return -EINVAL;
 int rc=-EINVAL;enum npo_mode mode;
 if(argc!=3||strcmp(argv[2],"confirm")||!backend_valid(sh))goto refused;
 if(!strcmp(argv[1],"write"))mode=NPO_WRITE_ONCE;
 else if(!strcmp(argv[1],"recover"))mode=NPO_RECOVER_ONLY;
 else goto refused;
 rc=mic_commands_reserve_external();if(rc)goto refused;
 struct nand_id_state state;nand_id_get_state(&state);
 if(state.busy||state.fault_latched||pendant_button_read()!=0||
   (mode==NPO_WRITE_ONCE&&state.attempted)||
   (public_object_wire.handshake_started&&!public_object_wire.console_reuse_allowed)){
  mic_commands_release_external();rc=-EBUSY;goto refused;
 }
 const struct npo_wire_io io={.write=raw_write,.read=raw_read,.now_ms=raw_now,
  .yield_us=raw_yield,.pending_rx=raw_pending,.user=(void*)sh};
 rc=npo_wire_init(&public_object_wire,&io);
 if(rc){mic_commands_release_external();goto refused;}
 unsigned int key=irq_lock();
 atomic_clear(&public_object_usb_fault);
 public_object_usb_session_epoch=(uint32_t)atomic_get(&public_object_usb_epoch);
 atomic_set(&public_object_usb_active,1);shell_set_bypass(sh,discard_input,NULL);irq_unlock(key);
 rc=npo_wire_handshake(&public_object_wire);
 if(rc)return 0; /* Failure stays quarantined; never parse queued bytes as commands. */
 const struct npo_observer observer={.user=&public_object_wire,.check=observer_check,.event=observer_event};
 rc=nand_public_object_run(&public_object_result,mode,&observer);
 if((rc==-EBUSY||rc==-EALREADY)&&!public_object_wire.deadline_armed){
  public_object_wire.last_rc=NPO_WIRE_STATE;public_object_wire.transport_usable=false;
  public_object_wire.data_stopped=true;public_object_wire.quarantined=true;
  public_object_wire.console_reuse_allowed=false;return 0;
 }
 nand_id_get_state(&state);bool clean=clean_result(&state);
 rc=npo_wire_finish(&public_object_wire,public_object_result.words,clean);
 if(clean&&!rc&&public_object_wire.clean_end_acknowledged&&public_object_wire.console_reuse_allowed){
  bool pending=true;key=irq_lock();int pending_rc=pending_locked(sh,&pending);
  if(!pending_rc&&!pending){
   shell_set_bypass(sh,NULL,NULL);mic_commands_release_external();atomic_clear(&public_object_usb_active);
  }else{
   public_object_wire.quarantined=true;public_object_wire.console_reuse_allowed=false;
   public_object_wire.last_rc=pending_rc?NPO_WIRE_IO:NPO_WIRE_PROTOCOL;
  }
  irq_unlock(key);
 }
 return 0;
refused:
 shell_print(sh,"NAND_PUBLIC_REFUSED rc=%d",rc);return rc;
}
SHELL_CMD_ARG_REGISTER(nandobject,NULL,"Fixed public block1024 object: write/recover confirm.",
 command_nand_public_object,3,0);
SHELL_CMD_ARG_REGISTER(nandobjectstatus,NULL,"Cached public-object metadata only.",
 command_nand_public_object_status,1,0);
