/* SPDX-License-Identifier: Apache-2.0 */
/* OVERRIDE_celt_fatal is an upstream-supported override, not a vendor patch.
 * Diagnostic pointers may reference future private audio state. Never inspect,
 * format, copy or export them. No cleanup can safely assume a valid Opus stack.
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>

#if !defined(CONFIG_REBOOT) || !defined(CONFIG_RESET_ON_FATAL_ERROR) || \
    !defined(CONFIG_STACK_CANARIES_STRONG)
#error "Opus fatal path requires cold reboot and Zephyr strong stack protection"
#endif
#if defined(CONFIG_PRINTK) || defined(CONFIG_LOG) || defined(CONFIG_COVERAGE) || \
    defined(CONFIG_COVERAGE_DUMP) || defined(CONFIG_COVERAGE_DUMP_SEMIHOST)
#error "Opus fatal path must not log or export coverage"
#endif

FUNC_NORETURN void celt_fatal(const char *message, const char *file, int line)
{
    (void)message;
    (void)file;
    (void)line;
    sys_reboot(SYS_REBOOT_COLD);

    /* The pinned Zephyr sys_reboot is itself noreturn and has an IRQ-locked
     * endless idle fallback if sys_arch_reboot fails. This second fallback
     * also keeps a returning port/test shim from returning to damaged codec
     * state. A real GCC build may elide it due to sys_reboot's declaration.
     */
    (void)irq_lock();
    for (;;) {
        k_cpu_idle();
    }
}
