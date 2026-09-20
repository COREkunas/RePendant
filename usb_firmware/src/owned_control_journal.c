/* SPDX-License-Identifier: Apache-2.0 */
#include "owned_control_journal.h"
#include <string.h>
#define OCJ_MAGIC 0x4f434a32U
static const uint8_t magic[8]={'O','P','N','D','C','J','2',0};
static void wipe(void *p,size_t n) { volatile uint8_t *v=p; while(n--) *v++=0; }
static int equal(const void *a,const void *b,size_t n) { return memcmp(a,b,n)==0; }
static int overlap(const void *a,size_t an,const void *b,size_t bn)
{ uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
  return an>UINTPTR_MAX-x || bn>UINTPTR_MAX-y || (x<y+bn && y<x+an); }
static int all(const uint8_t *p,size_t n,uint8_t value)
{ unsigned d=0; while(n--) d|=*p++^value; return !d; }
static void put32(uint8_t *p,uint32_t v) { for(unsigned i=0;i<4;i++) p[i]=(uint8_t)(v>>(i*8)); }
static void put64(uint8_t *p,uint64_t v) { for(unsigned i=0;i<8;i++) p[i]=(uint8_t)(v>>(i*8)); }
static uint32_t get32(const uint8_t *p) { uint32_t v=0; for(unsigned i=0;i<4;i++) v|=(uint32_t)p[i]<<(i*8); return v; }
static uint64_t get64(const uint8_t *p) { uint64_t v=0; for(unsigned i=0;i<8;i++) v|=(uint64_t)p[i]<<(i*8); return v; }
static uint32_t crc(const uint8_t *p)
{ uint32_t v=UINT32_MAX; for(unsigned i=0;i<OCJ_BODY_BYTES;i++) {
    v^=(i>=252 && i<256)?0:p[i]; for(unsigned b=0;b<8;b++) v=(v>>1)^((0U-(v&1U))&0xedb88320U); } return ~v; }
static int sha(const struct owned_page_hash *h,const uint8_t *p,uint8_t out[32])
{ size_t actual=0; memset(out,0,32); return !h || !h->sha256 ||
    h->sha256(h->user,p,OCJ_BODY_BYTES,out,32,&actual) || actual!=32 ? -1:0; }
