/* SPDX-License-Identifier: Apache-2.0 */
#include "recording_sync_metadata.h"
#include <string.h>
#define MAGIC UINT32_C(0x52534d31)
static const uint8_t tag[8]={'O','P','N','D','S','M','1',0};
static void wipe(void *p,size_t n){volatile uint8_t *b=p;while(n--)*b++=0;}
static int overlap(const void *a,size_t an,const void *b,size_t bn)
{uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return an>UINTPTR_MAX-x||bn>UINTPTR_MAX-y||(x<y+bn&&y<x+an);}
static int ext(const struct recording_sync_metadata *c,const void *p,size_t n){return p&&!overlap(c,sizeof(*c),p,n);}
static int all(const uint8_t *p,size_t n,uint8_t v){unsigned x=0;while(n--)x|=*p++^v;return !x;}
static int id(const uint8_t *p,size_t n){return !all(p,n,0)&&!all(p,n,255);}
static uint32_t get32(const uint8_t *p){uint32_t x=0;for(unsigned i=0;i<4;i++)x|=(uint32_t)p[i]<<(8*i);return x;}
static uint64_t get64(const uint8_t *p){uint64_t x=0;for(unsigned i=0;i<8;i++)x|=(uint64_t)p[i]<<(8*i);return x;}
static void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
static void put64(uint8_t *p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(v>>(8*i));}
static uint32_t crc(const uint8_t *p){uint32_t x=UINT32_MAX;for(unsigned i=0;i<2016;i++){
 x^=(i>=120&&i<124)?0:p[i];for(unsigned k=0;k<8;k++)x=(x>>1)^((0U-(x&1U))&UINT32_C(0xedb88320));}return ~x;}
static uint32_t sector(const struct recording_sync_metadata *c,unsigned kind)
{return c->volume.capacity-kind-((c->format==2&&kind!=1)?2U:0U);}
static uint32_t tomb_limit(const struct recording_sync_metadata *c){return c->format==2?14U:8U;}
static uint32_t slots(const struct recording_sync_metadata *c){uint32_t n=(c->volume.capacity-11U)/18U;return n>32?32:n;}
static int fault(struct recording_sync_metadata *c){c->fault=1;c->mounted=0;return RSM_FAULT;}
static int check(struct recording_sync_metadata *c){uint64_t n=c->port.now_ms(c->port.user);
 if(c->fault||n<c->last_now||n>=c->deadline)return fault(c);
 c->last_now=n;return 0;}
static int lock(struct recording_sync_metadata *c){if(!c||c->magic!=MAGIC)return RSM_ARGUMENT;
 unsigned expected=0;return atomic_compare_exchange_strong(&c->gate,&expected,1)?0:RSM_BUSY;}
static int done(struct recording_sync_metadata *c,int rc){wipe(c->work,2048);wipe(c->verify,2048);
 if(!c->fault&&check(c))rc=RSM_FAULT;
 atomic_store(&c->gate,0);return c->fault?RSM_FAULT:rc;}
static int begin_limit(struct recording_sync_metadata *c,uint64_t d,uint64_t limit){int rc=lock(c);if(rc)return rc;
 uint64_t now=c->port.now_ms(c->port.user);if(c->fault||now<c->last_now||now>=d){fault(c);atomic_store(&c->gate,0);return RSM_FAULT;}
 c->deadline=d-now>limit?now+limit:d;c->last_now=now;return 0;}
