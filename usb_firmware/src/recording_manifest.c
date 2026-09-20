/* SPDX-License-Identifier: Apache-2.0 */
#include "recording_manifest.h"
#include <limits.h>
#include <stdbool.h>
#include <string.h>

static bool overlap(const void *a,size_t an,const void *b,size_t bn)
{uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return an>UINTPTR_MAX-x||bn>UINTPTR_MAX-y||(an&&bn&&x<y+bn&&y<x+an);}
static void put(uint8_t *p,uint64_t value,unsigned bytes)
{for(unsigned i=0;i<bytes;++i)p[i]=(uint8_t)(value>>(8U*i));}
static void wipe(void *p,size_t bytes){volatile uint8_t *b=p;while(bytes--)*b++=0;}
static bool identity(const struct es_binding *a,const struct es_binding *b)
{return a->generation==b->generation&&!memcmp(a->key_fingerprint,b->key_fingerprint,32)&&
 !memcmp(a->device_id,b->device_id,16)&&!memcmp(a->volume_id,b->volume_id,16)&&!memcmp(a->recording_id,b->recording_id,16);}
static bool digest_valid(const uint8_t d[32])
{unsigned zero=0,ff=0;for(unsigned i=0;i<32;++i){zero|=d[i];ff|=(unsigned)(d[i]^255U);}return zero&&ff;}

int recording_manifest_build(uint8_t *out,size_t bytes,uint8_t digest[32],
 const struct ros_record *r,const struct ros_segment *segments,size_t count,
 const struct owned_page_hash *hash)
{
 if(!out||!digest||!r||!hash||!hash->sha256||count>RM_MAX_SEGMENTS||(!segments&&count)||
    bytes!=RM_HEADER_BYTES+count*RM_ENTRY_BYTES)return RM_ARGUMENT;
 const void *spans[]={out,digest,r,segments,hash};
 const size_t sizes[]={bytes,32,sizeof(*r),count*sizeof(*segments),sizeof(*hash)};
 for(unsigned i=0;i<5;++i)for(unsigned j=i+1;j<5;++j)
  if(overlap(spans[i],sizes[i],spans[j],sizes[j]))return RM_ARGUMENT;
 if(r->present!=1||r->segments!=count||r->profile!=2||r->mode>1||r->blocked>1||
    r->finalized>1||r->interrupted>1||r->finalized+r->interrupted>1||
    r->identity.segment_sequence||r->identity.plaintext_bytes||r->first_sample>INT64_MAX||
    r->next_sample>INT64_MAX||r->next_sample<r->first_sample||r->committed_samples>INT64_MAX)return RM_INVALID;
 struct es_binding check=r->identity;check.plaintext_bytes=1;uint8_t header[ES_HEADER_BYTES];
 if(es_header_build(header,sizeof(header),&check))return RM_INVALID;
 uint64_t total=0,next=r->first_sample;uint32_t gaps=0,root=0,slots=0;
 for(size_t i=0;i<count;++i){const struct ros_segment *s=&segments[i];
  if(!identity(&r->identity,&s->identity)||s->identity.segment_sequence!=i||s->profile!=2||
     s->root>=ROS_ROOTS||s->slot>=ROS_SLOTS||s->gap>1||!s->samples||s->samples>160000U||s->samples%320U||
     s->first_sample>INT64_MAX||s->next_sample>INT64_MAX||s->first_sample<next||
     s->next_sample<=s->first_sample||s->next_sample-s->first_sample!=s->samples||
     s->gap!=(uint32_t)(s->first_sample>next)||!digest_valid(s->digest)||
     s->container_bytes!=80U+(s->samples/320U+1U)*68U+209U||
     s->identity.plaintext_bytes!=s->container_bytes-209U||s->pages!=(s->container_bytes+2047U)/2048U||
     (slots&(UINT32_C(1)<<s->slot))||(i&&s->root!=root)||total>INT64_MAX-s->samples)return RM_INVALID;
  root=s->root;slots|=UINT32_C(1)<<s->slot;next=s->next_sample;total+=s->samples;gaps+=s->gap;
 }
 if(total!=r->committed_samples||next!=r->next_sample||gaps!=r->gaps)return RM_INVALID;
 memset(out,0,bytes);memcpy(out,"OPNDMF1",7);put(out+8,1,2);put(out+10,128,2);put(out+12,64,2);
 uint32_t flags=r->finalized|(r->interrupted<<1U);put(out+14,flags,2);
 memcpy(out+16,r->identity.device_id,16);memcpy(out+32,r->identity.volume_id,16);put(out+48,r->identity.generation,8);
 memcpy(out+56,r->identity.recording_id,16);put(out+72,count*4U+flags,8);
 memcpy(out+80,r->identity.key_fingerprint,32);put(out+112,count,4);put(out+116,2,4);put(out+120,total,8);
 for(size_t i=0;i<count;++i){const struct ros_segment *s=&segments[i];uint8_t *p=out+128+i*64;
  put(p,i,4);put(p+4,s->container_bytes,4);put(p+8,s->first_sample,8);put(p+16,s->next_sample,8);
  put(p+24,s->samples,4);put(p+28,s->gap,4);memcpy(p+32,s->digest,32);
 }
 size_t actual=0;int rc=hash->sha256(hash->user,out,bytes,digest,32,&actual);
 if(rc||actual!=32){wipe(out,bytes);wipe(digest,32);return RM_HASH;}
 return RM_OK;
}

