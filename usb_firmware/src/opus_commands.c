/* Explicit public-pattern benchmark; metadata only, never encoded payload. */
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_uart.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>
#include "opus_commands.h"
#include "opus_benchmark.h"
#include "mic_commands.h"

#define OPUS_COMMAND_TIMEOUT_MS 30000u
#define OPUS_COMMAND_WORKER_STACK 65536u
#define OPUS_COMMAND_MIN_UNUSED 1024u
#define OPUS_COMMAND_COUNTER_HZ 32768u
#define OPUS_COMMAND_WORKER_PRIORITY 14
#define OPUS_COMMAND_SUPERVISOR_PRIORITY 4
/* Lower numeric preemptible priorities run first: command replies must not
 * be starved by a ready benchmark, while timeout supervision stays higher. */
_Static_assert(CONFIG_SHELL_THREAD_PRIORITY_OVERRIDE == 1 &&
               CONFIG_SHELL_THREAD_PRIORITY == 13 &&
               OPUS_COMMAND_SUPERVISOR_PRIORITY == 4 &&
               OPUS_COMMAND_WORKER_PRIORITY == 14 &&
               OPUS_COMMAND_SUPERVISOR_PRIORITY < CONFIG_SHELL_THREAD_PRIORITY &&
               CONFIG_SHELL_THREAD_PRIORITY < OPUS_COMMAND_WORKER_PRIORITY,
               "Review the supervisor/shell/Opus priority order before changing it");
_Static_assert(CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC == OPUS_COMMAND_COUNTER_HZ,
               "Review the actual Opus elapsed counter before changing frequency");
_Static_assert(OPUS_COMMAND_WORKER_STACK >= OPUS_BENCHMARK_WORKER_STACK_MIN,
               "Opus worker is below the reviewed admission floor");
enum opus_command_state { OPUS_IDLE, OPUS_RUNNING, OPUS_FINISHING,
                          OPUS_DONE, OPUS_FAILED, OPUS_EXPIRED };
static atomic_t opus_command_state;
static atomic_t opus_initialized = ATOMIC_INIT(-EAGAIN);
static atomic_t opus_reserved;
static struct opus_benchmark_result opus_result;
static int opus_terminal_error;
static uint32_t opus_result_valid, opus_elapsed, opus_unused, opus_stack_valid;
static int64_t opus_started_ms, opus_deadline_ms;
static struct k_work_q opus_timeout_queue;
static struct k_work_delayable opus_timeout_work;
static struct k_work_sync opus_cancel_sync;
K_THREAD_STACK_DEFINE(opus_timeout_stack, 2048);
K_SEM_DEFINE(opus_request, 0, 1);
int pendant_button_read(void);

static void opus_timeout_handler(struct k_work *work)
{
    (void)work;
    if (atomic_cas(&opus_command_state, OPUS_RUNNING, OPUS_EXPIRED)) {
        sys_reboot(SYS_REBOOT_COLD);
    }
}

static int opus_worker_expired(int expected)
{
    int64_t now = k_uptime_get();
    if (now < opus_started_ms || now >= opus_deadline_ms) {
        if (atomic_cas(&opus_command_state, expected, OPUS_EXPIRED)) {
            sys_reboot(SYS_REBOOT_COLD);
        }
        return 1;
    }
    return atomic_get(&opus_command_state) != expected;
}

static uint32_t opus_wall_ms(void *user)
{
    (void)user;
    return k_uptime_get_32();
}

static uint32_t opus_counter_ticks(void *user)
{
    (void)user;
    return k_cycle_get_32();
}

static bool opus_cancelled(void *user)
{
    (void)user;
    int64_t now = k_uptime_get();
    return atomic_get(&opus_command_state) != OPUS_RUNNING ||
           now < opus_started_ms || now >= opus_deadline_ms;
}

static const struct opus_benchmark_platform opus_platform = {
    .wall_ms = opus_wall_ms, .cycles = opus_counter_ticks,
    .cancelled = opus_cancelled, .user = NULL,
    .counter_hz = OPUS_COMMAND_COUNTER_HZ,
    .worker_stack_bytes = OPUS_COMMAND_WORKER_STACK
};

/* Treat the core as a closed typed boundary: malformed or unconfirmed cleanup
 * cannot release exclusion or expose even misleading numeric success fields. */
