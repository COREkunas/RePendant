/* SPDX-License-Identifier: Apache-2.0 */
#include "recording_native_store.h"
#include <string.h>
#define RNS_MAGIC 0x524e5331U
static struct recording_native_store *owner;
static const uint8_t tag[8]={'O','P','N','D','N','S','1',0};
static const uint8_t extent_tag[8]={'O','P','N','D','N','S','2',0};
static void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(v>>(i*8));}
static uint32_t get32(const uint8_t *p){uint32_t v=0;for(unsigned i=0;i<4;++i)v|=(uint32_t)p[i]<<(i*8);return v;}
static int external(const struct recording_native_store *s,const void *p,size_t n)
{uintptr_t a=(uintptr_t)s,b=(uintptr_t)p;return p&&n<=UINTPTR_MAX-b&&sizeof(*s)<=UINTPTR_MAX-a&&!(a<b+n&&b<a+sizeof(*s));}
static void cache_clear(struct recording_native_store *s)
{memset(s->meta_cache,0,sizeof(s->meta_cache));s->meta_next=0;}
static void cache_invalidate(struct recording_native_store *s,uint32_t first,uint32_t count)
{for(unsigned i=0;i<RNS_META_CACHE_ENTRIES;++i)
 if(s->meta_cache[i].valid&&s->meta_cache[i].page>=first&&s->meta_cache[i].page-first<count)
  memset(&s->meta_cache[i],0,sizeof(s->meta_cache[i]));}
