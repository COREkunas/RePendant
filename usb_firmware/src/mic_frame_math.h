/* Pure integer helper, shared by firmware and native offline tests. */
#ifndef OPENPENDANT_MIC_FRAME_MATH_H_
#define OPENPENDANT_MIC_FRAME_MATH_H_

#include <stdbool.h>
#include <stdint.h>

#define MIC_FRAME_MATH_SAMPLES 320U

/* Returns N*sum(x*x)-sum(x)^2 for one complete 320-sample int16 frame.
 * The output is unchanged on failure. No PCM or hardware access is required.
 * These moment bounds protect multiplication/subtraction, not authenticity.
 */
static inline bool mic_frame_ac_numerator(int64_t signed_sum,
					 uint64_t sum_square, uint64_t *output)
{
	if (output == 0 || signed_sum < -INT64_C(10485760) ||
	    signed_sum > INT64_C(10485440) ||
	    sum_square > UINT64_C(343597383680)) {
		return false;
	}
	uint64_t weighted_square = MIC_FRAME_MATH_SAMPLES * sum_square;
	uint64_t squared_sum = (uint64_t)(signed_sum * signed_sum);
	if (weighted_square < squared_sum) {
		return false;
	}
	*output = weighted_square - squared_sum;
	return true;
}

#endif