static int syntax(const struct ocj_snapshot *s)
{
    if(!s->epoch || s->epoch>INT64_MAX || s->previous_epoch!=s->epoch-1 ||
       s->bank>1 || s->slot>63 || s->state<1 || s->state>3 ||
       (s->epoch==1 && (s->bank || s->slot || s->high_water || s->pending_kind ||
        s->state!=OWNED_CONTROL_PROVISIONING || s->retired || s->quarantine || s->open)) ||
       (s->epoch==1?!all(s->previous_digest,32,0):all(s->previous_digest,32,0)) ||
       (s->retired&~s->quarantine) || (s->open&s->quarantine)) return 0;
    if(s->pending_kind!=0 && s->pending_kind!=OWNED_CONTROL_ERASE_BLOCK) return 0;
    if(!s->pending_kind) { if(s->pending_slot!=UINT32_MAX || s->pending_lease) return 0; }
    else if(s->pending_slot>=32 || !s->pending_lease ||
        !(s->quarantine&(1U<<s->pending_slot)) || (s->retired&(1U<<s->pending_slot))) return 0;
    for(unsigned i=0;i<32;i++) {
        const struct ocj_block *b=&s->blocks[i];
        if(b->consumed>64 || (!b->base && (b->valid || b->range_sequence || b->consumed!=64)) ||
           (b->base && (b->base>UINT64_MAX-64U || b->base+64U>s->high_water ||
            !b->range_sequence || b->range_sequence>s->epoch)) ||
           (b->consumed<64 && (b->valid>>b->consumed)) ||
           ((s->open&(1U<<i)) && (!b->base || b->consumed==64))) return 0;
        /* A reservation includes one erase ID followed by64 program IDs.
         * Slots cannot share even a boundary ID, regardless of valid pages. */
        if(b->base) for(unsigned k=0;k<i;k++) {
            const struct ocj_block *a=&s->blocks[k];
            if(a->base && b->base<=a->base+64U && a->base<=b->base+64U) return 0;
        }
    }
    if(s->pending_kind) {
        const struct ocj_block *b=&s->blocks[s->pending_slot];
        if(b->base!=s->pending_lease || b->valid || b->consumed!=64) return 0;
    }
    return 1;
}
int ocj_encode(const struct owned_volume_decoded *v,const struct ocj_snapshot *s,
               const struct owned_page_hash *h,uint8_t out[4096])
{
    if(!v || !s || !h || !out || !syntax(s) || overlap(out,4096,v,sizeof(*v)) ||
       overlap(out,4096,s,sizeof(*s)) || overlap(out,4096,h,sizeof(*h))) return OCJ_ARGUMENT;
    memset(out,0xff,4096); memset(out,0,OCJ_BODY_BYTES); memcpy(out,magic,8);
    put32(out+8,2); put32(out+12,256); memcpy(out+16,v->digest,32);
    put64(out+48,s->epoch); put64(out+56,s->previous_epoch); memcpy(out+64,s->previous_digest,32);
    put32(out+96,s->state); put32(out+100,s->bank); put32(out+104,s->slot);
    put32(out+108,v->spec.control_blocks[s->bank]*64U+s->slot); put32(out+112,1U-s->bank);
    put32(out+116,s->retired); put32(out+120,s->quarantine); put32(out+124,s->open);
    put64(out+128,s->high_water); put32(out+136,s->pending_kind); put32(out+140,s->pending_slot);
    put32(out+144,s->pending_kind?v->spec.map_blocks[s->pending_slot]*64U:UINT32_MAX);
    put64(out+152,s->pending_lease); put32(out+160,4096); put32(out+164,OCJ_BODY_BYTES);
    for(unsigned i=0;i<32;i++) { const struct ocj_block *b=&s->blocks[i]; uint8_t *p=out+256+i*32;
        put64(p,b->base); put64(p+8,b->valid); put64(p+16,b->range_sequence); put32(p+24,b->consumed); }
    put32(out+252,crc(out));
    if(sha(h,out,out+OCJ_DIGEST_OFFSET)) { wipe(out,4096); return OCJ_FAULT; } return 0;
}
int ocj_decode(const struct owned_volume_decoded *v,uint32_t bank,uint32_t slot,
               const struct owned_page_hash *h,const uint8_t p[4096],struct ocj_snapshot *s)
{
    if(!v || !h || bank>1 || slot>63 || !p || !s || overlap(s,sizeof(*s),p,4096) ||
       overlap(s,sizeof(*s),v,sizeof(*v)) || overlap(s,sizeof(*s),h,sizeof(*h))) return OCJ_ARGUMENT;
    memset(s,0,sizeof(*s));
    if(equal(p,"OPNDCT1",7)) return OCJ_CONFLICT;
    if(!equal(p,magic,8) || get32(p+8)!=2 || get32(p+12)!=256 ||
       get32(p+252)!=crc(p) || !all(p+OCJ_PADDING_OFFSET,4096-OCJ_PADDING_OFFSET,0xff)) return 1;
    uint8_t digest[32]; if(sha(h,p,digest)) return OCJ_FAULT;
    if(!equal(digest,p+OCJ_DIGEST_OFFSET,32)) { wipe(digest,32); return 1; }
    if(!equal(p+16,v->digest,32) || get32(p+100)!=bank || get32(p+104)!=slot ||
       get32(p+108)!=v->spec.control_blocks[bank]*64U+slot || get32(p+112)!=1U-bank ||
       get32(p+160)!=4096 || get32(p+164)!=OCJ_BODY_BYTES ||
       !all(p+148,4,0) || !all(p+168,84,0)) { wipe(digest,32); return OCJ_CONFLICT; }
    s->epoch=get64(p+48); s->previous_epoch=get64(p+56); memcpy(s->previous_digest,p+64,32);
    s->state=get32(p+96); s->bank=bank; s->slot=slot; s->retired=get32(p+116);
    s->quarantine=get32(p+120); s->open=get32(p+124); s->high_water=get64(p+128);
    s->pending_kind=get32(p+136); s->pending_slot=get32(p+140); s->pending_lease=get64(p+152);
    for(unsigned i=0;i<32;i++) { const uint8_t *b=p+256+i*32;
        s->blocks[i].base=get64(b); s->blocks[i].valid=get64(b+8);
        s->blocks[i].range_sequence=get64(b+16); s->blocks[i].consumed=get32(b+24);
        if(!all(b+28,4,0)) { wipe(s,sizeof(*s)); wipe(digest,32); return OCJ_CONFLICT; } }
    int ok=syntax(s) && get32(p+144)==(s->pending_kind?v->spec.map_blocks[s->pending_slot]*64U:UINT32_MAX);
    if(ok) memcpy(s->digest,digest,32); else wipe(s,sizeof(*s)); wipe(digest,32);
    return ok?0:OCJ_CONFLICT;
}
static int fault(struct owned_control_journal *j,int code)
{ j->fault=1; j->reconciled=0; return code<0?code:OCJ_FAULT; }
static int check(struct owned_control_journal *j)
{ uint64_t n=j->io.now_ms(j->io.user); if(j->fault || n<j->last_now || n>=j->deadline) return fault(j,OCJ_FAULT);
  j->last_now=n; return 0; }
