/* SPDX-License-Identifier: Apache-2.0 */
#include "owned_volume_descriptor.h"
#include <stdbool.h>

_Static_assert(OWNED_VOLUME_HEADER_BYTES == OWNED_VOLUME_DIGEST_OFFSET &&
    OWNED_VOLUME_DIGEST_OFFSET + 32U == OWNED_VOLUME_PADDING_OFFSET &&
    OWNED_VOLUME_PADDING_OFFSET + 128U == OWNED_VOLUME_DESCRIPTOR_BYTES,
    "Descriptor extents changed");
_Static_assert(OWNED_VOLUME_MAP_BLOCKS * OWNED_VOLUME_PAGES_PER_BLOCK ==
    OWNED_VOLUME_LOGICAL_PAGES && OWNED_VOLUME_BLOCK_LIMIT <= UINT32_MAX / 64U,
    "Mapping bounds changed");

static const uint8_t magic[8] = {'O','P','N','D','V','D','1',0};
const uint8_t owned_volume_dhara_sha256[32] = {
    0xfc,0x38,0x49,0x09,0x4b,0x35,0x4c,0x7b,0xc4,0x74,0xa1,0x65,0xcf,0x08,0xcf,0xf0,
    0xe3,0x6a,0x9e,0x33,0x95,0x4a,0xf8,0x5f,0xb7,0x8b,0xfd,0x6b,0xac,0x3a,0x3c,0xbd
};
static void put16(uint8_t *p,uint16_t v)
{ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void put32(uint8_t *p,uint32_t v)
{ for(unsigned i=0;i<4;i++) p[i]=(uint8_t)(v>>(8U*i)); }
static void put64(uint8_t *p,uint64_t v)
{ for(unsigned i=0;i<8;i++) p[i]=(uint8_t)(v>>(8U*i)); }
static uint16_t get16(const uint8_t *p)
{ return (uint16_t)((uint16_t)p[0]|((uint16_t)p[1]<<8)); }
static uint32_t get32(const uint8_t *p)
{ uint32_t v=0; for(unsigned i=0;i<4;i++) v|=(uint32_t)p[i]<<(8U*i); return v; }
static uint64_t get64(const uint8_t *p)
{ uint64_t v=0; for(unsigned i=0;i<8;i++) v|=(uint64_t)p[i]<<(8U*i); return v; }
static void fill(uint8_t *p,size_t n,uint8_t v)
{ for(size_t i=0;i<n;i++) p[i]=v; }
static void copy(uint8_t *a,const uint8_t *b,size_t n)
{ for(size_t i=0;i<n;i++) a[i]=b[i]; }
static void wipe(void *p,size_t n)
{ volatile uint8_t *v=p; for(size_t i=0;i<n;i++) v[i]=0; }
static bool equal(const uint8_t *a,const uint8_t *b,size_t n)
{ unsigned d=0; for(size_t i=0;i<n;i++) d|=(unsigned)(a[i]^b[i]); return d==0; }
static bool all(const uint8_t *p,size_t n,uint8_t v)
{ unsigned d=0; for(size_t i=0;i<n;i++) d|=(unsigned)(p[i]^v); return d==0; }
static bool identity(const uint8_t *p,size_t n)
{ return !all(p,n,0) && !all(p,n,0xff); }
static bool overlap(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t aa=(uintptr_t)a,bb=(uintptr_t)b;
    if(an>UINTPTR_MAX-aa || bn>UINTPTR_MAX-bb) return true;
    return aa<bb+bn && bb<aa+an;
}
static bool block_valid(uint32_t b)
{
    /* The reserved factory range is also excluded by the stricter upper bound.
     * Keep both guards explicit so later policy changes cannot quietly drop it. */
    return b<OWNED_VOLUME_BLOCK_LIMIT && !(b>=1549U && b<=1569U);
}
static bool spec_valid(const struct owned_volume_spec *s)
{
    if(!s || !identity(s->device_id,16) || !identity(s->volume_id,16) ||
       !s->generation || s->generation>INT64_MAX ||
       !identity(s->recipient_fingerprint,32) ||
       !identity(s->preservation_manifest_sha256,32)) return false;
    for(unsigned i=0;i<OWNED_VOLUME_MAP_BLOCKS;i++) {
        if(!block_valid(s->map_blocks[i])) return false;
        for(unsigned j=0;j<i;j++) if(s->map_blocks[i]==s->map_blocks[j]) return false;
        for(unsigned j=0;j<OWNED_VOLUME_CONTROL_BLOCKS;j++)
            if(s->map_blocks[i]==s->control_blocks[j]) return false;
    }
    for(unsigned i=0;i<OWNED_VOLUME_CONTROL_BLOCKS;i++) {
        if(!block_valid(s->control_blocks[i])) return false;
        for(unsigned j=0;j<i;j++) if(s->control_blocks[i]==s->control_blocks[j]) return false;
    }
    return true;
}
static bool hash_valid(const struct owned_page_hash *h)
{ return h && h->sha256; }
static uint32_t crc(const uint8_t *p)
{
    uint32_t v=UINT32_MAX;
    for(size_t i=0;i<OWNED_VOLUME_HEADER_BYTES;i++) {
        v^=(i>=320U && i<324U)?0:p[i];
        for(unsigned bit=0;bit<8;bit++) v=(v>>1)^((0U-(v&1U))&0xedb88320U);
    }
    return ~v;
}
static int digest(const struct owned_page_hash *h,const uint8_t *p,uint8_t out[32])
{
    size_t actual=0;
    fill(out,32,0);
    int rc=h->sha256(h->user,p,OWNED_VOLUME_HEADER_BYTES,out,32,&actual);
    if(rc || actual!=32) { wipe(out,32); return OWNED_VOLUME_HASH_ERROR; }
    return OWNED_VOLUME_VALID;
}
static void encode(uint8_t *p,const struct owned_volume_spec *s)
{
    fill(p,OWNED_VOLUME_HEADER_BYTES,0); copy(p,magic,8);
    put16(p+8,1); put16(p+10,OWNED_VOLUME_HEADER_BYTES);
    put32(p+12,OWNED_VOLUME_DESCRIPTOR_BYTES);
    copy(p+16,s->device_id,16); copy(p+32,s->volume_id,16); put64(p+48,s->generation);
    put32(p+56,4096); put16(p+60,256); put16(p+62,64); put32(p+64,2048);
    put16(p+68,32); put16(p+70,2); p[72]=1; p[73]=1; put16(p+74,1);
    copy(p+76,owned_volume_dhara_sha256,32);
    copy(p+108,s->recipient_fingerprint,32); copy(p+140,s->preservation_manifest_sha256,32);
    for(unsigned i=0;i<OWNED_VOLUME_MAP_BLOCKS;i++) put32(p+172+4U*i,s->map_blocks[i]);
    for(unsigned i=0;i<OWNED_VOLUME_CONTROL_BLOCKS;i++) put32(p+300+4U*i,s->control_blocks[i]);
    put16(p+308,OWNED_VOLUME_BLOCK_LIMIT); put32(p+320,crc(p));
}
static bool matches(const uint8_t *p,const struct owned_volume_spec *s)
{
    if(!equal(p,magic,8) || get16(p+8)!=1 || get16(p+10)!=OWNED_VOLUME_HEADER_BYTES ||
       get32(p+12)!=OWNED_VOLUME_DESCRIPTOR_BYTES || !equal(p+16,s->device_id,16) ||
       !equal(p+32,s->volume_id,16) || get64(p+48)!=s->generation ||
       get32(p+56)!=4096 || get16(p+60)!=256 || get16(p+62)!=64 || get32(p+64)!=2048 ||
       get16(p+68)!=32 || get16(p+70)!=2 || p[72]!=1 || p[73]!=1 || get16(p+74)!=1 ||
       !equal(p+76,owned_volume_dhara_sha256,32) ||
       !equal(p+108,s->recipient_fingerprint,32) ||
       !equal(p+140,s->preservation_manifest_sha256,32) ||
       get16(p+308)!=OWNED_VOLUME_BLOCK_LIMIT || !all(p+310,10,0) || !all(p+324,28,0))
        return false;
    for(unsigned i=0;i<OWNED_VOLUME_MAP_BLOCKS;i++)
        if(get32(p+172+4U*i)!=s->map_blocks[i]) return false;
    for(unsigned i=0;i<OWNED_VOLUME_CONTROL_BLOCKS;i++)
        if(get32(p+300+4U*i)!=s->control_blocks[i]) return false;
    return true;
}

int owned_volume_descriptor_build(uint8_t *out,size_t out_size,
    const struct owned_volume_spec *spec,const struct owned_page_hash *hash)
{
    if(!out || out_size!=OWNED_VOLUME_DESCRIPTOR_BYTES || !spec_valid(spec) ||
       !hash_valid(hash) || overlap(out,out_size,spec,sizeof(*spec)) ||
       overlap(out,out_size,hash,sizeof(*hash))) return OWNED_VOLUME_ARGUMENT;
    uint8_t value[32];
    fill(out,out_size,0xff); encode(out,spec);
    int rc=digest(hash,out,value);
    if(rc==OWNED_VOLUME_VALID) copy(out+OWNED_VOLUME_DIGEST_OFFSET,value,32);
    else wipe(out,out_size);
    wipe(value,sizeof(value)); return rc;
}

int owned_volume_descriptor_validate(const uint8_t *bytes,size_t size,
    const struct owned_volume_spec *expected,const struct owned_page_hash *hash,
    struct owned_volume_decoded *decoded)
{
    if(!decoded || (bytes && overlap(decoded,sizeof(*decoded),bytes,size)) ||
       (expected && overlap(decoded,sizeof(*decoded),expected,sizeof(*expected))) ||
       (hash && overlap(decoded,sizeof(*decoded),hash,sizeof(*hash)))) return OWNED_VOLUME_ARGUMENT;
    wipe(decoded,sizeof(*decoded));
    if(!bytes || size!=OWNED_VOLUME_DESCRIPTOR_BYTES || !spec_valid(expected) ||
       !hash_valid(hash) || overlap(bytes,size,expected,sizeof(*expected)) ||
       overlap(bytes,size,hash,sizeof(*hash))) return OWNED_VOLUME_ARGUMENT;
    if(!matches(bytes,expected) || get32(bytes+320)!=crc(bytes) ||
       !all(bytes+OWNED_VOLUME_PADDING_OFFSET,128,0xff)) return OWNED_VOLUME_NOT_VALID;
    uint8_t value[32];
    int rc=digest(hash,bytes,value);
    if(rc==OWNED_VOLUME_VALID && !equal(value,bytes+OWNED_VOLUME_DIGEST_OFFSET,32))
        rc=OWNED_VOLUME_NOT_VALID;
    if(rc==OWNED_VOLUME_VALID) {
        /* Copy fields explicitly; padding in the returned struct remains zero. */
        copy(decoded->spec.device_id,bytes+16,16); copy(decoded->spec.volume_id,bytes+32,16);
        decoded->spec.generation=get64(bytes+48);
        copy(decoded->spec.recipient_fingerprint,bytes+108,32);
        copy(decoded->spec.preservation_manifest_sha256,bytes+140,32);
        for(unsigned i=0;i<OWNED_VOLUME_MAP_BLOCKS;i++)
            decoded->spec.map_blocks[i]=get32(bytes+172+4U*i);
        for(unsigned i=0;i<OWNED_VOLUME_CONTROL_BLOCKS;i++)
            decoded->spec.control_blocks[i]=get32(bytes+300+4U*i);
        copy(decoded->digest,value,32);
    }
    wipe(value,sizeof(value)); return rc;
}

int owned_volume_descriptor_map(const uint8_t *bytes,size_t size,
    const struct owned_volume_spec *expected,const struct owned_page_hash *hash,
    uint32_t logical_page,uint32_t *physical_row)
{
    if(!physical_row || logical_page>=OWNED_VOLUME_LOGICAL_PAGES ||
       (bytes && overlap(physical_row,sizeof(*physical_row),bytes,size)) ||
       (expected && overlap(physical_row,sizeof(*physical_row),expected,sizeof(*expected))) ||
       (hash && overlap(physical_row,sizeof(*physical_row),hash,sizeof(*hash))))
        return OWNED_VOLUME_ARGUMENT;
    struct owned_volume_decoded decoded;
    int rc=owned_volume_descriptor_validate(bytes,size,expected,hash,&decoded);
    if(rc!=OWNED_VOLUME_VALID) return rc;
    uint32_t block=decoded.spec.map_blocks[logical_page/OWNED_VOLUME_PAGES_PER_BLOCK];
    /* spec_valid and the logical bound imply this already; retain the immediate
     * arithmetic guard as well, before multiply/add or publishing any result. */
    if(!block_valid(block) || block>(UINT32_MAX-63U)/OWNED_VOLUME_PAGES_PER_BLOCK) {
        wipe(&decoded,sizeof(decoded)); return OWNED_VOLUME_NOT_VALID;
    }
    uint32_t row=block*OWNED_VOLUME_PAGES_PER_BLOCK+(logical_page%OWNED_VOLUME_PAGES_PER_BLOCK);
    wipe(&decoded,sizeof(decoded)); *physical_row=row; return OWNED_VOLUME_VALID;
}
