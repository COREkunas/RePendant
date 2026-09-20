#ifndef OPENPENDANT_RECORDING_AEAD_SELFTEST_H
#define OPENPENDANT_RECORDING_AEAD_SELFTEST_H
#include <stddef.h>
#include <stdint.h>
/* Public known-answer test, no microphone/storage/private-owner-key operation.
 * Exclusive scratch is always wiped. PSA must already be initialized.
 * No caller may continue to recording after failure. */
int recording_aead_selftest(uint8_t *scratch,size_t capacity);
#endif
