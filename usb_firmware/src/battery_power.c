#include "battery_power.h"
/* TI AverageCurrent is positive into the battery. CHG is only permission.
 * Avoid idle/noise, full/discharge flags, stale/untrusted samples. No power
 * admission decision uses this cosmetic classification. */
int bp_charging(const struct bp_sample *s,uint64_t now)
{return bp_fresh(s,now)&&!(s->flags&0x0201U)&&s->current_ma>=5&&s->current_ma<=1000;}
int bp_fresh(const struct bp_sample *s,uint64_t t)
{return s&&s->valid==1&&s->sequence&&t>=s->at&&t-s->at<=BP_FRESH_MS&&
 s->mv>0&&s->mv<=6000&&s->soc<=100&&(s->flags&8U)&&!(s->flags&0xfc30U);}
int bp_start(const struct bp_sample *s,uint64_t t)
{return bp_fresh(s,t)&&s->mv>=3800&&s->mv<=4450&&s->soc>=25&&s->temp>=2781&&s->temp<=3131;}
int bp_continue(const struct bp_sample *s,uint64_t t)
{return bp_fresh(s,t)&&s->mv>=3700&&s->mv<=4450&&s->soc>=20&&s->temp>=2731&&s->temp<=3181;}
int bp_begin(struct bp_lease *l,const struct bp_sample *s,uint64_t t)
{if(!l||l->held||!bp_start(s,t))return 0;*l=(struct bp_lease){.held=1,.last=t};return 1;}
int bp_check(struct bp_lease *l,const struct bp_sample *s,uint64_t t)
{
 if(!l||!l->held||t<l->last)return 0;
 l->last=t;
 /* A real critical measurement overrides the grace window. Unknown/stale
  * data can only finish an already admitted save, never admit new work. */
 if(s&&((s->flags&0xc000U)||(s->valid&&(s->mv<3300||s->mv>4500||s->temp<2631||s->temp>3231))))return 0;
 if(!l->draining&&!bp_continue(s,t)){
  if(t>UINT64_MAX-BP_SAVE_MS)return 0;
  l->draining=1;l->drain_until=t+BP_SAVE_MS;
 }
 return !l->draining||t<l->drain_until;
}
