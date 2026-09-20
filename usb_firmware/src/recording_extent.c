/* SPDX-License-Identifier: Apache-2.0 */
#include "recording_extent.h"
#include <string.h>
static int overlap(const void *a,size_t an,const void *b,size_t bn)
{uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return an>UINTPTR_MAX-x||bn>UINTPTR_MAX-y||(x<y+bn&&y<x+an);}
static int identity(const uint8_t *p,size_t n)
{unsigned z=0,f=0;for(size_t i=0;i<n;++i){z|=p[i];f|=p[i]^255U;}return z&&f;}
static void put32(uint8_t *p,uint32_t v)
{for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(v>>(i*8));}
static void put64(uint8_t *p,uint64_t v)
{for(unsigned i=0;i<8;++i)p[i]=(uint8_t)(v>>(i*8));}
static int build(uint8_t out[REX_DESCRIPTOR_BYTES],const struct recording_extent_identity *id,const struct owned_page_hash *h,int full)
{
 if(!out||!id||!h||!h->sha256||overlap(out,256,id,sizeof(*id))||overlap(out,256,h,sizeof(*h))||
 !identity(id->device_id,16)||!identity(id->volume_id,16)||!identity(id->recipient_fingerprint,32)||
 !id->generation||id->generation>INT64_MAX)return -1;
 /* Explicit binding of chip, geometry, envelope and pinned Dhara revision.
  * All unassigned bytes are zero and covered by SHA-256. */
 static const uint8_t dhara[32]={0xfc,0x38,0x49,0x09,0x4b,0x35,0x4c,0x7b,0xc4,0x74,0xa1,0x65,0xcf,0x08,0xcf,0xf0,
  0xe3,0x6a,0x9e,0x33,0x95,0x4a,0xf8,0x5f,0xb7,0x8b,0xfd,0x6b,0xac,0x3a,0x3c,0xbd};
 memset(out,0,256);memcpy(out,"OPNDEX1",7);put32(out+8,1);put32(out+12,256);
 memcpy(out+16,id->device_id,16);memcpy(out+32,id->volume_id,16);put64(out+48,id->generation);
 memcpy(out+56,id->recipient_fingerprint,32);
 put32(out+88,REX_FIRST_BLOCK);put32(out+92,REX_BLOCKS);put32(out+96,REX_CAPACITY);
 put32(out+100,4096);put32(out+104,256);put32(out+108,64);put32(out+112,2048);
 put32(out+116,4);put32(out+120,REX_MAX_BAD_BLOCKS);out[124]=0x2c;out[125]=0x35;
 out[126]=0x48;out[127]=0x7c;memcpy(out+128,dhara,32);
 memcpy(out+160,"OpenPendant large native extent v1",33);
 if(full){memcpy(out,"OPNDEX2",7);put32(out+8,2);put32(out+92,RLL_BLOCKS);put32(out+96,RLL_CAPACITY);
  out[126]=0;memset(out+160,0,64);memcpy(out+160,"OpenPendant full native extent v2",32);}
 size_t actual=0;
 if(h->sha256(h->user,out,REX_DIGEST_OFFSET,out+REX_DIGEST_OFFSET,32,&actual)||actual!=32){memset(out,0,256);return -2;}
 return 0;
}
int rex_build(uint8_t out[REX_DESCRIPTOR_BYTES],const struct recording_extent_identity *id,const struct owned_page_hash *h)
{return build(out,id,h,0);}
int rex_build_full(uint8_t out[REX_DESCRIPTOR_BYTES],const struct recording_extent_identity *id,const struct owned_page_hash *h)
{return build(out,id,h,1);}
int rex_validate(const uint8_t bytes[REX_DESCRIPTOR_BYTES],const struct recording_extent_identity *id,const struct owned_page_hash *h)
{
 if(!bytes||!id||!h)return -1;
 uint8_t expected[REX_DESCRIPTOR_BYTES];int rc=rex_build(expected,id,h);
 if(!rc&&memcmp(bytes,expected,sizeof(expected)))rc=-1;
 memset(expected,0,sizeof(expected));return rc;
}
int rex_validate_full(const uint8_t bytes[REX_DESCRIPTOR_BYTES],const struct recording_extent_identity *id,const struct owned_page_hash *h)
{
 if(!bytes||!id||!h)return -1;
 uint8_t expected[REX_DESCRIPTOR_BYTES];int rc=rex_build_full(expected,id,h);
 if(!rc&&memcmp(bytes,expected,sizeof(expected)))rc=-1;
 memset(expected,0,sizeof(expected));return rc;
}