static int opus_metadata_valid(int rc)
{
    if (rc > 0 || rc < OPUS_BENCHMARK_TIMEOUT || opus_result.rc != rc ||
        opus_result.counter_hz != OPUS_COMMAND_COUNTER_HZ ||
        opus_result.buffers_wiped != 1u ||
        opus_result.stage > OPUS_BENCHMARK_DONE ||
        opus_result.profile_index >= OPUS_BENCHMARK_PROFILE_COUNT ||
        opus_result.frame_index >= OPUS_BENCHMARK_FRAMES_PER_PROFILE ||
        opus_result.elapsed_ms >= OPUS_COMMAND_TIMEOUT_MS ||
        opus_result.frames_attempted > OPUS_BENCHMARK_TOTAL_FRAMES ||
        opus_result.frames_completed > opus_result.frames_attempted ||
        opus_result.frames_attempted - opus_result.frames_completed > 1u ||
        opus_result.profiles_completed > OPUS_BENCHMARK_PROFILE_COUNT) { return 0; }
    uint32_t attempted = 0, completed = 0;
    for (unsigned int i = 0; i < OPUS_BENCHMARK_PROFILE_COUNT; ++i) {
        const struct opus_benchmark_profile *p = &opus_result.profiles[i];
        uint32_t complexity = 2u * i + 1u;
        if ((p->complexity != 0u && p->complexity != complexity) ||
            p->frames_attempted > OPUS_BENCHMARK_FRAMES_PER_PROFILE ||
            p->frames_completed > p->frames_attempted ||
            (p->lookahead_samples != 0u && p->lookahead_samples != 40u) ||
            p->max_encode_ms >= 1000u ||
            p->max_encode_cycles >= OPUS_COMMAND_COUNTER_HZ ||
            p->min_encode_cycles > p->max_encode_cycles) { return 0; }
        if (p->frames_attempted != 0u &&
            (p->complexity != complexity || p->lookahead_samples != 40u)) { return 0; }
        if (p->frames_completed == 0u) {
            if (p->min_encode_cycles || p->max_encode_cycles ||
                p->total_encode_cycles || p->max_encode_ms) { return 0; }
        } else if (p->min_encode_cycles == 0u ||
                   p->total_encode_cycles < (uint64_t)p->frames_completed * p->min_encode_cycles ||
                   p->total_encode_cycles > (uint64_t)p->frames_completed * p->max_encode_cycles) {
            return 0;
        }
        attempted += p->frames_attempted; completed += p->frames_completed;
    }
    if (attempted != opus_result.frames_attempted || completed != opus_result.frames_completed ||
        opus_result.profiles_completed * OPUS_BENCHMARK_FRAMES_PER_PROFILE > completed) { return 0; }
    if (rc == 0 && (opus_result.stage != OPUS_BENCHMARK_DONE || opus_result.opus_rc != 0 ||
        opus_result.required_state_bytes <= 0 ||
        opus_result.required_state_bytes > (int32_t)OPUS_BENCHMARK_STATE_CAPACITY ||
        completed != OPUS_BENCHMARK_TOTAL_FRAMES ||
        opus_result.profiles_completed != OPUS_BENCHMARK_PROFILE_COUNT)) { return 0; }
    return 1;
}

static void opus_release_exclusion(void)
{
    mic_commands_release_external();
    atomic_clear(&opus_reserved);
}

