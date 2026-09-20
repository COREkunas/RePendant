/* SPDX-License-Identifier: Apache-2.0 -- synthetic input; never microphone data. */
#ifndef OPENPENDANT_RECORDING_PUBLIC_PATTERN_H
#define OPENPENDANT_RECORDING_PUBLIC_PATTERN_H
#include <stdint.h>
/* Fixed, bounded 320-sample calculation. A 200 Hz triangle mixed with changing
 * seeded broadband noise, three public amplitude levels, no secret/random API.
 * Produces [-13500,13500] at 16 kHz; no silence fast path or int16 overflow. */
static inline void recording_public_pattern(uint32_t frame,int16_t out[320])
{
 uint32_t rng=UINT32_C(0x6d2b79f5)^frame;
 for(uint32_t i=0;i<320;++i){
  rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;
  int32_t noise=(int32_t)(rng&UINT32_C(0x3fff))-8192;
  int32_t phase=(int32_t)(i%80U),triangle=phase<40?phase:80-phase;
  int32_t value=noise+(triangle-20)*256;
  uint32_t scale=1U+(frame/25U)%3U;
  out[i]=(int16_t)(value*(int32_t)scale/3);
 }
}
#endif
