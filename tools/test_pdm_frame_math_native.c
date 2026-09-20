/* Offline host-only wrapper: exercise the exact header compiled into firmware. */
#include "../usb_firmware/src/mic_frame_math.h"

__declspec(dllexport) int pdm_frame_math_test(int64_t signed_sum,
                                            uint64_t sum_square,
                                            uint64_t *output)
{
    return mic_frame_ac_numerator(signed_sum, sum_square, output) ? 1 : 0;
}