static int begin(struct recording_sync_metadata *c,uint64_t d){return begin_limit(c,d,30000U);}
static int admit(struct recording_sync_metadata *c,int writing){if(check(c)||c->port.admit(c->port.user,writing,c->deadline)||check(c))return fault(c);return 0;}
static int hash(struct recording_sync_metadata *c,const uint8_t *p,uint8_t out[32])
{size_t n=0;return c->hash.sha256(c->hash.user,p,2016,out,32,&n)||n!=32?fault(c):0;}
static int seal(struct recording_sync_metadata *c,uint8_t *p){put32(p+120,crc(p));return hash(c,p,p+2016);}
static void header(struct recording_sync_metadata *c,uint8_t *p,unsigned kind,uint64_t rev,uint32_t count)
{
 memset(p,255,2048);memset(p,0,128);memcpy(p,tag,8);p[8]=(uint8_t)c->format;p[10]=(uint8_t)kind;p[12]=128;
 p[14]=kind==2?56:kind==3?128:0;
 memcpy(p+16,c->volume.device,16);memcpy(p+32,c->volume.volume,16);put64(p+48,c->volume.generation);
 memcpy(p+56,c->volume.recipient,32);put32(p+88,sector(c,kind));put32(p+92,c->volume.capacity);
 put32(p+96,c->volume.capacity-3);put32(p+100,slots(c));put64(p+104,rev);put32(p+112,count);put32(p+116,2);
 if(kind==1)memset(p+1984,0,32);else memcpy(p+1984,c->root_digest,32);
}
static int root(struct recording_sync_metadata *c,uint8_t *p)
{header(c,p,1,1,0);put32(p+128,sector(c,1));put32(p+132,sector(c,2));put32(p+136,sector(c,3));
 put32(p+140,8);put32(p+144,18);put32(p+148,32);put32(p+152,tomb_limit(c));put32(p+156,c->format);
 memcpy(p+160,c->volume.descriptor_digest,32);return seal(c,p);}
static int receipt_valid(const struct rsm_receipt *r){return id(r->recording,16)&&r->sequence<32&&
 r->container_bytes>=425&&r->container_bytes<=34357&&id(r->digest,32);}
static int terminal_valid(const struct rsm_terminal *t){return id(t->recording,16)&&id(t->manifest,32)&&
 t->segments<=32&&(t->state==1||t->state==2)&&t->revision==(uint64_t)t->segments*4+t->state;}
static void receipt_get(const uint8_t *p,struct rsm_receipt *r){memset(r,0,sizeof(*r));memcpy(r->recording,p,16);r->sequence=get32(p+16);r->container_bytes=get32(p+20);memcpy(r->digest,p+24,32);}
static void receipt_put(uint8_t *p,const struct rsm_receipt *r){memcpy(p,r->recording,16);put32(p+16,r->sequence);put32(p+20,r->container_bytes);memcpy(p+24,r->digest,32);}
static int receipt_order(const struct rsm_receipt *a,const struct rsm_receipt *b){int v=memcmp(a->recording,b->recording,16);return v?v:a->sequence<b->sequence?-1:a->sequence>b->sequence?1:0;}
static void tomb_get(const uint8_t *p,struct rsm_tombstone *t){memset(t,0,sizeof(*t));memcpy(t->operation,p,16);memcpy(t->terminal.recording,p+16,16);memcpy(t->terminal.manifest,p+32,32);
 t->terminal.revision=get64(p+64);t->revision=get64(p+72);t->terminal.segments=get32(p+80);t->terminal.state=get32(p+84);}
static void tomb_put(uint8_t *p,const uint8_t op[16],const struct rsm_terminal *t,uint64_t revision){memset(p,0,128);memcpy(p,op,16);memcpy(p+16,t->recording,16);memcpy(p+32,t->manifest,32);
 put64(p+64,t->revision);put64(p+72,revision);put32(p+80,t->segments);put32(p+84,t->state);}
