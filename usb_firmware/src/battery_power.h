#ifndef OPENPENDANT_BATTERY_POWER_H
#define OPENPENDANT_BATTERY_POWER_H
#include <stdint.h>
/* Conservative engineering policy, not a calibrated runtime estimator.
 * SOC never substitutes for measured voltage. USB does not make an unknown
 * battery safe. After a warning, only a fixed bounded save window remains. */
#define BP_FRESH_MS 20000U
#define BP_SAVE_MS 32000U
struct bp_sample { uint64_t at;uint16_t mv,temp,soc,flags;uint32_t valid,sequence;int16_t current_ma; };
int bp_charging(const struct bp_sample*,uint64_t);
struct bp_lease { uint64_t last,drain_until;uint32_t held,draining; };
int bp_fresh(const struct bp_sample*,uint64_t);
int bp_start(const struct bp_sample*,uint64_t);
int bp_continue(const struct bp_sample*,uint64_t);
int bp_begin(struct bp_lease*,const struct bp_sample*,uint64_t);
int bp_check(struct bp_lease*,const struct bp_sample*,uint64_t);
#endif
