/* SPDX-License-Identifier: Apache-2.0 */
#include "owned_dhara.h"
#include <stddef.h>
#include <string.h>
#ifdef OPENPENDANT_NATIVE_STORAGE
#include "recording_native_store.h"
#endif
#ifdef OPENPENDANT_DHARA_TRIAL
#include "dhara_trial.h"
#endif
#define OD_MAGIC 0x4f444831U
_Static_assert(offsetof(struct owned_dhara,nand)==0,"Dhara context layout");
_Static_assert(offsetof(struct owned_dhara,main)%4U==0 &&
    offsetof(struct owned_dhara,raw)%4U==0,"Permanent IO buffer alignment");
_Static_assert(OWNED_VOLUME_MAP_BLOCKS==32 && OWNED_VOLUME_LOGICAL_PAGES==2048 &&
    OWNED_PAGE_PAYLOAD_BYTES==2048 && OWNED_PAGE_BYTES==4096,"Fixed geometry");
static void wipe(void *p,size_t n) { volatile uint8_t *v=p; while(n--) *v++=0; }
static int eq(const void *a,const void *b,size_t n) { return memcmp(a,b,n)==0; }
static int nonzero(const uint8_t *p,size_t n)
{ unsigned v=0; while(n--) v|=*p++; return v!=0; }
static int alias(const void *a,size_t an,const void *b,size_t bn)
{ uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
  return an>UINTPTR_MAX-x || bn>UINTPTR_MAX-y || (x<y+bn && y<x+an); }
static unsigned good(uint32_t retired)
{ unsigned count=32; while(retired) { count-=retired&1U; retired>>=1; } return count; }
static int fail(struct owned_dhara *d,uint32_t why,dhara_error_t *err)
{ if(!d->fault) d->fault=why; d->writable=0; dhara_set_error(err,DHARA_E_ECC); return -1; }
static int time_ok(struct owned_dhara *d,dhara_error_t *err)
{ uint64_t now=d->io.now_ms(d->io.user);
  if(now<d->last_now || now>=d->deadline) return fail(d,OD_FAULT_DEADLINE,err);
  d->last_now=now; return 0; }
static void scrub(struct owned_dhara *d)
{ if(!d->retained_dma) { wipe(d->main,sizeof(d->main)); wipe(d->raw,sizeof(d->raw));
    wipe(d->payload,sizeof(d->payload)); } }
