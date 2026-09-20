/* Numeric, bounded profiling only. RTC ticks, not CPU execution cycles.
 * Single-core runtime owns this structure; readers require actor exclusion.
 * Wall intervals include preemption. Codec overlap is measured separately.
 * Saturation prevents a long session from silently wrapping aggregates. */
#ifndef OPENPENDANT_RECORDING_TIMING_H
#define OPENPENDANT_RECORDING_TIMING_H
#include <stdint.h>
#define RT_STAGES 10U
struct rt_stage { uint32_t calls,ticks,codec_ticks; };
struct recording_timing {
 uint32_t codec_calls,codec_ticks,codec_max,job_calls,job_ticks,job_max;
 struct rt_stage stages[RT_STAGES];
};
static inline uint32_t rt_add(uint32_t a,uint32_t b)
{return b>UINT32_MAX-a?UINT32_MAX:a+b;}
static inline void rt_codec(struct recording_timing *p,uint32_t ticks)
{p->codec_calls=rt_add(p->codec_calls,1);p->codec_ticks=rt_add(p->codec_ticks,ticks);
 if(ticks>p->codec_max)p->codec_max=ticks;}
static inline void rt_job(struct recording_timing *p,uint32_t ticks)
{p->job_calls=rt_add(p->job_calls,1);p->job_ticks=rt_add(p->job_ticks,ticks);
 if(ticks>p->job_max)p->job_max=ticks;}
static inline void rt_stage(struct recording_timing *p,uint32_t stage,uint32_t ticks,uint32_t codec)
{if(stage>=RT_STAGES)return;
 struct rt_stage *s=&p->stages[stage];s->calls=rt_add(s->calls,1);
 s->ticks=rt_add(s->ticks,ticks);s->codec_ticks=rt_add(s->codec_ticks,codec);}
#endif
