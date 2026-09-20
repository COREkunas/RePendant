/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OPENPENDANT_OPUS_SILK_FATAL_OVERRIDE_H
#define OPENPENDANT_OPUS_SILK_FATAL_OVERRIDE_H

/* Forced into the Opus target only, after command-line definitions. GNU SILK
 * has no pre-definition hook for silk_assert: include its unmodified typedef
 * once, then redirect silk_fatal. Its original enabled silk_assert macro is
 * preserved, including exactly one evaluation of the tested condition.
 * The header guard prevents later includes restoring the default fatal macro.
 * Unused upstream _silk_fatal must not survive compilation: archive audit
 * rejects it, stdio and abort rather than assuming dead-code elimination.
 */
#if !defined(ENABLE_ASSERTIONS) || !defined(FIXED_POINT) || \
    !defined(OPENPENDANT_SILK_FATAL_REDIRECT)
#error "SILK fatal redirect requires the reviewed enabled fixed-point assertions"
#endif
#include "silk/typedef.h"

/* Matches celt/arch.h exactly; never include that header here because CELT_C
 * is set later by celt.c. No diagnostics are evaluated, copied or exported.
 */
#if defined(__GNUC__)
__attribute__((noreturn))
#elif defined(_MSC_VER)
__declspec(noreturn)
#endif
void celt_fatal(const char *message, const char *file, int line);

#undef silk_fatal
#define silk_fatal(ignored_message) \
    celt_fatal((const char *)0, (const char *)0, 0)

#endif
