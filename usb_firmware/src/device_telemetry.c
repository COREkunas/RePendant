/* SPDX-License-Identifier: Apache-2.0 */
#include "device_telemetry.h"
#include <string.h>
static void le(uint8_t *p, uint64_t v, unsigned n)
{ for (unsigned i = 0; i < n; ++i) { p[i] = (uint8_t)(v >> (i * 8)); } }
int device_telemetry_encode(const struct device_telemetry *s, uint8_t *out, size_t cap)
{
    if (!s || !out || cap < DEVICE_TELEMETRY_SIZE || s->uptime_ms > INT64_MAX ||
        (s->faults & ~3U)) { return -1; }
    memset(out, 0, DEVICE_TELEMETRY_SIZE);
    out[0] = 1; out[1] = DEVICE_TELEMETRY_SIZE;
    out[2] = 0x0bU | (s->led_known ? 0x04U : 0U);
    le(out + 4, s->uptime_ms, 8);
    le(out + 12, s->major, 2); le(out + 14, s->minor, 2); le(out + 16, s->patch, 2);
    out[18] = s->microphone_power; out[19] = s->resource_busy;
    if (s->led_known) { out[20] = s->red; out[21] = s->green; out[22] = s->blue; }
    /* 23 legacy short clips; 24 no volume; 25 no recipient provisioning;
     * 26 unavailable battery; 27 unknown charging; 28..43 unavailable capacity. */
    out[26] = 0xffU;
    le(out + 44, s->faults, 4);
    return 0;
}
int device_telemetry_encode_recorder(const struct device_telemetry *s,
    const struct recorder_telemetry *r, uint8_t *out, size_t cap)
{
    if (!r || !out || cap < DEVICE_TELEMETRY_RECORDER_SIZE ||
        (r->flags & ~1023U) || !(r->flags & RT_ENGINEERING)) return -1;
    if (r->flags & RT_WORKER) {
        if (r->state > 7 || r->reason > 12 || r->mode > 1 ||
            r->captured_samples % 320 || r->committed_samples % 320 ||
            r->committed_samples > r->captured_samples) return -1;
    } else if (r->state || r->reason || r->mode || r->captured_samples || r->committed_samples) return -1;
    if (r->flags & RT_CAPACITY) {
        if ((r->flags & (RT_MOUNTED|RT_SUSPENDED|RT_BUSY|RT_RECORDING|RT_FAULT)) != (RT_MOUNTED|RT_SUSPENDED) ||
            /* Layout admission must not depend on this application's patch
             * version: an app-only bug fix does not change storage geometry. */
            !r->roots_total || (r->roots_total > 8 && !(r->roots_total==32 && r->slots_total==5120)) ||
            r->roots_used > r->roots_total ||
            !r->slots_total || r->slots_used > r->slots_total) return -1;
    } else if (r->roots_used || r->roots_total || r->slots_used || r->slots_total) return -1;
    if (device_telemetry_encode(s, out, cap)) return -1;
    memset(out+48, 0, 24); out[0]=2; out[1]=DEVICE_TELEMETRY_RECORDER_SIZE;
    le(out+48,r->flags,2);out[50]=r->state;out[51]=r->reason;out[52]=r->mode;
    out[53]=r->roots_used;out[54]=r->roots_total;
    le(out+56,r->slots_used,2);le(out+58,r->slots_total,2);
    le(out+60,r->captured_samples,4);le(out+64,r->committed_samples,4);
    return 0;
}
int device_telemetry_encode_battery(const struct device_telemetry *s,
    const struct recorder_telemetry *r,const struct battery_telemetry *b,uint8_t *out,size_t cap)
{
    if(!b||(b->flags&~15U)||!(b->flags&4U)||((b->flags&2U)&&!(b->flags&1U))||
       ((b->flags&8U)&&(b->flags&3U)))return -1;
    if(b->flags&1U){
        if(!b->mv||b->mv>6000||b->soc>100||b->temp<2300||b->temp>3700||
           b->age_ms>20000||!b->sequence||!(b->gauge_flags&8U)||(b->gauge_flags&0xfc30U))return -1;
        if((b->flags&2U)&&(b->mv<3800||b->mv>4450||b->soc<25||b->temp<2781||b->temp>3131))return -1;
    }else if(b->mv||b->soc||b->temp||b->gauge_flags||b->age_ms||b->sequence)return -1;
    if(device_telemetry_encode_recorder(s,r,out,cap))return -1;
    out[0]=3;out[27]=b->flags;
    if(b->flags&1U){
        out[2]|=0x10U;out[26]=(uint8_t)b->soc;
        le(out+28,b->mv,2);le(out+30,b->temp,2);le(out+32,b->age_ms,4);
        le(out+36,b->sequence,4);le(out+40,b->gauge_flags,2);
    }
    return 0;
}