static int validate(struct recording_sync_metadata *c,const uint8_t *p,unsigned kind)
{
 uint8_t digest[32];uint32_t count=get32(p+112);uint64_t rev=get64(p+104);
 if(memcmp(p,tag,8)||p[8]!=c->format||p[9]||p[10]!=kind||p[11]||p[12]!=128||p[13]||p[15]||
 p[14]!=(kind==2?56:kind==3?128:0)||memcmp(p+16,c->volume.device,16)||memcmp(p+32,c->volume.volume,16)||
 get64(p+48)!=c->volume.generation||memcmp(p+56,c->volume.recipient,32)||get32(p+88)!=sector(c,kind)||
 get32(p+92)!=c->volume.capacity||get32(p+96)!=c->volume.capacity-3||get32(p+100)!=slots(c)||!rev||rev>INT64_MAX||
 get32(p+116)!=2||get32(p+124)||get32(p+120)!=crc(p)||hash(c,p,digest)||memcmp(digest,p+2016,32))return fault(c);
 if(kind==1){
  if(count||rev!=1||get32(p+128)!=sector(c,1)||get32(p+132)!=sector(c,2)||get32(p+136)!=sector(c,3)||
   get32(p+140)!=8||get32(p+144)!=18||get32(p+148)!=32||get32(p+152)!=tomb_limit(c)||get32(p+156)!=c->format||
   memcmp(p+160,c->volume.descriptor_digest,32)||!all(p+192,1792,255)||!all(p+1984,32,0)||memcmp(p+2016,c->root_digest,32))return fault(c);
  return 0;
 }
 unsigned width=kind==2?56:128,max=kind==2?32:tomb_limit(c);
 if(count>max||memcmp(p+1984,c->root_digest,32)||!all(p+128+count*width,1984-128-count*width,255))return fault(c);
 uint64_t newest=1;
 if(kind==2&&rev!=(uint64_t)count+1)return fault(c);
 for(unsigned i=0;i<count;i++){
  const uint8_t *e=p+128+i*width;
  if(kind==2){struct rsm_receipt r,prior;receipt_get(e,&r);if(!receipt_valid(&r))return fault(c);
   if(i){receipt_get(e-width,&prior);if(receipt_order(&prior,&r)>=0)return fault(c);}}
  else{struct rsm_tombstone t;tomb_get(e,&t);
   if(!id(t.operation,16)||!terminal_valid(&t.terminal)||t.revision<=t.terminal.revision||t.revision>rev||!all(e+88,40,0)||
     (i&&memcmp(e-width,e,16)>=0))return fault(c);
   if(t.revision>newest)newest=t.revision;
   for(unsigned k=0;k<i;k++)if(!memcmp(p+128+k*128+16,t.terminal.recording,16)||!memcmp(p+128+k*128+32,t.terminal.manifest,32)||get64(p+128+k*128+72)==t.revision)return fault(c);
  }
 }
 return kind==3&&rev!=newest?fault(c):0;
}
static int read_page(struct recording_sync_metadata *c,unsigned kind,uint8_t *p)
{if(admit(c,0))return -1;int rc=c->port.read(c->port.user,sector(c,kind),p,c->deadline);
 if(check(c)||rc<0||rc>1)return fault(c);
 return rc;}