static struct owned_dhara *ctx(const struct dhara_nand *n)
{ return (struct owned_dhara *)(void *)n; }
static int enter(struct owned_dhara *d,uint32_t page,dhara_error_t *err)
{
    if(d->magic!=OD_MAGIC || !d->busy || d->nand.log2_page_size!=11 ||
       d->nand.log2_ppb!=6 || d->nand.num_blocks!=32 || page>=2048)
        return fail(d,OD_FAULT_ADDRESS,err);
    if(d->fault) { dhara_set_error(err,DHARA_E_ECC); return -1; }
    return time_ok(d,err);
}
static uint32_t row(const struct owned_dhara *d,uint32_t p)
{ return d->volume.spec.map_blocks[p/64U]*64U+p%64U; }
static int io_result(struct owned_dhara *d,int rc,dhara_error_t *err)
{
    if(rc==OD_IO_OK) return time_ok(d,err);
    if(rc==OD_IO_UNCERTAIN) d->retained_dma=1;
    return fail(d,OD_FAULT_TRANSPORT,err);
}
static int info(struct owned_dhara *d,uint32_t p,struct owned_dhara_page_info *i,
                dhara_error_t *err)
{
    memset(i,0,sizeof(*i));
    if(time_ok(d,err)) return -1;
    if(d->io.page_info(d->io.user,row(d,p),i,d->deadline) ||
       (i->state!=OD_PAGE_UNUSED && i->state!=OD_PAGE_COMMITTED &&
        !(i->state==OD_PAGE_ABANDONED && d->io.lease_mode==OD_LEASE_RESERVED_RANGE)) ||
       (i->state==OD_PAGE_UNUSED && i->lease_id) ||
       (i->state==OD_PAGE_COMMITTED && !i->lease_id)) return fail(d,OD_FAULT_HISTORY,err);
    return time_ok(d,err);
}
static int raw_unused(struct owned_dhara *d,uint32_t p,dhara_error_t *err)
{
    if(time_ok(d,err) || io_result(d,d->io.read_raw(d->io.user,row(d,p),d->raw,d->deadline),err)) return -1;
    unsigned different=0;
    for(size_t i=0;i<sizeof(d->raw);i++) different|=(unsigned)(d->raw[i]^0xffU);
    wipe(d->raw,sizeof(d->raw));
    return different?fail(d,OD_FAULT_HISTORY,err):0;
}
static struct owned_page_binding binding(const struct owned_dhara *d,uint32_t p,uint64_t lease)
{
    struct owned_page_binding b={0};
    memcpy(b.device_id,d->volume.spec.device_id,16); memcpy(b.volume_id,d->volume.spec.volume_id,16);
    b.generation=d->volume.spec.generation; b.logical_page=p; b.physical_row=row(d,p);
    b.lease_id=lease; b.kind=OWNED_PAGE_DATA; return b;
}
static int writable(struct owned_dhara *d,dhara_error_t *err)
{
    if(!d->writable || d->media_pending) return fail(d,OD_FAULT_LEASE,err);
    if(good(d->retired_map)<19) return fail(d,OD_FAULT_CAPACITY,err);
    return 0;
}
static int acquire(struct owned_dhara *d,uint32_t kind,uint32_t p,dhara_error_t *err)
{
    uint32_t slot=p/64U;
    if(writable(d,err) || (d->retired_map&(1U<<slot))) return fail(d,OD_FAULT_LEASE,err);
    if(d->sequence==UINT64_MAX || d->lease_high_water==UINT64_MAX)
        return fail(d,OD_FAULT_LEASE,err);
    memset(&d->pending,0,sizeof(d->pending));
    if(time_ok(d,err)) return -1;
    if(d->io.acquire(d->io.user,kind,slot,row(d,p),&d->pending,d->deadline) ||
       !eq(d->pending.descriptor_digest,d->volume.digest,32) ||
       d->pending.kind!=kind || d->pending.map_slot!=slot ||
       d->pending.physical_row!=row(d,p) || !d->pending.lease_id)
        return fail(d,OD_FAULT_LEASE,err);
    if(d->io.lease_mode==OD_LEASE_PER_OPERATION) {
        if(d->pending.range_base || d->pending.lease_id<=d->lease_high_water ||
           d->pending.sequence<=d->sequence) return fail(d,OD_FAULT_LEASE,err);
    } else {
        uint64_t base=d->pending.range_base;
        if(!base || base>UINT64_MAX-64U || d->pending.sequence<d->sequence)
            return fail(d,OD_FAULT_LEASE,err);
        if(kind==OWNED_CONTROL_ERASE_BLOCK) {
            if(base!=d->pending.lease_id || base<=d->lease_high_water ||
               d->pending.sequence<=d->sequence) return fail(d,OD_FAULT_LEASE,err);
        } else {
            uint64_t bit=UINT64_C(1)<<(p%64U);
            if(base!=d->range_base[slot] || d->pending.lease_id!=base+1U+p%64U ||
               (d->range_consumed[slot]&bit)) return fail(d,OD_FAULT_LEASE,err);
            d->range_consumed[slot]|=bit; /* Before START, including ambiguity. */
        }
    }
    d->sequence=d->pending.sequence;
    uint64_t high=d->io.lease_mode==OD_LEASE_RESERVED_RANGE?d->pending.range_base+64U:d->pending.lease_id;
    if(high>d->lease_high_water) d->lease_high_water=high;
    return time_ok(d,err);
}
static int mutation_result(struct owned_dhara *d,int rc,dhara_error_t *err)
{
    if(rc==OD_IO_MEDIA) {
        d->media_pending=1; d->retired_map|=1U<<d->pending.map_slot;
        /* Dhara can copy surviving pages to a new block BEFORE its delayed
         * mark_bad callback. Persist exclusion now, before returning BAD_BLOCK;
         * its eventual mark_bad is then an idempotent acknowledgement. */
        dhara_nand_mark_bad(&d->nand,d->pending.map_slot);
        if(d->fault) { dhara_set_error(err,DHARA_E_ECC); return -1; }
        dhara_set_error(err,DHARA_E_BAD_BLOCK); return -1;
    }
    if(io_result(d,rc,err)) return -1;
    if(time_ok(d,err)) return -1;
    if(d->io.resolve(d->io.user,&d->pending,d->deadline)) return fail(d,OD_FAULT_RESOLVE,err);
    if(time_ok(d,err)) return -1;
    if(d->io.lease_mode==OD_LEASE_RESERVED_RANGE && d->pending.kind==OWNED_CONTROL_ERASE_BLOCK) {
        d->range_base[d->pending.map_slot]=d->pending.range_base;
        d->range_consumed[d->pending.map_slot]=0;
    }
    memset(&d->pending,0,sizeof(d->pending)); return 0;
}
int dhara_nand_is_bad(const struct dhara_nand *n,dhara_block_t b)
{
#ifdef OPENPENDANT_NATIVE_STORAGE
    if(rns_owns(n))return rns_is_bad(n,b);
#endif
#ifdef OPENPENDANT_DHARA_TRIAL
 if(dt_owns(n))return dt_is_bad(n,b);
#endif
 struct owned_dhara *d=ctx(n); if(b>=32 || enter(d,b*64U,NULL)) {
    if(b>=32) fail(d,OD_FAULT_ADDRESS,NULL);
    return 1; }
  return (d->retired_map&(1U<<b))!=0; }