static int begin(struct owned_control_journal *j,uint64_t deadline)
{
    if(!j || j->initialized!=OCJ_MAGIC) return OCJ_ARGUMENT;
    if(j->busy) return OCJ_BUSY;
    if(j->fault) return OCJ_FAULT;
    uint64_t now=j->io.now_ms(j->io.user);
    if(now<j->last_now || now>=deadline || deadline-now>120000U) return fault(j,OCJ_FAULT);
    j->deadline=deadline; j->last_now=now; j->busy=1; return 0;
}
static int finish(struct owned_control_journal *j,int rc)
{ if(!j->retained) { wipe(j->tx,sizeof(j->tx)); wipe(j->rx,sizeof(j->rx)); wipe(j->raw,sizeof(j->raw)); }
  j->busy=0; return j->fault?(rc<0?rc:OCJ_FAULT):rc; }
static int admitted(struct owned_control_journal *j,uint32_t reason,uint32_t bank)
{
    if(check(j)) return OCJ_FAULT;
    if(j->io.fault_latched(j->io.user)!=0) return fault(j,OCJ_FAULT);
    if(j->io.admit(j->io.user,reason,j->volume.digest,bank<2?j->volume.spec.control_blocks[bank]:UINT32_MAX,
       j->current.epoch,j->deadline)) return OCJ_REFUSED;
    return check(j);
}
static int io_result(struct owned_control_journal *j,int rc,uint32_t bank,int reading)
{
    if(rc==OCJ_IO_UNCERTAIN) j->retained=1;
    if(rc==OCJ_IO_MEDIA) { /* Never erase-to-repair a reported media failure. */
        j->io.latch_fault(j->io.user,bank,j->deadline); return fault(j,OCJ_FAULT); }
    if(rc!=OCJ_IO_OK && !(reading && rc==OCJ_IO_CHECKED_RAW)) return fault(j,OCJ_FAULT);
    return check(j);
}
static int read_control(struct owned_control_journal *j,uint32_t bank,uint32_t slot)
{ if(check(j)) return OCJ_FAULT;
  return io_result(j,j->io.read(j->io.user,j->volume.spec.control_blocks[bank]*64U+slot,j->rx,j->deadline),bank,1); }
