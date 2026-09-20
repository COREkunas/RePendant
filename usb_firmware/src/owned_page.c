/* SPDX-License-Identifier: Apache-2.0 */
#include "owned_page.h"
#include <stdbool.h>

_Static_assert(OWNED_PAGE_HEADER_BYTES+OWNED_PAGE_PAYLOAD_BYTES==OWNED_PAGE_DIGEST_OFFSET,
    "Payload/hash boundary changed");
_Static_assert(OWNED_PAGE_DIGEST_OFFSET+OWNED_PAGE_DIGEST_BYTES==OWNED_PAGE_PADDING_OFFSET &&
    OWNED_PAGE_PADDING_OFFSET<OWNED_PAGE_BYTES, "Digest/padding boundary changed");

static const uint8_t page_magic[8] = {'O','P','N','D','P','G','1',0};
static const uint8_t commit_magic[8] = {'O','P','N','D','C','M','1',0};

static void put16(uint8_t *p, uint16_t v)
{ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void put32(uint8_t *p, uint32_t v)
{ for (unsigned i=0;i<4;i++) p[i]=(uint8_t)(v>>(8U*i)); }
static void put64(uint8_t *p, uint64_t v)
{ for (unsigned i=0;i<8;i++) p[i]=(uint8_t)(v>>(8U*i)); }
static uint16_t get16(const uint8_t *p)
{ return (uint16_t)((uint16_t)p[0]|(uint16_t)((uint16_t)p[1]<<8)); }
static uint32_t get32(const uint8_t *p)
{ uint32_t v=0; for(unsigned i=0;i<4;i++) v|=(uint32_t)p[i]<<(8U*i); return v; }
static uint64_t get64(const uint8_t *p)
{ uint64_t v=0; for(unsigned i=0;i<8;i++) v|=(uint64_t)p[i]<<(8U*i); return v; }
static void fill(uint8_t *p, size_t n, uint8_t v)
{ for(size_t i=0;i<n;i++) p[i]=v; }
static void copy(uint8_t *to, const uint8_t *from, size_t n)
{ for(size_t i=0;i<n;i++) to[i]=from[i]; }
static bool equal(const uint8_t *a, const uint8_t *b, size_t n)
{ unsigned different=0; for(size_t i=0;i<n;i++) different|=(unsigned)(a[i]^b[i]); return different==0; }
static bool all(const uint8_t *p, size_t n, uint8_t v)
{ unsigned different=0; for(size_t i=0;i<n;i++) different|=(unsigned)(p[i]^v); return different==0; }
static void wipe(uint8_t *p, size_t n)
{ volatile uint8_t *v=p; for(size_t i=0;i<n;i++) v[i]=0; }
static bool overlap(const void *a, size_t an, const void *b, size_t bn)
{
    uintptr_t aa=(uintptr_t)a, bb=(uintptr_t)b;
    if(an>UINTPTR_MAX-aa || bn>UINTPTR_MAX-bb) return true;
    return aa<bb+bn && bb<aa+an;
}
static bool identity(const uint8_t *p, size_t n)
{ return !all(p,n,0) && !all(p,n,0xff); }
static bool binding_valid(const struct owned_page_binding *b)
{
    return b!=NULL && identity(b->device_id,16) && identity(b->volume_id,16) &&
        b->generation!=0 && b->generation<=INT64_MAX && b->lease_id!=0 &&
        b->logical_page<OWNED_PAGE_ROW_LIMIT && b->physical_row<OWNED_PAGE_ROW_LIMIT &&
        (b->logical_page&63U)==(b->physical_row&63U) &&
        (b->kind==OWNED_PAGE_DATA || b->kind==OWNED_PAGE_COMMIT);
}
static bool hash_valid(const struct owned_page_hash *hash)
{ return hash!=NULL && hash->sha256!=NULL; }
static uint32_t crc(const uint8_t *p, size_t n, bool header)
{
    uint32_t v=UINT32_MAX;
    for(size_t i=0;i<n;i++) {
        v^=(header && i>=76 && i<80)?0:p[i];
        for(unsigned bit=0;bit<8;bit++) v=(v>>1)^((0U-(v&1U))&0xedb88320U);
    }
    return ~v;
}
static bool output_safe(uint8_t *out, size_t n,
    const struct owned_page_binding *b, const struct owned_page_hash *hash)
{
    return out!=NULL && n==OWNED_PAGE_BYTES && binding_valid(b) && hash_valid(hash) &&
        !overlap(out,n,b,sizeof(*b)) && !overlap(out,n,hash,sizeof(*hash));
}
static int digest(const struct owned_page_hash *hash, const uint8_t *page, uint8_t out[32])
{
    size_t written=0;
    fill(out,32,0);
    int rc=hash->sha256(hash->user,page,OWNED_PAGE_DIGEST_OFFSET,out,32,&written);
    if(rc!=0 || written!=32) { wipe(out,32); return OWNED_PAGE_HASH_ERROR; }
    return OWNED_PAGE_VALID;
}
static void header(uint8_t *out, const struct owned_page_binding *b)
{
    fill(out,OWNED_PAGE_HEADER_BYTES,0);
    copy(out,page_magic,8); put16(out+8,1); put16(out+10,b->kind);
    put16(out+12,OWNED_PAGE_HEADER_BYTES); put16(out+14,OWNED_PAGE_PAYLOAD_BYTES);
    copy(out+16,b->device_id,16); copy(out+32,b->volume_id,16);
    put64(out+48,b->generation); put32(out+56,b->logical_page);
    put32(out+60,b->physical_row); put64(out+64,b->lease_id);
    put32(out+72,crc(out+OWNED_PAGE_HEADER_BYTES,OWNED_PAGE_PAYLOAD_BYTES,false));
    put32(out+76,crc(out,OWNED_PAGE_HEADER_BYTES,true));
}
static int seal(uint8_t *out, const struct owned_page_binding *b,
    const struct owned_page_hash *hash)
{
    uint8_t value[32];
    header(out,b);
    int rc=digest(hash,out,value);
    if(rc==OWNED_PAGE_VALID) copy(out+OWNED_PAGE_DIGEST_OFFSET,value,32);
    else wipe(out,OWNED_PAGE_BYTES);
    wipe(value,sizeof(value));
    return rc;
}

int owned_page_build_data(uint8_t *out, size_t out_length,
    const struct owned_page_binding *binding, const uint8_t *payload,
    size_t payload_length, const struct owned_page_hash *hash)
{
    if(!output_safe(out,out_length,binding,hash) || binding->kind!=OWNED_PAGE_DATA ||
       payload==NULL || payload_length!=OWNED_PAGE_PAYLOAD_BYTES ||
       overlap(out,out_length,payload,payload_length)) return OWNED_PAGE_ARGUMENT;
    fill(out,out_length,0xff);
    copy(out+OWNED_PAGE_HEADER_BYTES,payload,payload_length);
    return seal(out,binding,hash);
}

static bool header_matches(const uint8_t *p, const struct owned_page_binding *b)
{
    return equal(p,page_magic,8) && get16(p+8)==1 && get16(p+10)==b->kind &&
        get16(p+12)==OWNED_PAGE_HEADER_BYTES && get16(p+14)==OWNED_PAGE_PAYLOAD_BYTES &&
        equal(p+16,b->device_id,16) && equal(p+32,b->volume_id,16) &&
        get64(p+48)==b->generation && get32(p+56)==b->logical_page &&
        get32(p+60)==b->physical_row && get64(p+64)==b->lease_id && all(p+80,16,0);
}
static bool commit_syntax(const uint8_t *p, const struct owned_page_binding *b)
{
    return equal(p,commit_magic,8) && get16(p+8)==1 && get16(p+10)==96 &&
        get32(p+12)==OWNED_PAGE_PAYLOAD_BYTES && identity(p+16,16) &&
        b->logical_page>0 && b->physical_row>0 && (b->logical_page&63U)!=0 &&
        get32(p+32)==b->logical_page-1U && get32(p+36)==b->physical_row-1U &&
        get64(p+40)==b->lease_id && all(p+80,16,0) &&
        all(p+96,OWNED_PAGE_PAYLOAD_BYTES-96,0xff);
}
int owned_page_validate(const uint8_t *page, size_t page_length,
    const struct owned_page_binding *expected, const struct owned_page_hash *hash,
    struct owned_page_view *view)
{
    if(view==NULL || (page!=NULL && overlap(view,sizeof(*view),page,page_length)) ||
       (expected!=NULL && overlap(view,sizeof(*view),expected,sizeof(*expected))) ||
       (hash!=NULL && overlap(view,sizeof(*view),hash,sizeof(*hash)))) return OWNED_PAGE_ARGUMENT;
    view->payload=NULL; fill(view->digest,32,0);
    if(page==NULL || page_length!=OWNED_PAGE_BYTES || !binding_valid(expected) || !hash_valid(hash))
        return OWNED_PAGE_ARGUMENT;
    if(overlap(page,page_length,expected,sizeof(*expected)) ||
       overlap(page,page_length,hash,sizeof(*hash))) return OWNED_PAGE_ARGUMENT;
    if(all(page,page_length,0xff)) return OWNED_PAGE_ERASED_LOOKING;
    if(!header_matches(page,expected) ||
       !all(page+OWNED_PAGE_PADDING_OFFSET,OWNED_PAGE_BYTES-OWNED_PAGE_PADDING_OFFSET,0xff) ||
       get32(page+76)!=crc(page,OWNED_PAGE_HEADER_BYTES,true) ||
       get32(page+72)!=crc(page+OWNED_PAGE_HEADER_BYTES,OWNED_PAGE_PAYLOAD_BYTES,false) ||
       (expected->kind==OWNED_PAGE_COMMIT && !commit_syntax(page+OWNED_PAGE_HEADER_BYTES,expected)))
        return OWNED_PAGE_NOT_VALID;
    uint8_t value[32];
    int rc=digest(hash,page,value);
    if(rc==OWNED_PAGE_VALID && !equal(value,page+OWNED_PAGE_DIGEST_OFFSET,32)) rc=OWNED_PAGE_NOT_VALID;
    if(rc==OWNED_PAGE_VALID) {
        copy(view->digest,value,32); view->payload=page+OWNED_PAGE_HEADER_BYTES;
    }
    wipe(value,sizeof(value));
    return rc;
}

static bool pair(const struct owned_page_binding *c, const struct owned_page_binding *d)
{
    return binding_valid(c) && binding_valid(d) && c->kind==OWNED_PAGE_COMMIT &&
        d->kind==OWNED_PAGE_DATA && equal(c->device_id,d->device_id,16) &&
        equal(c->volume_id,d->volume_id,16) && c->generation==d->generation &&
        c->lease_id==d->lease_id && c->logical_page==d->logical_page+1U &&
        c->physical_row==d->physical_row+1U &&
        (c->logical_page>>6)==(d->logical_page>>6) &&
        (c->physical_row>>6)==(d->physical_row>>6);
}
int owned_page_build_commit(uint8_t *out, size_t out_length,
    const struct owned_page_binding *commit_binding, const uint8_t object_id[16],
    const uint8_t *data_page, size_t data_length,
    const struct owned_page_binding *data_binding, const struct owned_page_hash *hash)
{
    if(!output_safe(out,out_length,commit_binding,hash) || !pair(commit_binding,data_binding) ||
       object_id==NULL || !identity(object_id,16) || data_page==NULL || data_length!=OWNED_PAGE_BYTES ||
       overlap(out,out_length,object_id,16) || overlap(out,out_length,data_page,data_length) ||
       overlap(out,out_length,data_binding,sizeof(*data_binding))) return OWNED_PAGE_ARGUMENT;
    struct owned_page_view data;
    int rc=owned_page_validate(data_page,data_length,data_binding,hash,&data);
    if(rc!=OWNED_PAGE_VALID) {
        if(rc==OWNED_PAGE_HASH_ERROR) wipe(out,out_length);
        return rc==OWNED_PAGE_ERASED_LOOKING?OWNED_PAGE_NOT_VALID:rc;
    }
    fill(out,out_length,0xff);
    uint8_t *p=out+OWNED_PAGE_HEADER_BYTES;
    fill(p,96,0); copy(p,commit_magic,8); put16(p+8,1); put16(p+10,96);
    put32(p+12,OWNED_PAGE_PAYLOAD_BYTES); copy(p+16,object_id,16);
    put32(p+32,data_binding->logical_page); put32(p+36,data_binding->physical_row);
    put64(p+40,data_binding->lease_id); copy(p+48,data.digest,32);
    wipe(data.digest,sizeof(data.digest)); data.payload=NULL;
    return seal(out,commit_binding,hash);
}
int owned_page_match_commit(const uint8_t *commit_page, size_t commit_length,
    const struct owned_page_binding *commit_binding, const uint8_t object_id[16],
    const uint8_t *data_page, size_t data_length,
    const struct owned_page_binding *data_binding, const struct owned_page_hash *hash)
{
    if(!pair(commit_binding,data_binding) || object_id==NULL || !identity(object_id,16))
        return OWNED_PAGE_ARGUMENT;
    struct owned_page_view commit,data;
    int rc=owned_page_validate(commit_page,commit_length,commit_binding,hash,&commit);
    if(rc!=OWNED_PAGE_VALID) return rc==OWNED_PAGE_ERASED_LOOKING?OWNED_PAGE_NOT_VALID:rc;
    rc=owned_page_validate(data_page,data_length,data_binding,hash,&data);
    if(rc==OWNED_PAGE_VALID && (!equal(commit.payload+16,object_id,16) ||
        !equal(commit.payload+48,data.digest,32))) rc=OWNED_PAGE_NOT_VALID;
    wipe(commit.digest,sizeof(commit.digest)); wipe(data.digest,sizeof(data.digest));
    return rc==OWNED_PAGE_ERASED_LOOKING?OWNED_PAGE_NOT_VALID:rc;
}
