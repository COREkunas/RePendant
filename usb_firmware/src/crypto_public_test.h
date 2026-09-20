#ifndef OPENPENDANT_CRYPTO_PUBLIC_TEST_H
#define OPENPENDANT_CRYPTO_PUBLIC_TEST_H
#include <stdint.h>
#define CRYPTO_PUBLIC_CONTAINER_BYTES 238u
#define CRYPTO_PUBLIC_CONTAINER_COUNT 2u
struct crypto_public_result {
    uint8_t containers[CRYPTO_PUBLIC_CONTAINER_COUNT][CRYPTO_PUBLIC_CONTAINER_BYTES];
    uint32_t completed;
    int error;
    int owner_faulted;
};
/* Local synthetic diagnostic only. Serial caller, valid exclusive output.
 * Runs at most once per boot, using one permanent PSA owner. No peripheral,
 * file, output, owner key, enrollment or recording input. Only a public RFC
 * recipient and public known-answer inputs. Repeated/null calls return -7
 * without touching output. Any accepted failure wipes both public containers.
 * Caller must supervise runtime, retain reservation on owner_faulted and only
 * publish output after completion. Success does NOT prove RNG quality.
 */
int crypto_public_test_run(struct crypto_public_result *out);
#endif