static int publish(struct recording_sync_metadata *c,unsigned kind)
{
 if(seal(c,c->work)||validate(c,c->work,kind)||admit(c,1)||c->port.write(c->port.user,sector(c,kind),c->work,c->deadline)||check(c)||
  admit(c,1)||c->port.sync(c->port.user,c->deadline)||check(c)||read_page(c,kind,c->verify)||
  validate(c,c->verify,kind)||memcmp(c->work,c->verify,2048)||check(c))return fault(c);
 return 0;
}
#include "recording_sync_paged.inc"
int rsm_init(struct recording_sync_metadata *c,const struct rsm_volume *v,const struct owned_page_hash *h,const struct rsm_port *p)
{
 if(!c||c->magic||!ext(c,v,sizeof(*v))||!ext(c,h,sizeof(*h))||!ext(c,p,sizeof(*p))||!h->sha256||
 !p->now_ms||!p->admit||!p->read||!p->write||!p->sync||!p->approve_create||!p->approve_receipt||!p->approve_delete||
 !id(v->device,16)||!id(v->volume,16)||!id(v->recipient,32)||!id(v->descriptor_digest,32)||!v->generation||v->generation>INT64_MAX||
 v->capacity<29||v->capacity>418||!atomic_is_lock_free(&c->gate))return RSM_ARGUMENT;
 c->volume=*v;c->hash=*h;c->port=*p;c->format=1;if(root(c,c->work))return RSM_FAULT;
 memcpy(c->root_digest,c->work+2016,32);wipe(c->work,2048);c->magic=MAGIC;return 0;
}
int rsm_open(struct recording_sync_metadata *c,uint32_t create,uint64_t d)
{
 if(create>1)return RSM_ARGUMENT;
 int rc=begin_limit(c,d,c&&c->extent_profile?300000U:30000U);if(rc)return rc;
 if(c->extent_profile)return done(c,rsmx_open(c,create));
 if(c->attempted)return done(c,RSM_REFUSED);
 c->attempted=1;
 int missing=read_page(c,1,c->work);if(missing<0)return done(c,RSM_FAULT);
 if(!missing&&c->work[8]==2){
  /* Only this exact layout has unused ROS tail sectors 413/414. Never accept
   * arbitrary sector pointers from media or silently change ROS capacity. */
  if(c->volume.capacity!=418)return done(c,fault(c));
  c->format=2;if(root(c,c->verify))return done(c,RSM_FAULT);
  memcpy(c->root_digest,c->verify+2016,32);
 }
 if(!missing&&validate(c,c->work,1))return done(c,RSM_FAULT);
 int a=read_page(c,2,c->receipts),b=read_page(c,3,c->tombstones);
 if(a<0||b<0)return done(c,RSM_FAULT);
 if(missing){
  if(a!=1||b!=1)return done(c,fault(c));
  if(!create)return done(c,RSM_NOT_CREATED);
  if(admit(c,1)||c->port.approve_create(c->port.user,&c->volume,c->deadline)||check(c))return done(c,fault(c));
  header(c,c->work,2,1,0);if(publish(c,2))return done(c,RSM_FAULT);memcpy(c->receipts,c->work,2048);
  header(c,c->work,3,1,0);if(publish(c,3))return done(c,RSM_FAULT);memcpy(c->tombstones,c->work,2048);
  if(root(c,c->work)||publish(c,1))return done(c,RSM_FAULT);
 }else if(a||b||validate(c,c->receipts,2)||validate(c,c->tombstones,3))return done(c,fault(c));
 c->mounted=1;return done(c,0);
}
int rsm_extend(struct recording_sync_metadata *c,uint64_t d)
{
 int rc=begin(c,d);if(rc)return rc;
 if(c->extent_profile||!c->mounted||c->volume.capacity!=418)return done(c,RSM_REFUSED);
 if(c->format==2)return done(c,0);
 if(c->format!=1)return done(c,RSM_REFUSED);
 if(admit(c,1)||validate(c,c->receipts,2)||validate(c,c->tombstones,3))return done(c,fault(c));
 /* ROS uses sectors [0,403]. 413/414 are outside its allocatable slots.
  * v1 companions 415/416 remain untouched until the new root is durable.
  * Each copy is independently synced: map writes may checkpoint themselves. */
 c->format=2;if(root(c,c->work))return done(c,RSM_FAULT);
 memcpy(c->root_digest,c->work+2016,32);
 for(unsigned kind=2;kind<=3;++kind){
  uint8_t *cached=kind==2?c->receipts:c->tombstones;
  memcpy(c->work,cached,2048);c->work[8]=2;put32(c->work+88,sector(c,kind));
  memcpy(c->work+1984,c->root_digest,32);
  if(publish(c,kind))return done(c,RSM_FAULT);
  memcpy(cached,c->work,2048);
 }
 if(root(c,c->work)||publish(c,1))return done(c,RSM_FAULT);
 return done(c,0);
}
static int receipt_find(struct recording_sync_metadata *c,const struct rsm_receipt *r,unsigned *position)
{uint32_t n=get32(c->receipts+112);*position=n;for(unsigned i=0;i<n;i++){struct rsm_receipt old;receipt_get(c->receipts+128+i*56,&old);
 int order=receipt_order(&old,r);if(!order)return !memcmp(&old,r,sizeof(old))?0:RSM_CONFLICT;if(order>0){*position=i;break;}}return RSM_NOT_FOUND;}