static int transition(const struct ocj_snapshot *a,const struct ocj_snapshot *b)
{
    if(b->epoch!=a->epoch+1 || b->previous_epoch!=a->epoch || !equal(b->previous_digest,a->digest,32) ||
       (b->bank==a->bank?b->slot!=a->slot+1:b->slot!=0) ||
       (a->retired&~b->retired) || b->high_water<a->high_water ||
       (a->state==OWNED_CONTROL_ACTIVE && b->state==OWNED_CONTROL_PROVISIONING) ||
       (a->state==OWNED_CONTROL_READ_ONLY && b->state!=OWNED_CONTROL_READ_ONLY)) return 0;
    unsigned changed=0;
    if(a->pending_kind && b->pending_kind) return 0;
    for(unsigned i=0;i<32;i++) {
        const struct ocj_block *x=&a->blocks[i],*y=&b->blocks[i];
        if(y->base!=x->base) {
            if(b->pending_kind!=OWNED_CONTROL_ERASE_BLOCK || b->pending_slot!=i ||
               a->high_water>UINT64_MAX-65U || y->base!=a->high_water+1U || y->valid ||
               y->range_sequence!=b->epoch || b->high_water!=y->base+64U) return 0;
            ++changed;
        } else if((x->valid&~y->valid) || y->range_sequence!=x->range_sequence ||
                  (y->consumed<x->consumed && !(a->pending_kind && a->pending_slot==i && !b->pending_kind))) return 0;
        if(((a->quarantine&~b->quarantine)|(b->open&~a->open))&(1U<<i)) {
            if(!a->pending_kind || a->pending_slot!=i || b->pending_kind ||
               y->consumed || y->valid || !(b->open&(1U<<i)) || (b->quarantine&(1U<<i))) return 0;
        }
    }
    if(changed>1 || (!changed && b->high_water!=a->high_water) || (b->pending_kind && !changed)) return 0;
    return 1;
}
int ocj_init(struct owned_control_journal *j,const uint8_t descriptor[512],
    const struct owned_volume_spec *spec,const struct owned_page_hash *hash,const struct ocj_io *io)
{
    if(!j || j->initialized || !descriptor || !spec || !hash || !io ||
       overlap(j,sizeof(*j),descriptor,512) || overlap(j,sizeof(*j),spec,sizeof(*spec)) ||
       overlap(j,sizeof(*j),hash,sizeof(*hash)) || overlap(j,sizeof(*j),io,sizeof(*io)) ||
       !io->now_ms || !io->admit || !io->read || !io->raw ||
       !io->erase || !io->program || !io->fault_latched || !io->latch_fault) return OCJ_ARGUMENT;
    struct owned_volume_decoded v;
    if(owned_volume_descriptor_validate(descriptor,512,spec,hash,&v)) return OCJ_ARGUMENT;
    memset(j,0,sizeof(*j)); j->volume=v; j->hash=*hash; j->io=*io; j->initialized=OCJ_MAGIC; return 0;
}
int ocj_boot(struct owned_control_journal *j,uint64_t deadline)
{
    int rc=begin(j,deadline); if(rc) return rc;
    if(j->booted) return finish(j,OCJ_REFUSED);
    rc=admitted(j,OCJ_ADMIT_BOOT,2); if(rc) return finish(j,rc);
    memset(j->first,0,sizeof(j->first)); memset(j->last,0,sizeof(j->last));
    unsigned broken[2]={0,0}; uint64_t highest[2]={0,0};
    for(unsigned bank=0;bank<2;bank++) {
        int gap=0;
        for(unsigned slot=0;slot<64;slot++) {
            if(read_control(j,bank,slot)) return finish(j,OCJ_FAULT);
            rc=ocj_decode(&j->volume,bank,slot,&j->hash,j->rx,&j->candidate);
            if(rc<0) return finish(j,fault(j,rc));
            if(rc==1) { gap=1; continue; }
            if(j->candidate.epoch>highest[bank]) highest[bank]=j->candidate.epoch;
            if(gap || (j->last[bank].epoch && !transition(&j->last[bank],&j->candidate))) broken[bank]=1;
            if(!j->first[bank].epoch) j->first[bank]=j->candidate;
            j->last[bank]=j->candidate;
        }
    }
    unsigned selected=j->last[1].epoch>j->last[0].epoch?1U:0U;
    if(!j->last[selected].epoch) return finish(j,OCJ_NO_ROOT);
    unsigned other=1U-selected;
    /* A partial erase may leave holes/older fragments in the disposable bank.
     * The selected authoritative bank itself must retain its complete prefix.
     * Surviving exact predecessor must match; older fragments cannot masquerade
     * as a overlapping successor or cause an older-root fallback. */
    if(broken[selected] || j->first[selected].slot ||
       (highest[other]>=j->first[selected].epoch && highest[other]) ||
       (j->last[other].epoch==j->first[selected].previous_epoch && j->last[other].epoch &&
        !transition(&j->last[other],&j->first[selected])))
        return finish(j,fault(j,OCJ_CONFLICT));
    j->current=j->last[selected]; memcpy(j->working,j->current.blocks,sizeof(j->working));
    j->booted=1; j->tail_fresh=0; j->fresh_map=0; j->reconciled=0;
    return finish(j,0);
}
static int erase_fresh(struct owned_control_journal *j,unsigned bank,uint32_t reason)
{
    int rc=admitted(j,reason,bank); if(rc) return rc;
    uint32_t block=j->volume.spec.control_blocks[bank];
    if(io_result(j,j->io.erase(j->io.user,block,j->deadline),bank,0)) return OCJ_FAULT;
    for(unsigned p=0;p<64;p++) {
        if(check(j) || io_result(j,j->io.raw(j->io.user,block*64U+p,j->raw,j->deadline),bank,0)) return OCJ_FAULT;
        if(!all(j->raw,sizeof(j->raw),0xff)) {
            j->io.latch_fault(j->io.user,bank,j->deadline); return fault(j,OCJ_FAULT); }
    }
    return 0;
}
static int publish(struct owned_control_journal *j,int genesis)
{
    struct ocj_snapshot *s=&j->candidate;
    if(!genesis) { int admission=admitted(j,OCJ_ADMIT_MAP,2); if(admission) return fault(j,admission); }
    if(!genesis) {
        if(j->current.epoch==INT64_MAX) return fault(j,OCJ_REFUSED);
        s->epoch=j->current.epoch+1; s->previous_epoch=j->current.epoch;
        memcpy(s->previous_digest,j->current.digest,32);
        s->bank=j->tail_fresh && j->next_slot<64?j->current.bank:1U-j->current.bank;
        s->slot=s->bank==j->current.bank?j->next_slot:0;
    }
    if(ocj_encode(&j->volume,s,&j->hash,j->tx)) return fault(j,OCJ_CONFLICT);
    if(!genesis && !transition(&j->current,s)) return fault(j,OCJ_CONFLICT);
    if(!genesis && s->bank!=j->current.bank) {
        int rc=erase_fresh(j,s->bank,OCJ_ADMIT_RECYCLE); if(rc) return fault(j,rc);
    }
    if(check(j)) return OCJ_FAULT;
    /* Consume before program; no failed attempt can reuse this boot/tail. */
    j->tail_fresh=0;
    if(io_result(j,j->io.program(j->io.user,j->volume.spec.control_blocks[s->bank]*64U+s->slot,
       j->tx,j->deadline),s->bank,0)) return OCJ_FAULT;
    if(read_control(j,s->bank,s->slot)) return OCJ_FAULT;
    if(!equal(j->tx,j->rx,4096)) {
        j->io.latch_fault(j->io.user,s->bank,j->deadline); return fault(j,OCJ_FAULT); }
    memcpy(s->digest,j->tx+OCJ_DIGEST_OFFSET,32);
    if(check(j)) return OCJ_FAULT;
    j->current=*s; j->tail_fresh=1; j->next_slot=s->slot+1; return 0;
}
int ocj_provision(struct owned_control_journal *j,uint64_t deadline)
{
    int rc=begin(j,deadline); if(rc) return rc;
    if(j->booted || j->current.epoch) return finish(j,OCJ_REFUSED);
    rc=erase_fresh(j,1,OCJ_ADMIT_PROVISION); if(!rc) rc=erase_fresh(j,0,OCJ_ADMIT_PROVISION);
    if(rc) return finish(j,fault(j,rc));
    memset(&j->candidate,0,sizeof(j->candidate)); j->candidate.epoch=1;
    j->candidate.state=OWNED_CONTROL_PROVISIONING; j->candidate.pending_slot=UINT32_MAX;
    for(unsigned b=0;b<32;b++) j->candidate.blocks[b].consumed=64;
    rc=publish(j,1);
    if(!rc) { j->booted=j->reconciled=1; memcpy(j->working,j->current.blocks,sizeof(j->working)); }
    return finish(j,rc);
}
int ocj_reconcile(struct owned_control_journal *j,uint64_t deadline)
{
    int rc=begin(j,deadline); if(rc) return rc;
    if(!j->booted || j->reconciled || j->map_inflight) return finish(j,OCJ_REFUSED);
    j->candidate=j->current; j->candidate.quarantine|=j->candidate.open;
    if(j->candidate.pending_kind) j->candidate.quarantine|=1U<<j->candidate.pending_slot;
    j->candidate.open=0; j->candidate.pending_kind=0;
    j->candidate.pending_slot=UINT32_MAX; j->candidate.pending_lease=0;
    rc=publish(j,0); if(!rc) { j->reconciled=1; memcpy(j->working,j->current.blocks,sizeof(j->working)); }
    return finish(j,rc);
}
int ocj_activate(struct owned_control_journal *j,uint64_t deadline)
{
    int rc=begin(j,deadline); if(rc) return rc;
    if(!j->booted || !j->reconciled || j->map_inflight || j->current.pending_kind ||
       j->current.state!=OWNED_CONTROL_PROVISIONING) return finish(j,OCJ_REFUSED);
    rc=admitted(j,OCJ_ADMIT_ACTIVE,2); if(rc) return finish(j,rc);
    j->candidate=j->current; j->candidate.state=OWNED_CONTROL_ACTIVE;
    rc=publish(j,0); return finish(j,rc);
}
int ocj_capability(struct owned_control_journal *j,struct owned_dhara_capability *c)
{
    if(!j || !c || j->initialized!=OCJ_MAGIC || overlap(c,sizeof(*c),j,sizeof(*j))) return OCJ_ARGUMENT;
    if(j->busy) return OCJ_BUSY;
    if(j->fault || !j->booted || !j->reconciled || j->map_inflight || j->current.pending_kind ||
       j->current.state==OWNED_CONTROL_READ_ONLY) return OCJ_REFUSED;
    memset(c,0,sizeof(*c)); memcpy(c->descriptor_digest,j->volume.digest,32);
    memcpy(c->control_digest,j->current.digest,32); c->sequence=j->current.epoch;
    c->lease_high_water=j->current.high_water; c->state=j->current.state; c->retired_map=j->current.retired; return 0;
}
int ocj_validate_capability(void *u,const struct owned_dhara_capability *c,uint64_t deadline)
{
    struct owned_control_journal *j=u; struct owned_dhara_capability actual;
    int rc=ocj_capability(j,&actual); if(rc || !c || !equal(c,&actual,sizeof(actual))) return OCJ_REFUSED;
    rc=begin(j,deadline); if(rc) return rc;
    rc=admitted(j,c->state==OWNED_CONTROL_ACTIVE?OCJ_ADMIT_ACTIVE:OCJ_ADMIT_MAP,2);
    return finish(j,rc);
}
static int mapped(struct owned_control_journal *j,uint32_t row,uint32_t *slot,uint32_t *page)
{ for(unsigned b=0;b<32;b++) if(j->volume.spec.map_blocks[b]==row/64U) { *slot=b; *page=row%64U; return 0; }
  return fault(j,OCJ_ARGUMENT); }
