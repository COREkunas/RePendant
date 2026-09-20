/* Explicit public-fixture test; outputs never come from recordings or keys. */
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_uart.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>
#include "crypto_commands.h"
#include "crypto_public_test.h"
#include "mic_commands.h"

#define CRYPTO_TIMEOUT_MS 20000u
#define CRYPTO_WORKER_STACK 16384u
#define CRYPTO_MIN_UNUSED 1024u
#define CRYPTO_WORKER_PRIORITY 14
#define CRYPTO_SUPERVISOR_PRIORITY 4
/* Keep command service ahead of CPU-heavy work, without lowering timeout
 * supervision or changing the higher-priority Bluetooth/USB threads. */
_Static_assert(CONFIG_SHELL_THREAD_PRIORITY_OVERRIDE == 1 &&
               CONFIG_SHELL_THREAD_PRIORITY == 13 &&
               CRYPTO_SUPERVISOR_PRIORITY == 4 && CRYPTO_WORKER_PRIORITY == 14 &&
               CRYPTO_SUPERVISOR_PRIORITY < CONFIG_SHELL_THREAD_PRIORITY &&
               CONFIG_SHELL_THREAD_PRIORITY < CRYPTO_WORKER_PRIORITY,
               "Review the supervisor/shell/crypto priority order before changing it");
enum crypto_state { CRYPTO_IDLE, CRYPTO_RUNNING, CRYPTO_FINISHING,
                    CRYPTO_DONE, CRYPTO_FAILED, CRYPTO_EXPIRED };
static atomic_t crypto_state;
static atomic_t crypto_initialized = ATOMIC_INIT(-EAGAIN);
static atomic_t crypto_reserved;
static struct crypto_public_result public_result;
static int terminal_error;
static uint32_t terminal_elapsed, terminal_unused, terminal_stack_valid;
static int64_t started_ms, deadline_ms;
static struct k_work_q crypto_timeout_queue;
static struct k_work_delayable crypto_timeout_work;
static struct k_work_sync crypto_cancel_sync;
K_THREAD_STACK_DEFINE(crypto_timeout_stack, 2048);
K_SEM_DEFINE(crypto_request, 0, 1);
int pendant_button_read(void);

static void wipe_public(void)
{
    volatile uint8_t *p = &public_result.containers[0][0];
    for (size_t n = 0; n < sizeof(public_result.containers); ++n) { p[n] = 0; }
}

static void crypto_timeout_handler(struct k_work *work)
{
    (void)work;
    if (atomic_cas(&crypto_state, CRYPTO_RUNNING, CRYPTO_EXPIRED)) {
        sys_reboot(SYS_REBOOT_COLD);
    }
}

/* Only this worker or the timeout can expire a committed run. Never release
 * exclusion, print, or publish output after expiry, even in a returning shim. */
static int worker_expired(int expected)
{
    int64_t now = k_uptime_get();
    if (now < started_ms || now >= deadline_ms) {
        if (atomic_cas(&crypto_state, expected, CRYPTO_EXPIRED)) {
            sys_reboot(SYS_REBOOT_COLD);
        }
        return 1;
    }
    return atomic_get(&crypto_state) != expected;
}

static void release_exclusion(void)
{
    mic_commands_release_external();
    atomic_clear(&crypto_reserved);
}