static void opus_worker_once(void)
{
    if (atomic_get(&opus_command_state) != OPUS_RUNNING ||
        opus_worker_expired(OPUS_RUNNING)) { return; }
    int rc = opus_benchmark_run(&opus_platform, &opus_result);
    if (opus_worker_expired(OPUS_RUNNING) ||
        !atomic_cas(&opus_command_state, OPUS_RUNNING, OPUS_FINISHING)) { return; }
    (void)k_work_cancel_delayable_sync(&opus_timeout_work, &opus_cancel_sync);
    size_t unused = 0;
    int stack_rc = k_thread_stack_space_get(k_current_get(), &unused);
    if (opus_worker_expired(OPUS_FINISHING)) { return; }
    int64_t finished = k_uptime_get();
    if (finished < opus_started_ms || finished >= opus_deadline_ms) {
        if (atomic_cas(&opus_command_state, OPUS_FINISHING, OPUS_EXPIRED)) {
            sys_reboot(SYS_REBOOT_COLD);
        }
        return;
    }
    opus_elapsed = (uint32_t)(finished - opus_started_ms);
    opus_stack_valid = stack_rc == 0 && unused <= OPUS_COMMAND_WORKER_STACK;
    opus_unused = opus_stack_valid ? (uint32_t)unused : 0u;
    int stack_safe = opus_stack_valid && unused >= OPUS_COMMAND_MIN_UNUSED;
    opus_result_valid = (uint32_t)opus_metadata_valid(rc);
    opus_terminal_error = !stack_safe ? -EOVERFLOW : (!opus_result_valid ? -EIO : rc);
    if (!opus_result_valid) { memset(&opus_result, 0, sizeof(opus_result)); }
    /* Validation can be preempted after the supervisor was joined. Recheck
     * before releasing ownership or publishing a terminal result. */
    if (opus_worker_expired(OPUS_FINISHING)) { return; }
    if (stack_safe && opus_result_valid) { opus_release_exclusion(); }
    atomic_set(&opus_command_state,
               stack_safe && opus_result_valid && rc == 0 ? OPUS_DONE : OPUS_FAILED);
}

static void opus_worker(void *a, void *b, void *c)
{
    (void)a; (void)b; (void)c;
    for (;;) {
        if (k_sem_take(&opus_request, K_FOREVER) == 0) { opus_worker_once(); }
    }
}
K_THREAD_DEFINE(opus_worker_id, OPUS_COMMAND_WORKER_STACK, opus_worker,
                NULL, NULL, NULL, OPUS_COMMAND_WORKER_PRIORITY, 0, 0);

int opus_commands_init(void)
{
    if (!atomic_cas(&opus_initialized, -EAGAIN, -EBUSY)) { return -EALREADY; }
    k_work_queue_init(&opus_timeout_queue);
    k_work_init_delayable(&opus_timeout_work, opus_timeout_handler);
    k_work_queue_start(&opus_timeout_queue, opus_timeout_stack,
                       K_THREAD_STACK_SIZEOF(opus_timeout_stack),
                       K_PRIO_PREEMPT(OPUS_COMMAND_SUPERVISOR_PRIORITY), NULL);
    atomic_clear(&opus_initialized);
    return 0;
}

static int opus_refused(const struct shell *sh, int error)
{
    shell_print(sh, "OPUS_REFUSED rc=%d", error);
    return error;
}

static int command_opus_status(const struct shell *sh, size_t argc, char **argv)
{
    (void)argv;
    if (sh != shell_backend_uart_get_ptr() || argc != 1u) { return -EINVAL; }
    int state = (int)atomic_get(&opus_command_state);
    int terminal = state == OPUS_DONE || state == OPUS_FAILED;
    int valid = terminal && opus_result_valid;
    shell_print(sh, "OPUS_STATUS state=%d error=%d result_valid=%u core_rc=%d opus_rc=%d stage=%u frames_attempted=%u frames_completed=%u profiles_completed=%u buffers_wiped=%u elapsed_ms=%u stack_unused=%u stack_valid=%u reservation_held=%u timeout_ms=30000 counter_hz=32768 public_only=1",
                state, terminal ? opus_terminal_error : 0, valid ? 1u : 0u,
                valid ? (int)opus_result.rc : 0, valid ? (int)opus_result.opus_rc : 0,
                valid ? (unsigned)opus_result.stage : 0u,
                valid ? (unsigned)opus_result.frames_attempted : 0u,
                valid ? (unsigned)opus_result.frames_completed : 0u,
                valid ? (unsigned)opus_result.profiles_completed : 0u,
                valid ? (unsigned)opus_result.buffers_wiped : 0u,
                terminal ? (unsigned)opus_elapsed : 0u, terminal ? (unsigned)opus_unused : 0u,
                terminal ? (unsigned)opus_stack_valid : 0u,
                atomic_get(&opus_reserved) != 0 ? 1u : 0u);
    return 0;
}