int ocj_page_info(void *u,uint32_t row,struct owned_dhara_page_info *i,uint64_t deadline)
{
    struct owned_control_journal *j=u; int rc=begin(j,deadline); if(rc) return rc;
    uint32_t b,p; if(!i || overlap(i,sizeof(*i),j,sizeof(*j)) || !j->booted || mapped(j,row,&b,&p)) return finish(j,OCJ_ARGUMENT);
    const struct ocj_block *block=&j->working[b]; memset(i,0,sizeof(*i));
    if(block->valid&(UINT64_C(1)<<p)) { i->state=OD_PAGE_COMMITTED; i->lease_id=block->base+1U+p; }
    else if((j->fresh_map&(1U<<b)) && !(j->current.retired&(1U<<b)) && p>=block->consumed)
        i->state=OD_PAGE_UNUSED;
    else i->state=OD_PAGE_ABANDONED;
    return finish(j,check(j));
}
static void ticket(struct owned_control_journal *j,uint32_t kind,uint32_t b,uint32_t row,
                   struct owned_dhara_lease *l)
{ memset(l,0,sizeof(*l)); memcpy(l->descriptor_digest,j->volume.digest,32);
  l->sequence=j->current.epoch; l->range_base=j->working[b].base;
  l->lease_id=l->range_base+(kind==OWNED_CONTROL_PROGRAM_PAGE?1U+row%64U:0);
  l->kind=kind; l->map_slot=b; l->physical_row=row; }