int rsm_receive(struct recording_sync_metadata *c,const struct rsm_receipt *r,uint64_t d)
{
 if(!c||!ext(c,r,sizeof(*r))||!receipt_valid(r))return RSM_ARGUMENT;
 int rc=begin(c,d);if(rc)return rc;
 if(c->extent_profile||!c->mounted)return done(c,RSM_REFUSED);
 if(admit(c,0))return done(c,RSM_FAULT);
 unsigned at;rc=receipt_find(c,r,&at);if(rc!=RSM_NOT_FOUND)return done(c,rc);
 uint32_t n=get32(c->receipts+112);uint64_t rev=get64(c->receipts+104);
 if(n==32)return done(c,RSM_FULL);
 if(rev==INT64_MAX)return done(c,fault(c));
 if(admit(c,1)||c->port.approve_receipt(c->port.user,&c->volume,r,c->deadline)||check(c))return done(c,fault(c));
 memcpy(c->work,c->receipts,2048);memmove(c->work+128+(at+1)*56,c->work+128+at*56,(n-at)*56);
 receipt_put(c->work+128+at*56,r);put32(c->work+112,n+1);put64(c->work+104,rev+1);
 if(publish(c,2))return done(c,RSM_FAULT);
 memcpy(c->receipts,c->work,2048);return done(c,0);
}
static int replay(struct recording_sync_metadata *c,const uint8_t op[16],const uint8_t digest[32],struct rsm_tombstone *out)
{uint32_t n=get32(c->tombstones+112);for(unsigned i=0;i<n;i++){struct rsm_tombstone t;tomb_get(c->tombstones+128+i*128,&t);
 if(!memcmp(t.operation,op,16)){if(memcmp(t.terminal.manifest,digest,32))return RSM_CONFLICT;*out=t;return 0;}}return RSM_NOT_FOUND;}
int rsm_delete(struct recording_sync_metadata *c,const uint8_t op[16],const struct rsm_terminal *t,uint64_t *revision,uint64_t d)
{
 if(!c||!ext(c,op,16)||!ext(c,t,sizeof(*t))||!ext(c,revision,sizeof(*revision))||overlap(revision,8,op,16)||overlap(revision,8,t,sizeof(*t))||
 !id(op,16)||!(c->extent_profile?rsmx_terminal_valid(t):terminal_valid(t)))return RSM_ARGUMENT;
 int rc=begin(c,d);if(rc)return rc;
 if(c->extent_profile){uint64_t value=0;rc=rsmx_delete(c,op,t,&value);
  rc=done(c,rc);if(!rc)*revision=value;return rc;}
 if(!c->mounted)return done(c,RSM_REFUSED);
 if(admit(c,0))return done(c,RSM_FAULT);
 struct rsm_tombstone old;rc=replay(c,op,t->manifest,&old);
 if(!rc){if(memcmp(&old.terminal,t,sizeof(*t)))return done(c,RSM_CONFLICT);*revision=old.revision;return done(c,0);}
 if(rc!=RSM_NOT_FOUND)return done(c,rc);
 uint32_t n=get32(c->tombstones+112),at=n;uint64_t rev=get64(c->tombstones+104);
 for(unsigned i=0;i<n;i++){const uint8_t *e=c->tombstones+128+i*128;
  if(!memcmp(e+16,t->recording,16)||!memcmp(e+32,t->manifest,32))return done(c,RSM_CONFLICT);
  if(at==n&&memcmp(e,op,16)>0)at=i;}
 if(n==tomb_limit(c))return done(c,RSM_FULL);
 if(rev==INT64_MAX)return done(c,fault(c));
 ++rev;if(rev<=t->revision)rev=t->revision+1;
 if(admit(c,1)||c->port.approve_delete(c->port.user,&c->volume,op,t,c->deadline)||check(c))return done(c,fault(c));
 memcpy(c->work,c->tombstones,2048);memmove(c->work+128+(at+1)*128,c->work+128+at*128,(n-at)*128);
 tomb_put(c->work+128+at*128,op,t,rev);put32(c->work+112,n+1);put64(c->work+104,rev);
 if(publish(c,3))return done(c,RSM_FAULT);
 memcpy(c->tombstones,c->work,2048);*revision=rev;return done(c,0);
}
static int query(struct recording_sync_metadata *c){int rc=lock(c);if(rc)return rc;
 if(c->fault||!c->mounted){atomic_store(&c->gate,0);return RSM_REFUSED;}return 0;}
int rsm_has_receipt(struct recording_sync_metadata *c,const struct rsm_receipt *r)
{if(!c||!ext(c,r,sizeof(*r))||!receipt_valid(r))return RSM_ARGUMENT;int rc=query(c);if(rc)return rc;
 unsigned at;rc=c->extent_profile?RSM_REFUSED:receipt_find(c,r,&at);atomic_store(&c->gate,0);return rc;}