static int full_segment_valid(const struct ros_segment *s)
{
 return s->identity.segment_sequence<RLL_SLOTS&&s->root<RLL_ROOTS&&s->slot<RLL_SLOTS&&s->profile==2&&s->gap<=1&&
 s->samples&&s->samples<=160000U&&!(s->samples%320U)&&s->first_sample<=INT64_MAX&&s->next_sample<=INT64_MAX&&
 s->next_sample>s->first_sample&&s->next_sample-s->first_sample==s->samples&&digest_valid(s->digest)&&
 s->container_bytes==80U+(s->samples/320U+1U)*68U+209U&&s->identity.plaintext_bytes==s->container_bytes-209U&&
 s->pages==(s->container_bytes+2047U)/2048U;
}
int recording_manifest_entry(uint8_t out[64],const struct ros_segment *s)
{
 if(!out||!s||overlap(out,64,s,sizeof(*s)))return RM_ARGUMENT;
 if(!full_segment_valid(s))return RM_INVALID;
 put(out,s->identity.segment_sequence,4);put(out+4,s->container_bytes,4);put(out+8,s->first_sample,8);
 put(out+16,s->next_sample,8);put(out+24,s->samples,4);put(out+28,s->gap,4);memcpy(out+32,s->digest,32);return 0;
}
int recording_manifest_stream(uint8_t output[128],uint8_t digest[32],const struct ros_record *r,const struct rm_stream_port *p)
{
 if(!output||!digest||!r||!p||!p->begin||!p->update||!p->finish||!p->abort||!p->segment)return RM_ARGUMENT;
 const void *spans[]={output,digest,r,p};const size_t sizes[]={128,32,sizeof(*r),sizeof(*p)};
 for(unsigned i=0;i<4;++i)for(unsigned j=i+1;j<4;++j)if(overlap(spans[i],sizes[i],spans[j],sizes[j]))return RM_ARGUMENT;
 if(r->present!=1||r->segments>RLL_SLOTS||r->profile!=2||r->mode>1||r->blocked>1||r->finalized>1||r->interrupted>1||
 r->finalized+r->interrupted>1||r->identity.segment_sequence||r->identity.plaintext_bytes||r->first_sample>INT64_MAX||
 r->next_sample>INT64_MAX||r->next_sample<r->first_sample||r->committed_samples>INT64_MAX)return RM_INVALID;
 struct es_binding binding=r->identity;binding.plaintext_bytes=1;uint8_t header[128],sum[32],entry_bytes[64];
 if(es_header_build(header,128,&binding))return RM_INVALID;
 memset(header,0,128);memcpy(header,"OPNDMF2",7);put(header+8,2,2);put(header+10,128,2);put(header+12,64,2);
 uint32_t flags=r->finalized|(r->interrupted<<1U);put(header+14,flags,2);
 memcpy(header+16,r->identity.device_id,16);memcpy(header+32,r->identity.volume_id,16);put(header+48,r->identity.generation,8);
 memcpy(header+56,r->identity.recording_id,16);put(header+72,(uint64_t)r->segments*4U+flags,8);
 memcpy(header+80,r->identity.key_fingerprint,32);put(header+112,r->segments,4);put(header+116,2,4);put(header+120,r->committed_samples,8);
 uint8_t slots[RLL_SLOTS/8U]={0};uint64_t total=0,next=r->first_sample;uint32_t gaps=0,root=0;
 int rc=RM_HASH;
 if(p->begin(p->user)||p->update(p->user,header,128))goto done;
 for(uint32_t i=0;i<r->segments;++i){struct ros_segment s;
  memset(&s,0,sizeof(s));rc=RM_INVALID;
  if(p->segment(p->user,i,&s)||!identity(&r->identity,&s.identity)||s.identity.segment_sequence!=i||
   !full_segment_valid(&s)||s.first_sample<next||s.gap!=(uint32_t)(s.first_sample>next)||
   (slots[s.slot/8U]&(1U<<(s.slot%8U)))||(i&&s.root!=root)||total>INT64_MAX-s.samples)goto done;
  slots[s.slot/8U]|=(uint8_t)(1U<<(s.slot%8U));root=s.root;next=s.next_sample;total+=s.samples;gaps+=s.gap;
  if(recording_manifest_entry(entry_bytes,&s))goto done;
  rc=RM_HASH;if(p->update(p->user,entry_bytes,64))goto done;
 }
 rc=RM_INVALID;if(total!=r->committed_samples||next!=r->next_sample||gaps!=r->gaps)goto done;
 rc=RM_HASH;if(p->finish(p->user,sum))goto done;
 rc=0;
done:
 p->abort(p->user);
 if(!rc){memcpy(output,header,128);memcpy(digest,sum,32);}
 wipe(header,sizeof(header));wipe(sum,sizeof(sum));wipe(entry_bytes,sizeof(entry_bytes));wipe(slots,sizeof(slots));return rc;
}