int ocj_acquire(void *u,uint32_t kind,uint32_t b,uint32_t row,struct owned_dhara_lease *l,uint64_t deadline)
{
    struct owned_control_journal *j=u; int rc=begin(j,deadline); if(rc) return rc;
    if(!l || overlap(l,sizeof(*l),j,sizeof(*j)) || b>=32 || row!=j->volume.spec.map_blocks[b]*64U+row%64U ||
       !j->booted || !j->reconciled || j->map_inflight || j->current.pending_kind ||
       j->current.state==OWNED_CONTROL_READ_ONLY || (j->current.retired&(1U<<b)))
        return finish(j,OCJ_REFUSED);
    rc=admitted(j,OCJ_ADMIT_MAP,2); if(rc) return finish(j,rc);
    if(kind==OWNED_CONTROL_ERASE_BLOCK) {
        if(row%64U || j->current.high_water>UINT64_MAX-65U) return finish(j,fault(j,OCJ_REFUSED));
        j->candidate=j->current; struct ocj_block *x=&j->candidate.blocks[b];
        memset(x,0,sizeof(*x)); x->base=j->current.high_water+1; x->range_sequence=j->current.epoch+1; x->consumed=64;
        j->candidate.high_water=x->base+64; j->candidate.pending_kind=kind;
        j->candidate.pending_slot=b; j->candidate.pending_lease=x->base;
        j->candidate.quarantine|=1U<<b; j->candidate.open&=~(1U<<b);
        rc=publish(j,0); if(rc) return finish(j,rc);
        j->working[b]=j->current.blocks[b]; j->fresh_map&=~(1U<<b);
    } else if(kind==OWNED_CONTROL_PROGRAM_PAGE) {
        if(!(j->fresh_map&(1U<<b)) || row%64U<j->working[b].consumed) return finish(j,fault(j,OCJ_REFUSED));
        j->working[b].consumed=row%64U+1U;
    } else return finish(j,OCJ_ARGUMENT);
    ticket(j,kind,b,row,&j->inflight); *l=j->inflight; j->map_inflight=1;
    return finish(j,check(j));
}
int ocj_resolve(void *u,const struct owned_dhara_lease *l,uint64_t deadline)
{
    struct owned_control_journal *j=u; int rc=begin(j,deadline); if(rc) return rc;
    if(!l || !j->map_inflight || !equal(l,&j->inflight,sizeof(*l))) return finish(j,fault(j,OCJ_REFUSED));
    uint32_t b=l->map_slot;
    if(l->kind==OWNED_CONTROL_ERASE_BLOCK) {
        j->candidate=j->current; j->candidate.pending_kind=0; j->candidate.pending_slot=UINT32_MAX;
        j->candidate.pending_lease=0; j->candidate.blocks[b].consumed=0;
        j->candidate.quarantine&=~(1U<<b); j->candidate.open|=1U<<b;
        rc=publish(j,0); if(rc) return finish(j,rc);
        j->working[b]=j->current.blocks[b]; j->fresh_map|=1U<<b;
    } else j->working[b].valid|=UINT64_C(1)<<(l->physical_row%64U);
    j->map_inflight=0; memset(&j->inflight,0,sizeof(j->inflight)); return finish(j,check(j));
}
int ocj_history_sync(void *u,uint64_t deadline)
{
    struct owned_control_journal *j=u; int rc=begin(j,deadline); if(rc) return rc;
    if(!j->booted || !j->reconciled || j->map_inflight || j->current.pending_kind ||
       j->current.state==OWNED_CONTROL_READ_ONLY) return finish(j,OCJ_REFUSED);
    j->candidate=j->current; memcpy(j->candidate.blocks,j->working,sizeof(j->working));
    for(unsigned b=0;b<32;b++) if(j->working[b].consumed==64) j->candidate.open&=~(1U<<b);
    rc=publish(j,0); return finish(j,rc);
}
int ocj_retire(void *u,uint32_t b,uint32_t block,uint32_t bitmap,const struct owned_dhara_lease *l,uint64_t deadline)
{
    struct owned_control_journal *j=u; int rc=begin(j,deadline); if(rc) return rc;
    if(b>=32 || block!=j->volume.spec.map_blocks[b] || bitmap!=(j->current.retired|(1U<<b)) ||
       !l || !j->map_inflight || !equal(l,&j->inflight,sizeof(*l))) return finish(j,fault(j,OCJ_REFUSED));
    j->candidate=j->current; j->candidate.retired=bitmap; j->candidate.quarantine|=1U<<b;
    j->candidate.open&=~(1U<<b); j->candidate.pending_kind=0;
    j->candidate.pending_slot=UINT32_MAX; j->candidate.pending_lease=0;
    /* Retain known surviving pages, never classify the failed page as valid. */
    j->candidate.blocks[b].consumed=j->working[b].consumed;
    rc=publish(j,0); if(!rc) { j->fresh_map&=~(1U<<b); j->map_inflight=0; memset(&j->inflight,0,sizeof(j->inflight)); }
    return finish(j,rc);
}
