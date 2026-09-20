/* SPDX-License-Identifier: Apache-2.0 */
#include "long_recording_control_codec.h"
#include <string.h>

static int span(const void *p,size_t n)
{return p && n && (uintptr_t)p<=UINTPTR_MAX-n;}
static int separate(const void *a,size_t an,const void *b,size_t bn)
{return span(a,an)&&span(b,bn)&&((uintptr_t)a+an<=(uintptr_t)b||(uintptr_t)b+bn<=(uintptr_t)a);}
static int zero(const uint8_t *p,size_t n)
{for(size_t i=0;i<n;++i)if(p[i])return 0;return 1;}
static int identity(const uint8_t *p,size_t n)
{unsigned any=0,notff=0;for(size_t i=0;i<n;++i){any|=p[i];notff|=(unsigned)(p[i]^255U);}return any&&notff;}
static uint16_t get16(const uint8_t *p){return (uint16_t)((uint16_t)p[0]|(uint16_t)p[1]<<8);}
static uint32_t get32(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put16(uint8_t *p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(v>>(8U*i));}
static int request_valid(const struct lc_request *r)
{
 if(!r->sequence||!identity(r->binding,32))return 0;
 if(r->command==LC_STATUS){
  if(r->mode)return 0;
  return (identity(r->boot,16)||zero(r->boot,16))&&
   (identity(r->operation,16)||zero(r->operation,16))&&
   (!zero(r->boot,16)||zero(r->operation,16));
 }
 return (r->command==LC_START||r->command==LC_STOP)&&identity(r->boot,16)&&
  identity(r->operation,16)&&(r->command==LC_START?(r->mode==1U||r->mode==2U):r->mode==0U);
}
int lc_parse_request(const uint8_t *p,size_t n,const uint8_t binding[32],
 const uint8_t boot[16],struct lc_request *out)
{
 if(n!=LC_REQUEST_BYTES||!separate(p,n,out,sizeof(*out))||
  !separate(binding,32,out,sizeof(*out))||!separate(boot,16,out,sizeof(*out)))return LC_ARGUMENT;
 if(!identity(binding,32)||!identity(boot,16)||p[0]!='O'||p[1]!='P'||p[2]!=1||get16(p+6)!=72||
  !zero(p+73,7))return LC_INVALID;
 struct lc_request r;memset(&r,0,sizeof(r));
 r.command=p[3];r.sequence=get16(p+4);r.mode=p[72];
 memcpy(r.boot,p+8,16);memcpy(r.operation,p+24,16);memcpy(r.binding,p+40,32);
 if(!request_valid(&r)||memcmp(r.binding,binding,32)||(!zero(r.boot,16)&&memcmp(r.boot,boot,16)))return LC_INVALID;
 *out=r;return LC_OK;
}
int lc_validate_state(const struct lc_state *s)
{
 if(!span(s,sizeof(*s)))return LC_ARGUMENT;
 unsigned f=s->flags;
 if(!identity(s->boot,16)||s->phase>LC_FAULT||s->reason>12||f&~LC_FLAGS_MASK||
  s->epoch>UINT32_C(0x7fffffff)||s->accepted_frames>LC_MAX_FRAMES||
  s->committed_frames>s->accepted_frames||s->admitted_frames>LC_MAX_FRAMES||s->admitted_frames%500U||
  (!(f&LC_CAPACITY_KNOWN)&&s->admitted_frames)||
  ((f&LC_CAPACITY_KNOWN)&&s->accepted_frames>s->admitted_frames)||
  ((f&LC_MIC_ON)&&(f&LC_PRODUCER_JOINED))||
  ((f&LC_RELEASED)&&(!(f&LC_PRODUCER_JOINED)||!(f&LC_STORAGE_JOINED)||(f&LC_MIC_ON))))return LC_INVALID;
 if((f&LC_BUTTON_ARMED)&&(s->phase!=LC_STARTING||(f&(LC_SESSION_CREATED|LC_MIC_ON|LC_STOP_REQUESTED))||
    s->epoch||s->accepted_frames||s->committed_frames))return LC_INVALID;
 if((!identity(s->recording,16)&&!zero(s->recording,16))||
  (!identity(s->operation,16)&&!zero(s->operation,16)))return LC_INVALID;
 if(s->phase==LC_IDLE){
  return zero(s->operation,16)&&zero(s->recording,16)&&!s->epoch&&!s->reason&&
   !s->accepted_frames&&!s->committed_frames&&
   (f&(LC_PRODUCER_JOINED|LC_STORAGE_JOINED|LC_RELEASED))==44U&&
   !(f&(LC_MIC_ON|LC_FINALIZED|LC_SESSION_CREATED|LC_FAULT_LATCH|LC_STOP_REQUESTED))?LC_OK:LC_INVALID;
 }
 if(!identity(s->operation,16))return LC_INVALID;
 if(s->phase==LC_FAULT)return (f&LC_FAULT_LATCH)&&s->reason&&!(f&LC_FINALIZED)?LC_OK:LC_INVALID;
 if(f&LC_FAULT_LATCH)return LC_INVALID;
 if(s->phase==LC_NO_CAPACITY)return s->reason==11&&zero(s->recording,16)&&!s->epoch&&
  !s->accepted_frames&&!s->committed_frames&&!s->admitted_frames&&
  (f&(LC_PRODUCER_JOINED|LC_STORAGE_JOINED|LC_RELEASED|LC_CAPACITY_KNOWN))==300U&&
  !(f&(LC_MIC_ON|LC_FINALIZED|LC_SESSION_CREATED))?LC_OK:LC_INVALID;
 if(s->phase==LC_CANCELLED_BEFORE_START)return s->reason==1&&zero(s->recording,16)&&!s->epoch&&
  !s->accepted_frames&&!s->committed_frames&&
  (f&(LC_PRODUCER_JOINED|LC_STORAGE_JOINED|LC_RELEASED))==44U&&
  !(f&(LC_MIC_ON|LC_FINALIZED|LC_SESSION_CREATED))?LC_OK:LC_INVALID;
 if(s->phase==LC_STARTING)return !s->reason&&!s->accepted_frames&&!s->committed_frames&&
  !(f&(LC_MIC_ON|LC_FINALIZED))&&
  ((f&LC_SESSION_CREATED)?(s->epoch&&identity(s->recording,16)&&(f&LC_CAPACITY_KNOWN)&&s->admitted_frames&&!(f&LC_RELEASED)):
                          (!s->epoch&&zero(s->recording,16)))?LC_OK:LC_INVALID;
 if(s->phase==LC_STOPPING&&!(f&LC_SESSION_CREATED))return s->reason==1&&!s->epoch&&zero(s->recording,16)&&
  !s->accepted_frames&&!s->committed_frames&&!(f&(LC_MIC_ON|LC_FINALIZED|LC_RELEASED))?LC_OK:LC_INVALID;
 if(!identity(s->recording,16)||!s->epoch||!(f&LC_SESSION_CREATED)||!(f&LC_CAPACITY_KNOWN)||
  !s->admitted_frames)return LC_INVALID;
 if(s->phase==LC_RUNNING)return !s->reason&&(f&LC_MIC_ON)&&(f&(LC_USB_PRESENT|LC_PORTABLE))&&
  !(f&(LC_PRODUCER_JOINED|LC_FINALIZED|LC_RELEASED))?LC_OK:LC_INVALID;
 if(s->phase==LC_STOPPED)return (s->reason==1||s->reason==11||(s->reason==2&&(f&LC_PORTABLE)))&&
  s->accepted_frames==s->committed_frames&&
  (f&(LC_PRODUCER_JOINED|LC_STORAGE_JOINED|LC_FINALIZED|LC_RELEASED))==60U&&!(f&LC_MIC_ON)&&
  (s->reason!=11||s->accepted_frames==s->admitted_frames)?LC_OK:LC_INVALID;
 if(s->reason!=1&&s->reason!=11&&!(s->reason==2&&(f&LC_PORTABLE)))return LC_INVALID;
 if(f&(LC_FINALIZED|LC_RELEASED))return LC_INVALID;
 if(s->phase==LC_DRAINING&&(!(f&LC_PRODUCER_JOINED)||(f&LC_MIC_ON)))return LC_INVALID;
 return LC_OK;
}
static int selected(const struct lc_request *r,const struct lc_state *s)
{
 return request_valid(r)&&lc_validate_state(s)==LC_OK&&
  (zero(r->boot,16)||!memcmp(r->boot,s->boot,16))&&
  (zero(r->operation,16)||!memcmp(r->operation,s->operation,16))&&
  (r->command!=LC_START||s->phase!=LC_IDLE)&&
  (r->command!=LC_STOP||(s->phase!=LC_IDLE&&
   ((s->flags&LC_STOP_REQUESTED)||(s->phase!=LC_STARTING&&s->phase!=LC_RUNNING))));
}
int lc_encode_response(uint8_t *out,size_t n,const struct lc_request *r,const struct lc_state *s)
{
 if(n!=LC_RESPONSE_BYTES||!separate(out,n,r,sizeof(*r))||!separate(out,n,s,sizeof(*s)))return LC_ARGUMENT;
 if(!selected(r,s))return LC_INVALID;
 uint8_t p[LC_RESPONSE_BYTES]={0};p[0]='O';p[1]='P';p[2]=1;p[3]=(uint8_t)(r->command|128U);
 put16(p+4,r->sequence);put16(p+6,73);/* status0 only: no error body accepted */
 memcpy(p+9,s->boot,16);memcpy(p+25,s->operation,16);memcpy(p+41,s->recording,16);
 p[57]=s->phase;p[58]=s->reason;put16(p+59,s->flags);put32(p+61,s->epoch);
 put32(p+65,s->accepted_frames);put32(p+69,s->committed_frames);put32(p+73,s->admitted_frames);
 memcpy(out,p,sizeof(p));return LC_OK;
}
int lc_parse_response(const uint8_t *p,size_t n,const struct lc_request *r,struct lc_state *out)
{
 if(n!=LC_RESPONSE_BYTES||!separate(p,n,out,sizeof(*out))||!separate(r,sizeof(*r),out,sizeof(*out)))return LC_ARGUMENT;
 if(p[0]!='O'||p[1]!='P'||p[2]!=1||p[3]!=(uint8_t)(r->command|128U)||
  get16(p+4)!=r->sequence||get16(p+6)!=73||p[8]||!zero(p+77,4))return LC_INVALID;
 struct lc_state s;memset(&s,0,sizeof(s));memcpy(s.boot,p+9,16);memcpy(s.operation,p+25,16);memcpy(s.recording,p+41,16);
 s.phase=p[57];s.reason=p[58];s.flags=get16(p+59);s.epoch=get32(p+61);
 s.accepted_frames=get32(p+65);s.committed_frames=get32(p+69);s.admitted_frames=get32(p+73);
 if(!selected(r,&s))return LC_INVALID;
 *out=s;return LC_OK;
}
int lc_same_operation_progress(const struct lc_state *a,const struct lc_state *b)
{
 if(lc_validate_state(a)||lc_validate_state(b))return LC_INVALID;
 if(!identity(a->operation,16)||memcmp(a->boot,b->boot,16)||memcmp(a->operation,b->operation,16)||
  (!zero(a->recording,16)&&memcmp(a->recording,b->recording,16))||
  ((a->flags^b->flags)&LC_PORTABLE)||
  (a->epoch&&a->epoch!=b->epoch)||a->accepted_frames>b->accepted_frames||
  a->committed_frames>b->committed_frames||a->phase>b->phase||
  (a->reason&&b->phase!=LC_FAULT&&a->reason!=b->reason)||
  ((a->flags&LC_SESSION_CREATED)&&!(b->flags&LC_SESSION_CREATED))||
  ((a->flags&LC_CAPACITY_KNOWN)&&(!(b->flags&LC_CAPACITY_KNOWN)||a->admitted_frames!=b->admitted_frames))||
  (b->phase==LC_NO_CAPACITY&&a->phase!=LC_STARTING&&a->phase!=LC_NO_CAPACITY)||
  (b->phase==LC_CANCELLED_BEFORE_START&&(a->flags&LC_SESSION_CREATED)))return LC_INVALID;
 if(a->phase>=LC_STOPPED&&(a->phase!=b->phase||a->reason!=b->reason||a->epoch!=b->epoch||
  a->accepted_frames!=b->accepted_frames||a->committed_frames!=b->committed_frames||
  a->admitted_frames!=b->admitted_frames||((a->flags^b->flags)&~LC_USB_PRESENT)))return LC_INVALID;
 return LC_OK;
}