int rsm_replay_delete(struct recording_sync_metadata *c,const uint8_t op[16],const uint8_t digest[32],struct rsm_tombstone *out)
{if(!c||!ext(c,op,16)||!ext(c,digest,32)||!ext(c,out,sizeof(*out))||overlap(out,sizeof(*out),op,16)||overlap(out,sizeof(*out),digest,32)||!id(op,16)||!id(digest,32))return RSM_ARGUMENT;
 int rc=query(c);if(rc)return rc;rc=c->extent_profile?RSM_REFUSED:replay(c,op,digest,out);atomic_store(&c->gate,0);return rc;}
int rsm_record_deleted(struct recording_sync_metadata *c,const uint8_t record[16])
{if(!c||!ext(c,record,16)||!id(record,16))return RSM_ARGUMENT;int rc=query(c);if(rc)return rc;rc=RSM_NOT_FOUND;
 if(c->extent_profile){rc=rsmx_deleted(c,record);atomic_store(&c->gate,0);return rc;}
 for(unsigned i=0;i<get32(c->tombstones+112);i++)if(!memcmp(c->tombstones+128+i*128+16,record,16)){rc=0;break;}
 atomic_store(&c->gate,0);return rc;}
int rsm_get_root_proof(struct recording_sync_metadata *c,struct rsm_root_proof *out)
{if(!c||!ext(c,out,sizeof(*out)))return RSM_ARGUMENT;int rc=query(c);if(rc)return rc;
 memset(out,0,sizeof(*out));memcpy(out->descriptor_digest,c->volume.descriptor_digest,32);memcpy(out->root_digest,c->root_digest,32);
 out->capacity=c->volume.capacity;out->ros_capacity=c->extent_profile?RLL_ROS_CAPACITY:c->volume.capacity-3;
 out->slots=c->extent_profile?RLL_SLOTS:slots(c);atomic_store(&c->gate,0);return 0;}
int rsm_deleted_snapshot(struct recording_sync_metadata *c,uint8_t ids[RSM_TOMBSTONES][16],uint32_t *count)
{
 if(!c||!ext(c,ids,RSM_TOMBSTONES*16U)||!ext(c,count,sizeof(*count))||
    overlap(ids,RSM_TOMBSTONES*16U,count,sizeof(*count)))return RSM_ARGUMENT;
 int rc=query(c);if(rc)return rc;uint32_t n=get32(c->tombstones+112);
 if(c->extent_profile){atomic_store(&c->gate,0);return RSM_REFUSED;}
 if(n>RSM_TOMBSTONES){fault(c);atomic_store(&c->gate,0);return RSM_FAULT;}
 memset(ids,0,RSM_TOMBSTONES*16U);
 for(uint32_t i=0;i<n;++i)memcpy(ids[i],c->tombstones+128+i*128+16,16);
 *count=n;atomic_store(&c->gate,0);return RSM_OK;
}
int rsm_extent_deleted_snapshot(struct recording_sync_metadata *c,uint8_t ids[RLL_TOMBSTONES][16],uint32_t *count)
{
 if(!c||!ext(c,ids,RLL_TOMBSTONES*16U)||!ext(c,count,sizeof(*count))||
    overlap(ids,RLL_TOMBSTONES*16U,count,sizeof(*count)))return RSM_ARGUMENT;
 int rc=query(c);if(rc)return rc;
 if(!c->extent_profile){atomic_store(&c->gate,0);return RSM_REFUSED;}
 memcpy(ids,c->deleted_ids,sizeof(c->deleted_ids));*count=c->deleted_count;
 atomic_store(&c->gate,0);return RSM_OK;
}
int rsm_visit_extent_deleted(struct recording_sync_metadata *c,void *user,
 int (*visit)(void *,const struct rsm_volume *,const uint8_t *,uint32_t))
{
 if(!c||!visit)return RSM_ARGUMENT;
 int rc=query(c);if(rc)return rc;
 if(!c->extent_profile){atomic_store(&c->gate,0);return RSM_REFUSED;}
 rc=visit(user,&c->volume,&c->deleted_ids[0][0],c->deleted_count);
 atomic_store(&c->gate,0);return rc?RSM_REFUSED:RSM_OK;
}