static void worker_once(void)
{
    if (atomic_get(&crypto_state) != CRYPTO_RUNNING ||
        worker_expired(CRYPTO_RUNNING)) { return; }
    int rc = crypto_public_test_run(&public_result);
    if (worker_expired(CRYPTO_RUNNING) ||
        !atomic_cas(&crypto_state, CRYPTO_RUNNING, CRYPTO_FINISHING)) { return; }
    /* Handler has no waits/locks and cannot newly expire FINISHING. This
     * synchronization must finish before release or immutable publication. */
    (void)k_work_cancel_delayable_sync(&crypto_timeout_work, &crypto_cancel_sync);
    size_t unused = 0;
    int stack_rc = k_thread_stack_space_get(k_current_get(), &unused);
    if (worker_expired(CRYPTO_FINISHING)) { return; }
    int64_t finished_ms = k_uptime_get();
    /* Validate the final signed sample before narrowing: a backwards jump
     * must not wrap into a plausible small elapsed value. */
    if (finished_ms < started_ms || finished_ms >= deadline_ms) {
        if (atomic_cas(&crypto_state, CRYPTO_FINISHING, CRYPTO_EXPIRED)) {
            sys_reboot(SYS_REBOOT_COLD);
        }
        return;
    }
    terminal_elapsed = (uint32_t)(finished_ms - started_ms);
    terminal_stack_valid = stack_rc == 0 && unused <= CRYPTO_WORKER_STACK;
    terminal_unused = terminal_stack_valid ? (uint32_t)unused : 0u;
    int stack_safe = terminal_stack_valid && unused >= CRYPTO_MIN_UNUSED;
    int valid = rc == 0 && public_result.error == 0 &&
                public_result.completed == CRYPTO_PUBLIC_CONTAINER_COUNT &&
                public_result.owner_faulted == 0;
    terminal_error = !stack_safe ? -EOVERFLOW : (valid ? 0 : -EIO);
    if (!valid || !stack_safe) { wipe_public(); }
    /* Defense against a broken result contract: finite metadata only. */
    if (public_result.completed > CRYPTO_PUBLIC_CONTAINER_COUNT) {
        public_result.completed = 0;
    }
    public_result.owner_faulted = public_result.owner_faulted != 0;
    if (!public_result.owner_faulted && stack_safe) { release_exclusion(); }
    atomic_set(&crypto_state, valid && stack_safe ? CRYPTO_DONE : CRYPTO_FAILED);
}

static void crypto_worker(void *a, void *b, void *c)
{
    (void)a; (void)b; (void)c;
    for (;;) {
        if (k_sem_take(&crypto_request, K_FOREVER) == 0) { worker_once(); }
    }
}
K_THREAD_DEFINE(crypto_worker_id, CRYPTO_WORKER_STACK, crypto_worker,
                NULL, NULL, NULL, CRYPTO_WORKER_PRIORITY, 0, 0);

int crypto_commands_init(void)
{
    if (!atomic_cas(&crypto_initialized, -EAGAIN, -EBUSY)) { return -EALREADY; }
    k_work_queue_init(&crypto_timeout_queue);
    k_work_init_delayable(&crypto_timeout_work, crypto_timeout_handler);
    k_work_queue_start(&crypto_timeout_queue, crypto_timeout_stack,
                       K_THREAD_STACK_SIZEOF(crypto_timeout_stack),
                       K_PRIO_PREEMPT(CRYPTO_SUPERVISOR_PRIORITY), NULL);
    atomic_clear(&crypto_initialized);
    return 0;
}

static int refused(const struct shell *sh, int error)
{
    shell_print(sh, "CRYPTO_REFUSED rc=%d", error);
    return error;
}

static int command_crypto_status(const struct shell *sh, size_t argc, char **argv)
{
    (void)argv;
    if (sh != shell_backend_uart_get_ptr() || argc != 1u) { return -EINVAL; }
    int state = (int)atomic_get(&crypto_state);
    int terminal = state == CRYPTO_DONE || state == CRYPTO_FAILED;
    shell_print(sh, "CRYPTO_STATUS state=%d error=%d core_error=%d completed=%u owner_faulted=%u elapsed_ms=%u stack_unused=%u stack_valid=%u reservation_held=%u timeout_ms=20000 public_only=1",
                state, terminal ? terminal_error : 0, terminal ? public_result.error : 0,
                terminal ? (unsigned int)public_result.completed : 0u,
                terminal ? (unsigned int)public_result.owner_faulted : 0u,
                terminal ? (unsigned int)terminal_elapsed : 0u,
                terminal ? (unsigned int)terminal_unused : 0u,
                terminal ? (unsigned int)terminal_stack_valid : 0u,
                atomic_get(&crypto_reserved) != 0 ? 1u : 0u);
    return 0;
}