int recording_manifest_start(struct rm_incremental *c,const struct ros_record *r,uint8_t out[128])
{
 if(!c||!r||!out||overlap(c,sizeof(*c),r,sizeof(*r))||overlap(c,sizeof(*c),out,128)||
  overlap(r,sizeof(*r),out,128)||c->state)return RM_ARGUMENT;
 if(r->present!=1||r->segments>RLL_SLOTS||r->profile!=2||r->mode>1||r->blocked>1||r->finalized>1||r->interrupted>1||
  r->finalized+r->interrupted>1||r->identity.segment_sequence||r->identity.plaintext_bytes||r->first_sample>INT64_MAX||
  r->next_sample>INT64_MAX||r->next_sample<r->first_sample||r->committed_samples>INT64_MAX){c->state=2;return RM_INVALID;}
 struct es_binding binding=r->identity;binding.plaintext_bytes=1;uint8_t header[128];
 if(es_header_build(header,128,&binding)){c->state=2;return RM_INVALID;}
 memset(header,0,128);memcpy(header,"OPNDMF2",7);put(header+8,2,2);put(header+10,128,2);put(header+12,64,2);
 uint32_t flags=r->finalized|(r->interrupted<<1U);put(header+14,flags,2);
 memcpy(header+16,r->identity.device_id,16);memcpy(header+32,r->identity.volume_id,16);put(header+48,r->identity.generation,8);
 memcpy(header+56,r->identity.recording_id,16);put(header+72,(uint64_t)r->segments*4U+flags,8);
 memcpy(header+80,r->identity.key_fingerprint,32);put(header+112,r->segments,4);put(header+116,2,4);put(header+120,r->committed_samples,8);
 memset(c,0,sizeof(*c));c->record=*r;c->next=r->first_sample;c->state=1;
 memcpy(out,header,128);wipe(header,sizeof(header));return RM_OK;
}
int recording_manifest_push(struct rm_incremental *c,const struct ros_segment *s,uint8_t out[64])
{
 if(!c||!s||!out||overlap(c,sizeof(*c),s,sizeof(*s))||overlap(c,sizeof(*c),out,64)||
  overlap(s,sizeof(*s),out,64))return RM_ARGUMENT;
 if(c->state!=1)return RM_INVALID;
 if(c->count>=c->record.segments||!identity(&c->record.identity,&s->identity)||s->identity.segment_sequence!=c->count||
  !full_segment_valid(s)||s->first_sample<c->next||s->gap!=(uint32_t)(s->first_sample>c->next)||
  (c->slots[s->slot/8U]&(1U<<(s->slot%8U)))||(c->count&&s->root!=c->root)||c->total>INT64_MAX-s->samples){
  c->state=2;return RM_INVALID;
 }
 uint8_t entry[64];int rc=recording_manifest_entry(entry,s);
 if(rc){c->state=2;wipe(entry,sizeof(entry));return rc;}
 c->slots[s->slot/8U]|=(uint8_t)(1U<<(s->slot%8U));c->root=s->root;c->next=s->next_sample;
 c->total+=s->samples;c->gaps+=s->gap;++c->count;
 memcpy(out,entry,64);wipe(entry,sizeof(entry));return RM_OK;
}
int recording_manifest_finish(struct rm_incremental *c)
{
 if(!c)return RM_ARGUMENT;
 if(c->state!=1)return RM_INVALID;
 if(c->count!=c->record.segments||c->total!=c->record.committed_samples||c->next!=c->record.next_sample||
  c->gaps!=c->record.gaps){c->state=2;return RM_INVALID;}
 c->state=3;return RM_OK;
}
void recording_manifest_clear(struct rm_incremental *c){if(c)wipe(c,sizeof(*c));}
