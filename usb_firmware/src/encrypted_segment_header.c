#include "encrypted_segment_header.h"
#include <stdbool.h>
static const uint8_t magic[8]={'O','P','N','D','E','S','1',0};
static const uint8_t domain[]="OpenPendant encrypted segment v1";
_Static_assert(sizeof(domain)==33U,"HPKE info domain size differs");
_Static_assert(ES_INFO_BYTES==sizeof(domain)+ES_HEADER_BYTES,"Info size differs");
static void cp(uint8_t *to,const uint8_t *from,size_t n)
{ for(size_t i=0;i<n;++i)to[i]=from[i]; }
static void zero(uint8_t *p,size_t n)
{ for(size_t i=0;i<n;++i)p[i]=0; }
static bool eq(const uint8_t *a,const uint8_t *b,size_t n)
{ unsigned d=0; for(size_t i=0;i<n;++i)d|=a[i]^b[i]; return d==0; }
static bool all(const uint8_t *p,size_t n,uint8_t v)
{ unsigned d=0; for(size_t i=0;i<n;++i)d|=p[i]^v; return d==0; }
static bool ident(const uint8_t *p,size_t n)
{ return !all(p,n,0) && !all(p,n,255); }
static bool overlap(const void *a,size_t an,const void *b,size_t bn)
{
 uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
 if(an>UINTPTR_MAX-x || bn>UINTPTR_MAX-y)return true;
 return x<y+bn && y<x+an;
}
static void put(uint8_t *p,uint64_t v,size_t n)
{ for(size_t i=0;i<n;++i)p[n-1U-i]=(uint8_t)(v>>(i*8U)); }
static uint64_t get(const uint8_t *p,size_t n)
{ uint64_t v=0; for(size_t i=0;i<n;++i)v=(v<<8)|p[i]; return v; }
static bool valid(const struct es_binding *b)
{
 return b && ident(b->key_fingerprint,32) && ident(b->device_id,16) &&
  ident(b->volume_id,16) && ident(b->recording_id,16) && b->generation>0 &&
  b->generation<=INT64_MAX && b->segment_sequence<=INT32_MAX &&
  b->plaintext_bytes>0 && b->plaintext_bytes<=ES_MAX_PLAINTEXT_BYTES;
}
int es_header_build(uint8_t *out,size_t size,const struct es_binding *b)
{
 if(!out || size!=ES_HEADER_BYTES || !valid(b) || overlap(out,size,b,sizeof(*b)))return ES_ARGUMENT;
 zero(out,size); cp(out,magic,8); put(out+8,1,2); put(out+10,128,2);
 put(out+12,16,2); put(out+14,1,2); put(out+16,2,2);
 cp(out+20,b->key_fingerprint,32); cp(out+52,b->device_id,16); cp(out+68,b->volume_id,16);
 put(out+84,b->generation,8); cp(out+92,b->recording_id,16);
 put(out+108,b->segment_sequence,4); put(out+112,b->plaintext_bytes,4); return ES_OK;
}
int es_header_validate(const uint8_t *p,size_t size,const struct es_binding *b)
{
 if(!p || size!=ES_HEADER_BYTES || !valid(b) || overlap(p,size,b,sizeof(*b)))return ES_ARGUMENT;
 if(!eq(p,magic,8) || get(p+8,2)!=1 || get(p+10,2)!=128 || get(p+12,2)!=16 ||
  get(p+14,2)!=1 || get(p+16,2)!=2 || get(p+18,2)!=0 || !all(p+116,12,0) ||
  !eq(p+20,b->key_fingerprint,32) || !eq(p+52,b->device_id,16) ||
  !eq(p+68,b->volume_id,16) || get(p+84,8)!=b->generation ||
  !eq(p+92,b->recording_id,16) || get(p+108,4)!=b->segment_sequence ||
  get(p+112,4)!=b->plaintext_bytes)return ES_INVALID;
 return ES_OK;
}
int es_container_validate(const uint8_t *p,size_t size,const struct es_binding *b)
{
 if(!p || !valid(b) || size<ES_HEADER_BYTES || size>ES_MAX_CONTAINER_BYTES ||
  overlap(p,size,b,sizeof(*b)))return ES_ARGUMENT;
 if(size!=ES_HEADER_BYTES+ES_ENCAP_BYTES+b->plaintext_bytes+ES_TAG_BYTES)return ES_INVALID;
 int rc=es_header_validate(p,ES_HEADER_BYTES,b); if(rc)return rc;
 return p[ES_HEADER_BYTES]==4 ? ES_OK : ES_INVALID;
}
int es_info_build(uint8_t *out,size_t size,const uint8_t *p,size_t psize,const struct es_binding *b)
{
 if(!out || !p || !b || size!=ES_INFO_BYTES || overlap(out,size,p,psize) ||
  overlap(out,size,b,sizeof(*b)))return ES_ARGUMENT;
 int rc=es_header_validate(p,psize,b); if(rc)return rc;
 cp(out,domain,sizeof(domain)); cp(out+sizeof(domain),p,ES_HEADER_BYTES); return ES_OK;
}