static int command_crypto_run(const struct shell *sh, size_t argc, char **argv)
{
    if (sh != shell_backend_uart_get_ptr()) { return -EINVAL; }
    if (argc != 2u || strcmp(argv[1], "public-confirm") != 0) {
        return refused(sh, -EINVAL);
    }
    if (atomic_get(&crypto_initialized) != 0 ||
        atomic_get(&crypto_state) != CRYPTO_IDLE) { return refused(sh, -EBUSY); }
    int rc = mic_commands_reserve_external();
    if (rc != 0) { return refused(sh, rc); }
    if (pendant_button_read() != 0 || pendant_recovery_is_pending()) {
        mic_commands_release_external();
        return refused(sh, -EBUSY);
    }
    started_ms = k_uptime_get();
    if (started_ms < 0 || started_ms > INT64_MAX - CRYPTO_TIMEOUT_MS) {
        mic_commands_release_external();
        return refused(sh, -EOVERFLOW);
    }
    deadline_ms = started_ms + CRYPTO_TIMEOUT_MS;
    atomic_set(&crypto_reserved, 1);
    if (!atomic_cas(&crypto_state, CRYPTO_IDLE, CRYPTO_RUNNING)) {
        release_exclusion();
        return refused(sh, -EBUSY);
    }
    /* Arm absolute time, not a fresh relative allowance after scheduler delay. */
    rc = k_work_schedule_for_queue(&crypto_timeout_queue, &crypto_timeout_work,
                                   K_TIMEOUT_ABS_MS(deadline_ms));
    if (rc != 1) {
        if (!atomic_cas(&crypto_state, CRYPTO_RUNNING, CRYPTO_FINISHING)) {
            return refused(sh, -ETIMEDOUT);
        }
        (void)k_work_cancel_delayable_sync(&crypto_timeout_work, &crypto_cancel_sync);
        if (worker_expired(CRYPTO_FINISHING)) { return -ETIMEDOUT; }
        terminal_error = -EIO;
        release_exclusion();
        atomic_set(&crypto_state, CRYPTO_FAILED);
        return refused(sh, -EIO);
    }
    k_sem_give(&crypto_request);
    shell_print(sh, "CRYPTO_ACCEPTED public_only=1 timeout_ms=20000");
    return 0;
}

static int command_crypto_result(const struct shell *sh, size_t argc, char **argv)
{
    (void)argv;
    if (sh != shell_backend_uart_get_ptr() || argc != 1u) { return -EINVAL; }
    if (atomic_get(&crypto_state) != CRYPTO_DONE) { return refused(sh, -EBUSY); }
    static const char digits[] = "0123456789abcdef";
    shell_print(sh, "CRYPTO_RESULT containers=2 bytes_each=238 public_only=1");
    for (unsigned int i = 0; i < CRYPTO_PUBLIC_CONTAINER_COUNT; ++i) {
        for (unsigned int offset = 0; offset < CRYPTO_PUBLIC_CONTAINER_BYTES; offset += 32u) {
            char hex[65];
            unsigned int count = CRYPTO_PUBLIC_CONTAINER_BYTES - offset;
            if (count > 32u) { count = 32u; }
            for (unsigned int n = 0; n < count; ++n) {
                uint8_t value = public_result.containers[i][offset + n];
                hex[2u * n] = digits[value >> 4];
                hex[2u * n + 1u] = digits[value & 15u];
            }
            hex[2u * count] = '\0';
            shell_print(sh, "CRYPTO_DATA index=%u offset=%u bytes=%u hex=%s", i, offset, count, hex);
        }
    }
    shell_print(sh, "CRYPTO_RESULT_END containers=2 bytes=476 public_only=1");
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(cryptodiag_commands,
    SHELL_CMD_ARG(status, NULL, "Public crypto diagnostic metadata; no work.", command_crypto_status, 1, 0),
    SHELL_CMD_ARG(run, NULL, "Once per boot, public-confirm: synthetic data only.", command_crypto_run, 2, 0),
    SHELL_CMD_ARG(result, NULL, "Completed public synthetic containers only.", command_crypto_result, 1, 0),
    SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(cryptodiag, &cryptodiag_commands, "Public synthetic crypto test only.", NULL);