static int failure(struct recording_native_store *s,dhara_error_t *e,uint32_t line)
{if(!s->fault)s->fault_line=line;s->fault=1;s->state=RNS_FENCED;cache_clear(s);dhara_set_error(e,DHARA_E_ECC);return RNS_FAULT;}
#define fault(s,e) failure(s,e,__LINE__)
static int time_ok(struct recording_native_store *s)
{uint64_t n=s->target.now_ms(s->target.user);if(s->fault||n<s->last_now||n>=s->deadline)return fault(s,NULL);s->last_now=n;return 0;}
static int writing(const struct recording_native_store *s)
{return s->state==RNS_FORMATTING||s->state==RNS_WRITING;}
static int authorize(void *u,uint32_t access,uint32_t row,uint64_t d)
{
 struct recording_native_store *s=u;
 if(!atomic_load(&s->gate)||d!=s->deadline||time_ok(s)||access<NOP_READ||access>NOP_ERASE||
 row<s->first_block*64U||row>=(s->first_block+s->block_count)*64U||
 (access!=NOP_READ&&!writing(s))||s->hooks.admit(s->hooks.user,access,row,d)||
 s->target.authorize(s->target.user,access,row,d)||time_ok(s))return -1;
 return 0;
}
static uint64_t now(void *u){struct recording_native_store *s=u;return s->target.now_ms(s->target.user);}
static void wait_us(void *u,uint32_t us){struct recording_native_store *s=u;s->target.wait_us(s->target.user,us);}
static int open_bus(void *u,uint64_t d){struct recording_native_store *s=u;return s->target.open(s->target.user,d);}
static int close_bus(void *u,uint64_t d){struct recording_native_store *s=u;return s->target.close(s->target.user,d);}
static int transfer(void *u,const uint8_t *tx,uint8_t *rx,uint32_t n,uint32_t fast,uint64_t d,struct nop_transfer_result *r)
{struct recording_native_store *s=u;return s->target.transfer(s->target.user,tx,rx,n,fast,d,r);}
static int physical(struct recording_native_store *s,int rc,dhara_error_t *e)
{return rc||time_ok(s)?fault(s,e):0;}
static int cooperate(struct recording_native_store *s)
{return time_ok(s)||s->hooks.yield(s->hooks.user,s->deadline)||time_ok(s)?fault(s,NULL):0;}
static int begin(struct recording_native_store *s,uint64_t d)
{
 if(!s||s->magic!=RNS_MAGIC)return RNS_ARGUMENT;
 unsigned expected=0;if(!atomic_compare_exchange_strong(&s->gate,&expected,1))return RNS_BUSY;
 if(s->format_pending&&d>s->format_deadline)d=s->format_deadline;
 /* A format/metadata parent can span minutes, but every synchronous map
  * operation must retain the PHY's finite <=120s contract. Never renew a
  * caller's earlier deadline or relax the PHY admission check. */
 uint64_t n=s->target.now_ms(s->target.user);
 if(d>n&&d-n>120000U)d=n+120000U;
 s->deadline=d;if(time_ok(s)){atomic_store(&s->gate,0);return RNS_FAULT;}return 0;
}
static int done(struct recording_native_store *s,int rc)
{
 /* Caller spans were never DMA targets. A fault retains the embedded PHY
  * buffers and ownership; only CPU-only scratch is scrubbed here. */
 if(!s->fault&&time_ok(s))rc=RNS_FAULT;
 memset(s->page,0,sizeof(s->page));memset(s->scratch,0,sizeof(s->scratch));
 atomic_store(&s->gate,0);return s->fault?RNS_FAULT:rc;
}
int rns_owns(const struct dhara_nand *n){return owner&&n==&owner->nand;}
static int enter(const struct dhara_nand *n,uint32_t page,dhara_error_t *e)
{if(!rns_owns(n))return -1;return !atomic_load(&owner->gate)||page>=owner->block_count*64U||time_ok(owner)?fault(owner,e):0;}
static int bad(const struct recording_native_store *s,uint32_t b)
{return b>=s->block_count||((s->bad_blocks[b/8U]>>(b%8U))&1U);}
int rns_is_bad(const struct dhara_nand *n,dhara_block_t b)
{return !rns_owns(n)||b>=owner->block_count||enter(n,b*64U,NULL)||bad(owner,b);}
void rns_mark_bad(const struct dhara_nand *n,dhara_block_t b)
{(void)b;if(rns_owns(n))fault(owner,NULL); /* No unjournaled retirement. */}
int rns_erase(const struct dhara_nand *n,dhara_block_t b,dhara_error_t *e)
{
 if(!rns_owns(n))return -1;struct recording_native_store *s=owner;
 if(b>=s->block_count||!writing(s)||rns_is_bad(n,b))return fault(s,e);
 cache_invalidate(s,b*64U,64U); /* BEFORE any possibly uncertain mutation. */
 if(physical(s,nand_owned_phy_erase_checked(&s->phy,s->first_block+b,s->deadline),e))return -1;
 ++s->erases;return cooperate(s);
}
int rns_prog(const struct dhara_nand *n,dhara_page_t p,const uint8_t *data,dhara_error_t *e)
{
 if(!rns_owns(n))return -1;struct recording_native_store *s=owner;
 if(enter(n,p,e)||!writing(s)||!data||rns_is_bad(n,p/64U))return fault(s,e);
 cache_invalidate(s,p,1);
 if(physical(s,nand_owned_phy_program_checked(&s->phy,s->first_block*64U+p,data,s->deadline),e))return -1;
 ++s->programs;return cooperate(s);
}
int rns_is_free(const struct dhara_nand *n,dhara_page_t p)
{
 if(!rns_owns(n))return 0;struct recording_native_store *s=owner;
 if(enter(n,p,NULL)||rns_is_bad(n,p/64U)||physical(s,nand_owned_phy_raw(&s->phy,s->first_block*64U+p,s->scratch,s->deadline),NULL))return 0;
 for(unsigned i=0;i<sizeof(s->scratch);++i)if(s->scratch[i]!=255)return 0;
 return 1;
}
int rns_nand_read(const struct dhara_nand *n,dhara_page_t p,size_t off,size_t len,uint8_t *out,dhara_error_t *e)
{
 if(!rns_owns(n))return -1;struct recording_native_store *s=owner;uint32_t ecc=0;
 if(enter(n,p,e)||!out||off>4096||len>4096-off)return fault(s,e);
 if(!len)return cooperate(s);
 const size_t base=DHARA_HEADER_SIZE+DHARA_COOKIE_SIZE;
 int meta=len==DHARA_META_SIZE&&off>=base&&(off-base)%DHARA_META_SIZE==0&&
  (s->state==RNS_READING||s->state==RNS_WRITING);
 if(meta){
  for(unsigned i=0;i<RNS_META_CACHE_ENTRIES;++i)if(s->meta_cache[i].valid&&
      s->meta_cache[i].page==p&&s->meta_cache[i].offset==off){
   /* A cache hit skips SPI, not current ownership/power/deadline admission.
    * Full page reads below ALWAYS reach the PHY and its ECC check. */
   if(authorize(s,NOP_READ,s->first_block*64U+p,s->deadline))return fault(s,e);
   memmove(out,s->meta_cache[i].bytes,len);++s->meta_hits;return cooperate(s);
  }
  ++s->meta_misses;
 }
 if(physical(s,nand_owned_phy_read_range(&s->phy,s->first_block*64U+p,(uint32_t)off,(uint32_t)len,s->scratch,&ecc,s->deadline),e))return -1;
 if(ecc!=0&&ecc!=1&&ecc!=3&&ecc!=5)return fault(s,e);
 if(meta){unsigned i=s->meta_next;s->meta_next=(i+1U)%RNS_META_CACHE_ENTRIES;
  s->meta_cache[i].valid=0;s->meta_cache[i].page=p;s->meta_cache[i].offset=(uint32_t)off;
  memcpy(s->meta_cache[i].bytes,s->scratch,len);s->meta_cache[i].valid=1;}
 memmove(out,s->scratch,len);++s->reads;return cooperate(s);
}
int rns_copy(const struct dhara_nand *n,dhara_page_t src,dhara_page_t dst,dhara_error_t *e)
{
 if(!rns_owns(n))return -1;
 if(rns_nand_read(n,src,0,4096,owner->scratch,e))return -1;
 return rns_prog(n,dst,owner->scratch,e);
}
static int digest(struct recording_native_store *s,const uint8_t *p,uint8_t out[32])
{size_t actual=0;return s->hash.sha256(s->hash.user,p,4064,out,32,&actual)||actual!=32?fault(s,NULL):0;}
static int encode(struct recording_native_store *s,uint32_t sector,const uint8_t *in)
{
 memset(s->page,0,sizeof(s->page));memcpy(s->page,s->capacity==RNS_CAPACITY?tag:extent_tag,8);put32(s->page+8,sector);
 put32(s->page+12,s->capacity);memcpy(s->page+16,s->descriptor_digest,32);
 if(in)memcpy(s->page+64,in,2048);else {put32(s->page+48,s->first_block);put32(s->page+52,s->block_count);put32(s->page+56,12);put32(s->page+60,4);}
 return digest(s,s->page,s->page+4064);
}
static int decode(struct recording_native_store *s,uint32_t sector)
{
 uint8_t sum[32];
 if(memcmp(s->page,s->capacity==RNS_CAPACITY?tag:extent_tag,8)||get32(s->page+8)!=sector||get32(s->page+12)!=s->capacity||
 memcmp(s->page+16,s->descriptor_digest,32)||digest(s,s->page,sum)||memcmp(sum,s->page+4064,32))return fault(s,NULL);
 if(sector==s->capacity){if(get32(s->page+48)!=s->first_block||get32(s->page+52)!=s->block_count||get32(s->page+56)!=12||get32(s->page+60)!=4)return fault(s,NULL);}
 else for(unsigned i=48;i<64;++i)if(s->page[i])return fault(s,NULL);
 for(unsigned i=sector==s->capacity?64U:2112U;i<4064;++i)if(s->page[i])return fault(s,NULL);
 return 0;
}
int rns_init(struct recording_native_store *s,const struct owned_volume_decoded *v,const struct nop_port *p,const struct owned_page_hash *h,const struct rns_hooks *hooks)
{
 if(!s||s->magic||owner||!external(s,v,sizeof(*v))||!external(s,p,sizeof(*p))||!external(s,h,sizeof(*h))||!external(s,hooks,sizeof(*hooks))||
 !p->now_ms||!p->wait_us||!p->authorize||!p->open||!p->transfer||!p->close||!h->sha256||!hooks->admit||!hooks->yield)return RNS_ARGUMENT;
 for(unsigned i=0;i<RNS_BLOCKS;++i)if(v->spec.map_blocks[i]!=RNS_FIRST_BLOCK+i)return RNS_ARGUMENT;
 s->magic=RNS_MAGIC;s->target=*p;s->hash=*h;s->hooks=*hooks;memcpy(s->descriptor_digest,v->digest,32);
 s->first_block=RNS_FIRST_BLOCK;s->block_count=RNS_BLOCKS;s->capacity=RNS_CAPACITY;
 s->nand=(struct dhara_nand){12,6,RNS_BLOCKS};owner=s;
 struct nop_port port={s,now,wait_us,authorize,open_bus,transfer,close_bus};
 if(nand_owned_phy_init(&s->phy,v,&port))return fault(s,NULL);
 dhara_map_init(&s->map,&s->nand,s->metadata,4);return 0;
}
static int init_extent(struct recording_native_store *s,const uint8_t bytes[REX_DESCRIPTOR_BYTES],
 const struct recording_extent_identity *id,const struct nop_port *p,const struct owned_page_hash *h,const struct rns_hooks *hooks,int full)
{
 if(!s||s->magic||owner||!external(s,bytes,REX_DESCRIPTOR_BYTES)||!external(s,id,sizeof(*id))||
 !external(s,p,sizeof(*p))||!external(s,h,sizeof(*h))||!external(s,hooks,sizeof(*hooks))||
 !p->now_ms||!p->wait_us||!p->authorize||!p->open||!p->transfer||!p->close||!h->sha256||!hooks->admit||!hooks->yield||
 (full?rex_validate_full(bytes,id,h):rex_validate(bytes,id,h)))return RNS_ARGUMENT;
 s->magic=RNS_MAGIC;s->target=*p;s->hash=*h;s->hooks=*hooks;
 memcpy(s->descriptor_digest,bytes+REX_DIGEST_OFFSET,32);
 s->first_block=REX_FIRST_BLOCK;s->block_count=full?RLL_BLOCKS:REX_BLOCKS;s->capacity=full?RLL_CAPACITY:REX_CAPACITY;
 s->nand=(struct dhara_nand){12,6,s->block_count};owner=s;
 struct nop_port port={s,now,wait_us,authorize,open_bus,transfer,close_bus};
 if(full?nand_owned_phy_init_full_recording(&s->phy,&port):nand_owned_phy_init_recording_extent(&s->phy,&port))return fault(s,NULL);
 dhara_map_init(&s->map,&s->nand,s->metadata,4);return 0;
}
int rns_init_extent(struct recording_native_store *s,const uint8_t bytes[REX_DESCRIPTOR_BYTES],
 const struct recording_extent_identity *id,const struct nop_port *p,const struct owned_page_hash *h,const struct rns_hooks *hooks)
{return init_extent(s,bytes,id,p,h,hooks,0);}
int rns_init_full(struct recording_native_store *s,const uint8_t bytes[REX_DESCRIPTOR_BYTES],
 const struct recording_extent_identity *id,const struct nop_port *p,const struct owned_page_hash *h,const struct rns_hooks *hooks)
{return init_extent(s,bytes,id,p,h,hooks,1);}
static int capacity_ok(struct recording_native_store *s)
{
 /* Check with the observed marker count, not Dhara's initial 1/64 guess.
  * A copy avoids altering upstream resume/checkpoint bad-block accounting. */
 if(s->map.journal.bb_last>=s->block_count||s->map.journal.bb_current>=s->block_count)return 0;
 struct dhara_map check=s->map;
 if(check.journal.bb_last<s->bad_count)check.journal.bb_last=s->bad_count;
 return dhara_map_capacity(&check)>s->capacity;
}
static int open_scanned(struct recording_native_store *s)
{
 cache_clear(s);
 s->attempted=1;s->state=RNS_OPENING;
 if(physical(s,nand_owned_phy_open(&s->phy,s->deadline),NULL))return RNS_FAULT;
 for(unsigned b=0;b<s->block_count;++b)for(unsigned p=0;p<2;++p){
  if(physical(s,nand_owned_phy_raw(&s->phy,(s->first_block+b)*64U+p,s->scratch,s->deadline),NULL))return RNS_FAULT;
  if(s->scratch[4096]!=255||s->scratch[4097]!=255){
   if(!bad(s,b))++s->bad_count;
   s->bad_blocks[b/8U]|=(uint8_t)(1U<<(b%8U));
   if(b<32U)s->bad_mask|=1U<<b; /* legacy diagnostic mirror only */
  }
  if(cooperate(s))return RNS_FAULT;
 }
 return (s->block_count==RNS_BLOCKS?s->bad_count>8U:s->bad_count>REX_MAX_BAD_BLOCKS)||!capacity_ok(s)?fault(s,NULL):0;
}
static int publish_root(struct recording_native_store *s)
{
 dhara_error_t e=DHARA_E_NONE;
 if(encode(s,s->capacity,NULL)||dhara_map_write(&s->map,s->capacity,s->page,&e)||dhara_map_sync(&s->map,&e)||s->fault||
 dhara_map_read(&s->map,s->capacity,s->page,&e)||decode(s,s->capacity))return fault(s,&e);
 s->state=RNS_WRITING;s->format_pending=0;return 0;
}
int rns_format(struct recording_native_store *s,uint64_t d)
{
 int rc=begin(s,d);if(rc)return rc;
 if(s->state!=RNS_NEW||s->attempted||s->block_count!=RNS_BLOCKS)return done(s,RNS_REFUSED);
 if(open_scanned(s))return done(s,RNS_FAULT);
 s->state=RNS_FORMATTING;dhara_error_t e=DHARA_E_NONE;
 for(unsigned b=0;b<s->block_count;++b)if(!bad(s,b)&&rns_erase(&s->nand,b,&e))return done(s,fault(s,&e));
 return done(s,publish_root(s));
}
int rns_format_begin(struct recording_native_store *s,uint64_t overall)
{
 if(!s||s->magic!=RNS_MAGIC)return RNS_ARGUMENT;
 uint64_t n=s->target.now_ms(s->target.user);
 if(overall<=n||overall-n>(s->block_count==RLL_BLOCKS?3600000U:900000U))return RNS_ARGUMENT;
 int rc=begin(s,overall-n>120000U?n+120000U:overall);if(rc)return rc;
 if(s->state!=RNS_NEW||s->attempted||(s->block_count!=REX_BLOCKS&&s->block_count!=RLL_BLOCKS))return done(s,RNS_REFUSED);
 s->format_deadline=overall;s->format_pending=1;
 if(open_scanned(s))return done(s,RNS_FAULT);
 s->format_next=0;s->state=RNS_FORMATTING;return done(s,RNS_MORE);
}
int rns_format_step(struct recording_native_store *s,uint64_t d)
{
 if(!s||s->magic!=RNS_MAGIC)return RNS_ARGUMENT;
 uint64_t n=s->target.now_ms(s->target.user);if(d>n&&d-n>120000U)d=n+120000U;
 int rc=begin(s,d);if(rc)return rc;
 if(s->state!=RNS_FORMATTING||!s->format_pending)return done(s,RNS_REFUSED);
 if(s->format_next==s->block_count)return done(s,publish_root(s));
 dhara_error_t e=DHARA_E_NONE;uint32_t b=s->format_next;
 if(!bad(s,b)&&rns_erase(&s->nand,b,&e))return done(s,fault(s,&e));
 ++s->format_next;return done(s,RNS_MORE);
}
int rns_mount(struct recording_native_store *s,uint64_t d)
{
 int rc=begin(s,d);if(rc)return rc;
 if(s->state!=RNS_NEW||s->attempted)return done(s,RNS_REFUSED);
 if(open_scanned(s))return done(s,RNS_FAULT);
 dhara_error_t e=DHARA_E_NONE;s->state=RNS_READING;
 if(dhara_map_resume(&s->map,&e)||!capacity_ok(s)||dhara_map_read(&s->map,s->capacity,s->page,&e)||s->fault||decode(s,s->capacity))return done(s,fault(s,&e));
 return done(s,0);
}
int rns_grant(struct recording_native_store *s,uint64_t d)
{int rc=begin(s,d);if(rc)return rc;if(s->state!=RNS_READING)return done(s,RNS_REFUSED);s->state=RNS_WRITING;return done(s,0);}
int rns_read(struct recording_native_store *s,uint32_t sector,uint8_t out[2048],uint64_t d)
{
 if(!s||sector>=s->capacity||!external(s,out,2048))return RNS_ARGUMENT;
 int rc=begin(s,d);if(rc)return rc;
 if(s->state!=RNS_READING&&s->state!=RNS_WRITING)return done(s,RNS_REFUSED);
 /* map_read deliberately returns success plus FF for an absent sector.
  * Only map_find can prove MISSING; never infer absence from page contents. */
 dhara_error_t e=DHARA_E_NONE;dhara_page_t physical_page=0;
 rc=dhara_map_find(&s->map,sector,&physical_page,&e);
 if(rc){if(e==DHARA_E_NOT_FOUND&&!s->fault)return done(s,RNS_MISSING);return done(s,fault(s,&e));}
 if(rns_nand_read(&s->nand,physical_page,0,4096,s->page,&e))return done(s,fault(s,&e));
 if(s->fault||decode(s,sector))return done(s,RNS_FAULT);
 memcpy(out,s->page+64,2048);return done(s,0);
}
int rns_write(struct recording_native_store *s,uint32_t sector,const uint8_t in[2048],uint64_t d)
{
 if(!s||sector>=s->capacity||!external(s,in,2048))return RNS_ARGUMENT;
 int rc=begin(s,d);if(rc)return rc;
 if(s->state!=RNS_WRITING)return done(s,RNS_REFUSED);
 dhara_error_t e=DHARA_E_NONE;
 if(encode(s,sector,in)||dhara_map_write(&s->map,sector,s->page,&e)||s->fault)return done(s,fault(s,&e));
 return done(s,0); /* durability is established by explicit sync */
}
static int sync_map(struct recording_native_store *s)
{dhara_error_t e=DHARA_E_NONE;return dhara_map_sync(&s->map,&e)||s->fault?fault(s,&e):0;}
int rns_sync(struct recording_native_store *s,uint64_t d)
{int rc=begin(s,d);if(rc)return rc;if(s->state!=RNS_WRITING)return done(s,RNS_REFUSED);return done(s,sync_map(s));}
static int finish(struct recording_native_store *s,uint64_t d,uint32_t state)
{
 int rc=begin(s,d);if(rc)return rc;
 if(s->state!=RNS_READING&&s->state!=RNS_WRITING)return done(s,RNS_REFUSED);
 if((s->state==RNS_WRITING&&sync_map(s))||physical(s,nand_owned_phy_close(&s->phy,d),NULL))return done(s,RNS_FAULT);
 cache_clear(s);
 s->state=state;return done(s,0);
}
int rns_suspend(struct recording_native_store *s,uint64_t d){return finish(s,d,RNS_SUSPENDED);}
int rns_close(struct recording_native_store *s,uint64_t d){return finish(s,d,RNS_CLOSED);}
int rns_reopen(struct recording_native_store *s,uint64_t d)
{
 int rc=begin(s,d);if(rc)return rc;if(s->state!=RNS_SUSPENDED)return done(s,RNS_REFUSED);
 cache_clear(s);
 s->state=RNS_OPENING;if(physical(s,nand_owned_phy_open(&s->phy,d),NULL))return done(s,RNS_FAULT);
 s->state=RNS_READING;return done(s,0);
}
