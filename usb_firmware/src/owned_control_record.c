/* SPDX-License-Identifier: Apache-2.0 */
#include "owned_control_record.h"
#include <stdbool.h>

_Static_assert(OWNED_CONTROL_HEADER_BYTES+32U==OWNED_CONTROL_PADDING_OFFSET &&
    OWNED_CONTROL_PADDING_OFFSET<OWNED_CONTROL_PAGE_BYTES &&
    OWNED_CONTROL_BANKS*OWNED_CONTROL_SLOTS==OWNED_CONTROL_RECORDS,"Control extents changed");
static const uint8_t control_magic[8]={'O','P','N','D','C','T','1',0};
/* Prefix private helpers so this pure C module can share a native-test TU with
 * the actual descriptor codec, without replacing either implementation. */
static void ct_put32(uint8_t *p,uint32_t v)
{ for(unsigned i=0;i<4;i++) p[i]=(uint8_t)(v>>(8U*i)); }
static void ct_put64(uint8_t *p,uint64_t v)
{ for(unsigned i=0;i<8;i++) p[i]=(uint8_t)(v>>(8U*i)); }
static uint32_t ct_get32(const uint8_t *p)
{ uint32_t v=0; for(unsigned i=0;i<4;i++) v|=(uint32_t)p[i]<<(8U*i); return v; }
static uint64_t ct_get64(const uint8_t *p)
{ uint64_t v=0; for(unsigned i=0;i<8;i++) v|=(uint64_t)p[i]<<(8U*i); return v; }
static void ct_fill(uint8_t *p,size_t n,uint8_t v)
{ for(size_t i=0;i<n;i++) p[i]=v; }
static void ct_copy(uint8_t *a,const uint8_t *b,size_t n)
{ for(size_t i=0;i<n;i++) a[i]=b[i]; }
static void ct_wipe(void *p,size_t n)
{ volatile uint8_t *v=p; for(size_t i=0;i<n;i++) v[i]=0; }
static bool ct_equal(const uint8_t *a,const uint8_t *b,size_t n)
{ unsigned d=0; for(size_t i=0;i<n;i++) d|=(unsigned)(a[i]^b[i]); return d==0; }
static bool ct_all(const uint8_t *p,size_t n,uint8_t v)
{ unsigned d=0; for(size_t i=0;i<n;i++) d|=(unsigned)(p[i]^v); return d==0; }
static bool ct_overlap(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t aa=(uintptr_t)a,bb=(uintptr_t)b;
    if(an>UINTPTR_MAX-aa || bn>UINTPTR_MAX-bb) return true;
    return aa<bb+bn && bb<aa+an;
}
static bool ct_hash_valid(const struct owned_page_hash *h)
{ return h && h->sha256; }
static bool ct_record_valid(const struct owned_control_record *r,const struct owned_volume_spec *s)
{
    if(!r || !r->sequence || r->sequence>INT64_MAX || r->previous_sequence>INT64_MAX ||
       r->state<OWNED_CONTROL_PROVISIONING || r->state>OWNED_CONTROL_READ_ONLY ||
       r->bank>=2 || r->slot>=64 || r->retired_control>3 ||
       r->physical_row!=s->control_blocks[r->bank]*64U+r->slot ||
       r->lease_kind>OWNED_CONTROL_PROGRAM_PAGE || r->resolution>OWNED_CONTROL_ABANDONED_RETIRED)
        return false;
    if(r->sequence==1) {
        if(r->previous_sequence || !ct_all(r->previous_digest,32,0) ||
           r->state!=OWNED_CONTROL_PROVISIONING || r->lease_kind || r->lease_high_water ||
           r->resolved_lease_id || r->resolution) return false;
    } else if(r->previous_sequence!=r->sequence-1U ||
              ct_all(r->previous_digest,32,0) || ct_all(r->previous_digest,32,0xff)) return false;
    if(r->lease_kind==OWNED_CONTROL_NO_LEASE) {
        if(r->lease_id || r->target_map_slot!=OWNED_CONTROL_NO_TARGET ||
           r->target_page!=OWNED_CONTROL_NO_TARGET || r->target_physical_row!=OWNED_CONTROL_NO_TARGET)
            return false;
        if(r->resolution==OWNED_CONTROL_UNRESOLVED) return r->resolved_lease_id==0;
        return r->resolved_lease_id!=0 && r->resolved_lease_id==r->lease_high_water &&
            (r->resolution!=OWNED_CONTROL_ABANDONED_RETIRED || r->state==OWNED_CONTROL_READ_ONLY);
    }
    if(!r->lease_id || r->lease_id!=r->lease_high_water || r->resolution || r->resolved_lease_id ||
       r->target_map_slot>=32 || r->target_page>=64 ||
       (r->lease_kind==OWNED_CONTROL_ERASE_BLOCK && r->target_page!=0) ||
       r->target_physical_row!=s->map_blocks[r->target_map_slot]*64U+r->target_page)
        return false;
    return !(r->retired_map&(1U<<r->target_map_slot)) || r->state==OWNED_CONTROL_READ_ONLY;
}
static uint32_t ct_crc(const uint8_t *p)
{
    uint32_t v=UINT32_MAX;
    for(size_t i=0;i<OWNED_CONTROL_HEADER_BYTES;i++) {
        v^=(i>=184U && i<188U)?0:p[i];
        for(unsigned b=0;b<8;b++) v=(v>>1)^((0U-(v&1U))&0xedb88320U);
    }
    return ~v;
}
static int ct_digest(const struct owned_page_hash *h,const uint8_t *p,uint8_t out[32])
{
    size_t actual=0; ct_fill(out,32,0);
    int rc=h->sha256(h->user,p,OWNED_CONTROL_HEADER_BYTES,out,32,&actual);
    if(rc || actual!=32) { ct_wipe(out,32); return OWNED_CONTROL_HASH_ERROR; }
    return OWNED_CONTROL_VALID;
}
static void ct_encode(uint8_t *p,const struct owned_control_record *r,const uint8_t dd[32])
{
    ct_fill(p,OWNED_CONTROL_HEADER_BYTES,0); ct_copy(p,control_magic,8);
    ct_put32(p+8,1); ct_put32(p+12,OWNED_CONTROL_HEADER_BYTES); ct_copy(p+16,dd,32);
    ct_put64(p+48,r->sequence); ct_put64(p+56,r->previous_sequence); ct_copy(p+64,r->previous_digest,32);
    ct_put32(p+96,r->state); ct_put32(p+100,r->lease_kind); ct_put64(p+104,r->lease_id);
    ct_put32(p+112,r->target_map_slot); ct_put32(p+116,r->target_page); ct_put32(p+120,r->target_physical_row);
    ct_put32(p+124,r->bank); ct_put32(p+128,r->slot); ct_put32(p+132,r->physical_row);
    ct_put32(p+136,r->retired_map); ct_put32(p+140,r->retired_control);
    ct_put32(p+144,r->resolution); ct_put64(p+148,r->resolved_lease_id); ct_put64(p+156,r->lease_high_water);
    ct_put32(p+164,OWNED_CONTROL_PAGE_BYTES); ct_put32(p+184,ct_crc(p));
}
static void ct_decode(const uint8_t *p,struct owned_control_record *r)
{
    ct_wipe(r,sizeof(*r)); r->sequence=ct_get64(p+48); r->previous_sequence=ct_get64(p+56);
    ct_copy(r->previous_digest,p+64,32); r->state=ct_get32(p+96); r->lease_kind=ct_get32(p+100);
    r->lease_id=ct_get64(p+104); r->target_map_slot=ct_get32(p+112); r->target_page=ct_get32(p+116);
    r->target_physical_row=ct_get32(p+120); r->bank=ct_get32(p+124); r->slot=ct_get32(p+128);
    r->physical_row=ct_get32(p+132); r->retired_map=ct_get32(p+136); r->retired_control=ct_get32(p+140);
    r->resolution=ct_get32(p+144); r->resolved_lease_id=ct_get64(p+148); r->lease_high_water=ct_get64(p+156);
}
static int ct_descriptor(const uint8_t *d,size_t n,const struct owned_volume_spec *s,
    const struct owned_page_hash *h,struct owned_volume_decoded *decoded)
{
    int rc=owned_volume_descriptor_validate(d,n,s,h,decoded);
    if(rc==OWNED_VOLUME_HASH_ERROR) return OWNED_CONTROL_HASH_ERROR;
    if(rc==OWNED_VOLUME_ARGUMENT) return OWNED_CONTROL_ARGUMENT;
    return rc==OWNED_VOLUME_VALID?OWNED_CONTROL_VALID:OWNED_CONTROL_NOT_VALID;
}
static bool ct_alias_inputs(const void *o,size_t n,const uint8_t *d,size_t dn,
    const struct owned_volume_spec *s,const struct owned_page_hash *h)
{
    return (d && ct_overlap(o,n,d,dn)) || (s && ct_overlap(o,n,s,sizeof(*s))) ||
        (h && ct_overlap(o,n,h,sizeof(*h)));
}
int owned_control_record_build(uint8_t *out,size_t n,const struct owned_control_record *r,
    const uint8_t *d,size_t dn,const struct owned_volume_spec *s,const struct owned_page_hash *h)
{
    if(!out || n!=OWNED_CONTROL_PAGE_BYTES || !r || !ct_hash_valid(h) ||
       ct_overlap(out,n,r,sizeof(*r)) || ct_alias_inputs(out,n,d,dn,s,h)) return OWNED_CONTROL_ARGUMENT;
    struct owned_volume_decoded descriptor;
    int rc=ct_descriptor(d,dn,s,h,&descriptor);
    if(rc!=OWNED_CONTROL_VALID) {
        if(rc==OWNED_CONTROL_HASH_ERROR) ct_wipe(out,n);
        return rc;
    }
    if(!ct_record_valid(r,&descriptor.spec)) { ct_wipe(&descriptor,sizeof(descriptor)); return OWNED_CONTROL_ARGUMENT; }
    uint8_t digest[32]; ct_fill(out,n,0xff); ct_encode(out,r,descriptor.digest);
    rc=ct_digest(h,out,digest);
    if(rc==OWNED_CONTROL_VALID) ct_copy(out+OWNED_CONTROL_DIGEST_OFFSET,digest,32);
    else ct_wipe(out,n);
    ct_wipe(digest,sizeof(digest)); ct_wipe(&descriptor,sizeof(descriptor)); return rc;
}
static int ct_validate_page(const uint8_t *p,uint32_t bank,uint32_t slot,
    const struct owned_volume_decoded *d,const struct owned_page_hash *h,struct owned_control_decoded *out)
{
    ct_wipe(out,sizeof(*out));
    if(!ct_equal(p,control_magic,8) || ct_get32(p+8)!=1 || ct_get32(p+12)!=OWNED_CONTROL_HEADER_BYTES ||
       !ct_equal(p+16,d->digest,32) || ct_get32(p+124)!=bank || ct_get32(p+128)!=slot ||
       ct_get32(p+164)!=OWNED_CONTROL_PAGE_BYTES || !ct_all(p+168,16,0) || !ct_all(p+188,4,0) ||
       !ct_all(p+OWNED_CONTROL_PADDING_OFFSET,OWNED_CONTROL_PAGE_BYTES-OWNED_CONTROL_PADDING_OFFSET,0xff) ||
       ct_get32(p+184)!=ct_crc(p)) return OWNED_CONTROL_NOT_VALID;
    struct owned_control_record r; ct_decode(p,&r);
    if(!ct_record_valid(&r,&d->spec)) { ct_wipe(&r,sizeof(r)); return OWNED_CONTROL_NOT_VALID; }
    uint8_t digest[32]; int rc=ct_digest(h,p,digest);
    if(rc==OWNED_CONTROL_VALID && !ct_equal(digest,p+OWNED_CONTROL_DIGEST_OFFSET,32)) rc=OWNED_CONTROL_NOT_VALID;
    if(rc==OWNED_CONTROL_VALID) { out->record=r; ct_copy(out->digest,digest,32); }
    ct_wipe(&r,sizeof(r)); ct_wipe(digest,sizeof(digest)); return rc;
}
int owned_control_record_validate(const uint8_t *p,size_t n,uint32_t bank,uint32_t slot,
    const uint8_t *d,size_t dn,const struct owned_volume_spec *s,const struct owned_page_hash *h,
    struct owned_control_decoded *out)
{
    if(!out || (p && ct_overlap(out,sizeof(*out),p,n)) || ct_alias_inputs(out,sizeof(*out),d,dn,s,h))
        return OWNED_CONTROL_ARGUMENT;
    ct_wipe(out,sizeof(*out));
    if(!p || n!=OWNED_CONTROL_PAGE_BYTES || bank>=2 || slot>=64 || !ct_hash_valid(h) ||
       ct_alias_inputs(p,n,d,dn,s,h)) return OWNED_CONTROL_ARGUMENT;
    struct owned_volume_decoded descriptor;
    int rc=ct_descriptor(d,dn,s,h,&descriptor);
    if(rc==OWNED_CONTROL_VALID) rc=ct_validate_page(p,bank,slot,&descriptor,h,out);
    ct_wipe(&descriptor,sizeof(descriptor)); return rc;
}
static bool ct_transition(const struct owned_control_record *a,const struct owned_control_record *b)
{
    if((b->retired_map&a->retired_map)!=a->retired_map ||
       (b->retired_control&a->retired_control)!=a->retired_control ||
       (a->state==OWNED_CONTROL_READ_ONLY && b->state!=OWNED_CONTROL_READ_ONLY) ||
       (a->state==OWNED_CONTROL_ACTIVE && b->state==OWNED_CONTROL_PROVISIONING) ||
       b->lease_high_water<a->lease_high_water) return false;
    if(!a->lease_kind && !b->lease_kind)
        return !b->resolution && !b->resolved_lease_id && b->lease_high_water==a->lease_high_water;
    if(!a->lease_kind) {
        return b->lease_id>a->lease_high_water && !(b->retired_map&(1U<<b->target_map_slot)) &&
            b->state==a->state && b->state!=OWNED_CONTROL_READ_ONLY;
    }
    if(b->lease_kind) {
        return b->lease_kind==a->lease_kind && b->lease_id==a->lease_id &&
            b->target_map_slot==a->target_map_slot && b->target_page==a->target_page &&
            b->target_physical_row==a->target_physical_row && b->lease_high_water==a->lease_high_water &&
            (b->state==a->state || b->state==OWNED_CONTROL_READ_ONLY);
    }
    if(b->resolved_lease_id!=a->lease_id || b->lease_high_water!=a->lease_high_water) return false;
    if(b->resolution==OWNED_CONTROL_VERIFIED_COMPLETE)
        return !(b->retired_map&(1U<<a->target_map_slot)) &&
            (b->state==a->state || b->state==OWNED_CONTROL_READ_ONLY);
    return b->resolution==OWNED_CONTROL_ABANDONED_RETIRED &&
        (b->retired_map&(1U<<a->target_map_slot)) && b->state==OWNED_CONTROL_READ_ONLY;
}
int owned_control_select(const struct owned_control_observation *obs,size_t count,
    const uint8_t *d,size_t dn,const struct owned_volume_spec *s,const struct owned_page_hash *h,
    struct owned_control_workspace *work,struct owned_control_selection *out)
{
    if(!obs || count!=OWNED_CONTROL_RECORDS || !work || !out || !ct_hash_valid(h) ||
       ct_overlap(work,sizeof(*work),out,sizeof(*out)) ||
       ct_overlap(work,sizeof(*work),obs,count*sizeof(*obs)) ||
       ct_overlap(out,sizeof(*out),obs,count*sizeof(*obs)) ||
       ct_alias_inputs(work,sizeof(*work),d,dn,s,h) || ct_alias_inputs(out,sizeof(*out),d,dn,s,h))
        return OWNED_CONTROL_ARGUMENT;
    /* Check every span/observation before clearing outputs, including later rows. */
    for(size_t i=0;i<count;i++) {
        if(obs[i].kind>OWNED_CONTROL_UNREADABLE ||
           (obs[i].kind==OWNED_CONTROL_PAGE_PRESENT)!= (obs[i].page!=NULL)) return OWNED_CONTROL_ARGUMENT;
        if(obs[i].page && (ct_overlap(work,sizeof(*work),obs[i].page,OWNED_CONTROL_PAGE_BYTES) ||
           ct_overlap(out,sizeof(*out),obs[i].page,OWNED_CONTROL_PAGE_BYTES) ||
           ct_overlap(obs,count*sizeof(*obs),obs[i].page,OWNED_CONTROL_PAGE_BYTES) ||
           ct_alias_inputs(obs[i].page,OWNED_CONTROL_PAGE_BYTES,d,dn,s,h))) return OWNED_CONTROL_ARGUMENT;
    }
    ct_wipe(work,sizeof(*work)); ct_wipe(out,sizeof(*out));
    struct owned_volume_decoded descriptor;
    int rc=ct_descriptor(d,dn,s,h,&descriptor);
    if(rc!=OWNED_CONTROL_VALID) { ct_wipe(&descriptor,sizeof(descriptor)); return rc; }
    unsigned best=0,valid_count=0; uint32_t reasons=0,retired_map=0,retired_control=0;
    for(unsigned i=0;i<OWNED_CONTROL_RECORDS;i++) {
        if(obs[i].kind==OWNED_CONTROL_PROVEN_UNUSED) continue;
        if(obs[i].kind!=OWNED_CONTROL_PAGE_PRESENT) { reasons|=OWNED_CONTROL_UNKNOWN; continue; }
        rc=ct_validate_page(obs[i].page,i/64U,i%64U,&descriptor,h,&work->records[i]);
        if(rc==OWNED_CONTROL_HASH_ERROR) {
            ct_wipe(work,sizeof(*work)); ct_wipe(&descriptor,sizeof(descriptor)); return rc;
        }
        if(rc!=OWNED_CONTROL_VALID) { reasons|=OWNED_CONTROL_BAD_RECORD; continue; }
        work->valid[i]=1;
        /* Even a broken chain cannot erase earlier valid retirement evidence.
         * Union is conservative metadata; any regression still blocks selection. */
        retired_map|=work->records[i].record.retired_map;
        retired_control|=work->records[i].record.retired_control;
        if(!valid_count || work->records[i].record.sequence>work->records[best].record.sequence) best=i;
        valid_count++;
        for(unsigned j=0;j<i;j++) if(work->valid[j] &&
            work->records[j].record.sequence==work->records[i].record.sequence)
            reasons|=OWNED_CONTROL_DUPLICATE_SEQUENCE;
    }
    if(valid_count) {
        const struct owned_control_decoded *last=&work->records[best];
        out->latest_valid=1; out->latest_bank=last->record.bank; out->latest_slot=last->record.slot;
        out->latest_sequence=last->record.sequence; ct_copy(out->latest_digest,last->digest,32);
        out->retired_map=retired_map; out->retired_control=retired_control;
        /* Missing or reordered initial history is never checkpoint authority. */
        if(!work->valid[0] || work->records[0].record.sequence!=1 || best+1U!=valid_count)
            reasons|=OWNED_CONTROL_INCOMPLETE_HISTORY;
        for(unsigned i=0;i<OWNED_CONTROL_RECORDS;i++) if(work->valid[i]) {
            const struct owned_control_record *r=&work->records[i].record;
            if(r->sequence!=(uint64_t)i+1U) reasons|=OWNED_CONTROL_INCOMPLETE_HISTORY;
            if(i) {
                const struct owned_control_decoded *previous=&work->records[i-1U];
                if(!work->valid[i-1U] || r->previous_sequence!=previous->record.sequence ||
                   !ct_equal(r->previous_digest,previous->digest,32)) reasons|=OWNED_CONTROL_CHAIN_GAP;
                else if(!ct_transition(&previous->record,r)) reasons|=OWNED_CONTROL_BAD_TRANSITION;
            }
        }
        if(last->record.lease_kind) {
            reasons|=OWNED_CONTROL_PENDING_LEASE; out->pending_lease_valid=1;
            out->pending_lease_id=last->record.lease_id; out->pending_map_slot=last->record.target_map_slot;
            out->pending_physical_block=descriptor.spec.map_blocks[last->record.target_map_slot];
        }
        if(retired_control) reasons|=OWNED_CONTROL_RETIRED_CONTROL;
        if(last->record.state!=OWNED_CONTROL_ACTIVE) reasons|=OWNED_CONTROL_NOT_ACTIVE;
        out->kind=reasons?OWNED_CONTROL_RECOVER_READ_ONLY:OWNED_CONTROL_CLEAN_CANDIDATE;
    } else {
        out->kind=reasons?OWNED_CONTROL_RECOVER_READ_ONLY:OWNED_CONTROL_NO_CANDIDATE;
    }
    out->reasons=reasons;
    if(reasons&(OWNED_CONTROL_UNKNOWN|OWNED_CONTROL_BAD_RECORD|OWNED_CONTROL_DUPLICATE_SEQUENCE|
       OWNED_CONTROL_CHAIN_GAP|OWNED_CONTROL_BAD_TRANSITION|OWNED_CONTROL_INCOMPLETE_HISTORY))
        out->quarantine_all_map_tails=1;
    ct_wipe(work,sizeof(*work)); ct_wipe(&descriptor,sizeof(descriptor)); return OWNED_CONTROL_VALID;
}
