/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OPENPENDANT_BUTTON_GESTURE_H
#define OPENPENDANT_BUTTON_GESTURE_H
#include <stdint.h>
/* Pure, bounded physical-input decoder. No I/O. Five taps consume the entire
 * sequence; incomplete multi-taps never fall back to a recording command.
 * Single taps wait600ms so the first press cannot accidentally start capture.
 * GPIO debounce and held-at-boot suppression match the existing button policy. */
enum bg_event { BG_NONE=0, BG_PRESS=1, BG_SINGLE=2, BG_FIVE=3 };
struct button_gesture {
 uint64_t last,changed,pressed,deadline,first;
 uint8_t seen,candidate,stable,released,count;
};
static inline enum bg_event bg_sample(struct button_gesture *g,int pressed,uint64_t t)
{
 if((pressed!=0&&pressed!=1)||(g->seen&&t<g->last)||t>UINT64_MAX-4000U){
  *g=(struct button_gesture){0};return BG_NONE;
 }
 g->last=t;
 if(!g->seen){
  g->seen=1;g->candidate=g->stable=(uint8_t)pressed;g->released=pressed==0;
  g->changed=g->pressed=t;return BG_NONE;
 }
 if(g->candidate!=(uint8_t)pressed){g->candidate=(uint8_t)pressed;g->changed=t;}
 /* A sixth/additional tap is part of the consumed gesture until600ms quiet. */
 if(g->count==5){
  if(pressed)g->deadline=t+600U;
  if(g->stable!=g->candidate&&t-g->changed>=50U)g->stable=g->candidate;
  if(!pressed&&!g->stable&&t>=g->deadline){g->count=0;g->released=1;}
  return BG_NONE;
 }
 if(g->stable!=g->candidate&&t-g->changed>=50U){
  g->stable=g->candidate;
  if(pressed){
   g->pressed=t;
   /* A delayed sampler cannot reinterpret an expired partial sequence. */
   if(g->count&&t>=g->deadline){g->count=0;g->released=0;}
   if(!g->count&&g->released){g->first=t;return BG_PRESS;}
  }else{
   uint64_t held=t-g->pressed;
   if(g->released&&held>=50U&&held<=1000U&&t-g->first<=3500U){
    ++g->count;g->deadline=t+600U;
    if(g->count==5)return BG_FIVE;
   }else g->count=0;
   g->released=1;
  }
 }
 if(g->count&&!g->stable&&!g->candidate&&t>=g->deadline){
  unsigned count=g->count;g->count=0;return count==1?BG_SINGLE:BG_NONE;
 }
 return BG_NONE;
}
#endif