static int command_opus_run(const struct shell *sh, size_t argc, char **argv)
{
    if (sh != shell_backend_uart_get_ptr()) { return -EINVAL; }
    if (argc != 2u || strcmp(argv[1], "public-confirm") != 0) { return opus_refused(sh, -EINVAL); }
    if (atomic_get(&opus_initialized) != 0 || atomic_get(&opus_command_state) != OPUS_IDLE) {
        return opus_refused(sh, -EBUSY);
    }
    int rc = mic_commands_reserve_external();
    if (rc != 0) { return opus_refused(sh, rc); }
    if (pendant_button_read() != 0 || pendant_recovery_is_pending()) {
        mic_commands_release_external(); return opus_refused(sh, -EBUSY);
    }
    opus_started_ms = k_uptime_get();
    if (opus_started_ms < 0 || opus_started_ms > INT64_MAX - OPUS_COMMAND_TIMEOUT_MS) {
        mic_commands_release_external(); return opus_refused(sh, -EOVERFLOW);
    }
    opus_deadline_ms = opus_started_ms + OPUS_COMMAND_TIMEOUT_MS;
    atomic_set(&opus_reserved, 1);
    if (!atomic_cas(&opus_command_state, OPUS_IDLE, OPUS_RUNNING)) {
        opus_release_exclusion(); return opus_refused(sh, -EBUSY);
    }
    rc = k_work_schedule_for_queue(&opus_timeout_queue, &opus_timeout_work,
                                   K_TIMEOUT_ABS_MS(opus_deadline_ms));
    if (rc != 1) {
        if (!atomic_cas(&opus_command_state, OPUS_RUNNING, OPUS_FINISHING)) {
            return opus_refused(sh, -ETIMEDOUT);
        }
        (void)k_work_cancel_delayable_sync(&opus_timeout_work, &opus_cancel_sync);
        if (opus_worker_expired(OPUS_FINISHING)) { return -ETIMEDOUT; }
        opus_terminal_error = -EIO;
        opus_release_exclusion();
        atomic_set(&opus_command_state, OPUS_FAILED);
        return opus_refused(sh, -EIO);
    }
    k_sem_give(&opus_request);
    shell_print(sh, "OPUS_ACCEPTED public_only=1 timeout_ms=30000 counter_hz=32768");
    return 0;
}

static int command_opus_result(const struct shell *sh, size_t argc, char **argv)
{
    (void)argv;
    if (sh != shell_backend_uart_get_ptr() || argc != 1u) { return -EINVAL; }
    int state = (int)atomic_get(&opus_command_state);
    if ((state != OPUS_DONE && state != OPUS_FAILED) || !opus_result_valid) {
        return opus_refused(sh, -EBUSY);
    }
    shell_print(sh, "OPUS_RESULT core_rc=%d opus_rc=%d required_state_bytes=%d stage=%u profile_index=%u frame_index=%u frames_attempted=%u frames_completed=%u profiles_completed=%u buffers_wiped=1 core_elapsed_ms=%u counter_hz=32768 public_only=1",
                (int)opus_result.rc, (int)opus_result.opus_rc, (int)opus_result.required_state_bytes,
                (unsigned)opus_result.stage, (unsigned)opus_result.profile_index, (unsigned)opus_result.frame_index,
                (unsigned)opus_result.frames_attempted, (unsigned)opus_result.frames_completed,
                (unsigned)opus_result.profiles_completed, (unsigned)opus_result.elapsed_ms);
    for (unsigned i = 0; i < OPUS_BENCHMARK_PROFILE_COUNT; ++i) {
        const struct opus_benchmark_profile *p = &opus_result.profiles[i];
        shell_print(sh, "OPUS_PROFILE index=%u complexity=%u frames_attempted=%u frames_completed=%u lookahead_samples=%u min_encode_ticks=%u max_encode_ticks=%u max_encode_ms=%u total_encode_ticks=%llu",
                    i, (unsigned)p->complexity, (unsigned)p->frames_attempted,
                    (unsigned)p->frames_completed, (unsigned)p->lookahead_samples,
                    (unsigned)p->min_encode_cycles, (unsigned)p->max_encode_cycles,
                    (unsigned)p->max_encode_ms, (unsigned long long)p->total_encode_cycles);
    }
    shell_print(sh, "OPUS_RESULT_END profiles=3 public_only=1");
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(opusdiag_commands,
    SHELL_CMD_ARG(status, NULL, "Public Opus metadata; no work.", command_opus_status, 1, 0),
    SHELL_CMD_ARG(run, NULL, "Once per boot, public-confirm: no microphone.", command_opus_run, 2, 0),
    SHELL_CMD_ARG(result, NULL, "Terminal public numeric metadata only.", command_opus_result, 1, 0),
    SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(opusdiag, &opusdiag_commands, "Public synthetic Opus benchmark only.", NULL);