void dhara_nand_mark_bad(const struct dhara_nand *n,dhara_block_t b)
{
#ifdef OPENPENDANT_NATIVE_STORAGE
    if(rns_owns(n)){rns_mark_bad(n,b);return;}
#endif
#ifdef OPENPENDANT_DHARA_TRIAL
    if(dt_owns(n)){dt_mark_bad(n,b);return;}
#endif
    struct owned_dhara *d=ctx(n);
    if(b>=32 || enter(d,b*64U,NULL)) { if(b>=32) fail(d,OD_FAULT_ADDRESS,NULL); return; }
    if(!d->media_pending && (d->retired_map&(1U<<b))) return;
    /* Dhara may only retire the exact diagnosed failure. No guessed bad mark. */
    if(!d->writable || !d->media_pending || d->pending.map_slot!=b) {
        fail(d,OD_FAULT_RETIRE,NULL); return; }
    if(time_ok(d,NULL)) return;
    if(d->io.retire(d->io.user,b,d->volume.spec.map_blocks[b],d->retired_map,&d->pending,d->deadline)) {
        fail(d,OD_FAULT_RETIRE,NULL); return; }
    if(time_ok(d,NULL)) return;
    d->media_pending=0; memset(&d->pending,0,sizeof(d->pending));
    if(good(d->retired_map)<19) fail(d,OD_FAULT_CAPACITY,NULL);
}
int dhara_nand_erase(const struct dhara_nand *n,dhara_block_t b,dhara_error_t *err)
{
#ifdef OPENPENDANT_NATIVE_STORAGE
    if(rns_owns(n))return rns_erase(n,b,err);
#endif
#ifdef OPENPENDANT_DHARA_TRIAL
    if(dt_owns(n))return dt_erase(n,b,err);
#endif
    struct owned_dhara *d=ctx(n);
    if(b>=32) return fail(d,OD_FAULT_ADDRESS,err);
    if(enter(d,b*64U,err) || acquire(d,OWNED_CONTROL_ERASE_BLOCK,b*64U,err)) return -1;
    return mutation_result(d,d->io.erase(d->io.user,d->volume.spec.map_blocks[b],d->deadline),err);
}
int dhara_nand_prog(const struct dhara_nand *n,dhara_page_t p,const uint8_t *data,dhara_error_t *err)
{
#ifdef OPENPENDANT_NATIVE_STORAGE
    if(rns_owns(n))return rns_prog(n,p,data,err);
#endif
#ifdef OPENPENDANT_DHARA_TRIAL
    if(dt_owns(n))return dt_prog(n,p,data,err);
#endif
    struct owned_dhara *d=ctx(n);
    if(enter(d,p,err) || !data) return fail(d,OD_FAULT_ADDRESS,err);
    struct owned_dhara_page_info i;
    if(info(d,p,&i,err)) return -1;
    if(i.state!=OD_PAGE_UNUSED) return fail(d,OD_FAULT_HISTORY,err);
    if(acquire(d,OWNED_CONTROL_PROGRAM_PAGE,p,err)) return -1;
    struct owned_page_binding b=binding(d,p,d->pending.lease_id);
    int rc=owned_page_build_data(d->main,sizeof(d->main),&b,data,2048,&d->hash);
    if(rc!=OWNED_PAGE_VALID) return fail(d,OD_FAULT_HASH,err);
    if(time_ok(d,err)) return -1;
    rc=mutation_result(d,d->io.program(d->io.user,row(d,p),d->main,d->deadline),err);
    if(!d->retained_dma) wipe(d->main,sizeof(d->main));
    return rc;
}
int dhara_nand_is_free(const struct dhara_nand *n,dhara_page_t p)
{
#ifdef OPENPENDANT_NATIVE_STORAGE
    if(rns_owns(n))return rns_is_free(n,p);
#endif
#ifdef OPENPENDANT_DHARA_TRIAL
    if(dt_owns(n))return dt_is_free(n,p);
#endif
    struct owned_dhara *d=ctx(n); struct owned_dhara_page_info i;
    if(enter(d,p,NULL) || info(d,p,&i,NULL)) return 0;
    if(i.state!=OD_PAGE_UNUSED) return 0;
    return raw_unused(d,p,NULL)==0;
}
int dhara_nand_read(const struct dhara_nand *n,dhara_page_t p,size_t off,size_t len,
                    uint8_t *data,dhara_error_t *err)
{
#ifdef OPENPENDANT_NATIVE_STORAGE
    if(rns_owns(n))return rns_nand_read(n,p,off,len,data,err);
#endif
#ifdef OPENPENDANT_DHARA_TRIAL
    if(dt_owns(n))return dt_read(n,p,off,len,data,err);
#endif
    struct owned_dhara *d=ctx(n); struct owned_dhara_page_info i;
    if(enter(d,p,err) || !data || off>2048 || len>2048-off)
        return fail(d,OD_FAULT_ADDRESS,err);
    if(info(d,p,&i,err)) return -1;
    if(i.state==OD_PAGE_ABANDONED) { memset(data,0xff,len); return 0; }
    if(i.state==OD_PAGE_UNUSED) {
        if(raw_unused(d,p,err)) return -1;
        memset(data,0xff,len); return 0;
    }
    uint32_t ecc=UINT32_MAX;
    if(time_ok(d,err) || io_result(d,d->io.read_corrected(d->io.user,row(d,p),d->main,&ecc,d->deadline),err)) return -1;
    if(ecc!=0 && ecc!=1 && ecc!=3 && ecc!=5) {
        wipe(d->main,sizeof(d->main)); return fail(d,OD_FAULT_ECC,err); }
    struct owned_page_binding b=binding(d,p,i.lease_id); struct owned_page_view view;
    int rc=owned_page_validate(d->main,sizeof(d->main),&b,&d->hash,&view);
    if(rc==OWNED_PAGE_VALID && time_ok(d,err)) rc=OWNED_PAGE_NOT_VALID;
    if(rc==OWNED_PAGE_VALID) memcpy(data,view.payload+off,len);
    wipe(d->main,sizeof(d->main));
    return rc==OWNED_PAGE_VALID?0:fail(d,rc==OWNED_PAGE_HASH_ERROR?OD_FAULT_HASH:OD_FAULT_ENVELOPE,err);
}
int dhara_nand_copy(const struct dhara_nand *n,dhara_page_t src,dhara_page_t dst,dhara_error_t *err)
{
#ifdef OPENPENDANT_NATIVE_STORAGE
    if(rns_owns(n))return rns_copy(n,src,dst,err);
#endif
#ifdef OPENPENDANT_DHARA_TRIAL
    if(dt_owns(n))return dt_copy(n,src,dst,err);
#endif
    struct owned_dhara *d=ctx(n);
    if(dhara_nand_read(n,src,0,2048,d->payload,err)) return -1;
    int rc=dhara_nand_prog(n,dst,d->payload,err);
    if(!d->retained_dma) wipe(d->payload,sizeof(d->payload));
    return rc;
}
static int valid(const struct owned_dhara *d) { return d && d->magic==OD_MAGIC; }
static uint32_t capacity_limit(const struct owned_dhara *d)
{
    /* Dhara's bad-block estimate can lag the authoritative retirement bitmap.
     * Bound allocation by BOTH, avoiding its unsigned small-pool underflow.
     * Sector identity remains0..417 so retirement does not hide existing data. */
    unsigned blocks=good(d->retired_map);
    if(blocks<19) return 0;
    uint32_t conservative=30U*(blocks-1U)-512U;
    uint32_t library=dhara_map_capacity(&d->map);
    return library<conservative?library:conservative;
}
static int begin(struct owned_dhara *d,int writing,uint64_t limit)
{
    if(!valid(d)) return OD_ARGUMENT;
    if(d->busy) return OD_BUSY;
    if(d->fault) return OD_FATAL;
    if(writing && !d->writable) return OD_READ_ONLY;
    uint64_t now=d->io.now_ms(d->io.user);
    if(now<d->last_now) { fail(d,OD_FAULT_DEADLINE,NULL); return OD_FATAL; }
    d->last_now=now;
    if(d->last_now>UINT64_MAX-d->io.operation_budget_ms) {
        fail(d,OD_FAULT_DEADLINE,NULL); return OD_FATAL; }
    d->deadline=d->last_now+d->io.operation_budget_ms;
    if(limit<d->deadline) d->deadline=limit;
    if(d->deadline<=d->last_now) { fail(d,OD_FAULT_DEADLINE,NULL); return OD_FATAL; }
    d->busy=1; d->library_error=DHARA_E_NONE; return OD_OK;
}
static int end(struct owned_dhara *d,int rc)
{
    if(!d->fault) time_ok(d,NULL);
    if(d->media_pending && !d->fault) fail(d,OD_FAULT_RETIRE,NULL);
    scrub(d); d->busy=0;
    return d->fault?OD_FATAL:(rc<0?OD_LIBRARY:OD_OK);
}
int owned_dhara_init(struct owned_dhara *d,const uint8_t descriptor[512],
    const struct owned_volume_spec *expected,const struct owned_page_hash *hash,
    const struct owned_dhara_io *io,uint32_t retired)
{
    if(!d || d->magic || !descriptor || !expected || !hash || !hash->sha256 || !io ||
       !io->now_ms || !io->operation_budget_ms || io->operation_budget_ms>120000 ||
       !io->validate_capability || !io->page_info || !io->read_corrected || !io->read_raw ||
       !io->program || !io->erase || !io->acquire || !io->resolve || !io->retire ||
       io->lease_mode>OD_LEASE_RESERVED_RANGE ||
       (io->lease_mode==OD_LEASE_RESERVED_RANGE && !io->history_sync) ||
       alias(d,sizeof(*d),descriptor,512) || alias(d,sizeof(*d),expected,sizeof(*expected)) ||
       alias(d,sizeof(*d),hash,sizeof(*hash)) || alias(d,sizeof(*d),io,sizeof(*io)) || good(retired)<19)
        return OD_ARGUMENT;
    struct owned_volume_decoded v;
    if(owned_volume_descriptor_validate(descriptor,512,expected,hash,&v)!=OWNED_VOLUME_VALID)
        return OD_ARGUMENT;
    memset(d,0,sizeof(*d)); d->volume=v; d->hash=*hash; d->io=*io;
    d->magic=OD_MAGIC; d->retired_map=retired; d->nand.log2_page_size=11;
    d->nand.log2_ppb=6; d->nand.num_blocks=32;
    dhara_map_init(&d->map,&d->nand,d->metadata,1); return OD_OK;
}
int owned_dhara_resume(struct owned_dhara *d,uint64_t deadline)
{
    int rc=begin(d,0,deadline); if(rc) return rc;
    d->writable=0; d->ready=0; d->provisioning=0;
    dhara_map_init(&d->map,&d->nand,d->metadata,1);
    rc=dhara_map_resume(&d->map,&d->library_error);
    if(!rc && !d->fault) d->ready=1;
    int answer=end(d,rc); return answer==OD_LIBRARY?OD_NO_JOURNAL:answer;
}
static int capability(struct owned_dhara *d,const struct owned_dhara_capability *c,uint32_t state)
{
    if(!c || alias(c,sizeof(*c),d,sizeof(*d)) || c->state!=state || !c->sequence ||
       c->sequence<d->sequence || c->lease_high_water<d->lease_high_water ||
       !eq(c->descriptor_digest,d->volume.digest,32) ||
       !nonzero(c->control_digest,32) || (c->retired_map&d->retired_map)!=d->retired_map ||
       good(c->retired_map)<19) return OD_ARGUMENT;
    if(time_ok(d,NULL)) return OD_FATAL;
    if(d->io.validate_capability(d->io.user,c,d->deadline)) return OD_READ_ONLY;
    if(time_ok(d,NULL)) return OD_FATAL;
    d->capability=*c; d->sequence=c->sequence; d->lease_high_water=c->lease_high_water;
    d->retired_map=c->retired_map; d->writable=1;
    return OD_OK;
}
int owned_dhara_grant(struct owned_dhara *d,const struct owned_dhara_capability *c,uint64_t deadline)
{
    int rc=begin(d,0,deadline); if(rc) return rc;
    d->writable=0; /* A denied/expired replacement never preserves old authority. */
    if(!d->ready) rc=OD_READ_ONLY;
    else rc=capability(d,c,OWNED_CONTROL_ACTIVE);
    if(!rc) d->provisioning=0;
    d->busy=0; return rc;
}
int owned_dhara_format(struct owned_dhara *d,const struct owned_dhara_capability *c,uint64_t deadline)
{
    int rc=begin(d,0,deadline); if(rc) return rc;
    if(d->ready || d->writable || d->sequence) { d->busy=0; return OD_READ_ONLY; }
    rc=capability(d,c,OWNED_CONTROL_PROVISIONING);
    if(rc) { d->busy=0; return rc; }
    d->provisioning=1;
    for(unsigned b=0;b<32;b++) if(!(d->retired_map&(1U<<b))) {
        if(dhara_nand_erase(&d->nand,b,&d->library_error)) {
            if(d->library_error==DHARA_E_BAD_BLOCK && !d->fault) {
                dhara_nand_mark_bad(&d->nand,b); if(!d->fault) continue; }
            return end(d,-1);
        }
    }
    dhara_map_init(&d->map,&d->nand,d->metadata,1); d->ready=1;
    return end(d,0);
}
int owned_dhara_read(struct owned_dhara *d,uint32_t sector,uint8_t out[2048],uint64_t deadline)
{
    if(!valid(d) || !out || alias(out,2048,d,sizeof(*d))) return OD_ARGUMENT;
    int rc=begin(d,0,deadline); if(rc) return rc;
    if(!d->ready || sector>=418U) { d->busy=0; return OD_ARGUMENT; }
    dhara_page_t page;
    rc=dhara_map_find(&d->map,sector,&page,&d->library_error);
    if(rc && !d->fault && d->library_error==DHARA_E_NOT_FOUND) {
        int missing=end(d,0); return missing==OD_OK?OD_MISSING:missing; }
    if(!rc) rc=dhara_nand_read(&d->nand,page,0,2048,d->payload,&d->library_error);
    if(!rc && !d->fault && !time_ok(d,NULL)) memcpy(out,d->payload,2048);
    return end(d,rc);
}
int owned_dhara_write(struct owned_dhara *d,uint32_t sector,const uint8_t data[2048],uint64_t deadline)
{
    if(!valid(d) || !data || alias(data,2048,d,sizeof(*d))) return OD_ARGUMENT;
    int rc=begin(d,1,deadline); if(rc) return rc;
    if(!d->ready || sector>=418U) { d->busy=0; return OD_ARGUMENT; }
    dhara_page_t existing;
    rc=dhara_map_find(&d->map,sector,&existing,&d->library_error);
    if(rc && (d->fault || d->library_error!=DHARA_E_NOT_FOUND)) return end(d,rc);
    if(rc && dhara_map_size(&d->map)>=capacity_limit(d)) {
        d->library_error=DHARA_E_MAP_FULL; return end(d,-1); }
    /* Dhara receives caller bytes synchronously; physical callback rebuilds
     * into permanent main[], so no physical HAL may retain this caller span. */
    rc=dhara_map_write(&d->map,sector,data,&d->library_error); return end(d,rc);
}
int owned_dhara_sync(struct owned_dhara *d,uint64_t deadline)
{
    int rc=begin(d,1,deadline); if(rc) return rc;
    if(!d->ready) { d->busy=0; return OD_READ_ONLY; }
    rc=dhara_map_sync(&d->map,&d->library_error);
    if(!rc && !d->fault && d->io.lease_mode==OD_LEASE_RESERVED_RANGE) {
        if(time_ok(d,NULL) || d->io.history_sync(d->io.user,d->deadline) || time_ok(d,NULL))
            fail(d,OD_FAULT_RESOLVE,NULL);
    }
    return end(d,rc);
}
int owned_dhara_capacity(const struct owned_dhara *d,uint32_t *sectors)
{
    if(!valid(d) || !sectors || alias(sectors,sizeof(*sectors),d,sizeof(*d))) return OD_ARGUMENT;
    if(d->busy) return OD_BUSY;
    if(d->fault) return OD_FATAL;
    if(!d->ready) return OD_READ_ONLY;
    *sectors=capacity_limit(d); return OD_OK;
}
void owned_dhara_revoke(struct owned_dhara *d) { if(valid(d) && !d->busy) d->writable=0; }
